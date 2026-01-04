# File Migration Plan - Detailed Mapping

**Date**: January 3, 2026
**Goal**: Map every file from current state → final macemu-next structure

---

## Directory Structure (Target)

```
macemu-next/
├── src/
│   ├── common/
│   │   └── include/         # Shared headers
│   ├── cpu/                 # CPU backends (Unicorn, UAE, DualCPU)
│   ├── core/                # Mac emulation core
│   ├── drivers/             # Hardware drivers (null stubs)
│   ├── platform/            # NEW: Video/Audio output API
│   ├── webrtc/              # NEW: WebRTC streaming
│   ├── config/              # NEW: JSON config system
│   └── main.cpp             # NEW: Single entry point
├── client/                  # NEW: Web UI (HTML/JS/CSS)
├── external/
│   └── unicorn/             # Unicorn submodule
├── docs/                    # Documentation
├── tests/                   # Unit tests
├── meson.build              # Build system
└── macemu-config.json       # Config file
```

---

## File-by-File Migration

### Phase 1: Core Emulation (Already in macemu-next ✅)

**KEEP AS-IS** - Already clean and modern

| Current File | Final Location | Status | Notes |
|-------------|----------------|--------|-------|
| `macemu-next/src/cpu/cpu_unicorn.cpp` | `src/cpu/cpu_unicorn.cpp` | ✅ Keep | Unicorn backend |
| `macemu-next/src/cpu/cpu_uae.c` | `src/cpu/cpu_uae.c` | ✅ Keep | UAE backend |
| `macemu-next/src/cpu/cpu_dualcpu.c` | `src/cpu/cpu_dualcpu.c` | ✅ Keep | DualCPU validation |
| `macemu-next/src/cpu/unicorn_wrapper.*` | `src/cpu/unicorn_wrapper.*` | ✅ Keep | Unicorn API |
| `macemu-next/src/cpu/unicorn_validation.cpp` | `src/cpu/unicorn_validation.cpp` | ✅ Keep | Validation logic |
| `macemu-next/src/cpu/cpu_trace.c` | `src/cpu/cpu_trace.c` | ✅ Keep | Debug tracing |
| `macemu-next/src/cpu/uae_cpu/**` | `src/cpu/uae_cpu/**` | ✅ Keep | UAE CPU (all files) |
| `macemu-next/src/core/**` | `src/core/**` | ✅ Keep | All core files |
| `macemu-next/src/common/include/**` | `src/common/include/**` | ✅ Keep | All headers |
| `macemu-next/src/drivers/**` | `src/drivers/**` | ✅ Keep | Null drivers |
| `macemu-next/external/unicorn/` | `external/unicorn/` | ✅ Keep | Submodule |
| `macemu-next/docs/**` | `docs/**` | ✅ Keep | All docs |
| `macemu-next/tests/**` | `tests/**` | ✅ Keep | All tests |
| `macemu-next/meson.build` | `meson.build` | 🔧 Modify | Add WebRTC deps |
| `macemu-next/scripts/**` | `scripts/**` | ✅ Keep | Trace comparison |

---

### Phase 2: Platform API (NEW - Create from scratch)

**CREATE NEW** - In-process video/audio buffers

| New File | Purpose | Source | Lines |
|----------|---------|--------|-------|
| `src/platform/video_output.h` | Video buffer API | Design from scratch | ~100 |
| `src/platform/video_output.cpp` | Triple buffer impl | Adapt from `ipc_protocol.h` | ~300 |
| `src/platform/audio_output.h` | Audio buffer API | Design from scratch | ~80 |
| `src/platform/audio_output.cpp` | Ring buffer impl | Design from scratch | ~250 |
| `src/platform/thread_utils.h` | Thread helpers | Design from scratch | ~50 |

**Adapts concepts from**:
- `BasiliskII/src/IPC/ipc_protocol.h` - Triple buffer structure, atomics
- Remove: Unix sockets, SHM, PID-based naming

---

### Phase 3: WebRTC Server (COPY & ADAPT from web-streaming/)

#### 3a. Encoders (COPY - minimal changes)

