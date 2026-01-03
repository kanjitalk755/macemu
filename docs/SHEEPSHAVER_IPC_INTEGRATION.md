# SheepShaver IPC Integration Plan

**Goal**: Port the IPC video/audio/input driver from BasiliskII to SheepShaver, enabling web streaming of PowerPC Mac OS 8/9 via the existing WebRTC server.

**Status**: Planning phase
**Target**: Full feature parity with BasiliskII web streaming + SheepShaver-specific enhancements

---

## Executive Summary

### What Changes

**In SheepShaver:**
- Add IPC video driver (`SheepShaver/src/IPC/`)
- Integrate with existing video driver architecture (`VideoDoDriverIO`)
- Support Display Manager control codes (cursor, VBL, mode switching)
- Add configure/build system support for `ENABLE_IPC_VIDEO`/`ENABLE_IPC_AUDIO`

**In web-streaming server:**
- Add emulator type detection (m68k vs PPC)
- Support multiple prefs files (`basilisk_ii.prefs`, `sheepshaver.prefs`)
- Add UI selector for emulator architecture
- Implement hardware cursor rendering in browser
- Minor config/launch adjustments (mostly path changes)

**Protocol compatibility:**
- IPC protocol is **100% identical** (same SHM layout, same socket protocol)
- Server needs **zero protocol changes** - just launch different binary

---

## Architecture Comparison

### BasiliskII (68k, Mac OS 7)

```
┌─────────────────────────────────────────┐
│ monitor_desc (C++ base class)           │
│  ├─ vector<video_mode> modes            │
│  ├─ virtual switch_to_current_mode()    │
│  ├─ virtual set_palette()                │
│  └─ virtual set_gamma()                  │
└─────────────────────────────────────────┘
                    ▲
                    │ inherits
                    │
┌─────────────────────────────────────────┐
│ IPC_monitor_desc                        │
│  ├─ Implements virtual methods          │
│  ├─ Manages SHM/socket connection       │
│  └─ Converts framebuffer → BGRA         │
└─────────────────────────────────────────┘
```

**Driver hooks:**
- Simple control codes (cscSetMode, cscSetEntries)
- Memory polling for cursor (reads 0x844)
- VideoInterrupt() for VBL (basic)

### SheepShaver (PPC, Mac OS 8/9)

```
┌─────────────────────────────────────────┐
│ VideoInfo VModes[64] (global array)     │
│  ├─ viType, viRowBytes                  │
│  ├─ viXsize, viYsize                    │
│  ├─ viAppleMode (depth)                 │
│  └─ viAppleID (resolution)              │
└─────────────────────────────────────────┘
                    │
                    ▼
┌─────────────────────────────────────────┐
│ VideoDoDriverIO()                       │
│  ├─ kInitCommand → VideoInit()          │
│  ├─ kOpenCommand → VideoOpen()          │
│  ├─ kControlCommand → VideoControl()    │
│  └─ kStatusCommand → VideoStatus()      │
└─────────────────────────────────────────┘
                    │
                    ▼
┌─────────────────────────────────────────┐
│ Platform video driver (video_x.cpp)     │
│  ├─ video_set_cursor()                  │
│  ├─ video_set_palette()                 │
│  ├─ VideoVBL() → VSLDoInterruptService()│
│  └─ NQD acceleration hooks (optional)   │
└─────────────────────────────────────────┘
```

**Driver hooks:**
- Full Display Manager (cscSwitchMode, cscSetHardwareCursor, etc.)
- Driver-based cursor via control codes
- VSL interrupt services for VBL
- NQD (Native QuickDraw) acceleration

---

## Implementation Phases

### Phase 1: Core IPC Driver (Week 1-2)

**Files to create:**

```
SheepShaver/src/IPC/
├── ipc_protocol.h        # Symlink to BasiliskII version (identical)
├── video_ipc_sheep.cpp   # SheepShaver-specific video driver
├── audio_ipc.cpp         # Symlink/copy from BasiliskII (99% same)
├── audio_ipc.h           # Symlink/copy from BasiliskII
├── audio_config.h        # Symlink/copy from BasiliskII
└── control_ipc.cpp       # Symlink/copy from BasiliskII
```

