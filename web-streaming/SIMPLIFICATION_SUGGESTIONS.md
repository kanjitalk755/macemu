# WebRTC Server Simplification Suggestions

## ✅ Completed: Removed Stdin Monitor Thread

**What was removed:**
- `monitor_stdin_for_capture()` function and thread
- `g_capture_requested` global flag
- `g_webrtc_server` global pointer
- `send_capture_trigger()` method
- Stdin terminal manipulation (termios)
- Audio capture debug code

**Impact:**
- **One less thread** (4 threads → 3 threads)
- **Simpler lifecycle** (no detached threads)
- **Removed dependencies**: termios.h
- **Cleaner codebase**: ~100 lines removed

**Rationale:**
- This was purely a debug feature for synchronized audio capture
- Not needed for production use
- If audio debugging needed, can be triggered via API endpoint instead

---

## Recommended Simplifications

### 1. **Merge Audio Thread into Video Thread** ⭐ High Impact

**Current State:**
- Video thread: Waits on eventfd → encodes video → sends
- Audio thread: Sleeps 20ms → requests audio → encodes → sends
- Both run continuously in parallel

**Proposed Change:**
Combine into single media processing thread:
```cpp
while (g_running) {
    // Wait for frame with timeout
    epoll_wait(epoll_fd, events, 2, 20);  // 20ms timeout

    // Check video eventfd
    if (frame_event_ready) {
        encode_and_send_video();
    }

    // Process audio every 20ms (timeout or explicit check)
    static auto last_audio = now();
    if (now() - last_audio >= 20ms) {
        encode_and_send_audio();
        last_audio = now();
    }
}
```

**Benefits:**
- ✅ One less thread (3 → 2 threads)
- ✅ Simpler synchronization
- ✅ Natural audio/video interleaving
- ✅ Easier to coordinate shutdown
- ✅ Better cache locality (same thread accesses IPC)

**Trade-offs:**
- ⚠️ Audio timing slightly less precise (but 20ms is audio frame duration anyway)
- ⚠️ Video encoding spikes could delay audio (but typically <5ms)

**Recommendation:** **Do this** - audio/video are tightly coupled via IPC anyway

---

### 2. **Remove Unused/Legacy Code** ⭐ Medium Impact

**Dead Code Identified:**
```cpp
// server.cpp warnings:
static std::string json_escape(const std::string& s) { ... }  // Line 291
static std::string get_storage_json() { ... }                 // Line 525
static bool write_prefs_file(const std::string& content) { ... }  // Line 530
static void read_webcodec_pref() { ... }                      // Line 546
static std::string read_prefs_file() { ... }                  // Line 536
```

**Action:** Delete these functions - they're replaced by modular code in:
- `http/api_handlers.cpp` (handles storage/prefs API)
- `storage/file_scanner.cpp` (scans storage)
- `storage/prefs_manager.cpp` (reads/writes prefs)

**Benefits:**
- ✅ ~200 lines removed
- ✅ No compiler warnings
- ✅ Cleaner codebase

---

### 3. **Consolidate Codec Encoders** ⭐ Low Impact

**Current State:**
- H264Encoder, AV1Encoder, PNGEncoder - 3 separate classes
- All created at startup even if not used
- Pass all 3 to video_loop()

**Proposed Change:**
Use polymorphism:
```cpp
class VideoEncoder {
    virtual EncodedFrame encode(const uint8_t* bgra, ...) = 0;
    virtual void request_keyframe() = 0;
};

class H264Encoder : public VideoEncoder { ... };
class AV1Encoder : public VideoEncoder { ... };
class PNGEncoder : public VideoEncoder { ... };

// In main:
std::unique_ptr<VideoEncoder> encoder;
switch (g_server_codec) {
    case H264: encoder = std::make_unique<H264Encoder>(); break;
    case AV1:  encoder = std::make_unique<AV1Encoder>(); break;
    case PNG:  encoder = std::make_unique<PNGEncoder>(); break;
}

video_loop(webrtc, *encoder);  // Pass single encoder
```

**Benefits:**
- ✅ Only create encoder you need
- ✅ Simpler function signatures
- ✅ Easier to add new codecs
- ✅ Less memory usage

**Trade-offs:**
- ⚠️ Can't hot-swap codecs without reconnecting
- ⚠️ Virtual function call overhead (negligible)

**Recommendation:** **Maybe later** - current design supports multi-codec peers

---

### 4. **Simplify WebRTC Server Initialization** ⭐ Low Impact

**Current State:**
```cpp
WebRTCServer webrtc;
webrtc.init(g_signaling_port);
webrtc.set_key_input_callback(...);
webrtc.set_mouse_input_callback(...);
webrtc.set_ping_input_callback(...);
webrtc.set_command_callback(...);
webrtc.set_stun_config(...);
webrtc.set_codec(...);
```

**Proposed Change:**
Use builder pattern or constructor:
```cpp
struct WebRTCConfig {
    int signaling_port;
    CodecType codec;
    bool enable_stun;
    std::string stun_server;
    KeyInputCallback key_cb;
    MouseInputCallback mouse_cb;
    // ...
};

WebRTCServer webrtc(config);  // All-in-one initialization
```

**Benefits:**
- ✅ Clearer initialization order
- ✅ Validation at construction time
- ✅ Immutable after construction
- ✅ Easier to test

---

### 5. **Remove Global Variables** ⭐ High Impact (Long-term)

**Current Globals:**
```cpp
static std::atomic<bool> g_running(true);
static std::atomic<bool> g_emulator_connected(false);
static ipc::IPCConnection g_ipc;
static pid_t g_emulator_pid = -1;
// ... many more
```

