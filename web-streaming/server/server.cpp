/*
 * Standalone WebRTC Server for macemu (BasiliskII / SheepShaver)
 *
 * Architecture (v4):
 * - Server CONNECTS to emulator resources by PID
 * - Emulator creates SHM (/macemu-video-{PID}) and socket (/tmp/macemu-{PID}.sock)
 * - Emulator converts Mac framebuffer to BGRA (B,G,R,A bytes = libyuv "ARGB")
 * - Server encodes to H.264 (via I420 conversion) or PNG (via RGB conversion)
 * - Server handles browser keycode to Mac keycode conversion
 */

#include "ipc_protocol.h"
#include "codec.h"
#include "h264_encoder.h"
#include "av1_encoder.h"
#include "vp9_encoder.h"
#include "png_encoder.h"
#include "webp_encoder.h"
#include "opus_encoder.h"
#include "audio_config.h"

// Utility modules
#include "utils/keyboard_map.h"
#include "utils/json_utils.h"
#include "config/server_config.h"
#include "config/config_manager.h"
#include "ipc/ipc_connection.h"
#include "storage/file_scanner.h"
#include "storage/prefs_manager.h"
#include "http/http_server.h"
#include "http/api_handlers.h"
#include "http/static_files.h"

#include <rtc/rtc.hpp>
#include <rtc/rtppacketizer.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/av1rtppacketizer.hpp>
#include <rtc/vp9rtppacketizer.hpp>
#include <rtc/rtppacketizationconfig.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <rtc/frameinfo.hpp>

#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <map>
#include <vector>
#include <thread>
#include <chrono>
#include <functional>
#include <sstream>
#include <fstream>
#include <csignal>
#include <iomanip>
#include <execinfo.h>
#include <ucontext.h>

// POSIX IPC and process management
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <pwd.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>

// Configuration (centralized in ServerConfig)
static server_config::ServerConfig g_config;

// Helper: Expand $HOME in paths
static std::string expand_path(const std::string& path) {
    if (path.find("$HOME") == 0) {
        const char* home = getenv("HOME");
        if (!home) {
            struct passwd* pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : "/tmp";
        }
        return std::string(home) + path.substr(5);  // Skip "$HOME"
    }
    return path;
}

// Legacy accessors for gradual migration
// TODO: Phase 7 - Pass ServerConfig reference instead of using globals
#define g_http_port         (g_config.http_port)
#define g_signaling_port    (g_config.signaling_port)
#define g_roms_path         (g_config.roms_path)
#define g_images_path       (g_config.images_path)
#define g_prefs_path        (g_config.prefs_path)
#define g_emulator_path     (g_config.emulator_path)
#define g_auto_start_emulator (g_config.auto_start_emulator)
#define g_target_emulator_pid (g_config.target_emulator_pid)
#define g_server_codec      (g_config.server_codec)
#define g_enable_stun       (g_config.enable_stun)
#define g_stun_server       (g_config.stun_server)
#define g_debug_connection  (g_config.debug_connection)
#define g_debug_perf        (g_config.debug_perf)
#define g_debug_frames      (g_config.debug_frames)
#define g_debug_audio       (g_config.debug_audio)
#define g_debug_mouse       (g_config.debug_mouse)

// g_debug_mode_switch and g_debug_png need to be non-static for encoders
bool g_debug_mode_switch = false;
bool g_debug_png = false;

// Global state
static std::atomic<bool> g_running(true);
static std::atomic<bool> g_emulator_connected(false);
static std::atomic<bool> g_restart_emulator_requested(false);
static std::atomic<bool> g_user_stopped_emulator(false);  // User explicitly stopped via web UI
static pid_t g_emulator_pid = -1;
static CodecType g_previous_codec = CodecType::PNG;  // Track codec changes for peer disconnection

// IPC connection (replaces individual global handles)
static ipc::IPCConnection g_ipc;

// Legacy accessors for gradual migration
// TODO: Phase 7 - Pass IPCConnection reference instead of using globals
#define g_ipc_shm           (g_ipc.get_shm())  // Shared memory for both video and audio
#define g_control_socket    (g_ipc.get_control_socket())
#define g_frame_ready_eventfd (g_ipc.get_frame_eventfd())
#define g_audio_ready_eventfd (g_ipc.get_audio_eventfd())
#define g_connected_shm_name  (g_ipc.get_shm_name())
#define g_connected_socket_path (g_ipc.get_socket_path())

// Input event counters (for stats)
static std::atomic<uint64_t> g_mouse_move_count(0);
static std::atomic<uint64_t> g_mouse_click_count(0);
static std::atomic<uint64_t> g_key_count(0);


// Global flag to request keyframe (set when new peer connects)
static std::atomic<bool> g_request_keyframe(false);

// Audio capture trigger (removed - was debug feature via stdin monitor)
// static std::atomic<bool> g_capture_requested(false);

// Audio encoder (shared across all peers)
static std::unique_ptr<OpusAudioEncoder> g_audio_encoder;

// Signal handler for graceful shutdown
static void signal_handler(int sig) {
    fprintf(stderr, "\nServer: Received signal %d, shutting down...\n", sig);
    g_running = false;
}

// Crash handler for fatal signals with full backtrace
static void crash_handler(int sig, siginfo_t *info, void *context) {
    // Use async-signal-safe functions only
    const char *signame = "UNKNOWN";
    switch (sig) {
        case SIGSEGV: signame = "SIGSEGV (Segmentation Fault)"; break;
        case SIGBUS:  signame = "SIGBUS (Bus Error)"; break;
        case SIGABRT: signame = "SIGABRT (Abort)"; break;
        case SIGILL:  signame = "SIGILL (Illegal Instruction)"; break;
        case SIGFPE:  signame = "SIGFPE (Floating Point Exception)"; break;
    }

    fprintf(stderr, "\n");
    fprintf(stderr, "╔════════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║              FATAL CRASH IN WEBRTC SERVER                      ║\n");
    fprintf(stderr, "╚════════════════════════════════════════════════════════════════╝\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Signal:  %d (%s)\n", sig, signame);

    if (info) {
        fprintf(stderr, "Code:    %d\n", info->si_code);
        if (sig == SIGSEGV || sig == SIGBUS) {
            fprintf(stderr, "Address: %p (invalid memory access)\n", info->si_addr);
        }
    }

    // Print register state (x86-64 specific)
#if defined(__x86_64__)
    if (context) {
        ucontext_t *uctx = (ucontext_t *)context;
        mcontext_t *mctx = &uctx->uc_mcontext;

        fprintf(stderr, "\n=== REGISTER STATE ===\n");
        fprintf(stderr, "  RIP: 0x%016llx  (instruction pointer)\n", (unsigned long long)mctx->gregs[REG_RIP]);
        fprintf(stderr, "  RSP: 0x%016llx  (stack pointer)\n", (unsigned long long)mctx->gregs[REG_RSP]);
        fprintf(stderr, "  RBP: 0x%016llx  (base pointer)\n", (unsigned long long)mctx->gregs[REG_RBP]);
        fprintf(stderr, "  RAX: 0x%016llx  RBX: 0x%016llx\n",
            (unsigned long long)mctx->gregs[REG_RAX],
            (unsigned long long)mctx->gregs[REG_RBX]);
        fprintf(stderr, "  RCX: 0x%016llx  RDX: 0x%016llx\n",
            (unsigned long long)mctx->gregs[REG_RCX],
            (unsigned long long)mctx->gregs[REG_RDX]);
        fprintf(stderr, "=== END REGISTER STATE ===\n");
    }
#endif

    // Print backtrace with source locations using addr2line
    fprintf(stderr, "\n=== BACKTRACE ===\n");
    void *array[64];
    size_t size = backtrace(array, 64);
    char **strings = backtrace_symbols(array, size);

    // Get binary base address from /proc/self/maps
    FILE *maps = fopen("/proc/self/maps", "r");
    unsigned long base_addr = 0;
    if (maps) {
        char line[512];
        if (fgets(line, sizeof(line), maps)) {
            // First line is the base mapping
            sscanf(line, "%lx-", &base_addr);
        }
        fclose(maps);
    }

    fprintf(stderr, "Binary base address: 0x%lx\n\n", base_addr);

    // Print with symbols and calculate offsets
    for (size_t i = 0; i < size; i++) {
        unsigned long offset = (unsigned long)array[i] - base_addr;
        fprintf(stderr, "  [%2zu] %p (offset 0x%lx)", i, array[i], offset);
        if (strings && strings[i]) {
            fprintf(stderr, " %s", strings[i]);
        }
        fprintf(stderr, "\n");
    }

    fprintf(stderr, "\nDecoded (using addr2line via fork/exec):\n");
    fflush(stderr);

    // Fork and use addr2line to decode with offsets (signal-safe)
    pid_t pid = fork();
    if (pid == 0) {
        // Child process - run addr2line
        char exe_path[256];
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (len == -1) {
            _exit(1);
        }
        exe_path[len] = '\0';

        // Build argv for addr2line using offsets from base
        char *argv[size + 10];
        argv[0] = (char*)"addr2line";
        argv[1] = (char*)"-e";
        argv[2] = exe_path;
        argv[3] = (char*)"-f";
        argv[4] = (char*)"-C";
        argv[5] = (char*)"-i";
        argv[6] = (char*)"-p";

        char addr_strs[64][32];
        for (size_t i = 0; i < size; i++) {
            unsigned long offset = (unsigned long)array[i] - base_addr;
            snprintf(addr_strs[i], sizeof(addr_strs[i]), "0x%lx", offset);
            argv[7 + i] = addr_strs[i];
        }
        argv[7 + size] = NULL;

        execvp("addr2line", argv);
        _exit(1);  // If execvp fails
    } else if (pid > 0) {
        // Parent - wait for addr2line to finish
        int status;
        waitpid(pid, &status, 0);
    }

    if (strings) {
        free(strings);
    }

    fprintf(stderr, "=== END BACKTRACE ===\n\n");

    // Print server state information
    fprintf(stderr, "=== SERVER STATE ===\n");
    fprintf(stderr, "  Emulator connected: %s\n", g_emulator_connected.load() ? "YES" : "NO");
    if (g_emulator_pid > 0) {
        fprintf(stderr, "  Emulator PID:       %d\n", g_emulator_pid);
    }
    fprintf(stderr, "  Codec:              %s\n",
        g_server_codec == CodecType::H264 ? "H.264" :
        g_server_codec == CodecType::AV1 ? "AV1" :
        g_server_codec == CodecType::VP9 ? "VP9" :
        g_server_codec == CodecType::WEBP ? "WebP" : "PNG");
    fprintf(stderr, "=== END SERVER STATE ===\n\n");

    fprintf(stderr, "Crash report complete. Generating core dump...\n");
    fflush(stderr);

    // Re-raise signal with default handler to generate core dump
    signal(sig, SIG_DFL);
    raise(sig);
}


// Browser keycode to Mac ADB keycode conversion moved to utils/keyboard_map.cpp

// JSON helpers moved to utils/json_utils.cpp
// Using nlohmann/json library instead of hand-written parsing
using json = json_utils::json;

// JSON utility for parsing WebSocket messages
static std::string json_get_string(const std::string& json_str, const std::string& key) {
    try {
        json j = json::parse(json_str);
        return json_utils::get_string(j, key);
    } catch (...) {
        return "";
    }
}


// IPC functions moved to ipc/ipc_connection.cpp

// Wrapper functions for compatibility during migration
static bool send_key_input(int mac_keycode, bool down) {
    return g_ipc.send_key_input(mac_keycode, down);
}

static bool send_mouse_input(int dx, int dy, uint8_t buttons, uint64_t browser_timestamp_ms, bool absolute = false) {
    return g_ipc.send_mouse_input(dx, dy, buttons, browser_timestamp_ms, absolute);
}

static bool send_mouse_mode_change(bool relative) {
    return g_ipc.send_mouse_mode_change(relative);
}

static bool send_command(uint8_t command) {
    return g_ipc.send_command(command);
}

static bool send_ping_input(uint32_t sequence, uint64_t t1_browser_send_ms) {
    return g_ipc.send_ping_input(sequence, t1_browser_send_ms);
}

static bool try_connect_to_emulator(pid_t pid) {
    bool success = g_ipc.connect_to_emulator(pid);
    if (success) {
        g_emulator_pid = pid;
        g_emulator_connected = true;
        // Request keyframe so browser gets a full frame from new emulator
        g_request_keyframe.store(true);
    }
    return success;
}

// Forward declarations
class WebRTCServer;
static void disconnect_from_emulator(WebRTCServer* webrtc = nullptr, bool disconnect_peers = false);


/*
 * Scan for running emulators (wrapper for ipc::scan_for_emulators)
 */

static std::vector<pid_t> scan_for_emulators() {
    return ipc::scan_for_emulators();
}


/*
 * Emulator Process Management (for when server starts emulator)
 */

static std::string find_emulator() {
    // If path explicitly set, use it
    if (!g_emulator_path.empty()) {
        if (access(g_emulator_path.c_str(), X_OK) == 0) {
            return g_emulator_path;
        }
        fprintf(stderr, "Emulator: Specified path not executable: %s\n", g_emulator_path.c_str());
        return "";
    }

    // Look for emulator in bin/ subdirectory only
    const char* candidates[] = {
        "./bin/BasiliskII",
        "./bin/SheepShaver",
        nullptr
    };

    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], X_OK) == 0) {
            char* resolved = realpath(candidates[i], nullptr);
            if (resolved) {
                std::string path(resolved);
                free(resolved);
                return path;
            }
        }
    }

    return "";
}

static pid_t g_started_emulator_pid = -1;  // PID of emulator we started

