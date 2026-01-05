#ifndef WEBRTC_GLOBALS_H
#define WEBRTC_GLOBALS_H

#include <atomic>

namespace webrtc {

// Global shutdown flag - set to false to stop all threads
extern std::atomic<bool> g_running;

// Global keyframe request flag - set to true to request keyframe from encoder
extern std::atomic<bool> g_request_keyframe;

}  // namespace webrtc

// Debug flags (global scope for encoder compatibility)
extern bool g_debug_png;
extern bool g_debug_mode_switch;

#endif  // WEBRTC_GLOBALS_H
