# WebRTC Streaming for Basilisk II / SheepShaver

This document describes the WebRTC streaming feature that allows running Basilisk II and SheepShaver in headless mode and accessing them through a web browser.

## Overview

Basilisk II and SheepShaver support browser-based access via WebRTC streaming. The implementation uses a **split architecture** with separate emulator and server processes communicating via IPC (Inter-Process Communication).

### Key Technologies

- **Standalone WebRTC Server** - Separate process (`macemu-webrtc`) that connects to the emulator via IPC
- **H.264/AV1 Encoding** - SVT-AV1 encoder for efficient video compression
- **Opus Audio** - Low-latency audio encoding (20ms frames, 96kbps stereo)
- **libdatachannel** - Lightweight C++ WebRTC library for signaling and media transport
- **libyuv** - Fast color space conversion (BGRA to I420)
- **WebRTC DataChannel** - Low-latency bidirectional input (mouse/keyboard)

## Architecture

The system uses a split architecture with IPC between the emulator and streaming server:

```
┌─────────────────────────────────────────────────────────────────────┐
│                     Basilisk II / SheepShaver                        │
│  ┌──────────────────┐     ┌─────────────────────────────────────┐  │
│  │ Mac Framebuffer  │────►│ video_ipc.cpp                       │  │
│  │ (1/2/4/8/16/32)  │     │ - BGRA conversion (libyuv)          │  │
│  │                  │     │ - Triple-buffered SHM               │  │
│  └──────────────────┘     │ - Eventfd signaling                 │  │
│                           └─────────────────────────────────────┘  │
│  ┌──────────────────┐     ┌─────────────────────────────────────┐  │
│  │ Mac Audio        │────►│ audio_ipc.cpp                       │  │
│  │ (PCM S16)        │     │ - Endian conversion                 │  │
│  │                  │     │ - Frame-based ring buffer (20ms)    │  │
│  └──────────────────┘     └─────────────────────────────────────┘  │
│                           ┌─────────────────────────────────────┐  │
│                           │ Control Socket                       │  │
│                           │ - Binary input protocol             │  │
│                           │ - Latency tracking                  │  │
│                           └─────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
              │                              │                     │
              │ /macemu-video-{PID}          │ /tmp/macemu-{PID}.sock
              │ (Shared Memory ~25MB)        │ (Unix Socket)       │
              │                              │                     │
              │ frame_ready_eventfd          │ audio_ready_eventfd │
              │ (passed via SCM_RIGHTS)      │ (passed via SCM_RIGHTS)
              v                              v                     v
┌─────────────────────────────────────────────────────────────────────┐
│                WebRTC Server (macemu-webrtc)                         │
│  ┌──────────────────┐     ┌─────────────────────────────────────┐  │
│  │ IPC Connection   │────►│ Video Encoder                       │  │
│  │ - Epoll on       │     │ - BGRA → I420 (H.264/AV1)          │  │
│  │   eventfds       │     │ - BGRA → RGB (PNG)                 │  │
│  │ - Read from SHM  │     │ - Dirty rect optimization          │  │
│  └──────────────────┘     └─────────────────────────────────────┘  │
│  ┌──────────────────┐     ┌─────────────────────────────────────┐  │
│  │ Audio Ring Read  │────►│ Opus Encoder                        │  │
│  │ - Lock-free      │     │ - Resample to 48kHz                │  │
│  │ - 20ms frames    │     │ - Encode to Opus                   │  │
│  └──────────────────┘     └─────────────────────────────────────┘  │
│                           ┌─────────────────────────────────────┐  │
│                           │ WebRTC (libdatachannel)             │  │
│                           │ - RTP packetization                 │  │
│                           │ - DTLS/SRTP encryption              │  │
│                           │ - ICE connectivity                  │  │
│                           │ - DataChannel for input             │  │
│                           └─────────────────────────────────────┘  │
│                           ┌─────────────────────────────────────┐  │
│                           │ HTTP Server (port 8000)             │  │
│                           │ Signaling Server (port 8090)        │  │
│                           │ REST API (storage, prefs)           │  │
│                           └─────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
                                   │
                          WebRTC + DataChannel
                                   v
┌─────────────────────────────────────────────────────────────────────┐
│                         Web Browser                                  │
│  ┌────────────────────┐     ┌───────────────────────────────────┐  │
│  │ client.js          │────►│ Media Elements                    │  │
│  │ - WebSocket sig    │     │ - <video> H.264/AV1 decode       │  │
│  │ - RTCPeerConnection│     │ - <audio> Opus decode            │  │
│  │ - Input capture    │     │ - <canvas> PNG rendering         │  │
│  │ - Stats panel      │     │ - Hardware acceleration          │  │
│  └────────────────────┘     └───────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

## IPC Protocol (Version 4)

### Resource Ownership Model

**Emulator creates and owns:**
- Shared memory segment at `/macemu-video-{PID}`
- Unix socket at `/tmp/macemu-{PID}.sock`
- Eventfds for frame/audio notifications (passed to server via SCM_RIGHTS)

**Server connects as client:**
- Opens SHM with read-write access (needs to update audio ring read index)
- Connects to socket for bidirectional communication
- Receives eventfds for efficient epoll-based waiting

### Shared Memory Layout

```c
typedef struct {
    // === Header (validated by server on connect) ===
    uint32_t magic;              // 0x4D454D34 ("MEM4")
    uint32_t version;            // Protocol version 4
    uint32_t pid;                // Emulator PID
    uint32_t state;              // MACEMU_STATE_* (running/paused/stopped)

    // === Video Frame Metadata ===
    uint32_t width, height;      // Current resolution (≤1920x1080)
    uint32_t pixel_format;       // MACEMU_PIXFMT_BGRA (B,G,R,A bytes)

    // Dirty rectangle for PNG optimization (computed by emulator)
    uint32_t dirty_x, dirty_y;
    uint32_t dirty_width, dirty_height;

    // === Triple Buffer Synchronization (lock-free) ===
    // Plain fields - synchronized by eventfd write/read
    // Kernel provides memory barriers
    uint32_t write_index;        // Buffer emulator writes to (0-2)
    uint32_t ready_index;        // Buffer ready for server (0-2)
    uint64_t frame_count;        // Total frames completed (monotonic)
    uint64_t timestamp_us;       // CLOCK_REALTIME of last frame

    // === Latency Tracking ===
    // Atomics allow thread-safe updates from stats thread
    atomic_uint32 mouse_latency_avg_ms;   // x10 for 0.1ms precision
    atomic_uint32 mouse_latency_samples;

    // Ping/pong RTT measurement (optimized with write-release pattern)
    // Only ping_sequence is atomic - acts as "ready" flag
    // All timestamps written BEFORE setting sequence (write-release)
    // Server reads sequence atomically (read-acquire), guarantees timestamp visibility
    struct {
        uint64_t t1_browser_ms;   // Browser send (performance.now())
        uint64_t t2_server_us;    // Server receive (CLOCK_REALTIME)
        uint64_t t3_emulator_us;  // Emulator receive (CLOCK_REALTIME)
        uint64_t t4_frame_us;     // Frame ready (CLOCK_REALTIME)
    } ping_timestamps;
    atomic_uint32 ping_sequence;  // Sequence number (atomic flag)

    // === Eventfd Handles ===
    int32_t frame_ready_eventfd;  // Video frame notification
    int32_t audio_ready_eventfd;  // Audio frame notification (optional)

    // === Audio Format (dynamic, like video dimensions) ===
    uint32_t audio_sample_rate;   // 11025/22050/44100/48000 Hz
    uint32_t audio_channels;      // 1=mono, 2=stereo
    uint32_t audio_format;        // MACEMU_AUDIO_FORMAT_PCM_S16
    uint32_t audio_samples_in_frame;  // Actual samples in current frame

    // === Audio Frame Ring Buffer (lock-free) ===
    atomic_uint32 audio_frame_write_idx;  // Producer index (0-2)
    atomic_uint32 audio_frame_read_idx;   // Consumer index (0-2)
    MacEmuAudioFrame audio_frame_ring[3]; // 3 frames @ 20ms = 60ms buffer

    // === Video Frame Buffers ===
    // 3 × 1920×1080×4 bytes = ~24.9 MB
    // Fixed size for max resolution, stride = MACEMU_MAX_WIDTH * 4
    uint8_t frames[3][MACEMU_BGRA_FRAME_SIZE];
} MacEmuIPCBuffer;
```

### Audio Frame Structure

```c
typedef struct {
    uint32_t sample_rate;      // Mac's native rate (DYNAMIC!)
    uint32_t channels;         // 1 or 2
    uint32_t samples;          // Actual samples in this frame (variable)
    uint32_t format;           // MACEMU_AUDIO_FORMAT_PCM_S16
    uint64_t timestamp_us;     // CLOCK_REALTIME when frame completed
    uint8_t data[3840];        // Max: 48kHz × 2ch × 2bytes × 20ms
} MacEmuAudioFrame;
```

### Control Socket Protocol

Binary messages sent from server to emulator:

```c
// Keyboard input (8 bytes)
typedef struct {
    uint8_t type;          // MACEMU_INPUT_KEY
    uint8_t flags;         // MACEMU_KEY_DOWN or MACEMU_KEY_UP
    uint16_t _reserved;
    uint8_t mac_keycode;   // ADB keycode (converted by server)
    uint8_t modifiers;     // Shift, Ctrl, Alt, Cmd
    uint16_t _reserved;
} MacEmuKeyInput;