**video_ipc_sheep.cpp structure:**

```cpp
// Based on BasiliskII/src/IPC/video_ipc.cpp but adapted for SheepShaver

#include "sysdeps.h"
#include "video.h"
#include "video_defs.h"
#include "ipc_protocol.h"

// Global state (like other SheepShaver video drivers)
static VideoSharedMemory* video_shm = nullptr;
static int control_socket = -1;
static uint8* the_buffer = nullptr;  // Mac framebuffer
static std::thread video_thread;

// Platform-specific functions called by video.cpp
bool VideoInit(void) {
    // Create SHM /dev/shm/macemu-video-{pid}
    // Create socket /tmp/macemu-{pid}.sock
    // Allocate Mac framebuffer (vm_acquire)
    // Populate VModes[] array with supported modes
    // Start video refresh thread
}

void VideoExit(void) {
    // Stop video thread
    // Cleanup SHM/socket
}

void VideoVBL(void) {
    // Trigger VSL interrupt service
    if (private_data && private_data->interruptsEnabled) {
        VSLDoInterruptService(private_data->vslServiceID);
    }
}

int16 video_mode_change(VidLocals *csSave, uint32 ParamPtr) {
    // Handle Display Manager mode switching
    // Update VModes[cur_mode]
    // Update SHM width/height
}

void video_set_cursor(void) {
    // Copy MacCursor[68] to SHM for server
    if (video_shm) {
        memcpy(video_shm->cursor_data, MacCursor, 68);
        video_shm->cursor_visible = private_data->cursorVisible;
    }
}

void video_set_palette(void) {
    // Store palette for indexed→BGRA conversion
}

// Video refresh thread (60 FPS)
static void video_refresh_thread() {
    while (video_thread_running) {
        // Convert Mac framebuffer (screen_base) → BGRA
        convert_frame_to_bgra();

        // Signal server via eventfd
        macemu_frame_complete();

        // Trigger VBL interrupt
        if (private_data && private_data->interruptsEnabled) {
            VSLDoInterruptService(private_data->vslServiceID);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}
```

**Key differences from BasiliskII version:**

1. **No monitor_desc class** - use global VModes[] array
2. **VideoDoDriverIO integration** - called from video.cpp
3. **VidLocals support** - maintain driver context structure
4. **VSL interrupts** - proper Mac OS 8/9 VBL handling
5. **Hardware cursor** - full cscSetHardwareCursor/cscDrawHardwareCursor support
6. **Display Manager** - handle all standard control/status codes

---

### Phase 2: Build System Integration (Week 1)

**Files to modify:**

```
SheepShaver/src/Unix/configure.ac
SheepShaver/src/Unix/Makefile.in
SheepShaver/src/Unix/config.h.in
```

**Changes:**

```bash
# configure.ac additions
AC_ARG_ENABLE(ipc-video,
  [  --enable-ipc-video      enable IPC video driver],
  [WANT_IPC_VIDEO=$enableval], [WANT_IPC_VIDEO=no])

AC_ARG_ENABLE(ipc-audio,
  [  --enable-ipc-audio      enable IPC audio driver],
  [WANT_IPC_AUDIO=$enableval], [WANT_IPC_AUDIO=no])

if [[ "x$WANT_IPC_VIDEO" = "xyes" ]]; then
  AC_DEFINE(ENABLE_IPC_VIDEO, 1, [Define if using IPC video driver])
  SYSSRCS="$SYSSRCS ../IPC/video_ipc_sheep.cpp ../IPC/control_ipc.cpp"
fi

if [[ "x$WANT_IPC_AUDIO" = "xyes" ]]; then
  AC_DEFINE(ENABLE_IPC_AUDIO, 1, [Define if using IPC audio driver])
  SYSSRCS="$SYSSRCS ../IPC/audio_ipc.cpp"
fi
```

**Build command:**

```bash
cd SheepShaver/src/Unix
./autogen.sh
./configure --enable-ipc-video --enable-ipc-audio \
            --without-gtk --disable-vosf --disable-xf86-dga
make
```

**Result:** `SheepShaver` binary with IPC support

---

### Phase 3: Hardware Cursor Support (Week 2)

