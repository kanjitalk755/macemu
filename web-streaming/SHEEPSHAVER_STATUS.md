# SheepShaver IPC Integration Status

**Date:** 2025-12-29
**Status:** ✅ Basic integration complete, ready for testing

---

## Completed Work

### 1. SheepShaver IPC Video Driver ✅

**Location:** `SheepShaver/src/IPC/video_ipc_sheep.cpp` (458 lines)

**Features implemented:**
- Shared memory (`/dev/shm/macemu-video-{pid}`) creation and management
- Triple-buffered frame synchronization (lock-free)
- Mac ARGB → BGRA conversion for WebRTC
- 60 FPS video refresh thread
- VSL interrupt integration for proper Mac OS 8/9 VBL
- Cursor position tracking (bitmap data TODO)
- Mode switching support
- Eventfd-based frame signaling

**Build system:**
- Modified `configure.ac` to add `--enable-ipc-video` flag
- Modified `Makefile.in` to include IPC sources
- Created symlinks to shared BasiliskII IPC code

**Build command:**
```bash
cd SheepShaver/src/Unix
autoconf
./configure --enable-ipc-video --disable-vosf --without-gtk
make
```

**Binary:** `SheepShaver/src/Unix/SheepShaver` (7.6 MB)

---

### 2. Web Client Updates ✅

**Files modified:**
- `web-streaming/client/index.html` - Added emulator selector dropdown
- `web-streaming/client/client.js` - Added `changeEmulator()` function

**Features:**
- Dropdown to select between "Basilisk II (68k)" and "SheepShaver (PPC)"
- Dynamic page title update based on selection
- Sends selection to server via `/api/emulator` endpoint

---

### 3. Web Server Updates ✅

**Files modified:**
- `web-streaming/server/http/api_handlers.h` - Added `handle_emulator_change()` declaration
- `web-streaming/server/http/api_handlers.cpp` - Implemented `/api/emulator` POST endpoint

**Features:**
- Accepts `{"emulator": "basilisk"}` or `{"emulator": "sheepshaver"}` JSON requests
- Validates emulator selection
- Returns success response (restart logic TODO)

**Build:** Rebuilt successfully with new endpoint

---

### 4. Configuration Files ✅

**Created:**
- `web-streaming/sheepshaver.prefs` - SheepShaver configuration template
  - 128 MB RAM
  - 1024x768 IPC screen
  - H.264 codec
  - Relative mouse mode
  - ExtFS support

**Existing:**
- `web-streaming/basilisk_ii.prefs` - BasiliskII configuration (already working)

---

### 5. Binary Deployment ✅

**Copied to `web-streaming/build/`:**
- `BasiliskII` (13 MB) - With IPC video/audio
- `SheepShaver` (7.6 MB) - With IPC video

Both binaries ready for server launch.

---

## Architecture Overview

```
┌─────────────────────────────────────────┐
│ Browser (client.js)                     │
│  ├─ Emulator selector dropdown          │
│  ├─ Dynamic title                       │
│  └─ POST /api/emulator                  │
└─────────────────┬───────────────────────┘
                  │ HTTP/WebSocket
┌─────────────────▼───────────────────────┐
│ Web Server (macemu-webrtc)              │
│  ├─ /api/emulator endpoint              │
│  ├─ Serves client HTML/JS               │
│  ├─ WebRTC signaling                    │
│  └─ Launches emulator process           │
└─────────────────┬───────────────────────┘
                  │ Unix socket + SHM
┌─────────────────▼───────────────────────┐
│ Emulator (BasiliskII or SheepShaver)    │
│  ├─ IPC video driver                    │
│  ├─ Writes BGRA frames to SHM           │
│  ├─ Signals via eventfd                 │
│  └─ Reads input from socket             │
└─────────────────────────────────────────┘
```

---

## What Works

1. ✅ SheepShaver compiles with IPC video driver
2. ✅ Web UI shows emulator selector
3. ✅ Client sends emulator selection to server
4. ✅ Server accepts and validates emulator selection
5. ✅ Both binaries deployed to `build/` directory
6. ✅ Configuration files created for both emulators

---

## What's TODO

### Immediate (Server-side):

1. **Emulator binary selection logic** - Server needs to:
   - Store selected emulator type in config
   - Choose correct binary path (`./build/BasiliskII` vs `./build/SheepShaver`)
   - Choose correct prefs file (`basilisk_ii.prefs` vs `sheepshaver.prefs`)
   - Currently hardcoded to BasiliskII

2. **Audio IPC for SheepShaver** - Currently disabled due to conflicts:
   - Need to exclude SDL audio when IPC audio is enabled
   - Implement `IPC_GetVideoSHM()` helper function
   - Similar to video override logic in configure.ac

### Testing:

3. **SheepShaver ROM requirement** - Need valid ROM file:
   - Location: `web-streaming/storage/roms/SheepShaver_ROM.rom`
   - Can be extracted from Mac OS 9 CD or compatible ROM dump
   - Without ROM, SheepShaver won't boot

4. **SheepShaver disk image** - Need bootable Mac OS 8/9 disk:
   - Add to `sheepshaver.prefs`: `disk storage/images/MacOS9.dsk`
   - Can create with SheepShaver standalone or use existing image

