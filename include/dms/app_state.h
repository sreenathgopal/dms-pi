#pragma once

#include <opencv2/core.hpp>
#include <mutex>
#include <condition_variable>
#include <string>
#include <atomic>
#include <ctime>

namespace dms {

// Single-slot frame handoff: capture → detection thread.
// If detection is slower than capture, frames are dropped (correct behavior).
struct FrameSlot {
    std::mutex mtx;
    std::condition_variable cv;
    cv::Mat frame;
    bool ready = false;

    void push(const cv::Mat& f) {
        std::lock_guard<std::mutex> lock(mtx);
        frame = f.clone();
        ready = true;
        cv.notify_one();
    }

    // Wait for a new frame. Returns false if shutdown requested.
    bool wait(cv::Mat& out, std::atomic<bool>& shutdown) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::milliseconds(500),
                     [&] { return ready || shutdown.load(); });
        if (shutdown.load()) return false;
        if (!ready) return false;
        out = frame;
        ready = false;
        return true;
    }
};

// Shared application state: detection thread writes, web server reads.
struct AppState {
    mutable std::mutex mtx;

    std::string detection_status = "starting";
    float ear_l = 0, ear_r = 0, mar = 0;
    float detection_fps = 0;
    int total_frames = 0;
    int sleep_alerts = 0;
    int yawn_alerts = 0;
    time_t start_time = 0;

    void update(const std::string& status, float el, float er, float m,
                float fps, int sleeps, int yawns) {
        std::lock_guard<std::mutex> lock(mtx);
        detection_status = status;
        ear_l = el;
        ear_r = er;
        mar = m;
        detection_fps = fps;
        sleep_alerts = sleeps;
        yawn_alerts = yawns;
    }
};

} // namespace dms