static bool start_emulator() {
    if (g_started_emulator_pid > 0) {
        // Already started one, check if still alive
        int status;
        pid_t result = waitpid(g_started_emulator_pid, &status, WNOHANG);
        if (result == 0) {
            // Still running
            return true;
        }
        // Exited
        g_started_emulator_pid = -1;
    }

    // Load unified JSON config from ~/.macemu/
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    std::string config_path = std::string(home) + "/.macemu/macemu-config.json";
    config::MacemuConfig cfg = config::load_config(config_path);

    // Update server codec from config
    if (cfg.web.codec == "h264") {
        g_config.server_codec = CodecType::H264;
    } else if (cfg.web.codec == "av1") {
        g_config.server_codec = CodecType::AV1;
    } else if (cfg.web.codec == "vp9") {
        g_config.server_codec = CodecType::VP9;
    } else if (cfg.web.codec == "webp") {
        g_config.server_codec = CodecType::WEBP;
    } else {
        g_config.server_codec = CodecType::PNG;
    }

    // Determine which emulator to launch (use binaries in PATH)
    bool is_ppc = (cfg.web.emulator == "ppc");
    const char* emu_name = is_ppc ? "sheepshaver-webrtc" : "basiliskii-webrtc";

    // Generate appropriate prefs file and ensure config directories exist
    std::string prefs_content;
    std::string prefs_file;
    // home already declared above when loading config

    if (is_ppc) {
        prefs_content = config::generate_sheepshaver_prefs(cfg, g_roms_path, g_images_path);
        // SheepShaver reads from ~/.config/SheepShaver/prefs by default
        std::string config_dir = std::string(home) + "/.config/SheepShaver";
        // Create directory if it doesn't exist
        mkdir((std::string(home) + "/.config").c_str(), 0755);
        mkdir(config_dir.c_str(), 0755);
        prefs_file = config_dir + "/prefs";
    } else {
        prefs_content = config::generate_basilisk_prefs(cfg, g_roms_path, g_images_path);
        // BasiliskII will use --config flag
        std::string config_dir = std::string(home) + "/.config/BasiliskII";
        // Create directory if it doesn't exist
        mkdir((std::string(home) + "/.config").c_str(), 0755);
        mkdir(config_dir.c_str(), 0755);
        prefs_file = config_dir + "/prefs";
    }

    // Write prefs file
    if (!storage::write_prefs_file(prefs_file, prefs_content)) {
        fprintf(stderr, "Emulator: Failed to write prefs file: %s\n", prefs_file.c_str());
        return false;
    }

    fprintf(stderr, "\n");
    fprintf(stderr, "🚀 LAUNCHING EMULATOR:\n");
    fprintf(stderr, "   Type:   %s\n", is_ppc ? "SheepShaver (PPC)" : "BasiliskII (68k)");
    fprintf(stderr, "   Binary: %s (from PATH)\n", emu_name);
    fprintf(stderr, "   Prefs:  %s (generated from macemu-config.json)\n", prefs_file.c_str());
    fprintf(stderr, "   Codec:  %s\n", cfg.web.codec.c_str());
    fprintf(stderr, "   Mouse:  %s\n", cfg.web.mousemode.c_str());
    fprintf(stderr, "\n");

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Emulator: Fork failed: %s\n", strerror(errno));
        return false;
    }

    if (pid == 0) {
        // Child process

        // Close server's file descriptors
        for (int fd = 3; fd < 1024; fd++) {
            close(fd);
        }

        // Pass debug flags to emulator via environment variables
        if (g_debug_mode_switch) setenv("MACEMU_DEBUG_MODE_SWITCH", "1", 1);
        if (g_debug_perf) setenv("MACEMU_DEBUG_PERF", "1", 1);
        if (g_debug_frames) setenv("MACEMU_DEBUG_FRAMES", "1", 1);

        // Execute emulator with generated prefs file (search PATH)
        // BasiliskII uses --config and --xpram flags
        // SheepShaver reads from ~/.config/SheepShaver/prefs by default
        if (is_ppc) {
            // SheepShaver: no args needed, reads prefs from default location
            execlp(emu_name, emu_name, nullptr);
        } else {
            // BasiliskII: use --config and --xpram flags
            std::string xpram_path = std::string(home) + "/.config/BasiliskII/xpram";
            execlp(emu_name, emu_name,
                   "--config", prefs_file.c_str(),
                   "--xpram", xpram_path.c_str(),
                   nullptr);
        }

        // If exec fails
        fprintf(stderr, "Emulator: Failed to execute %s: %s\n", emu_name, strerror(errno));
        fprintf(stderr, "Emulator: Make sure %s is installed and in your PATH\n", emu_name);
        _exit(1);
    }

    // Parent process
    g_started_emulator_pid = pid;
    fprintf(stderr, "Emulator: Started with PID %d\n", pid);
    return true;
}

static void stop_emulator() {
    if (g_started_emulator_pid <= 0) return;

    fprintf(stderr, "Emulator: Stopping PID %d\n", g_started_emulator_pid);

    // Try graceful shutdown first via control socket
    bool sent_graceful_stop = false;
    if (g_control_socket >= 0) {
        sent_graceful_stop = send_command(MACEMU_CMD_STOP);
        if (sent_graceful_stop) {
            fprintf(stderr, "Emulator: Sent graceful stop command via socket\n");
        }
    }

    // Wait up to 2 seconds for graceful shutdown
    if (sent_graceful_stop) {
        for (int i = 0; i < 20; i++) {
            int status;
            pid_t result = waitpid(g_started_emulator_pid, &status, WNOHANG);
            if (result != 0) {
                g_started_emulator_pid = -1;
                fprintf(stderr, "Emulator: Stopped gracefully\n");
                return;
            }
            usleep(100000);  // 100ms
        }
        fprintf(stderr, "Emulator: Graceful shutdown timed out, sending SIGTERM\n");
    }

    // Graceful shutdown failed or not attempted - send SIGTERM
    kill(g_started_emulator_pid, SIGTERM);

    // Wait up to 3 seconds for SIGTERM
    for (int i = 0; i < 30; i++) {
        int status;
        pid_t result = waitpid(g_started_emulator_pid, &status, WNOHANG);
        if (result != 0) {
            g_started_emulator_pid = -1;
            fprintf(stderr, "Emulator: Stopped via SIGTERM\n");
            return;
        }
        usleep(100000);  // 100ms
    }

    // Force kill with SIGKILL
    fprintf(stderr, "Emulator: SIGTERM timed out, force killing with SIGKILL\n");
    kill(g_started_emulator_pid, SIGKILL);
    waitpid(g_started_emulator_pid, nullptr, 0);
    g_started_emulator_pid = -1;
    fprintf(stderr, "Emulator: Force killed\n");
}

// Returns: 0 if still running, -1 if not running/error, positive if exited with code
static int check_emulator_status() {
    if (g_started_emulator_pid <= 0) return -1;

    int status;
    pid_t result = waitpid(g_started_emulator_pid, &status, WNOHANG);
    if (result > 0) {
        // Emulator exited
        int exit_code = -1;
        if (WIFEXITED(status)) {
            exit_code = WEXITSTATUS(status);
            fprintf(stderr, "Emulator: Exited with code %d\n", exit_code);
            if (exit_code == 75) {
                fprintf(stderr, "Emulator: Restart requested (exit code 75)\n");
            }
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "Emulator: Killed by signal %d\n", WTERMSIG(status));
        }
        g_started_emulator_pid = -1;
        // disconnect_from_emulator() will set g_emulator_connected to false
        disconnect_from_emulator();
        return exit_code >= 0 ? exit_code : -1;
    } else if (result == 0) {
        return 0;  // Still running
    }
    return -1;  // Error
}


/*
 * Storage and prefs management
 *
 * Wrapper functions for compatibility - delegate to storage modules
 * TODO: Phase 7 - Remove wrappers and call modules directly
 */

// Wrapper: Create minimal prefs if needed
static void create_minimal_prefs_if_needed() {
    storage::create_minimal_prefs_if_needed(g_prefs_path);
}


/*
 * HTTP Server (wrapper for http:: modules)
 *
 * TODO: Phase 7 - Remove wrapper and use http:: modules directly
 */

class HTTPServer {
public:
    HTTPServer() {
        // Initialize API context (will be populated before start())
        api_ctx_.debug_connection = false;
        api_ctx_.debug_mode_switch = false;
        api_ctx_.server_codec = &g_server_codec;  // Point to global codec
        api_ctx_.debug_perf = false;
        api_ctx_.emulator_connected = false;
        api_ctx_.emulator_pid = -1;
        api_ctx_.started_emulator_pid = -1;
        api_ctx_.ipc_shm = nullptr;
    }

    bool start(int port) {
        // Populate API context with current state
        update_api_context();

        // Create API router and static file handler
        api_router_ = std::make_unique<http::APIRouter>(&api_ctx_);

        // Client files are in ~/.macemu/client/
        const char* home = getenv("HOME");
        if (!home) {
            struct passwd* pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : "/tmp";
        }
        std::string client_path = std::string(home) + "/.macemu/client";
        static_handler_ = std::make_unique<http::StaticFileHandler>(client_path);

        // Start HTTP server with combined request handler
        auto handler = [this](const http::Request& req) -> http::Response {
            // Update context before each request (for dynamic state)
            update_api_context();

            // Try API routes first
            bool handled = false;
            http::Response resp = api_router_->handle(req, &handled);
            if (handled) {
                return resp;
            }

            // Try static files
            if (static_handler_->handles(req.path)) {
                return static_handler_->serve(req.path);
            }

            // Not found
            return http::Response::not_found();
        };

        return server_.start(port, handler);
    }

    void stop() {
        server_.stop();
    }

    // Set WebRTC server reference for codec change notifications
    // Implementation after WebRTCServer class definition
    void set_webrtc_server(WebRTCServer* webrtc);

private:
    // Populate API context with current state (called before handling requests)
    void update_api_context() {
        api_ctx_.debug_connection = g_debug_connection;
        api_ctx_.debug_mode_switch = g_debug_mode_switch;
        api_ctx_.debug_perf = g_debug_perf;
        api_ctx_.prefs_path = g_prefs_path;
        api_ctx_.roms_path = g_roms_path;
        api_ctx_.images_path = g_images_path;
        api_ctx_.emulator_connected = g_emulator_connected;
        api_ctx_.emulator_pid = g_emulator_pid;
        api_ctx_.started_emulator_pid = g_started_emulator_pid;
        api_ctx_.ipc_shm = g_ipc_shm;
        api_ctx_.user_stopped_emulator = &g_user_stopped_emulator;

        // Codec state (set once in constructor)
        // api_ctx_.server_codec and notify_codec_change_fn are already set

        // Set command callbacks
        api_ctx_.send_command_fn = [](uint8_t cmd) { send_command(cmd); };
        api_ctx_.start_emulator_fn = []() { return start_emulator(); };
        api_ctx_.stop_emulator_fn = []() { stop_emulator(); };
        api_ctx_.disconnect_emulator_fn = []() { disconnect_from_emulator(); };
        api_ctx_.request_restart_fn = [](bool val) { g_restart_emulator_requested = val; };
    }

    http::Server server_;
    http::APIContext api_ctx_;
    std::unique_ptr<http::APIRouter> api_router_;
    std::unique_ptr<http::StaticFileHandler> static_handler_;
};


/*
 * WebRTC Peer Connection
 */

struct PeerConnection {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video_track;
    std::shared_ptr<rtc::Track> audio_track;
    std::shared_ptr<rtc::DataChannel> data_channel;
    std::string id;
    CodecType codec = CodecType::H264;  // Codec type for this peer
    bool ready = false;
    bool needs_first_frame = true;  // Still-image peers (PNG/WebP) need full first frame
    // Note: pending_candidates removed - libdatachannel queues candidates internally
};


/*
 * WebRTC Server
 */

class WebRTCServer {
public:
    bool init(int signaling_port) {
        port_ = signaling_port;

        rtc::InitLogger(rtc::LogLevel::Error);
        rtc::Preload();

        try {
            rtc::WebSocketServer::Configuration config;
            config.port = signaling_port;
            config.enableTls = false;

            ws_server_ = std::make_unique<rtc::WebSocketServer>(config);

            ws_server_->onClient([this](std::shared_ptr<rtc::WebSocket> ws) {
                // Store WebSocket shared_ptr so we can send messages to it later
                {
                    std::lock_guard<std::mutex> lock(peers_mutex_);
                    ws_connections_[ws.get()] = ws;
                }

                ws->onOpen([ws]() {
                    std::string welcome = "{\"type\":\"welcome\",\"peerId\":\"server\"}";
                    ws->send(welcome);
                });

                ws->onMessage([this, ws](auto data) {
                    if (std::holds_alternative<std::string>(data)) {
                        process_signaling(ws, std::get<std::string>(data));
                    }
                });

                ws->onError([](std::string error) {
                    fprintf(stderr, "[WebRTC] WebSocket error: %s\n", error.c_str());
                });

                ws->onClosed([this, ws]() {
                    std::lock_guard<std::mutex> lock(peers_mutex_);
                    auto it = ws_to_peer_id_.find(ws.get());
                    if (it != ws_to_peer_id_.end()) {
                        peers_.erase(it->second);
                        ws_to_peer_id_.erase(it);
                        peer_count_--;
                        fprintf(stderr, "[WebRTC] Peer disconnected, %d remaining\n", peer_count_.load());
                    }
                    // Remove from WebSocket connections map
                    ws_connections_.erase(ws.get());
                });
            });

            initialized_ = true;
            fprintf(stderr, "WebRTC: Signaling server on port %d\n", signaling_port);

        } catch (const std::exception& e) {
            fprintf(stderr, "WebRTC: Failed to start server: %s\n", e.what());
            return false;
        }

        return true;
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        peers_.clear();
        ws_to_peer_id_.clear();
        ws_server_.reset();
        initialized_ = false;
    }

    // Send H.264 frame via RTP video track
    void send_h264_frame(const EncodedFrame& frame) {
        if (frame.data.empty() || peer_count_ == 0) return;

        std::lock_guard<std::mutex> lock(peers_mutex_);

        // Calculate frame timestamp using chrono for precise timing
        // H264 clock rate is 90000 Hz
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - start_time_);

        // Use the sendFrame method with FrameInfo for proper RTP timestamps
        rtc::FrameInfo frameInfo(elapsed);

        // Log only IDR frame sends (P frames logged in stats summary)
        if (frame.is_keyframe) {
            fprintf(stderr, "[WebRTC] Sending IDR frame: %zu bytes\n", frame.data.size());
        }

        // T7: Capture send timestamp right before sending
        struct timespec ts_send;
        clock_gettime(CLOCK_REALTIME, &ts_send);
        uint64_t t7_server_send_us = (uint64_t)ts_send.tv_sec * 1000000 + ts_send.tv_nsec / 1000;

