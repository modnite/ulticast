#include "webcam_mf.hpp"
#include "log.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::vector<std::wstring> WebcamCapture::ListDevices() {
    std::vector<std::wstring> names;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return names;
    ComPtr<IMFAttributes> attr;
    if (SUCCEEDED(MFCreateAttributes(attr.GetAddressOf(), 1))) {
        attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(MFEnumDeviceSources(attr.Get(), &devices, &count))) {
            for (UINT32 i = 0; i < count; ++i) {
                WCHAR* name = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                                             &name, &len))) {
                    names.emplace_back(name);
                    CoTaskMemFree(name);
                }
                devices[i]->Release();
            }
            CoTaskMemFree(devices);
        }
    }
    MFShutdown();
    return names;
}

static bool FindDeviceByName(const std::wstring& want, IMFActivate** outActivate) {
    ComPtr<IMFAttributes> attr;
    if (FAILED(MFCreateAttributes(attr.GetAddressOf(), 1))) return false;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                  MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attr.Get(), &devices, &count))) return false;

    const std::wstring target = Lower(want);
    bool found = false;
    for (UINT32 i = 0; i < count; ++i) {
        WCHAR* name = nullptr;
        UINT32 len = 0;
        std::wstring nameStr;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name,
                                                     &len))) {
            nameStr = name;
            CoTaskMemFree(name);
        }
        if (!found && (target.empty() || target == L"default" ||
                       Lower(nameStr).find(target) != std::wstring::npos)) {
            devices[i]->AddRef();
            *outActivate = devices[i];
            found = true;
        }
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    return found;
}

bool WebcamCapture::Run(std::string* err) {
    IMFActivate* activate = nullptr;
    if (!FindDeviceByName(match_, &activate)) {
        *err = "no video capture device matched";
        return false;
    }
    ComPtr<IMFActivate> activateRef;
    activateRef.Attach(activate);
    {
        std::lock_guard<std::mutex> lock(nameMu_);
        activeDevice_ = match_;
    }

    ComPtr<IMFMediaSource> source;
    if (FAILED(activateRef->ActivateObject(__uuidof(IMFMediaSource),
                                           reinterpret_cast<void**>(source.GetAddressOf())))) {
        *err = "ActivateObject(video source) failed";
        return false;
    }

    ComPtr<IMFAttributes> readerAttr;
    MFCreateAttributes(readerAttr.GetAddressOf(), 1);
    readerAttr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), readerAttr.Get(),
                                                   reader.GetAddressOf()))) {
        *err = "MFCreateSourceReaderFromMediaSource failed";
        return false;
    }

    // Request RGB32 at native frame size.
    ComPtr<IMFMediaType> nativeType, outType;
    if (FAILED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                          nativeType.GetAddressOf()))) {
        *err = "no native video type";
        return false;
    }
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(nativeType.Get(), MF_MT_FRAME_SIZE, &w, &h);
    if (w == 0 || h == 0) {
        *err = "invalid camera frame size";
        return false;
    }

    MFCreateMediaType(outType.GetAddressOf());
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    outType->SetUINT64(MF_MT_FRAME_SIZE, ((UINT64)w << 32) | h);
    outType->SetUINT64(MF_MT_FRAME_RATE, ((UINT64)30 << 32) | 1);
    outType->SetUINT64(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr,
                                           outType.Get()))) {
        *err = "camera does not support RGB32 output";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        width_ = w;
        height_ = h;
    }

    SetEvent(hReady_);  // device is live

    while (!quit_.load()) {
        DWORD streamIndex = 0, flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex,
                                        &flags, &timestamp, sample.GetAddressOf());
        if (FAILED(hr)) {
            if (quit_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) {
            // No frame ready right now: do NOT busy-spin (it starves the capture
            // and main threads and wrecks the media clock).
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(buffer.GetAddressOf()))) continue;
        BYTE* data = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(buffer->Lock(&data, &maxLen, &curLen))) continue;

        const size_t tightStride = (size_t)w * 4;
        size_t stride = tightStride;
        if (h > 0 && curLen >= (size_t)h * tightStride && (curLen % h) == 0)
            stride = curLen / h;  // MF buffers may be padded
        {
            std::lock_guard<std::mutex> lock(mu_);
            std::vector<uint8_t>& dst = useA_ ? bufA_ : bufB_;
            dst.resize(tightStride * h);
            for (uint32_t y = 0; y < h; ++y)
                memcpy(dst.data() + y * tightStride, data + (size_t)y * stride, tightStride);
            useA_ = !useA_;
            frameNo_++;
        }
        buffer->Unlock();
    }
    return true;
}

void WebcamCapture::ThreadMain() {
    // Non-critical background capture: keep it out of the way of the pipeline.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInited = SUCCEEDED(hr);
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        std::string err;
        if (!Run(&err)) {
            initError_ = err;
            SetEvent(hReady_);
        }
        MFShutdown();
    } else {
        initError_ = "MFStartup failed";
        SetEvent(hReady_);
    }
    if (comInited) CoUninitialize();
}

bool WebcamCapture::Start(const std::wstring& nameSubstr, std::string* err) {
    match_ = nameSubstr;
    quit_ = false;
    hReady_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hReady_) {
        *err = "CreateEvent failed";
        return false;
    }
    thread_ = std::thread(&WebcamCapture::ThreadMain, this);
    DWORD w = WaitForSingleObject(hReady_, 8000);
    if (w != WAIT_OBJECT_0 || !initError_.empty()) {
        *err = initError_.empty() ? "camera start timed out" : initError_;
        Stop();
        return false;
    }
    return true;
}

bool WebcamCapture::GetLatest(std::vector<uint8_t>* out, uint32_t* width, uint32_t* height,
                              uint64_t* frameNo) {
    std::lock_guard<std::mutex> lock(mu_);
    if (frameNo_ == *frameNo || width_ == 0) return false;
    *out = useA_ ? bufB_ : bufA_;  // the buffer most recently written
    *width = width_;
    *height = height_;
    *frameNo = frameNo_;
    return true;
}

void WebcamCapture::Stop() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    if (hReady_) {
        CloseHandle(hReady_);
        hReady_ = nullptr;
    }
}
