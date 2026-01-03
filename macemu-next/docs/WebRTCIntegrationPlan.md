# WebRTC Integration Plan - Complete Implementation Strategy

**Date**: January 3, 2026
**Status**: Planning Complete - Ready for Implementation
**Goal**: Unified macemu-next with in-process WebRTC streaming

---

## Executive Summary

**Vision**: Single-process emulator with integrated WebRTC streaming for browser-based remote access.

**Current State**:
- ✅ macemu-next: Modern Unicorn-based M68K emulator
- ✅ web-streaming: WebRTC server with IPC-based architecture
- ✅ Branches merged: `phoenix-mac-planning` contains both codebases

**Target State**:
- Single binary (`macemu-next`) with 7 threads
- In-process video/audio buffers (no IPC layer)
- JSON configuration system
- Browser client for remote display
- Clean, modern architecture suitable for Raspberry Pi deployment

---

## Architecture Overview

### Threading Model (4 Threads - Optimized for Multi-Core)

```
┌─────────────────────────────────────────────────────────────┐
│                    macemu-next Process                       │
├─────────────────────────────────────────────────────────────┤
│  Thread 1: CPU/MAIN ⚡      - Unicorn M68K JIT + timer      │
│                               (runs on main thread, 100%)    │
│  Thread 2: VIDEO ENCODER     - H.264/VP9/WebP encoding       │
│                               (60-90% core utilization)      │
│  Thread 3: AUDIO ENCODER     - Opus encoding                 │
│                               (10-20% core utilization)      │
│  Thread 4: WEB SERVER        - HTTP + WebRTC I/O             │
│                               (event-driven, bursty 0-30%)   │
└─────────────────────────────────────────────────────────────┘
```

**Key Design Principles**:
- **Minimal threads**: 4 threads = 4 cores = actual parallelism
- **Timer on main thread**: SIGALRM signal (no thread overhead)
- Lock-free where possible (atomic operations, triple buffers)
- Clear ownership boundaries (no shared mutable state)
- Producer-consumer patterns with ring buffers
- Clean thread lifecycle (join on shutdown)

### Data Flow

```
CPU Thread                    Encoder Threads              WebRTC Thread
    │                              │                            │
    ├─→ VideoOutput ──────────────→ H.264/VP9 ──────────────→ DataChannel
    │   (triple buffer)             Encoder                    (to browser)
    │                              │                            │
    ├─→ AudioOutput ──────────────→ Opus ────────────────────→ DataChannel
    │   (ring buffer)               Encoder                    (to browser)
    │                              │                            │
```

---

## What Gets Removed

### IPC Layer (Entire Subsystem)
- ❌ Unix domain sockets (`/tmp/macemu-{PID}.sock`)
- ❌ Shared memory (SHM) objects (`/macemu-video-{PID}`)
- ❌ IPC protocol v4 overhead (marshaling/unmarshaling)
- ❌ Separate process launches
- ❌ Process discovery by PID
- **Impact**: ~3,000 lines of code deleted

### Legacy Drivers (OS-Specific)
- ❌ X11 video drivers (obsolete)
- ❌ SDL1 drivers (replaced with WebRTC)
- ❌ Platform-specific audio (replaced with WebRTC)
- ❌ Unsupported OS code (BeOS, AmigaOS, etc.)
- **Impact**: ~10,000 lines of code deleted

**Total Deletion**: ~65 files, ~13,000 lines

---

## What Gets Kept

### From macemu-next (All Retained)
- ✅ All Unicorn backend code (~8,000 lines)
- ✅ All Platform API (~2,000 lines)
- ✅ All dual-CPU validation (~3,000 lines)
- ✅ All documentation
- ✅ All test infrastructure

### From web-streaming (Selectively Migrated)
- ✅ Video encoders (H.264, VP9, WebP, PNG) - ~8,000 lines
- ✅ Audio encoder (Opus) - ~500 lines
- ✅ JSON config system - ~500 lines
- ✅ HTTP server - ~1,200 lines (adapted)
- ✅ WebRTC peer connection logic - ~850 lines (rewritten)
- ✅ Client-side code (HTML/JS/CSS) - ~2,500 lines