        // Encode metadata header for data channel (all metadata in one message)
        // Format: [cursor_x:2][cursor_y:2][cursor_visible:1][ping_seq:4][t1:8][t2:8][t3:8][t4:8][t5:8][t6:8][t7:8]
        uint8_t metadata[65];  // 5 cursor + 4 seq + 7*8 timestamps = 65 bytes
        metadata[0] = frame.cursor_x & 0xFF;
        metadata[1] = (frame.cursor_x >> 8) & 0xFF;
        metadata[2] = frame.cursor_y & 0xFF;
        metadata[3] = (frame.cursor_y >> 8) & 0xFF;
        metadata[4] = frame.cursor_visible;
        std::memcpy(&metadata[5], &frame.ping_sequence, 4);
        std::memcpy(&metadata[9], &frame.t1_browser_ms, 8);
        std::memcpy(&metadata[17], &frame.t2_server_us, 8);
        std::memcpy(&metadata[25], &frame.t3_emulator_us, 8);
        std::memcpy(&metadata[33], &frame.t4_frame_us, 8);
        std::memcpy(&metadata[41], &frame.t5_server_read_us, 8);
        std::memcpy(&metadata[49], &frame.t6_encode_done_us, 8);
        std::memcpy(&metadata[57], &t7_server_send_us, 8);

        for (auto& [id, peer] : peers_) {
            if (peer->codec != CodecType::H264) continue;  // Skip non-H264 peers
            if (peer->ready && peer->video_track && peer->video_track->isOpen()) {
                try {
                    // Send video frame via RTP video track
                    peer->video_track->sendFrame(
                        reinterpret_cast<const std::byte*>(frame.data.data()),
                        frame.data.size(),
                        frameInfo);

                    // Send metadata via data channel
                    if (peer->data_channel && peer->data_channel->isOpen()) {
                        peer->data_channel->send(reinterpret_cast<const std::byte*>(metadata), sizeof(metadata));
                    }
                } catch (const std::exception& e) {
                    fprintf(stderr, "[WebRTC] Send error to %s: %s\n", id.c_str(), e.what());
                }
            }
        }
    }

    // Send AV1 frame via RTP video track
    void send_av1_frame(const EncodedFrame& frame) {
        if (frame.data.empty() || peer_count_ == 0) return;

        std::lock_guard<std::mutex> lock(peers_mutex_);

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - start_time_);
        rtc::FrameInfo frameInfo(elapsed);

        if (frame.is_keyframe) {
            fprintf(stderr, "[WebRTC] Sending AV1 keyframe: %zu bytes\n", frame.data.size());
        }

        // T7: Capture send timestamp right before sending
        struct timespec ts_send;
        clock_gettime(CLOCK_REALTIME, &ts_send);
        uint64_t t7_server_send_us = (uint64_t)ts_send.tv_sec * 1000000 + ts_send.tv_nsec / 1000;

        // Encode metadata header for data channel (all metadata in one message)
        // Format: [cursor_x:2][cursor_y:2][cursor_visible:1][ping_seq:4][t1:8][t2:8][t3:8][t4:8][t5:8][t6:8][t7:8]
        uint8_t metadata[65];  // 5 cursor + 4 seq + 7*8 timestamps = 65 bytes
        metadata[0] = frame.cursor_x & 0xFF;
        metadata[1] = (frame.cursor_x >> 8) & 0xFF;
        metadata[2] = frame.cursor_y & 0xFF;
        metadata[3] = (frame.cursor_y >> 8) & 0xFF;
        metadata[4] = frame.cursor_visible;
        std::memcpy(&metadata[5], &frame.ping_sequence, 4);
        std::memcpy(&metadata[9], &frame.t1_browser_ms, 8);
        std::memcpy(&metadata[17], &frame.t2_server_us, 8);
        std::memcpy(&metadata[25], &frame.t3_emulator_us, 8);
        std::memcpy(&metadata[33], &frame.t4_frame_us, 8);
        std::memcpy(&metadata[41], &frame.t5_server_read_us, 8);
        std::memcpy(&metadata[49], &frame.t6_encode_done_us, 8);
        std::memcpy(&metadata[57], &t7_server_send_us, 8);

        for (auto& [id, peer] : peers_) {
            if (peer->codec != CodecType::AV1) continue;
            if (peer->ready && peer->video_track && peer->video_track->isOpen()) {
                try {
                    // Send video frame via RTP video track
                    peer->video_track->sendFrame(
                        reinterpret_cast<const std::byte*>(frame.data.data()),
                        frame.data.size(),
                        frameInfo);

                    // Send metadata via data channel
                    if (peer->data_channel && peer->data_channel->isOpen()) {
                        peer->data_channel->send(reinterpret_cast<const std::byte*>(metadata), sizeof(metadata));
                    }
                } catch (const std::exception& e) {
                    fprintf(stderr, "[WebRTC] AV1 send error to %s: %s\n", id.c_str(), e.what());
                }
            }
        }
    }

    // Send VP9 frame via RTP video track
    void send_vp9_frame(const EncodedFrame& frame) {
        if (frame.data.empty() || peer_count_ == 0) return;

        std::lock_guard<std::mutex> lock(peers_mutex_);

        // Calculate frame timestamp using chrono for precise timing
        // VP9 clock rate is 90000 Hz
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - start_time_);

        // Use the sendFrame method with FrameInfo for proper RTP timestamps
        rtc::FrameInfo frameInfo(elapsed);

        // Log only keyframe sends
        if (frame.is_keyframe) {
            fprintf(stderr, "[WebRTC] Sending VP9 keyframe: %zu bytes\n", frame.data.size());
        }

        // T7: Capture send timestamp right before sending
        struct timespec ts_send;
        clock_gettime(CLOCK_REALTIME, &ts_send);
        uint64_t t7_server_send_us = (uint64_t)ts_send.tv_sec * 1000000 + ts_send.tv_nsec / 1000;

        // Encode metadata header for data channel (all metadata in one message)
        // Format: [cursor_x:2][cursor_y:2][cursor_visible:1][ping_seq:4][t1:8][t2:8][t3:8][t4:8][t5:8][t6:8][t7:8]
        uint8_t metadata[65];  // 5 cursor + 4 seq + 7*8 timestamps = 65 bytes
        metadata[0] = frame.cursor_x & 0xFF;
        metadata[1] = (frame.cursor_x >> 8) & 0xFF;
        metadata[2] = frame.cursor_y & 0xFF;
        metadata[3] = (frame.cursor_y >> 8) & 0xFF;
        metadata[4] = frame.cursor_visible;
        std::memcpy(&metadata[5], &frame.ping_sequence, 4);
        std::memcpy(&metadata[9], &frame.t1_browser_ms, 8);
        std::memcpy(&metadata[17], &frame.t2_server_us, 8);
        std::memcpy(&metadata[25], &frame.t3_emulator_us, 8);
        std::memcpy(&metadata[33], &frame.t4_frame_us, 8);
        std::memcpy(&metadata[41], &frame.t5_server_read_us, 8);
        std::memcpy(&metadata[49], &frame.t6_encode_done_us, 8);
        std::memcpy(&metadata[57], &t7_server_send_us, 8);

        for (auto& [id, peer] : peers_) {
            if (peer->codec != CodecType::VP9) continue;  // Skip non-VP9 peers
            if (peer->ready && peer->video_track && peer->video_track->isOpen()) {
                try {
                    // Send video frame via RTP video track
                    peer->video_track->sendFrame(
                        reinterpret_cast<const std::byte*>(frame.data.data()),
                        frame.data.size(),
                        frameInfo);

                    // Send metadata via data channel
                    if (peer->data_channel && peer->data_channel->isOpen()) {
                        peer->data_channel->send(reinterpret_cast<const std::byte*>(metadata), sizeof(metadata));
                    }
                } catch (const std::exception& e) {
                    fprintf(stderr, "[WebRTC] VP9 send error to %s: %s\n", id.c_str(), e.what());
                }
            }
        }
    }

    // Send Opus audio frame via RTP audio track
    void send_audio_to_all_peers(const std::vector<uint8_t>& opus_data, uint64_t frame_number) {
        if (opus_data.empty()) {
            if (g_debug_audio) {
                fprintf(stderr, "[WebRTC] Audio: Skipping empty opus_data\n");
            }
            return;
        }

        if (peer_count_ == 0) {
            if (g_debug_audio) {
                static int warn_count = 0;
                if (warn_count++ % 50 == 0) {
                    fprintf(stderr, "[WebRTC] Audio: No peers connected (%zu bytes Opus ready)\n", opus_data.size());
                }
            }
            return;
        }

        std::lock_guard<std::mutex> lock(peers_mutex_);

        // Calculate exact sample time based on frame number
        // Each frame is exactly 20ms (960 samples at 48kHz)
        // This ensures perfect timing regardless of jitter in the audio loop
        auto elapsed = std::chrono::duration<double>(frame_number * 0.020); // 20ms = 0.020 seconds

        // Create FrameInfo with exact sample time
        // The RtcpSrReporter will convert this to RTP timestamps at 48kHz clock rate
        rtc::FrameInfo frameInfo(elapsed);

        int sent_count = 0;
        int track_closed = 0;
        int track_missing = 0;

        for (auto& [id, peer] : peers_) {
            if (!peer->audio_track) {
                track_missing++;
                continue;
            }

            if (!peer->audio_track->isOpen()) {
                track_closed++;
                continue;
            }

            try {
                // Send raw Opus packet - RTP packetizer chain handles:
                // 1. OpusRtpPacketizer: Wraps Opus data in RTP packet
                // 2. RtcpSrReporter: Generates sender reports with proper timestamps
                // 3. RtcpNackResponder: Handles retransmission requests
                peer->audio_track->sendFrame(
                    reinterpret_cast<const std::byte*>(opus_data.data()),
                    opus_data.size(),
                    frameInfo);
                sent_count++;
            } catch (const std::exception& e) {
                fprintf(stderr, "[WebRTC] Audio send error to %s: %s\n", id.c_str(), e.what());
            }
        }

        if (g_debug_audio) {
            static int frame_count = 0;
            if (frame_count++ % 50 == 0) {
                fprintf(stderr, "[WebRTC] Audio sent: %zu bytes to %d peers (%d track_missing, %d track_closed)\n",
                        opus_data.size(), sent_count, track_missing, track_closed);
            }
        }
    }

    // Capture trigger removed - was debug feature

    // Send PNG frame via DataChannel (binary) with metadata header
    // Frame format: [8-byte t1_frame_ready] [4-byte x] [4-byte y] [4-byte width] [4-byte height]
    //               [4-byte frame_width] [4-byte frame_height] [8-byte t4_send_time]
    //               [2-byte cursor_x] [2-byte cursor_y] [1-byte cursor_visible]
    //               [4-byte ping_seq] [8-byte ping_t1] [8-byte ping_t2] [8-byte ping_t3]
    //               [8-byte ping_t4] [8-byte ping_t5] [8-byte ping_t6] [8-byte ping_t7] [PNG data]
    //   Total header: 113 bytes (40 base + 5 cursor + 68 ping with 7 timestamps)
    //   All values are little-endian uint32/uint64
    //   Complete ping/pong round-trip timestamps:
    //     t1 = browser send (performance.now ms)
    //     t2 = server receive (CLOCK_REALTIME us)
    //     t3 = emulator receive (CLOCK_REALTIME us)
    //     t4 = frame ready in SHM (CLOCK_REALTIME us)
    //     t5 = server read from SHM (CLOCK_REALTIME us)
    //     t6 = encoding finished (CLOCK_REALTIME us)
    //     t7 = server sending (CLOCK_REALTIME us)
    void send_png_frame(const EncodedFrame& encoded_frame, uint64_t t1_frame_ready_ms,
                        uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                        uint32_t frame_width, uint32_t frame_height) {
        if (encoded_frame.data.empty() || peer_count_ == 0) return;

        // Sanity check: PNG data shouldn't be larger than 10MB for 1920x1080
        if (encoded_frame.data.size() > 10 * 1024 * 1024) {
            fprintf(stderr, "[WebRTC] ERROR: PNG data size %zu is too large, skipping frame\n", encoded_frame.data.size());
            return;
        }

        // T4: Capture timestamp right before sending (Unix epoch milliseconds)
        // Use time() * 1000 + microseconds to get accurate Unix epoch timestamp
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t t4_send_ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

        // T7: Capture server send timestamp (for ping/pong)
        uint64_t t7_server_send_us = t4_send_ms * 1000;  // Convert ms to us

        // Build frame with metadata header (113 bytes total: 40 base + 5 cursor + 68 ping)
        std::vector<uint8_t> frame_with_header;
        try {
            frame_with_header.resize(113 + encoded_frame.data.size());
        } catch (const std::bad_alloc& e) {
            fprintf(stderr, "[WebRTC] ERROR: Failed to allocate %zu bytes for frame header\n", 113 + encoded_frame.data.size());
            return;
        }

        // T1: 8-byte emulator frame ready time (from SHM)
        for (int i = 0; i < 8; i++) {
            frame_with_header[i] = (t1_frame_ready_ms >> (i * 8)) & 0xFF;
        }
        // 4-byte x coordinate (dirty rect)
        for (int i = 0; i < 4; i++) {
            frame_with_header[8 + i] = (x >> (i * 8)) & 0xFF;
        }
        // 4-byte y coordinate (dirty rect)
        for (int i = 0; i < 4; i++) {
            frame_with_header[12 + i] = (y >> (i * 8)) & 0xFF;
        }
        // 4-byte width (dirty rect)
        for (int i = 0; i < 4; i++) {
            frame_with_header[16 + i] = (width >> (i * 8)) & 0xFF;
        }
        // 4-byte height (dirty rect)
        for (int i = 0; i < 4; i++) {
            frame_with_header[20 + i] = (height >> (i * 8)) & 0xFF;
        }
        // 4-byte frame width (full resolution)
        for (int i = 0; i < 4; i++) {
            frame_with_header[24 + i] = (frame_width >> (i * 8)) & 0xFF;
        }
        // 4-byte frame height (full resolution)
        for (int i = 0; i < 4; i++) {
            frame_with_header[28 + i] = (frame_height >> (i * 8)) & 0xFF;
        }
        // T4: 8-byte server send time
        for (int i = 0; i < 8; i++) {
            frame_with_header[32 + i] = (t4_send_ms >> (i * 8)) & 0xFF;
        }
        // Cursor position (5 bytes: x, y, visible)
        // 2-byte cursor_x
        frame_with_header[40] = encoded_frame.cursor_x & 0xFF;
        frame_with_header[41] = (encoded_frame.cursor_x >> 8) & 0xFF;
        // 2-byte cursor_y
        frame_with_header[42] = encoded_frame.cursor_y & 0xFF;
        frame_with_header[43] = (encoded_frame.cursor_y >> 8) & 0xFF;
        // 1-byte cursor_visible
        frame_with_header[44] = encoded_frame.cursor_visible;

        // Ping echo with all timestamps (44 bytes: sequence + 5 timestamps)
        // 4-byte ping sequence number (0 if no ping received)
        for (int i = 0; i < 4; i++) {
            frame_with_header[45 + i] = (encoded_frame.ping_sequence >> (i * 8)) & 0xFF;
        }
        // 8-byte t1: browser send time (performance.now() milliseconds)
        for (int i = 0; i < 8; i++) {
            frame_with_header[49 + i] = (encoded_frame.t1_browser_ms >> (i * 8)) & 0xFF;
        }
        // 8-byte t2: server receive time (CLOCK_REALTIME microseconds)
        for (int i = 0; i < 8; i++) {
            frame_with_header[57 + i] = (encoded_frame.t2_server_us >> (i * 8)) & 0xFF;
        }
        // 8-byte t3: emulator receive time (CLOCK_REALTIME microseconds)
        for (int i = 0; i < 8; i++) {
            frame_with_header[65 + i] = (encoded_frame.t3_emulator_us >> (i * 8)) & 0xFF;
        }
        // 8-byte t4: emulator/frame ready time (CLOCK_REALTIME microseconds)
        for (int i = 0; i < 8; i++) {
            frame_with_header[73 + i] = (encoded_frame.t4_frame_us >> (i * 8)) & 0xFF;
        }
        // 8-byte t5: server read from SHM (CLOCK_REALTIME microseconds)
        for (int i = 0; i < 8; i++) {
            frame_with_header[81 + i] = (encoded_frame.t5_server_read_us >> (i * 8)) & 0xFF;
        }
        // 8-byte t6: encoding finished (CLOCK_REALTIME microseconds)
        for (int i = 0; i < 8; i++) {
            frame_with_header[89 + i] = (encoded_frame.t6_encode_done_us >> (i * 8)) & 0xFF;
        }
        // 8-byte t7: server sending (CLOCK_REALTIME microseconds)
        for (int i = 0; i < 8; i++) {
            frame_with_header[97 + i] = (t7_server_send_us >> (i * 8)) & 0xFF;
        }
        // Copy PNG data after 113-byte header (was 105, now 113)
        memcpy(frame_with_header.data() + 113, encoded_frame.data.data(), encoded_frame.data.size());

        std::lock_guard<std::mutex> lock(peers_mutex_);

        for (auto& [id, peer] : peers_) {
            // Skip non-still-image peers (only PNG/WebP use DataChannel for video)
            if (peer->codec != CodecType::PNG && peer->codec != CodecType::WEBP) continue;
            if (peer->ready && peer->data_channel && peer->data_channel->isOpen()) {
                try {
                    peer->data_channel->send(
                        reinterpret_cast<const std::byte*>(frame_with_header.data()),
                        frame_with_header.size());
                } catch (const std::exception& e) {
                    fprintf(stderr, "[WebRTC] PNG send error to %s: %s\n", id.c_str(), e.what());
                }
            }
        }
    }

    // Populate frame metadata from shared memory (cursor + ping/pong data)
    // Called after encoding, populates metadata fields in EncodedFrame
    // t5_read = server read timestamp, t6_encode = encode done timestamp
    void populate_frame_metadata(EncodedFrame& frame, uint64_t t5_read_us, uint64_t t6_encode_us) {
        if (!g_ipc.is_connected() || !g_ipc_shm) return;

        // Cursor position (always available)
        frame.cursor_x = g_ipc_shm->cursor_x;
        frame.cursor_y = g_ipc_shm->cursor_y;
        frame.cursor_visible = g_ipc_shm->cursor_visible;

        // Ping/pong timestamps (atomic read-acquire for sequence number)
        frame.ping_sequence = ATOMIC_LOAD(g_ipc_shm->ping_sequence);

        // If ping available, read timestamp struct (no atomics needed - seq acts as guard)
        if (frame.ping_sequence > 0) {
            frame.t1_browser_ms = g_ipc_shm->ping_timestamps.t1_browser_ms;
            frame.t2_server_us = g_ipc_shm->ping_timestamps.t2_server_us;
            frame.t3_emulator_us = g_ipc_shm->ping_timestamps.t3_emulator_us;
            frame.t4_frame_us = g_ipc_shm->ping_timestamps.t4_frame_us;
            frame.t5_server_read_us = t5_read_us;      // Server read time (passed in)
            frame.t6_encode_done_us = t6_encode_us;    // Encode done time (passed in)
            // t7_server_send_us will be set in send functions right before sending
        }
    }

    // Check if any peer uses a specific codec
    bool has_codec_peer(CodecType codec) {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& [id, peer] : peers_) {
            if (peer->codec == codec && peer->ready) {
                if (g_debug_frames && codec == CodecType::H264) {
                    static bool logged_once = false;
                    if (!logged_once) {
                        fprintf(stderr, "[Server] H.264 peer %s is ready\n", id.c_str());
                        logged_once = true;
                    }
                }
                return true;
            }
        }
        if (g_debug_frames && codec == CodecType::H264) {
            static int no_peer_count = 0;
            if (no_peer_count++ < 5) {
                fprintf(stderr, "[Server] No ready H.264 peer (total peers: %zu)\n", peers_.size());
            }
        }
        return false;
    }

    // Check if any PNG peer needs their first frame, and clear the flag if so
    bool png_peer_needs_first_frame() {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& [id, peer] : peers_) {
            // Check if still-image peer (PNG or WebP) needs first frame
            if ((peer->codec == CodecType::PNG || peer->codec == CodecType::WEBP) && peer->ready && peer->needs_first_frame) {
                peer->needs_first_frame = false;  // Clear flag so next frame uses dirty rect
                return true;
            }
        }
        return false;
    }

    // Get codec peer counts in one lock (more efficient for frame loop)
    struct CodecPeerCounts {
        int h264 = 0;
        int av1 = 0;
        int vp9 = 0;
        int png = 0;
        int webp = 0;
    };

    CodecPeerCounts get_codec_peer_counts() {
        CodecPeerCounts counts;
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& [id, peer] : peers_) {
            if (!peer->ready) continue;
            switch (peer->codec) {
                case CodecType::H264: counts.h264++; break;
                case CodecType::AV1: counts.av1++; break;
                case CodecType::VP9: counts.vp9++; break;
                case CodecType::PNG: counts.png++; break;
                case CodecType::WEBP: counts.webp++; break;
            }
        }
        return counts;
    }

    int peer_count() { return peer_count_.load(); }
    bool is_enabled() { return initialized_.load(); }

    // Disconnect all peers (e.g., when emulator restarts and resolution changes)
    void disconnect_all_peers() {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        fprintf(stderr, "[WebRTC] Disconnecting all peers (%d) due to emulator restart\n", (int)peers_.size());

        // Explicitly close all peer connections to trigger browser reconnections
        for (auto& pair : peers_) {
            auto& peer = pair.second;
            if (peer->pc) {
                peer->pc->close();
            }
            if (peer->data_channel) {
                peer->data_channel->close();
            }
            if (peer->video_track) {
                peer->video_track->close();
            }
        }

        peers_.clear();
        ws_to_peer_id_.clear();
        peer_count_.store(0);
    }

    // Send "reconnect" message to all clients (for codec changes)
    void notify_codec_change(CodecType new_codec) {
        const char* codec_name = (new_codec == CodecType::H264) ? "h264" :
                                 (new_codec == CodecType::AV1) ? "av1" :
                                 (new_codec == CodecType::VP9) ? "vp9" :
                                 (new_codec == CodecType::WEBP) ? "webp" : "png";

        json msg = {
            {"type", "reconnect"},
            {"reason", "codec_change"},
            {"codec", codec_name}
        };
        std::string msg_str = msg.dump();

        fprintf(stderr, "[WebRTC] Notifying %d clients of codec change to %s\n",
                (int)ws_connections_.size(), codec_name);

        // Send to all connected WebSocket clients
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& pair : ws_connections_) {
            auto& ws = pair.second;
            if (ws) {
                try {
                    ws->send(msg_str);
                } catch (const std::exception& e) {
                    fprintf(stderr, "[WebRTC] Failed to send reconnect message: %s\n", e.what());
                }
            }
        }
    }

