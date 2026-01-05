# Code Reorganization Plan v2

**Date**: January 5, 2026
**Goal**: Clean up webrtc and drivers folder structure
**Changes from v1**: Single null_drivers.cpp, merge webrtc_server into main.cpp

---

## Problems Identified

### 1. WebRTC Folder Too Generic
**Current**: `src/webrtc/` contains HTTP server, UI utilities, encoders, thread launchers
**Issue**: "webrtc" is too broad - mixes web server concerns with encoding

### 2. Drivers Scattered
**Current**: Video/audio encoders in `src/webrtc/encoders/`
**Issue**: Encoders are drivers, should live with video/audio drivers

### 3. Dummy Drivers Mess
**Current**: Both `drivers/dummy/` (5 files) AND individual `*_null.cpp` files
**Issue**: Duplication and confusion - what's the difference between dummy and null?

---

## Proposed Structure

```
src/
├── main.cpp                      # UPDATE: Merge webrtc_server thread launching here
│   # Already contains (lines 453-490):
│   #   - std::thread video_encoder_thread(webrtc::video_encoder_main, ...)
│   #   - std::thread audio_encoder_thread(webrtc::audio_encoder_main, ...)
│   #   - std::thread webrtc_server_thread(webrtc::webrtc_server_main, ...)
│   #
│   # Will add globals from webrtc/globals.cpp:
│   #   - std::atomic<bool> g_running(true);
│   #   - std::atomic<bool> g_request_keyframe(false);
│   #
│   # webrtc_server_thread will launch HTTP server when implemented
│
├── webserver/                    # NEW: Web interface (was webrtc/http + storage + utils)
│   ├── http_server.cpp/h        # MOVE FROM: webrtc/http/http_server.*
│   ├── api_handlers.cpp/h       # MOVE FROM: webrtc/http/api_handlers.*
│   ├── static_files.cpp/h       # MOVE FROM: webrtc/http/static_files.*
│   ├── file_scanner.cpp/h       # MOVE FROM: webrtc/storage/file_scanner.*
│   ├── keyboard_map.cpp/h       # MOVE FROM: webrtc/utils/keyboard_map.*
│   ├── client/                  # TO BE ADDED: Browser UI (HTML/JS/CSS)
│   │   ├── index.html          # From web-streaming/client/
│   │   ├── app.js
│   │   └── style.css
│   └── meson.build             # NEW
│
├── drivers/
│   ├── video/
│   │   ├── video_driver.h       # Video driver interface
│   │   ├── video_null.cpp       # KEEP: Null implementation (no display)
│   │   ├── video_output.cpp/h   # MOVE FROM: platform/video_output.*
│   │   ├── encoders/            # NEW subdirectory
│   │   │   ├── codec.h          # MOVE FROM: webrtc/encoders/codec.h
│   │   │   ├── h264_encoder.*   # MOVE FROM: webrtc/encoders/
│   │   │   ├── vp9_encoder.*    # MOVE FROM: webrtc/encoders/
│   │   │   ├── webp_encoder.*   # MOVE FROM: webrtc/encoders/
│   │   │   ├── png_encoder.*    # MOVE FROM: webrtc/encoders/
│   │   │   ├── fpng.*           # MOVE FROM: webrtc/encoders/ (115KB file!)
│   │   │   └── meson.build      # NEW
│   │   ├── video_encoder_thread.* # MOVE FROM: webrtc/
│   │   └── meson.build          # UPDATE
│   │
│   ├── audio/
│   │   ├── audio_driver.h       # Audio driver interface
│   │   ├── audio_null.cpp       # KEEP: Null implementation (no sound)
│   │   ├── audio_output.cpp/h   # MOVE FROM: platform/audio_output.*
│   │   ├── encoders/            # NEW subdirectory
│   │   │   ├── opus_encoder.*   # MOVE FROM: webrtc/encoders/
│   │   │   ├── audio_config.h   # MOVE FROM: webrtc/encoders/
│   │   │   └── meson.build      # NEW
│   │   ├── audio_encoder_thread.* # MOVE FROM: webrtc/
│   │   └── meson.build          # UPDATE
│   │
│   ├── disk/
│   │   ├── disk_null.cpp        # KEEP: Null implementation
│   │   ├── disk_adapter.cpp     # MOVE FROM: adapter/disk_adapter.cpp
│   │   └── meson.build          # UPDATE
│   │
│   ├── scsi/
│   │   ├── scsi_null.cpp        # KEEP: Null implementation
│   │   ├── scsi_adapter.cpp     # MOVE FROM: adapter/scsi_adapter.cpp
│   │   └── meson.build          # UPDATE
│   │
│   ├── serial/
│   │   ├── serial_null.cpp      # KEEP: Null implementation
│   │   ├── serial_adapter.cpp   # MOVE FROM: adapter/serial_adapter.cpp
│   │   └── meson.build          # UPDATE
│   │
│   ├── ether/
│   │   ├── ether_null.cpp       # KEEP: Null implementation
│   │   ├── ether_adapter.cpp    # MOVE FROM: adapter/ether_adapter.cpp
│   │   └── meson.build          # UPDATE
│   │
│   ├── platform/
│   │   ├── platform_null.cpp    # KEEP: Host platform stubs
│   │   ├── timer_interrupt.*    # MOVE FROM: ../platform/timer_interrupt.*
│   │   └── meson.build          # UPDATE
│   │
│   ├── null_drivers.cpp         # NEW: Single file with all misc stubs
│   ├── null_drivers.h           # NEW: Header for null_drivers.cpp
│   │   # Consolidate 5 files from dummy/ into 1 file:
│   │   #   dummy/clip_dummy.cpp         (2 functions)
│   │   #   dummy/prefs_dummy.cpp        (10 functions)
│   │   #   dummy/prefs_editor_dummy.cpp (1 function)
│   │   #   dummy/user_strings_dummy.cpp (1 function)
│   │   #   dummy/xpram_dummy.cpp        (2 functions)
│   │   # Total: ~150 lines → single organized file
│   │
│   └── meson.build              # UPDATE: New structure, remove dummy/ and adapter/
│
├── platform/                     # Core platform API (no implementations)
│   └── platform.h               # Platform interface definition only
│   # (video_output.* and audio_output.* moved to drivers/)
│   # (timer_interrupt.* moved to drivers/platform/)
│
└── DELETE these directories:
    webrtc/                       # DELETE entire folder
    drivers/adapter/              # DELETE (files moved to respective drivers/)
    drivers/dummy/                # DELETE (consolidated into null_drivers.cpp)
```