**Total Kept/Migrated**: ~180 files, ~34,000 lines

---

## Implementation Phases

### Phase 1: Merge Foundations ✅ COMPLETE

**Status**: Done (commit 309d4fab)

**Outcome**:
- ✅ Merged master into phoenix-mac-planning
- ✅ All macemu-next work preserved
- ✅ WebRTC code available in tree
- ✅ Planning documents created

---

### Phase 2: Platform API - In-Process Buffers

**Goal**: Replace IPC with direct memory access

**New Files to Create**:
```
src/platform/
├── video_output.h          # Video output API (~100 lines)
├── video_output.cpp        # Triple buffer implementation (~300 lines)
├── audio_output.h          # Audio output API (~80 lines)
├── audio_output.cpp        # Ring buffer implementation (~250 lines)
└── thread_utils.h          # Thread helpers (~50 lines)
```

**Video Output API**:
```cpp
class VideoOutput {
public:
    // Called by emulator when frame is ready
    void submit_frame(uint32_t* pixels, int width, int height, PixelFormat fmt);

    // Called by encoder thread to get next frame
    const FrameBuffer* wait_for_frame();
    void release_frame();

private:
    FrameBuffer buffers[3];              // Triple buffer
    std::atomic<int> write_index;        // CPU writes here
    std::atomic<int> read_index;         // Encoder reads here
    uint64_t sequence;                   // Frame counter
};
```

**Audio Output API**:
```cpp
class AudioOutput {
public:
    // Called by emulator with audio samples
    void submit_samples(int16_t* samples, int count, int channels, int rate);

    // Called by encoder thread to get audio data
    int read_samples(int16_t* out, int max_samples);

private:
    int16_t* ring_buffer;                // Circular buffer
    int capacity;                        // Total samples
    int write_pos, read_pos;
    std::mutex mutex;
    std::condition_variable cv;
};
```

**Design Decisions**:
- **Video**: Lock-free triple buffer (atomic operations)
  - Adapted from `BasiliskII/src/IPC/ipc_protocol.h` triple buffer design
  - Remove: SHM, Unix sockets, PID-based naming
  - Keep: Atomic write-release/read-acquire pattern

- **Audio**: Mutex-protected ring buffer
  - Simpler than lock-free (audio not hot path)
  - Condition variable for efficient blocking

**Testing**:
```cpp
// Unit test for triple buffer
VideoOutput video;
uint32_t test_frame[1024*768];
// Fill with test pattern...
video.submit_frame(test_frame, 1024, 768, PIXFMT_BGRA);

const FrameBuffer* frame;
if ((frame = video.wait_for_frame())) {
    // Verify frame data matches
    assert(frame->width == 1024);
    assert(frame->height == 768);
}
```

**Estimated Effort**: 2-3 days

---

### Phase 3: WebRTC Integration

**Goal**: Copy WebRTC server code, wire to in-process buffers

**Directory Structure**:
```
src/webrtc/
├── webrtc_server.cpp               # Main WebRTC coordinator (~500 lines)
├── webrtc_server.h
├── video_encoder_thread.cpp        # Video encoding thread (~200 lines)
├── audio_encoder_thread.cpp        # Audio encoding thread (~150 lines)
├── encoders/
│   ├── h264_encoder.{cpp,h}        # Copy from web-streaming
│   ├── vp9_encoder.{cpp,h}
│   ├── webp_encoder.{cpp,h}
│   ├── png_encoder.{cpp,h}
│   ├── opus_encoder.{cpp,h}
│   ├── fpng.{cpp,h}                # Fast PNG library
│   └── codec.h                     # Base encoder class
├── http/
│   ├── http_server.{cpp,h}         # HTTP server (adapt from web-streaming)
│   ├── api_handlers.{cpp,h}        # API endpoints (rewrite for in-process)
│   └── static_files.{cpp,h}        # Serve client files
├── storage/
│   └── file_scanner.{cpp,h}        # ROM/disk file scanning
└── utils/
    ├── keyboard_map.{cpp,h}        # Browser keyboard → Mac keycodes
    └── tone_generator.h            # Audio test tone
```

