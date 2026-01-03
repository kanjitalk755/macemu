# MacEmu Web-Streaming Architecture Documentation

**Complete System Architecture: Threads, Processes, and IPC Communication Patterns**

Version 4 - Generated 2025-12-28

---

## Table of Contents

1. [System Overview](#system-overview)
2. [Process Architecture](#process-architecture)
3. [Thread Architecture](#thread-architecture)
4. [Shared Memory Structure](#shared-memory-structure)
5. [IPC Mechanisms](#ipc-mechanisms)
6. [Communication Flow](#communication-flow)
7. [Synchronization Mechanisms](#synchronization-mechanisms)

---

## System Overview

The macemu web-streaming system consists of three main processes communicating via shared memory, Unix sockets, and eventfd:

```
┌────────────┐         ┌──────────────┐         ┌─────────────┐
│  Browser   │◄───────►│ WebRTC Server│◄───────►│  Emulator   │
│ (Client)   │  WebRTC │   (server)   │   IPC   │(BasiliskII) │
└────────────┘         └──────────────┘         └─────────────┘
```

**Architecture Version**: v4 (Emulator-owned resources)
- Emulator creates and owns all IPC resources
- Server connects to emulator by PID
- Triple-buffered shared memory for zero-copy video
- Frame-based ring buffer for audio

---

## Process Architecture

### 1. Browser Process (JavaScript)

**Executable**: Web Browser (Chrome, Firefox, etc.)
**Purpose**: User interface and WebRTC client
**Communication**:
- WebSocket connection to server (signaling) on port 8090
- WebRTC data channels (input events, pings)
- WebRTC media tracks (H.264/AV1/PNG video, Opus audio)

**Key Responsibilities**:
- Capture user input (mouse, keyboard)
- Convert browser keycodes and send to server
- Decode and display video stream
- Play audio stream
- Measure and display latency metrics

---

### 2. WebRTC Server Process

**Executable**: `/home/mick/macemu/web-streaming/server/server`
**Source**: `/home/mick/macemu/web-streaming/server/server.cpp`
**Purpose**: WebRTC streaming server and emulator interface
**PID**: Dynamic (started manually or auto-started)

**Command Line Options**:
```
--http-port PORT          HTTP server port (default: 8080)
--signaling-port PORT     WebRTC signaling port (default: 8090)
--target-pid PID          Connect to specific emulator PID
--auto-start              Auto-start emulator if not running
--codec h264|av1|png      Video codec selection
--enable-stun             Enable STUN for NAT traversal
```

**Key Responsibilities**:
- Connect to emulator IPC resources by PID
- Read video frames from shared memory
- Encode video (H.264/AV1/PNG)
- Encode audio (Opus)
- Handle WebRTC peer connections
- Route input events to emulator
- Measure and report latency metrics

---

### 3. Emulator Process (BasiliskII)

**Executable**: `/home/mick/macemu/BasiliskII/BasiliskII`
**Source**: `/home/mick/macemu/BasiliskII/src/Unix/main_unix.cpp`
**Purpose**: Mac 68k emulator with IPC video/audio output
**PID**: Dynamic

**IPC Resources Created** (owned by emulator):
- Shared memory: `/macemu-video-{PID}` (~25 MB)
- Unix socket: `/tmp/macemu-{PID}.sock`
- Video eventfd: For frame ready signaling
- Audio eventfd: For audio frame signaling

**Key Responsibilities**:
- Emulate Mac 68k CPU and hardware
- Render Mac framebuffer
- Convert framebuffer to BGRA format
- Write frames to shared memory
- Process input events from server
- Generate audio samples
- Signal frame/audio readiness via eventfd

---

## Thread Architecture

### Browser (JavaScript)

Browser has its own internal thread architecture (not documented here). Key components:
- Main thread (UI, JavaScript execution)
- WebRTC media threads (decode, render)
- Worker threads (if used)

---

### Server Process Threads

Located in: `/home/mick/macemu/web-streaming/server/server.cpp`

#### Thread 1: Main Thread (Video Loop)

**Function**: `video_loop()` (line 1729)
**Purpose**: Video frame processing and encoding
**Created by**: `main()` - runs in main thread
**Runs until**: `g_running` becomes false

**What it does**:
1. Wait for frame ready event via `epoll_wait()` on eventfd
2. Read frame from shared memory (triple-buffered)
3. Encode frame (H.264/AV1/PNG)
4. Send to WebRTC peers
5. Track latency metrics
6. Scan for emulators if disconnected
7. Monitor emulator health

**Communicates with**:
- Emulator: Reads shared memory, receives eventfd signals
- WebRTC peers: Sends encoded frames
- Audio thread: Shares WebRTC server state

**Synchronization**:
- `epoll()` on `frame_ready_eventfd` (blocking wait)
- Atomic reads from shared memory (synchronized by eventfd)
- Mutex protection for WebRTC peer map

**Key Variables**:
```cpp
int epoll_fd              // epoll instance for eventfd
int current_eventfd       // Currently monitored eventfd
uint64_t frames_encoded   // Frame counter
```

---

#### Thread 2: Audio Thread

**Function**: `audio_loop()` → `audio_loop_mac_ipc()` (line 2155)
**Purpose**: Audio processing and encoding
**Created by**: `main()` via `std::thread audio_thread` (line 2507)
**Runs until**: `g_running` becomes false

**What it does**:
1. Wait 20ms intervals (Opus frame duration)
2. Check audio ring buffer for frames
3. Read audio frame from shared memory
4. Resample to 48kHz if needed
5. Encode to Opus (20ms frames)
6. Send to WebRTC peers
7. Handle underruns with silence padding

**Communicates with**:
- Emulator: Reads audio frames from ring buffer
- WebRTC peers: Sends Opus audio packets
- Video thread: Shares WebRTC server state

**Synchronization**:
- Lock-free ring buffer with atomic indices
- `std::this_thread::sleep_for()` for timing
- Mutex protection for WebRTC peer map

**Key Variables**:
```cpp
uint64_t frames_consumed     // Audio frames consumed
uint64_t frames_underrun     // Underrun counter
uint32_t audio_frame_read_idx   // Ring buffer read index
```

---

#### Thread 3: HTTP Server Thread

**Function**: `http::Server::run()` (line 183)
**Purpose**: HTTP API server
**Created by**: `HTTPServer::start()` via `std::thread` (line 166)
**File**: `/home/mick/macemu/web-streaming/server/http/http_server.cpp`

**What it does**:
1. Poll for HTTP connections on port 8080
2. Accept client connections
3. Parse HTTP requests
4. Route to API handlers
5. Serve static files
6. Return JSON responses

**API Endpoints**:
- `GET /api/status` - Server and emulator status
- `POST /api/restart` - Restart emulator
- `POST /api/codec` - Change codec (h264/av1/png)
- `GET /api/disks` - List available disk images
- `GET /api/roms` - List available ROMs
- `GET /api/prefs` - Get preferences file
- `POST /api/prefs` - Update preferences file

**Communicates with**:
- HTTP clients: TCP socket
- Main thread: Atomic flags (`g_restart_emulator_requested`)
- WebRTC server: Via callback for codec changes

**Synchronization**:
- `poll()` for non-blocking I/O
- Atomic variables for flags
- Mutex for API state access

---

#### Thread 4+: WebRTC Threads (libdatachannel)

**Created by**: libdatachannel library
**Count**: Multiple internal threads
**Purpose**: WebRTC protocol handling

**Responsibilities**:
- DTLS/SRTP encryption
- ICE candidate gathering
- STUN/TURN connectivity
- RTP packet transmission
- DataChannel message handling
- WebSocket signaling

**Not directly managed by application code** - library internal.

---

### Emulator Process Threads

Located in: `/home/mick/macemu/BasiliskII/src/`

#### Thread 1: Main Thread (68k Emulation)

**Function**: `Start680x0()` called from `main()` (line 1113)
**Purpose**: Mac 68k CPU emulation
**File**: `main_unix.cpp`

**What it does**:
1. Execute 68k instructions
2. Handle interrupts (60Hz timer, audio, etc.)
3. Process Mac OS system calls
4. Update emulated hardware state
5. Render to Mac framebuffer

**Communicates with**:
- Video thread: Reads from Mac framebuffer
- Audio thread: Triggers via interrupt flags
- Control socket thread: Via interrupt flags
- Tick thread: Receives 60Hz interrupts

**Synchronization**:
- Signal handlers for 68k interrupts
- Mutex `intflag_lock` for interrupt flags
- Emulated SR register for interrupt masking

**Key Variables**:
```cpp
uint32 InterruptFlags       // Pending interrupts
M68kRegisters regs          // CPU state
uint8* the_buffer           // Mac framebuffer
```

---

#### Thread 2: Video Refresh Thread

**Function**: `video_refresh_thread()` (line 834)
**Purpose**: Framebuffer conversion and publishing
**Created by**: `IPC_VideoInit()` via `std::thread` (line 1185)
**File**: `/home/mick/macemu/BasiliskII/src/IPC/video_ipc.cpp`

**What it does**:
1. Rate-limit to 60 FPS
2. Read Mac framebuffer (various depths: 1/2/4/8/16/32-bit)
3. Convert to BGRA format (libyuv)
4. Compute dirty rectangles (pixel comparison)
5. Write to shared memory frame buffer
6. Update cursor position
7. Signal frame complete via eventfd
8. Track mouse latency statistics

**Communicates with**:
- Main thread: Reads Mac framebuffer
- Server: Writes to shared memory
- Control socket thread: Reads mouse latency

**Synchronization**:
- Plain memory access to Mac framebuffer (owned by emulator)
- Triple-buffer indices (plain writes)
- eventfd write for atomicity (kernel provides barrier)
- `std::this_thread::sleep_for()` for rate limiting

**Key Variables**:
```cpp
std::atomic<bool> video_thread_running
uint8* the_buffer           // Mac framebuffer
MacEmuIPCBuffer* video_shm  // Shared memory
int frame_width, frame_height, frame_depth
```

**Conversion Paths**:
- 32-bit (ARGB) → BGRA: `libyuv::ARGBToBGRA()`
- 16-bit (RGB555) → BGRA: Manual pixel expansion
- 8/4/2/1-bit (indexed) → BGRA: Palette lookup

---

#### Thread 3: Control Socket Thread

**Function**: `control_socket_thread()` (line 493)
**Purpose**: Input event processing
**Created by**: `IPC_VideoInit()` via `std::thread` (line 1189)
**File**: `/home/mick/macemu/BasiliskII/src/IPC/video_ipc.cpp`

**What it does**:
1. Accept server connection on Unix socket
2. Send eventfd file descriptors via SCM_RIGHTS
3. Receive binary input messages
4. Process keyboard events → `ADBKeyDown/Up()`
5. Process mouse events → `ADBMouseMoved()`, `ADBMouseDown/Up()`
6. Process commands (start/stop/reset)
7. Process ping messages (RTT measurement)
8. Measure mouse input latency

**Communicates with**:
- Server: Unix socket (binary protocol)
- Main thread: Calls ADB functions directly
- Video thread: Reads latency stats

**Synchronization**:
- `recv()` with non-blocking flag
- ADB mutex (internal to ADB subsystem)
- 1ms sleep to avoid busy-waiting

**Key Variables**:
```cpp
std::atomic<bool> control_thread_running
int control_socket          // Connected server
int listen_socket           // Listening socket
```

**Binary Protocol**:
```
MacEmuKeyInput      (8 bytes)  - Keyboard events
MacEmuMouseInput    (20 bytes) - Mouse events (relative/absolute)
MacEmuCommandInput  (8 bytes)  - Commands (stop/reset/pause)
MacEmuPingInput     (24 bytes) - Latency pings
MacEmuAudioRequestInput (8 bytes) - Audio pull requests
MacEmuMouseModeInput (8 bytes)    - Mouse mode changes
```

---

#### Thread 4: Audio Thread

**Function**: `audio_thread_func()` (line 251)
**Purpose**: Audio frame generation (pull model)
**Created by**: `AudioInit()` via `std::thread` (line 177)
**File**: `/home/mick/macemu/BasiliskII/src/IPC/audio_ipc.cpp`

**What it does**:
1. **Wait** for server request (blocking on condition variable)
2. Trigger Mac audio interrupt via `SetInterruptFlag(INTFLAG_AUDIO)`
3. Wait for Mac to fill audio buffer (condition variable)
4. Read audio data from Mac memory via `audio_data` pointer
5. Convert U8→S16 if needed
6. Write frame to ring buffer
7. Update atomic write index
8. Send silence if Mac not ready

**Communicates with**:
- Server: Woken by audio requests
- Main thread: Triggers interrupt, waits for response
- Mac sound manager: Reads `audio_data` structure

**Synchronization**:
- Condition variable `audio_request_cv` (server → audio thread)
- Condition variable `audio_irq_done_cv` (main thread → audio thread)
- Mutex `audio_request_mutex`
- Mutex `audio_irq_mutex`
- Atomic ring buffer indices

**Key Variables**:
```cpp
std::atomic<bool> audio_thread_running
std::atomic<uint32_t> audio_frame_write_idx
MacEmuAudioFrame audio_frame_ring[3]
uint32 audio_data           // Mac memory pointer
```

**Audio Flow**:
```
Server request → audio_request_cv.notify_one()
  → SetInterruptFlag(INTFLAG_AUDIO) → TriggerInterrupt()
  → AudioInterrupt() → GetSourceData()
  → audio_irq_done_cv.notify_one()
  → Copy to ring buffer
```

---

#### Thread 5: 60Hz Tick Thread

**Function**: `tick_func()` (line 1494)
**Purpose**: System timer interrupts
**Created by**: `main()` via `pthread_create()` (line 1043)
**File**: `main_unix.cpp`

**What it does**:
1. Sleep for 16.625ms (60.15 Hz)
2. Call `one_tick()` every iteration
3. Call `one_second()` every 60 ticks
4. Set interrupt flags
5. Trigger main thread interrupt

**Communicates with**:
- Main thread: Sets `InterruptFlags` and sends `SIG_IRQ`

**Synchronization**:
- Mutex `intflag_lock` for interrupt flags
- Precise timing via `Delay_usec()`

**Key Variables**:
```cpp
std::atomic<bool> tick_thread_cancel
int64 ticks                 // Tick counter
```

---

#### Thread 6: XPRAM Watchdog Thread

**Function**: `xpram_func()` (line 1434)
**Purpose**: Periodic XPRAM save
**Created by**: `main()` via `pthread_create()` (line 1107)
**File**: `main_unix.cpp`

**What it does**:
1. Sleep for 60 seconds
2. Check if XPRAM changed
3. Save to disk if changed

**Communicates with**:
- Main thread: Reads `XPRAM` array

**Synchronization**:
- `memcmp()` for change detection
- No explicit locks (XPRAM changes are rare)

---

## Shared Memory Structure

### MacEmuIPCBuffer

**Name**: `/macemu-video-{PID}`
**Size**: ~25 MB (24,883,200 bytes)
**Owner**: Emulator process
**Created by**: `create_video_shm()` in `video_ipc.cpp:154`
**Destroyed by**: `destroy_video_shm()` in `video_ipc.cpp:210`

**File**: `/home/mick/macemu/BasiliskII/src/IPC/ipc_protocol.h:171`

**Structure Layout**:

```c
typedef struct MacEmuIPCBuffer {
    // === HEADER (32 bytes) ===
    uint32_t magic;              // 0x4D454D34 ("MEM4")
    uint32_t version;            // 4
    uint32_t pid;                // Emulator PID
    uint32_t state;              // 0=stopped, 1=running, 2=paused

    // === VIDEO METADATA (32 bytes) ===
    uint32_t width;              // Current width (≤1920)
    uint32_t height;             // Current height (≤1080)
    uint32_t pixel_format;       // 0=ARGB, 1=BGRA
    uint32_t _reserved;

    // === DIRTY RECT (16 bytes) ===
    uint32_t dirty_x;            // Top-left X
    uint32_t dirty_y;            // Top-left Y
    uint32_t dirty_width;        // Width (0 = no changes)
    uint32_t dirty_height;       // Height

    // === CURSOR (8 bytes) ===
    uint16_t cursor_x;           // Cursor X position
    uint16_t cursor_y;           // Cursor Y position
    uint8_t cursor_visible;      // 1 = visible
    uint8_t _cursor_reserved[3];

    // === VIDEO SYNC (48 bytes) ===
    uint32_t write_index;        // 0-2 (emulator writing)
    uint32_t ready_index;        // 0-2 (server reading)
    uint64_t frame_count;        // Total frames
    uint64_t timestamp_us;       // CLOCK_REALTIME

    // === LATENCY STATS (8 bytes) ===
    atomic<uint32_t> mouse_latency_avg_ms;  // x10 for 0.1ms precision
    atomic<uint32_t> mouse_latency_samples;

    // === PING TIMESTAMPS (32 bytes) ===
    struct {
        uint64_t t1_browser_ms;   // Browser send
        uint64_t t2_server_us;    // Server receive
        uint64_t t3_emulator_us;  // Emulator receive
        uint64_t t4_frame_us;     // Frame complete
    } ping_timestamps;
    atomic<uint32_t> ping_sequence;  // Sequence number

    // === EVENTFD (4 bytes) ===
    int32_t frame_ready_eventfd; // Video frame ready signal

    // === AUDIO METADATA (32 bytes) ===
    uint32_t audio_sample_rate;  // 11025/22050/44100/48000
    uint32_t audio_channels;     // 1=mono, 2=stereo
    uint32_t audio_format;       // 0=none, 1=PCM_S16
    uint32_t audio_samples_in_frame;

    uint32_t audio_write_index;  // (legacy, unused)
    uint32_t audio_ready_index;  // (legacy, unused)
    uint64_t audio_frame_count;
    uint64_t audio_timestamp_us;

    int32_t audio_ready_eventfd; // Audio frame ready signal

    // === AUDIO RING BUFFER (12 bytes + frames) ===
    atomic<uint32_t> audio_frame_write_idx;  // 0-2
    atomic<uint32_t> audio_frame_read_idx;   // 0-2

    MacEmuAudioFrame audio_frame_ring[3];    // 3 frames
    MacEmuAudioFrame audio_silence_frame;    // Silence template

    atomic<uint32_t> capture_trigger;        // Audio capture flag

    // === VIDEO FRAMES (24,883,200 bytes) ===
    uint8_t frames[3][MACEMU_BGRA_FRAME_SIZE];
    //     ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    //     3 × (1920 × 1080 × 4) = 24,883,200 bytes
} MacEmuIPCBuffer;
```

**Triple Buffering**:
```
frames[0]  ←── write_index (emulator writing)
frames[1]  ←── ready_index (server reading)
frames[2]  ←── (third buffer, free)
```

**Audio Frame Ring Buffer**:
```c
typedef struct MacEmuAudioFrame {
    uint32_t sample_rate;      // Actual rate
    uint32_t channels;         // 1 or 2
    uint32_t samples;          // Actual samples (≤960)
    uint32_t format;           // PCM_S16
    uint64_t timestamp_us;     // Completion time
    uint32_t _padding[2];

    uint8_t data[3840];        // Max: 960 samples × 2 ch × 2 bytes
} MacEmuAudioFrame;
```

**Ring Buffer Indices**:
```
audio_frame_ring[0]  ←── write_idx (emulator writing)
audio_frame_ring[1]  ←── read_idx (server reading)
audio_frame_ring[2]  ←── (third slot)
```

---

## IPC Mechanisms

### 1. Shared Memory (POSIX SHM)

**Type**: `shm_open()` / `mmap()`
**Name**: `/macemu-video-{PID}`
**Size**: 25 MB (fixed)
**Permissions**: 0666 (read/write all)

**Created by**: Emulator (`video_ipc.cpp:161`)
```cpp
video_shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
ftruncate(video_shm_fd, sizeof(MacEmuIPCBuffer));
video_shm = mmap(nullptr, ..., PROT_READ | PROT_WRITE, MAP_SHARED, ...);
```

**Connected by**: Server (`ipc/ipc_connection.cpp`)
```cpp
int shm_fd = shm_open(shm_name.c_str(), O_RDWR, 0);
shm_ = mmap(nullptr, ..., PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
```

**Data flow**:
- Emulator → Server: Video frames, audio frames, metadata
- No flow in reverse direction (read-only for server)

**Synchronization**: eventfd signals completion

---

### 2. Unix Socket (SOCK_STREAM)

**Type**: `AF_UNIX` datagram socket
**Path**: `/tmp/macemu-{PID}.sock`
**Protocol**: Binary messages (fixed-size structs)

**Created by**: Emulator (`video_ipc.cpp:227`)
```cpp
listen_socket = socket(AF_UNIX, SOCK_STREAM, 0);
bind(listen_socket, &addr, sizeof(addr));
listen(listen_socket, 1);
```

**Connected by**: Server (`ipc/ipc_connection.cpp`)
```cpp
control_socket = socket(AF_UNIX, SOCK_STREAM, 0);
connect(control_socket, &addr, sizeof(addr));
```

**Data flow**:
- Server → Emulator: Input events (keyboard, mouse, commands, pings)
- Emulator → Server: None (unidirectional)

**Message types**:
```
MACEMU_INPUT_KEY         8 bytes   Keyboard event
MACEMU_INPUT_MOUSE      20 bytes   Mouse event
MACEMU_INPUT_COMMAND     8 bytes   Emulator command
MACEMU_INPUT_PING       24 bytes   Latency ping
MACEMU_INPUT_AUDIO_REQUEST 8 bytes Audio request (pull)
MACEMU_INPUT_MOUSE_MODE  8 bytes   Mouse mode change
```

**Synchronization**: Non-blocking `recv()` with 1ms polling

---

### 3. Event File Descriptor (eventfd)

**Type**: Linux `eventfd()` with `EFD_SEMAPHORE`
**Count**: 2 per emulator (video, audio)
**Lifetime**: Created by emulator, shared via SCM_RIGHTS

**Created by**: Emulator (`ipc_protocol.h:457`)
```cpp
frame_ready_eventfd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
audio_ready_eventfd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
```

**Shared to server**: Via Unix socket control message (`video_ipc.cpp:524`)
```cpp
struct msghdr msg;
cmsg = CMSG_FIRSTHDR(&msg);
cmsg->cmsg_level = SOL_SOCKET;
cmsg->cmsg_type = SCM_RIGHTS;
memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * num_fds);
sendmsg(control_socket, &msg, 0);
```

**Used by server**: Epoll monitoring (`server.cpp:1739`)
```cpp
int epoll_fd = epoll_create1(0);
epoll_ctl(epoll_fd, EPOLL_CTL_ADD, frame_ready_eventfd, &ev);
epoll_wait(epoll_fd, events, 1, 5);  // 5ms timeout
read(eventfd, &val, sizeof(val));     // Consume signal
```

**Signaling**:
- Emulator writes: `write(eventfd, &val, sizeof(val))` (line: `ipc_protocol.h:419`)
- Server reads: `read(eventfd, &val, sizeof(val))` (line: `server.cpp:1879`)

**Memory barrier**: Kernel provides acquire/release semantics
- All emulator writes before `write(eventfd)` are visible after server's `read(eventfd)`

---

### 4. Atomic Variables (C++11 atomics)

**Type**: `std::atomic<T>` / `_Atomic T`
**Locations**: Shared memory fields
**Purpose**: Lock-free synchronization

**Examples**:
```cpp
// Latency stats (emulator writes, server reads)
atomic<uint32_t> mouse_latency_avg_ms;
atomic<uint32_t> mouse_latency_samples;

// Ping sequence (emulator writes, server reads)
atomic<uint32_t> ping_sequence;

// Audio ring buffer (both read and write)
atomic<uint32_t> audio_frame_write_idx;
atomic<uint32_t> audio_frame_read_idx;
```

**Memory ordering**:
```cpp
#define ATOMIC_LOAD(ptr)  (ptr).load(std::memory_order_acquire)
#define ATOMIC_STORE(ptr, val) (ptr).store(val, std::memory_order_release)
```

**Guarantees**:
- `memory_order_acquire`: All writes before release are visible
- `memory_order_release`: All writes before this are visible after acquire

---

## Communication Flow

### Complete Data Flow Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          BROWSER PROCESS                                │
│                                                                          │
│  JavaScript Thread                                                       │
│  ├─ User Input Capture                                                  │
│  │  └─ Keyboard/Mouse events → Binary protocol → WebSocket             │
│  ├─ WebRTC Peer Connection                                              │
│  │  ├─ Video Track: H.264/AV1/PNG decoder → Canvas                     │
│  │  └─ Audio Track: Opus decoder → Web Audio API                       │
│  └─ Latency Measurement                                                 │
│     └─ Ping/pong via DataChannel                                        │
└─────────────────────────────────────────────────────────────────────────┘
                                    │ ▲
                            WebRTC  │ │  WebRTC
                          (signaling│ │ media)
                                    ▼ │
┌─────────────────────────────────────────────────────────────────────────┐
│                        WEBRTC SERVER PROCESS                             │
│                                                                          │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────────┐ │
│  │ HTTP Thread      │  │ Video Thread     │  │ Audio Thread         │ │
│  │ (API/files)      │  │ (main)           │  │                      │ │
│  └──────────────────┘  └──────────────────┘  └──────────────────────┘ │
│          │                      │ ▲                    │ ▲             │
│          │                      │ │                    │ │             │
│          │              epoll() │ │ SHM read      20ms │ │ SHM read   │
│          │              on      │ │ triple        poll │ │ ring buf   │
│          │              eventfd │ │ buffer             │ │             │
│          │                      │ │                    │ │             │
│          │                      ▼ │                    ▼ │             │
│          │                 ┌──────────────────────────────┐            │
│          │                 │  IPC Connection Manager      │            │
│          │                 │  - SHM mapping               │            │
│          │                 │  - Unix socket               │            │
│          │                 │  - eventfd (SCM_RIGHTS)      │            │
│          │                 └──────────────────────────────┘            │
│          │                      │ ▲                                    │
│          └──────────────────────┼─┼────────────────────────────────────┤
│                          Control│ │ SHM                                │
│                           socket│ │ /macemu-video-{PID}                │
│                                 │ │                                    │
└─────────────────────────────────┼─┼────────────────────────────────────┘
                                  │ │
                          Binary  │ │  Triple-buffered
                          input   │ │  video + audio
                          msgs    │ │  ring buffer
                                  ▼ │
┌─────────────────────────────────────────────────────────────────────────┐
│                        EMULATOR PROCESS                                  │
│                                                                          │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌────────────┐ │
│  │ Main Thread  │  │ Video Thread │  │ Control      │  │ Audio      │ │
│  │ (68k CPU)    │  │              │  │ Socket Thread│  │ Thread     │ │
│  └──────────────┘  └──────────────┘  └──────────────┘  └────────────┘ │
│        │ ▲               │ ▲               │ ▲               │ ▲       │
│        │ │               │ │               │ │               │ │       │
│   Read │ │ Write    Read │ │ Write    Recv │ │ Send     Wait │ │ Write │
│   Mac  │ │ to       Mac  │ │ to       input│ │ ADB      for  │ │ to    │
│   FB   │ │ Mac FB   FB   │ │ SHM      msgs │ │ calls    IRQ  │ │ SHM   │
│        │ │               │ │               │ │               │ │       │
│        ▼ │               ▼ │               ▼ │               ▼ │       │
│  ┌────────────────────────────────────────────────────────────────┐   │
│  │              Shared Resources (Emulator-owned)                  │   │
│  │  - Mac framebuffer (the_buffer)                                 │   │
│  │  - MacEmuIPCBuffer (SHM)                                        │   │
│  │  - Unix socket (/tmp/macemu-{PID}.sock)                         │   │
│  │  - Video eventfd (frame_ready_eventfd)                          │   │
│  │  - Audio eventfd (audio_ready_eventfd)                          │   │
│  └────────────────────────────────────────────────────────────────┘   │
│                                                                         │
│  ┌──────────────┐  ┌──────────────┐                                   │
│  │ Tick Thread  │  │ XPRAM Thread │                                   │
│  │ (60Hz timer) │  │ (save PRAM)  │                                   │
│  └──────────────┘  └──────────────┘                                   │
└─────────────────────────────────────────────────────────────────────────┘
```

---

### Input Event Flow (Browser → Emulator)

**Step-by-step**:

1. **Browser**: User presses key
   - JavaScript captures `keydown` event
   - Get keycode (e.g., 65 = 'A')
   - Get timestamp: `performance.now()`
   - Create binary message: `[type=3, keycode=65, down=1, timestamp]`
   - Send via WebRTC DataChannel

2. **Server - WebRTC Thread**: Receive DataChannel message
   - `handle_input_binary()` called
   - Parse binary format
   - Convert browser keycode → Mac keycode: `keyboard_map::browser_to_mac_keycode(65)` → `0x00`
   - Call `send_key_input(0x00, true)`

3. **Server - Video Thread**: Send to emulator
   - Create `MacEmuKeyInput` struct:
     ```c
     struct {
       hdr.type = MACEMU_INPUT_KEY;
       hdr.flags = MACEMU_KEY_DOWN;
       mac_keycode = 0x00;
     }
     ```
   - `send()` on Unix socket to emulator

4. **Emulator - Control Socket Thread**: Receive input
   - `recv()` on Unix socket (1ms poll)
   - Parse `MacEmuInputHeader`
   - Dispatch to `process_binary_input()`
   - Call `ADBKeyDown(0x00)`

5. **Emulator - Main Thread**: Process ADB event
   - ADB subsystem queues key event
   - Next Mac OS system call reads keyboard
   - Application receives 'A' key press

**Latency breakdown**:
- Browser JS → DataChannel: <1ms (in-process)
- WebRTC network: 1-5ms (local) or 10-50ms (remote)
- Server processing: <1ms
- Unix socket: <0.1ms (local)
- Emulator ADB: <0.5ms
- **Total**: ~3-10ms (local), 12-60ms (remote)

---

### Video Frame Flow (Emulator → Browser)

**Step-by-step**:

1. **Emulator - Main Thread**: Render Mac framebuffer
   - Mac OS renders to memory at `the_buffer`
   - Format: Mac native (1/2/4/8/16/32-bit)
   - Size: Variable based on resolution and depth

2. **Emulator - Video Thread**: Convert and publish (60 FPS loop)
   - Read Mac framebuffer
   - Convert to BGRA (B,G,R,A bytes):
     - 32-bit: `libyuv::ARGBToBGRA()` (shuffle channels)
     - 16-bit: Manual RGB555 → BGRA expansion
     - 8/4/2/1-bit: Palette lookup + expansion
   - Compute dirty rectangle (compare with frame N-2)
   - Write to `frames[write_index]` in SHM
   - Update metadata (width, height, dirty rect, cursor)
   - Swap buffers: `ready_index = write_index`, `write_index++`
   - Signal completion: `write(frame_ready_eventfd, 1)`

3. **Server - Video Thread**: Wait for frame
   - `epoll_wait(epoll_fd, ..., 5)` blocks on eventfd
   - eventfd fires → `read(eventfd, &val)`
   - Read frame from `frames[ready_index]`
   - Read metadata (width, height, dirty rect)

4. **Server - Video Thread**: Encode frame
   - **H.264/AV1 path**:
     - Convert BGRA → I420: `libyuv::ARGBToI420()` (emulator naming is confusing)
     - Encode I420 → H.264/AV1: `h264_encoder.encode()` or `av1_encoder.encode()`
     - Get NAL units (H.264) or OBUs (AV1)
   - **PNG path**:
     - Extract dirty rect from BGRA
     - Convert BGRA → RGB: `libyuv::ARGBToRGB24()`
     - Compress RGB → PNG: `libpng` (zlib deflate)
     - Prepend metadata header with dirty rect, cursor, ping

5. **Server - Video Thread**: Send via WebRTC
   - **H.264/AV1**: RTP packetization → Video track
   - **PNG**: DataChannel message (binary)
   - Add metadata (frame number, timestamp, latency)

6. **Browser - WebRTC Thread**: Receive media
   - **H.264/AV1**: RTP depacketization → Decoder → Canvas
   - **PNG**: DataChannel message → Decompress → Canvas rect

**Frame timing (60 FPS)**:
- Mac render: 16.67ms intervals
- Emulator convert: ~1-2ms (BGRA conversion)
- Server encode:
  - H.264: ~2-5ms (hardware) or 5-15ms (software)
  - AV1: ~10-30ms (software)
  - PNG: ~5-15ms (zlib compression)
- Network: 1-5ms (local) or 10-50ms (remote)
- Browser decode/render: ~1-3ms
- **Total latency**: 20-50ms (local), 40-120ms (remote)

---

### Audio Frame Flow (Emulator → Browser)

**Pull Model Architecture** (Server-driven timing):

1. **Server - Audio Thread**: Request audio (20ms intervals)
   - Sleep for 20ms (Opus frame duration)
   - Send `MacEmuAudioRequestInput` via Unix socket
     ```c
     struct {
       hdr.type = MACEMU_INPUT_AUDIO_REQUEST;
       requested_samples = 960;  // 20ms @ 48kHz
     }
     ```

2. **Emulator - Control Socket Thread**: Receive request
   - Parse `MACEMU_INPUT_AUDIO_REQUEST`
   - Call `audio_request_data(960)`

3. **Emulator - Audio Thread**: Wake up and produce frame
   - Condition variable `audio_request_cv.notify_one()` wakes thread
   - Set interrupt: `SetInterruptFlag(INTFLAG_AUDIO)` + `TriggerInterrupt()`
   - Wait for Mac: `audio_irq_done_cv.wait()`

4. **Emulator - Main Thread**: Handle audio interrupt
   - Interrupt handler calls `AudioInterrupt()`
   - Mac Sound Manager callback: `GetSourceData()`
   - Mac fills `audio_data` structure:
     ```c
     struct StreamComponentData {
       uint32 scd_sampleCount;   // Samples produced
       uint32 scd_buffer;        // Mac memory pointer
       uint16 scd_numChannels;   // 1 or 2
       uint16 scd_sampleSize;    // 8 or 16 bits
     }
     ```
   - Signal completion: `audio_irq_done_cv.notify_one()`

5. **Emulator - Audio Thread**: Copy to ring buffer
   - Read Mac audio buffer: `Mac2HostAddr(buffer_ptr)`
   - Convert U8 → S16 if needed
   - Write to `audio_frame_ring[write_idx]`:
     ```c
     frame.sample_rate = 44100;  // Mac native rate
     frame.channels = 2;
     frame.samples = 882;        // 20ms @ 44.1kHz
     frame.format = PCM_S16;
     memcpy(frame.data, src, data_len);
     ```
   - Update index: `ATOMIC_STORE(write_idx, (write_idx + 1) % 3)`

6. **Server - Audio Thread**: Read from ring buffer
   - Check `audio_frame_read_idx != audio_frame_write_idx`
   - Read `audio_frame_ring[read_idx]`
   - Update index: `ATOMIC_STORE(read_idx, (read_idx + 1) % 3)`

7. **Server - Audio Thread**: Resample and encode
   - Resample Mac rate → 48kHz (if needed):
     - 44.1kHz → 48kHz: `libsamplerate` or `libspeexdsp`
     - Input: 882 samples @ 44.1kHz
     - Output: 960 samples @ 48kHz
   - Byte-swap: S16MSB (Mac big-endian) → S16LE (Opus little-endian)
   - Encode to Opus: `opus_encode()` (20ms frames, 48kHz stereo)
   - Output: ~50-150 bytes compressed

8. **Server - Audio Thread**: Send via WebRTC
   - RTP packetization (Opus payloader)
   - Send on audio track

9. **Browser - WebRTC Thread**: Decode and play
   - RTP depacketization
   - Opus decoder → PCM
   - Web Audio API buffer → Speaker

**Audio timing**:
- Mac generates: 20ms worth of samples (variable rate)
- Server requests: Every 20ms (fixed intervals)
- Ring buffer absorbs: Clock drift, jitter (60ms = 3 frames)
- Network jitter buffer: Browser-side (WebRTC built-in)

**Underrun handling**:
- If `read_idx == write_idx`: No frames available
- Server sends silence frame (pre-zeroed PCM)
- Emulator catches up within next request

---

### Latency Measurement Flow (Ping/Pong)

**Round-trip timestamp collection**:

1. **Browser**: Send ping
   - Generate sequence number: `ping_seq++`
   - Get timestamp: `t1 = performance.now()` (milliseconds)
   - Send via DataChannel: `[type=4, seq, t1]`

2. **Server**: Receive and forward
   - Parse ping message
   - Add server timestamp: `t2 = clock_gettime(CLOCK_REALTIME)` (microseconds)
   - Forward via Unix socket:
     ```c
     MacEmuPingInput {
       sequence = seq;
       t1_browser_send_ms = t1;
       t2_server_recv_us = t2;
     }
     ```

3. **Emulator - Control Thread**: Receive and store
   - Parse `MACEMU_INPUT_PING`
   - Add emulator timestamp: `t3 = clock_gettime(CLOCK_REALTIME)`
   - Write to SHM (regular writes, not atomic):
     ```c
     ping_timestamps.t1_browser_ms = t1;
     ping_timestamps.t2_server_us = t2;
     ping_timestamps.t3_emulator_us = t3;
     ping_timestamps.t4_frame_us = 0;  // Will be set by video thread
     ```
   - Publish with atomic seq: `ATOMIC_STORE(ping_sequence, seq)`

4. **Emulator - Video Thread**: Add frame timestamp
   - After frame completion: `update_ping_on_frame_complete()`
   - If `current_ping_seq > last_echoed_ping_seq`:
     - Set `t4 = frame_timestamp_us` (frame ready time)
     - Track for 5 frames (echo in metadata)

5. **Server - Video Thread**: Read ping timestamps
   - After frame encoding, read from SHM:
     ```c
     uint32_t seq = ATOMIC_LOAD(ping_sequence);
     if (seq > 0) {
       t1 = ping_timestamps.t1_browser_ms;
       t2 = ping_timestamps.t2_server_us;
       t3 = ping_timestamps.t3_emulator_us;
       t4 = ping_timestamps.t4_frame_us;
     }
     ```
   - Add server timestamps:
     - `t5 = server_read_us` (eventfd read time)
     - `t6 = encode_done_us` (encoding complete time)
   - Attach to frame metadata (PNG) or discard (H.264/AV1 - no metadata support)

6. **Browser**: Receive frame with ping echo
   - **PNG**: Parse metadata header, extract t1-t6
   - **H.264/AV1**: No ping support (RTP has no metadata)
   - Get timestamp: `t7 = performance.now()`
   - Calculate latencies:
     ```js
     browser_to_server = t2 - t1        // Network up
     server_to_emulator = t3 - t2       // IPC latency
     emulator_processing = t4 - t3      // Frame generation
     server_read_latency = t5 - t4      // Eventfd wake
     server_encode = t6 - t5            // Encoding
     server_to_browser = t7 - (t6/1000) // Network down
     total_rtt = t7 - t1                // End-to-end
     ```

**Ping echo mechanism**:
- Emulator echoes ping in **next 5 frames** after receiving ping
- Ensures ping response even if screen is static (no new frames)
- Heartbeat mechanism: Server sends tiny 1×1 PNG to carry ping echo

---

## Synchronization Mechanisms

### 1. Triple Buffering (Video Frames)

**Purpose**: Lock-free video frame transfer
**Mechanism**: 3 buffers with rotating indices

**Protocol**:
```
Initial state:
  write_index = 0  (emulator writing to frames[0])
  ready_index = 0  (no frames ready yet)

Emulator completes frame:
  1. Write BGRA data to frames[write_index]
  2. Update metadata (dirty rect, cursor, timestamps)
  3. ready_index = write_index  (publish frame)
  4. write_index = (write_index + 1) % 3  (advance to next)
  5. write(eventfd, 1)  (signal server)

Server reads frame:
  1. epoll_wait(eventfd)  (blocking wait)
  2. read(eventfd)  (consume signal, memory barrier)
  3. Read frames[ready_index]  (all writes guaranteed visible)
  4. Process frame
```

**Guarantees**:
- Emulator never overwrites frame being read by server
- Server always reads most recent complete frame
- No locks needed (eventfd provides synchronization)

**Buffer states**:
```
Time 0: [W0 R0] [--] [--]  (emulator writing frame 0)
Time 1: [R1 --] [W1] [--]  (frame 0 ready, writing frame 1)
Time 2: [R1 --] [R2 --] [W2]  (frame 1 ready, writing frame 2)
Time 3: [W3] [R2 --] [R3 --]  (frame 2 ready, writing frame 0)
         ^^^^^^^^^^^^^^^^^^^  (3 buffers always in different states)
```

---

### 2. Audio Ring Buffer (Lock-Free)

**Purpose**: Lock-free audio frame transfer
**Mechanism**: 3-slot ring buffer with atomic indices

**Protocol**:
```
Producer (emulator audio thread):
  1. write_idx = ATOMIC_LOAD(audio_frame_write_idx)
  2. read_idx = ATOMIC_LOAD(audio_frame_read_idx)
  3. next_write = (write_idx + 1) % 3
  4. if (next_write == read_idx) → buffer full, drop frame
  5. Write to audio_frame_ring[write_idx]
  6. ATOMIC_STORE(audio_frame_write_idx, next_write)  (publish)

Consumer (server audio thread):
  1. read_idx = ATOMIC_LOAD(audio_frame_read_idx)
  2. write_idx = ATOMIC_LOAD(audio_frame_write_idx)
  3. if (read_idx == write_idx) → buffer empty, use silence
  4. Read from audio_frame_ring[read_idx]
  5. ATOMIC_STORE(audio_frame_read_idx, (read_idx + 1) % 3)
```

**Guarantees**:
- Single producer, single consumer (SPSC queue)
- No ABA problem (indices never wrap to same value simultaneously)
- Memory ordering: `memory_order_release` on write, `memory_order_acquire` on read

**Buffer states**:
```
Empty:   read_idx == write_idx
Full:    (write_idx + 1) % 3 == read_idx
Normal:  1-2 frames buffered (60ms latency budget)
```

---

### 3. Eventfd Signaling (Video & Audio)

**Purpose**: Low-latency event notification (no polling)
**Type**: Linux `eventfd()` with `EFD_SEMAPHORE` flag

**Emulator side** (producer):
```cpp
// Frame complete
uint64_t val = 1;
write(frame_ready_eventfd, &val, sizeof(val));
// Kernel provides memory barrier - all previous writes visible
```

**Server side** (consumer):
```cpp
// Wait for event
struct epoll_event events[1];
int n = epoll_wait(epoll_fd, events, 1, 5);  // 5ms timeout
if (n > 0) {
  uint64_t val;
  read(eventfd, &val, sizeof(val));  // Consume event
  // Memory barrier - all emulator writes now visible
}
```

**Advantages**:
- Zero CPU usage while waiting (kernel blocks thread)
- Immediate wake-up on event (microsecond latency)
- Built-in memory barrier (no explicit fences needed)
- Integrates with epoll (can monitor multiple eventfds)

**Semaphore mode** (`EFD_SEMAPHORE`):
- Each `write(1)` increments counter
- Each `read()` decrements by 1 and returns 1
- Prevents event coalescing (every frame is counted)

---

### 4. Condition Variables (Audio Pull Model)

**Purpose**: Block audio thread until server requests data
**Type**: C++ `std::condition_variable`

**Request flow** (server → emulator):
```cpp
// Server (control socket thread)
{
  std::lock_guard<std::mutex> lock(audio_request_mutex);
  audio_request_pending = true;
  audio_requested_samples = 960;
}
audio_request_cv.notify_one();  // Wake audio thread
```

**Response flow** (emulator audio thread):
```cpp
// Wait for request
{
  std::unique_lock<std::mutex> lock(audio_request_mutex);
  audio_request_cv.wait(lock, []{ return audio_request_pending; });
  audio_request_pending = false;
}
// Produce audio frame...
```

**IRQ synchronization** (audio thread → main thread → audio thread):
```cpp
// Audio thread: Trigger Mac interrupt
SetInterruptFlag(INTFLAG_AUDIO);
TriggerInterrupt();
{
  std::unique_lock<std::mutex> lock(audio_irq_mutex);
  audio_irq_done_cv.wait(lock, []{ return audio_irq_done; });
  audio_irq_done = false;
}

// Main thread: Handle interrupt, signal completion
AudioInterrupt();  // Mac fills audio buffer
{
  std::lock_guard<std::mutex> lock(audio_irq_mutex);
  audio_irq_done = true;
}
audio_irq_done_cv.notify_one();
```

**Advantage**: Precise timing control - server controls frame rate, Mac responds on-demand

---

### 5. Interrupt Flags and Signals (Emulator)

**Purpose**: Trigger Mac 68k interrupts
**Type**: POSIX signals + atomic flags

**Setting interrupt** (any thread):
```cpp
void SetInterruptFlag(uint32 flag) {
  LOCK_INTFLAGS;  // pthread_mutex_lock(&intflag_lock)
  InterruptFlags |= flag;
  UNLOCK_INTFLAGS;
}

void TriggerInterrupt(void) {
  pthread_kill(emul_thread, SIG_IRQ);  // Send signal to main thread
}
```

**Handling interrupt** (main thread):
```cpp
static void sigirq_handler(int sig, int code, struct sigcontext *scp) {
  if (EmulatedSR & 0x0700) return;  // Interrupts disabled

  // Set up interrupt frame on Mac stack
  uint32 a7 = regs->a[7];
  a7 -= 2; WriteMacInt16(a7, 0x64);  // Vector
  a7 -= 4; WriteMacInt32(a7, scp->sc_pc);  // Return address
  a7 -= 2; WriteMacInt16(a7, scp->sc_ps | EmulatedSR);  // SR

  // Jump to Mac interrupt handler
  scp->sc_pc = ReadMacInt32(0x64);
  EmulatedSR |= 0x2100;  // Set interrupt level
}
```

**Interrupt types**:
```cpp
#define INTFLAG_60HZ    0x01  // 60Hz timer (tick thread)
#define INTFLAG_1HZ     0x02  // 1Hz timer (tick thread)
#define INTFLAG_AUDIO   0x04  // Audio buffer ready (audio thread)
#define INTFLAG_ETHER   0x08  // Network packet (not used in IPC)
```

---

### 6. WebRTC Peer Synchronization (Server)

**Purpose**: Thread-safe access to peer connections
**Type**: C++ `std::mutex`

**Peer map access**:
```cpp
std::mutex peers_mutex_;
std::map<std::string, std::shared_ptr<PeerConnection>> peers_;

void send_h264_frame(const EncodedFrame& frame) {
  std::lock_guard<std::mutex> lock(peers_mutex_);
  for (auto& [id, peer] : peers_) {
    if (peer->codec == CodecType::H264 && peer->video_track) {
      peer->video_track->send(frame.data);
    }
  }
}
```

**Thread contention**:
- Video thread: Frequent reads (every frame)
- Audio thread: Frequent reads (every 20ms)
- WebRTC threads: Rare writes (peer connect/disconnect)
- HTTP thread: Rare reads (status queries)

**Lock duration**: <1us (only map lookup, no I/O under lock)

---

## Summary Table

### Threads by Process

| Process | Thread | Purpose | Created By | Sync Method |
|---------|--------|---------|------------|-------------|
| **Server** | Video (main) | Frame encoding | `main()` | epoll on eventfd |
| Server | Audio | Audio encoding | `std::thread` | 20ms sleep + ring buffer |
| Server | HTTP | API server | `std::thread` | poll() on TCP socket |
| Server | WebRTC (N) | RTP/DTLS | libdatachannel | (library internal) |
| **Emulator** | Main | 68k CPU | `main()` | Signals (SIG_IRQ) |
| Emulator | Video Refresh | BGRA conversion | `std::thread` | 60 FPS sleep + eventfd write |
| Emulator | Control Socket | Input processing | `std::thread` | 1ms poll on Unix socket |
| Emulator | Audio | Frame generation | `std::thread` | Condition variable (pull) |
| Emulator | 60Hz Tick | Timer interrupts | `pthread_create` | Precise sleep (16.625ms) |
| Emulator | XPRAM | PRAM saving | `pthread_create` | 60s sleep |

### IPC Resources

| Type | Name | Owner | Purpose | Sync |
|------|------|-------|---------|------|
| SHM | `/macemu-video-{PID}` | Emulator | Video+audio frames | eventfd |
| Unix Socket | `/tmp/macemu-{PID}.sock` | Emulator | Input events | Non-blocking recv |
| eventfd | (in SHM) | Emulator | Frame ready signal | epoll |
| eventfd | (in SHM) | Emulator | Audio ready signal | (unused) |

### Data Flows

| Direction | Type | Mechanism | Latency |
|-----------|------|-----------|---------|
| Browser → Server | Input | WebRTC DataChannel | 1-5ms local, 10-50ms remote |
| Server → Emulator | Input | Unix socket (binary) | <0.1ms |
| Emulator → Server | Video | SHM + eventfd | <0.5ms |
| Emulator → Server | Audio | SHM ring buffer | <1ms |
| Server → Browser | Video | WebRTC (H.264/AV1/PNG) | 5-30ms encode + network |
| Server → Browser | Audio | WebRTC (Opus) | 2-10ms encode + network |

---

## File Locations

### Server Files
- Main: `/home/mick/macemu/web-streaming/server/server.cpp`
- IPC: `/home/mick/macemu/web-streaming/server/ipc/ipc_connection.cpp`
- HTTP: `/home/mick/macemu/web-streaming/server/http/http_server.cpp`
- Encoders: `/home/mick/macemu/web-streaming/server/{h264,av1,png,opus}_encoder.cpp`

### Emulator Files
- Main: `/home/mick/macemu/BasiliskII/src/Unix/main_unix.cpp`
- Video IPC: `/home/mick/macemu/BasiliskII/src/IPC/video_ipc.cpp`
- Audio IPC: `/home/mick/macemu/BasiliskII/src/IPC/audio_ipc.cpp`
- Protocol: `/home/mick/macemu/BasiliskII/src/IPC/ipc_protocol.h`

---

## Performance Characteristics

### Video Latency Budget (60 FPS target)

```
Mac render:           16.67ms  (60 Hz Mac OS)
Emulator conversion:   1-2ms   (BGRA conversion)
IPC transfer:         <0.5ms   (SHM + eventfd)
Server encode:         2-15ms  (H.264: 2-5ms, PNG: 5-15ms, AV1: 10-30ms)
Network:              1-50ms   (local: 1-5ms, remote: 10-50ms)
Browser decode:        1-3ms   (hardware decode)
─────────────────────────────────────────
Total:                22-87ms  (typical: 30-40ms local, 60-90ms remote)
```

### Audio Latency Budget (20ms frames)

```
Mac generation:       20ms     (Sound Manager callback)
IPC transfer:        <1ms     (ring buffer)
Server resample:      1-2ms   (44.1k→48k)
Server encode:        1-3ms   (Opus)
Network jitter:       10-50ms (WebRTC jitter buffer)
Browser decode:       1-2ms   (Opus decode)
─────────────────────────────────────────
Total:                33-78ms (typical: 40-60ms)
```

### Throughput

**Video** (1920×1080 @ 60 FPS):
- Raw BGRA: 1920×1080×4×60 = 497 MB/s (SHM bandwidth)
- H.264 stream: 2-10 Mbps (network)
- PNG stream: 5-20 Mbps (network)
- AV1 stream: 1-5 Mbps (network)

**Audio** (48kHz stereo):
- Raw PCM: 48000×2×2 = 192 KB/s
- Opus stream: 32-128 Kbps (network)

---

*End of Architecture Documentation*
