#include "d3d.hpp"
#include "diag.hpp"
#include "log.hpp"

std::vector<AdapterInfo> D3DContext::ListAdapters() {
    std::vector<AdapterInfo> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(factory.GetAddressOf()))))
        return out;
    UINT i = 0;
    ComPtr<IDXGIAdapter1> ad;
    while (factory->EnumAdapters1(i, ad.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 d{};
        ad->GetDesc1(&d);
        out.push_back({(int)i, d.Description, d.VendorId});
        ++i;
    }
    return out;
}

// NVIDIA's PCI vendor id: NVENC only exists on these adapters.
static constexpr uint32_t kNvidiaVendor = 0x10DE;

int D3DContext::PreferredAdapter() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                  reinterpret_cast<void**>(factory.GetAddressOf()))))
        return -1;
    UINT i = 0;
    ComPtr<IDXGIAdapter1> ad;
    while (factory->EnumAdapters1(i, ad.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 d{};
        ad->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId == kNvidiaVendor) return (int)i;
        ++i;
    }
    return -1;  // no NVIDIA GPU: let the runtime pick the default adapter
}

bool D3DContext::Create(std::string* err, int adapterIndex) {
    ComPtr<IDXGIAdapter1> adapter;
    D3D_DRIVER_TYPE driver = D3D_DRIVER_TYPE_HARDWARE;

    if (adapterIndex < 0) {
        // Auto: NVENC lives on the NVIDIA GPU. On a desktop with an Intel iGPU
        // (e.g. 13600K + RTX 3080) adapter 0 is often the iGPU, and every NVENC
        // call then fails with "no encode device". Prefer NVIDIA explicitly.
        adapterIndex = PreferredAdapter();
        if (adapterIndex >= 0) {
            ComPtr<IDXGIFactory1> f;
            if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                             reinterpret_cast<void**>(f.GetAddressOf())))) {
                if (SUCCEEDED(f->EnumAdapters1((UINT)adapterIndex, adapter.GetAddressOf()))) {
                    DXGI_ADAPTER_DESC1 d{};
                    adapter->GetDesc1(&d);
                    Diag::Log("[d3d] auto-selected GPU: %ls", d.Description);
                    driver = D3D_DRIVER_TYPE_UNKNOWN;
                } else {
                    adapter.Reset();
                }
            }
        }
    }

    if (adapterIndex >= 0 && !adapter) {
        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                        reinterpret_cast<void**>(factory.GetAddressOf()));
        if (FAILED(hr)) {
            if (err) *err = "CreateDXGIFactory1 failed: " + Hex(hr);
            return false;
        }
        hr = factory->EnumAdapters1((UINT)adapterIndex, adapter.GetAddressOf());
        if (FAILED(hr)) {
            if (err) *err = "adapter index out of range";
            return false;
        }
        driver = D3D_DRIVER_TYPE_UNKNOWN;  // required when an explicit adapter is given
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(adapter.Get(), driver, nullptr, flags, levels, _countof(levels),
                                   D3D11_SDK_VERSION, device.GetAddressOf(), &got,
                                   context.GetAddressOf());
    if (hr == E_INVALIDARG) {
        // Pre-11.1 runtime: retry without the 11_1 feature level.
        hr = D3D11CreateDevice(adapter.Get(), driver, nullptr, flags, levels + 1,
                               _countof(levels) - 1, D3D11_SDK_VERSION, device.GetAddressOf(), &got,
                               context.GetAddressOf());
    }
    if (FAILED(hr)) {
        if (err) *err = "D3D11CreateDevice failed: " + Hex(hr);
        return false;
    }
    return true;
}