**In SheepShaver IPC driver:**

```cpp
// video_ipc_sheep.cpp

void video_set_cursor(void) {
    if (!video_shm) return;

    // Copy cursor bitmap (16x16 monochrome + mask)
    memcpy(video_shm->cursor_data, MacCursor, 68);

    // Update cursor state
    if (private_data) {
        video_shm->cursor_x = private_data->cursorX;
        video_shm->cursor_y = private_data->cursorY;
        video_shm->cursor_visible = private_data->cursorVisible;
    }
}

// Called from VideoControl() when Mac OS updates cursor
case cscSetHardwareCursor: {
    // ... existing SheepShaver cursor handling ...
    video_set_cursor();  // Propagate to IPC
    return noErr;
}

case cscDrawHardwareCursor: {
    // ... existing SheepShaver cursor handling ...
    video_set_cursor();  // Propagate to IPC
    return noErr;
}
```

**In ipc_protocol.h (add to MacEmuVideoBuffer):**

```c
// Cursor data (68 bytes = MacCursor format)
uint8_t cursor_data[68];     // [0-1] size/version, [2-3] hotspot, [4-35] image, [36-67] mask
uint16_t cursor_x;           // Current X position
uint16_t cursor_y;           // Current Y position
uint8_t cursor_visible;      // 1=visible, 0=hidden
uint8_t cursor_padding[3];   // Alignment
```

**In web-streaming client (client.js):**

```javascript
// Hardware cursor rendering
let cursorCanvas = document.getElementById('cursor-overlay');
let cursorCtx = cursorCanvas.getContext('2d');
let currentCursor = null;

function renderHardwareCursor(cursorData) {
    // cursorData = {
    //   image: Uint8Array(32),  // 16x16 monochrome bitmap
    //   mask: Uint8Array(32),   // 16x16 mask
    //   hotX: number,
    //   hotY: number,
    //   x: number,
    //   y: number,
    //   visible: boolean
    // }

    if (!cursorData.visible) {
        cursorCanvas.style.display = 'none';
        return;
    }

    // Convert 1-bit bitmap to ImageData
    let imgData = cursorCtx.createImageData(16, 16);
    for (let y = 0; y < 16; y++) {
        for (let x = 0; x < 16; x++) {
            let byteIdx = y * 2 + (x >> 3);
            let bitIdx = 7 - (x & 7);

            let pixel = (cursorData.image[byteIdx] >> bitIdx) & 1;
            let maskBit = (cursorData.mask[byteIdx] >> bitIdx) & 1;

            let idx = (y * 16 + x) * 4;
            if (maskBit) {
                imgData.data[idx] = pixel ? 0 : 255;     // R
                imgData.data[idx+1] = pixel ? 0 : 255;   // G
                imgData.data[idx+2] = pixel ? 0 : 255;   // B
                imgData.data[idx+3] = 255;               // A (opaque)
            } else {
                imgData.data[idx+3] = 0;  // Transparent
            }
        }
    }

    // Draw to canvas
    cursorCtx.putImageData(imgData, 0, 0);

    // Position overlay
    cursorCanvas.style.left = (cursorData.x - cursorData.hotX) + 'px';
    cursorCanvas.style.top = (cursorData.y - cursorData.hotY) + 'px';
    cursorCanvas.style.display = 'block';
}

// Server sends cursor updates via data channel
dataChannel.addEventListener('message', (event) => {
    let msg = JSON.parse(event.data);
    if (msg.type === 'cursor') {
        renderHardwareCursor(msg.cursor);
    }
});
```

**In server (server.cpp):**

