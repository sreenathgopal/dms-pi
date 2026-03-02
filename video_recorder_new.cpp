/*
 * video_recorder.cpp
 * ==================
 * Lightweight frame publisher + alert clip recorder for Raspberry Pi.
 *
 * TWO MODES:
 *   --stdin     Read MJPEG from stdin (piped from rpicam-vid)
 *   (default)   Open camera directly via OpenCV
 *
 * PIPELINE (recommended for Pi):
 *   rpicam-vid -t 0 --width 640 --height 480 --framerate 10 \
 *     --codec mjpeg --nopreview -o - 2>/dev/null \
 *     | ./video_recorder --stdin --headless
 *
 * FEATURES:
 *   - ZeroMQ: sends frames to Python DMS (port 5556)
 *   - ZeroMQ: receives alerts from DMS (port 5555)
 *   - Ring buffer holds last 90s of JPEG frames in RAM (~27MB)
 *   - On alert: saves ring buffer + 90s post-alert as AVI clip
 *   - Auto disk cleanup (deletes oldest when space is low)
 */

#include <opencv2/opencv.hpp>
#include <zmq.h>
#include <zmq.hpp>
#include <iostream>
#include <string>
#include <ctime>
#include <csignal>
#include <sstream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <cstdio>

#define MKDIR(dir) mkdir(dir, 0755)

/* ── Configuration ──────────────────────────────────────── */
#define DEFAULT_CAMERA      0
#define VIDEO_FPS           10
#define FRAME_SEND_WIDTH    640
#define FRAME_SEND_HEIGHT   480

/* Disk management */
#define MIN_FREE_SPACE_MB   500
#define CLEANUP_CHECK_SECS  30

/* Ring buffer: 90 seconds of pre-alert footage */
#define RING_BUFFER_SECS    90

/* Post-alert recording duration */
#define POST_ALERT_SECS     90

/* Alert clip output folder */
#define ALERT_DIR           "alerts"

/* ZeroMQ */
#define ZMQ_ALERT_ENDPOINT  "tcp://127.0.0.1:5555"
#define ZMQ_FRAME_ENDPOINT  "tcp://127.0.0.1:5556"

/* ── Globals ────────────────────────────────────────────── */
static volatile bool running = true;

void signal_handler(int sig) {
    (void)sig;
    std::cout << "\n[INFO] Stopping..." << std::endl;
    running = false;
}

/* ── Ring Buffer (stores raw JPEG bytes, zero CPU) ──────── */
struct BufferedFrame {
    std::vector<uchar> jpeg_data;
    time_t timestamp;
};

class RingBuffer {
public:
    RingBuffer(int max_frames) : max_frames_(max_frames) {}

    void push(const std::vector<uchar> &jpeg, time_t ts) {
        std::lock_guard<std::mutex> lock(mtx_);
        buffer_.push_back({jpeg, ts});
        while ((int)buffer_.size() > max_frames_)
            buffer_.pop_front();
    }

    std::deque<BufferedFrame> snapshot() {
        std::lock_guard<std::mutex> lock(mtx_);
        return buffer_;
    }

    int size() {
        std::lock_guard<std::mutex> lock(mtx_);
        return (int)buffer_.size();
    }

    long ram_usage_mb() {
        std::lock_guard<std::mutex> lock(mtx_);
        long total = 0;
        for (auto &bf : buffer_)
            total += (long)bf.jpeg_data.size();
        return total / (1024 * 1024);
    }

private:
    std::deque<BufferedFrame> buffer_;
    std::mutex mtx_;
    int max_frames_;
};

/* ── Disk Space Management ──────────────────────────────── */
long get_free_space_mb(const std::string &path) {
    struct statvfs stat;
    if (statvfs(path.c_str(), &stat) == 0)
        return (long)((stat.f_bavail * stat.f_frsize) / (1024 * 1024));
    return -1;
}

bool delete_oldest_file(const std::string &dir) {
    std::vector<std::string> files;
    DIR *d = opendir(dir.c_str());
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            std::string name = entry->d_name;
            if (name.size() > 4 && name.substr(0, 6) == "alert_" &&
                name.substr(name.size() - 4) == ".avi")
                files.push_back(dir + "/" + name);
        }
        closedir(d);
    }
    if (files.size() <= 1) return false;
    std::sort(files.begin(), files.end());
    if (std::remove(files[0].c_str()) == 0) {
        std::cout << "[CLEANUP] Deleted: " << files[0] << "\n";
        return true;
    }
    return false;
}

