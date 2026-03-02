#pragma once

#include <vector>
#include <mutex>
#include <cstdint>
#include <ctime>

namespace dms {

struct JpegFrame {
    std::vector<uint8_t> data;
    time_t timestamp;
};

// Thread-safe circular buffer of raw JPEG frames.
// Push from capture thread, read from alert clip writer + web stream.
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) : capacity_(capacity) {
        frames_.reserve(capacity);
    }

    void push(const std::vector<uint8_t>& jpeg, time_t ts) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (frames_.size() < capacity_) {
            frames_.push_back({jpeg, ts});
        } else {
            frames_[head_] = {jpeg, ts};
        }
        head_ = (head_ + 1) % capacity_;
        total_frames_++;
        total_bytes_ += jpeg.size();
    }

    // Snapshot the last N seconds of frames (copies data)
    std::vector<JpegFrame> snapshot(int seconds) const {
        std::lock_guard<std::mutex> lock(mtx_);
        if (frames_.empty()) return {};

        time_t cutoff = time(nullptr) - seconds;
        std::vector<JpegFrame> result;
        size_t count = frames_.size();
        // Iterate oldest to newest
        size_t start = (count < capacity_) ? 0 : head_;
        for (size_t i = 0; i < count; i++) {
            size_t idx = (start + i) % capacity_;
            if (frames_[idx].timestamp >= cutoff) {
                result.push_back(frames_[idx]);
            }
        }
        return result;
    }

    // Get the latest frame (for MJPEG stream)
    bool latest(JpegFrame& out) const {
        std::lock_guard<std::mutex> lock(mtx_);
        if (frames_.empty()) return false;
        size_t idx = (head_ == 0) ? frames_.size() - 1 : head_ - 1;
        out = frames_[idx];
        return true;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return frames_.size();
    }

    long total_bytes() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return total_bytes_;
    }

    int total_frames() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return total_frames_;
    }

private:
    mutable std::mutex mtx_;
    size_t capacity_;
    size_t head_ = 0;
    std::vector<JpegFrame> frames_;
    long total_bytes_ = 0;
    int total_frames_ = 0;
};

} // namespace dms
