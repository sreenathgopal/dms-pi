#include <dms/management_service.h>
#include <dms/config.h>

#include <microhttpd.h>
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <iostream>
#include <string>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include <thread>

namespace dms {

using json = nlohmann::json;

static time_t g_start_time = 0;

// Forward-declare post_json for create_device / handshake
static size_t write_cb_mgmt(void* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

static json mgmt_post_json(const std::string& url, const json& payload, int timeout_sec = 15) {
    CURL* curl = curl_easy_init();
    if (!curl) return json();

    std::string body = payload.dump();
    std::string response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb_mgmt);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return json();
    try { return json::parse(response); } catch (...) { return json(); }
}

// --- Helper to send JSON response ---
static MHD_Result send_json(struct MHD_Connection* conn, int status_code, const json& data) {
    std::string body = data.dump();
    struct MHD_Response* response = MHD_create_response_from_buffer(
        body.size(), (void*)body.c_str(), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", "application/json");
    MHD_Result ret = MHD_queue_response(conn, status_code, response);
    MHD_destroy_response(response);
    return ret;
}

// --- Read process memory ---
static json get_memory() {
    json mem;
    std::ifstream f("/proc/self/status");
    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("VmRSS:") == 0) {
                int kb = 0;
                sscanf(line.c_str(), "VmRSS: %d", &kb);
                mem["rss_kb"] = kb;
            } else if (line.find("VmSize:") == 0) {
                int kb = 0;
                sscanf(line.c_str(), "VmSize: %d", &kb);
                mem["vsz_kb"] = kb;
            }
        }
    }
    return mem;
}

// --- Read POST body ---
struct PostData {
    std::string body;
};

// --- Route handlers ---

static MHD_Result handle_health(struct MHD_Connection* conn) {
    return send_json(conn, MHD_HTTP_OK, {{"status", "ok"}});
}

static MHD_Result handle_status(struct MHD_Connection* conn) {
    const auto& cfg = config();
    json resp = {
        {"uptime_seconds", static_cast<int>(time(nullptr) - g_start_time)},
        {"memory", get_memory()},
        {"architecture", "cpp-zmq-threads"},
        {"config", {
            {"resolution", std::to_string(cfg.frame_w) + "x" + std::to_string(cfg.frame_h)},
            {"skip_frames", cfg.skip_frames},
            {"speed_threshold", cfg.speed_threshold},
            {"tflite_enabled", cfg.use_tflite},
            {"zmq_detection", cfg.zmq_detection_endpoint},
            {"zmq_gps", cfg.zmq_gps_endpoint}
        }}
    };
    return send_json(conn, MHD_HTTP_OK, resp);
}