void check_and_cleanup(const std::string &dir) {
    long free_mb = get_free_space_mb(dir);
    if (free_mb < 0) return;
    while (free_mb < MIN_FREE_SPACE_MB) {
        std::cout << "[CLEANUP] Low space: " << free_mb << " MB\n";
        if (!delete_oldest_file(dir)) break;
        free_mb = get_free_space_mb(dir);
        if (free_mb < 0) break;
    }
}

/* ── Filename Generator ─────────────────────────────────── */
std::string generate_filename(const std::string &dir, const std::string &prefix) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    std::ostringstream ss;
    ss << dir << "/" << prefix
       << std::setfill('0')
       << std::setw(4) << (t->tm_year + 1900) << "-"
       << std::setw(2) << (t->tm_mon + 1) << "-"
       << std::setw(2) << t->tm_mday << "_"
       << std::setw(2) << t->tm_hour << "-"
       << std::setw(2) << t->tm_min << "-"
       << std::setw(2) << t->tm_sec << ".avi";
    return ss.str();
}

/* ── Alert State ────────────────────────────────────────── */
std::atomic<bool> alert_triggered(false);
std::atomic<bool> alert_saving(false);
std::atomic<bool> alert_recording(false);
std::atomic<int>  post_alert_countdown(0);
std::mutex post_frames_mtx;
std::vector<BufferedFrame> post_frames;
std::mutex alert_type_mtx;
std::string alert_type_received;

/* ── Alert Clip Writer Thread ───────────────────────────── */
void save_alert_clip(std::string alert_type,
                     std::deque<BufferedFrame> pre_frames,
                     int fps) {
    std::string filename = generate_filename(ALERT_DIR, "alert_" + alert_type + "_");
    std::cout << "[ALERT] Saving: " << filename << "\n";
    check_and_cleanup(ALERT_DIR);

    if (pre_frames.empty()) { alert_saving = false; return; }

    cv::Mat first = cv::imdecode(pre_frames.front().jpeg_data, cv::IMREAD_COLOR);
    if (first.empty()) { alert_saving = false; return; }

    int fourcc = cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
    cv::VideoWriter wr(filename, fourcc, fps, first.size());
    if (!wr.isOpened()) {
        std::cerr << "[ALERT] Failed to create file!\n";
        alert_saving = false;
        return;
    }

    int pre_count = 0;
    for (auto &bf : pre_frames) {
        cv::Mat decoded = cv::imdecode(bf.jpeg_data, cv::IMREAD_COLOR);
        if (!decoded.empty()) { wr.write(decoded); pre_count++; }
    }
    std::cout << "[ALERT] Pre-alert: " << pre_count << " frames\n";

    while (alert_recording && running)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    {
        std::lock_guard<std::mutex> lock(post_frames_mtx);
        int post_count = 0;
        for (auto &bf : post_frames) {
            cv::Mat decoded = cv::imdecode(bf.jpeg_data, cv::IMREAD_COLOR);
            if (!decoded.empty()) { wr.write(decoded); post_count++; }
        }
        std::cout << "[ALERT] Post-alert: " << post_count << " frames\n";
        post_frames.clear();
    }

    wr.release();
    alert_saving = false;
    std::cout << "[ALERT] Saved: " << filename << "\n";
}

