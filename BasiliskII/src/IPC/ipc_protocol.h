/*
 * IPC Protocol for macemu WebRTC Streaming - Version 4
 *
 * Architecture:
 * - Emulator OWNS resources (creates SHM and Unix socket)
 * - Server CONNECTS to emulator resources by PID
 * - Emulator outputs 32-bit pixels in one of two formats (see pixel_format field)
 * - Server converts to I420 for H.264, or to RGB for PNG, based on codec
 *
 * SHM naming: /macemu-video-{PID}
 * Socket naming: /tmp/macemu-{PID}.sock
 *
 * Triple buffering with atomics - no locks, no polling needed.
 *
 * Pixel formats (all 32-bit, 4 bytes per pixel):
 * - MACEMU_PIXFMT_ARGB: Mac native 32-bit, bytes A,R,G,B (use libyuv BGRA functions)
 * - MACEMU_PIXFMT_BGRA: Converted from indexed/16-bit, bytes B,G,R,A (use libyuv ARGB functions)
 */

#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifdef __cplusplus
#include <atomic>
#define ATOMIC_UINT32 std::atomic<uint32_t>
#define ATOMIC_UINT64 std::atomic<uint64_t>
#define ATOMIC_LOAD(ptr) (ptr).load(std::memory_order_acquire)
#define ATOMIC_STORE(ptr, val) (ptr).store(val, std::memory_order_release)
#define ATOMIC_FETCH_ADD(ptr, val) (ptr).fetch_add(val, std::memory_order_acq_rel)
#else
#include <stdatomic.h>
#define ATOMIC_UINT32 _Atomic uint32_t
#define ATOMIC_UINT64 _Atomic uint64_t
#define ATOMIC_LOAD(ptr) atomic_load_explicit(&(ptr), memory_order_acquire)
#define ATOMIC_STORE(ptr, val) atomic_store_explicit(&(ptr), val, memory_order_release)
#define ATOMIC_FETCH_ADD(ptr, val) atomic_fetch_add_explicit(&(ptr), val, memory_order_acq_rel)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Resource naming - emulator creates, server connects
 * Format: /macemu-video-{PID} and /tmp/macemu-{PID}.sock
 */
#define MACEMU_VIDEO_SHM_PREFIX "/macemu-video-"
#define MACEMU_CONTROL_SOCK_PREFIX "/tmp/macemu-"
#define MACEMU_CONTROL_SOCK_SUFFIX ".sock"

// Magic number and version
#define MACEMU_VIDEO_MAGIC 0x4D454D34  // "MEM4" (version 4 - BGRA)
#define MACEMU_IPC_VERSION 4

// Maximum supported resolution (1080p)
#define MACEMU_MAX_WIDTH  1920
#define MACEMU_MAX_HEIGHT 1080

// BGRA frame size for max resolution: 4 bytes per pixel
#define MACEMU_BGRA_FRAME_SIZE (MACEMU_MAX_WIDTH * MACEMU_MAX_HEIGHT * 4)

// Number of frame buffers for triple buffering
#define MACEMU_NUM_BUFFERS 3

// Total SHM size: header + 3 BGRA frames (~24.9 MB)
#define MACEMU_VIDEO_SHM_SIZE (sizeof(MacEmuIPCBuffer))

// Emulator state flags
#define MACEMU_STATE_STOPPED   0
#define MACEMU_STATE_RUNNING   1
#define MACEMU_STATE_PAUSED    2

// Pixel format flags (set per-frame by emulator)
// These describe the memory byte order of 32-bit pixels
#define MACEMU_PIXFMT_ARGB     0   // Mac native 32-bit: bytes A,R,G,B (libyuv "BGRA")
#define MACEMU_PIXFMT_BGRA     1   // Converted: bytes B,G,R,A (libyuv "ARGB")

// Audio format flags (set per-frame by emulator, like pixel_format for video)
#define MACEMU_AUDIO_FORMAT_NONE      0   // Audio disabled/muted
#define MACEMU_AUDIO_FORMAT_PCM_S16   1   // 16-bit signed PCM (Mac native)

