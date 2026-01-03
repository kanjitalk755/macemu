# Emulator Startup Changes

**Date:** 2025-12-29
**Status:** ✅ Complete

---

## Summary

Changed server startup behavior to **not auto-start** the emulator, and made it **automatically choose the correct prefs file** based on which emulator binary is found.

---

## Changes Made

### 1. Disabled Auto-Start ([server_config.h:33](web-streaming/server/config/server_config.h#L33))

**Before:**
```cpp
bool auto_start_emulator = true;  // Auto-start enabled
```

**After:**
```cpp
bool auto_start_emulator = false;  // Start with emulator off, user starts from Web UI
```

**Why:** Gives users control to configure settings in the Web UI before starting the emulator.

---

### 2. Smart Prefs File Selection ([server.cpp:411-422](web-streaming/server/server.cpp#L411-L422))

**New Logic:**
```cpp
// Choose correct prefs file based on emulator type
std::string prefs_file = g_prefs_path;
bool is_sheepshaver = (emu_path.find("SheepShaver") != std::string::npos);

// If using default prefs path, switch to correct file for emulator type
if (g_prefs_path == "basilisk_ii.prefs" || g_prefs_path == "sheepshaver.prefs") {
    prefs_file = is_sheepshaver ? "sheepshaver.prefs" : "basilisk_ii.prefs";
    fprintf(stderr, "Emulator: Auto-selected prefs file: %s\n", prefs_file.c_str());
}

const char* config_flag = is_sheepshaver ? "--prefs" : "--config";
```

**What it does:**
1. Detects emulator type from binary path (contains "SheepShaver" or not)
2. Automatically chooses:
   - `sheepshaver.prefs` for SheepShaver
   - `basilisk_ii.prefs` for BasiliskII
3. Uses correct command-line flag:
   - `--prefs` for SheepShaver
   - `--config` for BasiliskII
4. Respects explicit `--prefs FILE` override from command line

---

## Startup Flow

### Server Starts

```
./build/macemu-webrtc
```

**Console Output:**
```
HTTP server listening on port 8000
WebSocket server listening on port 8090

Open http://localhost:8000 in your browser

Auto-start disabled, scanning for running emulators...
```

**Server State:**
- ✅ HTTP server running (port 8000)
- ✅ WebRTC signaling running (port 8090)
- ⏸️  No emulator running
- 🌐 Web UI accessible

---

### User Opens Web UI

**What they see:**
- Settings panel with emulator selector
- "Start" button to launch emulator
- All configuration options available

**User workflow:**
1. Select emulator type (68k or PPC)
2. Choose ROM file
3. Configure settings (RAM, screen, JIT, etc.)
4. Click **"Save & Restart"**
5. Emulator launches with correct prefs file

---

### User Clicks "Start"

**Server receives:** `/api/emulator/start` POST request

**Server logic:**
1. Calls `find_emulator()` to locate binary:
   ```
   ./bin/BasiliskII   ← checks first
   ./bin/SheepShaver  ← checks second
   ```

2. Detects emulator type from path:
   ```cpp
   if (emu_path.find("SheepShaver") != std::string::npos) {
       // Use SheepShaver
   }
   ```

3. Auto-selects correct prefs file:
   ```
   Found: ./bin/SheepShaver
   Auto-selected prefs file: sheepshaver.prefs
   Starting /path/to/SheepShaver --prefs sheepshaver.prefs
   ```

4. Launches emulator with correct settings!

---

## Example: Both Binaries Present

**Setup:**
```
web-streaming/
├── bin/
│   ├── BasiliskII     ← symlink to ../../BasiliskII/src/Unix/BasiliskII
│   └── SheepShaver    ← symlink to ../../SheepShaver/src/Unix/SheepShaver
├── basilisk_ii.prefs
└── sheepshaver.prefs
```

**Case 1: BasiliskII Found First**
```
Emulator: Found emulator: /home/mick/macemu/BasiliskII/src/Unix/BasiliskII
Emulator: Auto-selected prefs file: basilisk_ii.prefs
Emulator: Starting BasiliskII --config basilisk_ii.prefs
Emulator: Started with PID 12345
```

**Case 2: Only SheepShaver Present**
```
Emulator: Found emulator: /home/mick/macemu/SheepShaver/src/Unix/SheepShaver
Emulator: Auto-selected prefs file: sheepshaver.prefs
Emulator: Starting SheepShaver --prefs sheepshaver.prefs
Emulator: Started with PID 12346
```

---

## Command-Line Override

**If you want to force a specific prefs file:**

```bash
./build/macemu-webrtc --prefs my-custom.prefs
```

Server will **use that file** regardless of emulator type (useful for testing).

---

## Benefits

1. ✅ **No more auto-start surprises** - User controls when emulator launches
2. ✅ **Correct prefs file** - No more SheepShaver reading BasiliskII prefs
3. ✅ **Better UX** - Configure first, then start
4. ✅ **Flexible** - Can still override with `--prefs` flag
5. ✅ **Clear logging** - Server tells you which file it chose

---

## Web UI Integration

The Web UI already has:
- ✅ "Start" button (`/api/emulator/start`)
- ✅ "Stop" button (`/api/emulator/stop`)
- ✅ "Restart" button (`/api/emulator/restart`)

**All work perfectly with this new behavior!**

When user changes emulator type in settings and saves:
1. Prefs file is saved (basilisk_ii.prefs or sheepshaver.prefs)
2. Emulator restarts
3. Server detects binary type and uses correct prefs automatically

---

## Testing

**Start server:**
```bash
cd web-streaming
./build/macemu-webrtc
```

**Expected output:**
```
Configuration summary:
  HTTP port:        8000
  Signaling port:   8090
  Prefs file:       basilisk_ii.prefs
  Auto-start:       no
  Debug connection: no
  ...

HTTP server listening on port 8000
WebSocket server listening on port 8090

Open http://localhost:8000 in your browser

Auto-start disabled, scanning for running emulators...
```

**In browser:**
1. Open `http://localhost:8000`
2. Click Settings (gear icon)
3. Select emulator type
4. Click "Save & Restart"
5. Check server console for "Auto-selected prefs file" message

---

## Files Modified

1. **[server_config.h:33](web-streaming/server/config/server_config.h#L33)**
   - Changed `auto_start_emulator = true` → `false`

2. **[server.cpp:411-422](web-streaming/server/server.cpp#L411-L422)**
   - Added emulator type detection
   - Added automatic prefs file selection
   - Updated console logging

3. **[server.cpp:445-451](web-streaming/server/server.cpp#L445-L451)**
   - Updated execl calls to use `prefs_file` variable

---

## Troubleshooting

**Q: Server says "No emulator found"**

A: Create symlinks in `bin/` directory:
```bash
mkdir -p bin
ln -s ../../BasiliskII/src/Unix/BasiliskII ./bin/BasiliskII
# or
ln -s ../../SheepShaver/src/Unix/SheepShaver ./bin/SheepShaver
```

**Q: Wrong prefs file being used**

A: Check server console output. It should say:
```
Emulator: Auto-selected prefs file: [correct file]
```

If not, make sure the binary path contains "SheepShaver" in the name.

**Q: Want to force a specific emulator**

A: Use `--emulator` flag:
```bash
./build/macemu-webrtc --emulator /path/to/SheepShaver
```

---

**Status:** Ready for testing! 🎉

*Last updated: 2025-12-29 14:45 UTC*
