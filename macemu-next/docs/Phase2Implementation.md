# Phase 2 Implementation Plan - Main Thread & Buffer System

**Date**: January 3, 2026
**Status**: Ready to implement
**Goal**: Create main CPU thread with timer interrupts and video/audio buffer copy mechanisms

---

## Overview

We're implementing the **4-thread architecture** starting with the **main CPU thread**. This thread will:

1. Run the Unicorn M68K CPU emulation (hot path)
2. Handle timer interrupts via SIGALRM signal (60 Hz for Mac)
3. Copy video frames from Mac memory → VideoOutput buffer
4. Copy audio samples from Mac memory → AudioOutput buffer
5. Spawn and manage the 3 worker threads (video encoder, audio encoder, web server)

---

## Current Architecture (Before Changes)

### Current main.cpp Flow
```cpp
int main() {
    // Setup
    platform_init();
    load_rom();
    Init680x0();
    cpu_backend_install();
    cpu_init();

    // CPU loop (simple, no threads)
    for (;;) {
        result = g_platform.cpu_execute_one();
        // Handle result...
    }
}
```

### Current Video/Audio Handling
- **Video**: `VideoRefresh()` called periodically from timer code
  - Currently does nothing (null driver)
  - Mac writes to framebuffer at `VideoMonitor.mac_frame_base`
  - No copying or encoding happens

- **Audio**: `audio_enter_stream()` / `audio_exit_stream()`
  - Currently stubs (no actual audio output)
  - Mac writes samples via Sound Manager API
  - Callbacks in `audio.cpp` handle Mac's audio component dispatch

### Current Timer System
- `TimerInit()` sets up Mac Time Manager emulation
- Can use `PRECISE_TIMING_POSIX` for pthread-based timer
- Currently no VIA (Versatile Interface Adapter) interrupts

---

## Target Architecture (After Changes)

### New main.cpp Structure
```cpp
int main(int argc, char** argv) {
    // 1. Load config
    JsonConfig config("macemu-config.json");

    // 2. Initialize shared buffers
    VideoOutput video_output(1920, 1080);
    AudioOutput audio_output(48000, 2);

    // 3. Launch worker threads
    std::thread video_encoder_thread(video_encoder_main, &config, &video_output);
    std::thread audio_encoder_thread(audio_encoder_main, &config, &audio_output);
    std::thread web_server_thread(web_server_main, &config);

    // 4. Platform init (existing code, adapted)
    platform_init();
    load_rom(argv[1]);
    Init680x0();
    cpu_backend_install();

    // 5. Wire up buffers to platform
    g_platform.video_output = &video_output;
    g_platform.audio_output = &audio_output;

    // 6. Initialize CPU
    cpu_init();
    cpu_reset();

    // 7. Set up timer interrupt (60 Hz)
    setup_timer_interrupt(16667);  // microseconds

    // 8. Main CPU loop (RUNS ON MAIN THREAD)
    while (!shutdown_requested) {
        g_platform.cpu_execute_one();
    }

    // 9. Clean shutdown
    cpu_destroy();
    video_encoder_thread.join();
    audio_encoder_thread.join();
    web_server_thread.join();

    return 0;
}
```

---

## Component Design

### 1. Timer Interrupt System

**Implementation**: SIGALRM-based timer (no separate thread)

**Code**:
```cpp
// Global interrupt flag (checked by Unicorn block hook)
std::atomic<bool> PendingInterrupt{false};

// Signal handler (called by kernel on SIGALRM)
void timer_signal_handler(int signum) {
    PendingInterrupt.store(true, std::memory_order_release);

    // Update VIA chip state (when we add VIA emulation)
    // update_via_timers();
}

// Setup function (called once at startup)
void setup_timer_interrupt(int interval_us) {
    struct itimerval timer;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = interval_us;  // 16667 for 60 Hz
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = interval_us;

    // Register signal handler
    signal(SIGALRM, timer_signal_handler);

    // Start periodic timer
    if (setitimer(ITIMER_REAL, &timer, NULL) < 0) {
        fprintf(stderr, "Failed to set up timer: %s\n", strerror(errno));
        exit(1);
    }

    printf("Timer interrupt setup: %d Hz\n", 1000000 / interval_us);
}
```

