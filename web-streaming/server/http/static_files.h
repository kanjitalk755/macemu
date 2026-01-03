/*
 * Static File Handler Module
 *
 * Serves static files (HTML, JS, CSS) from disk
 */

#ifndef STATIC_FILES_H
#define STATIC_FILES_H

#include "http_server.h"
#include <string>

namespace http {

/**
 * Static File Handler
 *
 * Serves files from a directory with appropriate content types
 */
class StaticFileHandler {
public:
    explicit StaticFileHandler(const std::string& root_dir);

    // Try to serve a static file for the given path
    // Returns empty response if file not found (check with is_valid())
    Response serve(const std::string& path);

    // Check if path is handled by static file handler
    bool handles(const std::string& path) const;

private:
    std::string map_path_to_file(const std::string& path) const;
    std::string get_content_type(const std::string& path) const;
    std::string inject_config_template(const std::string& html) const;

    std::string root_dir_;
};

} // namespace http

#endif // STATIC_FILES_H
