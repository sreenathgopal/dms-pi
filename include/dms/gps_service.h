#pragma once

#include <zmq.hpp>
#include <atomic>

namespace dms {

// GPS serial reader thread.
// Reads NMEA sentences from serial port, publishes gps.position on ZMQ inproc.
void gps_service(zmq::context_t& ctx, std::atomic<bool>& shutdown);

} // namespace dms