**Integration with Unicorn**:
```cpp
// In unicorn_wrapper.c, block hook already checks PendingInterrupt
static void hook_block(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    if (PendingInterrupt.load(std::memory_order_acquire)) {
        PendingInterrupt.store(false, std::memory_order_relaxed);
        process_interrupt(uc);
    }
}
```

**Existing Code**: `unicorn_wrapper.c` already has this structure! Just need to add `setup_timer_interrupt()` call.

---

### 2. VideoOutput Buffer System

**Design**: Triple buffer with atomic write-release/read-acquire pattern (adapted from IPC protocol)

**Header** (`src/platform/video_output.h`):
```cpp
#ifndef VIDEO_OUTPUT_H
#define VIDEO_OUTPUT_H

#include <atomic>
#include <cstdint>
#include <cstring>

// Pixel formats (matching IPC protocol)
enum PixelFormat {
    PIXFMT_ARGB = 0,  // Mac native 32-bit: A,R,G,B bytes
    PIXFMT_BGRA = 1,  // Converted: B,G,R,A bytes
};

// Single frame buffer
struct FrameBuffer {
    uint32_t* pixels;        // BGRA or ARGB pixels
    int width;
    int height;
    PixelFormat format;
    uint64_t sequence;       // Frame number (for detecting new frames)
    uint64_t timestamp_us;   // CLOCK_REALTIME

    // Dirty rectangle (for PNG optimization)
    uint32_t dirty_x, dirty_y;
    uint32_t dirty_width, dirty_height;
};

class VideoOutput {
public:
    VideoOutput(int max_width, int max_height);
    ~VideoOutput();

    // Called by CPU thread (emulator) when frame is ready
    void submit_frame(uint32_t* mac_pixels, int width, int height,
                      PixelFormat fmt, uint64_t timestamp_us);

    // Called by video encoder thread to get next frame
    // Returns pointer to frame buffer, or nullptr if no new frame
    const FrameBuffer* wait_for_frame(int timeout_ms);

    // Get current write buffer (for direct writes)
    FrameBuffer* get_write_buffer();

    // Mark current write buffer as complete
    void frame_complete(uint64_t timestamp_us);

private:
    FrameBuffer buffers[3];          // Triple buffer
    std::atomic<int> write_index;    // Buffer CPU is writing to (0-2)
    std::atomic<int> ready_index;    // Buffer ready for encoder (0-2)
    uint64_t last_read_sequence;     // Last frame read by encoder

    int max_width_;
    int max_height_;
};

#endif // VIDEO_OUTPUT_H
```

**Implementation** (`src/platform/video_output.cpp`):
```cpp
#include "video_output.h"
#include <sys/time.h>
#include <unistd.h>

VideoOutput::VideoOutput(int max_width, int max_height)
    : write_index(0), ready_index(0), last_read_sequence(0),
      max_width_(max_width), max_height_(max_height)
{
    // Allocate pixel buffers for all 3 frames
    for (int i = 0; i < 3; i++) {
        buffers[i].pixels = new uint32_t[max_width * max_height];
        buffers[i].width = 0;
        buffers[i].height = 0;
        buffers[i].format = PIXFMT_BGRA;
        buffers[i].sequence = 0;
        buffers[i].timestamp_us = 0;
        buffers[i].dirty_x = 0;
        buffers[i].dirty_y = 0;
        buffers[i].dirty_width = 0;
        buffers[i].dirty_height = 0;
    }
}

VideoOutput::~VideoOutput() {
    for (int i = 0; i < 3; i++) {
        delete[] buffers[i].pixels;
    }
}

void VideoOutput::submit_frame(uint32_t* mac_pixels, int width, int height,
                                 PixelFormat fmt, uint64_t timestamp_us) {
    // Get current write buffer
    int idx = write_index.load(std::memory_order_acquire);
    FrameBuffer* buf = &buffers[idx];

    // Copy pixel data
    int pixel_count = width * height;
    memcpy(buf->pixels, mac_pixels, pixel_count * 4);

    // Update metadata (will be visible after write_index store)
    buf->width = width;
    buf->height = height;
    buf->format = fmt;
    buf->timestamp_us = timestamp_us;
    buf->dirty_x = 0;
    buf->dirty_y = 0;
    buf->dirty_width = width;   // Full frame for now
    buf->dirty_height = height;

    // Increment sequence (signals new frame to encoder)
    buf->sequence++;

    // Publish frame: ready_index = current, write_index = next
    // Memory ordering: release ensures all writes above are visible
    ready_index.store(idx, std::memory_order_release);
    write_index.store((idx + 1) % 3, std::memory_order_release);
}

const FrameBuffer* VideoOutput::wait_for_frame(int timeout_ms) {
    // Check if new frame available
    int idx = ready_index.load(std::memory_order_acquire);
    const FrameBuffer* buf = &buffers[idx];

    if (buf->sequence > last_read_sequence) {
        last_read_sequence = buf->sequence;
        return buf;
    }

    // No new frame yet - encoder can poll or sleep briefly
    usleep(timeout_ms * 1000);
    return nullptr;
}

FrameBuffer* VideoOutput::get_write_buffer() {
    int idx = write_index.load(std::memory_order_acquire);
    return &buffers[idx];
}

void VideoOutput::frame_complete(uint64_t timestamp_us) {
    int idx = write_index.load(std::memory_order_acquire);
    buffers[idx].timestamp_us = timestamp_us;
    buffers[idx].sequence++;

    ready_index.store(idx, std::memory_order_release);
    write_index.store((idx + 1) % 3, std::memory_order_release);
}
```

