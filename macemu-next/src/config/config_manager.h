/*
 * Config Manager Module
 *
 * Handles unified JSON config file (macemu-config.json) for both emulators
 * and web client settings. Replaces separate .prefs files.
 */

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <string>
#include <vector>

namespace config {

struct CommonConfig {
    int ram = 64;              // RAM in MB
    std::string screen = "1024x768";
    bool sound = true;
    std::string extfs = "./storage";
};

struct M68kConfig {
    std::string rom;
    int modelid = 14;
    int cpu = 4;
    bool fpu = true;
    bool jit = true;
    std::vector<std::string> disks;
    std::vector<std::string> cdroms;
    bool idlewait = true;
    bool ignoresegv = true;
    bool swap_opt_cmd = true;
    int keyboardtype = 5;
};

struct PPCConfig {
    std::string rom;
    int modelid = 14;
    int cpu = 4;
    bool fpu = true;
    bool jit = true;
    bool jit68k = true;
    std::vector<std::string> disks;
    std::vector<std::string> cdroms;
    bool idlewait = true;
    bool ignoresegv = true;
    bool ignoreillegal = true;
    int keyboardtype = 5;
};

struct WebConfig {
    std::string emulator = "m68k";  // "m68k" or "ppc"
    std::string codec = "h264";
    std::string mousemode = "relative";
};

struct MacemuConfig {
    int version = 1;
    WebConfig web;
    CommonConfig common;
    M68kConfig m68k;
    PPCConfig ppc;
};

/**
 * Load config from JSON file
 * @param path Path to macemu-config.json
 * @return Parsed config, or default config if file doesn't exist
 */
MacemuConfig load_config(const std::string& path);

/**
 * Save config to JSON file
 * @param path Path to macemu-config.json
 * @param config Config to save
 * @return true if successful
 */
bool save_config(const std::string& path, const MacemuConfig& config);

/**
 * Generate BasiliskII prefs file from config
 * @param config Source config
 * @param roms_path Base path for ROMs
 * @param images_path Base path for disk images
 * @return Prefs file content
 */
std::string generate_basilisk_prefs(const MacemuConfig& config,
                                     const std::string& roms_path,
                                     const std::string& images_path);

/**
 * Generate SheepShaver prefs file from config
 * @param config Source config
 * @param roms_path Base path for ROMs
 * @param images_path Base path for disk images
 * @return Prefs file content
 */
std::string generate_sheepshaver_prefs(const MacemuConfig& config,
                                        const std::string& roms_path,
                                        const std::string& images_path);

} // namespace config

#endif // CONFIG_MANAGER_H
