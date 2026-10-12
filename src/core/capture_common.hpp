#pragma once
#include <cstdint>
#include <functional>
#include <d3d11.h>

// Called once per frame on the capture thread.
// `tex` is a borrowed BGRA8 texture, valid only for the duration of the call:
//   - WGC: valid until the callback returns
//   - DXGI duplication: our retained copy, always valid (repeated for CFR when
//     the desktop has not changed)
using FrameCallback =
    std::function<void(ID3D11Texture2D* tex, uint32_t width, uint32_t height, uint64_t frameIdx)>;
