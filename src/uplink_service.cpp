#include <dms/uplink_service.h>
#include <dms/config.h>
#include <dms/messages.h>

#include <curl/curl.h>
#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <chrono>
#include <thread>
#include <ctime>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

namespace dms {

using json = nlohmann::json;

// Base64 encoding (for image upload)
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const uint8_t* data, size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);

        result += b64_table[(n >> 18) & 0x3F];
        result += b64_table[(n >> 12) & 0x3F];
        result += (i + 1 < len) ? b64_table[(n >> 6) & 0x3F] : '=';
        result += (i + 2 < len) ? b64_table[n & 0x3F] : '=';
    }
    return result;
}

// libcurl write callback
static size_t write_cb(void* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

// POST JSON to URL with bearer token
static json post_json(const std::string& url, const json& payload,
                      const std::string& token, int timeout_sec = 10) {
    CURL* curl = curl_easy_init();
    if (!curl) return json();

    std::string body = payload.dump();
    std::string response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!token.empty()) {
        std::string auth = "Authorization: Bearer " + token;
        headers = curl_slist_append(headers, auth.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return json();

    try {
        return json::parse(response);
    } catch (...) {
        return json();
    }
}

static std::string now_str() {
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

// Save image to tmpfs and upload to fleet API
static void handle_image(const std::vector<uint8_t>& jpeg, const std::string& status,
                         const std::string& device_id, const std::string& token,
                         const std::string& image_dir) {
    // Save to tmpfs
    mkdir(image_dir.c_str(), 0755);
    time_t ts = time(nullptr);
    std::string path = image_dir + "/" + status + "_" + std::to_string(ts) + ".jpg";
    {
        std::ofstream f(path, std::ios::binary);
        if (f.is_open()) {
            f.write(reinterpret_cast<const char*>(jpeg.data()), jpeg.size());
        }
    }

    // Upload
    std::string encoded = base64_encode(jpeg.data(), jpeg.size());
    json payload = {
        {"device_id", device_id},
        {"file_name", encoded},
        {"date", now_str()}
    };

    auto result = post_json(config().api_upload_file, payload, token, 30);
    if (!result.is_null()) {
        // Delete after successful upload
        unlink(path.c_str());
    }
}

// Cleanup old images
static void cleanup_images(const std::string& image_dir, int max_age, int max_count) {
    DIR* dir = opendir(image_dir.c_str());
    if (!dir) return;

    time_t now = time(nullptr);
    struct FileInfo {
        std::string path;
        time_t mtime;
    };
    std::vector<FileInfo> files;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() < 5 || name.substr(name.size() - 4) != ".jpg") continue;

        std::string path = image_dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;

        double age = difftime(now, st.st_mtime);
        if (age > max_age) {
            unlink(path.c_str());
        } else {
            files.push_back({path, st.st_mtime});
        }
    }
    closedir(dir);

    // Keep only max_count most recent
    if (static_cast<int>(files.size()) > max_count) {
        std::sort(files.begin(), files.end(),
                  [](const FileInfo& a, const FileInfo& b) { return a.mtime > b.mtime; });
        for (size_t i = max_count; i < files.size(); i++) {
            unlink(files[i].path.c_str());
        }
    }
}

void uplink_service(zmq::context_t& ctx, std::atomic<bool>& shutdown) {
    const auto& cfg = config();

    // Initialize libcurl
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // --- Load credentials from database ---
    std::string device_id;
    std::string access_token;
    {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(cfg.db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
            sqlite3_stmt* stmt;

            // Device ID
            if (sqlite3_prepare_v2(db, "SELECT device_id FROM device LIMIT 1", -1, &stmt, nullptr) == SQLITE_OK) {
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    if (val) device_id = val;
                }
                sqlite3_finalize(stmt);
            }

            // Access token
            if (sqlite3_prepare_v2(db, "SELECT access_token FROM user_info LIMIT 1", -1, &stmt, nullptr) == SQLITE_OK) {
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    if (val) access_token = val;
                }
                sqlite3_finalize(stmt);
            }
            sqlite3_close(db);
        }
    }

    bool api_enabled = !device_id.empty() && !access_token.empty();
    std::cout << "[uplink] Uplink service running (device=" << (device_id.empty() ? "NONE" : device_id)
              << ", api=" << (api_enabled ? "enabled" : "disabled") << ")" << std::endl;

    // --- ZeroMQ subscriber ---
    zmq::socket_t sub(ctx, zmq::socket_type::sub);
    sub.set(zmq::sockopt::subscribe, "detection.");
    sub.set(zmq::sockopt::subscribe, "gps.position");
    sub.set(zmq::sockopt::rcvtimeo, 1000);
    sub.set(zmq::sockopt::linger, 0);
    sub.connect(cfg.zmq_detection_endpoint);
    sub.connect(cfg.zmq_gps_endpoint);

    double last_lat = 0, last_lon = 0;
    float last_acc = 0;
    auto last_api_send = std::chrono::steady_clock::now();
    auto last_cleanup = std::chrono::steady_clock::now();

    while (!shutdown.load()) {
        zmq::message_t topic_msg, payload_msg;
        auto rc = sub.recv(topic_msg, zmq::recv_flags::none);
        if (!rc) {
            // Periodic image cleanup during idle
            auto now = std::chrono::steady_clock::now();
            double since_cleanup = std::chrono::duration<double>(now - last_cleanup).count();
            if (since_cleanup >= 60.0) {
                cleanup_images(cfg.image_dir, cfg.image_max_age, cfg.image_max_count);
                last_cleanup = now;
            }
            continue;
        }
        sub.recv(payload_msg, zmq::recv_flags::none);

        std::string topic(static_cast<const char*>(topic_msg.data()), topic_msg.size());

        if (topic == "gps.position") {
            auto gps = unpack<GpsPositionMsg>(payload_msg.data(), payload_msg.size());
            last_lat = gps.lat;
            last_lon = gps.lon;
            last_acc = gps.acc;

        } else if (topic == "detection.status" && api_enabled) {
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - last_api_send).count();
            if (dt >= cfg.api_send_interval) {
                auto det = unpack<DetectionStatusMsg>(payload_msg.data(), payload_msg.size());
                json payload = {
                    {"device_id", device_id},
                    {"data", {
                        {"timestamp", now_str()},
                        {"speed", std::to_string(det.speed)},
                        {"lat", std::to_string(last_lat)},
                        {"long", std::to_string(last_lon)},
                        {"driver_status", det.status},
                        {"acceleration", std::to_string(last_acc)}
                    }}
                };
                post_json(cfg.api_store_data, payload, access_token);
                last_api_send = now;
            }

        } else if (topic == "detection.image" && api_enabled) {
            auto img = unpack<DetectionImageMsg>(payload_msg.data(), payload_msg.size());
            if (!img.jpeg.empty()) {
                handle_image(img.jpeg, img.status, device_id, access_token, cfg.image_dir);
            }
        }
    }

    sub.close();
    curl_global_cleanup();
    std::cout << "[uplink] Uplink service stopped" << std::endl;
}

} // namespace dms