// Mouse input (20 bytes)
typedef struct {
    uint8_t type;          // MACEMU_INPUT_MOUSE
    uint8_t flags;
    uint16_t _reserved;
    int16_t x, y;          // Relative delta
    uint8_t buttons;       // MACEMU_MOUSE_LEFT/RIGHT/MIDDLE
    uint8_t _reserved[3];
    uint64_t timestamp_ms; // Browser performance.now() for latency tracking
} MacEmuMouseInput;

// Ping input (24 bytes total)
typedef struct {
    uint8_t type;              // MACEMU_INPUT_PING
    uint8_t flags;
    uint16_t _reserved;
    uint32_t sequence;         // Ping sequence number
    uint64_t t1_browser_ms;    // Browser send time
    uint64_t t2_server_us;     // Server receive time
    uint64_t t3_emulator_us;   // Emulator receive time (filled by emulator)
} MacEmuPingInput;
```

## Video Pipeline

### Emulator Side (`BasiliskII/src/IPC/video_ipc.cpp`)

1. **Mac framebuffer rendering**
   - Mac OS renders to framebuffer (1/2/4/8/16/32-bit color depth)
   - Palette-based modes use lookup table

2. **BGRA conversion** (using libyuv)
   - All depths converted to 32-bit BGRA (B,G,R,A byte order)
   - For 32-bit Mac ARGB: Use `BGRAToI420` (confusing but correct!)
   - For converted modes: Already BGRA

3. **Triple-buffered write**
   - Write BGRA data to `frames[write_index]` in SHM
   - Compute dirty rectangle (for PNG optimization)
   - Update metadata (dimensions, pixel format, timestamp)

4. **Frame complete signaling**
   - Call `macemu_frame_complete(buffer, timestamp_us)`
   - Swaps ready/write indices (plain writes, no atomics needed)
   - Writes to `frame_ready_eventfd` (kernel provides memory barrier)
   - Server wakes up immediately via epoll

### Server Side (`web-streaming/server/server.cpp`)

1. **Eventfd-based waiting**
   - Epoll on `frame_ready_eventfd` (no polling loop!)
   - Wakes instantly when new frame available

2. **Read from SHM**
   - Read `frames[ready_index]` (triple buffering prevents tearing)
   - Check dirty rectangle for PNG optimization

3. **Codec-specific encoding**
   - **H.264**: BGRA → I420 (libyuv) → H.264 (SVT-AV1) → RTP packets
   - **AV1**: BGRA → I420 (libyuv) → AV1 (SVT-AV1) → RTP packets
   - **PNG**: BGRA → RGB (libyuv) → PNG (fpng) → DataChannel binary
     - Delta mode: Only send dirty rectangle
     - Prepend metadata (timestamp, dimensions, offset)

4. **Distribute to peers**
   - Send to all connected WebRTC peers
   - Per-peer codec selection
   - Request keyframe on new peer connection

### Browser Side (`web-streaming/client/client.js`)

- **H.264/AV1**:
  - Received via WebRTC video track
  - Rendered to `<video>` element
  - Hardware-accelerated decode (GPU)

- **PNG**:
  - Received via DataChannel (binary)
  - Parsed metadata (timestamp, dimensions, offset)
  - `createImageBitmap()` for fast decode
  - Rendered to `<canvas>`
  - Delta blitting for partial updates

## Audio Pipeline

### Emulator Side (`BasiliskII/src/IPC/audio_ipc.cpp`)

1. **Mac audio production**
   - Mac audio system produces PCM S16 samples
   - Native sample rate (11025/22050/44100/48000 Hz)
   - Big-endian (S16MSB)

2. **Endian conversion**
   - Convert S16MSB → S16LE (for Opus encoder)
   - Byte-swap each 16-bit sample

3. **Frame buffering**
   - Accumulate samples into 20ms frames
   - Variable sample count (depends on Mac sample rate)
   - Example: 44100 Hz → 882 samples per 20ms frame

4. **Ring buffer write**
   - Populate `audio_frame_ring[write_idx]` with:
     - Metadata (sample_rate, channels, samples, format, timestamp)
     - Audio data (S16LE PCM)
   - Atomically increment `audio_frame_write_idx` (lock-free)

### Server Side (`web-streaming/server/opus_encoder.cpp`)

1. **Ring buffer read**
   - Compare `audio_frame_read_idx` vs `audio_frame_write_idx`
   - If frames available: read `audio_frame_ring[read_idx]`
   - Atomically increment `audio_frame_read_idx` (lock-free)

2. **Resampling**
   - If Mac rate ≠ 48kHz: resample to 48kHz (Opus requirement)
   - Use linear interpolation or libspeexdsp

3. **Opus encoding**
   - Encode 20ms frames (960 samples @ 48kHz)
   - 96kbps stereo, VBR mode
   - FEC enabled for packet loss recovery

4. **RTP distribution**
   - Send to all peers via WebRTC audio track
   - RTP payload type 97 (Opus)

### Browser Side

- Opus packets received via WebRTC audio track
- Rendered to `<audio>` element
- Browser handles jitter buffer and decode

## Color Space Conversion

### Pixel Format Details

| Mac Format | Memory Layout | libyuv Function | Notes |
|------------|---------------|-----------------|-------|
| 32-bit ARGB | A,R,G,B bytes (big-endian) | `BGRAToI420` | Mac native 32-bit |
| 16-bit RGB555 | Manual swap → BGRA | `ARGBToI420` | Byte-swapped |
| 8/4/2/1-bit | Palette → BGRA | `ARGBToI420` | Indexed color |

**Important**: Mac 32-bit "ARGB" is actually BGRA in memory due to big-endian word storage. Always use `BGRAToI420` for Mac native 32-bit mode.

### I420 Format (for H.264/AV1)

- **Y plane**: Luma (brightness), full resolution
- **U plane**: Chroma blue, half resolution (subsampled 4:2:0)
- **V plane**: Chroma red, half resolution (subsampled 4:2:0)

Efficient for video compression and hardware-friendly.

## Encoder Configuration

### H.264/AV1 Settings (SVT-AV1)

| Setting | Value | Purpose |
|---------|-------|---------|
| Target Usage | Real-time | Low latency encoding |
| Rate Control | CQP | Constant quality |
| QP | 28-32 | Balance quality/size |
| Keyframe Interval | 60 frames (~2s) | Recovery from packet loss |
| Preset | 8 (faster) | Speed over quality |

### PNG Settings (fpng)

- Fast PNG encoder optimized for speed
- Delta mode: Only encode changed pixels
- Dirty rectangle tracking reduces data size

### Opus Settings

| Setting | Value |
|---------|-------|
| Sample Rate | 48000 Hz |
| Channels | 2 (stereo) |
| Frame Duration | 20ms |
| Bitrate | 96000 bps |
| Complexity | 5 (moderate) |
| VBR | Enabled |
| FEC | Enabled |
| Signal Type | Music |

## Latency Measurement

### Mouse Input Latency

Four-stage measurement from browser to emulator:

1. **Browser** (`t1`): Capture mouse event, record `performance.now()`
2. **Server** (`t2`): Receive from DataChannel, record `CLOCK_REALTIME`
3. **Emulator** (`t3`): Process input, record `CLOCK_REALTIME`

Clock synchronization:
- First message establishes epoch offset (browser epoch vs CLOCK_REALTIME)
- Subsequent messages use offset to calculate cross-process latency
- Stats updated every ~3 seconds, exposed via `/api/status`

### Ping/Pong RTT

Multi-layer round-trip measurement:

1. Browser sends ping with sequence number and `t1`
2. Server adds `t2`, forwards to emulator
3. Emulator adds `t3` immediately
4. Emulator adds `t4` when frame ready (includes rendering latency)
5. Server reads ping data from SHM, sends to browser (PNG metadata or logs)
6. Browser calculates:
   - Total RTT: `now() - t1`
   - Browser→Server: `t2 - t1` (adjusted for epoch)
   - Server→Emulator: `t3 - t2`
   - Emulator rendering: `t4 - t3`
   - Server→Browser: `now() - t4` (adjusted)

## Quick Start

### 1. Install Dependencies

```bash
# Ubuntu/Debian
sudo apt install cmake pkg-config libsvtav1enc-dev libopus-dev \
                 libyuv-dev libssl-dev

