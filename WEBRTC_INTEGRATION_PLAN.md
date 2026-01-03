# WebRTC Integration Plan - In-Process Architecture

**Date**: January 3, 2026
**Goal**: Merge macemu-next (Unicorn backend) with WebRTC streaming (from master) into single in-process architecture

---

## Current State Analysis

### What We Have Now (Post-Merge)

**macemu-next/** - Modern Unicorn-based emulator
- ✅ Platform API abstraction
- ✅ Unicorn M68K backend (JIT-compiled)
- ✅ UAE backend (validation baseline)
- ✅ Dual-CPU validation (514k+ instructions)
- ✅ Clean Meson build system
- ✅ Comprehensive documentation

**web-streaming/** - WebRTC server (IPC-based)
- ✅ Organized structure (config/, ipc/, http/, storage/, utils/)
- ✅ JSON config system (nlohmann/json)
- ✅ Triple-buffered video (atomic operations, no locks)
- ✅ Audio streaming (Opus encoder)
- ✅ Multiple codecs (H.264, VP9, WebP, PNG)
- ✅ HTTP API + WebRTC peer connections

**BasiliskII/** + **SheepShaver/** - Legacy emulators with IPC
- IPC video drivers (`video_ipc.cpp`)
- IPC audio drivers (`audio_ipc.cpp`)
- IPC control socket (`control_ipc.cpp`)
- Shared memory (SHM) for frame buffers

---

## Target Architecture

### Single Process with Multiple Threads

```
┌─────────────────────────────────────────────────────────┐
│              macemu-next (single binary)                │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  ┌───────────────────────────────────────────┐         │
│  │       Emulator Core (main thread)         │         │
│  ├───────────────────────────────────────────┤         │
│  │  • Unicorn M68K CPU (JIT)                │         │
│  │  • Platform API                           │         │
│  │  • Memory system (RAM/ROM)                │         │
│  │  • EmulOps, traps, interrupts             │         │
│  │                                           │         │
│  │  ┌─────────────────────┐                 │         │
│  │  │ Video Output        │                 │         │
│  │  │ (framebuffer)       │───┐             │         │
│  │  └─────────────────────┘   │             │         │
│  │                             │             │         │
│  │  ┌─────────────────────┐   │             │         │
│  │  │ Audio Output        │───┤             │         │
│  │  │ (sample buffer)     │   │             │         │
│  │  └─────────────────────┘   │             │         │
│  └──────────────────────────┬──┘             │         │
│                             │                │         │
│         In-Process Buffers  │                │         │
│         (no IPC!)           │                │         │
│                             ▼                │         │
│  ┌─────────────────────────────────────┐   │         │
│  │  Shared Buffers (in-process memory) │   │         │
│  ├─────────────────────────────────────┤   │         │
│  │  • Triple video buffer (atomics)    │   │         │
│  │  • Audio ring buffer                │   │         │
│  │  • Control state (JSON config)      │   │         │
│  └──────────────────┬──────────────────┘   │         │
│                     │                      │         │
│                     ▼                      │         │
│  ┌───────────────────────────────────────┐ │         │
│  │    WebRTC Streaming (worker thread)  │ │         │
│  ├───────────────────────────────────────┤ │         │
│  │  • Encoder (H.264/VP9/WebP)          │ │         │
│  │  • Opus audio encoder                │ │         │
│  │  • WebRTC peer connection            │ │         │
│  │  • HTTP server (config API)          │ │         │
│  └───────────────────────────────────────┘ │         │
│                                           │         │
└───────────────────────────────────────────┴─────────┘
```

---

## What Gets Removed

### IPC Layer (Entire Subsystem)
- ❌ Unix domain sockets (`/tmp/macemu-{PID}.sock`)
- ❌ Shared memory (SHM) objects (`/macemu-video-{PID}`)
- ❌ IPC protocol overhead (marshaling/unmarshaling)
- ❌ Separate process launches
- ❌ Process discovery by PID

**Files to Remove**:
- `BasiliskII/src/IPC/*` (replaced with in-process API)
- `SheepShaver/src/IPC/*` (symlinks, replaced)
- `web-streaming/server/ipc/ipc_connection.cpp` (replaced)
- `web-streaming/server/emulator/process_manager.cpp` (no separate process)

### Legacy Drivers (OS-specific)
- ❌ X11 video drivers (obsolete)
- ❌ SDL1 drivers (replaced with WebRTC)
- ❌ Platform-specific audio (replaced with WebRTC)
- ❌ Unsupported OS code (BeOS, AmigaOS, etc.)

---

## What Gets Kept

### From macemu-next
- ✅ **All** Unicorn backend code
- ✅ **All** Platform API
- ✅ **All** dual-CPU validation
- ✅ **All** documentation

### From web-streaming
- ✅ Video encoders (`h264_encoder.cpp`, `vp9_encoder.cpp`, `webp_encoder.cpp`)
- ✅ Audio encoder (`opus_encoder.cpp`)
- ✅ JSON config system (`config/`, `utils/json_utils.*`)
- ✅ HTTP server (`http/`)
- ✅ WebRTC peer connection logic
- ✅ Client-side code (`client/`)

### Triple Buffer Design (Adapted)
- ✅ Keep atomic operations (no locks)
- ✅ Keep triple buffering pattern
- ❌ Remove SHM (use regular heap allocation)
- ✅ Keep `ipc_protocol.h` structure definitions (rename to `video_buffer.h`)

---

## Integration Steps

### Phase 1: Merge Foundations ✅ DONE
- ✅ Merge master into phoenix-mac-planning
- ✅ Preserve all macemu-next work
- ✅ Study WebRTC architecture

### Phase 2: Create In-Process Video/Audio API
**Goal**: Replace IPC with direct function calls

**New Files** (in `macemu-next/src/`):
```
macemu-next/src/
├── platform/
│   ├── video_output.h         # Video output API
│   ├── video_output.cpp       # Triple-buffer implementation (in-process)
│   ├── audio_output.h         # Audio output API
│   └── audio_output.cpp       # Ring buffer implementation (in-process)
```

**Video Output API**:
```cpp
class VideoOutput {
public:
    // Called by emulator when frame is ready
    void submit_frame(uint32_t* pixels, int width, int height, PixelFormat fmt);

    // Called by WebRTC thread to get next frame
    bool get_next_frame(const FrameBuffer** out_frame);

private:
    TripleBuffer<FrameBuffer> buffers;  // Atomic triple buffering
};
```

**Audio Output API**:
```cpp
class AudioOutput {
public:
    // Called by emulator with audio samples
    void submit_samples(int16_t* samples, int count, int channels, int rate);

    // Called by WebRTC thread to get audio data
    int get_samples(int16_t* out, int max_samples);

private:
    RingBuffer<int16_t> buffer;  // Lock-free ring buffer
};
```

### Phase 3: Integrate WebRTC Server Into macemu-next
**Goal**: Single binary with WebRTC built-in

**Copy WebRTC code** into macemu-next:
```
macemu-next/src/
├── webrtc/
│   ├── server.cpp            # Main WebRTC server (adapted from web-streaming)
│   ├── encoders/
│   │   ├── h264_encoder.cpp
│   │   ├── vp9_encoder.cpp
│   │   ├── webp_encoder.cpp
│   │   └── opus_encoder.cpp
│   ├── http/
│   │   ├── http_server.cpp   # Config API
│   │   └── api_handlers.cpp
│   └── peer_connection.cpp   # WebRTC connection
├── config/
│   ├── json_config.h         # JSON config manager
│   └── json_config.cpp       # Uses nlohmann/json
└── main.cpp                  # Launch both emulator + WebRTC threads
```

**main.cpp** structure:
```cpp
int main(int argc, char** argv) {
    // Load JSON config
    JsonConfig config("macemu-config.json");

    // Create shared buffers (in-process)
    VideoOutput video_output;
    AudioOutput audio_output;

    // Launch emulator thread
    std::thread emulator_thread([&]() {
        platform_init();
        g_platform.video = &video_output;
        g_platform.audio = &audio_output;
        // Run emulator...
    });

    // Launch WebRTC server thread
    std::thread webrtc_thread([&]() {
        WebRTCServer server(&video_output, &audio_output, &config);
        server.run();
    });

    emulator_thread.join();
    webrtc_thread.join();
}
```

### Phase 4: Update Meson Build
**Goal**: Build single binary with all dependencies

```python
# macemu-next/meson.build

# Dependencies
libdatachannel = dependency('libdatachannel')
nlohmann_json = dependency('nlohmann_json')
libopus = dependency('opus')
libvpx = dependency('vpx')      # For VP9
libwebp = dependency('libwebp')
libavcodec = dependency('libavcodec')  # For H.264

# Source files
sources = [
    # Existing macemu-next sources
    'src/main.cpp',
    'src/cpu/cpu_unicorn.cpp',
    # ...

    # WebRTC sources (integrated)
    'src/webrtc/server.cpp',
    'src/webrtc/encoders/h264_encoder.cpp',
    'src/webrtc/encoders/opus_encoder.cpp',
    'src/webrtc/http/http_server.cpp',
    'src/platform/video_output.cpp',
    'src/platform/audio_output.cpp',
    'src/config/json_config.cpp',
]

executable('macemu-next',
    sources,
    dependencies: [libdatachannel, nlohmann_json, libopus, ...],
)
```

### Phase 5: JSON Prefs Migration
**Goal**: Replace old plaintext prefs with unified JSON

**Current Problems**:
- BasiliskII: `~/.basilisk_ii_prefs` (plaintext, custom format)
- SheepShaver: `~/.sheepshaver_prefs` (plaintext, different format)
- Inconsistent between emulators

**New Approach**:
- Single `macemu-config.json` (already exists in web-streaming!)
- STL map in memory for fast lookups
- Backward compatibility: migrate old prefs on first run

**Config API**:
```cpp
class JsonConfig {
public:
    // Load from file
    void load(const std::string& path);

    // Get values with defaults
    std::string get_string(const std::string& key, const std::string& default_val);
    int get_int(const std::string& key, int default_val);
    bool get_bool(const std::string& key, bool default_val);

    // Set values
    void set(const std::string& key, const std::string& value);

    // Save to file
    void save();

private:
    nlohmann::json config;  // In-memory JSON
    std::string config_path;
};
```

### Phase 6: Remove Legacy Code
**Goal**: Clean up obsolete platform code

**Audit and Remove**:
- [ ] X11 video drivers (`BasiliskII/src/Unix/video_x.cpp`)
- [ ] SDL1 audio/video (`BasiliskII/src/SDL/`)
- [ ] BeOS support (`BasiliskII/src/BeOS/`)
- [ ] AmigaOS support (`BasiliskII/src/AmigaOS/`)
- [ ] Windows-specific code (if not needed)
- [ ] MacOS X-specific code (if not needed)

**Keep**:
- Linux/Unix essentials
- POSIX-compliant code
- Cross-platform utilities

---

## Testing Strategy

### Phase 2 Test: Video Output
```cpp
// Test in-process buffer
VideoOutput video;
uint32_t test_frame[1024*768];
// ... fill with test pattern ...
video.submit_frame(test_frame, 1024, 768, PIXFMT_BGRA);

// Consumer side (simulated)
const FrameBuffer* frame;
if (video.get_next_frame(&frame)) {
    // Verify frame data
}
```

### Phase 3 Test: WebRTC Integration
1. Build single binary
2. Launch with test config
3. Connect browser to `http://localhost:8080`
4. Verify video/audio streaming works

### Phase 5 Test: Config Migration
```bash
# Old prefs exist
ls ~/.basilisk_ii_prefs

# Run new emulator
./macemu-next

# Check migration
cat ~/macemu-config.json  # Should contain migrated settings
```

---

## Success Criteria

### Functional
- ✅ Single binary launches emulator + WebRTC server
- ✅ Video streaming works (no IPC overhead)
- ✅ Audio streaming works
- ✅ JSON config loads and saves correctly
- ✅ Browser client connects and displays Mac screen

### Performance
- ✅ Lower latency (no IPC marshaling)
- ✅ Fewer threads (no separate process)
- ✅ Simpler architecture (easier to debug)

### Code Quality
- ✅ No legacy OS code
- ✅ No IPC layer
- ✅ Clean separation: emulator core / WebRTC / config
- ✅ Unified build system (Meson)

---

## Timeline Estimate

| Phase | Description | Effort |
|-------|-------------|--------|
| 1 | Merge foundations | ✅ Done |
| 2 | In-process video/audio API | 2-3 days |
| 3 | Integrate WebRTC | 3-4 days |
| 4 | Update build system | 1 day |
| 5 | JSON prefs migration | 2 days |
| 6 | Remove legacy code | 1-2 days |

**Total**: ~10-14 days of focused work

---

## Files to Study First

### Understanding IPC (to replace it)
1. `BasiliskII/src/IPC/ipc_protocol.h` - Buffer structure, atomics
2. `BasiliskII/src/IPC/video_ipc.cpp` - How emulator writes frames
3. `web-streaming/server/ipc/ipc_connection.cpp` - How server reads frames

### Understanding WebRTC Server
1. `web-streaming/server/server.cpp` - Main loop, peer connections
2. `web-streaming/server/config/config_manager.cpp` - JSON config
3. `web-streaming/server/http/http_server.cpp` - API endpoints

### Understanding JSON Config
1. `web-streaming/macemu-config.json` - Config schema
2. `web-streaming/server/utils/json_utils.cpp` - Helper functions
3. `web-streaming/server/storage/prefs_manager.cpp` - Old prefs conversion

---

## Questions to Answer

1. **Threading model**: Main thread = emulator? WebRTC in separate thread?
2. **Buffer ownership**: Who allocates frame buffers? Emulator or video output?
3. **Config hot-reload**: Can we reload JSON without restart?
4. **Backward compat**: Do we support old prefs files? Migrate once?
5. **Build dependencies**: Which libraries are required on target (Raspberry Pi)?

---

## Next Steps

1. **Study IPC code** (today)
   - Read `ipc_protocol.h`
   - Understand triple buffer design
   - Map out what to keep vs remove

2. **Design in-process API** (tomorrow)
   - Sketch `video_output.h` / `audio_output.h`
   - Decide on threading model
   - Plan atomic operations

3. **Prototype integration** (next week)
   - Create stub `video_output.cpp`
   - Test with macemu-next
   - Verify atomics work

4. **Full integration** (following week)
   - Copy WebRTC server code
   - Wire up to in-process buffers
   - Test end-to-end

---

**This is a true merge** - taking the best of both projects and creating something better than either alone!