```cpp
// Periodically send cursor updates when changed
static uint64_t last_cursor_hash = 0;

void check_cursor_update() {
    if (!g_ipc_shm) return;

    uint64_t current_hash = hash_memory(g_ipc_shm->cursor_data, 68);
    current_hash ^= (g_ipc_shm->cursor_x << 16) | g_ipc_shm->cursor_y;
    current_hash ^= g_ipc_shm->cursor_visible;

    if (current_hash != last_cursor_hash) {
        json cursor_msg = {
            {"type", "cursor"},
            {"cursor", {
                {"image", base64_encode(g_ipc_shm->cursor_data + 4, 32)},
                {"mask", base64_encode(g_ipc_shm->cursor_data + 36, 32)},
                {"hotX", g_ipc_shm->cursor_data[2]},
                {"hotY", g_ipc_shm->cursor_data[3]},
                {"x", g_ipc_shm->cursor_x},
                {"y", g_ipc_shm->cursor_y},
                {"visible", g_ipc_shm->cursor_visible == 1}
            }}
        };

        // Send to all connected peers
        send_to_all_data_channels(cursor_msg.dump());

        last_cursor_hash = current_hash;
    }
}
```

---

### Phase 4: Web UI Enhancements (Week 2)

**Changes to web-streaming/client/index.html:**

```html
<!-- Add emulator selector before codec selector -->
<div class="emulator-selector">
    <label for="emulator-select">Emulator:</label>
    <select id="emulator-select" onchange="changeEmulator()">
        <option value="basilisk" selected>Basilisk II (68k)</option>
        <option value="sheepshaver">SheepShaver (PPC)</option>
    </select>
</div>

<!-- Update title dynamically -->
<h1 id="emulator-title">Basilisk II Web</h1>
```

**Changes to client.js:**

```javascript
let currentEmulator = 'basilisk';  // 'basilisk' or 'sheepshaver'

function changeEmulator() {
    let select = document.getElementById('emulator-select');
    currentEmulator = select.value;

    // Update UI
    let title = currentEmulator === 'basilisk' ? 'Basilisk II Web' : 'SheepShaver Web';
    document.getElementById('emulator-title').textContent = title;

    // Notify server to restart with different emulator
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({
            type: 'change_emulator',
            emulator: currentEmulator
        }));
    }
}

// Send emulator preference when starting
function startEmulator() {
    fetch('/api/emulator/start', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
            emulator: currentEmulator
        })
    })
    .then(response => response.json())
    .then(data => {
        console.log('Started emulator:', data);
        updateStatus('Starting emulator...');
    });
}
```

---

### Phase 5: Server Adjustments (Week 2)

**Changes to server_config.h:**

```cpp
struct ServerConfig {
    // ... existing fields ...

    std::string emulator_type = "basilisk";  // "basilisk" or "sheepshaver"
    std::string basilisk_path = "./build/BasiliskII";
    std::string sheepshaver_path = "./build/SheepShaver";
    std::string basilisk_prefs = "basilisk_ii.prefs";
    std::string sheepshaver_prefs = "sheepshaver.prefs";

    // Derived getters
    std::string get_emulator_path() const {
        return emulator_type == "sheepshaver" ? sheepshaver_path : basilisk_path;
    }

    std::string get_prefs_path() const {
        return emulator_type == "sheepshaver" ? sheepshaver_prefs : basilisk_prefs;
    }
};
```

**Changes to process_manager.cpp:**

```cpp
pid_t ProcessManager::launch_emulator() {
    std::string emulator_path = config_.get_emulator_path();
    std::string prefs_path = config_.get_prefs_path();

    // Check if files exist
    if (access(emulator_path.c_str(), X_OK) != 0) {
        fprintf(stderr, "Emulator binary not found: %s\n", emulator_path.c_str());
        return -1;
    }

    if (access(prefs_path.c_str(), R_OK) != 0) {
        fprintf(stderr, "Prefs file not found: %s\n", prefs_path.c_str());
        return -1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        execl(emulator_path.c_str(),
              emulator_path.c_str(),
              "--config", prefs_path.c_str(),
              nullptr);
        _exit(1);
    }

    return pid;
}
```

**Changes to API handlers:**

```cpp
// POST /api/emulator/start
void handle_start_emulator(const httplib::Request& req, httplib::Response& res) {
    auto json_body = json::parse(req.body);

    if (json_body.contains("emulator")) {
        std::string emulator_type = json_body["emulator"];
        if (emulator_type == "basilisk" || emulator_type == "sheepshaver") {
            g_config.emulator_type = emulator_type;
        }
    }

    pid_t pid = process_manager.launch_emulator();

    res.set_content(json{
        {"success", pid > 0},
        {"pid", pid},
        {"emulator", g_config.emulator_type}
    }.dump(), "application/json");
}
```

