/*
 * Server Configuration Implementation
 */

#include "server_config.h"
#include "../codec.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <getopt.h>

namespace server_config {

ServerConfig::ServerConfig() {
    // Set default codec
    server_codec = CodecType::PNG;
}

void ServerConfig::parse_command_line(int argc, char* argv[]) {
    static struct option long_options[] = {
        {"help",          no_argument,       0, 'h'},
        {"http-port",     required_argument, 0, 'p'},
        {"signaling",     required_argument, 0, 's'},
        {"emulator",      required_argument, 0, 'e'},
        {"prefs",         required_argument, 0, 'P'},
        {"no-auto-start", no_argument,       0, 'n'},
        {"pid",           required_argument, 0,  0 },
        {"roms",          required_argument, 0,  0 },
        {"images",        required_argument, 0,  0 },
        {"enable-stun",   no_argument,       0,  0 },
        {"stun-server",   required_argument, 0,  0 },
        {0, 0, 0, 0}
    };

    int option_index = 0;
    int c;

    while ((c = getopt_long(argc, argv, "hp:s:e:P:n", long_options, &option_index)) != -1) {
        switch (c) {
            case 0:
                // Long option
                if (strcmp(long_options[option_index].name, "pid") == 0) {
                    target_emulator_pid = atoi(optarg);
                } else if (strcmp(long_options[option_index].name, "roms") == 0) {
                    roms_path = optarg;
                } else if (strcmp(long_options[option_index].name, "images") == 0) {
                    images_path = optarg;
                } else if (strcmp(long_options[option_index].name, "enable-stun") == 0) {
                    enable_stun = true;
                } else if (strcmp(long_options[option_index].name, "stun-server") == 0) {
                    stun_server = optarg;
                    enable_stun = true;
                }
                break;

            case 'h':
                print_usage(argv[0]);
                exit(0);

            case 'p':
                http_port = atoi(optarg);
                break;

            case 's':
                signaling_port = atoi(optarg);
                break;

            case 'e':
                emulator_path = optarg;
                break;

            case 'P':
                prefs_path = optarg;
                break;

            case 'n':
                auto_start_emulator = false;
                break;

            case '?':
                // Error message already printed by getopt_long
                exit(1);

            default:
                fprintf(stderr, "Unknown option\n");
                exit(1);
        }
    }
}

void ServerConfig::load_from_env() {
    // Debug flags from environment
    if (getenv("MACEMU_DEBUG_CONNECTION")) {
        debug_connection = true;
    }
    if (getenv("MACEMU_DEBUG_MODE_SWITCH")) {
        debug_mode_switch = true;
    }
    if (getenv("MACEMU_DEBUG_PERF")) {
        debug_perf = true;
    }
    if (getenv("MACEMU_DEBUG_FRAMES")) {
        debug_frames = true;
    }
    if (getenv("MACEMU_DEBUG_AUDIO")) {
        debug_audio = true;
    }
    if (getenv("MACEMU_DEBUG_PNG")) {
        debug_png = true;
    }
    if (getenv("MACEMU_DEBUG_MOUSE")) {
        debug_mouse = true;
    }
}

void ServerConfig::print_summary() const {
    fprintf(stderr, "\n=== macemu WebRTC Server ===\n");
    fprintf(stderr, "HTTP server:      http://0.0.0.0:%d\n", http_port);
    fprintf(stderr, "Signaling port:   %d\n", signaling_port);
    fprintf(stderr, "ROMs path:        %s\n", roms_path.c_str());
    fprintf(stderr, "Images path:      %s\n", images_path.c_str());
    fprintf(stderr, "Prefs file:       %s\n", prefs_path.c_str());
    fprintf(stderr, "STUN:             %s\n", enable_stun ? "enabled" : "disabled");
    if (enable_stun) {
        fprintf(stderr, "STUN server:      %s\n", stun_server.c_str());
    }

    fprintf(stderr, "\nEmulator:\n");
    fprintf(stderr, "  Auto-start:     %s\n", auto_start_emulator ? "yes" : "no");
    if (!emulator_path.empty()) {
        fprintf(stderr, "  Path:           %s\n", emulator_path.c_str());
    }
    if (target_emulator_pid > 0) {
        fprintf(stderr, "  Target PID:     %d\n", target_emulator_pid);
    }

    // Show active debug flags
    bool any_debug = debug_connection || debug_mode_switch || debug_perf ||
                     debug_frames || debug_audio || debug_png || debug_mouse;
    if (any_debug) {
        fprintf(stderr, "\nDebug flags:\n");
        if (debug_connection)   fprintf(stderr, "  - Connection (WebRTC/ICE/signaling)\n");
        if (debug_mode_switch)  fprintf(stderr, "  - Mode switch (resolution/color depth)\n");
        if (debug_perf)         fprintf(stderr, "  - Performance (stats/ping)\n");
        if (debug_frames)       fprintf(stderr, "  - Frames (save to disk)\n");
        if (debug_audio)        fprintf(stderr, "  - Audio processing\n");
        if (debug_png)          fprintf(stderr, "  - PNG encoding/dirty rects\n");
        if (debug_mouse)        fprintf(stderr, "  - Mouse input (absolute/relative)\n");
    }

    fprintf(stderr, "\n");
}

void ServerConfig::print_usage(const char* program_name) const {
    fprintf(stderr, "Usage: %s [options]\n\n", program_name);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -h, --help              Show this help\n");
    fprintf(stderr, "  -p, --http-port PORT    HTTP server port (default: %d)\n", http_port);
    fprintf(stderr, "  -s, --signaling PORT    WebSocket signaling port (default: %d)\n", signaling_port);
    fprintf(stderr, "  -e, --emulator PATH     Path to BasiliskII/SheepShaver executable\n");
    fprintf(stderr, "  -P, --prefs FILE        Emulator prefs file (default: %s)\n", prefs_path.c_str());
    fprintf(stderr, "  -n, --no-auto-start     Don't auto-start emulator\n");
    fprintf(stderr, "      --pid PID           Connect to specific emulator PID\n");
    fprintf(stderr, "      --roms PATH         ROMs directory (default: %s)\n", roms_path.c_str());
    fprintf(stderr, "      --images PATH       Disk images directory (default: %s)\n", images_path.c_str());
    fprintf(stderr, "      --enable-stun       Enable STUN for remote connections\n");
    fprintf(stderr, "      --stun-server URL   STUN server URL (default: %s)\n", stun_server.c_str());
    fprintf(stderr, "\nEnvironment variables:\n");
    fprintf(stderr, "  MACEMU_DEBUG_CONNECTION    Enable WebRTC/ICE/signaling debug logs\n");
    fprintf(stderr, "  MACEMU_DEBUG_MODE_SWITCH   Enable mode/resolution change logs\n");
    fprintf(stderr, "  MACEMU_DEBUG_PERF          Enable performance/ping logs\n");
    fprintf(stderr, "  MACEMU_DEBUG_FRAMES        Save frame dumps to disk\n");
    fprintf(stderr, "  MACEMU_DEBUG_AUDIO         Enable audio processing logs\n");
    fprintf(stderr, "  MACEMU_DEBUG_PNG           Enable PNG encoding/dirty rect logs\n");
    fprintf(stderr, "  MACEMU_DEBUG_MOUSE         Enable mouse input logs (absolute/relative)\n");
    fprintf(stderr, "\n");
}

} // namespace server_config