private:
    // Helper: Check if codec needs RTP video track (vs DataChannel)
    bool needs_video_track(CodecType codec) {
        // H.264, AV1, and VP9 all use RTP video tracks
        // PNG uses DataChannel for dirty-rect support
        return codec == CodecType::H264 || codec == CodecType::AV1 || codec == CodecType::VP9;
    }

    // Helper: Setup AV1 video track with RTP packetizer
    void setup_av1_track(std::shared_ptr<PeerConnection> peer) {
        rtc::Description::Video media("video-stream", rtc::Description::Direction::SendOnly);
        media.addAV1Codec(96);  // AV1 uses payload type 96
        media.addSSRC(ssrc_, "video-stream", "stream1", "video-stream");
        peer->video_track = peer->pc->addTrack(media);

        // Set up AV1 RTP packetizer (handles OBU fragmentation)
        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc_, "video-stream", 96, rtc::AV1RtpPacketizer::ClockRate
        );
        auto packetizer = std::make_shared<rtc::AV1RtpPacketizer>(
            rtc::AV1RtpPacketizer::Packetization::TemporalUnit,
            rtpConfig
        );
        peer->video_track->setMediaHandler(packetizer);

        peer->video_track->onOpen([peer]() {
            if (g_debug_connection) {
                fprintf(stderr, "[WebRTC] Video track OPEN for %s - ready to send frames!\n", peer->id.c_str());
            }
            peer->ready = true;
            g_request_keyframe.store(true);
        });

        peer->video_track->onClosed([peer]() {
            fprintf(stderr, "[WebRTC] Video track CLOSED for %s\n", peer->id.c_str());
            peer->ready = false;
        });

        peer->video_track->onError([peer](std::string error) {
            fprintf(stderr, "[WebRTC] Video track ERROR for %s: %s\n", peer->id.c_str(), error.c_str());
        });
    }

    // Helper: Setup VP9 video track with RTP packetizer
    void setup_vp9_track(std::shared_ptr<PeerConnection> peer) {
        rtc::Description::Video media("video-stream", rtc::Description::Direction::SendOnly);
        media.addVP9Codec(96);  // VP9 uses payload type 96
        media.addSSRC(ssrc_, "video-stream", "stream1", "video-stream");
        peer->video_track = peer->pc->addTrack(media);

        // Set up VP9 RTP packetizer (handles VP9 frame fragmentation per RFC 7741)
        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc_, "video-stream", 96, rtc::VP9RtpPacketizer::ClockRate
        );
        auto packetizer = std::make_shared<rtc::VP9RtpPacketizer>(rtpConfig);
        peer->video_track->setMediaHandler(packetizer);

        peer->video_track->onOpen([peer]() {
            if (g_debug_connection) {
                fprintf(stderr, "[WebRTC] Video track OPEN for %s - ready to send frames!\n", peer->id.c_str());
            }
            peer->ready = true;
            g_request_keyframe.store(true);
        });

        peer->video_track->onClosed([peer]() {
            fprintf(stderr, "[WebRTC] Video track CLOSED for %s\n", peer->id.c_str());
            peer->ready = false;
        });

        peer->video_track->onError([peer](std::string error) {
            fprintf(stderr, "[WebRTC] Video track ERROR for %s: %s\n", peer->id.c_str(), error.c_str());
        });
    }

    // Helper: Setup H.264 video track with RTP packetizer
    void setup_h264_track(std::shared_ptr<PeerConnection> peer) {
        rtc::Description::Video media("video-stream", rtc::Description::Direction::SendOnly);
        media.addH264Codec(96, "profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1");
        media.addSSRC(ssrc_, "video-stream", "stream1", "video-stream");
        peer->video_track = peer->pc->addTrack(media);

        // Set up H.264 RTP packetizer (handles NAL unit fragmentation)
        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc_, "video-stream", 96, rtc::H264RtpPacketizer::ClockRate
        );
        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::H264RtpPacketizer::Separator::LongStartSequence,
            rtpConfig
        );
        peer->video_track->setMediaHandler(packetizer);

        peer->video_track->onOpen([peer]() {
            if (g_debug_connection) {
                fprintf(stderr, "[WebRTC] Video track OPEN for %s - ready to send frames!\n", peer->id.c_str());
            }
            peer->ready = true;
            // Request keyframe so new peer gets a complete picture
            g_request_keyframe.store(true);
        });

        peer->video_track->onClosed([peer]() {
            fprintf(stderr, "[WebRTC] Video track CLOSED for %s\n", peer->id.c_str());
            peer->ready = false;
        });

        peer->video_track->onError([peer](std::string error) {
            fprintf(stderr, "[WebRTC] Video track ERROR for %s: %s\n", peer->id.c_str(), error.c_str());
        });
    }

    // Helper: Setup Opus audio track with RTP packetizer
    void setup_audio_track(std::shared_ptr<PeerConnection> peer) {
        rtc::Description::Audio media("audio-stream", rtc::Description::Direction::SendOnly);

        // Opus profile built from centralized audio_config.h constants
        // See audio_config.h for tuning: bitrate, frame duration, FEC, etc.
        std::string opusProfile = WEBRTC_OPUS_PROFILE;
        fprintf(stderr, "[Audio] Opus SDP profile: %s\n", opusProfile.c_str());
        media.addOpusCodec(OPUS_PAYLOAD_TYPE, opusProfile);

        media.addSSRC(ssrc_ + 1, "audio-stream", "stream1", "audio-stream");
        peer->audio_track = peer->pc->addTrack(media);

        // Set up Opus RTP packetizer with RTCP support (following libdatachannel best practices)
        // Opus uses 48000 Hz clock rate (defined in OpusRtpPacketizer template parameter)
        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc_ + 1, "audio-stream", OPUS_PAYLOAD_TYPE, rtc::OpusRtpPacketizer::defaultClockRate
        );
        auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);

        // Add RTCP SR (Sender Report) for proper timestamp synchronization
        // This is CRITICAL for browsers to correctly sync and play audio
        auto srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
        packetizer->addToChain(srReporter);

        // Add RTCP NACK (Negative Acknowledgement) responder for packet loss recovery
        // Improves audio quality on lossy networks
        auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
        packetizer->addToChain(nackResponder);

        peer->audio_track->setMediaHandler(packetizer);

        peer->audio_track->onOpen([peer]() {
            if (g_debug_connection) {
                fprintf(stderr, "[WebRTC] Audio track OPEN for %s\n", peer->id.c_str());
            }
        });

        peer->audio_track->onClosed([peer]() {
            fprintf(stderr, "[WebRTC] Audio track CLOSED for %s\n", peer->id.c_str());
        });

        peer->audio_track->onError([peer](std::string error) {
            fprintf(stderr, "[WebRTC] Audio track ERROR for %s: %s\n", peer->id.c_str(), error.c_str());
        });
    }

    void process_signaling(std::shared_ptr<rtc::WebSocket> ws, const std::string& msg) {
        std::string type = json_get_string(msg, "type");

        if (type == "connect") {
            std::string peer_id = "peer_" + std::to_string(rand());
            auto peer = std::make_shared<PeerConnection>();
            peer->id = peer_id;

            // Use server-side codec preference (from prefs file)
            peer->codec = g_server_codec;
            const char* codec_name = (g_server_codec == CodecType::H264) ? "h264" :
                                     (g_server_codec == CodecType::AV1) ? "av1" :
                                     (g_server_codec == CodecType::VP9) ? "vp9" :
                                     (g_server_codec == CodecType::WEBP) ? "webp" : "png";
            if (g_debug_connection) {
                fprintf(stderr, "[WebRTC] Peer %s using %s codec (server-configured)\n",
                        peer_id.c_str(), codec_name);
            }

            rtc::Configuration config;
            // Add STUN server if enabled (disabled by default for localhost/LAN)
            if (g_enable_stun) {
                config.iceServers.emplace_back(g_stun_server);
                if (g_debug_connection) {
                    fprintf(stderr, "[WebRTC] Using STUN server: %s\n", g_stun_server.c_str());
                }
            } else if (g_debug_connection) {
                fprintf(stderr, "[WebRTC] STUN disabled (localhost/LAN mode)\n");
            }
            // Allow large video frames (up to 16MB for high-res dithered content)
            config.maxMessageSize = 16 * 1024 * 1024;

            peer->pc = std::make_shared<rtc::PeerConnection>(config);

            {
                std::lock_guard<std::mutex> lock(peers_mutex_);
                ws_to_peer_id_[ws.get()] = peer_id;
                peers_[peer_id] = peer;
                peer_count_++;
            }

            // Send acknowledgment with codec info so client initializes correct decoder
            std::string ack = "{\"type\":\"connected\",\"peer_id\":\"" + peer_id +
                              "\",\"codec\":\"" + codec_name + "\"}";
            ws->send(ack);

            // onLocalDescription: Called when SDP is ready (replaces manual gathering state check)
            peer->pc->onLocalDescription([ws, peer_id](rtc::Description desc) {
                json msg = {
                    {"type", desc.typeString()},
                    {"sdp", std::string(desc)}
                };
                ws->send(msg.dump());
                if (g_debug_connection) {
                    fprintf(stderr, "[WebRTC] Sent %s to %s (sdp length=%zu)\n",
                            desc.typeString().c_str(), peer_id.c_str(), std::string(desc).size());
                }
            });

            // onLocalCandidate: Automatic trickle ICE (sends candidates as they're gathered)
            peer->pc->onLocalCandidate([ws, peer_id](rtc::Candidate cand) {
                json msg = {
                    {"type", "candidate"},
                    {"candidate", std::string(cand)},
                    {"mid", cand.mid()}
                };
                ws->send(msg.dump());
                if (g_debug_connection) {
                    fprintf(stderr, "[WebRTC] Sent ICE candidate to %s (mid=%s)\n",
                            peer_id.c_str(), cand.mid().c_str());
                }
            });

            // Conditionally create video track based on codec
            if (needs_video_track(peer->codec)) {
                if (peer->codec == CodecType::H264) {
                    setup_h264_track(peer);
                } else if (peer->codec == CodecType::AV1) {
                    setup_av1_track(peer);
                } else if (peer->codec == CodecType::VP9) {
                    setup_vp9_track(peer);
                }
            } else {
                // PNG codec uses DataChannel for video, mark as ready immediately
                // when DataChannel opens (no video track needed)
                if (g_debug_connection) {
                    fprintf(stderr, "[WebRTC] Peer %s using %s codec via DataChannel (no video track)\n",
                            peer_id.c_str(), codec_name);
                }
            }

            // Always setup audio track for all peers
            setup_audio_track(peer);

            peer->pc->onStateChange([peer](rtc::PeerConnection::State state) {
                const char* state_str = "unknown";
                switch (state) {
                    case rtc::PeerConnection::State::New: state_str = "New"; break;
                    case rtc::PeerConnection::State::Connecting: state_str = "Connecting"; break;
                    case rtc::PeerConnection::State::Connected: state_str = "Connected"; break;
                    case rtc::PeerConnection::State::Disconnected: state_str = "Disconnected"; break;
                    case rtc::PeerConnection::State::Failed: state_str = "Failed"; break;
                    case rtc::PeerConnection::State::Closed: state_str = "Closed"; break;
                }
                if (g_debug_connection) {
                    fprintf(stderr, "[WebRTC] Peer %s state: %s\n", peer->id.c_str(), state_str);
                }
            });

            peer->pc->onIceStateChange([peer](rtc::PeerConnection::IceState state) {
                const char* state_str = "unknown";
                switch (state) {
                    case rtc::PeerConnection::IceState::New: state_str = "New"; break;
                    case rtc::PeerConnection::IceState::Checking: state_str = "Checking"; break;
                    case rtc::PeerConnection::IceState::Connected: state_str = "Connected"; break;
                    case rtc::PeerConnection::IceState::Completed: state_str = "Completed"; break;
                    case rtc::PeerConnection::IceState::Disconnected: state_str = "Disconnected"; break;
                    case rtc::PeerConnection::IceState::Failed: state_str = "Failed"; break;
                    case rtc::PeerConnection::IceState::Closed: state_str = "Closed"; break;
                }
                if (g_debug_connection) {
                    fprintf(stderr, "[WebRTC] Peer %s ICE state: %s\n", peer->id.c_str(), state_str);
                }
            });

            // Add data channel for input (and video for still-image codecs)
            // For PNG/WebP: Use unreliable, unordered mode for lowest latency
            // - Each frame is independent (no inter-frame dependencies)
            // - Newer frames make old frames obsolete
            // - No retransmission delays
            rtc::DataChannelInit dc_init;
            if (peer->codec == CodecType::PNG || peer->codec == CodecType::WEBP) {
                dc_init.reliability.unordered = true;
                dc_init.reliability.maxRetransmits = 0;  // No retransmits = unreliable
                if (g_debug_connection) {
                    fprintf(stderr, "[WebRTC] Creating unreliable DataChannel for %s (still-image codec)\n", peer_id.c_str());
                }
            }
            peer->data_channel = peer->pc->createDataChannel("input", dc_init);
            peer->data_channel->onOpen([peer, peer_id = peer->id]() {
                if (g_debug_connection) {
                    fprintf(stderr, "[WebRTC] DataChannel OPEN for %s\n", peer_id.c_str());
                }
                // For still-image codecs (no video track), mark peer as ready when DataChannel opens
                if (peer->codec == CodecType::PNG || peer->codec == CodecType::WEBP) {
                    peer->ready = true;
                    if (g_debug_connection) {
                        fprintf(stderr, "[WebRTC] PNG peer %s marked ready (DataChannel opened)\n", peer_id.c_str());
                    }
                }
            });
            peer->data_channel->onMessage([this](auto data) {
                if (std::holds_alternative<std::string>(data)) {
                    const std::string& msg = std::get<std::string>(data);
                    if (g_debug_connection) {
                        static int msg_count = 0;
                        if (msg_count++ < 5) {
                            fprintf(stderr, "[WebRTC] DataChannel text message: '%s'\n", msg.c_str());
                        }
                    }
                    handle_input_text(msg);
                } else if (std::holds_alternative<rtc::binary>(data)) {
                    const rtc::binary& bin = std::get<rtc::binary>(data);
                    handle_input_binary(reinterpret_cast<const uint8_t*>(bin.data()), bin.size());
                }
            });

            // Also handle incoming data channels from browser
            peer->pc->onDataChannel([this, peer_id = peer->id](std::shared_ptr<rtc::DataChannel> dc) {
                fprintf(stderr, "[WebRTC] Incoming DataChannel '%s' from %s\n",
                        dc->label().c_str(), peer_id.c_str());
                dc->onOpen([label = dc->label(), peer_id]() {
                    fprintf(stderr, "[WebRTC] Incoming DataChannel '%s' OPEN for %s\n",
                            label.c_str(), peer_id.c_str());
                });
                dc->onMessage([this](auto data) {
                    if (std::holds_alternative<std::string>(data)) {
                        handle_input_text(std::get<std::string>(data));
                    } else if (std::holds_alternative<rtc::binary>(data)) {
                        const rtc::binary& bin = std::get<rtc::binary>(data);
                        handle_input_binary(reinterpret_cast<const uint8_t*>(bin.data()), bin.size());
                    }
                });
            });

            peer->pc->setLocalDescription();
        }
        else if (type == "answer") {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            auto it = ws_to_peer_id_.find(ws.get());
            if (it != ws_to_peer_id_.end()) {
                auto peer = peers_[it->second];
                std::string sdp = json_get_string(msg, "sdp");
                if (g_debug_connection) {
                    fprintf(stderr, "[WebRTC] Received answer from %s (sdp length=%zu)\n",
                            peer->id.c_str(), sdp.size());
                }

                // Set remote description - libdatachannel handles pending candidates internally
                try {
                    peer->pc->setRemoteDescription(rtc::Description(sdp, "answer"));
                    if (g_debug_connection) {
                        fprintf(stderr, "[WebRTC] Remote description set for %s\n", peer->id.c_str());
                    }
                } catch (const std::exception& e) {
                    fprintf(stderr, "[WebRTC] ERROR setting remote description for %s: %s\n",
                            peer->id.c_str(), e.what());
                }
            }
        }
        else if (type == "candidate") {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            auto it = ws_to_peer_id_.find(ws.get());
            if (it != ws_to_peer_id_.end()) {
                auto peer = peers_[it->second];
                std::string candidate = json_get_string(msg, "candidate");
                std::string mid = json_get_string(msg, "mid");
                if (!candidate.empty()) {
                    // libdatachannel handles candidate queuing if remote description not set yet
                    if (g_debug_connection) {
                        fprintf(stderr, "[WebRTC] Adding ICE candidate from %s (mid=%s)\n",
                                peer->id.c_str(), mid.c_str());
                    }
                    try {
                        peer->pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
                    } catch (const std::exception& e) {
                        fprintf(stderr, "[WebRTC] Failed to add candidate: %s\n", e.what());
                    }
                }
            }
        }
    }

    // Binary input protocol handler (new, optimized)
    // Format from browser:
    // Mouse move (relative): [type=1:1] [dx:int16] [dy:int16] [timestamp:float64]
    // Mouse button: [type=2:1] [button:uint8] [down:uint8] [timestamp:float64]
    // Key: [type=3:1] [keycode:uint16] [down:uint8] [timestamp:float64]
    // Ping: [type=4:1] [sequence:uint32] [timestamp:float64]
    // Mouse move (absolute): [type=5:1] [x:uint16] [y:uint16] [timestamp:float64]
    // Mouse mode change: [type=6:1] [mode:uint8] (0=absolute, 1=relative)
    void handle_input_binary(const uint8_t* data, size_t len) {
        if (len < 1) return;

        uint8_t type = data[0];
        static uint8_t current_buttons = 0;
        static bool mouse_mode_relative = false;  // Track current mouse mode

        switch (type) {
            case 1: {  // Mouse move (relative)
                if (len < 13) return;  // 1 + 2 + 2 + 8
                int16_t dx = *reinterpret_cast<const int16_t*>(data + 1);
                int16_t dy = *reinterpret_cast<const int16_t*>(data + 3);
                double timestamp = *reinterpret_cast<const double*>(data + 5);
                uint64_t browser_ts = static_cast<uint64_t>(timestamp);
                send_mouse_input(dx, dy, current_buttons, browser_ts, false);  // relative=false for backward compat
                g_mouse_move_count++;
                break;
            }
            case 2: {  // Mouse button
                if (len < 11) return;  // 1 + 1 + 1 + 8
                uint8_t button = data[1];
                uint8_t down = data[2];
                double timestamp = *reinterpret_cast<const double*>(data + 3);
                uint64_t browser_ts = static_cast<uint64_t>(timestamp);

                if (down) {
                    if (button == 0) current_buttons |= MACEMU_MOUSE_LEFT;
                    else if (button == 1) current_buttons |= MACEMU_MOUSE_MIDDLE;
                    else if (button == 2) current_buttons |= MACEMU_MOUSE_RIGHT;
                } else {
                    if (button == 0) current_buttons &= ~MACEMU_MOUSE_LEFT;
                    else if (button == 1) current_buttons &= ~MACEMU_MOUSE_MIDDLE;
                    else if (button == 2) current_buttons &= ~MACEMU_MOUSE_RIGHT;
                }
                send_mouse_input(0, 0, current_buttons, browser_ts);
                g_mouse_click_count++;
                break;
            }
            case 3: {  // Key
                if (len < 12) return;  // 1 + 2 + 1 + 8
                uint16_t keycode = *reinterpret_cast<const uint16_t*>(data + 1);
                uint8_t down = data[3];
                int mac_code = keyboard_map::browser_to_mac_keycode(keycode);
                if (mac_code >= 0) {
                    send_key_input(mac_code, down != 0);
                    g_key_count++;
                }
                break;
            }
            case 4: {  // Ping
                if (len < 13) return;  // 1 + 4 + 8
                uint32_t sequence = *reinterpret_cast<const uint32_t*>(data + 1);
                double timestamp = *reinterpret_cast<const double*>(data + 5);
                uint64_t timestamp_ms = static_cast<uint64_t>(timestamp);
                bool success = send_ping_input(sequence, timestamp_ms);
                if (g_debug_perf) {
                    fprintf(stderr, "[Server] Binary ping #%u (t1=%.1fms) forwarded to emulator: %s\n",
                            sequence, timestamp, success ? "OK" : "FAILED");
                }
                break;
            }
            case 5: {  // Mouse move (absolute)
                if (len < 13) return;  // 1 + 2 + 2 + 8
                uint16_t x = *reinterpret_cast<const uint16_t*>(data + 1);
                uint16_t y = *reinterpret_cast<const uint16_t*>(data + 3);
                double timestamp = *reinterpret_cast<const double*>(data + 5);
                uint64_t browser_ts = static_cast<uint64_t>(timestamp);
                if (g_debug_mouse) {
                    fprintf(stderr, "[Server] Absolute mouse: x=%u, y=%u\n", x, y);
                }
                send_mouse_input(x, y, current_buttons, browser_ts, true);  // absolute=true
                g_mouse_move_count++;
                break;
            }
            case 6: {  // Mouse mode change
                if (len < 2) return;  // 1 + 1
                uint8_t mode = data[1];
                mouse_mode_relative = (mode == 1);
                send_mouse_mode_change(mouse_mode_relative);
                if (g_debug_mouse) {
                    fprintf(stderr, "[Server] Mouse mode changed to: %s\n",
                            mouse_mode_relative ? "relative" : "absolute");
                }
                break;
            }
        }
    }

    // Text input protocol handler (legacy fallback)
    void handle_input_text(const std::string& msg) {
        // Simple text protocol from browser: M dx,dy | D btn | U btn | K code | k code
        // Server converts browser keycodes to Mac keycodes and sends binary to emulator
        if (msg.empty()) return;

        char cmd = msg[0];
        const char* args = msg.c_str() + 1;

        static uint8_t current_buttons = 0;

        switch (cmd) {
            case 'M': {
                // Mouse move: M dx,dy,timestamp
                int dx = 0, dy = 0;
                double ts = 0;
                if (sscanf(args, "%d,%d,%lf", &dx, &dy, &ts) >= 2) {
                    uint64_t browser_ts = static_cast<uint64_t>(ts);
                    send_mouse_input(dx, dy, current_buttons, browser_ts);
                    g_mouse_move_count++;
                }
                break;
            }
            case 'D': {
                // Mouse down: D button,timestamp
                int button = 0;
                double ts = 0;
                sscanf(args, "%d,%lf", &button, &ts);
                if (button == 0) current_buttons |= MACEMU_MOUSE_LEFT;
                else if (button == 1) current_buttons |= MACEMU_MOUSE_MIDDLE;
                else if (button == 2) current_buttons |= MACEMU_MOUSE_RIGHT;
                uint64_t browser_ts = static_cast<uint64_t>(ts);
                send_mouse_input(0, 0, current_buttons, browser_ts);
                g_mouse_click_count++;
                break;
            }
            case 'U': {
                // Mouse up: U button,timestamp
                int button = 0;
                double ts = 0;
                sscanf(args, "%d,%lf", &button, &ts);
                if (button == 0) current_buttons &= ~MACEMU_MOUSE_LEFT;
                else if (button == 1) current_buttons &= ~MACEMU_MOUSE_MIDDLE;
                else if (button == 2) current_buttons &= ~MACEMU_MOUSE_RIGHT;
                uint64_t browser_ts = static_cast<uint64_t>(ts);
                send_mouse_input(0, 0, current_buttons, browser_ts);
                g_mouse_click_count++;
                break;
            }
            case 'K': {
                // Key down: K keycode
                int keycode = atoi(args);
                int mac_code = keyboard_map::browser_to_mac_keycode(keycode);
                if (mac_code >= 0) {
                    send_key_input(mac_code, true);
                    g_key_count++;
                }
                break;
            }
            case 'k': {
                // Key up: k keycode
                int keycode = atoi(args);
                int mac_code = keyboard_map::browser_to_mac_keycode(keycode);
                if (mac_code >= 0) {
                    send_key_input(mac_code, false);
                    g_key_count++;
                }
                break;
            }
            case 'P': {
                // Ping: P sequence,timestamp
                // NOTE: Ping responses are only sent in PNG codec mode via DataChannel metadata header.
                // H.264 uses RTP video track with no metadata support - ping echoes would be discarded.
                uint32_t sequence = 0;
                double ts = 0;
                if (sscanf(args, "%u,%lf", &sequence, &ts) == 2) {
                    uint64_t timestamp_ms = static_cast<uint64_t>(ts);
                    bool success = send_ping_input(sequence, timestamp_ms);
                    if (g_debug_perf) {
                        fprintf(stderr, "[Server] Ping #%u (t1=%.1fms) forwarded to emulator: %s\n",
                                sequence, ts, success ? "OK" : "FAILED");
                    }
                } else {
                    fprintf(stderr, "[Server] ERROR: Failed to parse ping message: '%s'\n", args);
                }
                break;
            }
        }
    }

    std::atomic<bool> initialized_{false};
    std::atomic<int> peer_count_{0};

    int port_ = 8090;
    std::unique_ptr<rtc::WebSocketServer> ws_server_;
    std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();

    std::mutex peers_mutex_;
    std::map<std::string, std::shared_ptr<PeerConnection>> peers_;
    std::map<rtc::WebSocket*, std::string> ws_to_peer_id_;
    std::map<rtc::WebSocket*, std::shared_ptr<rtc::WebSocket>> ws_connections_;  // Keep WebSocket alive

    uint32_t ssrc_ = 1;
};

