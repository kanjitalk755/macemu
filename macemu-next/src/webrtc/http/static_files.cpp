/*
 * Static File Handler Module
 *
 * Implementation of static file serving
 */

#include "static_files.h"
#include "../config/config_manager.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>

namespace http {

StaticFileHandler::StaticFileHandler(const std::string& root_dir)
    : root_dir_(root_dir)
{}

bool StaticFileHandler::handles(const std::string& path) const {
    // Handle root paths and known static files
    return path == "/" ||
           path == "/index.html" ||
           path == "/client.js" ||
           path == "/styles.css" ||
           path == "/Apple.svg" ||
           path == "/Motorola.svg" ||
           path == "/PowerPC.svg";
}

Response StaticFileHandler::serve(const std::string& path) {
    std::string file_path = map_path_to_file(path);
    if (file_path.empty()) {
        return Response::not_found();
    }

    // Read file
    std::ifstream file(file_path);
    if (!file.is_open()) {
        return Response::not_found();
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    // Template injection for index.html: embed config JSON to eliminate race conditions
    if (path == "/" || path == "/index.html") {
        content = inject_config_template(content);
        fprintf(stderr, "[HTTP] After injection, content size=%zu, has placeholder=%d\n",
                content.size(), content.find("{{CONFIG_JSON}}") != std::string::npos ? 1 : 0);
    }

    // Build response
    Response resp;
    resp.set_content_type(get_content_type(path));

    // Don't cache index.html since it contains dynamic config
    if (path == "/" || path == "/index.html") {
        resp.add_header("Cache-Control", "no-cache, no-store, must-revalidate");
        resp.add_header("Pragma", "no-cache");
        resp.add_header("Expires", "0");
    }

    resp.set_body(content);
    fprintf(stderr, "[HTTP] Serving %s with body size=%zu\n", path.c_str(), content.size());
    return resp;
}

std::string StaticFileHandler::map_path_to_file(const std::string& path) const {
    // Map URL paths to disk files
    if (path == "/" || path == "/index.html") {
        return root_dir_ + "/index.html";
    } else if (path == "/client.js") {
        return root_dir_ + "/client.js";
    } else if (path == "/styles.css") {
        return root_dir_ + "/styles.css";
    } else if (path == "/Apple.svg") {
        return root_dir_ + "/Apple.svg";
    } else if (path == "/Motorola.svg") {
        return root_dir_ + "/Motorola.svg";
    } else if (path == "/PowerPC.svg") {
        return root_dir_ + "/PowerPC.svg";
    }
    return "";  // Not found
}

std::string StaticFileHandler::get_content_type(const std::string& path) const {
    if (path.find(".html") != std::string::npos || path == "/") {
        return "text/html";
    } else if (path.find(".js") != std::string::npos) {
        return "application/javascript";
    } else if (path.find(".css") != std::string::npos) {
        return "text/css";
    } else if (path.find(".svg") != std::string::npos) {
        return "image/svg+xml";
    }
    return "text/plain";
}

std::string StaticFileHandler::inject_config_template(const std::string& html) const {
    // Load config from disk
    config::MacemuConfig cfg = config::load_config("macemu-config.json");

    // Build JSON (same structure as /api/config endpoint)
    nlohmann::json j;
    j["version"] = cfg.version;

    // Web config (what the client actually needs)
    j["webcodec"] = cfg.web.codec;           // Client expects "webcodec" key
    j["mousemode"] = cfg.web.mousemode;
    j["resolution"] = cfg.common.screen;

    // Debug flags (client expects these at top level)
    j["debug_connection"] = false;  // TODO: read from actual debug config if exists
    j["debug_mode_switch"] = false;
    j["debug_perf"] = false;

    // Full config structure (for backwards compatibility with existing client code)
    j["web"]["emulator"] = cfg.web.emulator;
    j["web"]["codec"] = cfg.web.codec;
    j["web"]["mousemode"] = cfg.web.mousemode;

    j["common"]["ram"] = cfg.common.ram;
    j["common"]["screen"] = cfg.common.screen;
    j["common"]["sound"] = cfg.common.sound;
    j["common"]["extfs"] = cfg.common.extfs;

    // Serialize to JSON string with pretty printing
    std::string config_json = j.dump(2);  // 2-space indent

    // Replace {{CONFIG_JSON}} placeholder
    std::string result = html;
    const std::string placeholder = "{{CONFIG_JSON}}";
    size_t pos = result.find(placeholder);

    fprintf(stderr, "[HTTP] inject_config_template: html size=%zu, looking for placeholder...\n", html.size());

    if (pos != std::string::npos) {
        result.replace(pos, placeholder.length(), config_json);
        fprintf(stderr, "[HTTP] Injected config into index.html at pos %zu (webcodec=%s, mousemode=%s, result size=%zu)\n",
                pos, cfg.web.codec.c_str(), cfg.web.mousemode.c_str(), result.size());
    } else {
        fprintf(stderr, "[HTTP] Warning: {{CONFIG_JSON}} placeholder not found in index.html (size=%zu)\n", html.size());
    }

    return result;
}

} // namespace http