// Maximum audio parameters (allocate buffers for worst case)
// Fixed-size buffers like video - handles all Mac audio formats without reallocation
#define MACEMU_AUDIO_MAX_SAMPLE_RATE  48000   // Max: 48kHz (WebRTC standard)
#define MACEMU_AUDIO_MAX_CHANNELS     2       // Max: Stereo

// Frame duration (fixed at 20ms for low-latency Opus streaming)
#define MACEMU_AUDIO_FRAME_MS         20

// Maximum audio frame size calculation
// Worst case: 48kHz, stereo, 16-bit PCM, 20ms
// = (48000 samples/sec × 0.020 sec) × 2 channels × 2 bytes = 3840 bytes
#define MACEMU_AUDIO_MAX_SAMPLES_PER_FRAME \
    ((MACEMU_AUDIO_MAX_SAMPLE_RATE * MACEMU_AUDIO_FRAME_MS) / 1000)
#define MACEMU_AUDIO_MAX_FRAME_SIZE \
    (MACEMU_AUDIO_MAX_SAMPLES_PER_FRAME * MACEMU_AUDIO_MAX_CHANNELS * 2)

// Number of audio buffers (double buffering sufficient - audio changes less than video)
#define MACEMU_AUDIO_NUM_BUFFERS      2

/*
 * 32-bit Frame Layout (within each frame buffer):
 *
 * For a frame of actual size (width x height):
 * - 4 bytes per pixel (format indicated by pixel_format field)
 * - Stride: MACEMU_MAX_WIDTH * 4 (fixed for all resolutions)
 * - Actual data: width * height * 4 bytes
 *
 * Server should check pixel_format and use appropriate libyuv function:
 * - MACEMU_PIXFMT_ARGB: BGRAToI420(), BGRAToRAW() (Mac native naming is confusing!)
 * - MACEMU_PIXFMT_BGRA: ARGBToI420(), ARGBToRAW()
 */

/*
 * Shared Video Buffer - Fixed size, emulator-owned
 *
 * Protocol:
 * 1. Emulator creates SHM at startup, initializes header
 * 2. Emulator converts Mac framebuffer to BGRA, writes to frames[write_index]
 * 3. On frame complete, emulator calls macemu_frame_complete()
 * 4. Server connects, maps SHM, reads from frames[ready_index]
 * 5. Server converts BGRA to I420 (H.264) or RGB (PNG) based on codec
 */
/*
 * Audio frame structure - individual frame with metadata
 * Used in frame-based ring buffer for lock-free audio transfer
 */
typedef struct {
    // Metadata (written atomically with frame data)
    uint32_t sample_rate;      // Mac's native rate (11025/22050/44100/48000)
    uint32_t channels;         // 1 (mono) or 2 (stereo)
    uint32_t samples;          // Actual samples in this frame (variable!)
    uint32_t format;           // MACEMU_AUDIO_FORMAT_* (PCM_S16)
    uint64_t timestamp_us;     // CLOCK_REALTIME when frame completed
    uint32_t _padding[2];      // Align to 32 bytes

    // Audio data (max 48kHz * 2ch * 2bytes * 20ms = 3840 bytes)
    // Mac provides S16MSB (big-endian), server byte-swaps to S16LE for Opus
    uint8_t data[MACEMU_AUDIO_MAX_FRAME_SIZE];
} MacEmuAudioFrame;

// Ring buffer size (number of frames, not bytes)
// Small SHM buffer - just for producer/consumer handoff
// Server maintains its own jitter buffer for smooth playback
// 3 frames @ 20ms/frame = 60ms (enough for IPC latency variation)
#define MACEMU_AUDIO_FRAME_RING_SIZE 3

/*
 * MacEmuIPCBuffer - Shared memory for video, audio, and metadata
 *
 * This structure contains:
 * - Video frames (BGRA, up to 1920x1080, triple-buffered)
 * - Audio frames (frame-based ring buffer, variable sample rate)
 * - Mouse input latency stats
 * - Ping/pong RTT measurements
 * - Frame metadata (dimensions, pixel format, dirty rects, timestamps)
 *
 * Synchronization:
 * - Video: frame_ready_eventfd signals new video frames
 * - Audio: Lock-free frame ring buffer with atomic indices
 * - Separate eventfds allow independent video/audio processing
 */
