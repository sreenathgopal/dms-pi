#include <dms/management_service.h>
#include <dms/config.h>

#include <microhttpd.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <fcntl.h>

namespace dms {

using json = nlohmann::json;

// Global pointers set by start_web_server()
static AppState* g_state = nullptr;
static RingBuffer* g_ring = nullptr;
static std::atomic<bool>* g_shutdown = nullptr;
static struct MHD_Daemon* g_daemon = nullptr;

// --- Helper: send JSON response ---
static MHD_Result send_json(struct MHD_Connection* conn, int status_code, const json& data) {
    std::string body = data.dump();
    struct MHD_Response* response = MHD_create_response_from_buffer(
        body.size(), (void*)body.c_str(), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", "application/json");
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    MHD_Result ret = MHD_queue_response(conn, status_code, response);
    MHD_destroy_response(response);
    return ret;
}

// --- Helper: send plain text ---
static MHD_Result send_text(struct MHD_Connection* conn, int status_code, const std::string& text) {
    struct MHD_Response* response = MHD_create_response_from_buffer(
        text.size(), (void*)text.c_str(), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(response, "Content-Type", "text/plain");
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

// --- Get disk usage ---
static json get_disk_usage(const std::string& path) {
    struct statvfs st;
    if (statvfs(path.c_str(), &st) == 0) {
        long total_mb = (long)(st.f_blocks * st.f_frsize) / (1024 * 1024);
        long free_mb = (long)(st.f_bfree * st.f_frsize) / (1024 * 1024);
        return {{"total_mb", total_mb}, {"free_mb", free_mb}, {"used_mb", total_mb - free_mb}};
    }
    return {};
}

// --- List files in a directory (*.avi) ---
static json list_files(const std::string& dir) {
    json files = json::array();
    DIR* d = opendir(dir.c_str());
    if (!d) return files;

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() < 5) continue;
        // Accept .avi and .jpg files
        std::string ext = name.substr(name.size() - 4);
        if (ext != ".avi" && ext != ".jpg") continue;

        std::string full_path = dir + "/" + name;
        struct stat st;
        if (stat(full_path.c_str(), &st) == 0) {
            files.push_back({
                {"name", name},
                {"size", st.st_size},
                {"mtime", static_cast<long>(st.st_mtime)}
            });
        }
    }
    closedir(d);

    // Sort by mtime descending (newest first)
    std::sort(files.begin(), files.end(), [](const json& a, const json& b) {
        return a["mtime"].get<long>() > b["mtime"].get<long>();
    });
    return files;
}

// --- Validate filename (prevent path traversal) ---
static bool valid_filename(const std::string& name) {
    if (name.empty() || name.find("..") != std::string::npos || name.find('/') != std::string::npos) {
        return false;
    }
    std::string ext = name.substr(name.size() >= 4 ? name.size() - 4 : 0);
    return (ext == ".avi" || ext == ".jpg");
}

// --- POST body accumulation ---
struct PostData {
    std::string body;
};

// --- Route handlers ---

static MHD_Result handle_status(struct MHD_Connection* conn) {
    const auto& cfg = config();
    json status_data;
    {
        std::lock_guard<std::mutex> lock(g_state->mtx);
        status_data = {
            {"status", g_state->detection_status},
            {"ear_l", g_state->ear_l},
            {"ear_r", g_state->ear_r},
            {"mar", g_state->mar},
            {"detection_fps", g_state->detection_fps},
            {"sleep_alerts", g_state->sleep_alerts},
            {"yawn_alerts", g_state->yawn_alerts},
            {"uptime_seconds", static_cast<int>(time(nullptr) - g_state->start_time)},
        };
    }
    status_data["ring_buffer_frames"] = static_cast<int>(g_ring->size());
    status_data["ring_buffer_total"] = g_ring->total_frames();
    status_data["memory"] = get_memory();
    status_data["disk"] = get_disk_usage(cfg.recordings_dir);
    status_data["config"] = {
        {"resolution", std::to_string(cfg.frame_w) + "x" + std::to_string(cfg.frame_h)},
        {"fps", cfg.fps},
        {"skip_frames", cfg.skip_frames},
        {"tflite_enabled", cfg.use_tflite}
    };
    return send_json(conn, MHD_HTTP_OK, status_data);
}

static MHD_Result handle_recordings(struct MHD_Connection* conn) {
    const auto& cfg = config();
    return send_json(conn, MHD_HTTP_OK, list_files(cfg.recordings_dir));
}

static MHD_Result handle_alerts(struct MHD_Connection* conn) {
    const auto& cfg = config();
    return send_json(conn, MHD_HTTP_OK, list_files(cfg.alerts_dir));
}

static MHD_Result handle_download(struct MHD_Connection* conn) {
    const char* f_param = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "f");
    if (!f_param || !valid_filename(f_param)) {
        return send_json(conn, MHD_HTTP_BAD_REQUEST, {{"error", "invalid filename"}});
    }

    const auto& cfg = config();
    std::string filename(f_param);

    // Search in recordings and alerts directories
    std::string full_path;
    std::string try_rec = cfg.recordings_dir + "/" + filename;
    std::string try_alert = cfg.alerts_dir + "/" + filename;

    struct stat st;
    if (stat(try_rec.c_str(), &st) == 0) {
        full_path = try_rec;
    } else if (stat(try_alert.c_str(), &st) == 0) {
        full_path = try_alert;
    } else {
        return send_json(conn, MHD_HTTP_NOT_FOUND, {{"error", "file not found"}});
    }

    // Open file
    int fd = open(full_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, {{"error", "cannot open file"}});
    }

