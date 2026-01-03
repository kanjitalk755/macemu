# Basilisk II / SheepShaver Web Streaming

Browser-based access to Basilisk II and SheepShaver running in headless mode using WebRTC.

## Architecture

```
┌─────────────────────┐         ┌─────────────────────┐         ┌──────────────┐
│     Emulator        │   IPC   │   WebRTC Server     │  HTTP/  │   Browser    │
│ (BasiliskII/        │◄───────►│                     │  WS     │              │
│  SheepShaver)       │         │ macemu-webrtc       │◄───────►│ Web Client   │
└─────────────────────┘         └─────────────────────┘         └──────────────┘
        │                               │                              │
        │ Creates:                      │ Connects to:                 │
        │ - SHM /macemu-video-{PID}     │ - Emulator SHM (read-write)  │
        │ - Socket /tmp/macemu-{PID}.sock│ - Emulator socket (input)   │
        │ - Eventfds for notifications  │                              │
        │                               │                              │
        │ Outputs:                      │ Encodes & Streams:           │
        │ - BGRA frames (triple-buf)    │ - H.264/AV1 via RTP Track    │
        │ - PCM audio (ring buffer)     │ - Opus audio via RTP Track   │
        │ - Accepts binary input        │ - PNG via DataChannel        │
        │                               │ - Input relay to emulator    │
        └───────────────────────────────┴──────────────────────────────┘
```

### Key Design Principles

- **Emulator OWNS resources**: Creates SHM, sockets, and eventfds at startup
- **Server CONNECTS**: Discovers emulators by scanning `/dev/shm/macemu-video-*`
- **Lock-free video**: Triple-buffered BGRA frames with atomic indices and eventfd signaling
- **Lock-free audio**: Frame-based ring buffer with atomic read/write indices
- **Multiple codecs**: H.264, AV1, or PNG (server-configured per connection)
- **Binary input protocol**: Efficient mouse/keyboard relay with multi-layer latency tracking
- **Zero-copy IPC**: Shared memory mapping, no data copying between processes

## Quick Start

```bash
# Install system dependencies (Ubuntu/Debian)
sudo apt install cmake pkg-config libsvtav1enc-dev libopus-dev \
                 libyuv-dev libssl-dev

# Build libdatachannel and WebRTC server
cd web-streaming
make

# Build Basilisk II with IPC video support
cd ../BasiliskII/src/Unix
./configure --enable-ipc-video --enable-ipc-audio
make

# Run the server (auto-starts emulator)
cd ../../../web-streaming
./build/macemu-webrtc

# Open http://localhost:8000 in your browser
```

## IPC Protocol (Version 4)

### Resource Naming

- **Shared Memory**: `/macemu-video-{PID}` (POSIX shared memory, ~25MB for 1080p)
- **Control Socket**: `/tmp/macemu-{PID}.sock` (Unix domain socket, SOCK_STREAM)
- **Frame Eventfd**: Passed via SCM_RIGHTS on socket connect (for epoll/select)
- **Audio Eventfd**: Passed via SCM_RIGHTS (separate from video for independent processing)

### Shared Memory Layout

```c
typedef struct {
    // Header - validated on connect
    uint32_t magic;              // 0x4D454D34 ("MEM4")
    uint32_t version;            // 4
    uint32_t pid;                // Emulator PID
    uint32_t state;              // Running/paused/stopped

    // Current frame dimensions (dynamic!)
    uint32_t width, height;      // Actual size (≤1920x1080)
    uint32_t pixel_format;       // MACEMU_PIXFMT_BGRA (B,G,R,A bytes)

    // Dirty rectangle (for PNG optimization)
    uint32_t dirty_x, dirty_y, dirty_width, dirty_height;

    // Triple buffer sync (plain fields, synchronized via eventfd)
    uint32_t write_index;        // Emulator writes here (0-2)
    uint32_t ready_index;        // Server reads here (0-2)
    uint64_t frame_count;        // Monotonic counter
    uint64_t timestamp_us;       // CLOCK_REALTIME when frame completed

    // Latency tracking (atomics for thread-safe stats updates)
    atomic_uint32 mouse_latency_avg_ms;   // x10 for 0.1ms precision
    atomic_uint32 mouse_latency_samples;

    // Ping/pong RTT measurement (write-release/read-acquire pattern)
    struct {
        uint64_t t1_browser_ms;   // performance.now()
        uint64_t t2_server_us;    // CLOCK_REALTIME
        uint64_t t3_emulator_us;  // CLOCK_REALTIME
        uint64_t t4_frame_us;     // CLOCK_REALTIME
    } ping_timestamps;
    atomic_uint32 ping_sequence;  // Acts as "ready" flag

    // Eventfd handles
    int32_t frame_ready_eventfd;  // Signaled on frame complete
    int32_t audio_ready_eventfd;  // Signaled on audio frame

    // Audio format (dynamic, like video dimensions!)
    uint32_t audio_sample_rate;   // 11025/22050/44100/48000 Hz
    uint32_t audio_channels;      // 1=mono, 2=stereo
    uint32_t audio_format;        // MACEMU_AUDIO_FORMAT_PCM_S16

    // Audio frame ring buffer (lock-free)
    atomic_uint32 audio_frame_write_idx;  // Producer index
    atomic_uint32 audio_frame_read_idx;   // Consumer index
    MacEmuAudioFrame audio_frame_ring[3]; // 3 frames @ 20ms = 60ms buffer

    // BGRA frame buffers (3 × 1920×1080×4 = ~24.9 MB)
    uint8_t frames[3][MACEMU_BGRA_FRAME_SIZE];
} MacEmuIPCBuffer;
```

