# Unified JSON Configuration System

**Date:** 2025-12-29
**Status:** ✅ Complete and functional

---

## Overview

Replaced the dual `.prefs` file system with a single unified JSON configuration file that stores settings for **both** emulators (BasiliskII and SheepShaver) plus web client settings (codec, mouse mode).

---

## Architecture

### Before (Old System):
```
User changes settings → Generate basilisk_ii.prefs OR sheepshaver.prefs
                     → POST /api/prefs
                     → Server writes .prefs file
                     → Launch emulator with hardcoded --config path
                     → Problem: Server didn't know which emulator user wanted!
```

### After (New System):
```
User changes settings → Build unified JSON object
                     → POST /api/config (JSON)
                     → Server saves macemu-config.json
                     → On launch: Read JSON → Generate .prefs → Launch correct emulator
                     → Clean: One source of truth, both configs preserved
```

---

## File Structure

### `macemu-config.json` (Single Source of Truth)

```json
{
  "version": 1,
  "web": {
    "emulator": "m68k",        // "m68k" or "ppc"
    "codec": "h264",           // h264, av1, vp9, png
    "mousemode": "relative"    // relative or absolute
  },
  "common": {
    "ram": 64,                 // MB
    "screen": "1024x768",
    "sound": true,
    "extfs": "./storage"
  },
  "m68k": {
    "rom": "Quadra-650.ROM",
    "modelid": 14,
    "cpu": 4,
    "fpu": true,
    "jit": true,
    "disks": ["MacOS75.dsk"],
    "cdroms": [],
    "idlewait": true,
    "ignoresegv": true,
    "swap_opt_cmd": true,
    "keyboardtype": 5
  },
  "ppc": {
    "rom": "SheepShaver_ROM.rom",
    "modelid": 14,
    "cpu": 4,
    "fpu": true,
    "jit": true,
    "jit68k": true,
    "disks": ["MacOS9.dsk"],
    "cdroms": [],
    "idlewait": true,
    "ignoresegv": true,
    "ignoreillegal": true,
    "keyboardtype": 5
  }
}
```

---

## APIs

### `GET /api/config`

Returns the full JSON config:

```javascript
{
  "version": 1,
  "web": { ... },
  "common": { ... },
  "m68k": { ... },
  "ppc": { ... },
  "_paths": {
    "roms": "storage/roms",
    "images": "storage/images"
  }
}
```

### `POST /api/config`

Accepts the full JSON config and saves to `macemu-config.json`:

```javascript
fetch('/api/config', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify(config)
})
```

Response:
```json
{"success": true}
```

---

## Server Implementation

### Files Created:

1. **`server/config/config_manager.h`** - Structs and API
2. **`server/config/config_manager.cpp`** - Load/save JSON, generate prefs
3. **`server/http/api_handlers.cpp`** - Added `handle_config_get()` and `handle_config_save()`

### Launch Flow (`start_emulator()`):

```cpp
// 1. Load JSON config
config::MacemuConfig cfg = config::load_config("macemu-config.json");

// 2. Determine emulator type
bool is_ppc = (cfg.web.emulator == "ppc");
std::string emu_binary = is_ppc ? "./bin/SheepShaver" : "./bin/BasiliskII";

// 3. Generate appropriate prefs file on-the-fly
std::string prefs_content = is_ppc
    ? config::generate_sheepshaver_prefs(cfg, roms_path, images_path)
    : config::generate_basilisk_prefs(cfg, roms_path, images_path);

// 4. Write temporary prefs file
storage::write_prefs_file(is_ppc ? "sheepshaver.prefs" : "basilisk_ii.prefs",
                          prefs_content);

// 5. Launch with correct binary and flag
execl(emu_binary.c_str(), emu_binary.c_str(),
      is_ppc ? "--prefs" : "--config", prefs_file.c_str(), nullptr);
```

