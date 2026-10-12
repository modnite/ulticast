#include "compositor.hpp"
#include "log.hpp"

#include <cstring>
#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;

static const char* kShaderSource = R"(
cbuffer Layer : register(b0) {
    float4 rect;      // x, y, width, height in NDC
    float  opacity;
    float3 pad;
    float4 keyColor;  // rgb + w = key enabled
    float4 keyParams; // x = similarity, y = smoothness
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    // Triangle strip: 0=(0,0) 1=(1,0) 2=(0,1) 3=(1,1)
    float2 corner = float2(id & 1, (id >> 1) & 1);
    VSOut o;
    o.pos = float4(rect.xy + corner * rect.zw, 0.0, 1.0);
    o.uv = corner;
    return o;
}

Texture2D tex : register(t0);
SamplerState smp : register(s0);

float4 PSMain(VSOut i) : SV_TARGET {
    float4 c = tex.Sample(smp, i.uv);
    float alpha = opacity;
    float3 rgb = c.rgb;
    if (keyParams.z > 0.5) {
        // source alpha channel drives blending (browser overlays)
        float a = c.a;
        if (keyParams.w > 0.5 && a > 0.004) rgb = rgb / a;  // un-premultiply
        alpha *= a;
    }
    if (keyColor.w > 0.5) {
        float d = distance(rgb, keyColor.rgb);
        alpha *= smoothstep(keyParams.x, keyParams.x + keyParams.y, d);
    }
    return float4(rgb, alpha);
}
)";

struct LayerCB {
    float rect[4];
    float opacity;
    float pad[3];
    float keyColor[4];
    float keyParams[4];
};

bool Compositor::Init(ID3D11Device* device, ID3D11DeviceContext* ctx, uint32_t canvasW,
                      uint32_t canvasH, std::string* err) {
    device_ = device;
    ctx_ = ctx;
    canvasW_ = canvasW;
    canvasH_ = canvasH;

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    HRESULT hr = D3DCompile(kShaderSource, strlen(kShaderSource), "Ulticast_scene", nullptr,
                            nullptr, "VSMain", "vs_5_0", 0, 0, vsBlob.GetAddressOf(),
                            errBlob.GetAddressOf());
    if (FAILED(hr)) {
        *err = "VS compile failed";
        return false;
    }
    errBlob.Reset();
    hr = D3DCompile(kShaderSource, strlen(kShaderSource), "Ulticast_scene", nullptr, nullptr,
                    "PSMain", "ps_5_0", 0, 0, psBlob.GetAddressOf(), errBlob.GetAddressOf());
    if (FAILED(hr)) {
        *err = "PS compile failed";
        return false;
    }

    if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                           nullptr, vs_.GetAddressOf())) ||
        FAILED(device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                          nullptr, ps_.GetAddressOf()))) {
        *err = "shader creation failed";
        return false;
    }

    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = sizeof(LayerCB);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&cb, nullptr, cb_.GetAddressOf()))) {
        *err = "constant buffer creation failed";
        return false;
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sd, sampler_.GetAddressOf()))) {
        *err = "sampler creation failed";
        return false;
    }

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&bd, blend_.GetAddressOf()))) {
        *err = "blend state creation failed";
        return false;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device_->CreateRasterizerState(&rd, raster_.GetAddressOf()))) {
        *err = "rasterizer state creation failed";
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    dsd.StencilEnable = FALSE;
    if (FAILED(device_->CreateDepthStencilState(&dsd, depth_.GetAddressOf()))) {
        *err = "depth state creation failed";
        return false;
    }
    return true;
}

ComPtr<ID3D11ShaderResourceView> Compositor::CreateSrv(ID3D11Device* device,
                                                       ID3D11Texture2D* tex) {
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = desc.Format;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> view;
    HRESULT hr = device->CreateShaderResourceView(tex, &srv, view.GetAddressOf());
    if (FAILED(hr)) return nullptr;
    return view;
}

void Compositor::Render(ID3D11RenderTargetView* target, const LayerDraw* layers, int count) {
    const float black[4] = {0.f, 0.f, 0.f, 1.f};
    ctx_->ClearRenderTargetView(target, black);
    ctx_->OMSetRenderTargets(1, &target, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)canvasW_;
    vp.Height = (float)canvasH_;
    vp.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &vp);

    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx_->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(depth_.Get(), 0);
    ctx_->RSSetState(raster_.Get());
    ctx_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    // D3D11 keeps separate constant-buffer slots per stage: the pixel shader
    // reads the layer opacity from its own b0 binding.
    ctx_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());

    const float W = (float)canvasW_;
    const float H = (float)canvasH_;

    for (int i = 0; i < count; ++i) {
        const LayerDraw& l = layers[i];
        if (!l.srv || l.w <= 0 || l.h <= 0) continue;

        // Pixel rect (top-left origin) -> NDC rect (y up), as (x, y, w, h)
        const float x0 = l.x / W * 2.f - 1.f;
        const float x1 = (l.x + l.w) / W * 2.f - 1.f;
        const float y0 = 1.f - l.y / H * 2.f;
        const float y1 = 1.f - (l.y + l.h) / H * 2.f;

        LayerCB cb = {};
        cb.rect[0] = x0;
        cb.rect[1] = y0;
        cb.rect[2] = x1 - x0;
        cb.rect[3] = y1 - y0;
        cb.opacity = l.opacity;
        cb.keyColor[0] = l.keyR;
        cb.keyColor[1] = l.keyG;
        cb.keyColor[2] = l.keyB;
        cb.keyColor[3] = l.key ? 1.0f : 0.0f;
        cb.keyParams[0] = l.similarity;
        cb.keyParams[1] = l.smoothness;
        cb.keyParams[2] = l.useAlpha ? 1.0f : 0.0f;
        cb.keyParams[3] = l.premultiplied ? 1.0f : 0.0f;

        D3D11_MAPPED_SUBRESOURCE map{};
        if (SUCCEEDED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            memcpy(map.pData, &cb, sizeof(cb));
            ctx_->Unmap(cb_.Get(), 0);
        }

        ctx_->PSSetShaderResources(0, 1, &l.srv);
        ctx_->Draw(4, 0);
    }

    ID3D11ShaderResourceView* none = nullptr;
    ctx_->PSSetShaderResources(0, 1, &none);
}