typedef struct {
    // Header - validated by server on connect
    uint32_t magic;              // Must be MACEMU_VIDEO_MAGIC
    uint32_t version;            // Protocol version (4)
    uint32_t pid;                // Emulator PID (for validation)
    uint32_t state;              // MACEMU_STATE_* (running/paused/stopped)

    // Current frame dimensions (actual, not max)
    uint32_t width;              // Actual frame width (≤ MACEMU_MAX_WIDTH)
    uint32_t height;             // Actual frame height (≤ MACEMU_MAX_HEIGHT)
    uint32_t pixel_format;       // MACEMU_PIXFMT_* (set per frame by emulator)
    uint32_t _reserved;          // Future use, alignment

    // Dirty rectangle for PNG optimization (computed by emulator)
    // Plain fields - synchronized by eventfd write/read
    uint32_t dirty_x;            // X coordinate of dirty rect (0 for full frame)
    uint32_t dirty_y;            // Y coordinate of dirty rect
    uint32_t dirty_width;        // Width of dirty rect (0 = no changes, same as width = full frame)
    uint32_t dirty_height;       // Height of dirty rect

    // Cursor position (for rendering cursor overlay in browser)
    // Updated each frame, read by server for sending to browser
    uint16_t cursor_x;           // Current cursor X position (0 to width-1)
    uint16_t cursor_y;           // Current cursor Y position (0 to height-1)
    uint8_t cursor_visible;      // 1 if cursor should be shown, 0 if hidden
    uint8_t _cursor_reserved[3]; // Padding for alignment

    // Triple buffer synchronization
    // Plain fields - synchronized by eventfd (kernel provides memory barriers)
    uint32_t write_index;        // Buffer emulator is writing to (0-2)
    uint32_t ready_index;        // Buffer ready for server to read (0-2)
    uint64_t frame_count;        // Total frames completed (monotonic, for stats only)
    uint64_t timestamp_us;       // Timestamp of last completed frame (microseconds)

    // Latency stats (written by emulator, read by server)
    // Updated every stats interval (~3 seconds)
    ATOMIC_UINT32 mouse_latency_avg_ms;  // Average mouse input latency in ms (x10 for 0.1ms precision)
    ATOMIC_UINT32 mouse_latency_samples; // Number of samples in current average

    // Ping/pong for RTT measurement (written by emulator, echoed by server in frame metadata)
    // OPTIMIZATION: Only ping_sequence needs to be atomic - it acts as the "ready" flag
    // All timestamps are written BEFORE setting ping_sequence (write-release semantics)
    // Server reads ping_sequence atomically (read-acquire), which guarantees visibility of timestamps
    struct {
        uint64_t t1_browser_ms;   // Browser send time (performance.now())
        uint64_t t2_server_us;    // Server receive time (CLOCK_REALTIME microseconds)
        uint64_t t3_emulator_us;  // Emulator receive time (CLOCK_REALTIME microseconds)
        uint64_t t4_frame_us;     // Frame ready time (CLOCK_REALTIME microseconds)
    } ping_timestamps;            // Regular struct - no atomics needed

    ATOMIC_UINT32 ping_sequence;  // Sequence number - atomic write-release / read-acquire
                                  // When server sees non-zero, all timestamp writes are visible

    // Event notification (Linux eventfd for low-latency signaling)
    // Emulator writes to this fd after frame completion
    // Server uses epoll to wait for new frames instead of polling
    int32_t frame_ready_eventfd;         // eventfd for frame ready notification (-1 if not supported)

    // Audio format (set per-frame like video width/height!)
    // These fields are updated atomically with audio_ready_index
    // Server must handle dynamic format changes (sample rate, channels)
    uint32_t audio_sample_rate;      // DYNAMIC: 11025/22050/44100/48000 Hz (Mac supports all)
    uint32_t audio_channels;         // DYNAMIC: 1=mono, 2=stereo
    uint32_t audio_format;           // DYNAMIC: MACEMU_AUDIO_FORMAT_* (PCM_S16/NONE)
    uint32_t audio_samples_in_frame; // Actual samples in current frame (≤ MAX)

    // Audio synchronization (double buffering)
    uint32_t audio_write_index;      // Buffer emulator is writing to (0-1)
    uint32_t audio_ready_index;      // Buffer ready for server to read (0-1)
    uint64_t audio_frame_count;      // Total audio frames completed
    uint64_t audio_timestamp_us;     // Timestamp of last audio frame

    int32_t audio_ready_eventfd;     // Separate eventfd for audio frames (-1 if not supported)

    // Frame-based audio ring buffer (NEW ARCHITECTURE)
    // Mac produces frames at native sample rate (44.1k, 48k, etc) every ~20ms
    // Server consumes frames at 20ms intervals, resamples to 48kHz for Opus
    // Ring buffer absorbs timing jitter and clock drift

    // Frame ring buffer indices (atomic for lock-free operation)
    ATOMIC_UINT32 audio_frame_write_idx;  // 0 to (MACEMU_AUDIO_FRAME_RING_SIZE-1)
    ATOMIC_UINT32 audio_frame_read_idx;   // 0 to (MACEMU_AUDIO_FRAME_RING_SIZE-1)

    // Ring buffer of audio frames (see MacEmuAudioFrame typedef above)
    MacEmuAudioFrame audio_frame_ring[MACEMU_AUDIO_FRAME_RING_SIZE];

    // Pre-zeroed silence frame (used during underruns, not in ring buffer)
    MacEmuAudioFrame audio_silence_frame;

    // Audio capture trigger (set by server when user presses 'C')
    // 0 = no capture, 1 = trigger capture on all layers
    ATOMIC_UINT32 capture_trigger;

    // BGRA frame buffers - fixed size for max resolution
    // Each frame: width * height * 4 bytes (B, G, R, A per pixel)
    uint8_t frames[MACEMU_NUM_BUFFERS][MACEMU_BGRA_FRAME_SIZE];
} MacEmuIPCBuffer;

