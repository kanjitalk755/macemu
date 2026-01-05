/*
 *  json_config.h - JSON configuration file handling
 *
 *  macemu-next (C) 2026
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef JSON_CONFIG_H
#define JSON_CONFIG_H

#include <string>

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  Find configuration file in standard locations
 *  Priority: CLI override > ~/.config/macemu-next/config.json > ./macemu-next.json
 *
 *  Returns path to config file, or NULL if not found
 *  Caller must free() the returned string
 */
char* FindConfigFile(const char *cli_override);

/*
 *  Load configuration from JSON file
 *  Populates the prefs system via PrefsAddXXX() functions
 *
 *  Returns true on success, false on error
 */
bool LoadConfigJSON(const char *path);

/*
 *  Save configuration to JSON file
 *  Writes current prefs to JSON format
 *
 *  If path is NULL, saves to user config location (~/.config/macemu-next/config.json)
 *  Returns true on success, false on error
 */
bool SaveConfigJSON(const char *path);

/*
 *  Get XDG config directory
 *  Returns ~/.config or $XDG_CONFIG_HOME
 */
std::string GetXDGConfigDir();

/*
 *  Get user config file path
 *  Returns ~/.config/macemu-next/config.json
 */
std::string GetUserConfigPath();

#ifdef __cplusplus
}
#endif

#endif // JSON_CONFIG_H
