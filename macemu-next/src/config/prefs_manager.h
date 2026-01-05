/*
 * Prefs Manager Module
 *
 * Handles reading, writing, and creating Basilisk II/SheepShaver preferences files.
 * Also parses the webcodec preference for server-side codec selection.
 */

#ifndef PREFS_MANAGER_H
#define PREFS_MANAGER_H

#include <string>
#include "../codec.h"

namespace storage {

/**
 * Read prefs file content
 * @param prefs_path Path to prefs file
 * @return File content, or empty string if file doesn't exist
 */
std::string read_prefs_file(const std::string& prefs_path);

/**
 * Write prefs file content
 * @param prefs_path Path to prefs file
 * @param content Content to write
 * @return true if successful, false otherwise
 */
bool write_prefs_file(const std::string& prefs_path, const std::string& content);

/**
 * Create minimal prefs file if it doesn't exist
 * @param prefs_path Path to prefs file
 */
void create_minimal_prefs_if_needed(const std::string& prefs_path);

/**
 * Read webcodec preference from prefs file
 * @param prefs_path Path to prefs file
 * @return Codec type (defaults to PNG if not found or invalid)
 */
CodecType read_webcodec_pref(const std::string& prefs_path);

/**
 * Read mousemode preference from prefs file
 * @param prefs_path Path to prefs file
 * @return Mouse mode string: "relative" or "absolute" (defaults to "absolute")
 */
std::string read_mousemode_pref(const std::string& prefs_path);

/**
 * Write mousemode preference to prefs file
 * @param prefs_path Path to prefs file
 * @param mousemode Mouse mode: "relative" or "absolute"
 * @return true if successful, false otherwise
 */
bool write_mousemode_pref(const std::string& prefs_path, const std::string& mousemode);

} // namespace storage

#endif // PREFS_MANAGER_H
