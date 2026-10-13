#include "audio_mixer.hpp"
#include "log.hpp"

#include <algorithm>
#include <chrono>

bool AudioMixer::Prepare(const AudioConfig& cfg, int sampleRate, std::string* err) {
    cfg_ = cfg;
    sampleRate_ = sampleRate;
    gateOn_ = cfg.gate;
    gateDb_ = cfg.gateDb;
    gateHoldSamples_ = (int)(0.120f * sampleRate);
    gateEnv_ = 1.f;
    gateHold_ = 0;
    compOn_ = cfg.comp;
    compDb_ = cfg.compDb;
    compRatio_ = cfg.compRatio;
    compEnv_ = 0.f;
    if (cfg.gate)
        std::printf("[audio] mic noise gate on (%.0f dB)\n", cfg.gateDb);
    if (cfg.comp)
        std::printf("[audio] mic compressor on (%.0f dB, %.1f:1)\n", cfg.compDb, cfg.compRatio);
    if (encMix_) {
        encMix_->Shutdown();
        encMix_.reset();
    }
    if (encMic_) {
        encMic_->Shutdown();
        encMic_.reset();
    }
    encMix_ = std::make_unique<AacEncoder>();
    std::string e;
    if (!encMix_->Init(sampleRate, 2, cfg.bitrateKbps, &e)) {
        *err = "mix encoder: " + e;
        encMix_.reset();
        return false;
    }
    if (cfg.tracks == 2 && cfg.mic) {
        encMic_ = std::make_unique<AacEncoder>();
        if (!encMic_->Init(sampleRate, 2, cfg.bitrateKbps, &e)) {
            std::printf("[audio] mic track encoder unavailable, using single track: %s\n", e.c_str());
            encMic_.reset();
        }
    }
    return true;
}

bool AudioMixer::StartDevices(std::string* err) {
    std::string e;
    if (cfg_.desktop) {
        desktop_ = std::make_unique<WasapiCapture>();
        if (!desktop_->Start(true, cfg_.desktopDevice.empty() ? L"default" : cfg_.desktopDevice, &e)) {
            std::printf("[audio] desktop loopback unavailable: %s\n", e.c_str());
            desktop_.reset();
        } else {
            std::printf("[audio] desktop: %ls\n", desktop_->ActiveDevice().c_str());
        }
    }
    if (cfg_.mic) {
        mic_ = std::make_unique<WasapiCapture>();
        if (!mic_->Start(false, cfg_.micDevice.empty() ? L"default" : cfg_.micDevice, &e)) {
            std::printf("[audio] microphone unavailable: %s\n", e.c_str());
            mic_.reset();
        } else {
            std::printf("[audio] microphone: %ls\n", mic_->ActiveDevice().c_str());
        }
    }
    if (!AnyActive()) {
        *err = "no audio sources available";
        return false;
    }
    if (cfg_.monitor) {
        SetMonitoring(true, cfg_.monitorDevice);
        if (!monitor_) {
            std::printf("[audio] monitoring disabled (no output device)\n");
        } else if (cfg_.desktop) {
            std::printf("[audio] note: monitor + desktop audio can feed back through speakers\n");
        }
    }
    return true;
}

void AudioMixer::AttachSink(const Sink& sink) {
    sink_ = sink;
    // Drop everything buffered before the video pipeline started.
    if (desktop_) desktop_->ring.DropAll();
    if (mic_) mic_->ring.DropAll();
    quit_ = false;
    pump_ = std::thread(&AudioMixer::Pump, this);
}

bool AudioMixer::Fill(const std::unique_ptr<WasapiCapture>& src, float gain, float* dst) {    const size_t F = (size_t)encMix_->FrameSize();
    size_t got = src->ring.Read(dst, F);
    for (size_t i = got; i < F; ++i) {
        dst[2 * i] = 0.f;
        dst[2 * i + 1] = 0.f;
    }
    for (size_t i = 0; i < F * 2; ++i) dst[i] *= gain;
    return got > 0;
}

void AudioMixer::SetMonitoring(bool on, const std::wstring& device) {
    if (on && !monitor_) {
        auto mon = std::make_unique<WasapiRender>();
        std::string e;
        if (mon->Start(device, sampleRate_, &e)) {
            monitor_ = std::move(mon);
        } else {
            std::printf("[audio] monitoring unavailable: %s\n", e.c_str());
        }
    } else if (!on && monitor_) {
        monitor_->Stop();
        monitor_.reset();
        std::printf("[audio] monitoring off\n");
    }
}

