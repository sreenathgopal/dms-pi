#include <dms/camera_service.h>
#include <dms/config.h>
#include <dms/messages.h>
#include <dms/face_detector.h>
#include <dms/landmark_tracker.h>
#include <dms/classifier.h>
#include <dms/state_machine.h>

#include <opencv2/videoio.hpp>
#include <opencv2/imgcodecs.hpp>
#include <iostream>
#include <chrono>
#include <thread>
#include <cmath>

namespace dms {

// Forward declarations for message factory functions
DetectionStatusMsg make_detection_status(
    const std::string& status, float ear_l, float ear_r, float mar,
    float speed, const std::string& buzz, const std::string& gaze);
DetectionImageMsg make_detection_image(
    const std::vector<uint8_t>& jpeg, const std::string& status);

void camera_service(zmq::context_t& ctx, std::atomic<bool>& shutdown) {
    const auto& cfg = config();

    // --- Face detector (YuNet) ---
    FaceDetector face_det(cfg.yunet_model_path, cfg.frame_w, cfg.frame_h,
                          cfg.face_detect_confidence);

    // --- Landmark tracker (TFLite face_landmark) ---
    LandmarkTracker tracker(cfg.landmark_model_path, 0.2f);

    // --- Drowsiness classifier (TFLite INT8) ---
    std::unique_ptr<DrowsinessClassifier> classifier;
    if (cfg.use_tflite) {
        classifier = std::make_unique<DrowsinessClassifier>(cfg.tflite_model_path);
        if (!classifier->is_loaded()) {
            std::cout << "[camera] TFLite classifier not available, using threshold fallback" << std::endl;
            classifier.reset();
        } else {
            std::cout << "[camera] TFLite INT8 classifier enabled" << std::endl;
        }
    }

    // --- State machine ---
    DrowsinessStateMachine fsm;

    // --- ZeroMQ publisher (detection events + images) ---
    zmq::socket_t pub(ctx, zmq::socket_type::pub);
    pub.set(zmq::sockopt::sndhwm, 64);
    pub.set(zmq::sockopt::linger, 1000);
    pub.bind(cfg.zmq_detection_endpoint);

    // --- ZeroMQ subscriber (GPS data for speed gating) ---
    zmq::socket_t gps_sub(ctx, zmq::socket_type::sub);
    gps_sub.set(zmq::sockopt::subscribe, "gps.");
    gps_sub.set(zmq::sockopt::rcvtimeo, 0);  // non-blocking
    gps_sub.set(zmq::sockopt::linger, 0);
    gps_sub.connect(cfg.zmq_gps_endpoint);

    // Brief pause for ZMQ socket setup
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // --- Camera setup ---
    std::cout << "[camera] Opening camera " << cfg.cam_id
              << " (" << cfg.frame_w << "x" << cfg.frame_h
              << " @ " << cfg.fps << " fps)" << std::endl;

    cv::VideoCapture cap(cfg.cam_id);
    cap.set(cv::CAP_PROP_FRAME_WIDTH, cfg.frame_w);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, cfg.frame_h);
    cap.set(cv::CAP_PROP_FPS, cfg.fps);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    if (!cap.isOpened()) {
        std::cerr << "[camera] Cannot open camera " << cfg.cam_id << " — exiting" << std::endl;
        pub.close();
        gps_sub.close();
        return;
    }

    float speed = 0.0f;
    int frame_count = 0;
    int fps_counter = 0;
    auto fps_time = std::chrono::steady_clock::now();

    std::vector<int> encode_params = {cv::IMWRITE_JPEG_QUALITY, cfg.image_quality};
    cv::Mat frame;

    std::cout << "[camera] Camera service running" << std::endl;

    while (!shutdown.load()) {
        if (!cap.read(frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        frame_count++;
        if (frame_count % (cfg.skip_frames + 1) != 0) continue;

        // --- Poll GPS (drain all pending, keep latest) ---
        {
            zmq::message_t topic_msg, payload_msg;
            while (gps_sub.recv(topic_msg, zmq::recv_flags::dontwait)) {
                gps_sub.recv(payload_msg, zmq::recv_flags::none);
                auto gps = unpack<GpsPositionMsg>(payload_msg.data(), payload_msg.size());
                speed = gps.speed;
            }
        }

        // --- Speed threshold gating ---
        if (cfg.speed_threshold > 0 && speed < cfg.speed_threshold) {
            auto msg = make_detection_status("idle", 0, 0, 0, speed, "", "");
            auto packed = pack(msg);
            pub.send(zmq::buffer("detection.status"), zmq::send_flags::sndmore);
            pub.send(zmq::buffer(packed), zmq::send_flags::none);
            continue;
        }

        // --- Face detection (YuNet) ---
        FaceBox face;
        bool face_found = face_det.detect(frame, face);

        float ear_l = 0, ear_r = 0, mar_val = 0;
        std::string gaze_dir;
        float eye_prob = 0, yawn_prob_val = 0;

        if (face_found) {
            // --- Landmark tracking ---
            auto lm_result = tracker.process(frame, face);

            if (lm_result.face_detected) {
                ear_l = lm_result.ear_left;
                ear_r = lm_result.ear_right;
                mar_val = lm_result.mar;
                gaze_dir = lm_result.gaze;

                // --- Classification ---
                if (classifier) {
                    auto [ep, yp] = classifier->predict(ear_l, ear_r, mar_val);
                    eye_prob = ep;
                    yawn_prob_val = yp;
                } else {
                    // Threshold-based fallback
                    bool both_closed = (ear_l < cfg.eye_closed_ratio && ear_r < cfg.eye_closed_ratio);
                    eye_prob = both_closed ? 1.0f : 0.0f;
                    yawn_prob_val = (mar_val > cfg.mouth_open_ratio) ? 1.0f : 0.0f;
                }
            } else {
                face_found = false;  // landmark detection failed
            }
        }

        // --- State machine ---
        auto [status, buzz, is_new] = fsm.update(face_found, eye_prob, yawn_prob_val, speed);

        // --- Publish status event ---
        {
            auto msg = make_detection_status(
                status,
                face_found ? ear_l : 0.0f,
                face_found ? ear_r : 0.0f,
                face_found ? mar_val : 0.0f,
                speed, buzz,
                face_found ? gaze_dir : ""
            );
            auto packed = pack(msg);
            pub.send(zmq::buffer("detection.status"), zmq::send_flags::sndmore);
            pub.send(zmq::buffer(packed), zmq::send_flags::none);
        }

        // --- Publish image on new alerts ---
        if (is_new && (status == "sleeping" || status == "yawning")) {
            std::vector<uint8_t> jpeg_buf;
            if (cv::imencode(".jpg", frame, jpeg_buf, encode_params)) {
                auto img_msg = make_detection_image(jpeg_buf, status);
                auto packed = pack(img_msg);
                pub.send(zmq::buffer("detection.image"), zmq::send_flags::sndmore);
                pub.send(zmq::buffer(packed), zmq::send_flags::none);
            }
        }

        // --- FPS tracking ---
        fps_counter++;
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - fps_time).count();
        if (elapsed >= 10.0) {
            std::cout << "[camera] Detection FPS: " << (fps_counter / elapsed)
                      << " | state=" << status << " | speed=" << speed << std::endl;
            fps_counter = 0;
            fps_time = now;
        }
    }

    cap.release();
    pub.close();
    gps_sub.close();
    std::cout << "[camera] Camera service stopped" << std::endl;
}

} // namespace dms