    // Use MHD's file response with Range support
    struct MHD_Response* response = MHD_create_response_from_fd_at_offset64(
        st.st_size, fd, 0);

    // Content-Type based on extension
    if (filename.size() >= 4 && filename.substr(filename.size() - 4) == ".jpg") {
        MHD_add_response_header(response, "Content-Type", "image/jpeg");
    } else {
        MHD_add_response_header(response, "Content-Type", "video/x-msvideo");
    }
    MHD_add_response_header(response, "Accept-Ranges", "bytes");
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");

    // Check for Range header
    const char* range = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Range");
    int status_code = MHD_HTTP_OK;
    if (range) {
        // Parse "bytes=START-END"
        long long start = 0, end = st.st_size - 1;
        if (sscanf(range, "bytes=%lld-%lld", &start, &end) >= 1) {
            if (end >= st.st_size) end = st.st_size - 1;
            // Recreate response with offset
            MHD_destroy_response(response);
            close(fd);
            fd = open(full_path.c_str(), O_RDONLY);
            if (fd < 0) {
                return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, {{"error", "cannot reopen file"}});
            }
            long long length = end - start + 1;
            response = MHD_create_response_from_fd_at_offset64(length, fd, start);

            char content_range[128];
            snprintf(content_range, sizeof(content_range), "bytes %lld-%lld/%lld",
                     start, end, (long long)st.st_size);
            MHD_add_response_header(response, "Content-Range", content_range);
            MHD_add_response_header(response, "Accept-Ranges", "bytes");
            MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
            if (filename.size() >= 4 && filename.substr(filename.size() - 4) == ".jpg") {
                MHD_add_response_header(response, "Content-Type", "image/jpeg");
            } else {
                MHD_add_response_header(response, "Content-Type", "video/x-msvideo");
            }
            status_code = MHD_HTTP_PARTIAL_CONTENT;
        }
    }

    MHD_Result ret = MHD_queue_response(conn, status_code, response);
    MHD_destroy_response(response);
    return ret;
}

static MHD_Result handle_delete(struct MHD_Connection* conn) {
    const char* f_param = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "f");
    if (!f_param || !valid_filename(f_param)) {
        return send_json(conn, MHD_HTTP_BAD_REQUEST, {{"error", "invalid filename"}});
    }

    const auto& cfg = config();
    std::string filename(f_param);

    // Search in recordings and alerts
    std::string try_rec = cfg.recordings_dir + "/" + filename;
    std::string try_alert = cfg.alerts_dir + "/" + filename;

    if (unlink(try_rec.c_str()) == 0 || unlink(try_alert.c_str()) == 0) {
        return send_json(conn, MHD_HTTP_OK, {{"deleted", filename}});
    }
    return send_json(conn, MHD_HTTP_NOT_FOUND, {{"error", "file not found"}});
}

