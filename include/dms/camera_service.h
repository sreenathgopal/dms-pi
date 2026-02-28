#pragma once

#include <zmq.hpp>
#include <atomic>

namespace dms {

// Camera detection pipeline thread.
// Captures frames, runs YuNet + landmark TFLite + classifier + state machine.
// Publishes detection.status and detection.image on ZMQ inproc.
void camera_service(zmq::context_t& ctx, std::atomic<bool>& shutdown);

} // namespace dms
