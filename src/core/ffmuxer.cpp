#include "ffmuxer.hpp"
#include "log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

static std::string AvErr(int e) {
    char b[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, b, sizeof(b));
    return b;
}

static void SetStreamExtradata(AVCodecParameters* par, const std::vector<uint8_t>& data) {
    if (data.empty()) return;
    par->extradata = (uint8_t*)av_mallocz(data.size() + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(par->extradata, data.data(), data.size());
    par->extradata_size = (int)data.size();
}

bool FfMuxer::Open(const std::string& url, const Config& cfg, std::string* err) {
    std::lock_guard<std::mutex> lock(mu_);

    // Streaming URLs have no extension: pick the container from the scheme.
    const char* fmtName = nullptr;
    if (url.rfind("rtmp://", 0) == 0 || url.rfind("rtmps://", 0) == 0) fmtName = "flv";

    int ret = avformat_alloc_output_context2(&fmt_, nullptr, fmtName, url.c_str());
    if (ret < 0 || !fmt_) {
        *err = "cannot choose an output format for: " + url;
        return false;
    }

    AVStream* vs = avformat_new_stream(fmt_, nullptr);
    if (!vs) {
        *err = "avformat_new_stream failed";
        avformat_free_context(fmt_);
        fmt_ = nullptr;
        return false;
    }
    vs->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vs->codecpar->codec_id = cfg.hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    vs->codecpar->width = cfg.width;
    vs->codecpar->height = cfg.height;
    vs->codecpar->format = AV_PIX_FMT_YUV420P;
    SetStreamExtradata(vs->codecpar, cfg.videoExtradata);
    vs->time_base = {1, (int)cfg.fps};
    videoStream_ = vs->index;

    for (int t = 0; t < cfg.audioTracks && t < 2; ++t) {
        AVStream* as = avformat_new_stream(fmt_, nullptr);
        if (!as) {
            *err = "avformat_new_stream(audio) failed";
            avformat_free_context(fmt_);
            fmt_ = nullptr;
            return false;
        }
        as->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        as->codecpar->codec_id = AV_CODEC_ID_AAC;
        as->codecpar->sample_rate = cfg.audioSampleRate;
        av_channel_layout_default(&as->codecpar->ch_layout, 2);
        as->codecpar->format = AV_SAMPLE_FMT_FLTP;
        SetStreamExtradata(as->codecpar, cfg.audioExtradata);
        as->time_base = {1, cfg.audioSampleRate};
        audioStreams_[t] = as->index;
    }
    if (cfg.audioTracks == 0) {
        audioStreams_[0] = audioStreams_[1] = -1;
    }

    fps_ = (int)cfg.fps;
    sampleRate_ = cfg.audioSampleRate;
    bytes_ = 0;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rw_timeout", "10000000", 0);  // 10 s: Stop() must not hang on a dead socket
    ret = avio_open2(&fmt_->pb, url.c_str(), AVIO_FLAG_WRITE, nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        *err = "avio_open: " + AvErr(ret);
        avformat_free_context(fmt_);
        fmt_ = nullptr;
        return false;
    }

    ret = avformat_write_header(fmt_, nullptr);
    if (ret < 0) {
        *err = "write_header: " + AvErr(ret);
        avio_closep(&fmt_->pb);
        avformat_free_context(fmt_);
        fmt_ = nullptr;
        return false;
    }
    return true;
}

bool FfMuxer::WriteVideo(const uint8_t* data, size_t size, bool keyframe, int64_t ptsFrames,
                         std::string* err) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!fmt_ || videoStream_ < 0) return false;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        *err = "av_packet_alloc failed";
        return false;
    }
    int ret = av_new_packet(pkt, (int)size);
    if (ret < 0) {
        av_packet_free(&pkt);
        *err = "av_new_packet: " + AvErr(ret);
        return false;
    }
    memcpy(pkt->data, data, size);
    pkt->pts = pkt->dts = ptsFrames;
    pkt->duration = 1;
    pkt->stream_index = videoStream_;
    if (keyframe) pkt->flags |= AV_PKT_FLAG_KEY;

    // Muxers may override the stream time base during write_header (MKV -> 1/1000,
    // MP4 -> 1/(fps*256), FLV -> 1/1000); our pts are frame indices in a 1/fps base.
    const AVRational srcTb{1, fps_};
    av_packet_rescale_ts(pkt, srcTb, fmt_->streams[videoStream_]->time_base);

    ret = av_interleaved_write_frame(fmt_, pkt);
    if (ret >= 0) bytes_ += size;
    av_packet_free(&pkt);
    if (ret < 0) {
        *err = "write_frame(video): " + AvErr(ret);
        return false;
    }
    return true;
}

bool FfMuxer::WriteAudio(int track, const uint8_t* data, size_t size, int64_t ptsSamples,
                         std::string* err) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!fmt_ || track < 0 || track > 1 || audioStreams_[track] < 0) return false;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        *err = "av_packet_alloc failed";
        return false;
    }
    int ret = av_new_packet(pkt, (int)size);
    if (ret < 0) {
        av_packet_free(&pkt);
        *err = "av_new_packet: " + AvErr(ret);
        return false;
    }
    memcpy(pkt->data, data, size);
    pkt->pts = pkt->dts = ptsSamples;
    pkt->duration = 1024;
    pkt->stream_index = audioStreams_[track];
    pkt->flags |= AV_PKT_FLAG_KEY;

    const AVRational srcTb{1, sampleRate_};
    av_packet_rescale_ts(pkt, srcTb, fmt_->streams[audioStreams_[track]]->time_base);

    ret = av_interleaved_write_frame(fmt_, pkt);
    if (ret >= 0) bytes_ += size;
    av_packet_free(&pkt);
    if (ret < 0) {
        *err = "write_frame(audio): " + AvErr(ret);
        return false;
    }
    return true;
}

bool FfMuxer::IsOpen() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mu_));
    return fmt_ != nullptr;
}

void FfMuxer::Close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!fmt_) return;
    if (fmt_->pb) {
        av_write_trailer(fmt_);
        avio_closep(&fmt_->pb);
    }
    avformat_free_context(fmt_);
    fmt_ = nullptr;
    videoStream_ = -1;
    audioStreams_[0] = audioStreams_[1] = -1;
}
