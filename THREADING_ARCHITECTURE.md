# Threading Architecture - Complete System Design

**Date**: January 3, 2026
**Project**: macemu-next with integrated WebRTC streaming

---

## Executive Summary

**Architecture**: Single process with 4 threads (optimized for multi-core)

**Design Principles**:
- Lock-free where possible (atomic operations, triple buffers)
- Clear ownership boundaries (no shared mutable state)
- Producer-consumer patterns with ring buffers
- Clean thread lifecycle (join on shutdown)
- Minimal thread count (focus on actual parallelism, not artificial separation)

---

## Thread Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    macemu-next Process                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Thread 1: CPU/MAIN ⚡ (Hot path - JIT execution)              │
│  ├─ Unicorn M68K JIT                                           │
│  ├─ Execute instructions (~12M insns/sec)                      │
│  ├─ EmulOps, traps, interrupts                                 │
│  ├─ Timer interrupts (60 Hz, integrated in main loop)          │
│  ├─ VIA chip emulation                                         │
│  ├─ WRITES → VideoOutput (triple buffer)                       │
│  └─ WRITES → AudioOutput (ring buffer)                         │
│                                                                 │
│  Thread 2: VIDEO ENCODER 🎥 (Blocks on new frames)            │
│  ├─ READS ← VideoOutput (triple buffer)                        │
│  ├─ Encode to H.264/VP9/WebP                                   │
│  ├─ WRITES → WebRTC DataChannel                                │
│  └─ Target: 60 FPS (16.7ms frame budget)                       │
│                                                                 │
│  Thread 3: AUDIO ENCODER 🔊 (Periodic - 20ms chunks)          │
│  ├─ READS ← AudioOutput (ring buffer)                          │
│  ├─ Encode to Opus                                             │
│  ├─ WRITES → WebRTC DataChannel                                │
│  └─ Target: 20ms chunks (WebRTC standard)                      │
│                                                                 │
│  Thread 4: WEB SERVER 🌍 (Event-driven)                       │
│  ├─ HTTP server (serve client HTML/JS/CSS)                     │
│  ├─ WebRTC peer connection management                          │
│  ├─ ICE/STUN/TURN handling                                     │
│  ├─ Signaling (websocket)                                      │
│  ├─ Config API (GET/POST /config)                              │
│  ├─ Control API (POST /reset, /pause)                          │
│  ├─ File scanning API (GET /files)                             │
│  └─ Network I/O (libdatachannel + cpp-httplib)                 │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

**Rationale**:
- **4 threads = 4 cores utilized** (actual parallelism)
- **CPU/Main**: Does all emulation work including timer interrupts (no context switching overhead)
- **Video Encoder**: Dedicated thread for H.264/VP9 encoding (CPU-intensive)
- **Audio Encoder**: Dedicated thread for Opus encoding (lighter but consistent load)
- **Web Server**: Handles all network I/O, HTTP, WebRTC (event-driven, mostly I/O-bound)

---

## Thread Details

### Thread 1: CPU/MAIN ⚡

**Role**: Main thread runs CPU emulation + timer interrupts

**Critical Path**: This is the HOT path - must be as fast as possible

**Loop**:
```cpp
int main(int argc, char** argv) {
    // 1. Load config
    JsonConfig config("macemu-config.json");

    // 2. Initialize shared buffers
    VideoOutput video_output(1920, 1080);  // Max resolution
    AudioOutput audio_output(48000, 2);    // 48kHz stereo

    // 3. Launch encoder threads
    std::thread video_encoder_thread(video_encoder_main, &config, &video_output);
    std::thread audio_encoder_thread(audio_encoder_main, &config, &audio_output);

    // 4. Launch web server thread
    std::thread web_server_thread(web_server_main, &config);

    // 5. Initialize Platform API (on main thread)
    platform_init();
    g_platform.video = &video_output;
    g_platform.audio = &audio_output;

    // 6. Initialize CPU backend (Unicorn)
    g_platform.cpu_init();
    g_platform.cpu_reset();

    // 7. Set up timer for interrupts (60 Hz)
    setup_timer_interrupt(16667);  // 16.667ms = 60 Hz

    // 8. Main CPU execution loop (RUNS ON MAIN THREAD)
    while (!shutdown_requested) {
        int result = g_platform.cpu_execute_one();  // Execute 1 basic block

        // Handle results (STOP, EXCEPTION, etc.)
        if (result != CPU_EXEC_OK) {
            handle_cpu_event(result);
        }
    }

    // 9. Cleanup and join threads
    g_platform.cpu_destroy();
    video_encoder_thread.join();
    audio_encoder_thread.join();
    web_server_thread.join();

    return 0;
}
```

