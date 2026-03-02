#pragma once

#include <string>
#include <array>

namespace dms {

struct Config {
    // Camera / capture
    int frame_w = 640;
    int frame_h = 480;
    int fps = 10;
    int skip_frames = 2;

    // Facial detection thresholds (fallback when TFLite unavailable)
    float eye_closed_ratio = 0.20f;
    float mouth_open_ratio = 0.8f;
    float gaze_left_ratio = 0.35f;
    float gaze_right_ratio = 0.65f;

    // Face detection
    float face_detect_confidence = 0.5f;

    // TFLite INT8 classifier
    bool use_tflite = true;
    std::string tflite_model_path = "/opt/dms/models/drowsiness_int8.tflite";
    std::string landmark_model_path = "/opt/dms/models/face_landmark.tflite";
    std::string yunet_model_path = "/opt/dms/models/face_detection_yunet.onnx";
    float eye_closed_prob = 0.6f;
    float yawn_prob = 0.6f;

    // State machine (time-windowed detection)
    float eyes_closed_duration = 2.0f;
    float sleep_alert_cooldown = 5.0f;
    float yawn_alert_cooldown = 10.0f;
    float warning_window = 300.0f;
    float no_face_timeout = 3.0f;

    // Landmark indices (MediaPipe FaceMesh 468+10 iris)
    static constexpr std::array<int, 6> LEFT_EYE  = {33, 160, 158, 133, 153, 144};
    static constexpr std::array<int, 6> RIGHT_EYE = {362, 385, 387, 263, 373, 380};
    static constexpr std::array<int, 4> MOUTH     = {13, 14, 78, 308};
    static constexpr std::array<int, 5> LEFT_IRIS  = {468, 469, 470, 471, 472};
    static constexpr std::array<int, 5> RIGHT_IRIS = {473, 474, 475, 476, 477};

    // GPIO (BCM numbering)
    int buzzer_pin_1 = 17;
    int buzzer_pin_2 = 22;
    int led_pin = 4;
    float buzzer_duration = 0.12f;

    // Ring buffer
    int ring_buffer_seconds = 90;

    // Recording / alerts
    std::string recordings_dir = "/home/test/recordings";
    std::string alerts_dir = "alerts";
    int alert_pre_seconds = 10;
    int alert_post_seconds = 90;

    // Web server
    int web_port = 8080;

    // Logging
    std::string log_level = "INFO";
};

// Load config from JSON file + environment variable overrides
void load_config(Config& cfg, const std::string& path = "/boot/dms_config.json");

// Global config instance
Config& config();

} // namespace dms
