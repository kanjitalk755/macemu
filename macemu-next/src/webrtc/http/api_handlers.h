/*
 * API Handlers Module
 *
 * Handles all /api/ endpoints for server control and status
 */

#ifndef API_HANDLERS_H
#define API_HANDLERS_H

#include "http_server.h"
#include "../../BasiliskII/src/IPC/ipc_protocol.h"
#include "../config/server_config.h"  // For CodecType
#include <string>
#include <functional>

namespace http {

/**
 * API Context - provides access to server state for API handlers
 *
 * This is a temporary bridge pattern to avoid global variables.
 * TODO: In Phase 7, convert to proper dependency injection
 */
struct APIContext {
    // Configuration
    bool debug_connection;
    bool debug_mode_switch;
    bool debug_perf;
    std::string prefs_path;
    std::string roms_path;
    std::string images_path;

    // Emulator state
    bool emulator_connected;
    int emulator_pid;
    int started_emulator_pid;
    MacEmuIPCBuffer* ipc_shm;  // Shared memory for video and audio IPC
    std::atomic<bool>* user_stopped_emulator;  // Flag: user explicitly stopped via web UI

    // Codec state
    CodecType* server_codec;  // Pointer to g_server_codec
    std::function<void(CodecType)> notify_codec_change_fn;  // Notify clients of codec change

    // Command callbacks
    std::function<void(uint8_t)> send_command_fn;
    std::function<bool()> start_emulator_fn;
    std::function<void()> stop_emulator_fn;
    std::function<void()> disconnect_emulator_fn;
    std::function<void(bool)> request_restart_fn;
};

/**
 * API Router
 *
 * Routes API requests to appropriate handlers
 */
class APIRouter {
public:
    explicit APIRouter(APIContext* context);

    // Handle API request, returns response (or empty if not an API route)
    Response handle(const Request& req, bool* handled);

private:
    // New unified config endpoints
    Response handle_config_get(const Request& req);     // GET /api/config - return macemu-config.json
    Response handle_config_save(const Request& req);    // POST /api/config - save macemu-config.json

    // Legacy endpoints (deprecated, kept for compatibility)
    Response handle_config(const Request& req);         // Old config endpoint
    Response handle_config_post(const Request& req);    // Old config POST
    Response handle_storage(const Request& req);
    Response handle_prefs_get(const Request& req);      // Deprecated
    Response handle_prefs_post(const Request& req);     // Deprecated

    Response handle_restart(const Request& req);
    Response handle_status(const Request& req);
    Response handle_codec_post(const Request& req);
    Response handle_emulator_change(const Request& req);
    Response handle_emulator_start(const Request& req);
    Response handle_emulator_stop(const Request& req);
    Response handle_emulator_restart(const Request& req);
    Response handle_emulator_reset(const Request& req);
    Response handle_log(const Request& req);
    Response handle_error(const Request& req);

    APIContext* ctx_;
};

} // namespace http

#endif // API_HANDLERS_H
