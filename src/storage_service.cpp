#include <dms/storage_service.h>
#include <dms/config.h>
#include <dms/messages.h>

#include <sqlite3.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <ctime>

#include <sys/stat.h>

namespace dms {

static const char* CREATE_TABLES = R"SQL(
CREATE TABLE IF NOT EXISTS device (
    device_id TEXT PRIMARY KEY
);
CREATE TABLE IF NOT EXISTS user_info (
    user_id INTEGER PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    last_name TEXT NOT NULL DEFAULT '',
    email TEXT NOT NULL DEFAULT '',
    phone_number TEXT NOT NULL DEFAULT '',
    access_token TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS configure (
    config_key TEXT PRIMARY KEY,
    config_value TEXT NOT NULL DEFAULT '',
    description TEXT DEFAULT '',
    created_at TEXT DEFAULT (datetime('now')),
    updated_at TEXT DEFAULT (datetime('now'))
);
CREATE TABLE IF NOT EXISTS gps_data (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    latitude REAL,
    longitude REAL,
    speed REAL,
    timestamp TEXT DEFAULT (datetime('now')),
    driver_status TEXT DEFAULT '',
    acceleration REAL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS car_data (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT DEFAULT (datetime('now')),
    driver_status TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_gps_timestamp ON gps_data(timestamp);
CREATE INDEX IF NOT EXISTS idx_car_timestamp ON car_data(timestamp);
)SQL";

// Ensure parent directory exists
static void ensure_dir(const std::string& path) {
    auto pos = path.rfind('/');
    if (pos != std::string::npos) {
        std::string dir = path.substr(0, pos);
        mkdir(dir.c_str(), 0755);
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

void storage_service(zmq::context_t& ctx, std::atomic<bool>& shutdown) {
    const auto& cfg = config();

    // --- SQLite setup ---
    ensure_dir(cfg.db_path);
    sqlite3* db = nullptr;
    int rc = sqlite3_open(cfg.db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        std::cerr << "[storage] Cannot open DB: " << cfg.db_path
                  << " — " << sqlite3_errmsg(db) << std::endl;
        return;
    }

    // Pragmas
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA cache_size=-2000", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA busy_timeout=5000", nullptr, nullptr, nullptr);

    // Create tables
    char* err_msg = nullptr;
    sqlite3_exec(db, CREATE_TABLES, nullptr, nullptr, &err_msg);
    if (err_msg) {
        std::cerr << "[storage] Table creation error: " << err_msg << std::endl;
        sqlite3_free(err_msg);
    }

    // Prepared statements
    sqlite3_stmt* stmt_gps = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT INTO gps_data (latitude, longitude, speed, timestamp, driver_status, acceleration) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        -1, &stmt_gps, nullptr);

    sqlite3_stmt* stmt_car = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT INTO car_data (driver_status) VALUES (?)",
        -1, &stmt_car, nullptr);

    sqlite3_stmt* stmt_cleanup_gps = nullptr;
    sqlite3_prepare_v2(db,
        "DELETE FROM gps_data WHERE timestamp < datetime('now', '-24 hours')",
        -1, &stmt_cleanup_gps, nullptr);

    sqlite3_stmt* stmt_cleanup_car = nullptr;
    sqlite3_prepare_v2(db,
        "DELETE FROM car_data WHERE timestamp < datetime('now', '-24 hours')",
        -1, &stmt_cleanup_car, nullptr);

    std::cout << "[storage] Database opened: " << cfg.db_path << std::endl;

    // --- ZeroMQ subscriber ---
    zmq::socket_t sub(ctx, zmq::socket_type::sub);
    sub.set(zmq::sockopt::subscribe, "detection.status");
    sub.set(zmq::sockopt::subscribe, "gps.position");
    sub.set(zmq::sockopt::rcvtimeo, 1000);
    sub.set(zmq::sockopt::linger, 0);
    sub.connect(cfg.zmq_detection_endpoint);
    sub.connect(cfg.zmq_gps_endpoint);

    // Local state
    double last_lat = 0, last_lon = 0;
    float last_speed = 0, last_acc = 0;
    std::string last_status = "normal";
    auto last_gps_write = std::chrono::steady_clock::now();
    auto last_cleanup = std::chrono::steady_clock::now();

    std::cout << "[storage] Storage service running" << std::endl;

    while (!shutdown.load()) {
        zmq::message_t topic_msg, payload_msg;
        auto recv_rc = sub.recv(topic_msg, zmq::recv_flags::none);
        if (!recv_rc) {
            // Periodic cleanup during idle
            auto now = std::chrono::steady_clock::now();
            double since_cleanup = std::chrono::duration<double>(now - last_cleanup).count();
            if (since_cleanup >= 3600.0) {
                sqlite3_step(stmt_cleanup_gps);
                sqlite3_reset(stmt_cleanup_gps);
                sqlite3_step(stmt_cleanup_car);
                sqlite3_reset(stmt_cleanup_car);
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
            last_speed = gps.speed;
            last_acc = gps.acc;

            // Write GPS at most once per interval
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - last_gps_write).count();
            if (dt >= cfg.gps_store_interval) {
                sqlite3_bind_double(stmt_gps, 1, last_lat);
                sqlite3_bind_double(stmt_gps, 2, last_lon);
                sqlite3_bind_double(stmt_gps, 3, last_speed);
                auto ts = now_str();
                sqlite3_bind_text(stmt_gps, 4, ts.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt_gps, 5, last_status.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(stmt_gps, 6, last_acc);
                sqlite3_step(stmt_gps);
                sqlite3_reset(stmt_gps);
                last_gps_write = now;
            }

        } else if (topic == "detection.status") {
            auto det = unpack<DetectionStatusMsg>(payload_msg.data(), payload_msg.size());
            const auto& status = det.status;

            if (status != last_status &&
                (status == "sleeping" || status == "yawning" || status == "no_face"))
            {
                sqlite3_bind_text(stmt_car, 1, status.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt_car);
                sqlite3_reset(stmt_car);
            }
            last_status = status;
        }
    }

    // Cleanup
    sqlite3_finalize(stmt_gps);
    sqlite3_finalize(stmt_car);
    sqlite3_finalize(stmt_cleanup_gps);
    sqlite3_finalize(stmt_cleanup_car);
    sqlite3_close(db);
    sub.close();
    std::cout << "[storage] Storage service stopped" << std::endl;
}

} // namespace dms