**Key Code Changes**:

**OLD** (web-streaming with IPC):
```cpp
// api_handlers.cpp - OLD
void handle_reset(Request& req, Response& res) {
    send_control_message_to_emulator(emulator_pid, MSG_RESET);
    res.json({{"status", "ok"}});
}
```

**NEW** (macemu-next in-process):
```cpp
// api_handlers.cpp - NEW
void handle_reset(Request& req, Response& res) {
    g_platform.cpu_reset();  // Direct function call!
    res.json({{"status", "ok"}});
}
```

**File Migration**:
- **Copy verbatim** (~13 files, ~8,000 lines): All encoders, fpng, codec.h
- **Adapt** (~6 files, ~1,200 lines): HTTP server, API handlers (remove IPC)
- **Rewrite** (3 new files, ~850 lines): webrtc_server, encoder threads
- **Delete** (~15 files, ~3,000 lines): IPC layer, process manager

**Estimated Effort**: 3-4 days

---

### Phase 4: JSON Configuration

**Goal**: Unified JSON config system replacing old plaintext prefs

**Current Problems**:
- BasiliskII: `~/.basilisk_ii_prefs` (plaintext, custom format)
- SheepShaver: `~/.sheepshaver_prefs` (plaintext, different format)
- Inconsistent between emulators

**New Approach**:
- Single `macemu-config.json` (already exists in web-streaming)
- STL map in memory for fast lookups
- Thread-safe access (mutex-protected)
- Backward compatibility: migrate old prefs on first run

**Files to Migrate**:
```
src/config/
├── json_config.{cpp,h}         # Rename from ConfigManager
├── json_utils.{cpp,h}          # Helper functions (copy from web-streaming)
└── prefs_migrator.{cpp,h}      # Convert old prefs (copy from PrefsManager)
```

**Config Schema** (from `macemu-config.json`):
```json
{
  "common": {
    "extfs": "./storage",
    "ram": 64,
    "screen": "1024x768"
  },
  "m68k": {
    "cpu": 4,        // 68040 (enum value)
    "fpu": true,
    "jit": true,
    "rom": ""
  },
  "web": {
    "codec": "h264",
    "port": 8080
  }
}
```

**Config API**:
```cpp
class JsonConfig {
public:
    void load(const std::string& path);
    void save();

    // Thread-safe getters
    std::string get_string(const std::string& key, const std::string& default_val) const;
    int get_int(const std::string& key, int default_val) const;
    bool get_bool(const std::string& key, bool default_val) const;

    // Thread-safe setters
    void set(const std::string& key, const std::string& value);

private:
    nlohmann::json config;
    mutable std::mutex mutex;
    std::string config_path;
};
```

**Migration Strategy**:
```cpp
// On first run, check for old prefs
if (fs::exists("~/.basilisk_ii_prefs")) {
    PrefsMigrator migrator;
    migrator.migrate_from_basilisk("~/.basilisk_ii_prefs", "macemu-config.json");
    fs::rename("~/.basilisk_ii_prefs", "~/.basilisk_ii_prefs.old");
}
```

**Estimated Effort**: 2 days

---

### Phase 5: Main Entry Point

**Goal**: Create unified `main.cpp` launching all threads

**New File**: `src/main.cpp` (~200 lines)

**Structure**:
```cpp
int main(int argc, char** argv) {
    // 1. Load JSON config
    JsonConfig config("macemu-config.json");

    // 2. Initialize shared buffers
    VideoOutput video_output(1920, 1080);  // Max resolution
    AudioOutput audio_output(48000, 2);    // 48kHz stereo

    // 3. Launch CPU emulation thread
    std::thread cpu_thread(cpu_emulation_main, &config, &video_output, &audio_output);

    // 4. Launch timer interrupt thread
    std::thread timer_thread(timer_interrupt_main, &config);

    // 5. Launch video encoder thread
    std::thread video_encoder_thread(video_encoder_main, &config, &video_output);

    // 6. Launch audio encoder thread
    std::thread audio_encoder_thread(audio_encoder_main, &config, &audio_output);

    // 7. Launch WebRTC I/O thread
    std::thread webrtc_thread(webrtc_io_main, &config);

    // 8. Launch HTTP server thread
    std::thread http_thread(http_server_main, &config);

    // 9. Wait for shutdown signal (SIGINT, SIGTERM)
    wait_for_shutdown_signal();

    // 10. Clean shutdown - join all threads
    cpu_thread.join();
    timer_thread.join();
    video_encoder_thread.join();
    audio_encoder_thread.join();
    webrtc_thread.join();
    http_thread.join();

    return 0;
}
```

