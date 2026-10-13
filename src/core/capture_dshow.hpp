#pragma once
#include <string>
#include <vector>
#include <windows.h>

#include "video_capture.hpp"

// DirectShow video capture (fallback for cameras Media Foundation cannot see).
// Graph: capture filter -> SampleGrabber -> NullRenderer, RGB32, buffer
// callback into a locked frame buffer (same shape as WebcamCapture).
class DshowCapture : public VideoCaptureBase {
public:
    // Friendly names of all DirectShow video input devices.
    static std::vector<std::wstring> ListDevices();

    DshowCapture() = default;
    ~DshowCapture() override;
    DshowCapture(const DshowCapture&) = delete;
    DshowCapture& operator=(const DshowCapture&) = delete;

    bool Start(const std::wstring& nameSubstr, std::string* err) override;
    void Stop() override;
    bool GetLatest(std::vector<uint8_t>* out, uint32_t* width, uint32_t* height,
                   uint64_t* frameNo) override;
    std::wstring ActiveDevice() const override;
    bool IsDirectShow() const override { return true; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
