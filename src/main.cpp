#include <dms/config.h>
#include <dms/ring_buffer.h>
#include <dms/app_state.h>
#include <dms/face_detector.h>
#include <dms/landmark_tracker.h>
#include <dms/classifier.h>
#include <dms/state_machine.h>
#include <dms/alert_service.h>
#include <dms/management_service.h>

#include <opencv2/imgcodecs.hpp>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <csignal>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

static std::atomic<bool> g_shutdown{false};

static void signal_handler(int sig) {
    std::cout << "\n[main] Shutdown signal received (" << sig << ")" << std::endl;
    g_shutdown.store(true);
}

// --- Read one MJPEG frame from stdin ---
// MJPEG frames are delimited by SOI (FF D8) and EOI (FF D9) markers.
static bool read_mjpeg_frame(std::vector<uint8_t>& buf) {
    buf.clear();

    // Find SOI marker
    int prev = -1;
    while (true) {
        int c = std::getchar();
        if (c == EOF) return false;
        if (prev == 0xFF && c == 0xD8) {
            buf.push_back(0xFF);
            buf.push_back(0xD8);
            break;
        }
        prev = c;
    }

    // Read until EOI marker
    prev = -1;
    while (true) {
        int c = std::getchar();
        if (c == EOF) return false;
        buf.push_back(static_cast<uint8_t>(c));
        if (prev == 0xFF && c == 0xD9) {
            return true;
        }
        prev = c;
    }
}

// --- Alert clip saver (runs as detached thread) ---
static void save_alert_clip(std::vector<dms::JpegFrame> pre_frames,
                             dms::RingBuffer* ring_buf,
                             const std::string& alerts_dir,
                             int post_seconds, int fps)
{
    // Generate filename
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char fname[128];
    snprintf(fname, sizeof(fname), "%s/alert_%04d%02d%02d_%02d%02d%02d.avi",
             alerts_dir.c_str(),
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

    std::cout << "[alert] Saving clip: " << fname << " (pre=" << pre_frames.size()
              << " frames, post=" << post_seconds << "s)" << std::endl;

    // Decode first frame to get dimensions
    if (pre_frames.empty()) return;
    cv::Mat sample = cv::imdecode(pre_frames[0].data, cv::IMREAD_COLOR);
    if (sample.empty()) return;

    cv::VideoWriter writer(fname, cv::VideoWriter::fourcc('M','J','P','G'),
                            fps, sample.size());
    if (!writer.isOpened()) {
        std::cerr << "[alert] Failed to open VideoWriter: " << fname << std::endl;
        return;
    }

    // Write pre-alert frames
    for (auto& f : pre_frames) {
        cv::Mat decoded = cv::imdecode(f.data, cv::IMREAD_COLOR);
        if (!decoded.empty()) writer.write(decoded);
    }

    // Collect post-alert frames
    time_t end_time = time(nullptr) + post_seconds;
    while (time(nullptr) < end_time) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // Get latest frames from ring buffer
        auto recent = ring_buf->snapshot(1);
        for (auto& f : recent) {
            cv::Mat decoded = cv::imdecode(f.data, cv::IMREAD_COLOR);
            if (!decoded.empty()) writer.write(decoded);
        }
    }

    writer.release();
    std::cout << "[alert] Clip saved: " << fname << std::endl;
}

// --- Detection thread ---
static void detection_thread(dms::FrameSlot& slot, dms::RingBuffer& ring_buf,
                              dms::AppState& state, std::atomic<bool>& shutdown)
{
    const auto& cfg = dms::config();

    // Init detection pipeline
    dms::FaceDetector face_det(cfg.yunet_model_path, cfg.frame_w, cfg.frame_h,
                                cfg.face_detect_confidence);

    dms::LandmarkTracker tracker(cfg.landmark_model_path, 0.2f);

    std::unique_ptr<dms::DrowsinessClassifier> classifier;
    if (cfg.use_tflite) {
        classifier = std::make_unique<dms::DrowsinessClassifier>(cfg.tflite_model_path);
        if (!classifier->is_loaded()) {
            std::cout << "[detect] TFLite classifier not available, using threshold fallback" << std::endl;
            classifier.reset();
        } else {
            std::cout << "[detect] TFLite INT8 classifier enabled" << std::endl;
        }
    }

    dms::DrowsinessStateMachine fsm;
    std::string last_led_status;

    int fps_counter = 0;
    auto fps_time = std::chrono::steady_clock::now();
    float current_fps = 0;

    std::cout << "[detect] Detection thread running" << std::endl;

    while (!shutdown.load()) {
        cv::Mat frame;
        if (!slot.wait(frame, shutdown)) continue;

        // Face detection (YuNet)
        dms::FaceBox face;
        bool face_found = face_det.detect(frame, face);

        float ear_l = 0, ear_r = 0, mar_val = 0;
        float eye_prob = 0, yawn_prob_val = 0;

        if (face_found) {
            auto lm_result = tracker.process(frame, face);

            if (lm_result.face_detected) {
                ear_l = lm_result.ear_left;
                ear_r = lm_result.ear_right;
                mar_val = lm_result.mar;

                if (classifier) {
                    auto [ep, yp] = classifier->predict(ear_l, ear_r, mar_val);
                    eye_prob = ep;
                    yawn_prob_val = yp;
                } else {
                    bool both_closed = (ear_l < cfg.eye_closed_ratio && ear_r < cfg.eye_closed_ratio);
                    eye_prob = both_closed ? 1.0f : 0.0f;
                    yawn_prob_val = (mar_val > cfg.mouth_open_ratio) ? 1.0f : 0.0f;
                }
            } else {
                face_found = false;
            }
        }

        // State machine
        auto [status, buzz, is_new] = fsm.update(face_found, eye_prob, yawn_prob_val, 0);

        // GPIO inline
        if (buzz == "short") {
            dms::gpio_buzz(cfg.buzzer_duration);
        } else if (buzz == "long") {
            dms::gpio_buzz(cfg.buzzer_duration * 3);
        }

        // LED control
        bool is_alert = (status == "sleeping" || status == "yawning" || status == "no_face");
        bool was_alert = (last_led_status == "sleeping" || last_led_status == "yawning" || last_led_status == "no_face");
        if (is_alert && !was_alert) dms::gpio_led(true);
        else if (!is_alert && was_alert) dms::gpio_led(false);
        last_led_status = status;

        // Alert clip
        if (is_new && (status == "sleeping" || status == "yawning")) {
            auto pre_frames = ring_buf.snapshot(cfg.alert_pre_seconds);
            std::thread(save_alert_clip, std::move(pre_frames), &ring_buf,
                        cfg.alerts_dir, cfg.alert_post_seconds, cfg.fps).detach();
        }

        // FPS tracking
        fps_counter++;
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - fps_time).count();
        if (elapsed >= 10.0) {
            current_fps = static_cast<float>(fps_counter / elapsed);
            std::cout << "[detect] FPS: " << current_fps
                      << " | state=" << status << std::endl;
            fps_counter = 0;
            fps_time = now;
        }

        // Update shared state
        auto stats = fsm.get_stats();
        state.update(status,
                     face_found ? ear_l : 0.0f,
                     face_found ? ear_r : 0.0f,
                     face_found ? mar_val : 0.0f,
                     current_fps, stats.sleep_events_5min, stats.yawn_events_5min);
    }

    dms::gpio_led(false);
    std::cout << "[detect] Detection thread stopped" << std::endl;
}