### Audio Frame Structure

```c
typedef struct {
    uint32_t sample_rate;      // Mac's native rate (dynamic!)
    uint32_t channels;         // 1 or 2
    uint32_t samples;          // Actual samples in frame (variable)
    uint32_t format;           // MACEMU_AUDIO_FORMAT_PCM_S16
    uint64_t timestamp_us;     // CLOCK_REALTIME
    uint8_t data[3840];        // Max: 48kHz × 2ch × 2bytes × 20ms
} MacEmuAudioFrame;
```

### Binary Input Protocol

Sent over Unix socket from server to emulator:

| Message Type | Size | Fields |
|--------------|------|--------|
| Key | 8 bytes | type, flags, mac_keycode, modifiers |
| Mouse | 20 bytes | type, flags, x, y, buttons, timestamp_ms |
| Command | 8 bytes | type, flags, command (start/stop/reset/pause) |
| Ping | 24 bytes | type, sequence, t1, t2, t3 (accumulates timestamps) |

## Video Pipeline

### Emulator Side ([video_ipc.cpp](../BasiliskII/src/IPC/video_ipc.cpp))

1. Mac OS renders to framebuffer (1/2/4/8/16/32-bit color)
2. Convert any depth to **BGRA** (B,G,R,A bytes) using libyuv
3. Write to `frames[write_index]` in SHM
4. Call `macemu_frame_complete()`:
   - Updates metadata (timestamp, indices)
   - Swaps buffers atomically
   - Writes to eventfd (wakes server)

### Server Side ([server.cpp](server/server.cpp))

1. Connect to emulator SHM and socket by PID
2. Epoll on `frame_ready_eventfd` (no polling!)
3. Read from `frames[ready_index]`
4. Encode based on codec:
   - **H.264**: BGRA → I420 (libyuv) → H.264 (SVT-AV1) → RTP
   - **AV1**: BGRA → I420 (libyuv) → AV1 (SVT-AV1) → RTP
   - **PNG**: BGRA → RGB (libyuv) → PNG (fpng) → DataChannel (with delta/dirty rect)
5. Send to all connected WebRTC peers

### Browser Side ([client.js](client/client.js))

- **H.264/AV1**: WebRTC video track → `<video>` element (hardware decode)
- **PNG**: DataChannel binary → `createImageBitmap()` → canvas
- **Audio**: WebRTC audio track → `<audio>` element (Opus decode)

## Audio Pipeline

### Emulator Side ([audio_ipc.cpp](../BasiliskII/src/IPC/audio_ipc.cpp))

1. Mac audio system produces PCM S16 samples
2. Convert endianness (Mac S16MSB → S16LE)
3. Buffer into 20ms frames (variable sample count)
4. Write to ring buffer:
   - Populate `audio_frame_ring[write_idx]`
   - Increment `audio_frame_write_idx` atomically
5. Signal `audio_ready_eventfd` (optional, for debugging)

### Server Side ([opus_encoder.cpp](server/opus_encoder.cpp))

1. Read from ring buffer:
   - Check if frames available (compare read/write indices)
   - Read `audio_frame_ring[read_idx]`
   - Increment `audio_frame_read_idx` atomically
