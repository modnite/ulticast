#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "aac_encoder.hpp"
#include "wasapi_capture.hpp"

struct AudioConfig {
    bool desktop = true;
    bool mic = false;
    std::wstring desktopDevice;  // "default" or a friendly-name substring
    std::wstring micDevice;
    float desktopGain = 1.0f;
    float micGain = 1.0f;
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
    bool Fill(const std::unique_ptr<WasapiCapture>& src, float gain, float* dst);

    AudioConfig cfg_;
    std::unique_ptr<WasapiCapture> desktop_, mic_;
    std::unique_ptr<AacEncoder> encMix_, encMic_;
    Sink sink_;
    std::function<bool()> pausedFn_;
    std::thread pump_;
    std::atomic<bool> quit_{false};
    int sampleRate_ = 48000;
    bool loggedDesktopLost_ = false;
    bool loggedMicLost_ = false;
};
