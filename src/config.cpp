#include <dms/config.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <cstdlib>
#include <iostream>

namespace dms {

using json = nlohmann::json;

static Config g_config;

Config& config() {
    return g_config;
}

// Helper: read env var with DMS_ prefix
static const char* env(const char* name) {
    std::string key = std::string("DMS_") + name;
    return std::getenv(key.c_str());
}

void load_config(Config& cfg, const std::string& path) {
    // Load JSON config file (try given path, then Bookworm /boot/firmware fallback)
    std::string config_path = path;
    std::ifstream f(config_path);
    if (!f.is_open() && config_path == "/boot/dms_config.json") {
        config_path = "/boot/firmware/dms_config.json";
        f.open(config_path);
    }
    if (f.is_open()) {
        try {
            json j = json::parse(f);
            auto get_int = [&](const char* k, int& v) {
                if (j.contains(k) && j[k].is_number_integer()) v = j[k].get<int>();
            };
            auto get_float = [&](const char* k, float& v) {
                if (j.contains(k) && j[k].is_number()) v = j[k].get<float>();
            };
            auto get_str = [&](const char* k, std::string& v) {
                if (j.contains(k) && j[k].is_string()) v = j[k].get<std::string>();
            };
            auto get_bool = [&](const char* k, bool& v) {
                if (j.contains(k) && j[k].is_boolean()) v = j[k].get<bool>();
            };

            get_int("cam_id", cfg.cam_id);
            get_int("frame_w", cfg.frame_w);
            get_int("frame_h", cfg.frame_h);
            get_int("fps", cfg.fps);
            get_int("skip_frames", cfg.skip_frames);
            get_float("eye_closed_ratio", cfg.eye_closed_ratio);
            get_float("mouth_open_ratio", cfg.mouth_open_ratio);
            get_float("gaze_left_ratio", cfg.gaze_left_ratio);
            get_float("gaze_right_ratio", cfg.gaze_right_ratio);
            get_float("face_detect_confidence", cfg.face_detect_confidence);
            get_bool("use_tflite", cfg.use_tflite);
            get_str("tflite_model_path", cfg.tflite_model_path);
            get_str("landmark_model_path", cfg.landmark_model_path);
            get_str("yunet_model_path", cfg.yunet_model_path);
            get_float("eye_closed_prob", cfg.eye_closed_prob);
            get_float("yawn_prob", cfg.yawn_prob);
            get_float("eyes_closed_duration", cfg.eyes_closed_duration);
            get_float("sleep_alert_cooldown", cfg.sleep_alert_cooldown);
            get_float("yawn_alert_cooldown", cfg.yawn_alert_cooldown);
            get_float("warning_window", cfg.warning_window);
            get_float("no_face_timeout", cfg.no_face_timeout);
            get_int("buzzer_pin_1", cfg.buzzer_pin_1);
            get_int("buzzer_pin_2", cfg.buzzer_pin_2);
            get_int("led_pin", cfg.led_pin);
            get_float("buzzer_duration", cfg.buzzer_duration);
            get_int("speed_threshold", cfg.speed_threshold);
            get_str("zmq_detection_endpoint", cfg.zmq_detection_endpoint);
            get_str("zmq_gps_endpoint", cfg.zmq_gps_endpoint);
            get_str("api_base_url", cfg.api_base_url);
            get_str("api_store_data", cfg.api_store_data);
            get_str("api_upload_file", cfg.api_upload_file);
            get_str("api_create_device", cfg.api_create_device);
            get_str("api_handshake", cfg.api_handshake);
            get_str("db_path", cfg.db_path);
            get_str("image_dir", cfg.image_dir);
            get_int("image_quality", cfg.image_quality);
            get_int("image_max_age", cfg.image_max_age);
            get_int("image_max_count", cfg.image_max_count);
            get_float("api_send_interval", cfg.api_send_interval);
            get_float("gps_store_interval", cfg.gps_store_interval);
            get_str("gps_port", cfg.gps_port);
            get_int("gps_baud", cfg.gps_baud);
            get_int("mgmt_port", cfg.mgmt_port);
            get_str("log_level", cfg.log_level);
        } catch (const std::exception& e) {
            std::cerr << "[config] Warning: failed to parse " << path << ": " << e.what() << std::endl;
        }
    }

    // Environment variable overrides (DMS_FRAME_W, DMS_USE_TFLITE, etc.)
    auto env_int = [](const char* name, int& v) {
        const char* e = env(name);
        if (e) v = std::atoi(e);
    };
    auto env_float = [](const char* name, float& v) {
        const char* e = env(name);
        if (e) v = std::strtof(e, nullptr);
    };
    auto env_str = [](const char* name, std::string& v) {
        const char* e = env(name);
        if (e) v = e;
    };
    auto env_bool = [](const char* name, bool& v) {
        const char* e = env(name);
        if (e) {
            std::string s(e);
            v = (s == "1" || s == "true" || s == "yes");
        }
    };

    env_int("CAM_ID", cfg.cam_id);
    env_int("FRAME_W", cfg.frame_w);
    env_int("FRAME_H", cfg.frame_h);
    env_int("FPS", cfg.fps);
    env_int("SKIP_FRAMES", cfg.skip_frames);
    env_float("EYE_CLOSED_RATIO", cfg.eye_closed_ratio);
    env_float("MOUTH_OPEN_RATIO", cfg.mouth_open_ratio);
    env_bool("USE_TFLITE", cfg.use_tflite);
    env_str("TFLITE_MODEL_PATH", cfg.tflite_model_path);
    env_str("LANDMARK_MODEL_PATH", cfg.landmark_model_path);
    env_str("YUNET_MODEL_PATH", cfg.yunet_model_path);
    env_float("EYE_CLOSED_PROB", cfg.eye_closed_prob);
    env_float("YAWN_PROB", cfg.yawn_prob);
    env_float("EYES_CLOSED_DURATION", cfg.eyes_closed_duration);
    env_int("SPEED_THRESHOLD", cfg.speed_threshold);
    env_str("DB_PATH", cfg.db_path);
    env_str("IMAGE_DIR", cfg.image_dir);
    env_str("GPS_PORT", cfg.gps_port);
    env_int("GPS_BAUD", cfg.gps_baud);
    env_int("MGMT_PORT", cfg.mgmt_port);
    env_str("LOG_LEVEL", cfg.log_level);
}

} // namespace dms
