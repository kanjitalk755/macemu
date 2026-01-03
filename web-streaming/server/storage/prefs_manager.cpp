/*
 * Prefs Manager Module
 *
 * Handles reading, writing, and creating Basilisk II/SheepShaver preferences files.
 */

#include "prefs_manager.h"
#include <fstream>
#include <sstream>
#include <cstdio>

namespace storage {

// Read raw prefs file content (JS frontend handles all parsing)
std::string read_prefs_file(const std::string& prefs_path) {
    std::ifstream file(prefs_path);
    if (!file) {
        return "";  // Empty string means no file exists
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Write raw prefs file content (JS frontend handles all serialization)
bool write_prefs_file(const std::string& prefs_path, const std::string& content) {
    std::ofstream file(prefs_path);
    if (!file) {
        fprintf(stderr, "Config: Failed to open prefs file for writing: %s\n", prefs_path.c_str());
        return false;
    }

    file << content;
    file.close();

    fprintf(stderr, "Config: Wrote prefs file: %s (%zu bytes)\n", prefs_path.c_str(), content.size());
    return true;
}

// Create minimal prefs file if it doesn't exist
// Based on template from client.js
void create_minimal_prefs_if_needed(const std::string& prefs_path) {
    // Check if file already exists
    std::ifstream check(prefs_path);
    if (check.good()) {
        check.close();
        return;  // File exists, nothing to do
    }

    fprintf(stderr, "Config: Creating minimal prefs file at %s\n", prefs_path.c_str());

    // Minimal configuration template - matches client.js PREFS_TEMPLATE
    const char* minimal_prefs =
        "# Basilisk II preferences - minimal config for cold boot\n"
        "\n"
        "# ROM file (configure via web UI)\n"
        "rom \n"
        "\n"
        "# Boot volume (configure via web UI)\n"
        "bootdrive 0\n"
        "bootdriver 0\n"
        "\n"
        "# Screen (IPC mode for web streaming)\n"
        "screen ipc/1024/768\n"
        "\n"
        "# CPU and emulation\n"
        "modelid 14\n"
        "cpu 4\n"
        "fpu true\n"
        "jit true\n"
        "nosound false\n"
        "\n"
        "# Video codec for web streaming (png, h264, av1, or vp9)\n"
        "webcodec png\n"
        "\n"
        "# Mouse mode for web streaming (relative or absolute)\n"
        "mousemode absolute\n"
        "\n"
        "# JIT settings\n"
        "jitfpu true\n"
        "jitcachesize 8192\n"
        "jitlazyflush true\n"
        "jitinline true\n"
        "jitdebug false\n"
        "\n"
        "# Display settings\n"
        "displaycolordepth 0\n"
        "frameskip 0\n"
        "scale_nearest false\n"
        "scale_integer false\n"
        "\n"
        "# Input settings\n"
        "keyboardtype 5\n"
        "keycodes false\n"
        "mousewheelmode 1\n"
        "mousewheellines 3\n"
        "swap_opt_cmd true\n"
        "hotkey 0\n"
        "\n"
        "# Serial/Network\n"
        "seriala /dev/null\n"
        "serialb /dev/null\n"
        "udptunnel false\n"
        "udpport 6066\n"
        "etherpermanentaddress true\n"
        "ethermulticastmode 0\n"
        "routerenabled false\n"
        "\n"
        "# Performance\n"
        "idlewait true\n";

    if (!write_prefs_file(prefs_path, minimal_prefs)) {
        fprintf(stderr, "Config: Failed to create minimal prefs file\n");
    }
}

// Read webcodec preference from prefs file
CodecType read_webcodec_pref(const std::string& prefs_path) {
    std::ifstream file(prefs_path);
    if (!file) {
        fprintf(stderr, "Config: No prefs file, defaulting to PNG codec\n");
        return CodecType::PNG;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        // Look for "webcodec" preference
        if (line.rfind("webcodec ", 0) == 0) {
            std::string value = line.substr(9);
            // Trim whitespace
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
                value.pop_back();
            }

            if (value == "h264" || value == "H264") {
                fprintf(stderr, "Config: webcodec = h264\n");
                return CodecType::H264;
            } else if (value == "av1" || value == "AV1") {
                fprintf(stderr, "Config: webcodec = av1\n");
                return CodecType::AV1;
            } else if (value == "vp9" || value == "VP9") {
                fprintf(stderr, "Config: webcodec = vp9\n");
                return CodecType::VP9;
            } else if (value == "png" || value == "PNG") {
                fprintf(stderr, "Config: webcodec = png\n");
                return CodecType::PNG;
            } else {
                fprintf(stderr, "Config: Unknown webcodec '%s', defaulting to PNG\n", value.c_str());
                return CodecType::PNG;
            }
        }
    }

    // Not found - default to PNG
    fprintf(stderr, "Config: webcodec not set, defaulting to PNG\n");
    return CodecType::PNG;
}

// Read mousemode preference from prefs file
std::string read_mousemode_pref(const std::string& prefs_path) {
    std::ifstream file(prefs_path);
    if (!file) {
        fprintf(stderr, "Config: No prefs file, defaulting to absolute mouse mode\n");
        return "absolute";
    }

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        // Look for "mousemode" preference
        if (line.rfind("mousemode ", 0) == 0) {
            std::string value = line.substr(10);
            // Trim whitespace
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
                value.pop_back();
            }

            if (value == "relative" || value == "absolute") {
                fprintf(stderr, "Config: mousemode = %s\n", value.c_str());
                return value;
            } else {
                fprintf(stderr, "Config: Unknown mousemode '%s', defaulting to absolute\n", value.c_str());
                return "absolute";
            }
        }
    }

    // Not found - default to absolute
    fprintf(stderr, "Config: mousemode not set, defaulting to absolute\n");
    return "absolute";
}

// Write mousemode preference to prefs file
bool write_mousemode_pref(const std::string& prefs_path, const std::string& mousemode) {
    // Validate mousemode
    if (mousemode != "relative" && mousemode != "absolute") {
        fprintf(stderr, "Config: Invalid mousemode '%s', must be 'relative' or 'absolute'\n", mousemode.c_str());
        return false;
    }

    // Read entire file
    std::string content = read_prefs_file(prefs_path);
    if (content.empty()) {
        fprintf(stderr, "Config: No prefs file to update\n");
        return false;
    }

    // Parse lines and update mousemode
    std::stringstream input(content);
    std::stringstream output;
    std::string line;
    bool found = false;

    while (std::getline(input, line)) {
        if (line.rfind("mousemode ", 0) == 0) {
            // Update existing mousemode line
            output << "mousemode " << mousemode << "\n";
            found = true;
        } else {
            output << line << "\n";
        }
    }

    // If mousemode wasn't found, add it after webcodec
    if (!found) {
        std::stringstream input2(content);
        output.str("");
        output.clear();

        while (std::getline(input2, line)) {
            output << line << "\n";
            if (line.rfind("webcodec ", 0) == 0) {
                output << "\n# Mouse mode for web streaming (relative or absolute)\n";
                output << "mousemode " << mousemode << "\n";
                found = true;
            }
        }
    }

    // Write back to file
    if (!write_prefs_file(prefs_path, output.str())) {
        fprintf(stderr, "Config: Failed to write mousemode preference\n");
        return false;
    }

    fprintf(stderr, "Config: Updated mousemode = %s\n", mousemode.c_str());
    return true;
}

} // namespace storage