| Source File | Destination | Changes Needed |
|------------|-------------|----------------|
| `web-streaming/server/h264_encoder.cpp` | `src/webrtc/encoders/h264_encoder.cpp` | ✅ None (pure encoder) |
| `web-streaming/server/h264_encoder.h` | `src/webrtc/encoders/h264_encoder.h` | ✅ None |
| `web-streaming/server/vp9_encoder.cpp` | `src/webrtc/encoders/vp9_encoder.cpp` | ✅ None |
| `web-streaming/server/vp9_encoder.h` | `src/webrtc/encoders/vp9_encoder.h` | ✅ None |
| `web-streaming/server/webp_encoder.cpp` | `src/webrtc/encoders/webp_encoder.cpp` | ✅ None |
| `web-streaming/server/webp_encoder.h` | `src/webrtc/encoders/webp_encoder.h` | ✅ None |
| `web-streaming/server/png_encoder.cpp` | `src/webrtc/encoders/png_encoder.cpp` | ✅ None |
| `web-streaming/server/png_encoder.h` | `src/webrtc/encoders/png_encoder.h` | ✅ None |
| `web-streaming/server/opus_encoder.cpp` | `src/webrtc/encoders/opus_encoder.cpp` | ✅ None |
| `web-streaming/server/opus_encoder.h` | `src/webrtc/encoders/opus_encoder.h` | ✅ None |
| `web-streaming/server/fpng.cpp` | `src/webrtc/encoders/fpng.cpp` | ✅ None (library) |
| `web-streaming/server/fpng.h` | `src/webrtc/encoders/fpng.h` | ✅ None |
| `web-streaming/server/codec.h` | `src/webrtc/encoders/codec.h` | ✅ None (base class) |

**Total**: 13 files, ~8,000 lines - COPY AS-IS

#### 3b. Config System (COPY & ADAPT)

| Source File | Destination | Changes Needed |
|------------|-------------|----------------|
| `web-streaming/server/config/config_manager.cpp` | `src/config/json_config.cpp` | 🔧 Rename class: `ConfigManager` → `JsonConfig` |
| `web-streaming/server/config/config_manager.h` | `src/config/json_config.h` | 🔧 Rename class |
| `web-streaming/server/config/server_config.cpp` | ❌ DELETE | Not needed (merged into json_config) |
| `web-streaming/server/config/server_config.h` | ❌ DELETE | Not needed |
| `web-streaming/server/utils/json_utils.cpp` | `src/config/json_utils.cpp` | ✅ Copy as-is |
| `web-streaming/server/utils/json_utils.h` | `src/config/json_utils.h` | ✅ Copy as-is |
| `web-streaming/macemu-config.json` | `macemu-config.json` | ✅ Copy to root |

**Changes in `json_config.cpp`**:
```cpp
// OLD (web-streaming)
ConfigManager::ConfigManager(const std::string& path) {
    config_path = path;
    load();
}

// NEW (macemu-next)
JsonConfig::JsonConfig(const std::string& path) {
    config_path = path;
    load();
}
```

**Total**: 4 files kept, ~500 lines

#### 3c. HTTP Server (COPY & ADAPT)

| Source File | Destination | Changes Needed |
|------------|-------------|----------------|
| `web-streaming/server/http/http_server.cpp` | `src/webrtc/http/http_server.cpp` | 🔧 Remove IPC calls, add in-process API |
| `web-streaming/server/http/http_server.h` | `src/webrtc/http/http_server.h` | 🔧 Update interface |
| `web-streaming/server/http/api_handlers.cpp` | `src/webrtc/http/api_handlers.cpp` | 🔧 Rewrite for in-process |
| `web-streaming/server/http/api_handlers.h` | `src/webrtc/http/api_handlers.h` | 🔧 Update interface |
| `web-streaming/server/http/static_files.cpp` | `src/webrtc/http/static_files.cpp` | ✅ Copy as-is |
| `web-streaming/server/http/static_files.h` | `src/webrtc/http/static_files.h` | ✅ Copy as-is |