void AudioMixer::RecoverDevice(std::unique_ptr<WasapiCapture>& src, const std::wstring& match,
                               bool loopback, std::chrono::steady_clock::time_point* lastTry,
                               const char* label) {
    if (!src || src->running.load() || !src->deviceLost.load()) return;
    const auto now = std::chrono::steady_clock::now();
    if (*lastTry != std::chrono::steady_clock::time_point{} &&
        now - *lastTry < std::chrono::seconds(3))
        return;
    *lastTry = now;

    src->Stop();
    auto fresh = std::make_unique<WasapiCapture>();
    std::string e;
    if (fresh->Start(loopback, match.empty() ? L"default" : match, &e)) {
        src = std::move(fresh);
        std::printf("[audio] %s reconnected: %ls\n", label, src->ActiveDevice().c_str());
        if (loopback)
            loggedDesktopLost_ = false;
        else
            loggedMicLost_ = false;
    } else {
        std::printf("[audio] %s reconnect failed: %s\n", label, e.c_str());
    }
}

void AudioMixer::Pump() {
    const int F = encMix_->FrameSize();
    std::vector<float> d((size_t)F * 2), m((size_t)F * 2), mix((size_t)F * 2);

    // Diagnostics: emitted block rate and padding ratio per second.
    auto lastLog = std::chrono::steady_clock::now();
    uint64_t blocksSince = 0;
    uint64_t srcDSince = 0, srcMSince = 0;
    uint64_t lastSrcD = 0, lastSrcM = 0;

    while (!quit_.load()) {
        // Surface endpoint loss once instead of silently padding silence.
        if (desktop_ && !desktop_->running.load() && desktop_->deviceLost.load() &&
            !loggedDesktopLost_) {
            loggedDesktopLost_ = true;
            std::printf("[audio] desktop device lost; trying to reconnect\n");
        }
        if (mic_ && !mic_->running.load() && mic_->deviceLost.load() && !loggedMicLost_) {
            loggedMicLost_ = true;
            std::printf("[audio] microphone device lost; trying to reconnect\n");
        }
        // Auto-recovery: unplugging/replugging a headset (or waking a device)
        // restarts that endpoint in place; the ring gap is a few milliseconds.
        RecoverDevice(desktop_, cfg_.desktopDevice, true, &lastDesktopTry_, "desktop");
        RecoverDevice(mic_, cfg_.micDevice, false, &lastMicTry_, "microphone");
        if (pausedFn_ && pausedFn_()) {
            if (desktop_) desktop_->ring.DropAll();
            if (mic_) mic_->ring.DropAll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Wait for a full block from each active source (or give up and pad).
        auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            bool okD = !DesktopActive() || desktop_->ring.AvailableFrames() >= (size_t)F;
            bool okM = !MicActive() || mic_->ring.AvailableFrames() >= (size_t)F;
            if (okD && okM) break;
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > 0.05)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        if (DesktopActive())
            Fill(desktop_, cfg_.desktopGain * liveDeskGain_.load(), d.data());
        else
            std::fill(d.begin(), d.end(), 0.f);
        if (MicActive())
            Fill(mic_, cfg_.micGain * liveMicGain_.load(), m.data());
        else
            std::fill(m.begin(), m.end(), 0.f);

        // Noise gate on the mic track: attenuate below the threshold so
        // keyboard/fan noise never reaches the mix. Envelope follower with
        // ~2 ms attack, ~120 ms hold and ~150 ms release (no lookahead).
        if (gateOn_.load()) {
            const float thr = powf(10.f, gateDb_.load() / 20.f);
            const float atk = 1.f - expf(-1.f / (0.002f * (float)sampleRate_));
            const float rel = 1.f - expf(-1.f / (0.150f * (float)sampleRate_));
            for (int i = 0; i < F; ++i) {
                const float a = (std::max)(fabsf(m[2 * i]), fabsf(m[2 * i + 1]));
                if (a > thr) {
                    gateEnv_ += (1.f - gateEnv_) * atk;
                    gateHold_ = gateHoldSamples_;
                } else if (gateHold_ > 0) {
                    --gateHold_;
                } else {
                    gateEnv_ += (0.f - gateEnv_) * rel;
                }
                m[2 * i] *= gateEnv_;
                m[2 * i + 1] *= gateEnv_;
            }
        } else if (gateEnv_ != 1.f) {
            gateEnv_ = 1.f;
            gateHold_ = 0;
        }

        // Mic compressor: reduce anything above the threshold by the ratio,
        // then a -1 dBFS safety clip so the mix never hard-clips.
        if (compOn_.load()) {
            const float thr = powf(10.f, compDb_.load() / 20.f);
            const float ratio = compRatio_.load() < 1.f ? 1.f : compRatio_.load();
            const float atk = 1.f - expf(-1.f / (0.005f * (float)sampleRate_));  // ~5 ms
            const float rel = 1.f - expf(-1.f / (0.120f * (float)sampleRate_));  // ~120 ms
            const float ceil = 0.891f;  // -1 dBFS
            for (int i = 0; i < F; ++i) {
                const float peak = (std::max)(fabsf(m[2 * i]), fabsf(m[2 * i + 1]));
                compEnv_ += (peak - compEnv_) * (peak > compEnv_ ? atk : rel);
                float gain = 1.f;
                if (compEnv_ > thr && compEnv_ > 1e-6f)
                    gain = (thr + (compEnv_ - thr) / ratio) / compEnv_;
                float l = m[2 * i] * gain, r = m[2 * i + 1] * gain;
                m[2 * i] = std::clamp(l, -ceil, ceil);
                m[2 * i + 1] = std::clamp(r, -ceil, ceil);
            }
        } else if (compEnv_ != 0.f) {
            compEnv_ = 0.f;
        }

        // Monitoring: play the post-filter mic to the chosen output device.
        if (monitor_ && monitor_->running()) monitor_->ring.Write(m.data(), (size_t)F);

        // Diagnostics: how much real audio went in vs media time emitted.
        {
            blocksSince++;
            uint64_t srcD = desktop_ ? desktop_->samplesIn.load() : 0;
            uint64_t srcM = mic_ ? mic_->samplesIn.load() : 0;
            srcDSince += srcD - lastSrcD;
            srcMSince += srcM - lastSrcM;
            lastSrcD = srcD;
            lastSrcM = srcM;
        }

        for (int i = 0; i < F * 2; ++i) {
            float v = d[i] + m[i];
            mix[i] = v > 1.f ? 1.f : (v < -1.f ? -1.f : v);
        }

        std::string e;
        encMix_->EncodeBlock(mix.data(), F,
                             [this](const uint8_t* p, size_t s, int64_t pts) {
                                 if (sink_) sink_(0, p, s, pts);
                             },
                             &e);
        if (encMic_ && MicActive()) {
            std::string e2;
            encMic_->EncodeBlock(m.data(), F,
                                 [this](const uint8_t* p, size_t s, int64_t pts) {
                                     if (sink_) sink_(1, p, s, pts);
                                 },
                                 &e2);
        }

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - lastLog).count();
        if (dt >= 1.0) {
            lastLog = now;
            const double emitted = (double)blocksSince * F / dt;
            const double gotD = (double)srcDSince / dt;
            const double gotM = (double)srcMSince / dt;
            std::printf("[audio] %.1f blocks/s (%.0f samples/s out) | src: desktop %.0f/s, mic %.0f/s\n",
                        blocksSince / dt, emitted, gotD, gotM);
            blocksSince = srcDSince = srcMSince = 0;
        }
    }
}

std::vector<uint8_t> AudioMixer::MixExtradata() const {
    return encMix_ ? encMix_->Extradata() : std::vector<uint8_t>{};
}
std::vector<uint8_t> AudioMixer::MicExtradata() const {
    return encMic_ ? encMic_->Extradata() : std::vector<uint8_t>{};
}

void AudioMixer::Stop() {
    quit_ = true;
    if (pump_.joinable()) pump_.join();
    // Flush the encoder's delayed frames into the sink (still open at this point).
    if (encMix_ && sink_) {
        encMix_->Flush([this](const uint8_t* d, size_t s, int64_t pts) { sink_(0, d, s, pts); });
    }
    if (desktop_) {
        desktop_->Stop();
        desktop_.reset();
    }
    if (mic_) {
        mic_->Stop();
        mic_.reset();
    }
    if (encMix_) {
        encMix_->Shutdown();
        encMix_.reset();
    }
    if (encMic_) {
        encMic_->Shutdown();
        encMic_.reset();
    }
}