---

### 3. AudioOutput Buffer System

**Design**: Ring buffer with mutex protection (simpler than lock-free, audio not hot path)

**Header** (`src/platform/audio_output.h`):
```cpp
#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <mutex>
#include <condition_variable>
#include <cstdint>

class AudioOutput {
public:
    AudioOutput(int sample_rate, int channels);
    ~AudioOutput();

    // Called by CPU thread (emulator) with audio samples
    // samples: int16_t array (big-endian or little-endian based on Mac)
    // count: number of samples (NOT bytes)
    void submit_samples(int16_t* samples, int count, int channels, int rate);

    // Called by audio encoder thread to get samples
    // Returns number of samples read (may be less than requested)
    int read_samples(int16_t* out, int max_samples);

    // Get current buffer fill level
    int available_samples();

private:
    int16_t* ring_buffer_;      // Circular buffer
    int capacity_;              // Total samples (e.g., 96000 = 1 sec @ 48kHz stereo)
    int write_pos_;
    int read_pos_;
    std::mutex mutex_;
    std::condition_variable cv_;

    int sample_rate_;
    int channels_;
};

#endif // AUDIO_OUTPUT_H
```

**Implementation** (`src/platform/audio_output.cpp`):
```cpp
#include "audio_output.h"
#include <cstring>
#include <algorithm>

AudioOutput::AudioOutput(int sample_rate, int channels)
    : capacity_(sample_rate * channels * 2),  // 2 seconds buffer
      write_pos_(0), read_pos_(0),
      sample_rate_(sample_rate), channels_(channels)
{
    ring_buffer_ = new int16_t[capacity_];
    memset(ring_buffer_, 0, capacity_ * sizeof(int16_t));
}

AudioOutput::~AudioOutput() {
    delete[] ring_buffer_;
}

void AudioOutput::submit_samples(int16_t* samples, int count, int channels, int rate) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Copy samples to ring buffer (circular)
    for (int i = 0; i < count * channels; i++) {
        ring_buffer_[write_pos_] = samples[i];
        write_pos_ = (write_pos_ + 1) % capacity_;

        // Handle buffer overflow (overwrite old data)
        if (write_pos_ == read_pos_) {
            read_pos_ = (read_pos_ + 1) % capacity_;
        }
    }

    // Wake up encoder if waiting
    cv_.notify_one();
}

int AudioOutput::read_samples(int16_t* out, int max_samples) {
    std::unique_lock<std::mutex> lock(mutex_);

    // Wait for enough samples (or timeout)
    cv_.wait_for(lock, std::chrono::milliseconds(50), [&]() {
        int available = (write_pos_ - read_pos_ + capacity_) % capacity_;
        return available >= max_samples;
    });

    // Read available samples (may be less than requested)
    int available = (write_pos_ - read_pos_ + capacity_) % capacity_;
    int to_read = std::min(available, max_samples);

    for (int i = 0; i < to_read; i++) {
        out[i] = ring_buffer_[read_pos_];
        read_pos_ = (read_pos_ + 1) % capacity_;
    }

    return to_read;
}

int AudioOutput::available_samples() {
    std::lock_guard<std::mutex> lock(mutex_);
    return (write_pos_ - read_pos_ + capacity_) % capacity_;
}
```

---

### 4. Platform API Integration