**Protocol detection (automatic):**

The server doesn't need to know the emulator type for IPC - the protocol is identical! Only the prefs file and binary path differ.

```cpp
// Server can auto-detect from SHM if needed
if (g_ipc_shm && g_ipc_shm->magic == MACEMU_VIDEO_MAGIC) {
    // Both emulators use same magic - protocol is identical
    // Server handles video/audio/input the same way
}
```

---

### Phase 6: Testing & Validation (Week 3)

**Test matrix:**

| Feature | BasiliskII | SheepShaver | Notes |
|---------|-----------|-------------|-------|
| Video streaming | ✅ | 🧪 Test | H.264, VP9, PNG |
| Audio streaming | ✅ | 🧪 Test | Opus, 44.1kHz |
| Keyboard input | ✅ | 🧪 Test | Browser → Mac |
| Mouse input (relative) | ✅ | 🧪 Test | FPS-style |
| Mouse input (absolute) | ✅ | 🧪 Test | Touch/tablet |
| Hardware cursor | ⚠️ Polling | 🧪 Test | Driver-based |
| Mode switching | ✅ | 🧪 Test | 640x480 ↔ 1024x768 |
| Color depth switching | ✅ | 🧪 Test | 8-bit ↔ 32-bit |
| VBL interrupts | ⚠️ Basic | 🧪 Test | VSL services |
| NQD acceleration | ❌ N/A | ✅ Works | Orthogonal to IPC |
| Multiple peers | ✅ | 🧪 Test | Same SHM, same codec |

**Test procedure:**

```bash
# 1. Build both emulators with IPC
cd BasiliskII/src/Unix
./configure --enable-ipc-video --enable-ipc-audio && make

cd ../../SheepShaver/src/Unix
./configure --enable-ipc-video --enable-ipc-audio && make

# 2. Copy binaries to web-streaming
cp BasiliskII/src/Unix/BasiliskII web-streaming/build/
cp SheepShaver/src/Unix/SheepShaver web-streaming/build/

# 3. Create SheepShaver prefs
cat > web-streaming/sheepshaver.prefs <<EOF
rom storage/roms/SheepShaver_ROM.rom
screen ipc/1024/768
ramsize 134217728
cpu 4
fpu true
modelid 14
nogui true
webcodec h264
mousemode relative
EOF

# 4. Start server
cd web-streaming
./macemu-webrtc --emulator basilisk

# 5. Test in browser
# - Switch to SheepShaver in UI
# - Click "Start"
# - Verify video/audio/cursor work
```

---

## File Checklist

### New Files

```
SheepShaver/src/IPC/
├── ipc_protocol.h          # Symlink → ../../BasiliskII/src/IPC/ipc_protocol.h
├── video_ipc_sheep.cpp     # NEW (adapted from BasiliskII version)
├── audio_ipc.cpp           # Symlink → ../../BasiliskII/src/IPC/audio_ipc.cpp
├── audio_ipc.h             # Symlink → ../../BasiliskII/src/IPC/audio_ipc.h
├── audio_config.h          # Symlink → ../../BasiliskII/src/IPC/audio_config.h
└── control_ipc.cpp         # Symlink → ../../BasiliskII/src/IPC/control_ipc.cpp

web-streaming/
├── sheepshaver.prefs       # NEW (example SheepShaver config)
└── build/
    └── SheepShaver         # NEW (compiled binary)

docs/
└── SHEEPSHAVER_IPC_INTEGRATION.md  # THIS FILE
```

### Modified Files

```
SheepShaver/src/Unix/
├── configure.ac            # Add --enable-ipc-video/audio
├── Makefile.in             # Add IPC sources to SYSSRCS
└── config.h.in             # Add ENABLE_IPC_VIDEO/AUDIO defines

web-streaming/client/
├── index.html              # Add emulator selector UI
├── client.js               # Add emulator switching, cursor rendering
└── styles.css              # Style cursor overlay

web-streaming/server/
├── config/server_config.h  # Add emulator_type, paths
├── emulator/process_manager.cpp  # Support both emulators
└── http/api_handlers.cpp   # Handle emulator selection
```

