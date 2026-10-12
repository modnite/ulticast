#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <d3d11.h>
#include <wrl/client.h>

#include "encoder_config.hpp"
#include "nvenc_encoder.hpp"

// Replay buffer: continuously encodes into a RAM ring of *encoded packets*
// (not raw frames -- 30 s of 1080p60 raw BGRA would be ~14 GB; encoded it is
// tens of MB). On Save(), the ring is swapped out and written to a file on a
// background thread, starting at the newest keyframe before the cut point.
//
// Runs its own NVENC session so it works with or without recording/streaming.
class ReplayBuffer {
public:
    bool Init(ID3D11Device* device, const EncoderConfig& cfg, int seconds, std::string prefix,
              const std::vector<uint8_t>& audioExtradata, std::string* err);
    bool RegisterTexture(ID3D11Texture2D* tex, std::string* err);
    void Disable();  // called when texture registration failed
    void PushVideo(ID3D11Texture2D* tex, uint64_t frameIdx);
    void PushAudio(const uint8_t* data, size_t size, int64_t ptsSamples);
    int Save();  // 0 = save started, -1 = disabled or already saving
    void Stop();

    bool Enabled() const { return enabled_.load(); }
    int SaveCount() const { return saveCount_.load(); }

private:
    struct Packet {
        std::vector<uint8_t> data;
        int64_t pts = 0;    // native units (frames / samples)
        int64_t ptsUs = 0;  // microseconds, for trimming
        bool key = false;
        bool audio = false;
    };
    void SaveWorker(std::vector<Packet> packets);
    static int64_t ToUs(int64_t units, int64_t perSecond) {
        return perSecond ? units * 1000000 / perSecond : 0;
    }

    NvencEncoder enc_;
    EncoderConfig cfg_{};
    int seconds_ = 30;
    std::string prefix_;
    std::vector<uint8_t> videoExtradata_;
    std::vector<uint8_t> audioExtradata_;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> saving_{false};
    std::atomic<int> saveCount_{0};
    std::mutex mu_;
    std::deque<Packet> ring_;
    int64_t newestUs_ = 0;
    std::thread saveThread_;
};