static MHD_Result handle_config_get(struct MHD_Connection* conn) {
    const auto& cfg = config();
    json resp = {
        {"frame_w", cfg.frame_w},
        {"frame_h", cfg.frame_h},
        {"fps", cfg.fps},
        {"skip_frames", cfg.skip_frames},
        {"use_tflite", cfg.use_tflite},
        {"eyes_closed_duration", cfg.eyes_closed_duration},
        {"eye_closed_prob", cfg.eye_closed_prob},
        {"yawn_prob", cfg.yawn_prob},
        {"buzzer_pin_1", cfg.buzzer_pin_1},
        {"buzzer_pin_2", cfg.buzzer_pin_2},
        {"led_pin", cfg.led_pin},
        {"ring_buffer_seconds", cfg.ring_buffer_seconds},
        {"recordings_dir", cfg.recordings_dir},
        {"alerts_dir", cfg.alerts_dir},
        {"web_port", cfg.web_port}
    };
    return send_json(conn, MHD_HTTP_OK, resp);
}

static MHD_Result handle_config_post(struct MHD_Connection* conn, const std::string& body) {
    try {
        auto j = json::parse(body);
        auto& cfg = config();
        if (j.contains("skip_frames")) cfg.skip_frames = j["skip_frames"].get<int>();
        if (j.contains("eye_closed_prob")) cfg.eye_closed_prob = j["eye_closed_prob"].get<float>();
        if (j.contains("yawn_prob")) cfg.yawn_prob = j["yawn_prob"].get<float>();
        if (j.contains("eyes_closed_duration")) cfg.eyes_closed_duration = j["eyes_closed_duration"].get<float>();
        return send_json(conn, MHD_HTTP_OK, {{"status", "updated"}});
    } catch (...) {
        return send_json(conn, MHD_HTTP_BAD_REQUEST, {{"error", "invalid JSON"}});
    }
}

static MHD_Result handle_time_sync(struct MHD_Connection* conn, const std::string& body) {
    try {
        auto j = json::parse(body);
        std::string utc = j.value("utc", "");
        if (utc.empty()) {
            return send_json(conn, MHD_HTTP_BAD_REQUEST, {{"error", "missing 'utc'"}});
        }
        // Set system clock (requires root or sudo NOPASSWD)
        std::string cmd = "sudo date -s '" + utc + "' > /dev/null 2>&1";
        int ret = system(cmd.c_str());
        if (ret == 0) {
            return send_json(conn, MHD_HTTP_OK, {{"status", "time_set"}, {"utc", utc}});
        }
        return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, {{"error", "date command failed"}});
    } catch (...) {
        return send_json(conn, MHD_HTTP_BAD_REQUEST, {{"error", "invalid JSON"}});
    }
}

// --- MJPEG stream callback ---
struct StreamCtx {
    RingBuffer* ring;
    std::atomic<bool>* shutdown;
    int frame_count;
    bool header_sent;
};

static ssize_t mjpeg_stream_callback(void* cls, uint64_t pos, char* buf, size_t max) {
    (void)pos;
    auto* ctx = static_cast<StreamCtx*>(cls);
    if (ctx->shutdown->load()) {
        delete ctx;
        return MHD_CONTENT_READER_END_OF_STREAM;
    }

    JpegFrame latest;
    // Wait briefly for a new frame
    for (int i = 0; i < 10; i++) {
        if (ctx->ring->latest(latest)) break;
        usleep(100000);  // 100ms
        if (ctx->shutdown->load()) {
            delete ctx;
            return MHD_CONTENT_READER_END_OF_STREAM;
        }
    }

    if (latest.data.empty()) {
        delete ctx;
        return MHD_CONTENT_READER_END_OF_STREAM;
    }

    // Build multipart chunk
    std::string header = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                        + std::to_string(latest.data.size()) + "\r\n\r\n";
    std::string footer = "\r\n";

    size_t total = header.size() + latest.data.size() + footer.size();
    if (total > max) {
        // Buffer too small — skip this frame
        usleep(500000);
        return 0;
    }

    memcpy(buf, header.c_str(), header.size());
    memcpy(buf + header.size(), latest.data.data(), latest.data.size());
    memcpy(buf + header.size() + latest.data.size(), footer.c_str(), footer.size());

    ctx->frame_count++;

    // Throttle to ~2 fps for preview
    usleep(500000);

    return static_cast<ssize_t>(total);
}

