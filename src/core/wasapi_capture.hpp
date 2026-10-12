#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include "ring_buffer.hpp"

struct AudioDeviceInfo {
    std::wstring name;
    bool isDefault = false;
};

// WASAPI shared-mode capture. Two modes:
//   - loopback = true : captures what the system is playing (render endpoint loopback)
//   - loopback = false: captures a microphone (capture endpoint)
//
// Output is always converted to 48 kHz stereo float32 and pushed into the ring.
class WasapiCapture {
public:
    static std::vector<AudioDeviceInfo> ListRenderDevices();
    static std::vector<AudioDeviceInfo> ListCaptureDevices();

    // nameSubstr: "default" / empty -> system default endpoint, otherwise the
    // first endpoint whose friendly name contains the substring (case-insensitive).
    bool Start(bool loopback, const std::wstring& nameSubstr, std::string* err);
    void Stop();

    FloatRing ring;
    std::atomic<bool> running{false};
    std::atomic<bool> deviceLost{false};  // endpoint invalidated / capture failed
    std::atomic<uint64_t> samplesIn{0};  // diagnostic: total frames delivered to the ring
    std::wstring ActiveDevice() const {
        std::lock_guard<std::mutex> lock(nameMu_);
        return activeDevice_;
    }

private:
    void ThreadMain();
    bool Run(std::string* err);  // runs on the capture thread until quit

    std::thread thread_;
    std::atomic<bool> quit_{false};
    bool loopback_ = false;
    std::wstring match_;
    mutable std::mutex nameMu_;
    std::wstring activeDevice_;
    HANDLE hEvent_ = nullptr;
    HANDLE hQuit_ = nullptr;
    HANDLE hReady_ = nullptr;
    std::string initError_;
    std::vector<float> convertBuf_;
};
