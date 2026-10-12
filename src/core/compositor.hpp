#pragma once
#include <cstdint>
#include <string>
#include <d3d11.h>
#include <wrl/client.h>

// A scene layer: a texture drawn at a pixel rectangle, top-left origin.
struct LayerDraw {
    ID3D11ShaderResourceView* srv = nullptr;
    float x = 0, y = 0, w = 0, h = 0;  // pixels in canvas space
    float opacity = 1.0f;

    // Optional chroma key (per-layer, GPU-side in the same pass).
    bool key = false;
    float keyR = 0.0f, keyG = 1.0f, keyB = 0.0f;
    float similarity = 0.32f;  // distance below which pixels are fully keyed
    float smoothness = 0.14f;  // soft edge width above similarity

    // Texture alpha handling (browser overlays): use the source alpha channel,
    // and un-premultiply it if the source provides premultiplied alpha.
    bool useAlpha = false;
    bool premultiplied = false;
};

// Minimal D3D11 layer compositor. Renders into any BGRA8 render target --
// in this app, directly into the NVENC input texture (one GPU pass, no copies).
class Compositor {
public:
    bool Init(ID3D11Device* device, ID3D11DeviceContext* ctx, uint32_t canvasW, uint32_t canvasH,
              std::string* err);
    void Render(ID3D11RenderTargetView* target, const LayerDraw* layers, int count);
    static Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> CreateSrv(ID3D11Device* device,
                                                                     ID3D11Texture2D* tex);

private:
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    uint32_t canvasW_ = 0, canvasH_ = 0;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
};