static void mjpeg_stream_free(void* cls) {
    delete static_cast<StreamCtx*>(cls);
}

static MHD_Result handle_stream(struct MHD_Connection* conn) {
    auto* ctx = new StreamCtx{g_ring, g_shutdown, 0, false};

    struct MHD_Response* response = MHD_create_response_from_callback(
        MHD_SIZE_UNKNOWN, 256 * 1024,  // 256KB buffer
        &mjpeg_stream_callback, ctx, &mjpeg_stream_free);

    MHD_add_response_header(response, "Content-Type",
                             "multipart/x-mixed-replace; boundary=frame");
    MHD_add_response_header(response, "Cache-Control", "no-cache");
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");

    MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return ret;
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
        if (strcmp(url, "/api/config") == 0) {
            result = handle_config_post(connection, pd->body);
        } else if (strcmp(url, "/api/time") == 0) {
            result = handle_time_sync(connection, pd->body);
        } else {
            result = send_json(connection, MHD_HTTP_NOT_FOUND, {{"error", "not found"}});
        }
        delete pd;
        *con_cls = nullptr;
        return result;
    }

    // Handle DELETE
    if (strcmp(method, "DELETE") == 0) {
        if (strcmp(url, "/api/file") == 0) return handle_delete(connection);
        return send_json(connection, MHD_HTTP_NOT_FOUND, {{"error", "not found"}});
    }

    // GET routes
    if (strcmp(method, "GET") == 0) {
        if (strcmp(url, "/health") == 0)          return send_json(connection, MHD_HTTP_OK, {{"status", "ok"}});
        if (strcmp(url, "/api/status") == 0)       return handle_status(connection);
        if (strcmp(url, "/api/recordings") == 0)   return handle_recordings(connection);
        if (strcmp(url, "/api/alerts") == 0)        return handle_alerts(connection);
        if (strcmp(url, "/api/download") == 0)      return handle_download(connection);
        if (strcmp(url, "/api/config") == 0)        return handle_config_get(connection);
        if (strcmp(url, "/api/stream") == 0)        return handle_stream(connection);
        return send_json(connection, MHD_HTTP_NOT_FOUND, {{"error", "not found"}});
    }

    // CORS preflight
    if (strcmp(method, "OPTIONS") == 0) {
        struct MHD_Response* response = MHD_create_response_from_buffer(0, nullptr, MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
        MHD_add_response_header(response, "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
        MHD_add_response_header(response, "Access-Control-Allow-Headers", "Content-Type, Range");
        MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_NO_CONTENT, response);
        MHD_destroy_response(response);
        return ret;
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

void start_web_server(int port, AppState& state, RingBuffer& ring_buf,
                       std::atomic<bool>& shutdown)
{
    g_state = &state;
    g_ring = &ring_buf;
    g_shutdown = &shutdown;

    g_daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD,
        port,
        nullptr, nullptr,
        &request_handler, nullptr,
        MHD_OPTION_NOTIFY_COMPLETED, &request_completed, nullptr,
        MHD_OPTION_END
    );

    if (!g_daemon) {
        std::cerr << "[web] Failed to start HTTP server on :" << port << std::endl;
        return;
    }

    std::cout << "[web] HTTP server on :" << port << std::endl;
}

void stop_web_server() {
    if (g_daemon) {
        MHD_stop_daemon(g_daemon);
        g_daemon = nullptr;
        std::cout << "[web] HTTP server stopped" << std::endl;
    }
}

} // namespace dms
