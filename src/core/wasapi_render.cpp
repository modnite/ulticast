#include "wasapi_render.hpp"

#include <algorithm>
#include <cwchar>
#include <vector>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <wrl/client.h>

#include "log.hpp"
#include "diag.hpp"

using Microsoft::WRL::ComPtr;

// kPkeyFriendlyName {a45c254e-df1c-4efd-8020-67d146a850e0}, pid 14.
// Declared locally: pulling in functiondiscoverykeys_devpkey.h needs the
// DEFINE_PROPERTYKEY macro plumbing, which collides with initguid.h in the
// DirectShow translation unit.
static const PROPERTYKEY kPkeyFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

WasapiRender::~WasapiRender() { Stop(); }

bool WasapiRender::Start(const std::wstring& nameSubstr, int sampleRate, std::string* err) {
    Stop();
    match_ = nameSubstr;
    sampleRate_ = sampleRate;
    ring.Reset((size_t)sampleRate, 2);  // 1 s of stereo float
    quit_ = false;
    deviceLost_ = false;
    initError_.clear();
    hReady_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hReady_) {
        *err = "CreateEvent failed";
        return false;
    }
    thread_ = std::thread(&WasapiRender::ThreadMain, this);
    const DWORD w = WaitForSingleObject(hReady_, 5000);
    if (w != WAIT_OBJECT_0 || !initError_.empty()) {
        *err = initError_.empty() ? "monitor start timed out" : initError_;
        Stop();
        return false;
    }
    return true;
}

void WasapiRender::ThreadMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInited = SUCCEEDED(hrCom);

    auto fail = [&](const std::string& msg) {
        initError_ = msg;
        running_ = false;
        SetEvent(hReady_);
        if (comInited) CoUninitialize();
    };

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(enumerator.GetAddressOf()));
    if (FAILED(hr)) return fail("MMDeviceEnumerator: " + Hex(hr));

    // Pick the endpoint: "default"/empty = system default, else the first active
    // render device whose friendly name contains the match.
    ComPtr<IMMDevice> device;
    const std::wstring target = Lower(match_);
    if (target.empty() || target == L"default") {
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf())))
            return fail("no default output device");
    } else {
        ComPtr<IMMDeviceCollection> coll;
        if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                                  coll.GetAddressOf())))
            return fail("EnumAudioEndpoints failed");
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count && !device; ++i) {
            ComPtr<IMMDevice> dev;
            if (FAILED(coll->Item(i, dev.GetAddressOf()))) continue;
            ComPtr<IPropertyStore> props;
            if (FAILED(dev->OpenPropertyStore(STGM_READ, props.GetAddressOf()))) continue;
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(kPkeyFriendlyName, &v)) && v.vt == VT_LPWSTR &&
                Lower(v.pwszVal).find(target) != std::wstring::npos)
                device = dev;
            PropVariantClear(&v);
        }
        if (!device) return fail("no output device matched \"" + Narrow(match_) + "\"");
    }

    {
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.GetAddressOf()))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(kPkeyFriendlyName, &v)) && v.vt == VT_LPWSTR) {
                std::lock_guard<std::mutex> lk(nameMu_);
                active_ = v.pwszVal;
            }
            PropVariantClear(&v);
        }
    }

    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) return fail("Activate(IAudioClient): " + Hex(hr));

    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = (DWORD)sampleRate_;
    fmt.wBitsPerSample = 32;
    fmt.nBlockAlign = (WORD)(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    fmt.cbSize = 0;

    // Shared mode + automatic conversion: the endpoint decides the real format.
    const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1000000, 0, &fmt, nullptr);
    if (FAILED(hr)) return fail("Initialize(render): " + Hex(hr));

    UINT32 bufferFrames = 0;
    hr = client->GetBufferSize(&bufferFrames);
    if (FAILED(hr)) return fail("GetBufferSize: " + Hex(hr));

    ComPtr<IAudioRenderClient> render;
    hr = client->GetService(IID_PPV_ARGS(render.GetAddressOf()));
    if (FAILED(hr)) return fail("IAudioRenderClient: " + Hex(hr));

    hr = client->Start();
    if (FAILED(hr)) return fail("Start(render): " + Hex(hr));

    Diag::Log("[audio] monitor: %ls", ActiveDevice().c_str());
    running_ = true;
    SetEvent(hReady_);

    std::vector<float> block((size_t)bufferFrames * 2);
    while (!quit_.load()) {
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) {
            deviceLost_ = true;
            break;
        }
        UINT32 avail = bufferFrames > padding ? bufferFrames - padding : 0;
        if (avail == 0) {
            Sleep(2);
            continue;
        }
        // Keep roughly half the endpoint buffer filled.
        const UINT32 want = (std::min)(avail, bufferFrames / 2);
        const size_t got = ring.Read(block.data(), want);
        for (size_t i = got; i < want; ++i) {
            block[2 * i] = 0.f;
            block[2 * i + 1] = 0.f;
        }
        BYTE* out = nullptr;
        if (FAILED(render->GetBuffer(want, &out))) {
            deviceLost_ = true;
            break;
        }
        memcpy(out, block.data(), (size_t)want * 2 * sizeof(float));
        render->ReleaseBuffer(want, 0);
        Sleep(4);
    }

    client->Stop();
    running_ = false;
    if (comInited) CoUninitialize();
}

void WasapiRender::Stop() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
    if (hReady_) {
        CloseHandle(hReady_);
        hReady_ = nullptr;
    }
}

