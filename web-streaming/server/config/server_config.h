/*
 * Server Configuration
 *
 * Centralized configuration management for the WebRTC server.
 * Replaces scattered global variables with a clean config object.
 */

#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

#include <string>
#include <sys/types.h>  // for pid_t

// Forward declaration
enum class CodecType;

namespace server_config {

struct ServerConfig {
    // Network configuration
    int http_port = 8000;
    int signaling_port = 8090;
    bool enable_stun = false;
    std::string stun_server = "stun:stun.l.google.com:19302";

    // Paths
    std::string roms_path = "$HOME/.macemu/storage/roms";
    std::string images_path = "$HOME/.macemu/storage/images";
    std::string prefs_path = "basilisk_ii.prefs";
    std::string emulator_path;  // Path to BasiliskII/SheepShaver executable

    // Behavior
    bool auto_start_emulator = false;  // Start with emulator off, user starts from Web UI
    pid_t target_emulator_pid = 0;  // If specified, connect to this PID only
    CodecType server_codec;          // Server-side codec preference (set in constructor)

    // Debug flags
    bool debug_connection = false;   // WebRTC, ICE, signaling logs
    bool debug_mode_switch = false;  // Mode/resolution/color depth changes
    bool debug_perf = false;         // Performance stats, ping logs
    bool debug_frames = false;       // Save frame dumps to disk (.ppm files)
    bool debug_audio = false;        // Audio processing logs
    bool debug_png = false;          // PNG encoding and dirty rect logs
    bool debug_mouse = false;        // Mouse input logs (absolute/relative coordinates)

    // Constructor with default codec
    ServerConfig();

    /**
     * Parse command-line arguments
     * @param argc Argument count
     * @param argv Argument values
     */
    void parse_command_line(int argc, char* argv[]);

    /**
     * Load configuration from environment variables
     * Checks MACEMU_DEBUG_* variables
     */
    void load_from_env();

    /**
     * Print configuration summary
     */
    void print_summary() const;

private:
    void print_usage(const char* program_name) const;
};

} // namespace server_config

#endif // SERVER_CONFIG_H