static MHD_Result handle_devices(struct MHD_Connection* conn) {
    const auto& cfg = config();
    sqlite3* db = nullptr;
    std::string device_id;
    if (sqlite3_open_v2(cfg.db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "SELECT device_id FROM device LIMIT 1", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (val) device_id = val;
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }
    return send_json(conn, MHD_HTTP_OK, {{"device_id", device_id}});
}

static MHD_Result handle_users(struct MHD_Connection* conn) {
    const auto& cfg = config();
    json user_data = json::object();
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(cfg.db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "SELECT * FROM user_info LIMIT 1", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                int cols = sqlite3_column_count(stmt);
                for (int i = 0; i < cols; i++) {
                    const char* name = sqlite3_column_name(stmt, i);
                    const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                    if (name && val) user_data[name] = val;
                }
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }
    return send_json(conn, MHD_HTTP_OK, user_data);
}

static MHD_Result handle_get_config(struct MHD_Connection* conn) {
    const auto& cfg = config();
    json resp = {
        {"cam_id", cfg.cam_id},
        {"frame_w", cfg.frame_w},
        {"frame_h", cfg.frame_h},
        {"fps", cfg.fps},
        {"skip_frames", cfg.skip_frames},
        {"use_tflite", cfg.use_tflite},
        {"speed_threshold", cfg.speed_threshold},
        {"eyes_closed_duration", cfg.eyes_closed_duration},
        {"eye_closed_prob", cfg.eye_closed_prob},
        {"yawn_prob", cfg.yawn_prob},
        {"buzzer_pin_1", cfg.buzzer_pin_1},
        {"buzzer_pin_2", cfg.buzzer_pin_2},
        {"led_pin", cfg.led_pin},
        {"mgmt_port", cfg.mgmt_port},
        {"db_path", cfg.db_path},
        {"gps_port", cfg.gps_port}
    };
    return send_json(conn, MHD_HTTP_OK, resp);
}

static MHD_Result handle_create_device(struct MHD_Connection* conn) {
    const auto& cfg = config();
    json payload = {
        {"device_type", "DC"},
        {"admin_id", "admin@admin.com"},
        {"admin_pass", "12345678"}
    };

    auto result = mgmt_post_json(cfg.api_create_device, payload);
    if (result.is_null() || !result.value("success", false)) {
        return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, {{"error", "API returned no device_id"}});
    }

    std::string dev_id = result["data"]["device_id"].get<std::string>();

    // Save to DB
    sqlite3* db = nullptr;
    if (sqlite3_open(cfg.db_path.c_str(), &db) == SQLITE_OK) {
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO device (device_id) VALUES (?)", -1, &stmt, nullptr);
        sqlite3_bind_text(stmt, 1, dev_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        sqlite3_prepare_v2(db,
            "INSERT INTO configure (config_key, config_value) VALUES ('speed', '0') "
            "ON CONFLICT(config_key) DO UPDATE SET config_value='0'", -1, &stmt, nullptr);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }

    return send_json(conn, MHD_HTTP_OK, {{"device_id", dev_id}});
}

static MHD_Result handle_handshake(struct MHD_Connection* conn) {
    const auto& cfg = config();

    // Get device_id from DB
    std::string device_id;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(cfg.db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, "SELECT device_id FROM device LIMIT 1", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (val) device_id = val;
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }

    if (device_id.empty()) {
        return send_json(conn, MHD_HTTP_BAD_REQUEST, {{"error", "no device_id registered"}});
    }

    json payload = {{"device_id", device_id}};
    auto result = mgmt_post_json(cfg.api_handshake, payload);
    if (result.is_null() || !result.value("success", false)) {
        return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, {{"error", "handshake failed"}});
    }

    auto data = result["data"];

    // Save user info to DB
    sqlite3* db2 = nullptr;
    if (sqlite3_open(cfg.db_path.c_str(), &db2) == SQLITE_OK) {
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db2,
            "INSERT OR REPLACE INTO user_info (user_id, name, last_name, email, phone_number, access_token) "
            "VALUES (?, ?, ?, ?, ?, ?)", -1, &stmt, nullptr);

        sqlite3_bind_int(stmt, 1, data.value("user_id", 0));
        auto name = data.value("name", "");
        auto last = data.value("last_name", "");
        auto email = data.value("email", "");
        std::string phone = data.value("phone_code", "") + data.value("phone", "");
        auto token = data.value("access_token", "");

        sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, last.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, email.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, phone.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, token.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        sqlite3_close(db2);
    }

    return send_json(conn, MHD_HTTP_OK, data);
}

static MHD_Result handle_set_speed(struct MHD_Connection* conn, const std::string& body) {
    try {
        auto j = json::parse(body);
        if (j.contains("speed")) {
            config().speed_threshold = j["speed"].get<int>();
            return send_json(conn, MHD_HTTP_OK, {{"speed_threshold", config().speed_threshold}});
        }
    } catch (...) {}
    return send_json(conn, MHD_HTTP_BAD_REQUEST, {{"error", "missing 'speed'"}});
}

