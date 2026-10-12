#include "wasapi_capture.hpp"
#include "log.hpp"

#include <audioclient.h>
#include <cwchar>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <propsys.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

using Microsoft::WRL::ComPtr;

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static std::wstring FriendlyName(IMMDevice* dev) {
    ComPtr<IPropertyStore> props;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, props.GetAddressOf()))) return L"";
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR &&
        pv.pwszVal)
        name = pv.pwszVal;
    PropVariantClear(&pv);
    return name;
}

static std::vector<AudioDeviceInfo> ListDevices(EDataFlow flow) {
    std::vector<AudioDeviceInfo> out;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> en;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(en.GetAddressOf())))) {
        ComPtr<IMMDevice> def;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, def.GetAddressOf())))
            out.push_back({FriendlyName(def.Get()), true});
        ComPtr<IMMDeviceCollection> coll;
        if (SUCCEEDED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, coll.GetAddressOf()))) {
            UINT n = 0;
            coll->GetCount(&n);
            for (UINT i = 0; i < n; ++i) {
                ComPtr<IMMDevice> d;
                coll->Item(i, d.GetAddressOf());
                if (d) out.push_back({FriendlyName(d.Get()), false});
            }
        }
    }
    CoUninitialize();
    return out;
}

std::vector<AudioDeviceInfo> WasapiCapture::ListRenderDevices() {
    return ListDevices(eRender);
}
std::vector<AudioDeviceInfo> WasapiCapture::ListCaptureDevices() {
    return ListDevices(eCapture);
}

static bool ParseMixFormat(const WAVEFORMATEX* wf, AVSampleFormat* fmt, AVChannelLayout* layout,
                           int* rate) {
    *rate = (int)wf->nSamplesPerSec;
    WORD tag = wf->wFormatTag;
    WORD bits = wf->wBitsPerSample;
    bool isFloat = false, isPcm = false;
    if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = true;
    } else if (tag == WAVE_FORMAT_PCM) {
        isPcm = true;
    } else if (tag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
            isFloat = true;
        else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
            isPcm = true;
    }
    if (isFloat) {
        *fmt = AV_SAMPLE_FMT_FLT;
    } else if (isPcm && bits == 16) {
        *fmt = AV_SAMPLE_FMT_S16;
    } else if (isPcm && bits == 32) {
        *fmt = AV_SAMPLE_FMT_S32;
    } else {
        return false;
    }
    av_channel_layout_default(layout, wf->nChannels);
    return true;
}

void WasapiCapture::ThreadMain() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInited = SUCCEEDED(hr);
    std::string err;
    bool ok = Run(&err);
    if (!ok) {
        initError_ = err;
        SetEvent(hReady_);  // failure path: nothing else will signal it
    }
    if (comInited) CoUninitialize();
}

