#include <dms/alert_service.h>
#include <dms/config.h>
#include <dms/messages.h>

#include <iostream>
#include <thread>
#include <chrono>

#ifndef DMS_NO_GPIO
#include <gpiod.h>
#endif

namespace dms {

// Non-blocking buzz helper using a detached thread
static void async_buzz(
#ifndef DMS_NO_GPIO
    struct gpiod_line* line1,
    struct gpiod_line* line2,
#endif
    float duration_sec)
{
#ifndef DMS_NO_GPIO
    auto buzz_fn = [line1, line2, duration_sec]() {
        if (line1) gpiod_line_set_value(line1, 1);
        if (line2) gpiod_line_set_value(line2, 1);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<int>(duration_sec * 1000)));
        if (line1) gpiod_line_set_value(line1, 0);
        if (line2) gpiod_line_set_value(line2, 0);
    };
    std::thread(buzz_fn).detach();
#else
    (void)duration_sec;
    std::cout << "[alert] BUZZ (" << duration_sec << "s)" << std::endl;
#endif
}

void alert_service(zmq::context_t& ctx, std::atomic<bool>& shutdown) {
    const auto& cfg = config();

#ifndef DMS_NO_GPIO
    // --- GPIO setup via libgpiod v1 ---
    struct gpiod_chip* chip = gpiod_chip_open_by_name("gpiochip0");
    struct gpiod_line* buzzer1 = nullptr;
    struct gpiod_line* buzzer2 = nullptr;
    struct gpiod_line* led = nullptr;

    if (chip) {
        buzzer1 = gpiod_chip_get_line(chip, cfg.buzzer_pin_1);
        buzzer2 = gpiod_chip_get_line(chip, cfg.buzzer_pin_2);
        led = gpiod_chip_get_line(chip, cfg.led_pin);

        if (buzzer1) gpiod_line_request_output(buzzer1, "dms-buzz1", 0);
        if (buzzer2) gpiod_line_request_output(buzzer2, "dms-buzz2", 0);
        if (led) gpiod_line_request_output(led, "dms-led", 0);

        std::cout << "[alert] GPIO initialized (buzzer=" << cfg.buzzer_pin_1
                  << "," << cfg.buzzer_pin_2 << " led=" << cfg.led_pin << ")" << std::endl;
    } else {
        std::cerr << "[alert] GPIO chip unavailable — log-only alerts" << std::endl;
    }
#else
    std::cout << "[alert] Built without GPIO — log-only alerts" << std::endl;
#endif

    // --- ZeroMQ subscriber ---
    zmq::socket_t sub(ctx, zmq::socket_type::sub);
    sub.set(zmq::sockopt::subscribe, "detection.status");
    sub.set(zmq::sockopt::rcvtimeo, 500);
    sub.set(zmq::sockopt::linger, 0);
    sub.connect(cfg.zmq_detection_endpoint);

    std::string last_status;

    std::cout << "[alert] Alert service running" << std::endl;

    while (!shutdown.load()) {
        zmq::message_t topic_msg, payload_msg;
        auto rc = sub.recv(topic_msg, zmq::recv_flags::none);
        if (!rc) continue;
        sub.recv(payload_msg, zmq::recv_flags::none);

        auto event = unpack<DetectionStatusMsg>(payload_msg.data(), payload_msg.size());
        const auto& status = event.status;
        const auto& buzz = event.buzz;

        // --- Buzzer ---
        if (buzz == "short") {
            async_buzz(
#ifndef DMS_NO_GPIO
                buzzer1, buzzer2,
#endif
                cfg.buzzer_duration);
        } else if (buzz == "long") {
            async_buzz(
#ifndef DMS_NO_GPIO
                buzzer1, buzzer2,
#endif
                cfg.buzzer_duration * 3);
        }

        // --- LED: on during alert states, off when normal ---
        bool is_alert = (status == "sleeping" || status == "yawning" || status == "no_face");
        bool was_alert = (last_status == "sleeping" || last_status == "yawning" || last_status == "no_face");

#ifndef DMS_NO_GPIO
        if (is_alert && !was_alert && led) {
            gpiod_line_set_value(led, 1);
        } else if (!is_alert && was_alert && led) {
            gpiod_line_set_value(led, 0);
        }
#endif

        last_status = status;
    }

    // Cleanup
#ifndef DMS_NO_GPIO
    if (led) { gpiod_line_set_value(led, 0); gpiod_line_release(led); }
    if (buzzer1) { gpiod_line_set_value(buzzer1, 0); gpiod_line_release(buzzer1); }
    if (buzzer2) { gpiod_line_set_value(buzzer2, 0); gpiod_line_release(buzzer2); }
    if (chip) gpiod_chip_close(chip);
#endif
    sub.close();
    std::cout << "[alert] Alert service stopped" << std::endl;
}

} // namespace dms