**Update platform.h** to include buffer pointers:
```cpp
struct Platform {
    // ... existing fields ...

    // Video/Audio output buffers (set by main thread)
    VideoOutput* video_output;
    AudioOutput* audio_output;

    // ... existing function pointers ...
};

extern Platform g_platform;  // Global instance
```

**Video driver implementation** (`src/drivers/video/video_webrtc.cpp` - NEW FILE):
```cpp
#include "sysdeps.h"
#include "platform.h"
#include "video.h"
#include "video_output.h"
#include <sys/time.h>

// Get current time in microseconds
static uint64_t get_time_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000ULL + tv.tv_usec;
}

// Called when Mac framebuffer changes
void VideoRefresh(void) {
    if (!g_platform.video_output) {
        return;  // No buffer yet (initialization)
    }

    // Get Mac framebuffer pointer
    const monitor_desc &monitor = *VideoMonitors[0];
    const video_mode &mode = monitor.get_current_mode();
    uint32_t* mac_pixels = (uint32_t*)Mac2HostAddr(monitor.get_mac_frame_base());

    // Determine pixel format
    // Mac 32-bit mode is ARGB byte order
    PixelFormat fmt = PIXFMT_ARGB;

    // Copy to VideoOutput buffer
    g_platform.video_output->submit_frame(
        mac_pixels,
        mode.x,
        mode.y,
        fmt,
        get_time_us()
    );
}
```

**Audio driver integration** (modify existing `audio.cpp`):
```cpp
// In audio.cpp, find the callback that Mac uses to write samples
// (likely in AudioDispatch or related function)

// Example location (need to find exact spot):
void audio_write_samples_callback(int16_t* samples, int count) {
    if (!g_platform.audio_output) {
        return;  // No buffer yet
    }

    // Submit to AudioOutput buffer
    g_platform.audio_output->submit_samples(
        samples,
        count,
        AudioStatus.channels,
        AudioStatus.sample_rate
    );
}
```

---

### 5. Worker Thread Stubs

**For Phase 2**, we'll create minimal stub threads just to test the architecture:

```cpp
// Video encoder thread (stub for now)
void video_encoder_main(JsonConfig* config, VideoOutput* video_output) {
    printf("[Video Encoder] Started\n");

    while (!shutdown_requested) {
        const FrameBuffer* frame = video_output->wait_for_frame(100);
        if (frame) {
            printf("[Video Encoder] Got frame %llu: %dx%d\n",
                   frame->sequence, frame->width, frame->height);
        }
    }

    printf("[Video Encoder] Exiting\n");
}

// Audio encoder thread (stub for now)
void audio_encoder_main(JsonConfig* config, AudioOutput* audio_output) {
    printf("[Audio Encoder] Started\n");

    int16_t samples[960 * 2];  // 20ms @ 48kHz stereo

    while (!shutdown_requested) {
        int read = audio_output->read_samples(samples, 960);
        if (read > 0) {
            printf("[Audio Encoder] Got %d samples\n", read);
        }
    }

    printf("[Audio Encoder] Exiting\n");
}

// Web server thread (stub for now)
void web_server_main(JsonConfig* config) {
    printf("[Web Server] Started (stub)\n");

    while (!shutdown_requested) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    printf("[Web Server] Exiting\n");
}
```

---

## Implementation Steps

### Step 1: Create Platform Buffer Headers (1-2 hours)
- [x] Research current architecture
- [ ] Create `src/platform/video_output.h`
- [ ] Create `src/platform/audio_output.h`
- [ ] Add to meson.build

### Step 2: Implement Buffer Classes (2-3 hours)
- [ ] Implement `src/platform/video_output.cpp`
- [ ] Implement `src/platform/audio_output.cpp`
- [ ] Write unit tests (optional but recommended)

### Step 3: Timer Interrupt Integration (1-2 hours)
- [ ] Add `setup_timer_interrupt()` function
- [ ] Verify Unicorn block hook checks `PendingInterrupt`
- [ ] Test with simple ROM

### Step 4: Update main.cpp (2-3 hours)
- [ ] Add VideoOutput/AudioOutput instantiation
- [ ] Add thread launching code
- [ ] Add `setup_timer_interrupt()` call
- [ ] Add clean shutdown (join threads)
- [ ] Wire up `g_platform.video_output` / `audio_output`