**Writes**:
- **Video**: When framebuffer updates (via Platform API)
  ```cpp
  void VideoDriver::update_screen() {
      video_output->submit_frame(framebuffer, width, height, format);
  }
  ```
- **Audio**: When audio samples ready (~every 20ms)
  ```cpp
  void AudioDriver::write_samples(int16_t* samples, int count) {
      audio_output->submit_samples(samples, count, channels, rate);
  }
  ```

**Reads**:
- **Interrupt flags**: Checked at basic block boundaries (UC_HOOK_BLOCK)
- **Config**: Read-only after startup (safe to read from multiple threads)

**Timer Integration** (runs on main thread):
```cpp
// Set up periodic timer using platform API (e.g., setitimer, timerfd, etc.)
void setup_timer_interrupt(int interval_us) {
    struct itimerval timer;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = interval_us;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = interval_us;

    signal(SIGALRM, timer_signal_handler);
    setitimer(ITIMER_REAL, &timer, NULL);
}

// Signal handler (sets interrupt flag)
void timer_signal_handler(int signum) {
    PendingInterrupt.store(true, std::memory_order_release);
}

// Unicorn block hook checks flag
static void hook_block(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    if (PendingInterrupt.load(std::memory_order_acquire)) {
        PendingInterrupt.store(false, std::memory_order_relaxed);
        process_interrupt(uc);  // Handle interrupt
        update_via_timers();    // Update VIA chip state
    }
}
```

**Performance Notes**:
- ~12M instructions/sec (measured)
- ~6M blocks/sec (avg 2 insns/block)
- JIT compiled (Unicorn → x86-64 native)
- Interrupt checks: every ~2 instructions (block boundary)
- Timer runs via SIGALRM (no extra thread needed)

---

### Thread 2: VIDEO ENCODER 🎥

**Role**: Encode frames from triple buffer → H.264/VP9/WebP

**Loop**:
```cpp
void video_encoder_main(JsonConfig* config, VideoOutput* video_output) {
    // Create encoder based on config
    std::unique_ptr<VideoEncoder> encoder;
    std::string codec = config->get_string("web.codec", "h264");
    if (codec == "h264") {
        encoder = std::make_unique<H264Encoder>();
    } else if (codec == "vp9") {
        encoder = std::make_unique<VP9Encoder>();
    } else if (codec == "webp") {
        encoder = std::make_unique<WebPEncoder>();
    }

    while (!shutdown_requested) {
        // Block until new frame available
        const FrameBuffer* frame = video_output->wait_for_frame();

        if (!frame) continue;  // Timeout or shutdown

        // Encode frame
        EncodedFrame encoded = encoder->encode(frame->pixels,
                                                frame->width,
                                                frame->height);

        // Send to WebRTC
        webrtc_send_video_frame(encoded.data, encoded.size);

        // Release frame (mark as consumed)
        video_output->release_frame();
    }
}
```

**READS**:
- **VideoOutput**: Triple buffer (lock-free atomic reads)
- **Config**: Codec selection (read-only)

**WRITES**:
- **WebRTC DataChannel**: Encoded frames (via queue/channel)

