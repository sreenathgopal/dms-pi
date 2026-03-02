#pragma once

namespace dms {

// GPIO utility functions for buzzer + LED.
// Call gpio_init() once at startup, gpio_cleanup() at shutdown.

void gpio_init();
void gpio_cleanup();

// Non-blocking buzz (spawns a short detached thread)
void gpio_buzz(float duration_sec);

// LED on/off
void gpio_led(bool on);

} // namespace dms