**Changes in `api_handlers.cpp`**:
```cpp
// OLD (web-streaming) - IPC calls
void handle_reset(Request& req, Response& res) {
    send_control_message_to_emulator(emulator_pid, MSG_RESET);
    res.json({{"status", "ok"}});
}

// NEW (macemu-next) - Direct function call
void handle_reset(Request& req, Response& res) {
    g_platform.cpu_reset();  // Direct call!
    res.json({{"status", "ok"}});
}
```

**Total**: 6 files, ~1,200 lines (300 lines of IPC code removed)

#### 3d. Storage/File Scanning (COPY - minimal changes)

| Source File | Destination | Changes Needed |
|------------|-------------|----------------|
| `web-streaming/server/storage/file_scanner.cpp` | `src/webrtc/storage/file_scanner.cpp` | ✅ Copy as-is |
| `web-streaming/server/storage/file_scanner.h` | `src/webrtc/storage/file_scanner.h` | ✅ Copy as-is |
| `web-streaming/server/storage/prefs_manager.cpp` | `src/config/prefs_migrator.cpp` | 🔧 Rename (better name) |
| `web-streaming/server/storage/prefs_manager.h` | `src/config/prefs_migrator.h` | 🔧 Rename |

**Total**: 4 files, ~500 lines

#### 3e. Utilities (COPY - some deletions)

| Source File | Destination | Changes Needed |
|------------|-------------|----------------|
| `web-streaming/server/utils/keyboard_map.cpp` | `src/webrtc/utils/keyboard_map.cpp` | ✅ Copy as-is |
| `web-streaming/server/utils/keyboard_map.h` | `src/webrtc/utils/keyboard_map.h` | ✅ Copy as-is |
| `web-streaming/server/tone_generator.h` | `src/webrtc/utils/tone_generator.h` | ✅ Copy as-is (audio test) |

**Total**: 3 files, ~200 lines

#### 3f. Main Server (REWRITE - heavily modified)

| Source File | Destination | Changes Needed |
|------------|-------------|----------------|
| `web-streaming/server/server.cpp` | `src/webrtc/webrtc_server.cpp` | 🔧 MAJOR rewrite |
| *(none)* | `src/webrtc/webrtc_server.h` | ✨ NEW header |
| *(none)* | `src/webrtc/video_encoder_thread.cpp` | ✨ NEW (extracted from server.cpp) |
| *(none)* | `src/webrtc/audio_encoder_thread.cpp` | ✨ NEW (extracted from server.cpp) |

**OLD `server.cpp`** (2,800 lines):
- IPC connection management
- Process discovery
- SHM mapping
- Socket I/O
- Encoder threads
- WebRTC peer connections
- HTTP server
- Signaling

**NEW architecture** (split into modules):
```
webrtc_server.cpp (~500 lines):
  - WebRTC peer connection
  - Signaling
  - Coordinate threads

video_encoder_thread.cpp (~200 lines):
  - Read from VideoOutput
  - Encode frames
  - Send to WebRTC

audio_encoder_thread.cpp (~150 lines):
  - Read from AudioOutput
  - Encode audio
  - Send to WebRTC
```

**REMOVED** (~1,000 lines of IPC code):
- `ipc_connection.cpp` - Entire file deleted
- `process_manager.cpp` - Entire file deleted
- IPC socket handling
- SHM mapping
- Process spawning

**Total**: Old 2,800 lines → New 850 lines (65% reduction!)

---

### Phase 4: Client (COPY - no changes)

**COPY ENTIRE DIRECTORY** - Client code is IPC-agnostic

| Source | Destination | Changes |
|--------|-------------|---------|
| `web-streaming/client/**` | `client/**` | ✅ Copy entire directory |

**Files included**:
```
client/
├── index.html       (~230 lines)
├── client.js        (~2,200 lines)
├── styles.css       (~100 lines)
├── Apple.svg
├── Motorola.svg
└── PowerPC.svg
```

**Total**: 6 files, ~2,500 lines - NO CHANGES

---

### Phase 5: Main Entry Point (NEW - Create)

| New File | Purpose | Lines |
|----------|---------|-------|
| `src/main.cpp` | Single entry point, thread launcher | ~200 |

