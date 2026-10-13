#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Common interface for video capture devices. Media Foundation (webcam_mf.hpp)
// is the primary path; the DirectShow implementation (capture_dshow.hpp) is a
// fallback for virtual cameras that only register with DirectShow (some
// NVIDIA Broadcast builds, Snap Camera, older capture-card drivers).
class VideoCaptureBase {
public:
    virtual ~VideoCaptureBase() = default;
    // nameSubstr: "default"/empty = first device, otherwise a friendly-name
    // substring (case-insensitive).
    virtual bool Start(const std::wstring& nameSubstr, std::string* err) = 0;
    virtual void Stop() = 0;
    // Frames are BGRA (RGB32), tightly packed, top-down.
    virtual bool GetLatest(std::vector<uint8_t>* out, uint32_t* width, uint32_t* height,
                           uint64_t* frameNo) = 0;
    virtual std::wstring ActiveDevice() const = 0;
    virtual bool IsDirectShow() const { return false; }
};