**Blocking**:
- Blocks on `wait_for_frame()` (condition variable or polling)
- Target: 60 FPS = 16.7ms budget per frame
- If encoding takes >16ms, frames will be dropped

**Codec Options**:
- **H.264**: Best quality/bandwidth ratio (hardware accel on many platforms)
- **VP9**: Better compression than H.264 (software only, slower)
- **WebP**: Lossy image compression (fallback for compatibility)
- **PNG**: Lossless (debugging only, huge bandwidth)

---

### Thread 3: AUDIO ENCODER 🔊

**Role**: Encode audio samples from ring buffer → Opus

**Loop**:
```cpp
void audio_encoder_main(JsonConfig* config, AudioOutput* audio_output) {
    OpusEncoder opus(48000, 2);  // 48kHz stereo

    const int frame_samples = 960;  // 20ms at 48kHz = 960 samples
    int16_t frame_buffer[frame_samples * 2];  // Stereo

    while (!shutdown_requested) {
        // Read samples from ring buffer (blocks if not enough data)
        int samples_read = audio_output->read_samples(frame_buffer, frame_samples);

        if (samples_read < frame_samples) {
            // Not enough samples yet, sleep and retry
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Encode to Opus
        uint8_t encoded[4000];  // Max Opus frame size
        int encoded_size = opus.encode(frame_buffer, frame_samples, encoded);

        // Send to WebRTC
        webrtc_send_audio_frame(encoded, encoded_size);
    }
}
```

**READS**:
- **AudioOutput**: Ring buffer (lock-free or mutex-protected)
- **Config**: Sample rate, channels (read-only)

**WRITES**:
- **WebRTC DataChannel**: Encoded audio frames

**Timing**:
- 20ms chunks (WebRTC standard for Opus)
- At 48kHz: 20ms = 960 samples
- Stereo: 960 samples × 2 channels = 1920 int16_t values

**Opus Encoder**:
- Low-latency audio codec (designed for VoIP/WebRTC)
- Typical bitrates: 24-64 kbps for stereo
- Very good quality even at low bitrates

---

### Thread 4: WEB SERVER 🌍

**Role**: HTTP server + WebRTC peer connection management (all network I/O)

**Loop**:
```cpp
void web_server_main(JsonConfig* config) {
    // 1. Set up HTTP server
    HttpServer http_server(8080);

    // Register HTTP routes
    http_server.get("/", [](Request& req, Response& res) {
        res.send_file("client/index.html");
    });

    http_server.get("/config", [&config](Request& req, Response& res) {
        res.json(config->to_json());
    });

    http_server.post("/config", [&config](Request& req, Response& res) {
        config->update(req.json());
        config->save();
        res.json({{"status", "ok"}});
    });

    http_server.post("/reset", [](Request& req, Response& res) {
        g_platform.cpu_reset();  // Direct call to emulator
        res.json({{"status", "ok"}});
    });

    http_server.get("/files/roms", [](Request& req, Response& res) {
        auto roms = scan_rom_directory();
        res.json(roms);
    });

    // 2. Set up WebRTC peer connection
    rtc::Configuration rtc_config;
    rtc_config.iceServers.push_back(rtc::IceServer("stun:stun.l.google.com:19302"));

    auto peer = std::make_shared<rtc::PeerConnection>(rtc_config);

    // Set up data channels
    auto video_channel = peer->createDataChannel("video");
    auto audio_channel = peer->createDataChannel("audio");

    // Set up WebRTC callbacks
    peer->onLocalDescription([](rtc::Description desc) {
        send_sdp_to_client(desc);  // Via HTTP or WebSocket
    });

    peer->onLocalCandidate([](rtc::Candidate candidate) {
        send_ice_candidate_to_client(candidate);
    });

    // 3. Main event loop (handles both HTTP and WebRTC)
    // cpp-httplib and libdatachannel both have internal threads
    // This thread just coordinates their event loops
    http_server.run();  // Blocks, handles requests
}
```