// Stdin monitor removed - was debug feature for synchronized audio capture
// Capture functionality can be triggered via API endpoint if needed in future

// Track last disconnection time to prevent immediate reconnection
static std::chrono::steady_clock::time_point g_last_disconnect_time;

// Implementation of disconnect_from_emulator (needs WebRTCServer definition)
// NOTE: With new threading model, this is called from connection manager (main thread)
// Worker threads (video/audio) check g_emulator_connected and go idle when false
static void disconnect_from_emulator(WebRTCServer* webrtc, bool disconnect_peers) {
    // Set disconnected flag - worker threads will see this and stop processing
    g_emulator_connected = false;
    g_emulator_pid = -1;

    // Record disconnection time for reconnection grace period
    g_last_disconnect_time = std::chrono::steady_clock::now();

    // Wait for worker threads to notice flag and go idle
    // Audio loop: 20ms iterations, Video loop: blocks on epoll_wait with 5ms timeout
    // 200ms ensures both threads have fully exited their processing loops
    fprintf(stderr, "IPC: Waiting for worker threads to stop accessing shared memory...\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Now safe to unmap - threads are idle waiting on g_emulator_connected check
    g_ipc.disconnect();
    fprintf(stderr, "IPC: Disconnected and unmapped shared memory\n");

    // Disconnect WebRTC peers if requested (e.g., when codec changes)
    // Otherwise, keep peers connected for seamless restarts (resolution changes)
    if (disconnect_peers && webrtc) {
        fprintf(stderr, "Video: Disconnecting all WebRTC peers for codec change\n");
        webrtc->disconnect_all_peers();
    }
}

// Implementation of HTTPServer::set_webrtc_server (needs WebRTCServer definition)
void HTTPServer::set_webrtc_server(WebRTCServer* webrtc) {
    if (webrtc) {
        api_ctx_.notify_codec_change_fn = [webrtc](CodecType codec) {
            webrtc->notify_codec_change(codec);
        };
    }
}

/*
 * Connection management loop - runs in main thread
 * Handles: scanning, connecting, disconnecting, process management, restarts
 */

static void connection_manager_loop(WebRTCServer& webrtc,
                                    std::thread* video_thread,
                                    std::thread* audio_thread) {
    auto last_scan_time = std::chrono::steady_clock::now();
    auto last_emu_check = std::chrono::steady_clock::now();

    fprintf(stderr, "Connection: Starting connection manager\n");

    while (g_running) {
        auto now = std::chrono::steady_clock::now();

        // Periodically scan for emulators if not connected
        // BUT: Don't reconnect if user explicitly stopped the emulator
        if (!g_emulator_connected && !g_user_stopped_emulator) {
            auto scan_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_scan_time);
            if (scan_elapsed.count() >= 500) {
                last_scan_time = now;

                // Grace period after disconnect: wait for emulator to finish cleanup
                // Emulator needs time to: join threads, munmap SHM, unlink socket
                // 500ms should be sufficient for clean shutdown
                auto disconnect_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_disconnect_time);
                if (disconnect_elapsed.count() < 500) {
                    continue;  // Still in grace period, skip connection attempts
                }

                // Priority 1: If we started an emulator, ONLY try to connect to that one
                // This prevents connecting to stray emulators from previous sessions
                if (g_started_emulator_pid > 0) {
                    if (try_connect_to_emulator(g_started_emulator_pid)) {
                        fprintf(stderr, "Connection: Connected to our emulator PID %d\n", g_started_emulator_pid);
                    }
                }
                // Priority 2: If target PID specified via command line, try that
                else if (g_target_emulator_pid > 0) {
                    if (try_connect_to_emulator(g_target_emulator_pid)) {
                        fprintf(stderr, "Connection: Connected to target emulator PID %d\n", g_target_emulator_pid);
                    }
                }
                // Priority 3: No specific emulator - scan for any running emulator
                else {
                    auto pids = scan_for_emulators();
                    for (pid_t pid : pids) {
                        if (try_connect_to_emulator(pid)) {
                            fprintf(stderr, "Connection: Found and connected to emulator PID %d\n", pid);
                            break;
                        }
                    }
                }
            }
        }

        // Check if we started an emulator and it exited
        auto emu_check_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_emu_check);
        if (emu_check_elapsed.count() >= 500) {
            last_emu_check = now;

            int exit_code = check_emulator_status();
            if (exit_code > 0 && g_auto_start_emulator) {
                // Emulator we started exited - check if restart requested (exit code 75)
                if (exit_code == 75) {
                    fprintf(stderr, "Connection: Auto-restarting emulator...\n");
                    disconnect_from_emulator(&webrtc);
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    start_emulator();
                    g_previous_codec = g_server_codec;
                }
            }

            // Handle restart request from web UI
            if (g_restart_emulator_requested.exchange(false)) {
                fprintf(stderr, "Connection: Restart requested from web UI\n");

                if (g_started_emulator_pid > 0) {
                    stop_emulator();
                } else {
                    send_command(MACEMU_CMD_RESET);
                }
                disconnect_from_emulator(&webrtc, false);  // Don't disconnect peers
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                // Always restart when explicitly requested via web UI
                start_emulator();
            }
        }

        // Check if emulator disconnected (socket closed)
        if (g_emulator_connected && g_control_socket >= 0) {
            char buf;
            ssize_t n = recv(g_control_socket, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
            if (n == 0) {
                // Connection closed
                fprintf(stderr, "Connection: Emulator disconnected\n");
                disconnect_from_emulator(&webrtc);
            }
        }

        // Sleep to avoid busy-looping
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    fprintf(stderr, "Connection: Exiting connection manager\n");
}