2. Resample if needed (Mac rate → 48kHz for Opus)
3. Encode to Opus (20ms frames, 96kbps stereo)
4. Send to all connected peers via RTP audio track

### Audio Configuration ([audio_config.h](server/audio_config.h))

All audio parameters centralized for easy tuning:

```c
#define AUDIO_SAMPLE_RATE       48000  // WebRTC/Opus standard
#define AUDIO_CHANNELS          2      // Stereo
#define AUDIO_FRAME_DURATION_MS 20     // Low latency
#define OPUS_BITRATE            96000  // 96kbps stereo
#define OPUS_INBAND_FEC         1      // Packet loss recovery
```

## Latency Measurement

### Mouse Input Latency (Browser → Emulator)

Multi-layer timestamp tracking:

1. **Browser** (`t1`): `performance.now()` when mouse event captured
2. **Server** (`t2`): `CLOCK_REALTIME` when message received from browser
3. **Emulator** (`t3`): `CLOCK_REALTIME` when input processed

Emulator syncs clocks on first message (epoch offset), calculates latency, writes stats to SHM.

### Ping/Pong RTT (Round-Trip Time)

Four-timestamp measurement echoed in video frame metadata:

1. **Browser** (`t1`): Sends ping with `performance.now()`
2. **Server** (`t2`): Adds `CLOCK_REALTIME` on receive
3. **Emulator** (`t3`): Adds `CLOCK_REALTIME` on processing
4. **Emulator** (`t4`): Adds `CLOCK_REALTIME` on frame ready
5. **Server**: Reads ping data from SHM, sends in PNG metadata or logs for H.264
6. **Browser**: Calculates full RTT and per-layer latencies

Stats displayed in browser UI stats panel.

## Codec Selection

Configure in prefs file (`basilisk_ii.prefs`):

```
# Video codec: h264, av1, or png
webcodec h264
```

| Codec | Transport | Best For | Notes |
|-------|-----------|----------|-------|
| H.264 | WebRTC RTP Track | General use, low bandwidth | Hardware decode, ~30KB IDR |
| AV1 | WebRTC RTP Track | Best compression | Newer codec, better quality |
| PNG | WebRTC DataChannel | Pixel-perfect, lossless | High bandwidth, delta compression |

## Directory Structure

```
web-streaming/
├── server/
│   ├── server.cpp              # Main server (HTTP, WebRTC, IPC client)
│   ├── ipc/
│   │   ├── ipc_connection.cpp  # IPC connection manager
│   │   └── ipc_connection.h
│   ├── webrtc/
│   │   ├── webrtc_server.h     # WebRTC peer management
│   │   └── peer_connection.h
│   ├── http/
│   │   ├── http_server.cpp     # HTTP server
│   │   ├── api_handlers.cpp    # REST API endpoints
│   │   └── static_files.cpp    # Serve web client
│   ├── codec.h                 # Video codec abstraction
│   ├── h264_encoder.cpp/h      # H.264 encoding (SVT-AV1)
│   ├── av1_encoder.cpp/h       # AV1 encoding (SVT-AV1)
│   ├── png_encoder.cpp/h       # PNG encoding (fpng)
│   ├── opus_encoder.cpp/h      # Opus audio encoding
│   ├── audio_config.h          # Centralized audio settings
│   ├── utils/
│   │   ├── keyboard_map.cpp    # Browser → Mac keycode conversion
│   │   └── json_utils.cpp      # JSON helpers
│   ├── config/
│   │   └── server_config.cpp   # Configuration management
│   ├── storage/
│   │   ├── file_scanner.cpp    # ROM/disk image discovery
│   │   └── prefs_manager.cpp   # Prefs file management
│   └── emulator/
│       └── process_manager.cpp # Emulator lifecycle management
├── client/
│   ├── index.html              # Web UI
│   ├── client.js               # WebRTC client, video decoders
│   └── styles.css              # UI styles
├── build/                      # Build output
├── libdatachannel/             # WebRTC library (git submodule)
├── storage/
│   ├── roms/                   # Mac ROM files
│   └── images/                 # Disk images
├── basilisk_ii.prefs           # Emulator config
├── Makefile
└── README.md

BasiliskII/src/IPC/
├── ipc_protocol.h              # Shared IPC protocol definitions
├── video_ipc.cpp               # IPC video driver for emulator
├── audio_ipc.cpp               # IPC audio driver for emulator
└── audio_config.h              # Audio settings (shared with server)
```

