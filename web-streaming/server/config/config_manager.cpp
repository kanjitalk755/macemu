/*
 * Config Manager Implementation
 */

#include "config_manager.h"
#include "../utils/json_utils.h"
#include <fstream>
#include <sstream>
#include <cstdio>

namespace config {

// Load config from JSON file
MacemuConfig load_config(const std::string& path) {
    MacemuConfig cfg;

    std::ifstream file(path);
    if (!file) {
        fprintf(stderr, "Config: No config file at %s, using defaults\n", path.c_str());
        return cfg;  // Return defaults
    }

    try {
        auto j = json_utils::parse_file(path);

        // Web config
        if (j.contains("web")) {
            auto& web = j["web"];
            if (web.contains("emulator")) cfg.web.emulator = json_utils::get_string(web, "emulator");
            if (web.contains("codec")) cfg.web.codec = json_utils::get_string(web, "codec");
            if (web.contains("mousemode")) cfg.web.mousemode = json_utils::get_string(web, "mousemode");
        }

        // Common config
        if (j.contains("common")) {
            auto& common = j["common"];
            if (common.contains("ram")) cfg.common.ram = json_utils::get_int(common, "ram");
            if (common.contains("screen")) cfg.common.screen = json_utils::get_string(common, "screen");
            if (common.contains("sound")) cfg.common.sound = json_utils::get_bool(common, "sound");
            if (common.contains("extfs")) cfg.common.extfs = json_utils::get_string(common, "extfs");
        }

        // M68k config
        if (j.contains("m68k")) {
            auto& m68k = j["m68k"];
            if (m68k.contains("rom")) cfg.m68k.rom = json_utils::get_string(m68k, "rom");
            if (m68k.contains("modelid")) cfg.m68k.modelid = json_utils::get_int(m68k, "modelid");
            if (m68k.contains("cpu")) cfg.m68k.cpu = json_utils::get_int(m68k, "cpu");
            if (m68k.contains("fpu")) cfg.m68k.fpu = json_utils::get_bool(m68k, "fpu");
            if (m68k.contains("jit")) cfg.m68k.jit = json_utils::get_bool(m68k, "jit");
            if (m68k.contains("disks")) cfg.m68k.disks = json_utils::get_string_array(m68k, "disks");
            if (m68k.contains("cdroms")) cfg.m68k.cdroms = json_utils::get_string_array(m68k, "cdroms");
            if (m68k.contains("idlewait")) cfg.m68k.idlewait = json_utils::get_bool(m68k, "idlewait");
            if (m68k.contains("ignoresegv")) cfg.m68k.ignoresegv = json_utils::get_bool(m68k, "ignoresegv");
            if (m68k.contains("swap_opt_cmd")) cfg.m68k.swap_opt_cmd = json_utils::get_bool(m68k, "swap_opt_cmd");
            if (m68k.contains("keyboardtype")) cfg.m68k.keyboardtype = json_utils::get_int(m68k, "keyboardtype");
        }

        // PPC config
        if (j.contains("ppc")) {
            auto& ppc = j["ppc"];
            if (ppc.contains("rom")) cfg.ppc.rom = json_utils::get_string(ppc, "rom");
            if (ppc.contains("modelid")) cfg.ppc.modelid = json_utils::get_int(ppc, "modelid");
            if (ppc.contains("cpu")) cfg.ppc.cpu = json_utils::get_int(ppc, "cpu");
            if (ppc.contains("fpu")) cfg.ppc.fpu = json_utils::get_bool(ppc, "fpu");
            if (ppc.contains("jit")) cfg.ppc.jit = json_utils::get_bool(ppc, "jit");
            if (ppc.contains("jit68k")) cfg.ppc.jit68k = json_utils::get_bool(ppc, "jit68k");
            if (ppc.contains("disks")) cfg.ppc.disks = json_utils::get_string_array(ppc, "disks");
            if (ppc.contains("cdroms")) cfg.ppc.cdroms = json_utils::get_string_array(ppc, "cdroms");
            if (ppc.contains("idlewait")) cfg.ppc.idlewait = json_utils::get_bool(ppc, "idlewait");
            if (ppc.contains("ignoresegv")) cfg.ppc.ignoresegv = json_utils::get_bool(ppc, "ignoresegv");
            if (ppc.contains("ignoreillegal")) cfg.ppc.ignoreillegal = json_utils::get_bool(ppc, "ignoreillegal");
            if (ppc.contains("keyboardtype")) cfg.ppc.keyboardtype = json_utils::get_int(ppc, "keyboardtype");
        }

        fprintf(stderr, "Config: Loaded from %s (emulator=%s)\n", path.c_str(), cfg.web.emulator.c_str());

    } catch (const std::exception& e) {
        fprintf(stderr, "Config: Failed to parse JSON: %s\n", e.what());
        return cfg;  // Return defaults on parse error
    }

    return cfg;
}

// Save config to JSON file
bool save_config(const std::string& path, const MacemuConfig& config) {
    try {
        nlohmann::json j;

        j["version"] = config.version;

        // Web config
        j["web"]["emulator"] = config.web.emulator;
        j["web"]["codec"] = config.web.codec;
        j["web"]["mousemode"] = config.web.mousemode;

        // Common config
        j["common"]["ram"] = config.common.ram;
        j["common"]["screen"] = config.common.screen;
        j["common"]["sound"] = config.common.sound;
        j["common"]["extfs"] = config.common.extfs;

        // M68k config
        j["m68k"]["rom"] = config.m68k.rom;
        j["m68k"]["modelid"] = config.m68k.modelid;
        j["m68k"]["cpu"] = config.m68k.cpu;
        j["m68k"]["fpu"] = config.m68k.fpu;
        j["m68k"]["jit"] = config.m68k.jit;
        j["m68k"]["disks"] = config.m68k.disks;
        j["m68k"]["cdroms"] = config.m68k.cdroms;
        j["m68k"]["idlewait"] = config.m68k.idlewait;
        j["m68k"]["ignoresegv"] = config.m68k.ignoresegv;
        j["m68k"]["swap_opt_cmd"] = config.m68k.swap_opt_cmd;
        j["m68k"]["keyboardtype"] = config.m68k.keyboardtype;

        // PPC config
        j["ppc"]["rom"] = config.ppc.rom;
        j["ppc"]["modelid"] = config.ppc.modelid;
        j["ppc"]["cpu"] = config.ppc.cpu;
        j["ppc"]["fpu"] = config.ppc.fpu;
        j["ppc"]["jit"] = config.ppc.jit;
        j["ppc"]["jit68k"] = config.ppc.jit68k;
        j["ppc"]["disks"] = config.ppc.disks;
        j["ppc"]["cdroms"] = config.ppc.cdroms;
        j["ppc"]["idlewait"] = config.ppc.idlewait;
        j["ppc"]["ignoresegv"] = config.ppc.ignoresegv;
        j["ppc"]["ignoreillegal"] = config.ppc.ignoreillegal;
        j["ppc"]["keyboardtype"] = config.ppc.keyboardtype;

        // Write to file with nice formatting
        std::ofstream file(path);
        if (!file) {
            fprintf(stderr, "Config: Failed to open %s for writing\n", path.c_str());
            return false;
        }

        file << j.dump(2);  // 2-space indent
        file.close();

        fprintf(stderr, "Config: Saved to %s\n", path.c_str());
        return true;

    } catch (const std::exception& e) {
        fprintf(stderr, "Config: Failed to save: %s\n", e.what());
        return false;
    }
}

// Generate BasiliskII prefs file
std::string generate_basilisk_prefs(const MacemuConfig& config,
                                     const std::string& roms_path,
                                     const std::string& images_path) {
    std::ostringstream prefs;

    prefs << "# BasiliskII preferences - generated from macemu-config.json\n\n";

    // ROM
    if (!config.m68k.rom.empty()) {
        prefs << "rom " << roms_path << "/" << config.m68k.rom << "\n";
    } else {
        prefs << "rom \n";
    }
    prefs << "\n";

    // Disks
    for (const auto& disk : config.m68k.disks) {
        prefs << "disk " << images_path << "/" << disk << "\n";
    }
    if (!config.m68k.disks.empty()) prefs << "\n";

    // CD-ROMs
    for (const auto& cdrom : config.m68k.cdroms) {
        prefs << "cdrom " << images_path << "/" << cdrom << "\n";
    }
    if (!config.m68k.cdroms.empty()) prefs << "\n";

    // Screen (IPC mode for web streaming)
    prefs << "screen ipc/" << config.common.screen << "\n\n";

    // Hardware
    prefs << "modelid " << config.m68k.modelid << "\n";
    prefs << "cpu " << config.m68k.cpu << "\n";
    prefs << "fpu " << (config.m68k.fpu ? "true" : "false") << "\n";
    prefs << "jit " << (config.m68k.jit ? "true" : "false") << "\n";
    prefs << "ramsize " << (config.common.ram * 1024 * 1024) << "\n";
    prefs << "nosound " << (config.common.sound ? "false" : "true") << "\n\n";

    // Performance
    prefs << "idlewait " << (config.m68k.idlewait ? "true" : "false") << "\n";
    prefs << "ignoresegv " << (config.m68k.ignoresegv ? "true" : "false") << "\n\n";

    // Input
    prefs << "keyboardtype " << config.m68k.keyboardtype << "\n";
    prefs << "swap_opt_cmd " << (config.m68k.swap_opt_cmd ? "true" : "false") << "\n\n";

    // ExtFS
    prefs << "extfs " << config.common.extfs << "\n\n";

    // Web-specific (for emulator to read, but not used by BasiliskII core)
    prefs << "# Web streaming settings (read by IPC driver)\n";
    prefs << "webcodec " << config.web.codec << "\n";
    prefs << "mousemode " << config.web.mousemode << "\n\n";

    // Boot settings
    prefs << "bootdrive 0\n";
    prefs << "bootdriver 0\n\n";

    // Disable GUI (headless)
    prefs << "nogui true\n";

    return prefs.str();
}

// Generate SheepShaver prefs file
std::string generate_sheepshaver_prefs(const MacemuConfig& config,
                                        const std::string& roms_path,
                                        const std::string& images_path) {
    std::ostringstream prefs;

    prefs << "# SheepShaver preferences - generated from macemu-config.json\n\n";

    // ROM
    if (!config.ppc.rom.empty()) {
        prefs << "rom " << roms_path << "/" << config.ppc.rom << "\n";
    } else {
        prefs << "rom \n";
    }
    prefs << "\n";

    // Disks
    for (const auto& disk : config.ppc.disks) {
        prefs << "disk " << images_path << "/" << disk << "\n";
    }
    if (!config.ppc.disks.empty()) prefs << "\n";

    // CD-ROMs
    for (const auto& cdrom : config.ppc.cdroms) {
        prefs << "cdrom " << images_path << "/" << cdrom << "\n";
    }
    if (!config.ppc.cdroms.empty()) prefs << "\n";

    // Screen (IPC mode for web streaming)
    prefs << "screen ipc/" << config.common.screen << "\n\n";

    // Hardware (SheepShaver only uses ramsize, not modelid/cpu/fpu)
    prefs << "ramsize " << (config.common.ram * 1024 * 1024) << "\n\n";

    // JIT settings
    prefs << "jit " << (config.ppc.jit ? "true" : "false") << "\n";
    prefs << "jit68k " << (config.ppc.jit68k ? "true" : "false") << "\n\n";

    // Performance
    prefs << "idlewait " << (config.ppc.idlewait ? "true" : "false") << "\n";
    prefs << "ignoresegv " << (config.ppc.ignoresegv ? "true" : "false") << "\n";
    prefs << "ignoreillegal " << (config.ppc.ignoreillegal ? "true" : "false") << "\n\n";

    // Input
    prefs << "keyboardtype " << config.ppc.keyboardtype << "\n\n";

    // ExtFS
    prefs << "extfs " << config.common.extfs << "\n\n";

    // Boot settings
    prefs << "bootdrive 0\n";
    prefs << "bootdriver 0\n\n";

    // Disable GUI (headless)
    prefs << "nogui true\n";

    // Serial defaults
    prefs << "\n# Serial\n";
    prefs << "seriala /dev/null\n";
    prefs << "serialb /dev/null\n";

    return prefs.str();
}

} // namespace config