**Proposed Change:**
Create `ServerState` struct:
```cpp
struct ServerState {
    std::atomic<bool> running{true};
    std::atomic<bool> emulator_connected{false};
    ipc::IPCConnection ipc;
    pid_t emulator_pid = -1;
    // ...
};

void video_loop(ServerState& state, ...) {
    while (state.running) {
        if (!state.emulator_connected) { ... }
    }
}
```

**Benefits:**
- ✅ No global state
- ✅ Easier to test (inject state)
- ✅ Could run multiple servers in one process
- ✅ Clear ownership and lifecycle

**Trade-offs:**
- ⚠️ Large refactoring effort
- ⚠️ Need to pass state everywhere

**Recommendation:** **Future work** - requires significant refactoring

---

### 6. **Remove HTTP Server Thread** ⭐ Medium Impact

**Current State:**
- HTTP server runs in separate thread
- Uses poll() with 100ms timeout
- Relatively low traffic (just API calls)

**Proposed Change:**
Add HTTP socket to main epoll:
```cpp
// In video_loop:
epoll_add(http_server_fd);
epoll_add(frame_ready_eventfd);

while (g_running) {
    epoll_wait(epoll_fd, events, MAX_EVENTS, 20);  // 20ms for audio

    for (event in events) {
        if (event.fd == http_server_fd) {
            http_server.handle_connection();
        } else if (event.fd == frame_ready_eventfd) {
            encode_video();
        }
    }

    // Audio every 20ms
    if (should_process_audio) {
        encode_audio();
    }
}
```

**Benefits:**
- ✅ One less thread (2 → 1 thread!)
- ✅ Single event loop (epoll for everything)
- ✅ Simpler architecture

**Trade-offs:**
- ⚠️ HTTP requests could delay video/audio (but they're rare)
- ⚠️ More complex epoll setup

**Recommendation:** **Consider this** - worth the simplification

---

### 7. **Simplify Emulator Process Management** ⭐ Low Impact

**Current State:**
- `start_emulator_process()` - complex with multiple paths
- `monitor_emulator_process()` - polling with waitpid
- `stop_emulator()` - signal sending
- Spread across multiple functions

**Proposed Change:**
Create `EmulatorManager` class:
```cpp
class EmulatorManager {
    pid_t start(const std::string& binary, const std::string& prefs);
    bool is_running() const;
    void stop();
    int wait();  // Blocking wait for exit

private:
    pid_t pid_ = -1;
    std::thread monitor_thread_;  // Optional: for async monitoring
};
```

**Benefits:**
- ✅ Encapsulated lifecycle
- ✅ Clear ownership
- ✅ Easier to test
- ✅ RAII cleanup

---

## Priority Ranking

### **Quick Wins** (Do Now):
1. ✅ **Remove stdin monitor** - DONE
2. **Remove unused functions** - Easy, no risk, clean warnings
3. **Consolidate includes** - Remove unused headers

### **Medium Effort** (Next Sprint):
4. **Merge audio/video threads** - Significant simplification
5. **Remove HTTP server thread** - Single event loop

### **Long-term** (Future):
6. **Remove global variables** - Large refactor but worth it
7. **Codec encoder consolidation** - Nice to have
8. **Builder pattern for config** - Quality of life

---

## Estimated Impact

| Simplification | Lines Removed | Threads Removed | Complexity Reduction |
|----------------|---------------|-----------------|---------------------|
| Stdin monitor ✅ | ~100 | 1 | Low → None |
| Unused functions | ~200 | 0 | Low |
| Merge audio/video | ~50 | 1 | Medium → Low |
| Remove HTTP thread | ~100 | 1 | Medium → Low |
| **Total (if all done)** | **~450** | **3** | **High → Low** |

**Final State:** 1 thread (main event loop) handling:
- HTTP connections (epoll)
- Video frames (epoll on eventfd)
- Audio frames (20ms timer)
- All in one clean epoll-based event loop

---

## Implementation Order

### Phase 1: Cleanup (This Commit)
- ✅ Remove stdin monitor thread
- ⬜ Remove unused functions
- ⬜ Remove unused includes

### Phase 2: Thread Consolidation
- ⬜ Merge audio thread into video thread
- ⬜ Test thoroughly (audio timing is critical)

### Phase 3: Event Loop Unification
- ⬜ Remove HTTP server thread
- ⬜ Add HTTP fd to epoll
- ⬜ Single event loop

### Phase 4: Structural Improvements (Optional)
- ⬜ Remove globals → ServerState struct
- ⬜ Builder pattern for config
- ⬜ Codec encoder consolidation

---

## Testing Strategy

After each simplification:
1. **Build test**: `make clean && make -j4`
2. **Start test**: Server starts without errors
3. **Connect test**: Browser connects successfully
4. **Video test**: Smooth video playback (30fps)
5. **Audio test**: Clean audio without dropouts
6. **Reconnect test**: Stop/start emulator works
7. **Load test**: Multiple browser connections
8. **Latency test**: Ping/pong RTT measurement works

---

## Recommendation Summary

**Do immediately:**
1. ✅ Remove stdin monitor (done)
2. Remove unused functions (trivial)
3. Merge audio/video threads (good ROI)

**Consider for next version:**
4. Remove HTTP thread (nice to have)
5. Remove globals (long-term goal)

**Skip for now:**
6. Codec consolidation (current design is fine)
7. EmulatorManager class (current code works)

This would take the server from **4 threads** (main + audio + HTTP + stdin) down to **1 thread** (unified event loop), making it dramatically simpler and easier to maintain!
