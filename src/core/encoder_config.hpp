#pragma once
#include <cstdint>
#include <string>

// Everything the encoder needs to know, independent of NVENC types so the
// UI/CLI layer can construct it without pulling in the SDK headers.
struct EncoderConfig {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;

    bool hevc = true;      // HEVC for local recording; H.264 for maximum platform support
    bool cbr = false;      // CBR for streaming, CQP for local recording
    uint32_t cqp = 20;     // CQ level when cbr == false (lower = better/larger)
    uint32_t bitrateKbps = 6000;  // target bitrate when cbr == true
    int preset = 5;        // NVENC preset P1 (fastest) .. P7 (slowest/best)
    int bframes = 0;       // B-frames (0 disables); driver falls back if unsupported
    uint32_t gopSeconds = 2;
};