---

## Key Design Decisions

### 1. Protocol Compatibility

**Decision:** Use 100% identical IPC protocol for both emulators.

**Rationale:**
- Server is emulator-agnostic (doesn't care about 68k vs PPC)
- Same SHM layout, same socket protocol, same eventfd signaling
- Reduces maintenance burden (one protocol spec)
- Allows future emulator additions (PowerPC Mac mini, anyone?)

### 2. Cursor Implementation

**Decision:** SheepShaver uses driver-based cursor (cscSetHardwareCursor), BasiliskII keeps memory polling.

**Rationale:**
- SheepShaver has proper Display Manager support
- BasiliskII's Mac OS 7 uses low-memory globals
- Both send cursor data to server via SHM
- Browser rendering is identical

### 3. Build System

**Decision:** Keep separate binaries, share IPC code via symlinks.

**Rationale:**
- BasiliskII and SheepShaver have different main loops
- Symlinks avoid code duplication
- Configure flags enable IPC per-emulator
- Clean separation of concerns

### 4. UI Architecture

**Decision:** Single-page app with runtime emulator switching.

**Rationale:**
- Better UX than separate URLs
- Server handles emulator lifecycle
- Config persists across sessions
- Easier deployment (one server instance)

---

## Performance Considerations

### SheepShaver-Specific Optimizations

**NQD Acceleration:**
- Works transparently with IPC (modifies screen_base, IPC reads it)
- ~10-100x faster QuickDraw operations
- No changes needed - orthogonal systems

**VBL Timing:**
- Use VSL interrupts for accurate 60 Hz
- Call `VSLDoInterruptService()` from video thread
- Better animation/game timing than BasiliskII

**Dirty Rectangle Optimization:**
- Track dirty areas from NQD hooks
- Convert only changed regions to BGRA
- Reduce CPU usage (especially at 1920x1200)

**Example:**

```cpp
// In video refresh thread
void video_refresh_thread() {
    while (running) {
        // Get dirty rect from NQD (if available)
        int dirty_x, dirty_y, dirty_w, dirty_h;
        video_get_dirty_area(&dirty_x, &dirty_y, &dirty_w, &dirty_h);

        if (dirty_w > 0 && dirty_h > 0) {
            // Convert only dirty region
            convert_dirty_rect_to_bgra(dirty_x, dirty_y, dirty_w, dirty_h);
        } else {
            // Full frame (first frame, mode change, etc.)
            convert_frame_to_bgra();
        }

        // Signal + VBL
        macemu_frame_complete();
        VSLDoInterruptService(private_data->vslServiceID);

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}
```

---

## Migration Path

### Incremental Rollout

**Phase 1:** Core driver (no UI changes)
- Build SheepShaver with IPC
- Test with manual `./SheepShaver` launch
- Verify server connects and streams

**Phase 2:** Basic UI (emulator selector)
- Add dropdown to client
- Update server config
- Test switching between emulators

**Phase 3:** Hardware cursor
- Implement cursor data in SHM
- Add browser rendering
- Test cursor accuracy

**Phase 4:** Polish
- Prefs management UI
- Error handling
- Documentation

---

## Future Enhancements

### Beyond MVP

**1. Unified Prefs Editor**
- Web UI for editing both prefs formats
- Visual disk/ROM picker
- Real-time validation

**2. Save States**
- Freeze emulator state
- Store to server storage
- Load on demand

**3. Multi-User**
- Authentication
- Per-user ROM/disk storage
- Session management

**4. Advanced Cursor**
- Color cursor support (Mac OS 8+)
- Cursor scaling
- Custom cursors

**5. Performance Tuning**
- Adaptive frame rate
- Quality presets
- Bandwidth optimization

---

## Risks & Mitigations

| Risk | Impact | Probability | Mitigation |
|------|--------|-------------|------------|
| Display Manager incompatibility | High | Low | Test thoroughly with Mac OS 8/9 |
| VSL interrupt timing issues | Medium | Medium | Fallback to basic VideoInterrupt() |
| Cursor hotspot inference fails | Low | Low | Use known cursor patterns (arrow, etc.) |
| Build system conflicts | Medium | Low | Separate configure flags, test both |
| Server crashes on bad SHM | High | Low | Validate magic/version, handle errors |

---

## Success Metrics

**Must Have:**
- ✅ SheepShaver streams video at 60 FPS (H.264)
- ✅ Audio plays without glitches (Opus)
- ✅ Keyboard/mouse input works
- ✅ Mode switching (640x480 ↔ 1024x768)
- ✅ UI selector between BasiliskII/SheepShaver

**Should Have:**
- ✅ Hardware cursor renders correctly
- ✅ VBL interrupts trigger at 60 Hz
- ✅ NQD acceleration works with IPC
- ✅ Multiple browser connections supported

**Nice to Have:**
- ✅ Dirty rectangle optimization
- ✅ Color depth switching (8-bit ↔ 32-bit)
- ✅ Web prefs editor for SheepShaver

---

## Timeline

**Week 1:**
- Day 1-2: Create video_ipc_sheep.cpp skeleton
- Day 3-4: Implement VideoInit/Exit, basic rendering
- Day 5: Build system integration (configure.ac)

**Week 2:**
- Day 1-2: Hardware cursor implementation
- Day 3-4: Web UI emulator selector
- Day 5: Server config adjustments

**Week 3:**
- Day 1-3: Testing & debugging
- Day 4: Documentation
- Day 5: Polish & review

**Total:** ~3 weeks for feature parity with BasiliskII

---

## Dependencies

**Code:**
- BasiliskII IPC driver (reference implementation)
- SheepShaver video.cpp (driver integration point)
- web-streaming server (server.cpp, client.js)

**Build tools:**
- autoconf/automake
- C++17 compiler (for std::thread)
- POSIX SHM support

**Runtime:**
- Linux (Ubuntu 22.04+)
- Browser with WebRTC + WebCodecs
- SheepShaver ROM file

---

## Appendix: Code Snippets

### A. Example sheepshaver.prefs with IPC

```ini
# SheepShaver Web Streaming Configuration

# ROM (required - get from Mac OS 9 CD or download)
rom storage/roms/SheepShaver_ROM.rom

# Display - IPC driver for web streaming
screen ipc/1024/768

# Memory
ramsize 134217728

# CPU
cpu 4
fpu true
modelid 14

# Disable GUI (web-only)
nogui true

# WebRTC codec (h264, vp9, av1, png)
webcodec h264

# Mouse mode (relative for FPS, absolute for touch)
mousemode relative

# Disk images
disk storage/images/MacOS9.dsk

# CD-ROM
cdrom storage/images/MacOS9_Install.iso
```

### B. Example configure command

```bash
#!/bin/bash
# Build SheepShaver with IPC support

cd SheepShaver/src/Unix

# Clean
make clean 2>/dev/null
rm -f config.cache config.log

# Configure
./configure \
    --enable-ipc-video \
    --enable-ipc-audio \
    --without-gtk \
    --disable-vosf \
    --with-cpu=x86_64 \
    CXXFLAGS="-O3 -march=native"

# Build
make -j$(nproc)

# Test
./SheepShaver --help
```

### C. Example server launch script

```bash
#!/bin/bash
# Launch web-streaming server with SheepShaver

cd web-streaming

# Build server (if needed)
if [ ! -f macemu-webrtc ]; then
    make -j$(nproc)
fi

# Ensure SheepShaver binary exists
if [ ! -f build/SheepShaver ]; then
    echo "Error: SheepShaver binary not found!"
    echo "Build it first: cd ../SheepShaver/src/Unix && make"
    exit 1
fi

# Ensure prefs file exists
if [ ! -f sheepshaver.prefs ]; then
    echo "Error: sheepshaver.prefs not found!"
    echo "Create it from the template in docs/"
    exit 1
fi

# Launch server
./macemu-webrtc \
    --http-port 8000 \
    --signaling-port 8090 \
    --roms storage/roms \
    --images storage/images \
    --emulator build/SheepShaver \
    --prefs sheepshaver.prefs \
    --codec h264

# Server will auto-start emulator and stream to browser at http://localhost:8000
```

---

**End of Integration Plan**

*Last updated: 2025-12-29*
