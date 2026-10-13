#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include "video_capture.hpp"

// Webcam capture via Media Foundation's Source Reader.
// The MF thread fills double-buffered packed BGRA frames; the video thread
// pulls the latest complete buffer and uploads it to a D3D11 texture.
class WebcamCapture : public VideoCaptureBase {
public:
    static std::vector<std::wstring> ListDevices();

    bool Start(const std::wstring& nameSubstr, std::string* err) override;
    void Stop() override;

    std::wstring ActiveDevice() const override {
        std::lock_guard<std::mutex> lock(nameMu_);
        return activeDevice_;
    }

    // Returns the latest frame buffer (packed BGRA, width*height*4) and its
    // dimensions; frameNumber increments when a new frame is available.
    bool GetLatest(std::vector<uint8_t>* out, uint32_t* width, uint32_t* height,
                   uint64_t* frameNo) override;

private:
    void ThreadMain();
    bool Run(std::string* err);

    std::thread thread_;
    std::atomic<bool> quit_{false};
    HANDLE hReady_ = nullptr;
    std::string initError_;
    std::wstring match_;
    mutable std::mutex nameMu_;
    std::wstring activeDevice_;

    std::mutex mu_;
    std::vector<uint8_t> bufA_, bufB_;
    bool useA_ = true;
    uint32_t width_ = 0, height_ = 0;
    uint64_t frameNo_ = 0;
};