**READS**:
- **Video queue**: Encoded frames from video encoder
- **Audio queue**: Encoded frames from audio encoder
- **Config**: Read/write (mutex-protected)
- **Filesystem**: Scan ROM/disk directories
- **Signaling**: SDP/ICE messages from browser

**WRITES**:
- **WebRTC DataChannels**: Video/audio frames to browser
- **HTTP responses**: Client files, API responses
- **Config**: Updates from browser
- **Signaling**: SDP/ICE messages to browser

**Event-Driven**:
- cpp-httplib has internal threads for HTTP requests
- libdatachannel has internal threads for WebRTC I/O
- This thread coordinates both (mostly I/O-bound, not CPU-intensive)

---

## Inter-Thread Communication

### 1. VideoOutput (CPU → Video Encoder)

**Design**: Triple buffer with atomics (no locks!)

```cpp
class VideoOutput {
private:
    struct FrameBuffer {
        uint32_t* pixels;      // BGRA or ARGB format
        int width;
        int height;
        PixelFormat format;
        uint64_t sequence;     // Frame counter
    };

    FrameBuffer buffers[3];    // Triple buffer
    std::atomic<int> write_index;  // CPU writes here
    std::atomic<int> read_index;   // Encoder reads here

public:
    // Called by CPU thread
    void submit_frame(uint32_t* pixels, int w, int h, PixelFormat fmt) {
        int idx = write_index.load(std::memory_order_acquire);

        // Copy to buffer
        memcpy(buffers[idx].pixels, pixels, w * h * 4);
        buffers[idx].width = w;
        buffers[idx].height = h;
        buffers[idx].format = fmt;
        buffers[idx].sequence++;

        // Advance write pointer (circular)
        write_index.store((idx + 1) % 3, std::memory_order_release);
    }

    // Called by encoder thread
    const FrameBuffer* wait_for_frame() {
        int idx = read_index.load(std::memory_order_acquire);

        // Check if new frame available
        if (buffers[idx].sequence > last_read_sequence) {
            last_read_sequence = buffers[idx].sequence;
            return &buffers[idx];
        }

        return nullptr;  // No new frame
    }
};
```

**Properties**:
- Lock-free (atomic operations only)
- CPU thread never blocks
- Encoder thread polls or waits with timeout
- If CPU produces frames faster than encoder, middle frame is overwritten (acceptable)

---

### 2. AudioOutput (CPU → Audio Encoder)

**Design**: Ring buffer with mutex (simpler than lock-free)

```cpp
class AudioOutput {
private:
    int16_t* ring_buffer;      // Circular buffer
    int capacity;              // Total samples (e.g., 96000 = 1 sec at 48kHz stereo)
    int write_pos;             // Where CPU writes
    int read_pos;              // Where encoder reads
    std::mutex mutex;
    std::condition_variable cv;

public:
    // Called by CPU thread
    void submit_samples(int16_t* samples, int count, int channels, int rate) {
        std::lock_guard<std::mutex> lock(mutex);

        // Copy samples to ring buffer
        for (int i = 0; i < count * channels; i++) {
            ring_buffer[write_pos] = samples[i];
            write_pos = (write_pos + 1) % capacity;
        }

        cv.notify_one();  // Wake encoder if waiting
    }

    // Called by encoder thread
    int read_samples(int16_t* out, int count) {
        std::unique_lock<std::mutex> lock(mutex);

        // Wait for enough samples
        cv.wait(lock, [&]() {
            int available = (write_pos - read_pos + capacity) % capacity;
            return available >= count * 2 || shutdown_requested;
        });

        // Copy samples out
        for (int i = 0; i < count * 2; i++) {
            out[i] = ring_buffer[read_pos];
            read_pos = (read_pos + 1) % capacity;
        }

        return count;
    }
};
```

**Properties**:
- Mutex-protected (acceptable for audio - not hot path)
- Blocking on encoder side (waits for samples)
- Non-blocking on CPU side (just writes and continues)
- Condition variable for efficient waiting

