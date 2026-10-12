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
};

// The single D3D11 device/context every capture + encode path shares.
// One device, one immediate context, everything on one thread: zero locks,
// zero cross-device copies. That is the point of this app.
struct D3DContext {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    static std::vector<AdapterInfo> ListAdapters();
    // adapterIndex < 0 = system default
    bool Create(std::string* err, int adapterIndex = -1);
};