---

## Rationale

### 1. `src/webserver/` - Clear Purpose
**Contains**: HTTP server, REST API, static files, browser UI
**Purpose**: Web interface to the emulator
**Benefit**: Clear separation - "this is the web UI"

### 2. Encoders Live With Drivers
**Video encoders** → `drivers/video/encoders/` (H.264, VP9, WebP, PNG)
**Audio encoder** → `drivers/audio/encoders/` (Opus)

**Benefit**:
- Encoders ARE drivers (they drive encoding hardware/libraries)
- Co-located with video_output/audio_output they consume
- Driver hierarchy reflects functionality

### 3. Single `null_drivers.cpp` File
**Problem**: Confusion between `dummy/` and `*_null.cpp`
- Main drivers (video, audio, disk, scsi, serial, ether) have `*_null.cpp` - these are real null implementations
- `dummy/` has misc stubs (clip, prefs, xpram, user_strings) - these are just placeholder functions

**Solution**: Consolidate 5 dummy files → 1 `null_drivers.cpp`
- **Keep** `*_null.cpp` for real drivers (may have real implementations later)
- **Consolidate** misc stubs into single organized file
- **Delete** `drivers/dummy/` directory

**Code reduction**: 5 files (~150 lines + overhead) → 1 file (~150 lines)

### 4. Merge `webrtc_server` Into main.cpp
**Current**: `webrtc/webrtc_server.cpp` is a 55-line stub that just sleeps
**Issue**: It's literally just `while (g_running) { sleep(100ms); }`
**Solution**: Delete the file, merge thread launching logic into main.cpp

