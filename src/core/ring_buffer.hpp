#pragma once
#include <atomic>
#include <cstring>
#include <vector>

// SPSC lock-free float ring buffer (interleaved channels).
// One writer thread (audio capture), one reader thread (mixer pump).
class FloatRing {
public:
    void Reset(size_t capacityFrames, int channels) {
        cap_ = capacityFrames;
        ch_ = channels;
        buf_.assign(cap_ * (size_t)ch_, 0.0f);
        write_.store(0, std::memory_order_relaxed);
        read_.store(0, std::memory_order_relaxed);
    }

    size_t AvailableFrames() const {
        size_t w = write_.load(std::memory_order_acquire);
        size_t r = read_.load(std::memory_order_acquire);
        return (w >= r) ? (w - r) : (cap_ - r + w);
    }

    // Returns the number of frames actually written (drops the tail if full).
    size_t Write(const float* src, size_t frames) {
        size_t w = write_.load(std::memory_order_relaxed);
        size_t r = read_.load(std::memory_order_acquire);
        size_t freeFrames = (w >= r) ? (cap_ - (w - r) - 1) : (r - w - 1);
        size_t n = frames < freeFrames ? frames : freeFrames;
        for (size_t i = 0; i < n; ++i) {
            size_t idx = (w + i) % cap_;
            std::memcpy(&buf_[idx * (size_t)ch_], &src[i * (size_t)ch_], sizeof(float) * (size_t)ch_);
        }
        write_.store((w + n) % cap_, std::memory_order_release);
        return n;
    }

    // Returns the number of frames actually read.
    size_t Read(float* dst, size_t frames) {
        size_t r = read_.load(std::memory_order_relaxed);
        size_t w = write_.load(std::memory_order_acquire);
        size_t avail = (w >= r) ? (w - r) : (cap_ - r + w);
        size_t n = frames < avail ? frames : avail;
        for (size_t i = 0; i < n; ++i) {
            size_t idx = (r + i) % cap_;
            std::memcpy(&dst[i * (size_t)ch_], &buf_[idx * (size_t)ch_], sizeof(float) * (size_t)ch_);
        }
        read_.store((r + n) % cap_, std::memory_order_release);
        return n;
    }

    size_t DropAll() {
        size_t r = read_.load(std::memory_order_relaxed);
        size_t w = write_.load(std::memory_order_acquire);
        size_t avail = (w >= r) ? (w - r) : (cap_ - r + w);
        read_.store(w, std::memory_order_release);
        return avail;
    }

private:
    std::vector<float> buf_;
    size_t cap_ = 0;
    int ch_ = 2;
    std::atomic<size_t> write_{0};
    std::atomic<size_t> read_{0};
};
