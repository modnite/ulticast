#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct AVFormatContext;

// Container/network output. FFmpeg is used ONLY for muxing and transport
// (MP4/MKV files, RTMP(S) streams) -- never for video encoding.
//
// Thread-safe: the video callback thread and the audio pump thread can write
// concurrently; libavformat's interleaver orders packets by DTS.
class FfMuxer {
public:
    struct Config {
        int width = 0;
        int height = 0;
        uint32_t fps = 60;
        bool hevc = true;
        std::vector<uint8_t> videoExtradata;
        int audioTracks = 0;  // 0, 1 (mix) or 2 (mix + mic-only)
        int audioSampleRate = 48000;
        std::vector<uint8_t> audioExtradata;
    };

    bool Open(const std::string& url, const Config& cfg, std::string* err);
    bool WriteVideo(const uint8_t* data, size_t size, bool keyframe, int64_t ptsFrames,
                    std::string* err);
    bool WriteAudio(int track, const uint8_t* data, size_t size, int64_t ptsSamples,
                    std::string* err);
    bool IsOpen() const;
    uint64_t BytesWritten() const { return bytes_.load(); }
    void Close();

private:
    std::mutex mu_;
    AVFormatContext* fmt_ = nullptr;
    int videoStream_ = -1;
    int audioStreams_[2] = {-1, -1};
    int fps_ = 60;
    int sampleRate_ = 48000;
    std::atomic<uint64_t> bytes_{0};
};