/*
 * Binary Input Protocol - sent over Unix socket from server to emulator
 *
 * Replaces JSON with fixed-size binary messages for efficiency.
 * Server converts browser keycodes to Mac keycodes before sending.
 */

// Input message types
#define MACEMU_INPUT_KEY       1   // Keyboard event
#define MACEMU_INPUT_MOUSE     2   // Mouse move/button
#define MACEMU_INPUT_COMMAND   3   // Emulator command (start/stop/reset)
#define MACEMU_INPUT_PING      4   // Latency measurement ping (echoed in frame metadata)
#define MACEMU_INPUT_AUDIO_REQUEST  5   // Server requests audio data (pull model)
#define MACEMU_INPUT_MOUSE_MODE     6   // Mouse mode change (absolute/relative)

// Key event flags
#define MACEMU_KEY_DOWN        0x01
#define MACEMU_KEY_UP          0x00

// Mouse button flags
#define MACEMU_MOUSE_LEFT      0x01
#define MACEMU_MOUSE_RIGHT     0x02
#define MACEMU_MOUSE_MIDDLE    0x04

// Mouse mode flags (in MacEmuMouseInput.hdr.flags)
#define MACEMU_MOUSE_ABSOLUTE  0x10  // If set, x/y are absolute coordinates; else relative deltas

// Command types
#define MACEMU_CMD_START       1
#define MACEMU_CMD_STOP        2
#define MACEMU_CMD_RESET       3
#define MACEMU_CMD_PAUSE       4
#define MACEMU_CMD_RESUME      5

// Input message header (4 bytes)
typedef struct {
    uint8_t type;                // MACEMU_INPUT_*
    uint8_t flags;               // Type-specific flags
    uint16_t _reserved;          // Alignment/future use
} MacEmuInputHeader;

// Keyboard input (8 bytes total)
typedef struct {
    MacEmuInputHeader hdr;       // type = MACEMU_INPUT_KEY
    uint8_t mac_keycode;         // Mac keycode (converted by server)
    uint8_t modifiers;           // Modifier state (shift, ctrl, alt, cmd)
    uint16_t _reserved;
} MacEmuKeyInput;

// Mouse input (20 bytes total)
typedef struct {
    MacEmuInputHeader hdr;       // type = MACEMU_INPUT_MOUSE
    int16_t x;                   // X delta (relative movement)
    int16_t y;                   // Y delta (relative movement)
    uint8_t buttons;             // Button state (MACEMU_MOUSE_*)
    uint8_t _reserved[3];
    uint64_t timestamp_ms;       // Browser timestamp (performance.now()) for latency measurement
} MacEmuMouseInput;