### Step 5: Video Driver Integration (2-3 hours)
- [ ] Create `src/drivers/video/video_webrtc.cpp`
- [ ] Implement `VideoRefresh()` to call `video_output->submit_frame()`
- [ ] Update meson.build to build new driver
- [ ] Test with Quadra ROM

### Step 6: Audio Driver Integration (3-4 hours)
- [ ] Find audio sample write callback in `audio.cpp`
- [ ] Add `audio_output->submit_samples()` call
- [ ] Test audio data flow

### Step 7: Worker Thread Stubs (1 hour)
- [ ] Add stub thread functions
- [ ] Test thread startup/shutdown
- [ ] Verify buffer data flow

### Step 8: Testing & Validation (2-3 hours)
- [ ] Test with EMULATOR_TIMEOUT=5
- [ ] Verify timer interrupts firing (add debug prints)
- [ ] Verify video frames being submitted
- [ ] Verify audio samples being submitted
- [ ] Test clean shutdown (Ctrl+C)

---

## Testing Strategy

### Unit Tests (Optional)
```cpp
// test_video_output.cpp
void test_triple_buffer() {
    VideoOutput vo(1024, 768);
    uint32_t frame1[1024*768];
    uint32_t frame2[1024*768];

    // Fill with test patterns
    memset(frame1, 0xFF, sizeof(frame1));
    memset(frame2, 0xAA, sizeof(frame2));

    // Submit frames
    vo.submit_frame(frame1, 1024, 768, PIXFMT_BGRA, 1000);
    vo.submit_frame(frame2, 1024, 768, PIXFMT_BGRA, 2000);

    // Read back
    const FrameBuffer* f = vo.wait_for_frame(10);
    assert(f != nullptr);
    assert(f->sequence == 2);
    assert(f->pixels[0] == 0xAAAAAAAA);
}
```

### Integration Tests
```bash
# Test 1: Timer interrupts
EMULATOR_TIMEOUT=2 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom
# Should see: "Timer interrupt setup: 60 Hz"
# Should see: Periodic interrupt debug messages

# Test 2: Video buffer flow
EMULATOR_TIMEOUT=5 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom
# Should see: "[Video Encoder] Got frame 1: 640x480"
# Should see: Frame sequence incrementing

# Test 3: Audio buffer flow
EMULATOR_TIMEOUT=5 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom
# Should see: "[Audio Encoder] Got N samples"

# Test 4: DualCPU still works
EMULATOR_TIMEOUT=5 CPU_BACKEND=dualcpu ./build/macemu-next ~/quadra.rom
# Should validate without divergence
```

---

## Success Criteria

- [x] VideoOutput class compiles and links
- [ ] AudioOutput class compiles and links
- [ ] Timer interrupt fires at 60 Hz
- [ ] Video frames copied to VideoOutput buffer
- [ ] Audio samples copied to AudioOutput buffer
- [ ] Worker threads start and receive data
- [ ] Clean shutdown on Ctrl+C
- [ ] DualCPU validation still works
- [ ] No memory leaks (valgrind clean)

---

## Next Steps (After Phase 2)

Once this foundation is working:
1. **Phase 3**: Copy WebRTC encoders (H.264, Opus)
2. **Phase 4**: Implement actual video encoding thread
3. **Phase 5**: Implement actual audio encoding thread
4. **Phase 6**: Implement web server thread (HTTP + WebRTC)
5. **Phase 7**: End-to-end testing (browser → emulator)

---

## Questions & Decisions

### Q1: Where does VideoRefresh() get called from?
**A**: Need to verify - likely from timer code or explicit calls in emulator main loop.
**Action**: Grep for VideoRefresh calls, understand calling pattern.

### Q2: How does Mac audio currently work?
**A**: Through Sound Manager API in `audio.cpp` - Mac calls callbacks to write samples.
**Action**: Find exact callback location, add buffer copy.

### Q3: What resolution to use for VideoOutput buffer?
**A**: Start with 1920x1080 (max), Mac will use smaller (640x480 or 1024x768).
**Decision**: Allocate max, Mac will only use what it needs.

### Q4: What sample rate for AudioOutput?
**A**: Mac can use 11025, 22050, 44100, or 48000 Hz.
**Decision**: Allocate for 48kHz (WebRTC standard), Mac will use what it configures.

---

This plan provides everything needed to implement the main CPU thread with timer interrupts and buffer copy mechanisms. Ready to start coding!