### Future Enhancements:

5. **Hardware cursor bitmap support** - Current implementation only tracks position
   - Extend IPC protocol to include cursor bitmap data
   - Implement browser-side cursor rendering overlay
   - See `docs/SHEEPSHAVER_IPC_INTEGRATION.md` for full plan

6. **Unified prefs editor** - Web UI for editing both formats
7. **Save states** - Freeze/restore emulator state
8. **Multi-user support** - Per-user ROM/disk storage

---

## Testing Instructions

### Prerequisites:

```bash
# Ensure storage directories exist
mkdir -p web-streaming/storage/{roms,images}

# For SheepShaver, you need:
# 1. ROM file at: web-streaming/storage/roms/SheepShaver_ROM.rom
# 2. Mac OS 8/9 disk image (configured in sheepshaver.prefs)

# For BasiliskII, you need:
# 1. ROM file (already configured in basilisk_ii.prefs)
# 2. Optional: disk/CD images
```

### Start Server:

```bash
cd web-streaming
./build/macemu-webrtc
```

Server listens on:
- HTTP: `http://localhost:8000`
- WebSocket signaling: `ws://localhost:8090`

### Test in Browser:

1. Open `http://localhost:8000`
2. Select "Basilisk II (68k)" or "SheepShaver (PPC)" from dropdown
3. Click "Start" to launch emulator
4. Video should stream via WebRTC (H.264 codec)
5. Keyboard/mouse input should work

### Expected Behavior:

**BasiliskII (working):**
- ✅ Should connect and stream immediately
- ✅ Mac OS 7 desktop appears
- ✅ Input works (keyboard/mouse)

**SheepShaver (new, untested):**
- ⚠️ Requires ROM file (won't boot without it)
- ⚠️ Server currently hardcoded to BasiliskII binary
- ⚠️ Need to implement binary selection logic first
- 🧪 After fixes: Should boot Mac OS 8/9

---

## Protocol Compatibility

**IPC Protocol v4** is **100% identical** between BasiliskII and SheepShaver:
- Same SHM layout (`MacEmuIPCBuffer`)
- Same socket protocol (binary input messages)
- Same eventfd signaling
- Server doesn't need protocol changes - just different binary path!

This is a huge win - one protocol for both emulators.

---

## File Structure

```
macemu/
├── SheepShaver/src/
│   ├── IPC/
│   │   ├── video_ipc_sheep.cpp    # NEW - SheepShaver video driver
│   │   ├── ipc_protocol.h         # Symlink → BasiliskII
│   │   ├── control_ipc.cpp        # Symlink → BasiliskII
│   │   ├── control_ipc.h          # Symlink → BasiliskII
│   │   ├── audio_ipc.cpp          # Symlink → BasiliskII (not built yet)
│   │   ├── audio_ipc.h            # Symlink → BasiliskII
│   │   └── audio_config.h         # Symlink → BasiliskII
│   └── Unix/
│       ├── configure.ac           # MODIFIED - Add --enable-ipc-video
│       ├── Makefile.in            # MODIFIED - Add -I../IPC
│       └── SheepShaver            # Built binary (7.6 MB)
├── web-streaming/
│   ├── build/
│   │   ├── macemu-webrtc          # Server binary (12 MB)
│   │   ├── BasiliskII             # Emulator (13 MB)
│   │   └── SheepShaver            # Emulator (7.6 MB) NEW
│   ├── client/
│   │   ├── index.html             # MODIFIED - Add emulator selector
│   │   └── client.js              # MODIFIED - Add changeEmulator()
│   ├── server/http/
│   │   ├── api_handlers.h         # MODIFIED - Add handle_emulator_change()
│   │   └── api_handlers.cpp       # MODIFIED - Implement /api/emulator
│   ├── basilisk_ii.prefs          # BasiliskII config
│   ├── sheepshaver.prefs          # NEW - SheepShaver config
│   └── SHEEPSHAVER_STATUS.md      # This file
└── docs/
    └── SHEEPSHAVER_IPC_INTEGRATION.md  # Full integration plan
```

---

## Next Steps

**Priority 1: Make it work**
1. Add emulator binary selection logic to server
2. Test with SheepShaver ROM (need to acquire)
3. Fix any runtime issues

**Priority 2: Audio**
1. Enable IPC audio for SheepShaver
2. Fix audio_sdl conflicts in build system

**Priority 3: Polish**
1. Hardware cursor bitmap support
2. Better error messages when ROM missing
3. Prefs file validation

---

## Summary

**What's done:**
- ✅ Full IPC video driver for SheepShaver (458 lines of C++)
- ✅ Web UI emulator selector
- ✅ Server API endpoint (basic)
- ✅ Both binaries built and deployed
- ✅ Configuration files created

**What's needed to test:**
- 🔧 Server binary selection logic (50 lines of code)
- 📀 SheepShaver ROM file
- 💾 Mac OS 8/9 disk image

**Estimated effort to working prototype:** 1-2 hours (mostly finding ROM/disk image)

---

*Last updated: 2025-12-29 13:50 UTC*