// --- Print usage ---
static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  --stdin       Read MJPEG from stdin (pipe from rpicam-vid)\n"
              << "  --headless    No display (required on Pi)\n"
              << "  --fps N       Override FPS (default: from config)\n"
              << "  --port N      Override web port (default: 8080)\n"
              << "  --help        Show this help\n";
}

int main(int argc, char* argv[]) {
    bool use_stdin = false;
    bool headless = false;
    int override_fps = 0;
    int override_port = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stdin") == 0) use_stdin = true;
        else if (strcmp(argv[i], "--headless") == 0) headless = true;
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) override_fps = atoi(argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) override_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0) { print_usage(argv[0]); return 0; }
        else { std::cerr << "Unknown option: " << argv[i] << std::endl; return 1; }
    }

    (void)headless;  // always headless on Pi

    // Load configuration
    dms::load_config(dms::config());
    auto& cfg = dms::config();
    if (override_fps > 0) cfg.fps = override_fps;
    if (override_port > 0) cfg.web_port = override_port;

    std::cout << "=== DMS starting (C++ unified binary) ===" << std::endl;
    std::cout << "Config: " << cfg.frame_w << "x" << cfg.frame_h
              << " @" << cfg.fps << "fps, skip=" << cfg.skip_frames
              << ", tflite=" << (cfg.use_tflite ? "true" : "false")
              << ", port=" << cfg.web_port << std::endl;

    if (!use_stdin) {
        std::cerr << "[main] ERROR: --stdin required (pipe MJPEG from rpicam-vid)" << std::endl;
        return 1;
    }

    // Signal handling
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    // Ensure directories exist
    mkdir(cfg.alerts_dir.c_str(), 0755);
    mkdir(cfg.recordings_dir.c_str(), 0755);

    // Init GPIO
    dms::gpio_init();

    // Ring buffer: fps * ring_buffer_seconds frames
    size_t ring_capacity = static_cast<size_t>(cfg.fps * cfg.ring_buffer_seconds);
    dms::RingBuffer ring_buf(ring_capacity);

    // Shared state
    dms::AppState app_state;
    app_state.start_time = time(nullptr);

    // Frame slot (capture → detection)
    dms::FrameSlot frame_slot;

    // Start web server
    dms::start_web_server(cfg.web_port, app_state, ring_buf, g_shutdown);

    // Start detection thread
    std::thread det_thread(detection_thread, std::ref(frame_slot), std::ref(ring_buf),
                            std::ref(app_state), std::ref(g_shutdown));

    std::cout << "=== DMS running (capture + detection + web) ===" << std::endl;

    // Main thread = capture loop (read MJPEG from stdin)
    std::vector<uint8_t> jpeg_buf;
    int frame_count = 0;

    while (!g_shutdown.load()) {
        if (!read_mjpeg_frame(jpeg_buf)) {
            std::cerr << "[main] stdin EOF — shutting down" << std::endl;
            g_shutdown.store(true);
            break;
        }

        // Push raw JPEG to ring buffer (zero CPU — just a memcpy)
        ring_buf.push(jpeg_buf, time(nullptr));
        frame_count++;

        // Only decode every Nth frame for detection
        if (frame_count % (cfg.skip_frames + 1) != 0) continue;

        cv::Mat decoded = cv::imdecode(jpeg_buf, cv::IMREAD_COLOR);
        if (decoded.empty()) continue;

        frame_slot.push(decoded);
    }

    // Shutdown
    std::cout << "=== DMS shutting down ===" << std::endl;
    g_shutdown.store(true);

    // Wake up detection thread
    frame_slot.cv.notify_all();

    if (det_thread.joinable()) {
        det_thread.join();
        std::cout << "[main] Detection thread stopped" << std::endl;
    }

    dms::stop_web_server();
    dms::gpio_cleanup();

    std::cout << "=== DMS stopped ===" << std::endl;
    return 0;
}
