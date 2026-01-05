/*
 *  json_config.cpp - JSON configuration file handling
 *
 *  macemu-next (C) 2026
 */

#include "sysdeps.h"  // Must be first for int32 typedef
#include "json_config.h"
#include "prefs.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

using json = nlohmann::json;

// CPU name to type mapping
static int ParseCPUType(const std::string& cpu_str) {
	if (cpu_str == "68000") return 0;
	if (cpu_str == "68010") return 1;
	if (cpu_str == "68020") return 2;
	if (cpu_str == "68030") return 3;
	if (cpu_str == "68040") return 4;
	fprintf(stderr, "WARNING: Unknown CPU type '%s', defaulting to 68030\n", cpu_str.c_str());
	return 3;  // Default to 68030
}

static std::string CPUTypeToString(int cpu_type) {
	switch (cpu_type) {
		case 0: return "68000";
		case 1: return "68010";
		case 2: return "68020";
		case 3: return "68030";
		case 4: return "68040";
		default: return "68030";
	}
}

/*
 *  Get XDG config directory
 */
std::string GetXDGConfigDir() {
	const char* xdg_config = getenv("XDG_CONFIG_HOME");
	if (xdg_config && xdg_config[0] != '\0') {
		return std::string(xdg_config);
	}

	// Fallback to ~/.config
	const char* home = getenv("HOME");
	if (!home) {
		// Try getpwuid as final fallback
		struct passwd *pw = getpwuid(getuid());
		if (pw && pw->pw_dir) {
			home = pw->pw_dir;
		}
	}

	if (home) {
		return std::string(home) + "/.config";
	}

	return ".";  // Last resort: current directory
}

/*
 *  Get user config file path
 */
std::string GetUserConfigPath() {
	std::string config_dir = GetXDGConfigDir() + "/macemu-next";
	return config_dir + "/config.json";
}

/*
 *  Check if file exists
 */