// Command input (8 bytes total)
typedef struct {
    MacEmuInputHeader hdr;       // type = MACEMU_INPUT_COMMAND
    uint8_t command;             // MACEMU_CMD_*
    uint8_t _reserved[3];
} MacEmuCommandInput;

// Ping input for RTT measurement with timestamps at each layer
// Timestamps accumulate as ping travels: browser -> server -> emulator
// Each layer adds its own timestamp using its local clock
typedef struct {
    MacEmuInputHeader hdr;           // type = MACEMU_INPUT_PING
    uint32_t sequence;               // Ping sequence number
    uint64_t t1_browser_send_ms;     // Browser send time (performance.now())
    uint64_t t2_server_recv_us;      // Server receive time (CLOCK_REALTIME microseconds)
    uint64_t t3_emulator_recv_us;    // Emulator receive time (CLOCK_REALTIME microseconds)
} MacEmuPingInput;

// Audio request input (server pull model - 8 bytes total)
// Server sends this when Opus encoder needs more audio data
typedef struct {
    MacEmuInputHeader hdr;       // type = MACEMU_INPUT_AUDIO_REQUEST
    uint32_t requested_samples;  // Number of samples requested (usually 960 for 20ms @ 48kHz)
} MacEmuAudioRequestInput;

// Mouse mode change input (8 bytes total)
// Browser sends this to switch between absolute and relative mouse modes
typedef struct {
    MacEmuInputHeader hdr;       // type = MACEMU_INPUT_MOUSE_MODE
    uint8_t mode;                // 0 = absolute, 1 = relative
    uint8_t _reserved[3];
} MacEmuMouseModeInput;

// Union for receiving any input type
typedef union {
    MacEmuInputHeader hdr;
    MacEmuKeyInput key;
    MacEmuMouseInput mouse;
    MacEmuCommandInput cmd;
    MacEmuPingInput ping;
    MacEmuAudioRequestInput audio_req;
    MacEmuMouseModeInput mouse_mode;
} MacEmuInput;

/*
 * Helper functions
 */

// Get pointer to a specific frame buffer
static inline uint8_t* macemu_get_frame_ptr(MacEmuIPCBuffer* buf, uint32_t index) {
    return buf->frames[index];
}

// Get BGRA stride (fixed for max resolution)
static inline int macemu_get_bgra_stride(void) {
    return MACEMU_MAX_WIDTH * 4;
}

// Get pointer to the frame currently being written by emulator
static inline uint8_t* macemu_get_write_frame(MacEmuIPCBuffer* buf) {
    return macemu_get_frame_ptr(buf, buf->write_index);
}

// Get pointer to the most recently completed frame (for server to read)
static inline uint8_t* macemu_get_ready_frame(MacEmuIPCBuffer* buf) {
    return macemu_get_frame_ptr(buf, buf->ready_index);
}

// Get BGRA frame pointer for the ready frame (server use)
static inline uint8_t* macemu_get_ready_bgra(MacEmuIPCBuffer* buf) {
    return macemu_get_frame_ptr(buf, buf->ready_index);
}

// Called by emulator after frame is complete - publishes frame via eventfd
static inline void macemu_frame_complete(MacEmuIPCBuffer* buf, uint64_t timestamp_us) {
    uint32_t current = buf->write_index;
    uint32_t next = (current + 1) % MACEMU_NUM_BUFFERS;

    // Write all metadata (plain writes - eventfd provides memory barrier)
    buf->timestamp_us = timestamp_us;
    buf->ready_index = current;
    buf->write_index = next;
    buf->frame_count++;

    // NOTE: Ping t4 handling moved to video_ipc.cpp's update_ping_on_frame_complete()
    // This keeps the inline function simple and allows emulator to track echo state

    // Signal eventfd to wake up server (kernel write() provides memory barrier)
    // All writes above are guaranteed visible after server's read(eventfd)
    if (buf->frame_ready_eventfd >= 0) {
        uint64_t val = 1;
        (void)write(buf->frame_ready_eventfd, &val, sizeof(val));
    }
}