**Shutdown Sequence**:
```
1. Main thread catches signal (SIGINT/SIGTERM)
2. Set global shutdown_requested flag (atomic bool)
3. All threads check flag in their loops
4. Threads exit gracefully
5. Main joins all threads (blocks until complete)
6. Destructors clean up resources
7. Process exits
```

**Estimated Effort**: 1 day

---

### Phase 6: Build System Updates

**Goal**: Update Meson to build unified binary

**File**: `macemu-next/meson.build`

**New Dependencies**:
```python
# WebRTC and streaming dependencies
libdatachannel = dependency('libdatachannel')    # WebRTC
nlohmann_json = dependency('nlohmann_json')      # JSON config
libopus = dependency('opus')                     # Audio encoder
libvpx = dependency('vpx')                       # VP9 video encoder
libwebp = dependency('libwebp')                  # WebP encoder
libavcodec = dependency('libavcodec')            # H.264 encoder
libavutil = dependency('libavutil')              # FFmpeg utils
cpp_httplib = dependency('cpp-httplib')          # HTTP server
```

**New Source Files**:
```python
sources += [
    # Platform API
    'src/platform/video_output.cpp',
    'src/platform/audio_output.cpp',

    # Config system
    'src/config/json_config.cpp',
    'src/config/json_utils.cpp',
    'src/config/prefs_migrator.cpp',

    # WebRTC server
    'src/webrtc/webrtc_server.cpp',
    'src/webrtc/video_encoder_thread.cpp',
    'src/webrtc/audio_encoder_thread.cpp',

    # HTTP API
    'src/webrtc/http/http_server.cpp',
    'src/webrtc/http/api_handlers.cpp',
    'src/webrtc/http/static_files.cpp',

    # Storage
    'src/webrtc/storage/file_scanner.cpp',

    # Utils
    'src/webrtc/utils/keyboard_map.cpp',

    # Encoders
    'src/webrtc/encoders/h264_encoder.cpp',
    'src/webrtc/encoders/vp9_encoder.cpp',
    'src/webrtc/encoders/webp_encoder.cpp',
    'src/webrtc/encoders/png_encoder.cpp',
    'src/webrtc/encoders/opus_encoder.cpp',
    'src/webrtc/encoders/fpng.cpp',

    # Main entry point
    'src/main.cpp',
]
```

**Build Target**:
```python
executable('macemu-next',
    sources,
    dependencies: [
        unicorn_dep,
        libdatachannel,
        nlohmann_json,
        libopus,
        libvpx,
        libwebp,
        libavcodec,
        libavutil,
        cpp_httplib,
    ],
    install: true,
)
```

**Estimated Effort**: 1 day

---

### Phase 7: Client Migration

**Goal**: Copy browser client to macemu-next

**Source**: `web-streaming/client/`
**Destination**: `macemu-next/client/`

**Files** (copy entire directory):
```
client/
├── index.html          # Main UI (~230 lines)
├── client.js           # WebRTC client (~2,200 lines)
├── styles.css          # Styling (~100 lines)
├── Apple.svg           # Logo
├── Motorola.svg        # Logo
└── PowerPC.svg         # Logo
```

**No code changes needed** - client is IPC-agnostic (talks WebRTC only)

**HTTP Server Integration**:
```cpp
// http_server.cpp
void register_routes() {
    server.get("/", [](Request& req, Response& res) {
        res.send_file("client/index.html");
    });

    server.get("/client.js", [](Request& req, Response& res) {
        res.send_file("client/client.js");
    });

    server.get("/styles.css", [](Request& req, Response& res) {
        res.send_file("client/styles.css");
    });

    // ... static assets ...
}
```

