#pragma once
#include <cstdint>
#include <string>

// Everything the encoder needs to know, independent of NVENC types so the
// UI/CLI layer can construct it without pulling in the SDK headers.
struct EncoderConfig {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;

    bool hevc = true;  // HEVC for local recording; H.264 for maximum platform support

    enum class RateMode { CQ, CBR };
    RateMode rateMode = RateMode::CQ;
    uint32_t cqp = 20;            // CQ level (1..51, lower = better/larger)
    uint32_t bitrateKbps = 6000;  // target bitrate for CBR (streaming)
    uint32_t maxBitrateKbps = 0;  // CBR peak; 0 = same as target

    int preset = 5;        // NVENC preset P1 (fastest) .. P7 (slowest/best)
    int bframes = 0;       // B-frames (0 disables); driver falls back if unsupported
    bool lookahead = false;   // lookahead rate control (better quality, adds latency)
    bool multipass = false;   // two-pass full-resolution rate control
    uint32_t gopSeconds = 2;
};
