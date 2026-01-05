/*
 * WebRTC Server - Header
 */

#ifndef WEBRTC_SERVER_H
#define WEBRTC_SERVER_H

#include "../config/config_manager.h"

namespace webrtc {

/**
 * WebRTC server thread entry point
 *
 * Manages HTTP server, WebRTC signaling, and peer connections.
 * Runs until g_running is set to false.
 *
 * @param config Configuration
 */
void webrtc_server_main(config::MacemuConfig* config);

} // namespace webrtc

#endif // WEBRTC_SERVER_H
