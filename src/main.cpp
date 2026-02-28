#include <dms/config.h>
#include <dms/camera_service.h>
#include <dms/gps_service.h>
#include <dms/alert_service.h>
#include <dms/storage_service.h>
#include <dms/uplink_service.h>
#include <dms/management_service.h>

#include <zmq.hpp>
#include <iostream>
#include <thread>
#include <atomic>
#include <signal.h>
#include <cstring>
#include <chrono>

#include <sys/stat.h>
#include <unistd.h>

static std::atomic<bool> g_shutdown{false};

static void signal_handler(int sig) {
    std::cout << "\n[main] Shutdown signal received (" << sig << ")" << std::endl;
    g_shutdown.store(true);
}

int main() {
    // --- Load configuration ---
    dms::load_config(dms::config());
    const auto& cfg = dms::config();

    std::cout << "=== DMS starting (C++ single-process, ZMQ inproc) ===" << std::endl;
    std::cout << "Config: resolution=" << cfg.frame_w << "x" << cfg.frame_h
              << ", skip=" << cfg.skip_frames
              << ", speed_threshold=" << cfg.speed_threshold
              << ", tflite=" << (cfg.use_tflite ? "true" : "false") << std::endl;

    // --- Signal handling ---
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    // --- Ensure directories exist ---
    mkdir("/tmp/dms", 0755);
    mkdir("/tmp/dms_images", 0755);
    mkdir("/var/lib/dms", 0755);

    // --- ZeroMQ context (shared by all threads in single process) ---
    zmq::context_t zmq_ctx(1);

    // --- Start microservice threads ---

    // 1. GPS service — must start first (binds PUB, camera will connect as SUB)
    std::thread gps_thread(dms::gps_service, std::ref(zmq_ctx), std::ref(g_shutdown));
    std::cout << "[1/6] GPS service started" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 2. Camera detection (binds detection PUB, connects to GPS SUB)
    std::thread camera_thread(dms::camera_service, std::ref(zmq_ctx), std::ref(g_shutdown));
    std::cout << "[2/6] Camera service started" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 3. Alert service (subscribes to detection)
    std::thread alert_thread(dms::alert_service, std::ref(zmq_ctx), std::ref(g_shutdown));
    std::cout << "[3/6] Alert service started" << std::endl;

    // 4. Storage service (subscribes to detection + GPS)
    std::thread storage_thread(dms::storage_service, std::ref(zmq_ctx), std::ref(g_shutdown));
    std::cout << "[4/6] Storage service started" << std::endl;

    // 5. Uplink service (subscribes to detection + GPS)
    std::thread uplink_thread(dms::uplink_service, std::ref(zmq_ctx), std::ref(g_shutdown));
    std::cout << "[5/6] Uplink service started" << std::endl;

    // 6. Management HTTP server
    std::thread mgmt_thread(dms::management_service, std::ref(zmq_ctx), std::ref(g_shutdown));
    std::cout << "[6/6] Management service started on :" << cfg.mgmt_port << std::endl;

    std::cout << "=== DMS running (6 microservices, single process) ===" << std::endl;

    // --- Main loop: wait for shutdown ---
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    std::cout << "=== DMS shutting down ===" << std::endl;

    // Wait for all threads (each checks g_shutdown)
    auto join_with_timeout = [](std::thread& t, const char* name) {
        if (t.joinable()) {
            t.join();
            std::cout << "[main] " << name << " stopped" << std::endl;
        }
    };

    join_with_timeout(mgmt_thread, "Management");
    join_with_timeout(uplink_thread, "Uplink");
    join_with_timeout(storage_thread, "Storage");
    join_with_timeout(alert_thread, "Alert");
    join_with_timeout(camera_thread, "Camera");
    join_with_timeout(gps_thread, "GPS");

    zmq_ctx.close();
    std::cout << "=== DMS stopped ===" << std::endl;
    return 0;
}