**main.cpp already does this** (lines 453-490):
```cpp
#if defined(ENABLE_WEBRTC)
    std::thread video_encoder_thread(webrtc::video_encoder_main, ...);
    std::thread audio_encoder_thread(webrtc::audio_encoder_main, ...);
    std::thread webrtc_server_thread(webrtc::webrtc_server_main, ...);
#endif
```

**After reorganization**, main.cpp will just add HTTP server thread:
```cpp
#if defined(ENABLE_WEBRTC)
    std::thread video_encoder_thread(video::encoder_main, ...);
    std::thread audio_encoder_thread(audio::encoder_main, ...);
    std::thread http_server_thread(webserver::http_server_main, ...);
#endif
```

### 5. Move Adapters Into Driver Directories
**Before**: `drivers/adapter/scsi_adapter.cpp`
**After**: `drivers/scsi/scsi_adapter.cpp`
**Benefit**: Co-located with the driver they adapt

### 6. Move Platform Implementations to Drivers
**Before**: `platform/video_output.cpp`, `platform/audio_output.cpp`, `platform/timer_interrupt.cpp`
**After**:
- `drivers/video/video_output.cpp`
- `drivers/audio/audio_output.cpp`
- `drivers/platform/timer_interrupt.cpp`

**Benefit**: Implementations live with drivers, `platform/` only has interface definitions

---

## Migration Steps

### Step 1: Create New Directories
```bash
cd src
mkdir -p webserver
mkdir -p drivers/video/encoders
mkdir -p drivers/audio/encoders
```

### Step 2: Move Web Server Components
```bash
# HTTP server
mv webrtc/http/http_server.* webserver/
mv webrtc/http/api_handlers.* webserver/
mv webrtc/http/static_files.* webserver/

# Storage utilities
mv webrtc/storage/file_scanner.* webserver/

# Input utilities
mv webrtc/utils/keyboard_map.* webserver/

# Note: client/ (browser UI) will be added later from web-streaming/client/
```

### Step 3: Move Video Components
```bash
# Move encoders
mv webrtc/encoders/codec.h drivers/video/encoders/
mv webrtc/encoders/h264_encoder.* drivers/video/encoders/
mv webrtc/encoders/vp9_encoder.* drivers/video/encoders/
mv webrtc/encoders/webp_encoder.* drivers/video/encoders/
mv webrtc/encoders/png_encoder.* drivers/video/encoders/
mv webrtc/encoders/fpng.* drivers/video/encoders/

# Move encoder thread
mv webrtc/video_encoder_thread.* drivers/video/

# Move output API
mv platform/video_output.* drivers/video/
```

### Step 4: Move Audio Components
```bash
# Move encoder
mv webrtc/encoders/opus_encoder.* drivers/audio/encoders/
mv webrtc/encoders/audio_config.h drivers/audio/encoders/

# Move encoder thread
mv webrtc/audio_encoder_thread.* drivers/audio/

# Move output API
mv platform/audio_output.* drivers/audio/
```

### Step 5: Move Adapters to Driver Directories
```bash
mv drivers/adapter/disk_adapter.cpp drivers/disk/
mv drivers/adapter/scsi_adapter.cpp drivers/scsi/
mv drivers/adapter/serial_adapter.cpp drivers/serial/
mv drivers/adapter/ether_adapter.cpp drivers/ether/
mv drivers/adapter/video_adapter.cpp drivers/video/
mv drivers/adapter/audio_adapter.cpp drivers/audio/
```

### Step 6: Move Platform Timer
```bash
mv platform/timer_interrupt.* drivers/platform/
```

### Step 7: Create `null_drivers.cpp`
```bash
# Manually create null_drivers.cpp with contents from:
#   - drivers/dummy/clip_dummy.cpp
#   - drivers/dummy/prefs_dummy.cpp
#   - drivers/dummy/prefs_editor_dummy.cpp
#   - drivers/dummy/user_strings_dummy.cpp
#   - drivers/dummy/xpram_dummy.cpp

# Single organized file with all stubs
```