**Structure**:
```cpp
int main(int argc, char** argv) {
    // 1. Load config
    JsonConfig config("macemu-config.json");

    // 2. Create shared buffers
    VideoOutput video_output;
    AudioOutput audio_output;

    // 3. Launch threads
    std::thread cpu_thread(...);
    std::thread timer_thread(...);
    std::thread video_encoder_thread(...);
    std::thread audio_encoder_thread(...);
    std::thread webrtc_thread(...);
    std::thread http_thread(...);

    // 4. Wait for shutdown
    wait_for_signal();

    // 5. Join threads
    cpu_thread.join();
    // ... join all others ...

    return 0;
}
```

**Replaces**:
- Old `macemu-next/src/main.cpp` (simple test harness)
- `web-streaming/server/server.cpp` main() function

---

### Phase 6: Build System (MODIFY - Add dependencies)

| File | Changes |
|------|---------|
| `macemu-next/meson.build` | 🔧 Add WebRTC deps, new sources |

**New dependencies**:
```python
# Add to meson.build
libdatachannel = dependency('libdatachannel')
nlohmann_json = dependency('nlohmann_json')
libopus = dependency('opus')
libvpx = dependency('vpx')           # For VP9
libwebp = dependency('libwebp')
libavcodec = dependency('libavcodec') # For H.264
libavutil = dependency('libavutil')
cpp_httplib = dependency('cpp-httplib')  # For HTTP server
```

**New sources**:
```python
sources += [
    # Platform API
    'src/platform/video_output.cpp',
    'src/platform/audio_output.cpp',

    # Config
    'src/config/json_config.cpp',
    'src/config/json_utils.cpp',
    'src/config/prefs_migrator.cpp',

    # WebRTC
    'src/webrtc/webrtc_server.cpp',
    'src/webrtc/video_encoder_thread.cpp',
    'src/webrtc/audio_encoder_thread.cpp',
    'src/webrtc/http/http_server.cpp',
    'src/webrtc/http/api_handlers.cpp',
    'src/webrtc/http/static_files.cpp',
    'src/webrtc/storage/file_scanner.cpp',
    'src/webrtc/utils/keyboard_map.cpp',

    # Encoders
    'src/webrtc/encoders/h264_encoder.cpp',
    'src/webrtc/encoders/vp9_encoder.cpp',
    'src/webrtc/encoders/webp_encoder.cpp',
    'src/webrtc/encoders/png_encoder.cpp',
    'src/webrtc/encoders/opus_encoder.cpp',
    'src/webrtc/encoders/fpng.cpp',
]
```

---

## Files to DELETE (Not Migrated)

### From BasiliskII/SheepShaver

**DELETE ENTIRE IPC LAYER**:
```
BasiliskII/src/IPC/
├── audio_ipc.cpp          ❌ DELETE - Replaced by AudioOutput
├── audio_ipc.h            ❌ DELETE
├── audio_config.h         ❌ DELETE (copy concepts to AudioOutput)
├── video_ipc.cpp          ❌ DELETE - Replaced by VideoOutput
├── control_ipc.cpp        ❌ DELETE - Direct function calls now
├── control_ipc.h          ❌ DELETE
└── ipc_protocol.h         ⚠️  ADAPT to video_output.h (keep buffer structure, remove IPC)
```

**DELETE LEGACY DRIVERS**:
```
BasiliskII/src/Unix/
├── video_x.cpp            ❌ DELETE - X11 video (obsolete)
├── video_sdl.cpp          ❌ DELETE - SDL1 video (obsolete)
├── audio_oss.cpp          ❌ DELETE - OSS audio (obsolete)
├── audio_alsa.cpp         ❌ DELETE - ALSA audio (replaced by WebRTC)
└── ... (other Unix drivers)

BasiliskII/src/SDL/        ❌ DELETE ENTIRE DIRECTORY
BasiliskII/src/BeOS/       ❌ DELETE ENTIRE DIRECTORY
BasiliskII/src/AmigaOS/    ❌ DELETE ENTIRE DIRECTORY
BasiliskII/src/Windows/    ❌ DELETE (if not needed)
```

### From web-streaming/

