#include "replay_buffer.hpp"
#include "ffmuxer.hpp"
#include "log.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iterator>
#include <thread>

bool ReplayBuffer::Init(ID3D11Device* device, const EncoderConfig& cfg, int seconds,
                        std::string prefix, const std::vector<uint8_t>& audioExtradata,
                        std::string* err) {
    cfg_ = cfg;
    seconds_ = seconds;
    prefix_ = std::move(prefix);
    audioExtradata_ = audioExtradata;

    if (!enc_.Init(device, cfg_, err)) return false;
    videoExtradata_ = enc_.SequenceHeader();
    if (videoExtradata_.empty()) {
        *err = "replay encoder produced no sequence header";
        return false;
    }
    enabled_ = true;
    return true;
}

bool ReplayBuffer::RegisterTexture(ID3D11Texture2D* tex, std::string* err) {
    return enc_.RegisterTexture(tex, err);
}

void ReplayBuffer::Disable() {
    enabled_ = false;
}

void ReplayBuffer::PushVideo(ID3D11Texture2D* tex, uint64_t frameIdx) {
    if (!enabled_.load()) return;
    std::vector<uint8_t> data;
    bool key = false;
    int64_t outPts = -1;
    std::string e;
    if (!enc_.Encode(tex, frameIdx, false, &data, &key, &outPts, &e)) {
        std::printf("[replay] encode error: %s\n", e.c_str());
        return;
    }
    if (data.empty()) return;  // encoder buffering (B-frames/lookahead)
    Packet p;
    p.data = std::move(data);
    p.pts = outPts;
    p.ptsUs = ToUs(p.pts, (int64_t)cfg_.fps);
    p.key = key;
    p.audio = false;

    std::lock_guard<std::mutex> lock(mu_);
    if (p.ptsUs > newestUs_) newestUs_ = p.ptsUs;
    ring_.push_back(std::move(p));
    while (!ring_.empty() && newestUs_ - ring_.front().ptsUs > (int64_t)seconds_ * 1000000)
        ring_.pop_front();
}

void ReplayBuffer::PushAudio(const uint8_t* data, size_t size, int64_t ptsSamples) {
    if (!enabled_.load() || audioExtradata_.empty()) return;
    Packet p;
    p.data.assign(data, data + size);
    p.pts = ptsSamples;
    p.ptsUs = ToUs(ptsSamples, 48000);
    p.audio = true;

    std::lock_guard<std::mutex> lock(mu_);
    if (p.ptsUs > newestUs_) newestUs_ = p.ptsUs;
    ring_.push_back(std::move(p));
    while (!ring_.empty() && newestUs_ - ring_.front().ptsUs > (int64_t)seconds_ * 1000000)
        ring_.pop_front();
}

int ReplayBuffer::Save() {
    if (!enabled_.load()) return -1;
    if (saving_.exchange(true)) return -1;

    std::vector<Packet> packets;
    {
        std::lock_guard<std::mutex> lock(mu_);
        packets.reserve(ring_.size());
        std::move(ring_.begin(), ring_.end(), std::back_inserter(packets));
        ring_.clear();
        newestUs_ = 0;
    }
    if (saveThread_.joinable()) saveThread_.join();  // previous save already finished
    saveThread_ = std::thread([this, packets = std::move(packets)]() mutable {
        SaveWorker(std::move(packets));
    });
    return 0;
}
void ReplayBuffer::SaveWorker(std::vector<Packet> packets) {
    if (packets.empty()) {
        std::printf("[replay] buffer is empty, nothing saved\n");
        saving_ = false;
        return;
    }

    // Start at the most recent keyframe so the file opens cleanly.
    size_t start = packets.size();
    for (size_t i = 0; i < packets.size(); ++i) {
        if (!packets[i].audio && packets[i].key) {
            start = i;
            break;
        }
    }
    if (start == packets.size()) {
        std::printf("[replay] no keyframe in buffer yet, try again in a moment\n");
        saving_ = false;
        return;
    }
    const int64_t cutUs = packets[start].ptsUs;
    const int64_t cutFrame = packets[start].pts;
    int64_t cutSample = -1;
    for (size_t i = 0; i < packets.size(); ++i) {
        if (packets[i].audio && packets[i].ptsUs >= cutUs) {
            cutSample = packets[i].pts;
            break;
        }
    }

    char stamp[64] = {};
    time_t t = time(nullptr);
    struct tm lt{};
    localtime_s(&lt, &t);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", &lt);
    const std::string path = prefix_ + " " + stamp + ".mkv";

    FfMuxer mux;
    FfMuxer::Config mc;
    mc.width = (int)cfg_.width;
    mc.height = (int)cfg_.height;
    mc.fps = cfg_.fps;
    mc.hevc = cfg_.hevc;
    mc.videoExtradata = videoExtradata_;
    if (!audioExtradata_.empty() && cutSample >= 0) {
        mc.audioTracks = 1;
        mc.audioExtradata = audioExtradata_;
    }

    std::string err;
    if (!mux.Open(path, mc, &err)) {
        std::printf("[replay] open failed: %s\n", err.c_str());
        saving_ = false;
        return;
    }

    int64_t vw = 0, aw = 0;
    int failures = 0;
    for (size_t i = start; i < packets.size(); ++i) {
        const Packet& p = packets[i];
        if (!p.audio) {
            if (!mux.WriteVideo(p.data.data(), p.data.size(), p.key, p.pts - cutFrame, &err))
                ++failures;
            ++vw;
        } else if (mc.audioTracks > 0 && p.pts >= cutSample) {
            if (!mux.WriteAudio(0, p.data.data(), p.data.size(), p.pts - cutSample, &err))
                ++failures;
            ++aw;
        }
    }
    mux.Close();
    if (failures == 0) saveCount_.fetch_add(1);
    std::printf("[replay] saved \"%s\" (%lld video, %lld audio packets)\n", path.c_str(),
                (long long)vw, (long long)aw);
    saving_ = false;
}

void ReplayBuffer::Stop() {
    enabled_ = false;
    // The save worker holds `this`: join it (it finishes on its own; no timeout).
    if (saveThread_.joinable()) saveThread_.join();
    enc_.Shutdown();
}
