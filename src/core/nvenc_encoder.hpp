#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <ffnvcodec/nvEncodeAPI.h>

#include "encoder_config.hpp"

// Thin NVENC wrapper. Video never touches system memory and never touches the
// CPU: the captured BGRA texture is copied straight into NVENC's registered
// D3D11 input surface (a GPU-side copy) and the encoder emits a raw bitstream
// that goes directly to the muxer.
//
// Turing (RTX 20) and Ampere (RTX 30) share the same encode capabilities:
// H.264 + HEVC, no AV1. Capability differences (e.g. B-frames) are handled by
// falling back if the driver rejects the requested configuration.
class NvencEncoder {
public:
    bool Init(ID3D11Device* device, const EncoderConfig& cfg, std::string* err);
    bool RegisterTexture(ID3D11Texture2D* tex, std::string* err);
    // Returns false only on real errors. On success `out` may be empty when the
    // encoder needs more input (B-frames/lookahead) before producing a packet;
    // `outPts` then receives the encoder-assigned timestamp of that packet.
    bool Encode(ID3D11Texture2D* tex, uint64_t frameIdx, bool forceIdr, std::vector<uint8_t>* out,
                bool* keyframe, int64_t* outPts, std::string* err);
    std::vector<uint8_t> SequenceHeader();
    bool SupportsHevc() const { return cfg_.hevc; }
    void Shutdown();

private:
    EncoderConfig cfg_{};
    NV_ENCODE_API_FUNCTION_LIST fn_{};
    void* session_ = nullptr;
    NV_ENC_REGISTERED_PTR registered_ = nullptr;
    NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
    HMODULE lib_ = nullptr;
    bool open_ = false;
};
