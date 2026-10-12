#include "capture_dxgi.hpp"
#include "log.hpp"

#include <chrono>
#include <dxgi1_2.h>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

// Find the monitor device name (\\.\DISPLAYn) for the given EnumDisplayMonitors index.
static std::wstring MonitorDeviceName(int index) {
    struct Ctx {
        int want;
        int seen;
        std::wstring name;
    } ctx{index, 0, L""};
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            if (c->seen == c->want) {
                MONITORINFOEXW mi{};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(h, &mi)) c->name = mi.szDevice;
                return FALSE;
            }
            c->seen++;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.name;
}

bool DxgiDuplicator::AcquireOutput() {
    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(device_->QueryInterface(__uuidof(IDXGIDevice),
                                       reinterpret_cast<void**>(dxgiDev.GetAddressOf()))))
        return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDev->GetAdapter(adapter.GetAddressOf()))) return false;

    const std::wstring want = MonitorDeviceName(monitorIndex_);
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIOutput> out;
        if (adapter->EnumOutputs(i, out.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC d{};
        out->GetDesc(&d);
        if (!want.empty() && want == d.DeviceName) {
            ComPtr<IDXGIOutput1> out1;
            if (FAILED(out.As(&out1))) return false;
            if (FAILED(out1->DuplicateOutput(device_, dup_.ReleaseAndGetAddressOf()))) return false;
            outputIndex_ = (int)i;
            monitorName = d.DeviceName;
            width = (uint32_t)(d.DesktopCoordinates.right - d.DesktopCoordinates.left);
            height = (uint32_t)(d.DesktopCoordinates.bottom - d.DesktopCoordinates.top);
            originX = d.DesktopCoordinates.left;
            originY = d.DesktopCoordinates.top;
            return true;
        }
    }
    return false;
}

void DxgiDuplicator::ThreadMain() {
    std::string err;
    bool ok = Run(&err);
    if (!ok && initError_.empty()) {
        initError_ = err.empty() ? "DXGI duplication failed" : err;
        SetEvent(hReady_);
    }
}

// Sample a 5x5 grid of the retained frame; true if any sample is not black.
bool DxgiDuplicator::ProbeContent() {
    ComPtr<ID3D11DeviceContext> ctx;
    device_->GetImmediateContext(ctx.GetAddressOf());

    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = width;
    sd.Height = height;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc = {1, 0};
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&sd, nullptr, staging.GetAddressOf()))) return true;
    ctx->CopyResource(staging.Get(), retained_.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return true;
    int nonBlack = 0;
    for (int gy = 1; gy <= 5; ++gy) {
        for (int gx = 1; gx <= 5; ++gx) {
            uint32_t x = width * (uint32_t)gx / 6;
            uint32_t y = height * (uint32_t)gy / 6;
            const uint8_t* p = (const uint8_t*)m.pData + (size_t)y * m.RowPitch + (size_t)x * 4;
            if (p[0] > 8 || p[1] > 8 || p[2] > 8) nonBlack++;
        }
    }
    ctx->Unmap(staging.Get(), 0);
    std::printf("[capture] DXGI content probe: %d/25 sample points non-black\n", nonBlack);
    return nonBlack > 0;
}

bool DxgiDuplicator::EnsureRetained() {
    if (retained_) {
        D3D11_TEXTURE2D_DESC d{};
        retained_->GetDesc(&d);
        if (d.Width == width && d.Height == height) return true;
        retained_.Reset();  // display mode changed: old size no longer valid
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc = {1, 0};
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(device_->CreateTexture2D(&td, nullptr, retained_.ReleaseAndGetAddressOf()));
}

bool DxgiDuplicator::SeedFrame() {
    ComPtr<ID3D11DeviceContext> ctx;
    device_->GetImmediateContext(ctx.GetAddressOf());
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> res;
    HRESULT hr = dup_->AcquireNextFrame(1000, &info, res.GetAddressOf());
    if (hr != S_OK) {
        std::printf("[capture] DXGI: initial acquire -> %s\n",
                    hr == DXGI_ERROR_WAIT_TIMEOUT ? "no frame within 1 s (static desktop?)"
                                                  : Hex(hr).c_str());
        return false;
    }
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(res.As(&tex))) ctx->CopyResource(retained_.Get(), tex.Get());
    dup_->ReleaseFrame();
    newFrames.fetch_add(1);
    return true;
}

