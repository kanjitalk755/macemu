# macemu WebRTC Streaming Setup Guide

Complete guide for building and running BasiliskII/SheepShaver with WebRTC streaming support.

## Overview

This project enables browser-based streaming of classic Mac emulation using WebRTC. The architecture consists of:

- **BasiliskII/SheepShaver** - Mac emulators with IPC video/audio output
- **WebRTC Server** - C++ server that encodes video/audio and streams via WebRTC
- **Web Client** - Browser-based UI with multiple codec support

## System Requirements

- Ubuntu 20.04+ or Debian 11+ (or compatible Linux distribution)
- GCC/G++ with C++17 support
- Autotools (autoconf, automake, libtool)
- CMake 3.10+
- Modern web browser with WebRTC support

## Dependencies

### 1. Install All Required Packages

```bash
sudo apt-get update && sudo apt-get install -y \
    build-essential cmake pkg-config git autoconf automake libtool autogen \
    libssl-dev \
    libopenh264-dev \
    libsvtav1-dev libsvtav1enc-dev \
    libvpx-dev \
    libwebp-dev \
    libopus-dev \
    libyuv-dev \
    libmpfr-dev \
    libsdl2-dev \
    libgtk-3-dev
```

### Codec Dependencies Breakdown

| Codec | Library | Package | Use Case |
|-------|---------|---------|----------|
| **H.264** | OpenH264 | `libopenh264-dev` | WebRTC video track, wide compatibility |
| **AV1** | SVT-AV1 | `libsvtav1-dev libsvtav1enc-dev` | Best for 1-bit dithered Mac content |
| **VP9** | libvpx | `libvpx-dev` | Great for UI/screen content |
| **PNG** | fpng (bundled) | *(none)* | DataChannel, good for dithered, dirty rects |
| **WebP** | libwebp | `libwebp-dev` | DataChannel, 2-3x faster than PNG |
| **Opus** | libopus | `libopus-dev` | Audio codec (48kHz stereo) |

**Shared libraries:**
- `libyuv-dev` - Fast YUV/RGB color space conversion (used by all video codecs)
- `libssl-dev` - TLS/crypto for WebRTC connections
- `libmpfr-dev` - Multi-precision floating-point (BasiliskII/SheepShaver dependency)
- `libsdl2-dev`, `libgtk-3-dev` - Emulator UI dependencies

## Build Instructions

### Step 1: Build BasiliskII with IPC Support

```bash
cd BasiliskII/src/Unix

# Generate configure script
./autogen.sh

# Configure with IPC video and audio enabled
./configure --enable-ipc-video --enable-ipc-audio

# Build
make -j$(nproc)
```

The compiled binary will be at: `BasiliskII/src/Unix/BasiliskII`

### Step 2: Build SheepShaver with IPC Support

```bash
cd ../../../SheepShaver/src/Unix

# Generate configure script
./autogen.sh

# Configure with IPC video and audio enabled
./configure --enable-ipc-video --enable-ipc-audio

# Build
make -j$(nproc)
```

The compiled binary will be at: `SheepShaver/src/Unix/SheepShaver`

### Step 3: Build WebRTC Streaming Server

```bash
cd ../../../web-streaming

# Generate configure script
./autogen.sh

# Configure (will check all codec dependencies)
./configure

# Build server and libdatachannel
make -j$(nproc)
```

The compiled server will be at: `web-streaming/build/macemu-webrtc`

### Step 4: Create Directory Structure

```bash
# From the macemu root directory
cd web-streaming

# Create bin directory and symlink emulators
mkdir -p bin
ln -sf ../../BasiliskII/src/Unix/BasiliskII bin/BasiliskII
ln -sf ../../SheepShaver/src/Unix/SheepShaver bin/SheepShaver

# Create storage directories
mkdir -p storage/roms
mkdir -p storage/images

# Verify structure
ls -la bin/
ls -la storage/
```

Expected structure:
```
web-streaming/
├── bin/
│   ├── BasiliskII -> ../../BasiliskII/src/Unix/BasiliskII
│   └── SheepShaver -> ../../SheepShaver/src/Unix/SheepShaver
├── storage/
│   ├── roms/      (place ROM files here)
│   └── images/    (place disk images here)
└── build/
    └── macemu-webrtc
```

