#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>

#include "ring_buffer.hpp"

// Plays float32 stereo audio to a WASAPI render endpoint (shared mode with
// automatic sample-rate/format conversion). Used for mic monitoring: the mixer
// pushes the post-filter mic block into `ring` and this thread plays it out.
// Failures are non-fatal -- monitoring is a convenience, never part of the
// recording path.
class WasapiRender {
public:
    ~WasapiRender();
    WasapiRender() = default;
    WasapiRender(const WasapiRender&) = delete;
    WasapiRender& operator=(const WasapiRender&) = delete;

    bool Start(const std::wstring& nameSubstr, int sampleRate, std::string* err);
    void Stop();
    bool running() const { return running_.load(); }
    std::wstring ActiveDevice() const {
        std::lock_guard<std::mutex> lk(nameMu_);
        return active_;
    }

    FloatRing ring;

private:
    void ThreadMain();

    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> deviceLost_{false};
    HANDLE hReady_ = nullptr;
    std::string initError_;
    std::wstring match_;
    int sampleRate_ = 48000;
    mutable std::mutex nameMu_;
    std::wstring active_;
};