bool DxgiDuplicator::Run(std::string* err) {
    if (!AcquireOutput()) {
        *err = "could not duplicate monitor " + std::to_string(monitorIndex_) +
               " (access denied, or monitor is on a different adapter than the capture device)";
        return false;
    }

    ComPtr<ID3D11DeviceContext> ctx;
    device_->GetImmediateContext(ctx.GetAddressOf());

    bool firstSeed = true;
running_ready_:
    if (!EnsureRetained()) {
        *err = "CreateTexture2D(retained) failed";
        return false;
    }
    // Seed the retained frame with the current desktop so the very first tick
    // has real content even if the screen is static. Re-runs after a display
    // mode change (ACCESS_LOST), which also re-creates the retained texture.
    if (SeedFrame()) {
        if (firstSeed) {
            firstSeed = false;
            std::printf("[capture] DXGI: initial desktop frame acquired\n");
            contentOk_ = ProbeContent();
            if (requireContent_ && !contentOk_) {
                *err = "duplicated desktop returned black frames (virtual display?)";
                return false;  // fail before any frame reaches the pipeline
            }
        }
    }

    SetEvent(hReady_);  // capture is live

    const auto frameInterval = std::chrono::duration<double>(1.0 / (double)fps_);
    auto next = std::chrono::steady_clock::now();
    while (!quit_.load()) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dup_->AcquireNextFrame(0, &info, res.GetAddressOf());
        if (hr == S_OK) {
            ComPtr<ID3D11Texture2D> tex;
            if (SUCCEEDED(res.As(&tex))) {
                ctx->CopyResource(retained_.Get(), tex.Get());
                newFrames.fetch_add(1);
            }
            // Pointer state (hardware cursor) travels with each frame.
            if (info.PointerPosition.Visible) {
                cursor_.visible = true;
                cursor_.x = info.PointerPosition.Position.x;
                cursor_.y = info.PointerPosition.Position.y;
                pointerUpdates.fetch_add(1);
            } else {
                cursor_.visible = false;
            }
            if (info.PointerShapeBufferSize > 0) {
                cursor_.shape.resize(info.PointerShapeBufferSize);
                UINT required = 0;
                DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo{};
                if (SUCCEEDED(dup_->GetFramePointerShape((UINT)cursor_.shape.size(),
                                                         cursor_.shape.data(), &required,
                                                         &shapeInfo))) {
                    cursor_.shapeType = shapeInfo.Type;
                    cursor_.width = shapeInfo.Width;
                    cursor_.height = shapeInfo.Height;
                    cursor_.pitch = shapeInfo.Pitch;
                    cursor_.hotspotX = shapeInfo.HotSpot.x;
                    cursor_.hotspotY = shapeInfo.HotSpot.y;
                    cursor_.hasShape = true;
                    cursor_.shapeVersion++;
                    shapeUpdates.fetch_add(1);
                }
            }
            dup_->ReleaseFrame();
        } else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // static screen: re-submit the retained frame below
        } else if (hr == DXGI_ERROR_ACCESS_LOST) {
            dup_.Reset();
            for (int attempt = 0; attempt < 40 && !quit_.load(); ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                if (!AcquireOutput()) continue;
                goto running_ready_;
            }
            *err = "display mode changes kept invalidating duplication";
            return false;
        } else {
            *err = "AcquireNextFrame failed: " + Hex(hr);
            return false;
        }

        uint64_t idx = frameCount.fetch_add(1);
        if (cb_) cb_(retained_.Get(), width, height, idx);

        next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(frameInterval);
        auto now = std::chrono::steady_clock::now();
        if (next > now)
            std::this_thread::sleep_for(next - now);
        else
            next = now;  // we fell behind; resync pacing
    }
    return true;
}

bool DxgiDuplicator::Start(int monitorIndex, ID3D11Device* device, uint32_t fps,
                           FrameCallback cb, bool requireContent, std::string* err) {
    monitorIndex_ = monitorIndex;
    device_ = device;
    fps_ = fps;
    cb_ = std::move(cb);
    requireContent_ = requireContent;

    // Reset all per-run state: without this a restart after Stop() spawns a
    // thread whose loop exits immediately.
    quit_ = false;
    initError_.clear();
    contentOk_ = true;
    frameCount = 0;
    newFrames = 0;
    pointerUpdates = 0;
    shapeUpdates = 0;
    cursor_ = CursorState{};
    width = height = 0;
    originX = originY = 0;

    hReady_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hReady_) {
        *err = "CreateEvent failed";
        return false;
    }
    thread_ = std::thread(&DxgiDuplicator::ThreadMain, this);
    DWORD w = WaitForSingleObject(hReady_, 8000);
    if (w != WAIT_OBJECT_0 || !initError_.empty()) {
        *err = initError_.empty() ? "DXGI duplication start timed out" : initError_;
        Stop();
        return false;
    }
    return true;
}

void DxgiDuplicator::Stop() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    dup_.Reset();
    retained_.Reset();
    if (hReady_) {
        CloseHandle(hReady_);
        hReady_ = nullptr;
    }
}