static MHD_Result handle_wifi(struct MHD_Connection* conn, const std::string& body) {
    try {
        auto j = json::parse(body);
        std::string ssid = j.value("ssid", "");
        std::string password = j.value("password", "");
        if (ssid.empty()) {
            return send_json(conn, MHD_HTTP_BAD_REQUEST, {{"error", "missing 'ssid'"}});
        }

        // nmcli connect
        std::string cmd = "nmcli device wifi connect '" + ssid + "' password '" + password + "' 2>&1";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, {{"error", "popen failed"}});
        }
        char buf[256];
        std::string output;
        while (fgets(buf, sizeof(buf), pipe)) output += buf;
        int ret = pclose(pipe);

        if (ret == 0) {
            return send_json(conn, MHD_HTTP_OK, {{"status", "connected"}, {"ssid", ssid}});
        } else {
            return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                             {{"error", "nmcli failed: " + output}});
        }
    } catch (...) {
        return send_json(conn, MHD_HTTP_BAD_REQUEST, {{"error", "invalid JSON"}});
    }
}

// --- MHD request handler ---
static MHD_Result request_handler(void* cls,
                                   struct MHD_Connection* connection,
                                   const char* url,
                                   const char* method,
                                   const char* version,
                                   const char* upload_data,
                                   size_t* upload_data_size,
                                   void** con_cls)
{
    (void)cls;
    (void)version;

    // Handle POST body accumulation
    if (strcmp(method, "POST") == 0) {
        if (*con_cls == nullptr) {
            PostData* pd = new PostData();
            *con_cls = pd;
            return MHD_YES;
        }
        PostData* pd = static_cast<PostData*>(*con_cls);
        if (*upload_data_size > 0) {
            pd->body.append(upload_data, *upload_data_size);
            *upload_data_size = 0;
            return MHD_YES;
        }

        // POST body complete — route it
        MHD_Result result;
        if (strcmp(url, "/api/create_device") == 0) {
            result = handle_create_device(connection);
        } else if (strcmp(url, "/api/handshake") == 0) {
            result = handle_handshake(connection);
        } else if (strcmp(url, "/api/config/speed") == 0) {
            result = handle_set_speed(connection, pd->body);
        } else if (strcmp(url, "/api/wifi") == 0) {
            result = handle_wifi(connection, pd->body);
        } else {
            result = send_json(connection, MHD_HTTP_NOT_FOUND, {{"error", "not found"}});
        }
        delete pd;
        *con_cls = nullptr;
        return result;
    }

    // GET routes
    if (strcmp(method, "GET") == 0) {
        if (strcmp(url, "/health") == 0)       return handle_health(connection);
        if (strcmp(url, "/api/status") == 0)    return handle_status(connection);
        if (strcmp(url, "/api/devices") == 0)   return handle_devices(connection);
        if (strcmp(url, "/api/users") == 0)     return handle_users(connection);
        if (strcmp(url, "/api/config") == 0)    return handle_get_config(connection);
        return send_json(connection, MHD_HTTP_NOT_FOUND, {{"error", "not found"}});
    }

    return send_json(connection, MHD_HTTP_METHOD_NOT_ALLOWED, {{"error", "method not allowed"}});
}

// Cleanup callback for POST data
static void request_completed(void* cls, struct MHD_Connection* connection,
                               void** con_cls, enum MHD_RequestTerminationCode toe) {
    (void)cls;
    (void)connection;
    (void)toe;
    if (*con_cls) {
        delete static_cast<PostData*>(*con_cls);
        *con_cls = nullptr;
    }
}

void management_service(zmq::context_t& ctx, std::atomic<bool>& shutdown) {
    (void)ctx;
    const auto& cfg = config();
    g_start_time = time(nullptr);

    struct MHD_Daemon* daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD,
        cfg.mgmt_port,
        nullptr, nullptr,
        &request_handler, nullptr,
        MHD_OPTION_NOTIFY_COMPLETED, &request_completed, nullptr,
        MHD_OPTION_END
    );

    if (!daemon) {
        std::cerr << "[mgmt] Failed to start HTTP server on :" << cfg.mgmt_port << std::endl;
        return;
    }

    std::cout << "[mgmt] Management HTTP server on :" << cfg.mgmt_port << std::endl;

    while (!shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    MHD_stop_daemon(daemon);
    std::cout << "[mgmt] Management server stopped" << std::endl;
}

} // namespace dms