/*
 * Simplified video processing loop - ONLY processes frames
 * Connection management has been moved to connection_manager_loop() in main thread
 * This thread just waits for frames, encodes them, and sends to WebRTC
 */

static void video_loop(WebRTCServer& webrtc, H264Encoder& h264_encoder, AV1Encoder& av1_encoder, VP9Encoder& vp9_encoder, PNGEncoder& png_encoder, WebPEncoder& webp_encoder) {
    auto last_stats_time = std::chrono::steady_clock::now();
    int frames_encoded = 0;

    // Track input counts between stats intervals
    uint64_t last_mouse_move = 0;

    // Create epoll instance for low-latency event notification (REQUIRED)
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        fprintf(stderr, "Video: FATAL: Failed to create epoll: %s\n", strerror(errno));
        fprintf(stderr, "Video: epoll is required for frame synchronization (no polling fallback)\n");
        return;
    }
    int current_eventfd = -1;  // Track which eventfd is registered
    pid_t current_emulator_pid = -1;  // Track which emulator we're connected to

    fprintf(stderr, "Video: Starting frame processing loop\n");

    while (g_running) {
        auto now = std::chrono::steady_clock::now();

        // Wait for emulator connection (managed by main thread)
        if (!g_emulator_connected || !g_ipc_shm) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Register eventfd with epoll when emulator PID changes (new connection)
        // Kernel may reuse same fd number, so we check PID to detect new emulator
        if (current_emulator_pid != g_emulator_pid) {
            // Remove old eventfd from epoll if any
            if (current_eventfd >= 0) {
                fprintf(stderr, "Video: Removing old eventfd %d from epoll (PID %d->%d)\n",
                        current_eventfd, current_emulator_pid, g_emulator_pid);
                if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, current_eventfd, nullptr) < 0) {
                    fprintf(stderr, "Video: Warning: Failed to remove eventfd %d from epoll: %s\n",
                            current_eventfd, strerror(errno));
                }
                current_eventfd = -1;
            }

            // Add new eventfd
            if (g_frame_ready_eventfd >= 0) {
                struct epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.fd = g_frame_ready_eventfd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_frame_ready_eventfd, &ev) == 0) {
                    current_eventfd = g_frame_ready_eventfd;
                    current_emulator_pid = g_emulator_pid;
                    fprintf(stderr, "Video: Registered eventfd %d with epoll for PID %d\n",
                            current_eventfd, current_emulator_pid);
                } else {
                    fprintf(stderr, "Video: ERROR: Failed to add eventfd %d to epoll: %s\n",
                            g_frame_ready_eventfd, strerror(errno));
                    continue;
                }
            }
        }

        // Wait for new frame via epoll (blocking on eventfd)
        if (current_eventfd < 0) {
            // No eventfd registered yet, wait for emulator connection
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        struct epoll_event events[1];
        // Wait up to 5ms for frame ready event
        int n = epoll_wait(epoll_fd, events, 1, 5);
        if (n > 0) {
            // Frame ready! Consume the eventfd value (required for semaphore mode)
            // This read() provides memory barrier - all emulator writes before write(eventfd) are now visible
            uint64_t val;
            ssize_t ret = read(current_eventfd, &val, sizeof(val));
            if (ret != sizeof(val)) {
                fprintf(stderr, "Video: Error reading eventfd: %s\n",
                        ret < 0 ? strerror(errno) : "partial read");
                continue;
            }
        } else {
            // Timeout or error - continue loop
            continue;
        }

        // Check emulator still connected (race with disconnect)
        if (!g_emulator_connected || !g_ipc_shm) {
            continue;
        }

        // Latency measurement: time from emulator frame completion to now
        // Plain read - synchronized by eventfd read above
        uint64_t frame_timestamp_us = g_ipc_shm->timestamp_us;

        // T5: Server read timestamp - capture immediately after eventfd read
        // Use CLOCK_REALTIME to match the emulator's timestamp (both in same clock domain)
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t t5_server_read_us = (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
        uint64_t server_now_us = t5_server_read_us;  // Alias for existing latency code

        // Read frame dimensions (plain reads - synchronized by eventfd)
        uint32_t width = g_ipc_shm->width;
        uint32_t height = g_ipc_shm->height;

        // Track resolution changes
        static uint32_t last_width = 0;
        static uint32_t last_height = 0;
        if (g_debug_mode_switch && (width != last_width || height != last_height)) {
            fprintf(stderr, "[MODE] Server detected resolution change: %dx%d -> %dx%d\n",
                    last_width, last_height, width, height);
            last_width = width;
            last_height = height;
        }

        if (width == 0 || height == 0 || width > MACEMU_MAX_WIDTH || height > MACEMU_MAX_HEIGHT) {
            continue;
        }

        // Get BGRA frame from ready buffer
        // Emulator always outputs BGRA (B,G,R,A bytes), which is libyuv "ARGB"
        uint8_t* frame_data = macemu_get_ready_bgra(g_ipc_shm);
        int stride = macemu_get_bgra_stride();

        // Check if keyframe requested (new peer connected)
        bool keyframe_requested = g_request_keyframe.exchange(false);
        if (keyframe_requested) {
            h264_encoder.request_keyframe();
            av1_encoder.request_keyframe();
        }

        // Encode and send to H.264 peers
        auto encode_start = std::chrono::steady_clock::now();
        if (webrtc.has_codec_peer(CodecType::H264)) {
            EncodedFrame frame = h264_encoder.encode_bgra(frame_data, width, height, stride);

            // T6: Capture encode done timestamp
            struct timespec ts_enc;
            clock_gettime(CLOCK_REALTIME, &ts_enc);
            uint64_t t6_encode_done_us = (uint64_t)ts_enc.tv_sec * 1000000 + ts_enc.tv_nsec / 1000;

            if (!frame.data.empty()) {
                webrtc.populate_frame_metadata(frame, t5_server_read_us, t6_encode_done_us);
                webrtc.send_h264_frame(frame);
                frames_encoded++;
            }
        }

        // Encode and send to AV1 peers
        if (webrtc.has_codec_peer(CodecType::AV1)) {
            EncodedFrame frame = av1_encoder.encode_bgra(frame_data, width, height, stride);

            // T6: Capture encode done timestamp
            struct timespec ts_enc;
            clock_gettime(CLOCK_REALTIME, &ts_enc);
            uint64_t t6_encode_done_us = (uint64_t)ts_enc.tv_sec * 1000000 + ts_enc.tv_nsec / 1000;

            if (!frame.data.empty()) {
                webrtc.populate_frame_metadata(frame, t5_server_read_us, t6_encode_done_us);
                webrtc.send_av1_frame(frame);
                frames_encoded++;
            }
        }

        // Encode and send to VP9 peers
        if (webrtc.has_codec_peer(CodecType::VP9)) {
            EncodedFrame frame = vp9_encoder.encode_bgra(frame_data, width, height, stride);

            // T6: Capture encode done timestamp
            struct timespec ts_enc;
            clock_gettime(CLOCK_REALTIME, &ts_enc);
            uint64_t t6_encode_done_us = (uint64_t)ts_enc.tv_sec * 1000000 + ts_enc.tv_nsec / 1000;

            if (!frame.data.empty()) {
                webrtc.populate_frame_metadata(frame, t5_server_read_us, t6_encode_done_us);
                webrtc.send_vp9_frame(frame);
                frames_encoded++;
            }
        }

        // Encode and send to still-image codec peers (PNG/WebP) using dirty rects from emulator
        // Both PNG and WebP use the same dirty rect logic, differ only in encoding
        bool has_stillimage_peer = webrtc.has_codec_peer(CodecType::PNG) || webrtc.has_codec_peer(CodecType::WEBP);
        if (has_stillimage_peer) {
            // Read dirty rect from SHM (plain reads - synchronized by eventfd)
            uint32_t dirty_x = g_ipc_shm->dirty_x;
            uint32_t dirty_y = g_ipc_shm->dirty_y;
            uint32_t dirty_width = g_ipc_shm->dirty_width;
            uint32_t dirty_height = g_ipc_shm->dirty_height;

            // Debug logging for dirty rects (only if MACEMU_DEBUG_PNG is set)
            if (g_debug_png) {
                static int dirty_log_counter = 0;
                if (++dirty_log_counter % 30 == 0) {
                    fprintf(stderr, "StillImage: Dirty rect from emulator: x=%u y=%u w=%u h=%u (frame: %ux%u)\n",
                            dirty_x, dirty_y, dirty_width, dirty_height, width, height);
                }
            }

            // Force full frame if any still-image peer needs their first frame
            // Still-image peers need a full first frame to initialize the canvas
            bool needs_first = webrtc.png_peer_needs_first_frame();
            if (needs_first) {
                dirty_x = 0;
                dirty_y = 0;
                dirty_width = width;
                dirty_height = height;
            }

            // Heartbeat mechanism: Send tiny frame to carry ping echoes even when screen is static
            // This prevents ping responses from being delayed indefinitely on idle screens
            static auto last_heartbeat = std::chrono::steady_clock::now();
            if (dirty_width == 0 || dirty_height == 0) {
                // Check if there's a pending ping echo
                uint32_t ping_seq = ATOMIC_LOAD(g_ipc_shm->ping_sequence);
                if (ping_seq > 0) {
                    auto heartbeat_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_heartbeat);
                    // Send heartbeat at most once per second to avoid spam
                    if (heartbeat_elapsed.count() >= 1000) {
                        // Send a tiny 1x1 pixel frame just to carry the ping echo
                        dirty_x = 0;
                        dirty_y = 0;
                        dirty_width = 1;
                        dirty_height = 1;
                        last_heartbeat = now;
                        // Note: This encodes/sends 1 pixel which is ~100 bytes - negligible overhead
                    }
                }
            } else {
                // Screen is updating, reset heartbeat timer
                last_heartbeat = now;
            }

            // Only encode if there are changes (dirty_width > 0) or heartbeat triggered
            // Ping echoes are carried in the frame metadata header of any frame being sent
            if (dirty_width > 0 && dirty_height > 0) {
                // Check if this is a full frame or dirty rect
                bool is_full_frame = (dirty_x == 0 && dirty_y == 0 && dirty_width == width && dirty_height == height);

                if (g_debug_png) {
                    static int encode_log_counter = 0;
                    if (++encode_log_counter % 30 == 0) {
                        fprintf(stderr, "StillImage: Encoding %s (x=%u y=%u w=%u h=%u)\n",
                                is_full_frame ? "FULL FRAME" : "dirty rect",
                                dirty_x, dirty_y, dirty_width, dirty_height);
                    }
                }

                // Encode with PNG encoder if we have PNG peers
                if (webrtc.has_codec_peer(CodecType::PNG)) {
                    EncodedFrame frame;
                    if (is_full_frame) {
                        frame = png_encoder.encode_bgra(frame_data, width, height, stride);
                    } else {
                        frame = png_encoder.encode_bgra_rect(frame_data, width, height, stride,
                                                             dirty_x, dirty_y, dirty_width, dirty_height);
                    }

                    // T6: Capture encode done timestamp
                    struct timespec ts_enc;
                    clock_gettime(CLOCK_REALTIME, &ts_enc);
                    uint64_t t6_encode_done_us = (uint64_t)ts_enc.tv_sec * 1000000 + ts_enc.tv_nsec / 1000;

                    if (!frame.data.empty()) {
                        webrtc.populate_frame_metadata(frame, t5_server_read_us, t6_encode_done_us);
                        uint64_t t1_frame_ready_ms = frame_timestamp_us / 1000;
                        webrtc.send_png_frame(frame, t1_frame_ready_ms,
                                              dirty_x, dirty_y, dirty_width, dirty_height,
                                              width, height);
                        frames_encoded++;
                    }
                }

                // Encode with WebP encoder if we have WebP peers
                if (webrtc.has_codec_peer(CodecType::WEBP)) {
                    EncodedFrame frame;
                    if (is_full_frame) {
                        frame = webp_encoder.encode_bgra(frame_data, width, height, stride);
                    } else {
                        frame = webp_encoder.encode_bgra_rect(frame_data, width, height, stride,
                                                              dirty_x, dirty_y, dirty_width, dirty_height);
                    }

                    // T6: Capture encode done timestamp
                    struct timespec ts_enc;
                    clock_gettime(CLOCK_REALTIME, &ts_enc);
                    uint64_t t6_encode_done_us = (uint64_t)ts_enc.tv_sec * 1000000 + ts_enc.tv_nsec / 1000;

                    if (!frame.data.empty()) {
                        webrtc.populate_frame_metadata(frame, t5_server_read_us, t6_encode_done_us);
                        uint64_t t1_frame_ready_ms = frame_timestamp_us / 1000;
                        webrtc.send_png_frame(frame, t1_frame_ready_ms,
                                              dirty_x, dirty_y, dirty_width, dirty_height,
                                              width, height);
                        frames_encoded++;
                    }
                }
            }
            // else: no changes (dirty_width == 0), skip encoding
        }
        auto encode_end = std::chrono::steady_clock::now();

        // Track latency stats
        static uint64_t total_shm_latency_us = 0;
        static uint64_t total_encode_latency_us = 0;
        static int latency_samples = 0;

        if (frame_timestamp_us > 0) {
            uint64_t shm_latency = server_now_us - frame_timestamp_us;
            uint64_t encode_latency = std::chrono::duration_cast<std::chrono::microseconds>(
                encode_end - encode_start).count();

            total_shm_latency_us += shm_latency;
            total_encode_latency_us += encode_latency;
            latency_samples++;
        }

        // Cursor updates are now sent with every frame (in frame metadata)
        // No separate cursor update broadcast needed

        // Print stats every 3 seconds
        auto stats_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_stats_time);
        if (stats_elapsed.count() >= 3000) {
            float fps = frames_encoded * 1000.0f / stats_elapsed.count();

            // Calculate input rates
            uint64_t cur_mouse_move = g_mouse_move_count.load();
            uint64_t mouse_moves = cur_mouse_move - last_mouse_move;
            float mouse_rate = mouse_moves * 1000.0f / stats_elapsed.count();

            // Calculate average video latencies (server-side only)
            float avg_shm_ms = latency_samples > 0 ? (total_shm_latency_us / latency_samples) / 1000.0f : 0;
            float avg_encode_ms = latency_samples > 0 ? (total_encode_latency_us / latency_samples) / 1000.0f : 0;

            if (g_debug_perf) {
                fprintf(stderr, "[Server] fps=%.1f peers=%d | video: shm=%.1fms enc=%.1fms | mouse: rate=%.0f/s | emu=%s pid=%d\n",
                        fps, webrtc.peer_count(),
                        avg_shm_ms, avg_encode_ms,
                        mouse_rate,
                        g_emulator_connected ? "connected" : "scanning",
                        g_emulator_pid);
            }

            // Reset latency counters
            total_shm_latency_us = 0;
            total_encode_latency_us = 0;
            latency_samples = 0;

            // Save frame as PPM (readable image format) for debugging (disabled by default)
            if (g_debug_frames) {
                static int frame_save_count = 0;
                if (frame_save_count < 3) {  // Only save first 3 frames
                    char filename[64];
                    snprintf(filename, sizeof(filename), "frame_%d_%dx%d.ppm", frame_save_count, width, height);
                    FILE* f = fopen(filename, "wb");
                    if (f) {
                        // Convert BGRA frame to RGB and save as PPM
                        // BGRA = bytes B,G,R,A (libyuv "ARGB")
                        fprintf(f, "P6\n%d %d\n255\n", width, height);
                        for (uint32_t row = 0; row < height; row++) {
                            for (uint32_t col = 0; col < width; col++) {
                                const uint8_t* pixel = frame_data + row * stride + col * 4;
                                uint8_t B = pixel[0];
                                uint8_t G = pixel[1];
                                uint8_t R = pixel[2];
                                uint8_t rgb[3] = {R, G, B};
                                fwrite(rgb, 1, 3, f);
                            }
                        }
                        fclose(f);
                        fprintf(stderr, "[Server] Saved debug frame: %s (BGRA)\n", filename);
                        frame_save_count++;
                    }
                }
            }

            last_mouse_move = cur_mouse_move;
            frames_encoded = 0;
            last_stats_time = now;
        }
    }

    // Clean up epoll
    if (epoll_fd >= 0) {
        close(epoll_fd);
    }

    fprintf(stderr, "Video: Exiting frame processing loop\n");
}


