/*
 * File Scanner Module
 *
 * Scans storage directories for ROMs, disk images, and CD-ROM images.
 * Builds JSON inventory of available files.
 */

#ifndef FILE_SCANNER_H
#define FILE_SCANNER_H

#include <string>
#include <vector>
#include <cstdint>
#include <sstream>

namespace storage {

struct FileInfo {
    std::string name;
    int64_t size;
    uint32_t checksum;  // First 4 bytes for ROM identification (deprecated)
    bool has_checksum;
    std::string md5;    // MD5 hash of entire file
};

/**
 * Scan directory for files with given extensions
 * @param directory Path to directory to scan
 * @param extensions List of file extensions (e.g., {".rom", ".img"})
 * @param read_checksums Whether to read ROM checksums (first 4 bytes)
 * @param recursive Whether to scan subdirectories
 * @return Vector of matching files, sorted by name
 */
std::vector<FileInfo> scan_directory(const std::string& directory,
                                     const std::vector<std::string>& extensions,
                                     bool read_checksums = false,
                                     bool recursive = false);

/**
 * Get complete storage inventory as JSON string
 * @param roms_path Path to ROMs directory
 * @param images_path Path to disk images directory
 * @return JSON string with all files
 */
std::string get_storage_json(const std::string& roms_path, const std::string& images_path);

/**
 * Escape string for JSON (basic implementation)
 */
std::string json_escape(const std::string& s);

} // namespace storage

#endif // FILE_SCANNER_H