# Initialize git submodules
git submodule update --init --recursive
```

### 2. Build

```bash
# Build WebRTC server
cd web-streaming
make

# Build Basilisk II with IPC drivers
cd ../BasiliskII/src/Unix
./configure --enable-ipc-video --enable-ipc-audio
make
```

### 3. Configure

Edit your prefs file (e.g., `basilisk_ii.prefs`):

```
# Use IPC video driver
screen ipc/640/480

# Optional: Set codec (h264, av1, png)
webcodec h264
```

### 4. Run

```bash
# Auto-start mode (server launches emulator)
cd web-streaming
./build/macemu-webrtc

# Manual mode (run emulator separately)
./BasiliskII --config myprefs &
./build/macemu-webrtc --no-auto-start

# Open http://localhost:8000 in browser
```

## Command Line Options

```
Usage: macemu-webrtc [options]

Options:
  -h, --help              Show help
  -p, --http-port PORT    HTTP server port (default: 8000)
  -s, --signaling PORT    WebSocket signaling port (default: 8090)
  -e, --emulator PATH     Path to BasiliskII/SheepShaver executable
  -P, --prefs FILE        Emulator prefs file (default: basilisk_ii.prefs)
  -n, --no-auto-start     Don't auto-start emulator
  --pid PID               Connect to specific emulator PID
  --roms PATH             ROMs directory (default: storage/roms)
  --images PATH           Disk images directory (default: storage/images)
  --codec CODEC           Default codec (h264, av1, png)
  --stun SERVER           STUN server for NAT traversal