### Step 5: Add ROM Files and Disk Images

```bash
# Copy ROM files to storage/roms/
cp /path/to/your/roms/*.ROM storage/roms/

# Copy disk images to storage/images/
cp /path/to/your/disks/*.dsk storage/images/
```

**Required files:**
- **BasiliskII**: Mac ROM file (e.g., `Quadra-650.ROM`, `Performa.ROM`)
- **SheepShaver**: Mac OS ROM (e.g., `Mac OS ROM`, `newworld86.rom`)
- **Disk images**: `.dsk`, `.img`, or `.hfv` files with Mac OS installed

## Running the Server

### Start the WebRTC Server

```bash
cd web-streaming
./build/macemu-webrtc
```

The server will:
1. Load codec configuration from `macemu-config.json`
2. Start HTTP server on port 8000
3. Start WebSocket signaling on port 8080
4. Auto-start the emulator (if configured)

### Access the Web Interface

Open your browser to: **http://localhost:8000**

The web UI provides:
- Live video/audio streaming
- Codec selection (H.264, AV1, VP9, PNG, WebP)
- Mouse mode (relative/absolute)
- Emulator controls (start/stop/reset)
- Configuration management

## Configuration

### macemu-config.json

The server reads configuration from `macemu-config.json`:

```json
{
  "version": 1,
  "web": {
    "emulator": "m68k",     // "m68k" (BasiliskII) or "ppc" (SheepShaver)
    "codec": "webp",        // "h264", "av1", "vp9", "png", or "webp"
    "mousemode": "relative" // "relative" or "absolute"
  },
  "common": {
    "ram": 256,
    "screen": "1024x768",
    "sound": true,
    "extfs": ""
  },
  "m68k": {
    "rom": "Quadra-650.ROM",
    "modelid": 14,
    "cpu": 4,
    "fpu": true,
    "disks": ["System7.5.dsk"],
    "cdroms": []
  },
  "ppc": {
    "rom": "newworld86.rom",
    "modelid": 14,
    "cpu": 4,
    "disks": ["MacOS9.dsk"],
    "cdroms": []
  }
}
```

### Codec Selection Guide

| Codec | Latency | Quality | CPU Usage | Best For |
|-------|---------|---------|-----------|----------|
| **WebP** | Lowest | Lossless | Low | **Recommended** - Fast, low latency, dirty rects |
| **PNG** | Low | Lossless | Medium | Alternative to WebP, dirty rects |
| **VP9** | Medium | Excellent | High | UI content, text-heavy screens |
| **AV1** | High | Best | Highest | 1-bit dithered Mac content |
| **H.264** | Low | Good | Medium | Wide compatibility, hardware decode |

**Note:** PNG and WebP use **unreliable DataChannel** for lowest latency (no retransmissions).

### Debug Flags

Enable debug logging with environment variables:

```bash
# Connection/WebRTC/ICE debug
MACEMU_DEBUG_CONNECTION=1 ./build/macemu-webrtc

# Video mode/resolution changes
MACEMU_DEBUG_MODE_SWITCH=1 ./build/macemu-webrtc

# Performance/ping/latency stats
MACEMU_DEBUG_PERF=1 ./build/macemu-webrtc

# Frame dumps to disk
MACEMU_DEBUG_FRAMES=1 ./build/macemu-webrtc

# Audio processing
MACEMU_DEBUG_AUDIO=1 ./build/macemu-webrtc

# PNG/WebP encoding and dirty rects
MACEMU_DEBUG_PNG=1 ./build/macemu-webrtc

# Mouse input (absolute/relative)
MACEMU_DEBUG_MOUSE=1 ./build/macemu-webrtc

# Combine multiple flags
MACEMU_DEBUG_CONNECTION=1 MACEMU_DEBUG_PERF=1 ./build/macemu-webrtc
```

## Architecture Details

### IPC Communication

The emulators communicate with the server via:
- **Shared memory** (`/dev/shm/macemu-video-{PID}`) - Video frames (BGRA format)
- **Unix socket** (`/tmp/macemu-{PID}.sock`) - Control commands and audio