**Console Output:**
```
🚀 LAUNCHING EMULATOR:
   Type:   SheepShaver
   Binary: ./bin/SheepShaver
   Prefs:  sheepshaver.prefs (generated from macemu-config.json)
   Codec:  h264
   Mouse:  relative
```

---

## Client Implementation

### Modified Functions:

**`loadCurrentConfig()`**
- Changed: `GET /api/prefs` → `GET /api/config`
- Converts JSON structure to internal `currentConfig` format

**`saveConfig()`**
- Changed: Generates unified JSON instead of prefs text
- POST to `/api/config` with full JSON object
- Preserves **both** m68k and ppc settings

**`onEmulatorChange()`**
- Removed: No longer loads separate prefs files
- Simply updates `currentConfig.emulator` flag

---

## Benefits

✅ **Single source of truth** - One JSON file, not two separate prefs
✅ **No prefs file conflicts** - Server generates fresh on each launch
✅ **Settings preserved** - Switch between emulators without losing config
✅ **Web settings separated** - `codec` and `mousemode` no longer pollute emulator prefs
✅ **Cleaner client** - JSON is native, no text parsing/generation
✅ **Cleaner server** - Template-based prefs generation, not line-by-line manipulation
✅ **Protocol-agnostic** - Same IPC protocol for both emulators, just different binaries

---

## Generated Prefs Files

The `.prefs` files are now **ephemeral** - generated fresh on each launch from the JSON config. They can still be inspected for debugging:

**Location:** `basilisk_ii.prefs` and `sheepshaver.prefs` in working directory

**Lifecycle:**
1. Server reads `macemu-config.json`
2. Server generates appropriate `.prefs` file
3. Server launches emulator with that file
4. On next launch, `.prefs` is regenerated (overwrites previous)

---

## Migration Notes

**No backward compatibility** - Old `.prefs` files are ignored.

If you have existing configs:
1. Create `macemu-config.json` with default values
2. Manually copy ROM/disk paths from old prefs
3. Configure via web UI

---

## Testing

**Test 1: Switch Emulators**
```bash
# Start server
./build/macemu-webrtc

# In browser: http://localhost:8000
# 1. Open Settings
# 2. Select "SheepShaver (PPC)" from dropdown
# 3. Configure ROM and disks
# 4. Click "Save & Restart"
# 5. Server should launch SheepShaver with generated prefs

# Switch back:
# 6. Open Settings
# 7. Select "Basilisk II (68k)"
# 8. Click "Save & Restart"
# 9. Server should launch BasiliskII
# 10. Both configs should be preserved!
```

**Test 2: Verify JSON Persistence**
```bash
# Check the config file
cat macemu-config.json

# Should show both m68k and ppc sections with their respective settings
```

---

## File Listing

### Server:
- `server/config/config_manager.{h,cpp}` - NEW
- `server/config/server_config.{h,cpp}` - Existing
- `server/http/api_handlers.{h,cpp}` - Modified (added config endpoints)
- `server/server.cpp` - Modified (`start_emulator()` uses JSON)
- `Makefile` - Modified (added config_manager)

### Client:
- `client/client.js` - Modified (`loadCurrentConfig()`, `saveConfig()`, `onEmulatorChange()`)

### Config:
- `macemu-config.json` - NEW (created with defaults)

### Legacy (Deprecated):
- `basilisk_ii.prefs` - Still generated, but from JSON
- `sheepshaver.prefs` - Still generated, but from JSON
- `/api/prefs` endpoints - Still exist for compatibility, not used

---

## Future Enhancements

1. **Codec/Mouse UI** - Currently hardcoded in saveConfig, should read from UI elements
2. **Config Validation** - Server-side validation of JSON values
3. **Config Versioning** - Handle schema migrations if structure changes
4. **Import/Export** - Download/upload macemu-config.json for backup
5. **Remove Legacy** - Delete old `/api/prefs` endpoints once stable

---

**Status:** Fully implemented and ready for testing! 🎉