bool WasapiCapture::Run(std::string* err) {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(en.GetAddressOf())))) {
        *err = "MMDeviceEnumerator unavailable";
        return false;
    }

    const EDataFlow flow = loopback_ ? eRender : eCapture;
    ComPtr<IMMDevice> dev;
    const std::wstring want = Lower(match_);
    if (want.empty() || want == L"default") {
        if (FAILED(en->GetDefaultAudioEndpoint(flow, eConsole, dev.GetAddressOf()))) {
            *err = "no default audio endpoint";
            return false;
        }
    } else {
        ComPtr<IMMDeviceCollection> coll;
        if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, coll.GetAddressOf()))) {
            *err = "EnumAudioEndpoints failed";
            return false;
        }
        UINT n = 0;
        coll->GetCount(&n);
        for (UINT i = 0; i < n && !dev; ++i) {
            ComPtr<IMMDevice> d;
            coll->Item(i, d.GetAddressOf());
            if (d && Lower(FriendlyName(d.Get())).find(want) != std::wstring::npos) dev = d;
        }
        if (!dev) {
            *err = "no audio device matched the requested name";
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(nameMu_);
        activeDevice_ = FriendlyName(dev.Get());
    }

    ComPtr<IAudioClient> client;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void**>(client.GetAddressOf())))) {
        *err = "Activate IAudioClient failed";
        return false;
    }

    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix))) {
        *err = "GetMixFormat failed";
        return false;
    }

    AVSampleFormat inFmt;
    AVChannelLayout inLayout;
    int inRate = 0;
    if (!ParseMixFormat(mix, &inFmt, &inLayout, &inRate)) {
        CoTaskMemFree(mix);
        *err = "unsupported audio mix format";
        return false;
    }

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (loopback_) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, mix, nullptr);
    if (FAILED(hr)) {
        CoTaskMemFree(mix);
        *err = "IAudioClient::Initialize failed: " + Hex(hr);
        return false;
    }
    const WORD blockAlign = mix->nBlockAlign;
    CoTaskMemFree(mix);

    ComPtr<IAudioCaptureClient> cap;
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient),
                                  reinterpret_cast<void**>(cap.GetAddressOf())))) {
        *err = "GetService(IAudioCaptureClient) failed";
        return false;
    }

    if (FAILED(client->SetEventHandle(hEvent_))) {
        *err = "SetEventHandle failed";
        return false;
    }

    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, 2);
    SwrContext* swr = nullptr;
    if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, 48000, &inLayout, inFmt, inRate,
                            0, nullptr) < 0 ||
        swr_init(swr) < 0) {
        if (swr) swr_free(&swr);
        *err = "swresample init failed";
        return false;
    }

    ring.Reset(48000 * 2, 2);  // 2 seconds of stereo
    if (FAILED(client->Start())) {
        swr_free(&swr);
        *err = "IAudioClient::Start failed";
        return false;
    }
    running = true;
    SetEvent(hReady_);  // capture is live; Start() can return now

    std::vector<uint8_t> zeros;
    while (!quit_.load()) {
        HANDLE handles[2] = {hEvent_, hQuit_};
        DWORD w = WaitForMultipleObjects(2, handles, FALSE, 500);
        if (w == WAIT_OBJECT_0 + 1) break;
        if (w != WAIT_OBJECT_0) continue;

        UINT32 packet = 0;
        if (FAILED(cap->GetNextPacketSize(&packet))) {
            deviceLost = true;
            break;
        }
        while (packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD pflags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &pflags, nullptr, nullptr))) {
                deviceLost = true;
                break;
            }

            const BYTE* src = data;
            if ((pflags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                zeros.assign((size_t)frames * blockAlign, 0);
                src = zeros.data();
            }

            int maxOut = (int)swr_get_out_samples(swr, (int)frames) + 8;
            if (maxOut > 0) {
                convertBuf_.resize((size_t)maxOut * 2);
                uint8_t* outPtr = reinterpret_cast<uint8_t*>(convertBuf_.data());
                const uint8_t* inPtr = src;
                int got = swr_convert(swr, &outPtr, maxOut, &inPtr, (int)frames);
                if (got > 0) {
                    ring.Write(convertBuf_.data(), (size_t)got);
                    samplesIn.fetch_add((uint64_t)got);
                }
            }

            cap->ReleaseBuffer(frames);
            if (FAILED(cap->GetNextPacketSize(&packet))) {
                deviceLost = true;
                break;
            }
        }
    }

    client->Stop();
    swr_free(&swr);
    running = false;
    return true;
}

bool WasapiCapture::Start(bool loopback, const std::wstring& nameSubstr, std::string* err) {
    loopback_ = loopback;
    match_ = nameSubstr;
    quit_ = false;
    hEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hQuit_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    hReady_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hEvent_ || !hQuit_ || !hReady_) {
        *err = "CreateEvent failed";
        return false;
    }
    thread_ = std::thread(&WasapiCapture::ThreadMain, this);
    DWORD w = WaitForSingleObject(hReady_, 5000);
    if (w != WAIT_OBJECT_0 || !initError_.empty() || !running.load()) {
        *err = initError_.empty() ? "audio capture start timed out" : initError_;
        Stop();
        return false;
    }
    return true;
}

void WasapiCapture::Stop() {
    quit_ = true;
    if (hQuit_) SetEvent(hQuit_);
    if (thread_.joinable()) thread_.join();
    if (hEvent_) {
        CloseHandle(hEvent_);
        hEvent_ = nullptr;
    }
    if (hQuit_) {
        CloseHandle(hQuit_);
        hQuit_ = nullptr;
    }
    if (hReady_) {
        CloseHandle(hReady_);
        hReady_ = nullptr;
    }
    running = false;
}