## Build Targets

| Target | Description |
|--------|-------------|
| `all` | Build everything (default) |
| `libdatachannel` | Build WebRTC library only |
| `clean` | Remove build files |
| `distclean` | Remove all build files including libdatachannel |
| `deps` | Install system dependencies (Ubuntu/Debian) |
| `deps-check` | Verify dependencies are installed |
| `run` | Build and run the server |

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Web client |
| `/api/status` | GET | Emulator status, latency stats, peer count |
| `/api/storage` | GET | Available ROMs and disk images |
| `/api/prefs` | GET/POST | Read/write emulator preferences |
| `/api/emulator/start` | POST | Start emulator |
| `/api/emulator/stop` | POST | Stop emulator |
| `/api/emulator/restart` | POST | Restart emulator |
| `/api/log` | POST | Browser log relay (for debugging) |

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
  --stun SERVER           STUN server (e.g., stun:stun.l.google.com:19302)
```

## Dependencies

### System Packages (apt)

- **libsvtav1enc-dev** - SVT-AV1 encoder (H.264 and AV1)
- **libopus-dev** - Opus audio encoder
- **libyuv-dev** - Color space conversion
- **libssl-dev** - TLS/crypto for DTLS
- **cmake** - Build system for libdatachannel
- **pkg-config** - Build configuration

### Bundled

- **libdatachannel** - WebRTC library (git submodule)
- **fpng** - Fast PNG encoder (included source)
- **nlohmann/json** - JSON library (header-only, included)

## Ports

| Port | Protocol | Purpose |
|------|----------|---------|
| 8000 | HTTP | Web client |
| 8090 | WebSocket | WebRTC signaling |
| Dynamic | UDP | WebRTC media (ICE negotiated) |

## Troubleshooting

### No video displayed

1. Check emulator is running: `ls /dev/shm/macemu-video-*`
2. Check socket exists: `ls /tmp/macemu-*.sock`
3. Check server logs for "Connected to video SHM"
4. Ensure prefs has: `screen ipc/WIDTH/HEIGHT`
5. Check browser console (F12) for errors

### No audio

1. Check emulator built with: `--enable-ipc-audio`
2. Verify audio format in server logs
3. Check browser audio element is unmuted
4. Look for "audio_frame_ring" activity in debug logs

### High latency

1. Check stats panel for mouse/video/RTT latency
2. Use H.264 codec for lower latency than PNG
3. Reduce resolution if needed (640x480 vs 1024x768)
4. Check network conditions (try local network first)

### Colors wrong / artifacts

1. BGRA pixel format should be used (not ARGB)
2. Check dirty rectangle is being computed correctly
3. For PNG: ensure RGB conversion uses correct libyuv function
4. For H.264: verify I420 conversion

### Build errors

```bash
# Check all dependencies
make deps-check

# Install missing dependencies
make deps

# Clean and rebuild
make distclean
make
```

### Emulator won't start

1. Check emulator path: `-e /path/to/BasiliskII`
2. Verify prefs file exists: `-P /path/to/prefs`
3. Ensure ROM file is available in `storage/roms/`
4. Check emulator was built with `--enable-ipc-video`

## Performance

Typical performance at 640x480, H.264:

| Metric | Value |
|--------|-------|
| Frame rate | 30 fps |
| IDR frame size | 30-40 KB |
| P frame size | 1-5 KB |
| Video encode time | <5ms |
| Audio encode time | <1ms |
| End-to-end latency | 50-100ms (local network) |

## Debug Environment Variables

- `MACEMU_DEBUG_CONNECTION` - IPC connection lifecycle
- `MACEMU_DEBUG_PERF` - Frame timing and performance stats
- `MACEMU_DEBUG_FRAMES` - Per-frame logging (verbose!)
- `MACEMU_DEBUG_AUDIO` - Audio pipeline tracing
- `MACEMU_DEBUG_PNG` - PNG encoding details
- `MACEMU_DEBUG_MOUSE` - Mouse input logs (absolute/relative coordinates)

## See Also

- [WEBRTC_STREAMING.md](../docs/WEBRTC_STREAMING.md) - Architecture overview
- [libdatachannel](https://github.com/paullouisageneau/libdatachannel)
- [SVT-AV1](https://gitlab.com/AOMediaCodec/SVT-AV1)
- [Opus](https://opus-codec.org/)
- [libyuv](https://chromium.googlesource.com/libyuv/libyuv/)