```

## Ports Used

| Port | Protocol | Purpose |
|------|----------|---------|
| 8000 | HTTP | Web client files and REST API |
| 8090 | WebSocket | WebRTC signaling (SDP/ICE exchange) |
| Dynamic | UDP | WebRTC media (negotiated via ICE) |

## Troubleshooting

### Black screen / No video

1. Verify emulator running: `ls /dev/shm/macemu-video-*`
2. Check socket exists: `ls /tmp/macemu-*.sock`
3. Verify server connected: Look for "Connected to video SHM" in logs
4. Check prefs: Must have `screen ipc/WIDTH/HEIGHT`
5. Browser console (F12): Check for WebRTC errors

### No audio / Silent

1. Emulator built with `--enable-ipc-audio`?
2. Check server logs for audio format messages
3. Browser audio element unmuted?
4. Try: `MACEMU_DEBUG_AUDIO=1 ./build/macemu-webrtc`
5. Check ring buffer activity in logs

### High latency

1. Check stats panel in browser UI
2. Try H.264 codec (lower latency than PNG)
3. Reduce resolution (640x480 vs 1024x768)
4. Test on local network first (eliminate network issues)
5. Check CPU usage (encoding bottleneck?)

### Wrong colors / Artifacts

1. Verify BGRA pixel format (not ARGB)
2. Check conversion function matches pixel format
3. For PNG: Verify RGB conversion
4. For H.264: Verify I420 conversion with correct stride

### Connection fails

1. Ports 8000, 8090 not blocked by firewall?
2. WebSocket connection in browser console?
3. For remote access: May need TURN server (not just STUN)
4. NAT issues: Try `--stun stun:stun.l.google.com:19302`

### Build errors

```bash
# Check dependencies
make deps-check

