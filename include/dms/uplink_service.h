#pragma once

#include <zmq.hpp>
#include <atomic>

namespace dms {

// Fleet API uplink thread.
// Subscribes to detection + GPS events, sends status via libcurl, uploads images.
void uplink_service(zmq::context_t& ctx, std::atomic<bool>& shutdown);

} // namespace dms