**Estimated Effort**: 1 hour (just copy)

---

### Phase 8: Legacy Code Removal

**Goal**: Clean up obsolete code

**Directories to Delete**:
```
BasiliskII/src/IPC/           # Entire IPC layer (~3,000 lines)
BasiliskII/src/Unix/video_*   # X11/SDL1 video drivers
BasiliskII/src/Unix/audio_*   # OSS/ALSA audio drivers
BasiliskII/src/SDL/           # SDL1 backend
BasiliskII/src/BeOS/          # BeOS support
BasiliskII/src/AmigaOS/       # AmigaOS support
```

**Files to Delete from web-streaming**:
```
web-streaming/server/ipc/              # IPC connection layer
web-streaming/server/emulator/         # Process manager
web-streaming/server/config/server_config.*  # Merged into json_config
web-streaming/configure*               # Autotools (using Meson)
web-streaming/Makefile*                # Makefiles (using Meson)
```

**Total Deletion**: ~65 files, ~13,000 lines

**Estimated Effort**: 1-2 days (careful review + testing)

---

## Success Criteria

### Functional Requirements
- ✅ Single binary launches emulator + WebRTC server
- ✅ Video streaming works (no IPC overhead)
- ✅ Audio streaming works
- ✅ JSON config loads and saves correctly
- ✅ Browser client connects and displays Mac screen
- ✅ All existing macemu-next functionality preserved
- ✅ DualCPU validation still works

### Performance Requirements
- ✅ Lower latency than IPC version (no socket/SHM overhead)
- ✅ Fewer threads (no separate process = no extra threads)
- ✅ Memory efficient (no SHM duplication)
- ✅ 60 FPS video streaming at 1024x768

### Code Quality Requirements
- ✅ No legacy OS code (BeOS, AmigaOS, etc.)
- ✅ No IPC layer
- ✅ Clean separation: emulator core / WebRTC / config
- ✅ Unified build system (Meson only)
- ✅ Comprehensive documentation

---

## Timeline Estimate

| Phase | Description | Effort | Dependencies |
|-------|-------------|--------|--------------|
| 1 | Merge foundations | ✅ Done | None |
| 2 | Platform API (buffers) | 2-3 days | Phase 1 |
| 3 | WebRTC integration | 3-4 days | Phase 2 |
| 4 | JSON config | 2 days | Phase 1 |
| 5 | Main entry point | 1 day | Phases 2,3,4 |
| 6 | Build system | 1 day | Phase 5 |
| 7 | Client migration | 1 hour | Phase 6 |
| 8 | Legacy removal | 1-2 days | Phase 6 |

**Total**: ~10-14 days of focused work

**Critical Path**: Phase 2 → Phase 3 → Phase 5 → Phase 6

---

## Testing Strategy

### Phase 2 Testing (Platform API)
```bash
# Build unit tests
meson test -C build test_video_output
meson test -C build test_audio_output

# Stress test triple buffer
./build/tests/test_triple_buffer --threads=4 --frames=10000
```

### Phase 3 Testing (WebRTC)
```bash
# Build integrated binary
ninja -C build macemu-next

# Launch with test config
./build/macemu-next

# Connect browser
firefox http://localhost:8080

# Verify:
# - Video stream appears
# - Audio plays
# - Keyboard/mouse work
# - Config API responds
```

### Phase 6 Testing (Full Integration)
```bash
# Smoke test
EMULATOR_TIMEOUT=30 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom

# DualCPU validation (ensure we didn't break anything)
EMULATOR_TIMEOUT=30 CPU_BACKEND=dualcpu ./build/macemu-next ~/quadra.rom

# Performance test
EMULATOR_TIMEOUT=60 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom

# Client connection test
# (browser) http://localhost:8080
# Should see Mac booting
```

---

## Risk Mitigation