/*
 * Audio processing loop - PULL MODEL (server-driven, like SDL)
 *
 * Server requests audio at fixed Opus frame intervals (20ms @ 48kHz = 960 samples).
 * This matches SDL's callback architecture for perfect timing synchronization.
 */

static void audio_loop_mac_ipc(WebRTCServer& webrtc) {
    uint64_t frames_consumed = 0;
    uint64_t frames_underrun = 0;

    // Fixed 20ms timing for Opus
    // Server controls timing, Mac responds to requests
    const auto frame_duration = std::chrono::milliseconds(20);

    if (g_debug_audio) {
        fprintf(stderr, "Audio: Starting frame-based audio loop (PULL MODEL - server controls timing)\n");
    }

    while (g_running) {
        auto frame_start = std::chrono::steady_clock::now();

        // Check connection status
        if (!g_ipc_shm || !g_emulator_connected) {
            std::this_thread::sleep_for(frame_duration);
            continue;
        }

        // PULL MODEL: Request audio from Mac before reading
        // This wakes up Mac's audio thread to produce a frame
        if (g_control_socket >= 0) {
            MacEmuAudioRequestInput audio_req;
            audio_req.hdr.type = MACEMU_INPUT_AUDIO_REQUEST;
            audio_req.hdr.flags = 0;
            audio_req.hdr._reserved = 0;
            audio_req.requested_samples = 960;  // Opus frame size @ 48kHz

            ssize_t sent = send(g_control_socket, &audio_req, sizeof(audio_req), MSG_NOSIGNAL);
            if (sent != sizeof(audio_req)) {
                if (g_debug_audio) {
                    static int err_count = 0;
                    if (++err_count <= 10) {
                        fprintf(stderr, "Audio: Failed to send request to Mac (count=%d)\n", err_count);
                    }
                }
            }
        }

        // Give Mac time to respond to interrupt and produce frame
        // Mac needs to: wake thread, trigger interrupt, execute 68k audio code, write frame
        // During startup, Mac audio init can be slow, so give extra margin
        // 8ms is conservative but still leaves 12ms for resampling/encoding in 20ms budget
        std::this_thread::sleep_for(std::chrono::milliseconds(8));

        // Read frame ring buffer indices
        // Note: Use g_ipc_shm directly (don't cache) - it can change when reconnecting to new emulator
        // The 200ms grace period in disconnect_from_emulator() ensures old SHM stays valid during disconnect
        if (!g_ipc_shm || !g_emulator_connected) {
            continue;
        }
        uint32_t read_idx = ATOMIC_LOAD(g_ipc_shm->audio_frame_read_idx);
        uint32_t write_idx = ATOMIC_LOAD(g_ipc_shm->audio_frame_write_idx);

        // Select frame to consume
        const MacEmuAudioFrame* frame;
        bool is_silence = false;

        if (read_idx == write_idx) {
            // Ring buffer empty - underrun! Use silence frame
            frame = &g_ipc_shm->audio_silence_frame;
            is_silence = true;
            frames_underrun++;

            if (g_debug_audio && frames_underrun <= 10) {
                fprintf(stderr, "Audio: Underrun - no frames available, using silence (count=%lu)\n",
                        frames_underrun);
            }
        } else {
            // Normal case - consume frame from ring buffer
            frame = &g_ipc_shm->audio_frame_ring[read_idx];
            is_silence = false;
        }

        // Prepare buffer for resampling/byte-swapping (max 960 samples stereo S16)
        int16_t opus_buffer[960 * 2];
        const int16_t* samples_to_encode = nullptr;

        // Process frame: Byte-swap from Mac's big-endian to Opus's little-endian
        // Mac produces S16MSB (big-endian), Opus expects native int16_t (little-endian on x86_64)
        // We force 48kHz in audio_ipc.cpp, so no resampling needed
        const uint8_t* src_bytes = reinterpret_cast<const uint8_t*>(frame->data);
        uint32_t total_samples = std::min(frame->samples, (uint32_t)960) * frame->channels;

        // Detect if this is 8-bit audio masquerading as 16-bit (duplicated bytes)
        // This happens when Mac apps produce 8-bit audio stored as duplicated 16-bit samples
        bool is_8bit_duplicated = true;
        for (uint32_t i = 0; i < std::min((uint32_t)20, total_samples) && is_8bit_duplicated; i++) {
            if (src_bytes[i * 2] != src_bytes[i * 2 + 1]) {
                is_8bit_duplicated = false;
            }
        }

        // Log detection (once per session)
        static bool logged_format = false;
        if (!logged_format && !is_silence) {
            if (is_8bit_duplicated) {
                fprintf(stderr, "Audio: Detected 8-bit audio (duplicated bytes), converting to 16-bit\n");
            } else {
                fprintf(stderr, "Audio: Detected true 16-bit audio, byte-swapping\n");
            }
            logged_format = true;
        }

        if (is_8bit_duplicated) {
            // Convert 8-bit to proper 16-bit: read high byte as signed 8-bit, scale to full 16-bit range
            // Example: 0x19 0x19 -> read 0x19 as int8 (25) -> scale: 25 * 256 = 6400
            for (uint32_t i = 0; i < total_samples; i++) {
                int8_t sample_8bit = static_cast<int8_t>(src_bytes[i * 2]);  // High byte
                opus_buffer[i] = static_cast<int16_t>(sample_8bit) << 8;  // Scale to 16-bit range
            }
        } else {
            // True 16-bit: byte-swap from big-endian to little-endian
            const int16_t* src = reinterpret_cast<const int16_t*>(frame->data);
            for (uint32_t i = 0; i < total_samples; i++) {
                uint16_t val = static_cast<uint16_t>(src[i]);
                opus_buffer[i] = static_cast<int16_t>((val >> 8) | (val << 8));
            }
        }

        // Pad if needed
        if (frame->samples < 960) {
            memset(&opus_buffer[frame->samples * frame->channels], 0,
                   (960 - frame->samples) * frame->channels * sizeof(int16_t));
        }

        samples_to_encode = opus_buffer;

        // AUDIO DEBUG: Capture byte-swapped audio (what goes to Opus encoder)
        // Only capture non-silent frames (skip silence between sounds)
        {
            static FILE* capture_file = nullptr;
            static int frames_captured = 0;
            static int silent_frames = 0;
            static bool capture_started = false;

            // Debug capture removed - was triggered via stdin monitor
            // Can be re-enabled via API endpoint if needed
            if (false && capture_started && frames_captured < AUDIO_MAX_CAPTURE_FRAMES) {
                // Calculate energy to detect non-silence
                uint64_t energy = 0;
                for (uint32_t i = 0; i < total_samples; i++) {
                    int16_t val = opus_buffer[i];
                    energy += (val < 0) ? -val : val;
                }

                // Only capture non-silent frames
                if (energy > AUDIO_ENERGY_THRESHOLD) {
                    if (!capture_file) {
                        capture_file = fopen("/tmp/ipc_server_capture.raw", "wb");
                        fprintf(stderr, "IPC (server): Starting audio capture to /tmp/ipc_server_capture.raw\n");
                        fprintf(stderr, "IPC (server): Format: 16-bit S16LE (little-endian), %u channels, 48000 Hz\n",
                                frame->channels);
                        fprintf(stderr, "IPC (server): Capturing only non-silent frames (max %d frames)\n", AUDIO_MAX_CAPTURE_FRAMES);
                    }

                    // Write full Opus frame (960 samples * 2 channels = 1920 samples = 3840 bytes)
                    fwrite(opus_buffer, sizeof(int16_t), 960 * frame->channels, capture_file);
                    frames_captured++;
                    silent_frames = 0;

                    // Log every 50 frames
                    if (frames_captured % 50 == 0) {
                        fprintf(stderr, "IPC (server): Captured %d non-silent frames\n", frames_captured);
                    }

                    // Stop after max frames
                    if (frames_captured >= AUDIO_MAX_CAPTURE_FRAMES) {
                        fclose(capture_file);
                        fprintf(stderr, "IPC (server): Audio capture complete (%d frames)\n", frames_captured);
                    }
                } else {
                    silent_frames++;
                    // Log long silences
                    if (silent_frames == 50) {
                        fprintf(stderr, "IPC (server): Skipping silence (captured %d frames so far)\n", frames_captured);
                    }
                }
            }
        }

        // Encode to Opus (always 960 samples @ 48kHz stereo)
        if (g_audio_encoder) {
            std::vector<uint8_t> opus_data = g_audio_encoder->encode(
                samples_to_encode,
                960  // Opus frame size
            );

            // Send to peers with frame-based timing
            if (!opus_data.empty()) {
                webrtc.send_audio_to_all_peers(opus_data, frames_consumed);
            }
        }

        // Advance read index (only if we didn't use silence frame)
        if (!is_silence && g_ipc_shm) {
            uint32_t next_read_idx = (read_idx + 1) % MACEMU_AUDIO_FRAME_RING_SIZE;
            ATOMIC_STORE(g_ipc_shm->audio_frame_read_idx, next_read_idx);
            frames_consumed++;
        }

        // Periodic stats
        if (g_debug_audio && frames_consumed > 0 && frames_consumed % 100 == 0) {
            fprintf(stderr, "Audio: Stats - consumed=%lu underruns=%lu\n",
                    frames_consumed, frames_underrun);
        }

        // Maintain 20ms frame timing
        auto elapsed = std::chrono::steady_clock::now() - frame_start;
        auto remaining = frame_duration - elapsed;
        if (remaining > std::chrono::milliseconds(0)) {
            std::this_thread::sleep_for(remaining);
        }
    }

    if (g_debug_audio) {
        fprintf(stderr, "Audio: Exiting frame-based audio loop (consumed=%lu, underruns=%lu)\n",
                frames_consumed, frames_underrun);
    }
}


