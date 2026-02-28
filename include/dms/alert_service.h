#pragma once

#include <zmq.hpp>
#include <atomic>

namespace dms {

// GPIO alert thread.
// Subscribes to detection.status, drives buzzer + LED via libgpiod.
void alert_service(zmq::context_t& ctx, std::atomic<bool>& shutdown);

} // namespace dms
