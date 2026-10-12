#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct AdapterInfo {
    int index = 0;
    std::wstring name;
    uint32_t vendorId = 0;  // PCI vendor: 0x10DE = NVIDIA, 0x8086 = Intel, 0x1002 = AMD
};

// The single D3D11 device/context every capture + encode path shares.
// One device, one immediate context, everything on one thread: zero locks,
// zero cross-device copies. That is the point of this app.
struct D3DContext {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    static std::vector<AdapterInfo> ListAdapters();
    // Index of the first NVIDIA adapter, or -1 when there is none.
    static int PreferredAdapter();
    // adapterIndex < 0 = auto (prefers the NVIDIA GPU so NVENC works)
    bool Create(std::string* err, int adapterIndex = -1);
};
