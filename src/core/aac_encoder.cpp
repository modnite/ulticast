#include "aac_encoder.hpp"
#include "log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}

bool AacEncoder::Init(int sampleRate, int channels, int bitrateKbps, std::string* err) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        *err = "AAC encoder not available in this FFmpeg build";
        return false;
    }
    AVCodecContext* c = avcodec_alloc_context3(codec);
    if (!c) {
        *err = "avcodec_alloc_context3 failed";
        return false;
    }
    ctx_ = c;
    c->sample_fmt = AV_SAMPLE_FMT_FLTP;
    c->sample_rate = sampleRate;
    av_channel_layout_default(&c->ch_layout, channels);
    c->bit_rate = bitrateKbps * 1000;
    c->time_base = {1, sampleRate};
    if (avcodec_open2(c, codec, nullptr) < 0) {
        *err = "avcodec_open2(aac) failed";
        return false;
    }

    AVFrame* f = av_frame_alloc();
    if (!f) {
        *err = "av_frame_alloc failed";
        return false;
    }
    frame_ = f;
    f->format = c->sample_fmt;
    f->sample_rate = sampleRate;
    av_channel_layout_copy(&f->ch_layout, &c->ch_layout);
    f->nb_samples = c->frame_size;
    if (av_frame_get_buffer(f, 0) < 0) {
        *err = "av_frame_get_buffer failed";
        return false;
    }

    pkt_ = av_packet_alloc();
    if (!pkt_) {
        *err = "av_packet_alloc failed";
        return false;
    }

    frameSize_ = c->frame_size;
    channels_ = channels;
    if (c->extradata && c->extradata_size > 0)
        extradata_.assign(c->extradata, c->extradata + c->extradata_size);
    pts_ = 0;
    return true;
}

bool AacEncoder::EncodeBlock(const float* interleaved, int frames, const PacketCallback& cb,
                             std::string* err) {
    auto* c = static_cast<AVCodecContext*>(ctx_);
    auto* f = static_cast<AVFrame*>(frame_);
    auto* p = static_cast<AVPacket*>(pkt_);
    if (!c || !f || !p) {
        *err = "encoder not initialized";
        return false;
    }
    if (frames != frameSize_) {
        *err = "aac block size mismatch";
        return false;
    }
    if (av_frame_make_writable(f) < 0) {
        *err = "av_frame_make_writable failed";
        return false;
    }
    float* l = reinterpret_cast<float*>(f->data[0]);
    float* r = reinterpret_cast<float*>(f->data[1]);
    for (int i = 0; i < frames; ++i) {
        l[i] = interleaved[2 * i];
        r[i] = interleaved[2 * i + 1];
    }

    int ret = avcodec_send_frame(c, f);
    if (ret < 0) {
        *err = "avcodec_send_frame(aac) failed";
        return false;
    }
    while (true) {
        ret = avcodec_receive_packet(c, p);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            *err = "avcodec_receive_packet(aac) failed";
            return false;
        }
        cb(p->data, (size_t)p->size, pts_);
        pts_ += frameSize_;
        av_packet_unref(p);
    }
    return true;
}

void AacEncoder::Flush(const PacketCallback& cb) {
    auto* c = static_cast<AVCodecContext*>(ctx_);
    auto* p = static_cast<AVPacket*>(pkt_);
    if (!c || !p) return;
    if (avcodec_send_frame(c, nullptr) < 0) return;
    while (true) {
        int ret = avcodec_receive_packet(c, p);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;
        cb(p->data, (size_t)p->size, pts_);
        pts_ += frameSize_;
        av_packet_unref(p);
    }
}

void AacEncoder::Shutdown() {
    if (ctx_) {
        avcodec_free_context(reinterpret_cast<AVCodecContext**>(&ctx_));
        ctx_ = nullptr;
    }
    if (frame_) {
        av_frame_free(reinterpret_cast<AVFrame**>(&frame_));
        frame_ = nullptr;
    }
    if (pkt_) {
        av_packet_free(reinterpret_cast<AVPacket**>(&pkt_));
        pkt_ = nullptr;
    }
}