/*
 * Main audio processing loop - dispatches to tone or Mac IPC mode
 */

static void audio_loop(WebRTCServer& webrtc) {
    // Use Mac IPC audio (tone generator available via audio_loop_tone_only if needed)
    audio_loop_mac_ipc(webrtc);
}


/*
 * Print usage
 */

// print_usage() moved to config/server_config.cpp


/*
 * Main entry point
 */

int main(int argc, char* argv[]) {
    // Parse configuration from command line and environment
    g_config.parse_command_line(argc, argv);
    g_config.load_from_env();

    // Expand $HOME in paths
    g_config.roms_path = expand_path(g_config.roms_path);
    g_config.images_path = expand_path(g_config.images_path);

    // Ensure ~/.macemu directory exists
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    std::string macemu_dir = std::string(home) + "/.macemu";
    mkdir(macemu_dir.c_str(), 0755);

    // Sync debug flags (needed by encoders)
    g_debug_mode_switch = g_config.debug_mode_switch;
    g_debug_png = g_config.debug_png;

    // Set MACEMU_DEBUG_AUDIO environment variable if --debug-audio enabled
    // This will be inherited by the emulator process
    if (g_debug_audio) {
        setenv("MACEMU_DEBUG_AUDIO", "1", 1);
    }

    // Set up signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    // Set up crash handlers with full context
    struct sigaction sa_crash;
    memset(&sa_crash, 0, sizeof(sa_crash));
    sa_crash.sa_sigaction = crash_handler;
    sa_crash.sa_flags = SA_SIGINFO | SA_RESETHAND;  // Get siginfo_t, reset after first call
    sigemptyset(&sa_crash.sa_mask);

    sigaction(SIGSEGV, &sa_crash, NULL);  // Segmentation fault
    sigaction(SIGBUS, &sa_crash, NULL);   // Bus error
    sigaction(SIGABRT, &sa_crash, NULL);  // Abort
    sigaction(SIGILL, &sa_crash, NULL);   // Illegal instruction
    sigaction(SIGFPE, &sa_crash, NULL);   // Floating point exception

    // Create minimal prefs file if it doesn't exist (for cold boot)
    create_minimal_prefs_if_needed();

    // Load codec from JSON config at startup
    // This ensures browser connections use the correct codec from the start
    std::string config_path = macemu_dir + "/macemu-config.json";
    try {
        config::MacemuConfig cfg = config::load_config(config_path);
        if (cfg.web.codec == "h264") {
            g_config.server_codec = CodecType::H264;
        } else if (cfg.web.codec == "av1") {
            g_config.server_codec = CodecType::AV1;
        } else if (cfg.web.codec == "vp9") {
            g_config.server_codec = CodecType::VP9;
        } else if (cfg.web.codec == "webp") {
            g_config.server_codec = CodecType::WEBP;
        } else {
            g_config.server_codec = CodecType::PNG;
        }
    } catch (...) {
        // If config load fails, keep default PNG codec
        fprintf(stderr, "WARNING: Failed to load codec from %s, using PNG\n", config_path.c_str());
    }

    // Print configuration summary
    g_config.print_summary();

    // Start HTTP server
    HTTPServer http_server;
    if (!http_server.start(g_http_port)) {
        fprintf(stderr, "Failed to start HTTP server\n");
        return 1;
    }

    // Start WebRTC server
    WebRTCServer webrtc;
    if (!webrtc.init(g_signaling_port)) {
        fprintf(stderr, "Failed to start WebRTC server\n");
        http_server.stop();
        return 1;
    }

    // Connect HTTP server to WebRTC server for codec change notifications
    http_server.set_webrtc_server(&webrtc);

    fprintf(stderr, "\nOpen http://localhost:%d in your browser\n", g_http_port);

    // Create encoders
    H264Encoder h264_encoder;
    AV1Encoder av1_encoder;
    VP9Encoder vp9_encoder;
    PNGEncoder png_encoder;
    WebPEncoder webp_encoder;
    g_audio_encoder = std::make_unique<OpusAudioEncoder>();

    // Initialize audio encoder using centralized audio_config.h constants
    // Settings match the WebRTC Opus profile for consistency
    if (!g_audio_encoder->init(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, OPUS_BITRATE)) {
        fprintf(stderr, "WARNING: Failed to initialize Opus audio encoder\n");
    }

    // Auto-start emulator if enabled
    if (g_auto_start_emulator && g_target_emulator_pid == 0) {
        std::string emu = find_emulator();
        if (!emu.empty()) {
            fprintf(stderr, "Found emulator: %s\n", emu.c_str());
            if (start_emulator()) {
                fprintf(stderr, "Emulator started, waiting for IPC resources...\n\n");
                g_previous_codec = g_server_codec;  // Initialize codec tracking
            }
        } else {
            fprintf(stderr, "\n");
            fprintf(stderr, "ERROR: No emulator found!\n");
            fprintf(stderr, "\n");
            fprintf(stderr, "Please create a symlink in the bin/ directory:\n");
            fprintf(stderr, "  mkdir -p bin\n");
            fprintf(stderr, "  ln -s ../../BasiliskII/src/Unix/BasiliskII ./bin/BasiliskII\n");
            fprintf(stderr, "  or\n");
            fprintf(stderr, "  ln -s ../../SheepShaver/src/Unix/SheepShaver ./bin/SheepShaver\n");
            fprintf(stderr, "\n");
            fprintf(stderr, "Alternatively, use --emulator PATH to specify the executable.\n");
            fprintf(stderr, "\n");
            webrtc.shutdown();
            http_server.stop();
            return 1;
        }
    } else if (g_target_emulator_pid > 0) {
        fprintf(stderr, "Waiting to connect to emulator PID %d...\n\n", g_target_emulator_pid);
    } else {
        fprintf(stderr, "Auto-start disabled, scanning for running emulators...\n\n");
    }

    // Launch worker threads
    std::thread audio_thread(audio_loop, std::ref(webrtc));
    std::thread video_thread(video_loop, std::ref(webrtc), std::ref(h264_encoder), std::ref(av1_encoder), std::ref(vp9_encoder), std::ref(png_encoder), std::ref(webp_encoder));

    // Run connection manager in main thread
    // This handles scanning, connecting, disconnecting, process management, restarts
    connection_manager_loop(webrtc, &video_thread, &audio_thread);

    // Signal shutdown happened - wait for worker threads to exit cleanly
    fprintf(stderr, "Server: Waiting for worker threads to exit...\n");
    video_thread.join();
    audio_thread.join();
    fprintf(stderr, "Server: All worker threads exited\n");

    // Stop emulator if we started it
    stop_emulator();

    // Disconnect from emulator (if not already disconnected)
    if (g_emulator_connected) {
        disconnect_from_emulator();
    }

    // Cleanup
    webrtc.shutdown();
    http_server.stop();

    fprintf(stderr, "Server: Shutdown complete\n");
    return 0;
}
