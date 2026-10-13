#include "capture_dshow.hpp"

#include <algorithm>
#include <cwchar>
#include <mutex>

#include <initguid.h>
#include <dshow.h>
#include <wrl/client.h>

#include "log.hpp"
#include "diag.hpp"

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// SampleGrabber declarations. qedit.h was removed from the Windows SDK, so the
// interface and CLSIDs are declared here (the vtable order must match).
// ---------------------------------------------------------------------------

interface __declspec(uuid("0579154A-2B53-4994-B0D0-E773148EFF85")) ISampleGrabberCB
    : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double sampleTime, IMediaSample* sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double sampleTime, BYTE* buffer,
                                               long bufferLen) = 0;
};

interface __declspec(uuid("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")) ISampleGrabber
    : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL oneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL bufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* size, long* buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* callback,
                                                  long whichMethodToCallback) = 0;
};

// {C1F400A0-3F08-11D3-9F0B-006008039E37}
static const GUID kClsidSampleGrabber = {
    0xC1F400A0, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
// {C1F400A4-3F08-11D3-9F0B-006008039E37}
static const GUID kClsidNullRenderer = {
    0xC1F400A4, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};

// ---------------------------------------------------------------------------

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// Enumerates the DirectShow video input category and returns (moniker, name).
struct DsDevice {
    ComPtr<IMoniker> moniker;
    std::wstring name;
};

static std::vector<DsDevice> EnumDsDevices() {
    std::vector<DsDevice> out;
    ComPtr<ICreateDevEnum> devEnum;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(devEnum.GetAddressOf()))))
        return out;
    ComPtr<IEnumMoniker> enumMoniker;
    if (FAILED(devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                              enumMoniker.GetAddressOf(), 0)))
        return out;
    if (!enumMoniker) return out;

    ComPtr<IMoniker> moniker;
    while (enumMoniker->Next(1, moniker.GetAddressOf(), nullptr) == S_OK) {
        ComPtr<IPropertyBag> props;
        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(props.GetAddressOf())))) {
            VARIANT var;
            VariantInit(&var);
            DsDevice dev;
            if (SUCCEEDED(props->Read(L"FriendlyName", &var, nullptr)) && var.bstrVal) {
                dev.name = var.bstrVal;
                dev.moniker = moniker;
                out.push_back(std::move(dev));
            }
            VariantClear(&var);
        }
        moniker.Reset();
    }
    return out;
}

std::vector<std::wstring> DshowCapture::ListDevices() {
    std::vector<std::wstring> names;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInited = SUCCEEDED(hr);
    for (auto& d : EnumDsDevices()) names.push_back(d.name);
    if (comInited) CoUninitialize();
    return names;
}

// ---------------------------------------------------------------------------

struct DshowCapture::Impl : public ISampleGrabberCB {
    // --- IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ISampleGrabberCB)) {
            *ppv = static_cast<ISampleGrabberCB*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    // --- ISampleGrabberCB (called on a DirectShow thread)
    HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE* buffer, long len) override {
        if (!buffer || len <= 0) return S_OK;
        std::lock_guard<std::mutex> lk(mu);
        buf.assign(buffer, buffer + len);
        frameNo++;
        return S_OK;
    }

    ComPtr<IGraphBuilder> graph;
    ComPtr<ICaptureGraphBuilder2> builder;
    ComPtr<IBaseFilter> captureFilter;
    ComPtr<IBaseFilter> grabberFilter;
    ComPtr<IBaseFilter> nullFilter;
    ComPtr<ISampleGrabber> grabber;
    ComPtr<IMediaControl> control;
    IBaseFilter* grabberRaw = nullptr;  // SetCallback holds a raw pointer

    std::mutex mu;
    std::vector<uint8_t> buf;
    uint64_t frameNo = 0;
    uint32_t width = 0, height = 0;
    bool bottomUp = false;
    std::wstring active;
    LONG refs_ = 1;

    ~Impl() {
        if (grabber && grabberRaw) grabber->SetCallback(nullptr, 1);
    }
};

DshowCapture::~DshowCapture() { Stop(); }

