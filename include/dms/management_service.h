#pragma once

#include <dms/app_state.h>
#include <dms/ring_buffer.h>
#include <atomic>

namespace dms {

// Start the libmicrohttpd web server (non-blocking — uses internal polling thread).
void start_web_server(int port, AppState& state, RingBuffer& ring_buf,
                       std::atomic<bool>& shutdown);

// Stop the web server.
void stop_web_server();

} // namespace dms
