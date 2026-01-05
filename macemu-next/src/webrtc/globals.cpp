/*
 * WebRTC Global State
 *
 * Shared atomic variables used for thread coordination.
 * These will eventually be replaced with proper dependency injection.
 */

#include <atomic>

namespace webrtc {

// Global shutdown flag (set by main thread, read by all worker threads)
std::atomic<bool> g_running(true);

// Global keyframe request flag (set by WebRTC thread when new peer connects)
std::atomic<bool> g_request_keyframe(false);

} // namespace webrtc

// Debug flags (global scope for encoder compatibility)
bool g_debug_png = false;
bool g_debug_mode_switch = false;