### Risk 1: Threading Bugs
**Mitigation**:
- Use ThreadSanitizer (`meson configure -Db_sanitize=thread`)
- Unit test all lock-free data structures
- Clear ownership rules (documented in code)

### Risk 2: Performance Regression
**Mitigation**:
- Benchmark before/after (IPC vs in-process)
- Profile with perf/instruments
- Keep old IPC code in git (can compare)

### Risk 3: Build Dependency Hell
**Mitigation**:
- Document all dependencies in README
- Provide Docker container for build
- Test on clean Ubuntu/Debian VM

### Risk 4: Breaking Existing Functionality
**Mitigation**:
- Keep DualCPU validation working
- Run trace comparison tests
- Incremental commits (can bisect if broken)

---

## File Organization Reference

### Final Directory Structure
```
macemu-next/
├── src/
│   ├── common/include/          # Shared headers
│   ├── cpu/                     # Unicorn, UAE, DualCPU
│   ├── core/                    # Mac emulation core
│   ├── drivers/                 # Hardware drivers
│   ├── platform/                # NEW: Video/audio buffers
│   │   ├── video_output.{h,cpp}
│   │   └── audio_output.{h,cpp}
│   ├── config/                  # NEW: JSON config
│   │   ├── json_config.{h,cpp}
│   │   ├── json_utils.{h,cpp}
│   │   └── prefs_migrator.{h,cpp}
│   ├── webrtc/                  # NEW: WebRTC streaming
│   │   ├── webrtc_server.{h,cpp}
│   │   ├── video_encoder_thread.cpp
│   │   ├── audio_encoder_thread.cpp
│   │   ├── encoders/            # H.264, VP9, WebP, Opus
│   │   ├── http/                # HTTP server, API handlers
│   │   ├── storage/             # File scanning
│   │   └── utils/               # Keyboard map, etc.
│   └── main.cpp                 # NEW: Entry point
├── client/                      # NEW: Browser UI
│   ├── index.html
│   ├── client.js
│   └── styles.css
├── external/unicorn/            # Unicorn submodule
├── docs/                        # Documentation
├── tests/                       # Unit tests
├── scripts/                     # Helper scripts
├── meson.build                  # Build system
└── macemu-config.json           # Config file
```

---

## Next Steps

### Immediate (Today)
1. ✅ Review this plan with Michael
2. ✅ Update TodoStatus.md
3. ✅ Commit planning documents

### Tomorrow (if approved)
1. Create `src/platform/` directory
2. Write `video_output.h` header (API design)
3. Implement `video_output.cpp` (triple buffer)
4. Write unit tests for triple buffer

### This Week
1. Complete Phase 2 (Platform API)
2. Begin Phase 3 (copy encoders)
3. Test in-process buffers with dummy data

---

## Open Questions

1. **Threading model**: Should main thread also run event loop, or just coordinate?
   - **Proposed**: Main just coordinates, workers do all the work

2. **Buffer ownership**: Who allocates frame buffers?
   - **Proposed**: VideoOutput owns all 3 buffers (lifetime = program lifetime)

3. **Config hot-reload**: Can we reload JSON without restart?
   - **Proposed**: Yes - HTTP API can update config, threads re-read on next access

4. **Backward compat**: Support old prefs files?
   - **Proposed**: Yes - migrate once on first run, then use JSON

5. **Build dependencies**: Which libraries required on Raspberry Pi?
   - **Answer**: All dependencies available via apt on Raspberry Pi OS

---

## References

**Related Documents**:
- [THREADING_ARCHITECTURE.md](THREADING_ARCHITECTURE.md) - Complete 7-thread design
- [FILE_MIGRATION_PLAN.md](FILE_MIGRATION_PLAN.md) - File-by-file mapping
- [TodoStatus.md](TodoStatus.md) - Current project status
- [ProjectGoals.md](ProjectGoals.md) - Long-term vision

**Key Commits**:
- `309d4fab` - Merge master (WebRTC code)
- `a3712b98` - WebRTC integration plan (initial)
- `74347217` - Latest documentation reorganization

---

**This is the complete roadmap** from current IPC-based dual architecture to unified in-process design. All planning is done - ready to implement!