/* ── ZeroMQ Listener Thread ─────────────────────────────── */
void zmq_listener_thread() {
    try {
        zmq::context_t context(1);
        zmq::socket_t subscriber(context, ZMQ_SUB);
        subscriber.connect(ZMQ_ALERT_ENDPOINT);
        subscriber.setsockopt(ZMQ_SUBSCRIBE, "", 0);
        int timeout_ms = 500;
        subscriber.setsockopt(ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
        std::cout << "[ZMQ] Alert listener on " << ZMQ_ALERT_ENDPOINT << "\n";

        while (running) {
            zmq::message_t msg;
            bool received = subscriber.recv(&msg, ZMQ_DONTWAIT);
            if (!received) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (msg.size() > 0) {
                std::string message(static_cast<char*>(msg.data()), msg.size());
                std::cout << "[ZMQ] Received: " << message << "\n";
                if (message.substr(0, 5) == "ALERT") {
                    if (!alert_saving && !alert_recording) {
                        std::lock_guard<std::mutex> lock(alert_type_mtx);
                        alert_type_received = message;
                        alert_triggered = true;
                    } else {
                        std::cout << "[ZMQ] Ignored (already saving).\n";
                    }
                } else if (message == "STOP") {
                    running = false;
                }
            }
        }
        subscriber.close();
        context.close();
    } catch (const zmq::error_t &e) {
        std::cerr << "[ZMQ] Error: " << e.what() << "\n";
    }
}

/* ── MJPEG stdin reader ─────────────────────────────────── */
bool read_mjpeg_frame(std::vector<uchar> &jpeg_out) {
    jpeg_out.clear();

    /* Scan for SOI marker (0xFF 0xD8) */
    int prev = -1, curr;
    while ((curr = getchar_unlocked()) != EOF) {
        if (prev == 0xFF && curr == 0xD8) {
            jpeg_out.push_back(0xFF);
            jpeg_out.push_back(0xD8);
            break;
        }
        prev = curr;
    }
    if (curr == EOF) return false;

    /* Read until EOI marker (0xFF 0xD9) */
    prev = -1;
    while ((curr = getchar_unlocked()) != EOF) {
        jpeg_out.push_back((uchar)curr);
        if (prev == 0xFF && curr == 0xD9) {
            return true;
        }
        prev = curr;
    }
    return false;
}

/* ── Main ───────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    int camera_id = DEFAULT_CAMERA;
    int video_fps = VIDEO_FPS;
    bool headless = false;
    bool stdin_mode = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n\n"
                      << "Options:\n"
                      << "  --stdin          Read MJPEG from stdin (pipe from rpicam-vid)\n"
                      << "  --camera ID      Camera index for direct mode (default: 0)\n"
                      << "  --fps FPS        Frames per second (default: 10)\n"
                      << "  --headless       No preview window\n\n"
                      << "Pipeline mode:\n"
                      << "  rpicam-vid -t 0 --width 640 --height 480 --framerate 10 \\\n"
                      << "    --codec mjpeg --nopreview -o - 2>/dev/null \\\n"
                      << "    | ./video_recorder --stdin --headless\n";
            return 0;
        }
        else if (arg == "--stdin") { stdin_mode = true; }
        else if (arg == "--camera" && i + 1 < argc) { camera_id = std::atoi(argv[++i]); }
        else if (arg == "--fps" && i + 1 < argc) { video_fps = std::atoi(argv[++i]); }
        else if (arg == "--headless") { headless = true; }
    }

    if (video_fps <= 0) video_fps = VIDEO_FPS;
    int ring_buffer_frames = RING_BUFFER_SECS * video_fps;
    int post_alert_frames = POST_ALERT_SECS * video_fps;

    MKDIR(ALERT_DIR);

    std::cout << "==========================================\n"
              << "  VIDEO RECORDER + ALERT SYSTEM\n"
              << "==========================================\n"
              << "  Mode:         " << (stdin_mode ? "STDIN (rpicam-vid pipe)" : "Direct camera") << "\n"
              << "  FPS:          " << video_fps << "\n"
              << "  Ring buffer:  " << RING_BUFFER_SECS << "s ("
              << ring_buffer_frames << " frames)\n"
              << "  Alert clip:   " << (RING_BUFFER_SECS + POST_ALERT_SECS) << "s\n"
              << "  ZMQ alerts:   " << ZMQ_ALERT_ENDPOINT << "\n"
              << "  ZMQ frames:   " << ZMQ_FRAME_ENDPOINT << "\n"
              << "==========================================\n\n";

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    /* Start ZMQ listener */
    std::thread zmq_thread(zmq_listener_thread);
    zmq_thread.detach();

    /* Frame publisher socket */
    zmq::context_t frame_ctx(1);
    zmq::socket_t frame_pub(frame_ctx, ZMQ_PUB);
    try {
        frame_pub.bind(ZMQ_FRAME_ENDPOINT);
        std::cout << "[ZMQ] Frame publisher on " << ZMQ_FRAME_ENDPOINT << "\n";
    } catch (const zmq::error_t &e) {
        std::cerr << "[ZMQ] Frame publisher error: " << e.what() << "\n";
    }

    /* Open camera (direct mode only) */
    cv::VideoCapture cap;
    if (!stdin_mode) {
        std::cout << "[INFO] Opening camera " << camera_id << "...\n";
        int backends[] = { cv::CAP_V4L2, cv::CAP_ANY };
        const char *backend_names[] = {"V4L2", "Auto"};
        for (int i = 0; i < 2; i++) {
            std::cout << "[INFO] Trying " << backend_names[i] << "...\n";
            cap.open(camera_id, backends[i]);
            if (cap.isOpened()) {
                std::cout << "[INFO] Opened with " << backend_names[i] << "\n";
                break;
            }
        }
        if (!cap.isOpened()) {
            std::cerr << "[ERROR] Cannot open camera!\n";
            return 1;
        }
        cap.set(cv::CAP_PROP_FRAME_WIDTH, FRAME_SEND_WIDTH);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, FRAME_SEND_HEIGHT);
        cap.set(cv::CAP_PROP_FPS, video_fps);
        cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    } else {
        std::cout << "[INFO] Reading MJPEG from stdin...\n";
    }

    /* Ring buffer */
    RingBuffer ring_buffer(ring_buffer_frames);

    long total_frames = 0;
    time_t last_cleanup_check = 0;

    cv::Mat frame;
    std::vector<uchar> jpeg_buf;

    std::cout << "[INFO] Running!\n\n";

    while (running) {
        time_t now = time(NULL);

        if (stdin_mode) {
            /* Read one JPEG frame from rpicam-vid pipe */
            if (!read_mjpeg_frame(jpeg_buf)) {
                std::cerr << "[INFO] stdin EOF\n";
                break;
            }
        } else {
            /* Direct camera capture */
            if (!cap.read(frame) || frame.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            cv::flip(frame, frame, 1);
            std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 85};
            if (!cv::imencode(".jpg", frame, jpeg_buf, params)) continue;
        }

        total_frames++;

        /* Store in ring buffer (raw JPEG bytes, zero CPU) */
        ring_buffer.push(jpeg_buf, now);

        /* Capture post-alert frames */
        if (alert_recording) {
            std::lock_guard<std::mutex> lock(post_frames_mtx);
            post_frames.push_back({jpeg_buf, now});
            int rem = post_alert_countdown.fetch_sub(1);
            if (rem <= 1) {
                alert_recording = false;
                std::cout << "[ALERT] Post-alert complete.\n";
            }
        }

        /* Handle alert trigger */
        if (alert_triggered && !alert_saving && !alert_recording) {
            alert_triggered = false;
            alert_saving = true;
            alert_recording = true;
            post_alert_countdown = post_alert_frames;

            { std::lock_guard<std::mutex> lock(post_frames_mtx);
              post_frames.clear(); }

            std::string atype;
            { std::lock_guard<std::mutex> lock(alert_type_mtx);
              atype = alert_type_received; }

            auto pre = ring_buffer.snapshot();
            std::thread(save_alert_clip, atype, std::move(pre), video_fps).detach();

            std::cout << "[ALERT] Triggered! Saving "
                      << RING_BUFFER_SECS << "s + "
                      << POST_ALERT_SECS << "s clip.\n";
        }

        /* Send frame to DMS via ZMQ */
        {
            int32_t sw = FRAME_SEND_WIDTH;
            int32_t sh = FRAME_SEND_HEIGHT;

            size_t total_size = 8 + jpeg_buf.size();
            zmq::message_t zmsg(total_size);
            char *ptr = (char*)zmsg.data();
            memcpy(ptr, &sw, 4);
            memcpy(ptr + 4, &sh, 4);
            memcpy(ptr + 8, jpeg_buf.data(), jpeg_buf.size());
            frame_pub.send(zmsg, ZMQ_DONTWAIT);
        }

        /* Periodic cleanup */
        if (now - last_cleanup_check >= CLEANUP_CHECK_SECS) {
            check_and_cleanup(ALERT_DIR);
            last_cleanup_check = now;
        }

        if (!stdin_mode) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::cout << "\n[INFO] Done. Total frames: " << total_frames << "\n";
    if (cap.isOpened()) cap.release();
    frame_pub.close();
    frame_ctx.close();

    return 0;
}
