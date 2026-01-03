/*
 * File Scanner Module
 *
 * Scans storage directories for ROM files, disk images, and CD-ROMs
 * Provides file metadata including size and ROM checksums
 */

#include "file_scanner.h"
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstdio>
#include <openssl/md5.h>
#include <sstream>
#include <iomanip>

namespace storage {

// Helper: Check if filename has one of the given extensions
static bool has_extension(const std::string& filename, const std::vector<std::string>& extensions) {
    size_t dot = filename.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = filename.substr(dot);
    for (auto& c : ext) c = tolower(c);
    for (const auto& e : extensions) {
        if (ext == e) return true;
    }
    return false;
}

// Helper: Read first 4 bytes of ROM file as checksum (big-endian)
static uint32_t read_rom_checksum(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    uint8_t buf[4];
    if (fread(buf, 1, 4, f) != 4) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

// Helper: Calculate MD5 hash of entire file
static std::string calculate_md5(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";

    MD5_CTX md5_ctx;
    MD5_Init(&md5_ctx);

    unsigned char buffer[4096];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        MD5_Update(&md5_ctx, buffer, bytes_read);
    }

    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5_Final(digest, &md5_ctx);
    fclose(f);

    // Convert to hex string
    std::ostringstream hex;
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        hex << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    }

    return hex.str();
}

// Recursive directory scanning
static void scan_directory_recursive(const std::string& base_dir, const std::string& relative_path,
                                     const std::vector<std::string>& extensions, bool read_checksums,
                                     std::vector<FileInfo>& files) {
    std::string current_dir = relative_path.empty() ? base_dir : base_dir + "/" + relative_path;

    DIR* dir = opendir(current_dir.c_str());
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        std::string name = entry->d_name;
        std::string full_path = current_dir + "/" + name;
        std::string rel_name = relative_path.empty() ? name : relative_path + "/" + name;

        struct stat st;
        if (stat(full_path.c_str(), &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_directory_recursive(base_dir, rel_name, extensions, read_checksums, files);
        } else if (S_ISREG(st.st_mode)) {
            if (has_extension(name, extensions)) {
                FileInfo info;
                info.name = rel_name;
                info.size = st.st_size;
                info.checksum = 0;
                info.has_checksum = false;
                info.md5 = "";

                if (read_checksums) {
                    info.checksum = read_rom_checksum(full_path);
                    info.has_checksum = true;
                    info.md5 = calculate_md5(full_path);
                }

                files.push_back(info);
            }
        }
    }
    closedir(dir);
}

// Public: Scan directory for files with given extensions
std::vector<FileInfo> scan_directory(const std::string& directory,
                                     const std::vector<std::string>& extensions,
                                     bool read_checksums, bool recursive) {
    std::vector<FileInfo> files;

    if (recursive) {
        scan_directory_recursive(directory, "", extensions, read_checksums, files);
    } else {
        DIR* dir = opendir(directory.c_str());
        if (!dir) return files;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;

            std::string name = entry->d_name;
            if (has_extension(name, extensions)) {
                FileInfo info;
                info.name = name;
                info.size = 0;
                info.checksum = 0;
                info.has_checksum = false;
                info.md5 = "";

                std::string full_path = directory + "/" + name;
                struct stat st;
                if (stat(full_path.c_str(), &st) == 0) {
                    info.size = st.st_size;
                }

                if (read_checksums) {
                    info.checksum = read_rom_checksum(full_path);
                    info.has_checksum = true;
                    info.md5 = calculate_md5(full_path);
                }

                files.push_back(info);
            }
        }
        closedir(dir);
    }

    std::sort(files.begin(), files.end(), [](const FileInfo& a, const FileInfo& b) {
        return a.name < b.name;
    });
    return files;
}

// Public: Scan storage directories and build JSON inventory
std::string get_storage_json(const std::string& roms_path, const std::string& images_path) {
    auto roms = scan_directory(roms_path, {".rom"}, true, true);
    auto disks = scan_directory(images_path, {".img", ".dsk", ".hfv", ".toast"});
    auto cdroms = scan_directory(images_path, {".iso"});

    // Note: Client handles deduplication of known ROMs by MD5
    std::ostringstream json;
    json << "{\n";
    json << "  \"romsPath\": \"" << json_escape(roms_path) << "\",\n";
    json << "  \"imagesPath\": \"" << json_escape(images_path) << "\",\n";
    json << "  \"roms\": [";
    for (size_t i = 0; i < roms.size(); i++) {
        if (i > 0) json << ", ";
        json << "{\"name\": \"" << json_escape(roms[i].name) << "\", \"size\": " << roms[i].size;
        char checksum_hex[16];
        snprintf(checksum_hex, sizeof(checksum_hex), "%08x", roms[i].checksum);
        json << ", \"checksum\": \"" << checksum_hex << "\"";
        json << ", \"md5\": \"" << roms[i].md5 << "\"}";
    }
    json << "],\n";
    json << "  \"disks\": [";
    for (size_t i = 0; i < disks.size(); i++) {
        if (i > 0) json << ", ";
        json << "{\"name\": \"" << json_escape(disks[i].name) << "\", \"size\": " << disks[i].size << "}";
    }
    json << "],\n";
    json << "  \"cdroms\": [";
    for (size_t i = 0; i < cdroms.size(); i++) {
        if (i > 0) json << ", ";
        json << "{\"name\": \"" << json_escape(cdroms[i].name) << "\", \"size\": " << cdroms[i].size << "}";
    }
    json << "]\n";
    json << "}";

    return json.str();
}

// Helper: Escape string for JSON (basic implementation)
std::string json_escape(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c; break;
        }
    }
    return result;
}

} // namespace storage
