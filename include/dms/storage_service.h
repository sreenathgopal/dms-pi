#pragma once

#include <zmq.hpp>
#include <atomic>

namespace dms {

// SQLite storage thread.
// Subscribes to detection.status + gps.position, writes to local database.
void storage_service(zmq::context_t& ctx, std::atomic<bool>& shutdown);

} // namespace dms