### Video Pipeline

```
Emulator (BGRA) → Shared Memory → Server (encoding) → WebRTC → Browser (decoding)
                                     ↓
                          Codec Selection:
                          - H.264/AV1/VP9: WebRTC video track
                          - PNG/WebP: DataChannel with dirty rects
```

### Audio Pipeline

```
Emulator (s16le 48kHz) → Unix Socket → Server (Opus encoding) → WebRTC → Browser
```

### Dirty Rect Optimization

PNG and WebP codecs support dirty rectangle updates:
- Only changed screen regions are encoded/transmitted
- Reduces bandwidth and encoding time
- Heartbeat mechanism ensures ping responses even when idle
- First frame is always full screen

## Troubleshooting

### Server Won't Start

```bash
# Check if ports are in use
sudo lsof -i :8000
sudo lsof -i :8080

# Check codec libraries
pkg-config --modversion openh264
pkg-config --modversion SvtAv1Enc
pkg-config --modversion vpx
pkg-config --modversion opus
pkg-config --modversion libwebp
```

### Emulator Won't Connect

```bash
# Check if emulator is running
ps aux | grep -E 'BasiliskII|SheepShaver'

# Check IPC resources
ls -la /dev/shm/macemu-*
ls -la /tmp/macemu-*.sock

# Check emulator logs
MACEMU_DEBUG_CONNECTION=1 ./bin/BasiliskII
```

### Video Not Displaying

```bash
# Enable debug logging
MACEMU_DEBUG_MODE_SWITCH=1 MACEMU_DEBUG_FRAMES=1 ./build/macemu-webrtc

# Check browser console for errors
# Open DevTools (F12) → Console tab

# Verify codec selection
curl http://localhost:8000/api/config | jq .webcodec
```

### Poor Performance

1. **Try WebP codec** - Fastest encoding for still-image codecs
2. **Use H.264** - Hardware decode in browser, low latency
3. **Check CPU usage** - AV1 and VP9 are CPU-intensive
4. **Reduce resolution** - Edit `screen` in config to `800x600` or `640x480`

### Audio Issues

```bash
# Enable audio debug
MACEMU_DEBUG_AUDIO=1 ./build/macemu-webrtc

# Check audio in emulator prefs
cat ~/.config/BasiliskII/prefs | grep audio
cat ~/.config/SheepShaver/prefs | grep audio

# Verify Opus is working
pkg-config --modversion opus
```

## Performance Tuning

### Network Optimization

For **local network** use (lowest latency):
- Use WebP or PNG codec (unreliable DataChannel)
- No STUN server needed
- Direct peer-to-peer connection

For **remote/Internet** use:
- Enable STUN: `--enable-stun` or `--stun-server stun:stun.l.google.com:19302`
- Use H.264 codec (best for bandwidth)
- Consider VP9 for better compression

### CPU Optimization

```bash
# Reduce encoding quality for AV1 (faster)
# Edit av1_encoder.cpp, set cfg.enc_mode = 8 (fastest)

# Use hardware acceleration if available
# H.264 may use GPU decode in browser
```

## Development

### Rebuild After Code Changes

```bash
# Rebuild emulator
cd BasiliskII/src/Unix && make -j$(nproc)

# Rebuild server
cd ../../../web-streaming && make -j$(nproc)

# Restart server
killall macemu-webrtc
./build/macemu-webrtc
```

### Adding New Codecs

The architecture is designed for easy codec extension:

1. Add new `CodecType` enum value in `server/codec.h`
2. Create encoder class implementing `VideoCodec` interface
3. Add encoder to `video_loop()` in `server/server.cpp`
4. Update codec selection in UI and API handlers
5. Add library dependency to `configure.ac` and `Makefile`

See `webp_encoder.cpp` as a reference implementation.

## License

See individual component licenses:
- BasiliskII/SheepShaver: GPL v2
- libdatachannel: MPL 2.0
- fpng: Public Domain/Unlicense

## Support

For issues and questions:
- macemu: https://github.com/cebix/macemu
- libdatachannel: https://github.com/paullouisageneau/libdatachannel

---

🤖 *This guide was generated with assistance from [Claude Code](https://claude.com/claude-code)*
