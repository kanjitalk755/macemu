/*
 * API Handlers Module
 *
 * Implementation of all /api/ endpoints
 */

#include "api_handlers.h"
#include "../config/config_manager.h"
#include "../storage/file_scanner.h"
#include "../storage/prefs_manager.h"
#include "../utils/json_utils.h"
#include "../../BasiliskII/src/IPC/ipc_protocol.h"
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <fstream>
#include <pwd.h>
#include <unistd.h>

namespace http {

APIRouter::APIRouter(APIContext* context)
    : ctx_(context)
{}

Response APIRouter::handle(const Request& req, bool* handled) {
    *handled = false;

    // Check if this is an API route
    if (req.path.rfind("/api/", 0) != 0) {
        return Response::not_found();
    }

    *handled = true;

    // Route to handlers
    // New unified config API
    if (req.path == "/api/config" && req.method == "GET") {
        return handle_config_get(req);
    }
    if (req.path == "/api/config" && req.method == "POST") {
        return handle_config_save(req);
    }
    if (req.path == "/api/storage" && req.method == "GET") {
        return handle_storage(req);
    }
    if (req.path == "/api/prefs" && req.method == "GET") {
        return handle_prefs_get(req);
    }
    if (req.path == "/api/prefs" && req.method == "POST") {
        return handle_prefs_post(req);
    }
    if (req.path == "/api/restart" && req.method == "POST") {
        return handle_restart(req);
    }
    if (req.path == "/api/status" && req.method == "GET") {
        return handle_status(req);
    }
    if (req.path == "/api/codec" && req.method == "POST") {
        return handle_codec_post(req);
    }
    if (req.path == "/api/emulator" && req.method == "POST") {
        return handle_emulator_change(req);
    }
    if (req.path == "/api/emulator/start" && req.method == "POST") {
        return handle_emulator_start(req);
    }
    if (req.path == "/api/emulator/stop" && req.method == "POST") {
        return handle_emulator_stop(req);
    }
    if (req.path == "/api/emulator/restart" && req.method == "POST") {
        return handle_emulator_restart(req);
    }
    if (req.path == "/api/emulator/reset" && req.method == "POST") {
        return handle_emulator_reset(req);
    }
    if (req.path == "/api/log" && req.method == "POST") {
        return handle_log(req);
    }
    if (req.path == "/api/error" && req.method == "POST") {
        return handle_error(req);
    }

    // Unknown API endpoint
    Response resp;
    resp.set_status(404);
    resp.set_body("{\"error\": \"Unknown API endpoint\"}");
    resp.set_content_type("application/json");
    return resp;
}

Response APIRouter::handle_config(const Request& req) {
    // Parse prefs file to extract webcodec and mousemode
    std::string prefs_content = storage::read_prefs_file(ctx_->prefs_path);
    std::string webcodec = "h264";  // default
    std::string mousemode = "relative";  // default
    std::string resolution = "800x600";  // default

    // Parse prefs file line by line
    std::istringstream stream(prefs_content);
    std::string line;
    while (std::getline(stream, line)) {
        // Trim whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos || line[start] == '#') continue;

        size_t space = line.find(' ', start);
        if (space == std::string::npos) continue;

        std::string key = line.substr(start, space - start);
        std::string value = line.substr(space + 1);

        // Trim value
        size_t value_start = value.find_first_not_of(" \t\r\n");
        if (value_start != std::string::npos) {
            value = value.substr(value_start);
            size_t value_end = value.find_last_not_of(" \t\r\n");
            if (value_end != std::string::npos) {
                value = value.substr(0, value_end + 1);
            }
        }

        if (key == "webcodec") {
            webcodec = value;
        } else if (key == "mousemode") {
            mousemode = value;
        } else if (key == "screen") {
            // Parse "ipc/800/600" format
            size_t slash1 = value.find('/');
            if (slash1 != std::string::npos) {
                size_t slash2 = value.find('/', slash1 + 1);
                if (slash2 != std::string::npos) {
                    std::string w = value.substr(slash1 + 1, slash2 - slash1 - 1);
                    std::string h = value.substr(slash2 + 1);
                    resolution = w + "x" + h;
                }
            }
        }
    }

    std::ostringstream json;
    json << "{";
    json << "\"debug_connection\": " << (ctx_->debug_connection ? "true" : "false");
    json << ", \"debug_mode_switch\": " << (ctx_->debug_mode_switch ? "true" : "false");
    json << ", \"debug_perf\": " << (ctx_->debug_perf ? "true" : "false");
    json << ", \"webcodec\": \"" << storage::json_escape(webcodec) << "\"";
    json << ", \"mousemode\": \"" << storage::json_escape(mousemode) << "\"";
    json << ", \"resolution\": \"" << storage::json_escape(resolution) << "\"";
    json << "}";
    return Response::json(json.str());
}

Response APIRouter::handle_config_post(const Request& req) {
    // Parse JSON body
    auto j = json_utils::parse(req.body);

    // Check if mousemode is being updated
    if (j.contains("mousemode")) {
        std::string mousemode = json_utils::get_string(j, "mousemode");
        if (mousemode == "relative" || mousemode == "absolute") {
            if (storage::write_mousemode_pref(ctx_->prefs_path, mousemode)) {
                fprintf(stderr, "Config: Updated mousemode to '%s'\n", mousemode.c_str());
                return Response::json("{\"success\": true}");
            } else {
                return Response::json("{\"success\": false, \"error\": \"Failed to write mousemode\"}");
            }
        } else {
            return Response::json("{\"success\": false, \"error\": \"Invalid mousemode value\"}");
        }
    }

    return Response::json("{\"success\": false, \"error\": \"No valid config parameter provided\"}");
}

Response APIRouter::handle_storage(const Request& req) {
    std::string json_body = storage::get_storage_json(ctx_->roms_path, ctx_->images_path);
    return Response::json(json_body);
}

Response APIRouter::handle_prefs_get(const Request& req) {
    // Check if client requested a specific file via ?file=X query param
    std::string prefs_file = ctx_->prefs_path;
    size_t query_pos = req.path.find('?');
    if (query_pos != std::string::npos) {
        std::string query = req.path.substr(query_pos + 1);
        if (query.find("file=") == 0) {
            prefs_file = query.substr(5);
            fprintf(stderr, "📂 API GET /prefs?file=%s (requested specific file)\n", prefs_file.c_str());
        }
    } else {
        fprintf(stderr, "📂 API GET /prefs (default: %s)\n", prefs_file.c_str());
    }

    std::string prefs_content = storage::read_prefs_file(prefs_file);
    std::string json_body = "{\"content\": \"" + storage::json_escape(prefs_content) + "\", ";
    json_body += "\"file\": \"" + storage::json_escape(prefs_file) + "\", ";
    json_body += "\"path\": \"" + storage::json_escape(prefs_file) + "\", ";
    json_body += "\"romsPath\": \"" + storage::json_escape(ctx_->roms_path) + "\", ";
    json_body += "\"imagesPath\": \"" + storage::json_escape(ctx_->images_path) + "\"}";
    return Response::json(json_body);
}

Response APIRouter::handle_prefs_post(const Request& req) {
    auto j = json_utils::parse(req.body);
    std::string content = json_utils::get_string(j, "content");

    // Check if client specified which file to save to
    std::string prefs_file = json_utils::get_string(j, "file");
    if (prefs_file.empty()) {
        prefs_file = ctx_->prefs_path;  // Default to current prefs path
    }

    fprintf(stderr, "💾 API POST /prefs -> %s (content length=%zu)\n",
            prefs_file.c_str(), content.size());

    if (content.empty()) {
        fprintf(stderr, "⚠️  WARNING - prefs content is empty!\n");
        return Response::json("{\"success\": false, \"error\": \"Empty content\"}");
    }

    if (storage::write_prefs_file(prefs_file, content)) {
        fprintf(stderr, "✅ Saved prefs to %s\n", prefs_file.c_str());
        return Response::json("{\"success\": true}");
    } else {
        fprintf(stderr, "❌ Failed to write %s\n", prefs_file.c_str());
        return Response::json("{\"success\": false, \"error\": \"Failed to write prefs file\"}");
    }
}

Response APIRouter::handle_restart(const Request& req) {
    fprintf(stderr, "Server: Restart requested via API\n");
    if (ctx_->send_command_fn) {
        ctx_->send_command_fn(MACEMU_CMD_RESET);
    }
    return Response::json("{\"success\": true, \"message\": \"Restart sent to emulator\"}");
}

Response APIRouter::handle_status(const Request& req) {
    std::ostringstream json;
    json << "{";
    json << "\"emulator_connected\": " << (ctx_->emulator_connected ? "true" : "false");
    json << ", \"emulator_running\": " << (ctx_->started_emulator_pid > 0 ? "true" : "false");
    json << ", \"emulator_pid\": " << ctx_->emulator_pid;

    if (ctx_->ipc_shm) {
        json << ", \"video\": {\"width\": " << ctx_->ipc_shm->width;
        json << ", \"height\": " << ctx_->ipc_shm->height;
        json << ", \"frame_count\": " << ctx_->ipc_shm->frame_count;
        json << ", \"state\": " << ctx_->ipc_shm->state << "}";

        // Mouse latency from emulator (atomic - can be updated by stats thread)
        uint32_t latency_x10 = ATOMIC_LOAD(ctx_->ipc_shm->mouse_latency_avg_ms);
        uint32_t latency_samples = ATOMIC_LOAD(ctx_->ipc_shm->mouse_latency_samples);
        json << ", \"mouse_latency_ms\": " << std::fixed << std::setprecision(1) << (latency_x10 / 10.0);
        json << ", \"mouse_latency_samples\": " << latency_samples;
    }

    json << "}";
    return Response::json(json.str());
}

Response APIRouter::handle_emulator_start(const Request& req) {
    // Clear the "user stopped" flag - allow connection manager to scan/connect
    if (ctx_->user_stopped_emulator) {
        *ctx_->user_stopped_emulator = false;
    }

    std::string json_body;
    if (ctx_->started_emulator_pid > 0) {
        json_body = "{\"success\": false, \"message\": \"Emulator already running\", \"pid\": " +
                    std::to_string(ctx_->started_emulator_pid) + "}";
    } else if (ctx_->start_emulator_fn && ctx_->start_emulator_fn()) {
        json_body = "{\"success\": true, \"message\": \"Emulator started\", \"pid\": " +
                    std::to_string(ctx_->started_emulator_pid) + "}";
    } else {
        json_body = "{\"success\": false, \"message\": \"Failed to start emulator\"}";
    }
    return Response::json(json_body);
}

Response APIRouter::handle_emulator_stop(const Request& req) {
    // Set flag to prevent connection manager from auto-reconnecting
    if (ctx_->user_stopped_emulator) {
        *ctx_->user_stopped_emulator = true;
    }

    std::string json_body;
    if (ctx_->started_emulator_pid <= 0 && ctx_->emulator_pid <= 0) {
        json_body = "{\"success\": false, \"message\": \"Emulator not running\"}";
    } else {
        if (ctx_->started_emulator_pid > 0) {
            if (ctx_->stop_emulator_fn) {
                ctx_->stop_emulator_fn();
            }
        } else {
            // Just disconnect from external emulator
            if (ctx_->send_command_fn) {
                ctx_->send_command_fn(MACEMU_CMD_STOP);
            }
            if (ctx_->disconnect_emulator_fn) {
                ctx_->disconnect_emulator_fn();
            }
        }
        json_body = "{\"success\": true, \"message\": \"Emulator stopped\"}";
    }
    return Response::json(json_body);
}

Response APIRouter::handle_emulator_restart(const Request& req) {
    // Clear the "user stopped" flag - restart is an intentional start action
    if (ctx_->user_stopped_emulator) {
        *ctx_->user_stopped_emulator = false;
    }

    if (ctx_->request_restart_fn) {
        ctx_->request_restart_fn(true);
    }
    return Response::json("{\"success\": true, \"message\": \"Restart requested\"}");
}

Response APIRouter::handle_emulator_reset(const Request& req) {
    // Send RESET command to running emulator (soft reset, no restart)
    fprintf(stderr, "API: Reset requested via web UI\n");
    if (ctx_->send_command_fn) {
        ctx_->send_command_fn(MACEMU_CMD_RESET);
    }
    return Response::json("{\"success\": true, \"message\": \"Reset command sent\"}");
}

Response APIRouter::handle_log(const Request& req) {
    // Client logging endpoint - parse and display browser logs
    auto j = json_utils::parse(req.body);
    std::string level = json_utils::get_string(j, "level");
    std::string msg = json_utils::get_string(j, "message");
    std::string data = json_utils::get_string(j, "data");

    // Format: [Browser] level: message
    const char* prefix = "[Browser]";
    if (level == "error") {
        fprintf(stderr, "\033[31m%s ERROR: %s%s%s\033[0m\n", prefix, msg.c_str(),
                data.empty() ? "" : " | ", data.c_str());
    } else if (level == "warn") {
        fprintf(stderr, "\033[33m%s WARN: %s%s%s\033[0m\n", prefix, msg.c_str(),
                data.empty() ? "" : " | ", data.c_str());
    } else {
        fprintf(stderr, "%s %s: %s%s%s\n", prefix, level.c_str(), msg.c_str(),
                data.empty() ? "" : " | ", data.c_str());
    }

    return Response::json("{\"ok\": true}");
}

Response APIRouter::handle_error(const Request& req) {
    // Client error reporting endpoint - capture JavaScript errors, exceptions, and crashes
    auto j = json_utils::parse(req.body);
    std::string message = json_utils::get_string(j, "message");
    std::string stack = json_utils::get_string(j, "stack");
    std::string url = json_utils::get_string(j, "url");
    std::string line = json_utils::get_string(j, "line");
    std::string col = json_utils::get_string(j, "col");
    std::string type = json_utils::get_string(j, "type");

    // Format: [Browser ERROR] with red color for visibility
    fprintf(stderr, "\033[1;31m[Browser ERROR]\033[0m ");

    if (!type.empty()) {
        fprintf(stderr, "%s: ", type.c_str());
    }

    fprintf(stderr, "%s", message.c_str());

    if (!url.empty()) {
        fprintf(stderr, "\n  at %s", url.c_str());
        if (!line.empty()) {
            fprintf(stderr, ":%s", line.c_str());
            if (!col.empty()) {
                fprintf(stderr, ":%s", col.c_str());
            }
        }
    }

    if (!stack.empty()) {
        // Print stack trace with indentation
        fprintf(stderr, "\n  Stack trace:\n");
        // Split by newlines and indent each line
        size_t pos = 0;
        std::string stack_copy = stack;
        while ((pos = stack_copy.find('\n')) != std::string::npos) {
            std::string line_str = stack_copy.substr(0, pos);
            if (!line_str.empty()) {
                fprintf(stderr, "    %s\n", line_str.c_str());
            }
            stack_copy.erase(0, pos + 1);
        }
        if (!stack_copy.empty()) {
            fprintf(stderr, "    %s\n", stack_copy.c_str());
        }
    } else {
        fprintf(stderr, "\n");
    }

    return Response::json("{\"ok\": true}");
}

Response APIRouter::handle_emulator_change(const Request& req) {
    auto j = json_utils::parse(req.body);
    std::string emulator = json_utils::get_string(j, "emulator");

    if (emulator != "basilisk" && emulator != "sheepshaver") {
        return Response::json("{\"error\": \"Invalid emulator. Use 'basilisk' or 'sheepshaver'\"}");
    }

    fprintf(stderr, "API: Emulator change requested: %s\n", emulator.c_str());

    // For now, just acknowledge the request
    // TODO: Implement actual emulator switching with restart
    // This would require:
    // 1. Storing selected emulator in server config
    // 2. Stopping current emulator
    // 3. Updating binary path and prefs file
    // 4. Starting new emulator

    return Response::json("{\"ok\": true, \"message\": \"Emulator selection saved. Restart required.\"}");
}

Response APIRouter::handle_codec_post(const Request& req) {
    if (!ctx_->server_codec || !ctx_->notify_codec_change_fn) {
        return Response::json("{\"error\": \"Codec change not available\"}");
    }

    auto j = json_utils::parse(req.body);
    std::string codec_str = json_utils::get_string(j, "codec");

    CodecType new_codec;
    if (codec_str == "h264" || codec_str == "H264") {
        new_codec = CodecType::H264;
    } else if (codec_str == "av1" || codec_str == "AV1") {
        new_codec = CodecType::AV1;
    } else if (codec_str == "vp9" || codec_str == "VP9") {
        new_codec = CodecType::VP9;
    } else if (codec_str == "png" || codec_str == "PNG") {
        new_codec = CodecType::PNG;
    } else if (codec_str == "webp" || codec_str == "WEBP" || codec_str == "WebP") {
        new_codec = CodecType::WEBP;
    } else {
        return Response::json("{\"error\": \"Invalid codec. Use h264, av1, vp9, png, or webp\"}");
    }

    // Update codec
    CodecType old_codec = *ctx_->server_codec;
    *ctx_->server_codec = new_codec;

    fprintf(stderr, "Config: Codec changed from %d to %d via API\n", (int)old_codec, (int)new_codec);

    // Notify all clients (will send reconnect message via WebSocket)
    if (new_codec != old_codec) {
        ctx_->notify_codec_change_fn(new_codec);
    }

    return Response::json("{\"ok\": true}");
}

// ============================================================================
// New Unified Config API
// ============================================================================

Response APIRouter::handle_config_get(const Request& req) {
    // Load and return macemu-config.json
    config::MacemuConfig cfg = config::load_config("macemu-config.json");
    
    // Convert to JSON
    nlohmann::json j;
    j["version"] = cfg.version;
    
    // Web config
    j["web"]["emulator"] = cfg.web.emulator;
    j["web"]["codec"] = cfg.web.codec;
    j["web"]["mousemode"] = cfg.web.mousemode;
    
    // Common config
    j["common"]["ram"] = cfg.common.ram;
    j["common"]["screen"] = cfg.common.screen;
    j["common"]["sound"] = cfg.common.sound;
    j["common"]["extfs"] = cfg.common.extfs;
    
    // M68k config
    j["m68k"]["rom"] = cfg.m68k.rom;
    j["m68k"]["modelid"] = cfg.m68k.modelid;
    j["m68k"]["cpu"] = cfg.m68k.cpu;
    j["m68k"]["fpu"] = cfg.m68k.fpu;
    j["m68k"]["jit"] = cfg.m68k.jit;
    j["m68k"]["disks"] = cfg.m68k.disks;
    j["m68k"]["cdroms"] = cfg.m68k.cdroms;
    j["m68k"]["idlewait"] = cfg.m68k.idlewait;
    j["m68k"]["ignoresegv"] = cfg.m68k.ignoresegv;
    j["m68k"]["swap_opt_cmd"] = cfg.m68k.swap_opt_cmd;
    j["m68k"]["keyboardtype"] = cfg.m68k.keyboardtype;
    
    // PPC config
    j["ppc"]["rom"] = cfg.ppc.rom;
    j["ppc"]["modelid"] = cfg.ppc.modelid;
    j["ppc"]["cpu"] = cfg.ppc.cpu;
    j["ppc"]["fpu"] = cfg.ppc.fpu;
    j["ppc"]["jit"] = cfg.ppc.jit;
    j["ppc"]["jit68k"] = cfg.ppc.jit68k;
    j["ppc"]["disks"] = cfg.ppc.disks;
    j["ppc"]["cdroms"] = cfg.ppc.cdroms;
    j["ppc"]["idlewait"] = cfg.ppc.idlewait;
    j["ppc"]["ignoresegv"] = cfg.ppc.ignoresegv;
    j["ppc"]["ignoreillegal"] = cfg.ppc.ignoreillegal;
    j["ppc"]["keyboardtype"] = cfg.ppc.keyboardtype;
    
    // Add storage paths for client convenience
    j["_paths"]["roms"] = ctx_->roms_path;
    j["_paths"]["images"] = ctx_->images_path;
    
    return Response::json(j.dump(2));
}

Response APIRouter::handle_config_save(const Request& req) {
    // Parse incoming JSON
    auto j = json_utils::parse(req.body);
    
    // Build config struct from JSON
    config::MacemuConfig cfg;
    
    if (j.contains("version")) cfg.version = json_utils::get_int(j, "version");
    
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
    
    // Save to ~/.macemu/macemu-config.json
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    std::string config_path = std::string(home) + "/.macemu/macemu-config.json";

    if (!config::save_config(config_path, cfg)) {
        return Response::json("{\"success\": false, \"error\": \"Failed to save config file\"}");
    }

    fprintf(stderr, "✅ Config saved to %s (emulator=%s, codec=%s)\n",
            config_path.c_str(), cfg.web.emulator.c_str(), cfg.web.codec.c_str());

    // Regenerate prefs file so changes take effect immediately on restart
    std::string prefs_content;
    std::string prefs_file;

    if (cfg.web.emulator == "ppc") {
        prefs_content = config::generate_sheepshaver_prefs(cfg, ctx_->roms_path, ctx_->images_path);
        // SheepShaver reads from ~/.config/SheepShaver/prefs by default
        std::string config_dir = std::string(home) + "/.config/SheepShaver";
        prefs_file = config_dir + "/prefs";
    } else {
        // m68k -> BasiliskII
        prefs_content = config::generate_basilisk_prefs(cfg, ctx_->roms_path, ctx_->images_path);
        // BasiliskII uses --config flag, put prefs in ~/.config/BasiliskII/
        std::string config_dir = std::string(home) + "/.config/BasiliskII";
        prefs_file = config_dir + "/prefs";
    }

    if (!storage::write_prefs_file(prefs_file, prefs_content)) {
        fprintf(stderr, "⚠️  Warning: Failed to regenerate prefs file: %s\n", prefs_file.c_str());
        return Response::json("{\"success\": true, \"warning\": \"Config saved but prefs file not updated\"}");
    }

    fprintf(stderr, "✅ Regenerated prefs file: %s\n", prefs_file.c_str());

    return Response::json("{\"success\": true}");
}

} // namespace http