static bool FileExists(const char *path) {
	struct stat st;
	return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

/*
 *  Find configuration file
 */
char* FindConfigFile(const char *cli_override) {
	// 1. CLI override (highest priority)
	if (cli_override && FileExists(cli_override)) {
		return strdup(cli_override);
	}

	// 2. User config (~/.config/macemu-next/config.json)
	std::string user_config = GetUserConfigPath();
	if (FileExists(user_config.c_str())) {
		return strdup(user_config.c_str());
	}

	// 3. Current directory
	const char *cwd_config = "./macemu-next.json";
	if (FileExists(cwd_config)) {
		return strdup(cwd_config);
	}

	// Not found
	return NULL;
}

/*
 *  Load JSON configuration
 */
bool LoadConfigJSON(const char *path) {
	printf("Loading configuration from: %s\n", path);

	// Read JSON file
	std::ifstream file(path);
	if (!file.is_open()) {
		fprintf(stderr, "ERROR: Cannot open config file: %s\n", path);
		return false;
	}

	json config;
	try {
		file >> config;
	} catch (json::parse_error& e) {
		fprintf(stderr, "ERROR: Failed to parse JSON config: %s\n", e.what());
		return false;
	}

	// Parse emulator section
	if (config.contains("emulator")) {
		auto emu = config["emulator"];

		if (emu.contains("cpu") && emu["cpu"].is_string()) {
			int cpu_type = ParseCPUType(emu["cpu"]);
			PrefsReplaceInt32("cpu", cpu_type);
		}
		if (emu.contains("fpu") && emu["fpu"].is_boolean()) {
			PrefsReplaceBool("fpu", emu["fpu"]);
		}
		if (emu.contains("ramsize") && emu["ramsize"].is_number()) {
			PrefsReplaceInt32("ramsize", emu["ramsize"]);
		}
		if (emu.contains("modelid") && emu["modelid"].is_number()) {
			PrefsReplaceInt32("modelid", emu["modelid"]);
		}
	}

	// Parse boot section
	if (config.contains("boot")) {
		auto boot = config["boot"];
		if (boot.contains("bootdrive") && boot["bootdrive"].is_number()) {
			PrefsReplaceInt32("bootdrive", boot["bootdrive"]);
		}
		if (boot.contains("bootdriver") && boot["bootdriver"].is_number()) {
			PrefsReplaceInt32("bootdriver", boot["bootdriver"]);
		}
	}

	// Parse storage section
	if (config.contains("storage")) {
		auto storage = config["storage"];

		if (storage.contains("rom") && storage["rom"].is_string()) {
			PrefsReplaceString("rom", storage["rom"].get<std::string>().c_str());
		}

		if (storage.contains("disks") && storage["disks"].is_array()) {
			// Remove existing disk prefs
			for (int i = 0; PrefsFindString("disk", i) != NULL; i++) {
				PrefsRemoveItem("disk", 0);
			}
			// Add new disks
			for (auto& disk : storage["disks"]) {
				if (disk.is_string()) {
					PrefsAddString("disk", disk.get<std::string>().c_str());
				}
			}
		}

		if (storage.contains("floppies") && storage["floppies"].is_array()) {
			for (int i = 0; PrefsFindString("floppy", i) != NULL; i++) {
				PrefsRemoveItem("floppy", 0);
			}
			for (auto& floppy : storage["floppies"]) {
				if (floppy.is_string()) {
					PrefsAddString("floppy", floppy.get<std::string>().c_str());
				}
			}
		}

		if (storage.contains("cdroms") && storage["cdroms"].is_array()) {
			for (int i = 0; PrefsFindString("cdrom", i) != NULL; i++) {
				PrefsRemoveItem("cdrom", 0);
			}
			for (auto& cdrom : storage["cdroms"]) {
				if (cdrom.is_string()) {
					PrefsAddString("cdrom", cdrom.get<std::string>().c_str());
				}
			}
		}

		if (storage.contains("extfs") && !storage["extfs"].is_null()) {
			PrefsReplaceString("extfs", storage["extfs"].get<std::string>().c_str());
		}

		// SCSI devices
		if (storage.contains("scsi") && storage["scsi"].is_object()) {
			for (int i = 0; i < 7; i++) {
				std::string key = std::to_string(i);
				if (storage["scsi"].contains(key) && !storage["scsi"][key].is_null()) {
					char pref_name[16];
					snprintf(pref_name, sizeof(pref_name), "scsi%d", i);
					PrefsReplaceString(pref_name, storage["scsi"][key].get<std::string>().c_str());
				}
			}
		}
	}

	// Parse video section
	if (config.contains("video")) {
		auto video = config["video"];
		if (video.contains("displaycolordepth") && video["displaycolordepth"].is_number()) {
			PrefsReplaceInt32("displaycolordepth", video["displaycolordepth"]);
		}
		if (video.contains("screen") && !video["screen"].is_null()) {
			PrefsReplaceString("screen", video["screen"].get<std::string>().c_str());
		}
		if (video.contains("frameskip") && video["frameskip"].is_number()) {
			PrefsReplaceInt32("frameskip", video["frameskip"]);
		}
		if (video.contains("scale_nearest") && video["scale_nearest"].is_boolean()) {
			PrefsReplaceBool("scale_nearest", video["scale_nearest"]);
		}
		if (video.contains("scale_integer") && video["scale_integer"].is_boolean()) {
			PrefsReplaceBool("scale_integer", video["scale_integer"]);
		}
	}

	// Parse audio section
	if (config.contains("audio")) {
		auto audio = config["audio"];
		if (audio.contains("nosound") && audio["nosound"].is_boolean()) {
			PrefsReplaceBool("nosound", audio["nosound"]);
		}
		if (audio.contains("sound_buffer") && audio["sound_buffer"].is_number()) {
			PrefsReplaceInt32("sound_buffer", audio["sound_buffer"]);
		}
	}

	// Parse input section
	if (config.contains("input")) {
		auto input = config["input"];
		if (input.contains("keyboardtype") && input["keyboardtype"].is_number()) {
			PrefsReplaceInt32("keyboardtype", input["keyboardtype"]);
		}
		if (input.contains("keycodes") && input["keycodes"].is_boolean()) {
			PrefsReplaceBool("keycodes", input["keycodes"]);
		}
		if (input.contains("keycodefile") && !input["keycodefile"].is_null()) {
			PrefsReplaceString("keycodefile", input["keycodefile"].get<std::string>().c_str());
		}
		if (input.contains("mousewheelmode") && input["mousewheelmode"].is_number()) {
			PrefsReplaceInt32("mousewheelmode", input["mousewheelmode"]);
		}
		if (input.contains("mousewheellines") && input["mousewheellines"].is_number()) {
			PrefsReplaceInt32("mousewheellines", input["mousewheellines"]);
		}
		if (input.contains("hotkey") && input["hotkey"].is_number()) {
			PrefsReplaceInt32("hotkey", input["hotkey"]);
		}
		if (input.contains("swap_opt_cmd") && input["swap_opt_cmd"].is_boolean()) {
			PrefsReplaceBool("swap_opt_cmd", input["swap_opt_cmd"]);
		}
		if (input.contains("init_grab") && input["init_grab"].is_boolean()) {
			PrefsReplaceBool("init_grab", input["init_grab"]);
		}
	}

	// Parse network section
	if (config.contains("network")) {
		auto network = config["network"];
		if (network.contains("ether") && !network["ether"].is_null()) {
			PrefsReplaceString("ether", network["ether"].get<std::string>().c_str());
		}
		if (network.contains("etherconfig") && !network["etherconfig"].is_null()) {
			PrefsReplaceString("etherconfig", network["etherconfig"].get<std::string>().c_str());
		}
		if (network.contains("udptunnel") && network["udptunnel"].is_boolean()) {
			PrefsReplaceBool("udptunnel", network["udptunnel"]);
		}
		if (network.contains("udpport") && network["udpport"].is_number()) {
			PrefsReplaceInt32("udpport", network["udpport"]);
		}

		if (network.contains("redir") && network["redir"].is_array()) {
			for (int i = 0; PrefsFindString("redir", i) != NULL; i++) {
				PrefsRemoveItem("redir", 0);
			}
			for (auto& redir : network["redir"]) {
				if (redir.is_string()) {
					PrefsAddString("redir", redir.get<std::string>().c_str());
				}
			}
		}

		if (network.contains("host_domain") && network["host_domain"].is_array()) {
			for (int i = 0; PrefsFindString("host_domain", i) != NULL; i++) {
				PrefsRemoveItem("host_domain", 0);
			}
			for (auto& domain : network["host_domain"]) {
				if (domain.is_string()) {
					PrefsAddString("host_domain", domain.get<std::string>().c_str());
				}
			}
		}
	}

	// Parse serial section
	if (config.contains("serial")) {
		auto serial = config["serial"];
		if (serial.contains("seriala") && !serial["seriala"].is_null()) {
			PrefsReplaceString("seriala", serial["seriala"].get<std::string>().c_str());
		}
		if (serial.contains("serialb") && !serial["serialb"].is_null()) {
			PrefsReplaceString("serialb", serial["serialb"].get<std::string>().c_str());
		}
	}

	// Parse JIT section
	if (config.contains("jit")) {
		auto jit = config["jit"];
		if (jit.contains("enabled") && jit["enabled"].is_boolean()) {
			PrefsReplaceBool("jit", jit["enabled"]);
		}
		if (jit.contains("fpu") && jit["fpu"].is_boolean()) {
			PrefsReplaceBool("jitfpu", jit["fpu"]);
		}
		if (jit.contains("debug") && jit["debug"].is_boolean()) {
			PrefsReplaceBool("jitdebug", jit["debug"]);
		}
		if (jit.contains("cachesize") && jit["cachesize"].is_number()) {
			PrefsReplaceInt32("jitcachesize", jit["cachesize"]);
		}
		if (jit.contains("lazyflush") && jit["lazyflush"].is_boolean()) {
			PrefsReplaceBool("jitlazyflush", jit["lazyflush"]);
		}
		if (jit.contains("inline") && jit["inline"].is_boolean()) {
			PrefsReplaceBool("jitinline", jit["inline"]);
		}
		if (jit.contains("blacklist") && !jit["blacklist"].is_null()) {
			PrefsReplaceString("jitblacklist", jit["blacklist"].get<std::string>().c_str());
		}
	}

	// Parse UI section
	if (config.contains("ui")) {
		auto ui = config["ui"];
		if (ui.contains("nogui") && ui["nogui"].is_boolean()) {
			PrefsReplaceBool("nogui", ui["nogui"]);
		}
		if (ui.contains("noclipconversion") && ui["noclipconversion"].is_boolean()) {
			PrefsReplaceBool("noclipconversion", ui["noclipconversion"]);
		}
		if (ui.contains("title") && !ui["title"].is_null()) {
			PrefsReplaceString("title", ui["title"].get<std::string>().c_str());
		}
		if (ui.contains("gammaramp") && !ui["gammaramp"].is_null()) {
			PrefsReplaceString("gammaramp", ui["gammaramp"].get<std::string>().c_str());
		}
	}

	// Parse system section
	if (config.contains("system")) {
		auto sys = config["system"];
		if (sys.contains("nocdrom") && sys["nocdrom"].is_boolean()) {
			PrefsReplaceBool("nocdrom", sys["nocdrom"]);
		}
		if (sys.contains("ignoresegv") && sys["ignoresegv"].is_boolean()) {
			PrefsReplaceBool("ignoresegv", sys["ignoresegv"]);
		}
		if (sys.contains("delay") && sys["delay"].is_number()) {
			PrefsReplaceInt32("delay", sys["delay"]);
		}
		if (sys.contains("yearofs") && sys["yearofs"].is_number()) {
			PrefsReplaceInt32("yearofs", sys["yearofs"]);
		}
		if (sys.contains("dayofs") && sys["dayofs"].is_number()) {
			PrefsReplaceInt32("dayofs", sys["dayofs"]);
		}
		if (sys.contains("mag_rate") && !sys["mag_rate"].is_null()) {
			PrefsReplaceString("mag_rate", sys["mag_rate"].get<std::string>().c_str());
		}
		if (sys.contains("name_encoding") && sys["name_encoding"].is_number()) {
			PrefsReplaceInt32("name_encoding", sys["name_encoding"]);
		}
		if (sys.contains("xpram") && !sys["xpram"].is_null()) {
			PrefsReplaceString("xpram", sys["xpram"].get<std::string>().c_str());
		}
	}

	printf("Configuration loaded successfully\n");
	return true;
}

/*
 *  Save JSON configuration
 */
bool SaveConfigJSON(const char *path) {
	std::string output_path;

	// Use provided path or default to user config
	if (path) {
		output_path = path;
	} else {
		output_path = GetUserConfigPath();

		// Create config directory if it doesn't exist
		std::string config_dir = GetXDGConfigDir() + "/macemu-next";
		mkdir(config_dir.c_str(), 0755);
	}

	json config;
	config["version"] = "1.0";

	// Emulator section
	config["emulator"]["cpu"] = CPUTypeToString(PrefsFindInt32("cpu"));
	config["emulator"]["fpu"] = PrefsFindBool("fpu");
	config["emulator"]["ramsize"] = PrefsFindInt32("ramsize");
	config["emulator"]["modelid"] = PrefsFindInt32("modelid");

	// Boot section
	config["boot"]["bootdrive"] = PrefsFindInt32("bootdrive");
	config["boot"]["bootdriver"] = PrefsFindInt32("bootdriver");

	// Storage section
	const char* rom = PrefsFindString("rom");
	config["storage"]["rom"] = rom ? json(rom) : json(nullptr);

	config["storage"]["disks"] = json::array();
	for (int i = 0; ; i++) {
		const char* disk = PrefsFindString("disk", i);
		if (!disk) break;
		config["storage"]["disks"].push_back(disk);
	}

	config["storage"]["floppies"] = json::array();
	for (int i = 0; ; i++) {
		const char* floppy = PrefsFindString("floppy", i);
		if (!floppy) break;
		config["storage"]["floppies"].push_back(floppy);
	}

	config["storage"]["cdroms"] = json::array();
	for (int i = 0; ; i++) {
		const char* cdrom = PrefsFindString("cdrom", i);
		if (!cdrom) break;
		config["storage"]["cdroms"].push_back(cdrom);
	}

	config["storage"]["scsi"] = json::object();
	for (int i = 0; i < 7; i++) {
		char pref_name[16];
		snprintf(pref_name, sizeof(pref_name), "scsi%d", i);
		const char* scsi = PrefsFindString(pref_name);
		config["storage"]["scsi"][std::to_string(i)] = scsi ? json(scsi) : json(nullptr);
	}

	const char* extfs = PrefsFindString("extfs");
	config["storage"]["extfs"] = extfs ? json(extfs) : json(nullptr);

	// Video section
	config["video"]["displaycolordepth"] = PrefsFindInt32("displaycolordepth");
	const char* screen = PrefsFindString("screen");
	config["video"]["screen"] = screen ? json(screen) : json(nullptr);
	config["video"]["frameskip"] = PrefsFindInt32("frameskip");
	config["video"]["scale_nearest"] = PrefsFindBool("scale_nearest");
	config["video"]["scale_integer"] = PrefsFindBool("scale_integer");

	// Audio section
	config["audio"]["nosound"] = PrefsFindBool("nosound");
	config["audio"]["sound_buffer"] = PrefsFindInt32("sound_buffer");

	// Input section
	config["input"]["keyboardtype"] = PrefsFindInt32("keyboardtype");
	config["input"]["keycodes"] = PrefsFindBool("keycodes");
	const char* keycodefile = PrefsFindString("keycodefile");
	config["input"]["keycodefile"] = keycodefile ? json(keycodefile) : json(nullptr);
	config["input"]["mousewheelmode"] = PrefsFindInt32("mousewheelmode");
	config["input"]["mousewheellines"] = PrefsFindInt32("mousewheellines");
	config["input"]["hotkey"] = PrefsFindInt32("hotkey");
	config["input"]["swap_opt_cmd"] = PrefsFindBool("swap_opt_cmd");
	config["input"]["init_grab"] = PrefsFindBool("init_grab");

	// Network section
	const char* ether = PrefsFindString("ether");
	config["network"]["ether"] = ether ? json(ether) : json(nullptr);
	const char* etherconfig = PrefsFindString("etherconfig");
	config["network"]["etherconfig"] = etherconfig ? json(etherconfig) : json(nullptr);
	config["network"]["udptunnel"] = PrefsFindBool("udptunnel");
	config["network"]["udpport"] = PrefsFindInt32("udpport");

	config["network"]["redir"] = json::array();
	for (int i = 0; ; i++) {
		const char* redir = PrefsFindString("redir", i);
		if (!redir) break;
		config["network"]["redir"].push_back(redir);
	}

	config["network"]["host_domain"] = json::array();
	for (int i = 0; ; i++) {
		const char* domain = PrefsFindString("host_domain", i);
		if (!domain) break;
		config["network"]["host_domain"].push_back(domain);
	}

	// Serial section
	const char* seriala = PrefsFindString("seriala");
	config["serial"]["seriala"] = seriala ? json(seriala) : json(nullptr);
	const char* serialb = PrefsFindString("serialb");
	config["serial"]["serialb"] = serialb ? json(serialb) : json(nullptr);

	// JIT section
	config["jit"]["enabled"] = PrefsFindBool("jit");
	config["jit"]["fpu"] = PrefsFindBool("jitfpu");
	config["jit"]["debug"] = PrefsFindBool("jitdebug");
	config["jit"]["cachesize"] = PrefsFindInt32("jitcachesize");
	config["jit"]["lazyflush"] = PrefsFindBool("jitlazyflush");
	config["jit"]["inline"] = PrefsFindBool("jitinline");
	const char* jitblacklist = PrefsFindString("jitblacklist");
	config["jit"]["blacklist"] = jitblacklist ? json(jitblacklist) : json(nullptr);

	// UI section
	config["ui"]["nogui"] = PrefsFindBool("nogui");
	config["ui"]["noclipconversion"] = PrefsFindBool("noclipconversion");
	const char* title = PrefsFindString("title");
	config["ui"]["title"] = title ? json(title) : json(nullptr);
	const char* gammaramp = PrefsFindString("gammaramp");
	config["ui"]["gammaramp"] = gammaramp ? json(gammaramp) : json(nullptr);

	// System section
	config["system"]["nocdrom"] = PrefsFindBool("nocdrom");
	config["system"]["ignoresegv"] = PrefsFindBool("ignoresegv");
	config["system"]["delay"] = PrefsFindInt32("delay");
	config["system"]["yearofs"] = PrefsFindInt32("yearofs");
	config["system"]["dayofs"] = PrefsFindInt32("dayofs");
	const char* mag_rate = PrefsFindString("mag_rate");
	config["system"]["mag_rate"] = mag_rate ? json(mag_rate) : json(nullptr);
	config["system"]["name_encoding"] = PrefsFindInt32("name_encoding");
	const char* xpram = PrefsFindString("xpram");
	config["system"]["xpram"] = xpram ? json(xpram) : json(nullptr);

	// Write to file
	std::ofstream file(output_path);
	if (!file.is_open()) {
		fprintf(stderr, "ERROR: Cannot write config file: %s\n", output_path.c_str());
		return false;
	}

	file << config.dump(2);  // Pretty-print with 2-space indent
	file.close();

	printf("Configuration saved to: %s\n", output_path.c_str());
	return true;
}
