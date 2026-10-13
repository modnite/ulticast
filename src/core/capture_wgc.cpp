#include "capture_wgc.hpp"
#include "log.hpp"

#include <cwctype>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
using Microsoft::WRL::ComPtr;

struct WgcCapture::Impl {
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::event_token token{};
    FrameCallback cb;
    std::atomic<int> inflight{0};
    std::atomic<bool> revoked{false};
};

// ---- enumeration -----------------------------------------------------------

static BOOL CALLBACK MonitorProc(HMONITOR h, HDC, LPRECT, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(h, &mi)) {
        MonitorInfo info;
        info.index = (int)out->size();
        info.handle = h;
        info.rect = mi.rcMonitor;
        info.device = mi.szDevice;
        info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        out->push_back(info);
    }
    return TRUE;
}

std::vector<MonitorInfo> WgcCapture::ListMonitors() {
    std::vector<MonitorInfo> v;
    EnumDisplayMonitors(nullptr, nullptr, MonitorProc, reinterpret_cast<LPARAM>(&v));
    return v;
}

static BOOL CALLBACK WindowProc(HWND hwnd, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<HWND>*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindowTextLengthW(hwnd) == 0) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;  // skip owned popups
    out->push_back(hwnd);
    return TRUE;
}

std::vector<std::wstring> WgcCapture::ListWindows() {
    std::vector<HWND> hwnds;
    EnumWindows(WindowProc, reinterpret_cast<LPARAM>(&hwnds));
    std::vector<std::wstring> titles;
    for (HWND h : hwnds) {
        int len = GetWindowTextLengthW(h);
        std::wstring t((size_t)len + 1, L'\0');
        GetWindowTextW(h, t.data(), len + 1);
        t.resize((size_t)len);
        if (!t.empty()) titles.push_back(t);
    }
    return titles;
}

static bool ContainsNoCase(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return false;
    std::wstring h = hay, n = needle;
    for (auto& c : h) c = (wchar_t)towlower(c);
    for (auto& c : n) c = (wchar_t)towlower(c);
    return h.find(n) != std::wstring::npos;
}

// ---- start -----------------------------------------------------------------

bool WgcCapture::StartMonitor(int index, ID3D11Device* device, FrameCallback cb, std::string* err) {
    auto monitors = ListMonitors();
    if (index < 0 || index >= (int)monitors.size()) {
        *err = "monitor index out of range";
        return false;
    }
    try {
        auto interop =
            winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(interop->CreateForMonitor(monitors[index].handle,
                                                       winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                       winrt::put_abi(item)));
        return StartItem(item, device, std::move(cb), err);
    } catch (winrt::hresult_error const& e) {
        *err = "CreateForMonitor: " + winrt::to_string(e.message());
        return false;
    }
}

bool WgcCapture::StartWindow(const std::wstring& titleSubstr, ID3D11Device* device,
                             FrameCallback cb, std::string* err) {
    std::vector<HWND> hwnds;
    EnumWindows(WindowProc, reinterpret_cast<LPARAM>(&hwnds));
    HWND target = nullptr;
    for (HWND h : hwnds) {
        int len = GetWindowTextLengthW(h);
        std::wstring t((size_t)len + 1, L'\0');
        GetWindowTextW(h, t.data(), len + 1);
        t.resize((size_t)len);
        if (ContainsNoCase(t, titleSubstr)) {
            target = h;
            break;
        }
    }
    if (!target) {
        *err = "no visible window matched that title";
        return false;
    }
    if (IsIconic(target)) {
        // Windows Graphics Capture cannot capture a minimized window (frames
        // stay black), so say so instead of producing a black layer.
        *err = "that window is minimized — restore it first";
        return false;
    }
    try {
        auto interop =
            winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(interop->CreateForWindow(target,
                                                      winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                      winrt::put_abi(item)));
        return StartItem(item, device, std::move(cb), err);
    } catch (winrt::hresult_error const& e) {
        *err = "CreateForWindow: " + winrt::to_string(e.message());
        return false;
    }
}

bool WgcCapture::StartItem(wgc::GraphicsCaptureItem const& item, ID3D11Device* device,
                           FrameCallback cb, std::string* err) {
    try {
        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(device->QueryInterface(__uuidof(IDXGIDevice),
                                                    reinterpret_cast<void**>(dxgi.GetAddressOf())));
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
        auto rtDevice = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();

        auto impl = std::make_shared<Impl>();
        impl->cb = std::move(cb);

        auto size = item.Size();
        impl->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            rtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        impl->session = impl->pool.CreateCaptureSession(item);
        impl->session.IsCursorCaptureEnabled(true);
        try {
            impl->session.IsBorderRequired(false);  // Windows 11+; throws on Win10
        } catch (...) {
        }

        impl->token = impl->pool.FrameArrived(
            [this, impl](wgc::Direct3D11CaptureFramePool const& sender,
                         winrt::Windows::Foundation::IInspectable const&) {
                // Count in-flight work *before* checking revocation so Stop()
                // can never free pipeline state while a callback is running.
                impl->inflight.fetch_add(1);
                if (impl->revoked.load()) {
                    impl->inflight.fetch_sub(1);
                    return;
                }
                auto frame = sender.TryGetNextFrame();
                if (frame) {
                    // IDirect3DDxgiInterfaceAccess lives in the SDK's um/ interop header
                    // (global Windows::Graphics::... namespace), not in the WinRT projection.
                    winrt::com_ptr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>
                        access;
                    HRESULT hr = winrt::get_unknown(frame.Surface())
                                     ->QueryInterface(
                                         __uuidof(::Windows::Graphics::DirectX::Direct3D11::
                                                       IDirect3DDxgiInterfaceAccess),
                                         access.put_void());
                    winrt::com_ptr<ID3D11Texture2D> tex;
                    if (SUCCEEDED(hr)) {
                        hr = access->GetInterface(__uuidof(ID3D11Texture2D), tex.put_void());
                    }
                    if (SUCCEEDED(hr)) {
                        uint64_t idx = frameCount.fetch_add(1);
                        auto cs = frame.ContentSize();
                        if (impl->cb)
                            impl->cb(tex.get(), (uint32_t)cs.Width, (uint32_t)cs.Height, idx);
                    }
                    frame.Close();
                }
                impl->inflight.fetch_sub(1);
            });

        impl->session.StartCapture();
        impl_ = impl;
        return true;
    } catch (winrt::hresult_error const& e) {
        *err = "capture setup: " + winrt::to_string(e.message());
        return false;
    }
}

void WgcCapture::Stop() {
    if (!impl_) return;
    impl_->revoked = true;
    try {
        impl_->pool.FrameArrived(impl_->token);
    } catch (...) {
    }
    // Wait (unbounded) for any in-flight callback: the pipeline may still be
    // encoding/muxing a frame, and freeing state underneath it would be a
    // use-after-free.
    while (impl_->inflight.load() > 0) Sleep(5);
    try {
        if (impl_->session) impl_->session.Close();
    } catch (...) {
    }
    try {
        if (impl_->pool) impl_->pool.Close();
    } catch (...) {
    }
    impl_.reset();
}