---

### 3. Interrupt Flags (Timer → CPU) - SAME THREAD

**Design**: Simple atomic bool (set by signal handler, checked by CPU loop)

```cpp
// Global (accessed by signal handler and CPU main loop)
std::atomic<bool> PendingInterrupt{false};

// Signal handler (SIGALRM) sets it
void timer_signal_handler(int signum) {
    PendingInterrupt.store(true, std::memory_order_release);
    update_via_timers();  // Update VIA chip state
}

// CPU main loop checks it (in UC_HOOK_BLOCK)
static void hook_block(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    if (PendingInterrupt.load(std::memory_order_acquire)) {
        PendingInterrupt.store(false, std::memory_order_relaxed);
        process_interrupt(uc);
    }
}
```

**Properties**:
- Lock-free
- Very fast (single atomic load in hot path)
- No inter-thread communication (signal handler on same thread)
- No context switching overhead

---

### 4. Config (HTTP ↔ All Threads)

**Design**: Mutex-protected JSON object

```cpp
class JsonConfig {
private:
    nlohmann::json config;
    std::string path;
    mutable std::mutex mutex;

public:
    // Thread-safe read
    std::string get_string(const std::string& key, const std::string& default_val) const {
        std::lock_guard<std::mutex> lock(mutex);
        return json_utils::get_string(config, key, default_val);
    }

    // Thread-safe write
    void set(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex);
        config[key] = value;
    }

    // Thread-safe save
    void save() {
        std::lock_guard<std::mutex> lock(mutex);
        // Write to file...
    }
};
```

**Properties**:
- Mutex-protected (config access is infrequent, so mutex is fine)
- Most threads read config once at startup (safe - no concurrent writes)
- Only HTTP thread writes config (when user updates via web UI)

---

### 5. Encoded Frames (Encoders → WebRTC)

**Design**: Lock-free SPSC queue (single-producer, single-consumer)

```cpp
template<typename T>
class SPSCQueue {
private:
    std::vector<T> buffer;
    std::atomic<int> read_pos{0};
    std::atomic<int> write_pos{0};
    int capacity;

public:
    // Producer (encoder thread)
    bool push(T&& item) {
        int write = write_pos.load(std::memory_order_relaxed);
        int next = (write + 1) % capacity;

        if (next == read_pos.load(std::memory_order_acquire)) {
            return false;  // Queue full
        }

        buffer[write] = std::move(item);
        write_pos.store(next, std::memory_order_release);
        return true;
    }

    // Consumer (WebRTC thread)
    bool pop(T& item) {
        int read = read_pos.load(std::memory_order_relaxed);

        if (read == write_pos.load(std::memory_order_acquire)) {
            return false;  // Queue empty
        }

        item = std::move(buffer[read]);
        read_pos.store((read + 1) % capacity, std::memory_order_release);
        return true;
    }
};
```

**Properties**:
- Lock-free (atomic ring buffer)
- Single producer, single consumer (simpler than MPMC)
- If queue full, drop frame (acceptable for streaming)

---

## Memory Ownership

### Clear Ownership Rules

**VideoOutput buffers**: Owned by VideoOutput class
- CPU thread: Writes to write_index buffer (never freed)
- Encoder thread: Reads from read_index buffer (never freed)
- Lifetime: Allocated at startup, freed at shutdown

**AudioOutput buffer**: Owned by AudioOutput class
- CPU thread: Writes samples (never freed)
- Encoder thread: Reads samples (never freed)
- Lifetime: Allocated at startup, freed at shutdown

**Encoded frame data**: Owned by encoder, transferred to WebRTC
- Encoder: Allocates (e.g., `new uint8_t[]`)
- Queue: Transfers ownership (via `std::unique_ptr`)
- WebRTC: Frees after sending

**Config**: Owned by JsonConfig class
- All threads: Read-only pointers (never freed)
- HTTP thread: Can write (mutex-protected)
- Lifetime: Allocated at startup, freed at shutdown

