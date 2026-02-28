#pragma once

#include <zmq.hpp>
#include <atomic>

namespace dms {

// HTTP management server thread.
// Provides health checks and JSON API on port 8080 via libmicrohttpd.
void management_service(zmq::context_t& ctx, std::atomic<bool>& shutdown);

} // namespace dms
