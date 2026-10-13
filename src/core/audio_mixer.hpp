#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "aac_encoder.hpp"
#include "wasapi_capture.hpp"
#include "wasapi_render.hpp"

struct AudioConfig {
    bool desktop = true;
    bool mic = false;
    std::wstring desktopDevice;  // "default" or a friendly-name substring
    std::wstring micDevice;
    float desktopGain = 1.0f;
    float micGain = 1.0f;
    bool gate = false;      // mic noise gate
    float gateDb = -45.f;   // gate threshold (dBFS)
    bool comp = false;      // mic compressor
    float compDb = -18.f;   // compressor threshold (dBFS)
    float compRatio = 3.0f; // compression ratio
    bool monitor = false;   // play the post-filter mic to an output device
    std::wstring monitorDevice;  // "" = default output
    int bitrateKbps = 160;
    int tracks = 1;  // 1 = mix only, 2 = mix + mic-only track (file output)
};

// Mixes desktop loopback + microphone into a single 48 kHz stereo float stream,
// encodes each track with AAC and hands packets to a sink (the muxer).
//
// Pre-roll policy: devices start before the video pipeline does; everything
// captured before the sink attaches is discarded so A/V both start at t=0.
class AudioMixer {
public:
    using Sink =
        std::function<bool(int track, const uint8_t* data, size_t size, int64_t ptsSamples)>;

    bool Prepare(const AudioConfig& cfg, int sampleRate, std::string* err);  // encoders only
    bool StartDevices(std::string* err);
    void AttachSink(const Sink& sink);
    void SetPausedFn(std::function<bool()> fn) { pausedFn_ = std::move(fn); }
    // Runtime mute/unmute of the audio sources (scene Audio In/Out toggles).
    void SetLiveSources(bool desktop, bool mic) {
        liveDeskGain_.store(desktop ? 1.f : 0.f);
        liveMicGain_.store(mic ? 1.f : 0.f);
    }
    // Runtime mic noise gate (threshold in dBFS).
    void SetGate(bool on, float db) {
        gateOn_.store(on);
        gateDb_.store(db);
    }
    // Runtime mic compressor (threshold dBFS, ratio >= 1).
    void SetCompressor(bool on, float db, float ratio) {
        compOn_.store(on);
        compDb_.store(db);
        compRatio_.store(ratio < 1.f ? 1.f : ratio);
    }
    // Mic monitoring (hear the post-filter mic in headphones). Safe at runtime.
    void SetMonitoring(bool on, const std::wstring& device);
    void Stop();

    bool DesktopActive() const { return desktop_ && desktop_->running.load(); }
    bool MicActive() const { return mic_ && mic_->running.load(); }
    bool AnyActive() const { return DesktopActive() || MicActive(); }

    std::vector<uint8_t> MixExtradata() const;
    std::vector<uint8_t> MicExtradata() const;
    std::wstring DesktopName() const { return desktop_ ? desktop_->ActiveDevice() : L"-"; }
    std::wstring MicName() const { return mic_ ? mic_->ActiveDevice() : L"-"; }

private:
    void Pump();
    void RecoverDevice(std::unique_ptr<WasapiCapture>& src, const std::wstring& match,
                       bool loopback, std::chrono::steady_clock::time_point* lastTry,
                       const char* label);
    bool Fill(const std::unique_ptr<WasapiCapture>& src, float gain, float* dst);

    AudioConfig cfg_;
    std::unique_ptr<WasapiCapture> desktop_, mic_;
    std::unique_ptr<WasapiRender> monitor_;
    std::unique_ptr<AacEncoder> encMix_, encMic_;
    Sink sink_;
    std::function<bool()> pausedFn_;
    std::thread pump_;
    std::atomic<bool> quit_{false};
    std::atomic<float> liveDeskGain_{1.f}, liveMicGain_{1.f};
    std::atomic<bool> gateOn_{false};
    std::atomic<float> gateDb_{-45.f};
    float gateEnv_ = 1.f;   // current gate gain (smoothed)
    int gateHold_ = 0;      // samples left in the hold window
    int gateHoldSamples_ = 5760;  // ~120 ms at 48 kHz
    std::atomic<bool> compOn_{false};
    std::atomic<float> compDb_{-18.f};
    std::atomic<float> compRatio_{3.f};
    float compEnv_ = 0.f;   // compressor envelope follower
    int sampleRate_ = 48000;
    bool loggedDesktopLost_ = false;
    bool loggedMicLost_ = false;
    std::chrono::steady_clock::time_point lastDesktopTry_{}, lastMicTry_{};
};
