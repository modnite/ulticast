#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// AAC-LC encoder (FFmpeg libavcodec) fed with interleaved stereo float32.
// FFmpeg types are hidden behind void* so the rest of the app never includes
// avcodec headers.
class AacEncoder {
public:
    using PacketCallback = std::function<void(const uint8_t* data, size_t size, int64_t ptsSamples)>;

    bool Init(int sampleRate, int channels, int bitrateKbps, std::string* err);
    bool EncodeBlock(const float* interleaved, int frames, const PacketCallback& cb,
                     std::string* err);
    // Drain the encoder's delayed frames (send EOS and emit remaining packets).
    void Flush(const PacketCallback& cb);
    std::vector<uint8_t> Extradata() const { return extradata_; }
    int FrameSize() const { return frameSize_; }
    void Shutdown();

private:
    void* ctx_ = nullptr;    // AVCodecContext*
    void* frame_ = nullptr;  // AVFrame*
    void* pkt_ = nullptr;    // AVPacket*
    std::vector<uint8_t> extradata_;
    int64_t pts_ = 0;
    int frameSize_ = 1024;
    int channels_ = 2;
};