---

## Performance Characteristics

### CPU Budget (60 FPS target)

**Per-frame budget** (16.7ms):
```
CPU/Main thread:  ~16ms continuous (always running, includes timer interrupts)
Video encoder:    ~10-15ms per frame (can't exceed 16.7ms or frames drop)
Audio encoder:    ~1-2ms every 20ms (negligible)
Web server:       ~0-5ms (mostly idle, event-driven I/O)

Total CPU usage: ~200-250% on 4-core system (2-3 cores busy)
```

**Core Utilization**:
- **Core 1**: CPU/Main (100% busy)
- **Core 2**: Video encoder (60-90% busy)
- **Core 3**: Audio encoder (10-20% busy)
- **Core 4**: Web server (0-30% busy, bursty)

**Actual parallelism**: 4 threads = 4 cores = efficient multi-core usage

### Memory Usage

```
VideoOutput: 3 frames × 1920×1080×4 bytes = ~24 MB
AudioOutput: 1 sec × 48000×2×2 bytes = ~192 KB
Config: ~10 KB
Encoded queues: ~5 MB (frame buffers in flight)
Unicorn JIT cache: ~50-100 MB
Total: ~80-130 MB
```

### Latency Budget

```
Frame capture:     0ms (instant)
Video encode:      10-15ms (H.264)
Network:           10-50ms (depends on connection)
Browser decode:    5-10ms
Display:           16.7ms (vsync)

Total latency:     ~40-90ms (acceptable for interactive use)
```

---

## Shutdown Sequence

**Clean shutdown** (SIGINT/SIGTERM):

```
1. Main thread catches signal
2. Set shutdown_requested flag (atomic bool)
3. All threads check flag in their loops
4. Threads exit their loops
5. Main thread joins all threads (blocks until complete)
6. Destructors clean up resources
7. Process exits
```

**Critical**: No abrupt kills - always join threads cleanly

---

## Future Optimizations

### Possible Improvements

**Video encoder threading**:
- Could use separate threads for different codecs
- H.264 hardware encoding (GPU) - offload from CPU
- Multiple encoder threads for tiling (parallel encoding)

**Audio resampling**:
- If Mac audio rate ≠ 48kHz, need resampling thread
- Could be done in audio encoder thread (simpler)

**Disk I/O**:
- Currently synchronous (blocks CPU thread)
- Could add async I/O thread for disk reads/writes
- Use io_uring on Linux (modern async I/O)

**Multiple clients**:
- Currently single WebRTC peer
- Could add multiple encoder threads (one per client)
- Each client gets own encoder thread

---

## Summary Table

| Thread | Role | Frequency | Blocking | Memory Writes | Memory Reads |
|--------|------|-----------|----------|---------------|--------------|
| **CPU/Main** | M68K emulation + timer | ~12M insns/sec | No | VideoOutput, AudioOutput, Interrupt flags | Interrupt flags |
| **Video Enc** | H.264/VP9 | 60 FPS (16.7ms) | Yes (wait frame) | Encoded queue | VideoOutput |
| **Audio Enc** | Opus | 50 Hz (20ms) | Yes (wait samples) | Encoded queue | AudioOutput |
| **Web Server** | HTTP + WebRTC I/O | Event-driven | Yes (I/O) | Config, DataChannels | Config, filesystem, encoded queues |

**Total threads**: 4 (4 workers, no orchestration thread)

**Lock-free paths**: Video triple buffer, interrupt flags, encoded queues
**Mutex-protected**: Audio ring buffer, config access

**Advantages**:
- **Simple**: 4 threads = 4 cores (easy to understand)
- **Efficient**: No unnecessary thread context switches
- **Clean**: Each thread has clear, focused responsibility
- **Fast**: Timer interrupts via signal (no thread overhead)

---

This architecture is **clean, efficient, and maintainable** - much simpler than both the IPC approach and the original 7-thread design!