bool DshowCapture::Start(const std::wstring& nameSubstr, std::string* err) {
    Stop();

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInited = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

    auto devices = EnumDsDevices();
    if (devices.empty()) {
        *err = "no DirectShow video devices found";
        if (comInited && SUCCEEDED(hr)) CoUninitialize();
        return false;
    }
    const std::wstring target = Lower(nameSubstr);
    DsDevice* picked = nullptr;
    for (auto& d : devices) {
        if (target.empty() || target == L"default" ||
            Lower(d.name).find(target) != std::wstring::npos) {
            picked = &d;
            break;
        }
    }
    if (!picked) {
        *err = "no DirectShow device matched \"" + Narrow(nameSubstr) + "\"";
        if (comInited && SUCCEEDED(hr)) CoUninitialize();
        return false;
    }

    auto* impl = new Impl();
    impl_ = impl;
    impl->active = picked->name;

    auto fail = [&](const char* what, HRESULT code) {
        *err = std::string(what) + ": " + Hex(code);
        Stop();
        if (comInited && SUCCEEDED(hr)) CoUninitialize();
        return false;
    };

    if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(impl->graph.GetAddressOf()))))
        return fail("FilterGraph", E_FAIL);
    if (FAILED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(impl->builder.GetAddressOf()))))
        return fail("CaptureGraphBuilder2", E_FAIL);
    impl->builder->SetFiltergraph(impl->graph.Get());

    if (FAILED(picked->moniker->BindToObject(nullptr, nullptr,
                                             IID_PPV_ARGS(impl->captureFilter.GetAddressOf()))))
        return fail("bind capture filter", E_FAIL);
    if (FAILED(impl->graph->AddFilter(impl->captureFilter.Get(), L"Capture")))
        return fail("AddFilter(capture)", E_FAIL);

    // Sample grabber: RGB32, buffered samples, BufferCB callback.
    if (FAILED(CoCreateInstance(kClsidSampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(impl->grabberFilter.GetAddressOf()))))
        return fail("SampleGrabber", E_FAIL);
    if (FAILED(impl->grabberFilter.As(&impl->grabber))) return fail("ISampleGrabber", E_FAIL);

    AM_MEDIA_TYPE mt{};
    mt.majortype = MEDIATYPE_Video;
    mt.subtype = MEDIASUBTYPE_RGB32;
    mt.formattype = FORMAT_VideoInfo;
    impl->grabber->SetMediaType(&mt);
    impl->grabber->SetBufferSamples(TRUE);
    impl->grabber->SetOneShot(FALSE);
    impl->grabberRaw = impl->grabberFilter.Get();
    impl->grabber->SetCallback(impl, 1);  // 1 = BufferCB

    if (FAILED(impl->graph->AddFilter(impl->grabberFilter.Get(), L"Grabber")))
        return fail("AddFilter(grabber)", E_FAIL);
    if (FAILED(CoCreateInstance(kClsidNullRenderer, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(impl->nullFilter.GetAddressOf()))))
        return fail("NullRenderer", E_FAIL);
    if (FAILED(impl->graph->AddFilter(impl->nullFilter.Get(), L"Null")))
        return fail("AddFilter(null)", E_FAIL);

    if (FAILED(impl->builder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video,
                                           impl->captureFilter.Get(), impl->grabberFilter.Get(),
                                           impl->nullFilter.Get())))
        return fail("RenderStream", E_FAIL);

    // Resolve the negotiated frame size.
    AM_MEDIA_TYPE connected{};
    if (SUCCEEDED(impl->grabber->GetConnectedMediaType(&connected))) {
        if (connected.formattype == FORMAT_VideoInfo && connected.pbFormat) {
            auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(connected.pbFormat);
            impl->width = (uint32_t)vih->bmiHeader.biWidth;
            impl->bottomUp = vih->bmiHeader.biHeight > 0;
            impl->height = (uint32_t)abs(vih->bmiHeader.biHeight);
        }
        if (connected.pbFormat) CoTaskMemFree(connected.pbFormat);
        if (connected.pUnk) connected.pUnk->Release();
    }
    if (impl->width == 0 || impl->height == 0) return fail("no video format negotiated", E_FAIL);

    if (FAILED(impl->graph->QueryInterface(IID_PPV_ARGS(impl->control.GetAddressOf()))))
        return fail("IMediaControl", E_FAIL);
    if (FAILED(impl->control->Run())) return fail("Run", E_FAIL);

    Diag::Log("[video] %ls (DirectShow %ux%u)", impl->active.c_str(), impl->width, impl->height);
    return true;
}

void DshowCapture::Stop() {
    if (!impl_) return;
    Impl* impl = impl_;
    impl_ = nullptr;
    if (impl->control) impl->control->Stop();
    if (impl->grabber && impl->grabberRaw) impl->grabber->SetCallback(nullptr, 1);
    impl->graph.Reset();
    impl->builder.Reset();
    impl->captureFilter.Reset();
    impl->grabber.Reset();
    impl->grabberFilter.Reset();
    impl->nullFilter.Reset();
    impl->control.Reset();
    impl->Release();  // the callback held one reference
}

bool DshowCapture::GetLatest(std::vector<uint8_t>* out, uint32_t* width, uint32_t* height,
                             uint64_t* frameNo) {
    if (!impl_) return false;
    Impl* impl = impl_;
    std::lock_guard<std::mutex> lk(impl->mu);
    if (impl->buf.empty() || impl->width == 0) return false;
    if (impl->frameNo == *frameNo) return false;

    const size_t stride = (size_t)impl->width * 4;
    if (impl->buf.size() < stride * impl->height) return false;  // not a full frame yet
    out->resize(stride * impl->height);
    if (impl->bottomUp) {
        for (uint32_t y = 0; y < impl->height; ++y)
            memcpy(out->data() + (size_t)y * stride,
                   impl->buf.data() + (size_t)(impl->height - 1 - y) * stride, stride);
    } else {
        memcpy(out->data(), impl->buf.data(), stride * impl->height);
    }
    *width = impl->width;
    *height = impl->height;
    *frameNo = impl->frameNo;
    return true;
}

std::wstring DshowCapture::ActiveDevice() const {
    return impl_ ? impl_->active : L"";
}