// Initialize video buffer (called by emulator)
static inline void macemu_init_ipc_buffer(MacEmuIPCBuffer* buf, uint32_t pid,
                                            uint32_t width, uint32_t height) {
    buf->magic = MACEMU_VIDEO_MAGIC;
    buf->version = MACEMU_IPC_VERSION;
    buf->pid = pid;
    buf->state = MACEMU_STATE_STOPPED;
    buf->width = width;
    buf->height = height;
    buf->pixel_format = MACEMU_PIXFMT_BGRA;  // Default, emulator sets per-frame

    // Plain initialization - synchronized by eventfd
    buf->write_index = 0;
    buf->ready_index = 0;
    buf->dirty_x = 0;
    buf->dirty_y = 0;
    buf->dirty_width = width;   // Full frame initially
    buf->dirty_height = height;
    buf->frame_count = 0;
    buf->timestamp_us = 0;

    // Stats (can still use atomics for thread-safe updates from stats thread if needed)
    ATOMIC_STORE(buf->mouse_latency_avg_ms, 0);
    ATOMIC_STORE(buf->mouse_latency_samples, 0);

    // Initialize ping tracking (timestamp struct doesn't need atomics, just zero it)
    buf->ping_timestamps.t1_browser_ms = 0;
    buf->ping_timestamps.t2_server_us = 0;
    buf->ping_timestamps.t3_emulator_us = 0;
    buf->ping_timestamps.t4_frame_us = 0;
    ATOMIC_STORE(buf->ping_sequence, 0);  // Only seq number is atomic

    // Create eventfd for frame ready notification (REQUIRED - no polling fallback)
    // EFD_NONBLOCK: reads won't block if no data, EFD_SEMAPHORE: read returns 1 per event
    buf->frame_ready_eventfd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
    if (buf->frame_ready_eventfd < 0) {
        fprintf(stderr, "IPC: FATAL: Failed to create eventfd: %s\n", strerror(errno));
        // Caller must check for -1 and handle error
    }

    // Initialize audio subsystem (frame-based architecture)
    buf->audio_sample_rate = 0;
    buf->audio_channels = 0;
    buf->audio_format = MACEMU_AUDIO_FORMAT_NONE;
    buf->audio_samples_in_frame = 0;
    buf->audio_write_index = 0;
    buf->audio_ready_index = 0;
    buf->audio_frame_count = 0;
    buf->audio_timestamp_us = 0;

    // Create separate eventfd for audio (not used with frame ring buffer, but kept for compatibility)
    buf->audio_ready_eventfd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
    if (buf->audio_ready_eventfd < 0) {
        fprintf(stderr, "IPC: WARNING: Failed to create audio eventfd: %s (audio disabled)\n", strerror(errno));
    }

    // Initialize frame-based ring buffer
    ATOMIC_STORE(buf->audio_frame_write_idx, 0);
    ATOMIC_STORE(buf->audio_frame_read_idx, 0);

    // Zero all frames in ring buffer
    memset(buf->audio_frame_ring, 0, sizeof(buf->audio_frame_ring));

    // Initialize silence frame (pre-zeroed, all samples = 0)
    memset(&buf->audio_silence_frame, 0, sizeof(buf->audio_silence_frame));
    buf->audio_silence_frame.sample_rate = 48000;
    buf->audio_silence_frame.channels = 2;
    buf->audio_silence_frame.samples = 960;  // 20ms @ 48kHz
    buf->audio_silence_frame.format = MACEMU_AUDIO_FORMAT_PCM_S16;
}

// Validate video buffer (called by server on connect)
static inline int macemu_validate_ipc_buffer(const MacEmuIPCBuffer* buf, uint32_t expected_pid) {
    if (buf->magic != MACEMU_VIDEO_MAGIC) return -1;
    if (buf->version != MACEMU_IPC_VERSION) return -2;
    if (buf->pid != expected_pid) return -3;
    if (buf->width > MACEMU_MAX_WIDTH || buf->height > MACEMU_MAX_HEIGHT) return -4;
    return 0;
}

// Calculate actual BGRA frame size for current resolution
static inline size_t macemu_actual_bgra_size(uint32_t width, uint32_t height) {
    return (size_t)width * height * 4;
}

// Audio helper functions (frame-based ring buffer)
// Note: Frames are accessed directly via atomic indices, no helpers needed

#ifdef __cplusplus
}
#endif

#endif // IPC_PROTOCOL_H
