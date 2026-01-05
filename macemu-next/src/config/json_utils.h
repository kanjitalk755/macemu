/*
 * JSON Utilities
 *
 * Wrapper around nlohmann/json for common operations.
 * Replaces hand-written JSON parsing/escaping from server.cpp.
 */

#ifndef JSON_UTILS_H
#define JSON_UTILS_H

#include <nlohmann/json.hpp>
#include <string>

namespace json_utils {

// Type alias for convenience
using json = nlohmann::json;

/**
 * Parse JSON string
 * @param str JSON string
 * @return Parsed JSON object
 * @throws nlohmann::json::parse_error on invalid JSON
 */
json parse(const std::string& str);

/**
 * Convert JSON to string
 * @param j JSON object
 * @param indent Indentation level (-1 for compact)
 * @return JSON string representation
 */
std::string to_string(const json& j, int indent = -1);

/**
 * Get string value from JSON object with default
 * @param j JSON object
 * @param key Key to retrieve
 * @param default_val Default value if key doesn't exist or is not a string
 * @return String value or default
 */
std::string get_string(const json& j, const std::string& key,
                       const std::string& default_val = "");

/**
 * Get integer value from JSON object with default
 * @param j JSON object
 * @param key Key to retrieve
 * @param default_val Default value if key doesn't exist or is not an integer
 * @return Integer value or default
 */
int get_int(const json& j, const std::string& key, int default_val = 0);

/**
 * Get boolean value from JSON object with default
 * @param j JSON object
 * @param key Key to retrieve
 * @param default_val Default value if key doesn't exist or is not a boolean
 * @return Boolean value or default
 */
bool get_bool(const json& j, const std::string& key, bool default_val = false);

/**
 * Check if key exists in JSON object
 * @param j JSON object
 * @param key Key to check
 * @return True if key exists
 */
bool has_key(const json& j, const std::string& key);

/**
 * Get string array from JSON object
 * @param j JSON object
 * @param key Key to retrieve
 * @return Vector of strings, or empty vector if key doesn't exist or is not an array
 */
std::vector<std::string> get_string_array(const json& j, const std::string& key);

/**
 * Parse JSON file
 * @param path Path to JSON file
 * @return Parsed JSON object
 * @throws std::exception on file read or parse error
 */
json parse_file(const std::string& path);

} // namespace json_utils

#endif // JSON_UTILS_H
