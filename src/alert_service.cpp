#include <dms/alert_service.h>
#include <dms/config.h>

#include <iostream>
#include <thread>
#include <chrono>

#ifndef DMS_NO_GPIO
#include <gpiod.h>
#endif

namespace dms {

#ifndef DMS_NO_GPIO
static struct gpiod_chip* g_chip = nullptr;
static struct gpiod_line* g_buzzer1 = nullptr;
static struct gpiod_line* g_buzzer2 = nullptr;
static struct gpiod_line* g_led = nullptr;
#endif

void gpio_init() {
    const auto& cfg = config();

#ifndef DMS_NO_GPIO
    g_chip = gpiod_chip_open_by_name("gpiochip0");
    if (g_chip) {
        g_buzzer1 = gpiod_chip_get_line(g_chip, cfg.buzzer_pin_1);
        g_buzzer2 = gpiod_chip_get_line(g_chip, cfg.buzzer_pin_2);
        g_led = gpiod_chip_get_line(g_chip, cfg.led_pin);

        if (g_buzzer1) gpiod_line_request_output(g_buzzer1, "dms-buzz1", 0);
        if (g_buzzer2) gpiod_line_request_output(g_buzzer2, "dms-buzz2", 0);
        if (g_led) gpiod_line_request_output(g_led, "dms-led", 0);

        std::cout << "[gpio] Initialized (buzzer=" << cfg.buzzer_pin_1
                  << "," << cfg.buzzer_pin_2 << " led=" << cfg.led_pin << ")" << std::endl;
    } else {
        std::cerr << "[gpio] Chip unavailable — log-only alerts" << std::endl;
    }
#else
    (void)cfg;
    std::cout << "[gpio] Built without GPIO — log-only alerts" << std::endl;
#endif
}

void gpio_cleanup() {
#ifndef DMS_NO_GPIO
    if (g_led) { gpiod_line_set_value(g_led, 0); gpiod_line_release(g_led); }
    if (g_buzzer1) { gpiod_line_set_value(g_buzzer1, 0); gpiod_line_release(g_buzzer1); }
    if (g_buzzer2) { gpiod_line_set_value(g_buzzer2, 0); gpiod_line_release(g_buzzer2); }
    if (g_chip) gpiod_chip_close(g_chip);
    g_led = g_buzzer1 = g_buzzer2 = nullptr;
    g_chip = nullptr;
#endif
    std::cout << "[gpio] Cleaned up" << std::endl;
}

void gpio_buzz(float duration_sec) {
#ifndef DMS_NO_GPIO
    auto buzz_fn = [duration_sec]() {
        if (g_buzzer1) gpiod_line_set_value(g_buzzer1, 1);
        if (g_buzzer2) gpiod_line_set_value(g_buzzer2, 1);
        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<int>(duration_sec * 1000)));
        if (g_buzzer1) gpiod_line_set_value(g_buzzer1, 0);
        if (g_buzzer2) gpiod_line_set_value(g_buzzer2, 0);
    };
    std::thread(buzz_fn).detach();
#else
    (void)duration_sec;
    std::cout << "[gpio] BUZZ (" << duration_sec << "s)" << std::endl;
#endif
}

void gpio_led(bool on) {
#ifndef DMS_NO_GPIO
    if (g_led) gpiod_line_set_value(g_led, on ? 1 : 0);
#else
    (void)on;
#endif
}

} // namespace dms