**DELETE IPC CODE**:
```
web-streaming/server/ipc/
├── ipc_connection.cpp     ❌ DELETE - No IPC anymore
└── ipc_connection.h       ❌ DELETE

web-streaming/server/emulator/
├── process_manager.cpp    ❌ DELETE - Single process now
└── process_manager.h      ❌ DELETE

web-streaming/server/config/
└── server_config.*        ❌ DELETE - Merged into json_config
```

**DELETE BUILD ARTIFACTS**:
```
web-streaming/
├── configure              ❌ DELETE - Using Meson
├── configure.ac           ❌ DELETE
├── Makefile.in            ❌ DELETE
├── Makefile               ❌ DELETE
└── autom4te.cache/        ❌ DELETE
```

---

## Migration Summary by Numbers

### Source Files

| Category | Files | Lines | Action |
|----------|-------|-------|--------|
| **macemu-next (keep)** | ~150 | ~20,000 | ✅ Keep as-is |
| **Encoders (copy)** | 13 | ~8,000 | ✅ Copy verbatim |
| **Config (adapt)** | 4 | ~500 | 🔧 Minor changes |
| **HTTP (adapt)** | 6 | ~1,200 | 🔧 Remove IPC calls |
| **Storage (copy)** | 4 | ~500 | ✅ Mostly copy |
| **Client (copy)** | 6 | ~2,500 | ✅ Copy verbatim |
| **Platform API (new)** | 5 | ~800 | ✨ Create new |
| **Main (new)** | 1 | ~200 | ✨ Create new |
| **WebRTC server (rewrite)** | 3 | ~850 | 🔧 Heavy rewrite |
| **IPC layer (delete)** | ~15 | ~3,000 | ❌ Delete |
| **Legacy drivers (delete)** | ~50 | ~10,000 | ❌ Delete |

**Total**:
- **Keep/Copy**: ~170 files, ~33,000 lines
- **Create new**: ~10 files, ~1,000 lines
- **Delete**: ~65 files, ~13,000 lines

**Net result**: ~180 files, ~34,000 lines (cleaner, simpler!)

---

## Migration Phases (Recommended Order)

### Week 1: Foundation
1. ✅ Create `src/platform/` directory structure
2. ✅ Write `video_output.h` / `video_output.cpp`
3. ✅ Write `audio_output.h` / `audio_output.cpp`
4. ✅ Unit test buffers (triple buffer, ring buffer)

### Week 2: Config & Encoders
1. ✅ Copy config system (`src/config/`)
2. ✅ Copy all encoders (`src/webrtc/encoders/`)
3. ✅ Copy client (`client/`)
4. ✅ Test encoders with dummy data

### Week 3: Integration
1. ✅ Copy HTTP server (`src/webrtc/http/`)
2. ✅ Adapt API handlers (remove IPC)
3. ✅ Write `webrtc_server.cpp`
4. ✅ Write encoder thread functions
5. ✅ Write new `main.cpp`

### Week 4: Testing & Cleanup
1. ✅ Update `meson.build`
2. ✅ Build and test end-to-end
3. ✅ Delete IPC code
4. ✅ Delete legacy drivers
5. ✅ Documentation updates

---

## Dependency Graph

```
main.cpp
  ├─→ JsonConfig (config/)
  ├─→ VideoOutput (platform/)
  ├─→ AudioOutput (platform/)
  ├─→ cpu_emulation_main (cpu/)
  │     └─→ Platform API
  ├─→ video_encoder_thread (webrtc/)
  │     ├─→ H264Encoder (webrtc/encoders/)
  │     └─→ VideoOutput (platform/)
  ├─→ audio_encoder_thread (webrtc/)
  │     ├─→ OpusEncoder (webrtc/encoders/)
  │     └─→ AudioOutput (platform/)
  ├─→ webrtc_io_main (webrtc/)
  │     └─→ libdatachannel
  └─→ http_server_main (webrtc/http/)
        ├─→ cpp-httplib
        └─→ JsonConfig (config/)
```

**No circular dependencies** - Clean layered architecture!

---

This mapping ensures **nothing gets lost** and **everything has a place** in the new architecture.