### Step 8: Update main.cpp
```bash
# Remove webrtc_server_thread (or update to http_server_thread)
# Add globals from webrtc/globals.cpp:
#   std::atomic<bool> g_running(true);
#   std::atomic<bool> g_request_keyframe(false);
```

### Step 9: Delete Old Directories
```bash
rm -rf webrtc/
rm -rf drivers/adapter/
rm -rf drivers/dummy/
```

### Step 10: Create meson.build Files
```bash
# Create new meson.build files:
#   - webserver/meson.build
#   - drivers/video/encoders/meson.build
#   - drivers/audio/encoders/meson.build
#
# Update existing:
#   - drivers/video/meson.build
#   - drivers/audio/meson.build
#   - drivers/*/meson.build (for moved adapters)
#   - drivers/meson.build (top-level)
#   - meson.build (root)
```

### Step 11: Update Include Paths
Fix #include statements in all affected files:

**Before**:
```cpp
#include "webrtc/encoders/h264_encoder.h"
#include "webrtc/http/http_server.h"
#include "platform/video_output.h"
#include "platform/audio_output.h"
```

**After**:
```cpp
#include "drivers/video/encoders/h264_encoder.h"
#include "webserver/http_server.h"
#include "drivers/video/video_output.h"
#include "drivers/audio/audio_output.h"
```

### Step 12: Test Build
```bash
cd macemu-next
meson setup --reconfigure --wipe builddir -Dwebrtc=true
ninja -C builddir
```

---

## File Counts

### Files Moving
| From | To | Count |
|------|----|----|
| `webrtc/http/` | `webserver/` | 6 files |
| `webrtc/storage/` | `webserver/` | 2 files |
| `webrtc/utils/` | `webserver/` | 2 files |
| `webrtc/encoders/` (video) | `drivers/video/encoders/` | 10 files |
| `webrtc/encoders/` (audio) | `drivers/audio/encoders/` | 2 files |
| `webrtc/*_thread.*` | `drivers/{video,audio}/` | 4 files |
| `platform/*_output.*` | `drivers/{video,audio}/` | 4 files |
| `platform/timer_interrupt.*` | `drivers/platform/` | 2 files |
| `drivers/adapter/` | `drivers/*/` | 6 files |
| `drivers/dummy/` | `null_drivers.cpp` | 5 files → 1 file |

### Directories Deleted
- `src/webrtc/` (everything moved)
- `src/drivers/adapter/` (merged into driver dirs)
- `src/drivers/dummy/` (consolidated into null_drivers.cpp)

### New Directories Created
- `src/webserver/` (10 files from webrtc/)
- `src/drivers/video/encoders/` (10 files)
- `src/drivers/audio/encoders/` (2 files)

---

## Benefits

1. **Clearer Organization**
   - "webserver" = web UI (clear purpose)
   - Encoders live with the drivers they belong to
   - No confusion about what goes where

2. **Better Co-location**
   - Video: encoder + output + driver = all together
   - Audio: encoder + output + driver = all together
   - Each driver directory is self-contained

3. **Less Duplication**
   - Single `null_drivers.cpp` instead of 5 dummy files
   - Adapters merged into driver directories
   - No more `dummy/` vs `*_null.cpp` confusion

4. **Easier to Find**
   - Want to change video encoding? → `drivers/video/encoders/`
   - Want to change web UI? → `webserver/`
   - Want to add a driver? → `drivers/{name}/`

5. **main.cpp Owns Threading**
   - All thread launching in one place
   - Easy to see the 4-thread architecture
   - No separate "coordinator" file

---

## Summary

**Key Changes from v1**:
1. ✅ Single `null_drivers.cpp` (not separate files)
2. ✅ Merge `webrtc_server` into `main.cpp` (it just launches threads)
3. ✅ Move `timer_interrupt.*` to `drivers/platform/`
4. ✅ Move adapters into respective driver directories

**Result**: Clean, organized, self-documenting structure!

---

**Ready for review and execution!**