# Install missing
make deps

# Clean rebuild
make distclean
make
```

## Performance Benchmarks

Typical performance on modern hardware (640x480):

| Metric | Value |
|--------|-------|
| Frame rate | 30 fps |
| H.264 IDR frame | 30-40 KB |
| H.264 P frame | 1-5 KB |
| PNG full frame | 50-100 KB |
| PNG delta frame | 1-10 KB |
| Video encode time | <5ms |
| Audio encode time | <1ms |
| End-to-end latency | 50-100ms (LAN) |
| Mouse input latency | 10-30ms |
| Audio latency | 60-80ms (jitter buffer) |

## Directory Structure

```
web-streaming/
├── server/                 # WebRTC server C++ code
├── client/                 # Web client (HTML/JS/CSS)
├── libdatachannel/         # WebRTC library (submodule)
├── storage/                # ROMs and disk images
├── build/                  # Build output
└── Makefile

BasiliskII/src/IPC/
├── ipc_protocol.h          # Shared protocol definitions
├── video_ipc.cpp           # Video driver
├── audio_ipc.cpp           # Audio driver
└── audio_config.h          # Audio configuration
```

## Debug Environment Variables

- `MACEMU_DEBUG_CONNECTION` - IPC connection lifecycle and validation
- `MACEMU_DEBUG_PERF` - Frame timing and performance statistics
- `MACEMU_DEBUG_FRAMES` - Per-frame logging (very verbose!)
- `MACEMU_DEBUG_AUDIO` - Audio pipeline tracing
- `MACEMU_DEBUG_PNG` - PNG encoding details

## See Also

- [README.md](../web-streaming/README.md) - Quick start guide
- [libdatachannel](https://github.com/paullouisageneau/libdatachannel) - WebRTC library
- [SVT-AV1](https://gitlab.com/AOMediaCodec/SVT-AV1) - Video encoder
- [Opus](https://opus-codec.org/) - Audio codec
- [libyuv](https://chromium.googlesource.com/libyuv/libyuv/) - Color conversion
