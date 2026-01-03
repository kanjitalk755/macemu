# Design: Simplified Unix Socket Audio Architecture

## Overview

Replace the current shared memory + eventfd IPC audio system with a simple Unix domain socket using blocking I/O. This eliminates complexity while maintaining all functionality and improving audio quality.

---

## Current Problems

### 1. **Complexity**
- Shared memory with circular buffers
- eventfd for signaling
- epoll for event notification
- Atomic operations for synchronization
- Manual buffer management (`audio_ready_index`, `audio_write_index`)

### 2. **Audio Quality Issues**
- Naive linear resampling (44100Hz → 48000Hz)
- Timing mismatch: Mac sends ~93ms chunks, server processes as 20ms frames
- No buffering/jitter management
- Resampling on pre-chunked data causes artifacts

### 3. **Race Conditions**
- Thread wake-up timing issues (just fixed)
- Complex state management across processes

---

## Proposed Architecture

### High-Level Design

```
┌─────────────────────────────────────────────────────────────┐
│ BasiliskII Emulator Process                                 │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  Mac OS Sound Manager                                       │
│         ↓                                                    │
│  AudioInterrupt() → GetSourceData()                         │
│         ↓                                                    │
│  Audio Thread (20ms loop)                                   │
│         ↓                                                    │
│  Format Audio Packet (header + samples)                     │
│         ↓                                                    │
│  write() to Unix socket (BLOCKING)                          │
│                                                              │
└────────────────────────┬────────────────────────────────────┘
                         │
                         │ Unix Domain Socket
                         │ /tmp/macemu-audio-{PID}.sock
                         │
┌────────────────────────▼────────────────────────────────────┐
│ Server Process                                               │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  Audio Thread                                               │
│         ↓                                                    │
│  read() from Unix socket (BLOCKING)                         │
│         ↓                                                    │
│  Parse Audio Packet                                         │
│         ↓                                                    │
│  Handle silence / format changes                            │
│         ↓                                                    │
│  Resample (libswresample - high quality)                    │
│         ↓                                                    │
│  Opus Encode (48kHz, 20ms frames)                           │
│         ↓                                                    │
│  WebRTC Send                                                │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

---

## Wire Protocol

### Packet Format

Every audio transmission consists of a fixed-size header followed by variable-length sample data.

```cpp
struct AudioPacketHeader {
    uint32_t magic;          // 0x4D414344 ("MACD") - sanity check
    uint32_t sample_rate;    // 11025, 22050, 44100, etc.
    uint16_t channels;       // 1 (mono) or 2 (stereo)
    uint16_t sample_size;    // 8 or 16 bits
    uint16_t flags;          // Bit 0: silence flag
    uint16_t reserved;       // For future use, set to 0
    uint32_t num_samples;    // Number of samples (per channel)
    uint64_t timestamp_us;   // Microsecond timestamp (for jitter calc)
};
// Total: 24 bytes

// Followed by sample data:
// - If (flags & AUDIO_FLAG_SILENCE): no data follows
// - Else: num_samples * channels * (sample_size/8) bytes
```

### Flags

```cpp
#define AUDIO_FLAG_SILENCE  (1 << 0)  // Packet contains silence
#define AUDIO_FLAG_UNDERRUN (1 << 1)  // Mac audio underrun occurred
```

### Packet Types

#### 1. **Active Audio Packet**
```
[Header: flags=0, num_samples=4096, rate=44100, channels=2, size=16]
[Sample Data: 16384 bytes of S16 PCM]
```

#### 2. **Silence Packet**
```
[Header: flags=AUDIO_FLAG_SILENCE, num_samples=960, rate=44100, channels=2, size=16]
[No sample data - header only]
```

#### 3. **Format Change**
Mac changes from 44100Hz stereo to 22050Hz mono:
```
[Header: rate=22050, channels=1, ...]
[Sample Data with new format]
```
Server detects change and reinitializes resampler/encoder.

---

## Component Details

### A. Emulator Side (`audio_ipc.cpp`)

#### Socket Creation (during `AudioInit()`)

```cpp
static int audio_socket_fd = -1;

void AudioInit(void) {
    // ... existing init ...

    // Create Unix domain socket
    audio_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    // Connect to server's listening socket
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path),
             "/tmp/macemu-audio-%d.sock", getppid()); // Server PID

    if (connect(audio_socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Audio IPC: Failed to connect to audio socket\n");
        close(audio_socket_fd);
        audio_socket_fd = -1;
        return;
    }

    // Start audio thread
    audio_thread = std::thread(audio_thread_func);
}
```

#### Audio Thread

```cpp
static void audio_thread_func() {
    const auto frame_interval = std::chrono::milliseconds(20);

    while (!audio_thread_cancel) {
        auto frame_start = std::chrono::steady_clock::now();

        AudioPacketHeader header = {0};
        header.magic = 0x4D414344;
        header.sample_rate = AudioStatus.sample_rate >> 16;
        header.channels = AudioStatus.channels;
        header.sample_size = AudioStatus.sample_size;

        if (AudioStatus.num_sources > 0) {
            // Trigger interrupt to get Mac audio
            SetInterruptFlag(INTFLAG_AUDIO);
            TriggerInterrupt();

            // Wait for completion
            std::unique_lock<std::mutex> lock(audio_irq_mutex);
            audio_irq_done_cv.wait(lock, []{ return audio_irq_done; });
            audio_irq_done = false;

            // Read audio from Mac memory
            uint32 apple_stream_info = ReadMacInt32(audio_data + adatStreamInfo);
            if (apple_stream_info) {
                uint32 sample_count = ReadMacInt32(apple_stream_info + scd_sampleCount);
                uint32 buffer_ptr = ReadMacInt32(apple_stream_info + scd_buffer);

                if (sample_count > 0 && buffer_ptr != 0) {
                    // Convert endianness and send
                    header.flags = 0;
                    header.num_samples = sample_count;
                    header.timestamp_us = get_timestamp_us();

                    send_audio_packet(&header, buffer_ptr, sample_count);
                } else {
                    // Send silence
                    send_silence_packet(&header);
                }
            } else {
                send_silence_packet(&header);
            }
        } else {
            // No sources - send silence to maintain timing
            send_silence_packet(&header);
        }

        // Sleep for remainder of 20ms frame
        auto elapsed = std::chrono::steady_clock::now() - frame_start;
        auto remaining = frame_interval - elapsed;
        if (remaining > std::chrono::milliseconds(0)) {
            std::this_thread::sleep_for(remaining);
        }
    }
}

static void send_audio_packet(AudioPacketHeader* header,
                              uint32 buffer_ptr, uint32 sample_count) {
    if (audio_socket_fd < 0) return;

    // Send header
    if (write(audio_socket_fd, header, sizeof(*header)) != sizeof(*header)) {
        fprintf(stderr, "Audio IPC: Failed to write header\n");
        return;
    }

    // Convert and send samples
    size_t bytes_per_sample = (header->sample_size >> 3) * header->channels;
    size_t data_size = sample_count * bytes_per_sample;

    std::vector<uint8_t> buffer(data_size);

    if (header->sample_size == 16) {
        // Convert S16MSB to S16LE
        int16_t* src = (int16_t*)Mac2HostAddr(buffer_ptr);
        int16_t* dst = (int16_t*)buffer.data();
        for (uint32 i = 0; i < sample_count * header->channels; i++) {
            dst[i] = ntohs(src[i]);
        }
    } else {
        // 8-bit, just copy
        memcpy(buffer.data(), Mac2HostAddr(buffer_ptr), data_size);
    }

    if (write(audio_socket_fd, buffer.data(), data_size) != data_size) {
        fprintf(stderr, "Audio IPC: Failed to write audio data\n");
    }
}

static void send_silence_packet(AudioPacketHeader* header) {
    if (audio_socket_fd < 0) return;

    header->flags = AUDIO_FLAG_SILENCE;
    header->num_samples = 960; // 20ms @ 48kHz equivalent
    header->timestamp_us = get_timestamp_us();

    // Send header only (no data for silence)
    write(audio_socket_fd, header, sizeof(*header));
}
```

---

### B. Server Side (`server.cpp`)

#### Socket Setup (during server startup)

```cpp
static int audio_listen_fd = -1;
static int audio_client_fd = -1;

void setup_audio_socket() {
    audio_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path),
             "/tmp/macemu-audio-%d.sock", getpid());

    // Remove old socket file if exists
    unlink(addr.sun_path);

    bind(audio_listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(audio_listen_fd, 1);

    fprintf(stderr, "Audio: Listening on %s\n", addr.sun_path);
}

void accept_audio_connection() {
    // Called after emulator connects
    audio_client_fd = accept(audio_listen_fd, nullptr, nullptr);
    if (audio_client_fd >= 0) {
        fprintf(stderr, "Audio: Emulator connected\n");
    }
}
```

#### Audio Loop

```cpp
static void audio_loop_unix_socket(WebRTCServer& webrtc) {
    fprintf(stderr, "Audio: Starting audio loop (Unix socket mode)\n");

    // Optional: Use libswresample for high-quality resampling
    SwrContext* swr_ctx = nullptr;
    int last_rate = 0, last_channels = 0;

    while (g_running && audio_client_fd >= 0) {
        // Read packet header (blocking)
        AudioPacketHeader header;
        ssize_t n = read(audio_client_fd, &header, sizeof(header));

        if (n != sizeof(header)) {
            if (n == 0) {
                fprintf(stderr, "Audio: Emulator disconnected\n");
                break;
            }
            fprintf(stderr, "Audio: Header read error\n");
            continue;
        }

        // Validate magic
        if (header.magic != 0x4D414344) {
            fprintf(stderr, "Audio: Invalid packet magic\n");
            continue;
        }

        // Handle silence
        if (header.flags & AUDIO_FLAG_SILENCE) {
            // Don't send anything to WebRTC
            continue;
        }

        // Read sample data
        size_t bytes_per_sample = (header.sample_size >> 3) * header.channels;
        size_t data_size = header.num_samples * bytes_per_sample;

        std::vector<uint8_t> samples(data_size);
        n = read(audio_client_fd, samples.data(), data_size);
        if (n != data_size) {
            fprintf(stderr, "Audio: Sample data read error\n");
            continue;
        }

        // Handle format changes
        if (header.sample_rate != last_rate || header.channels != last_channels) {
            fprintf(stderr, "Audio: Format change: %uHz %uch -> %uHz %uch\n",
                    last_rate, last_channels, header.sample_rate, header.channels);

            // Reinitialize resampler
            if (swr_ctx) swr_free(&swr_ctx);
            swr_ctx = swr_alloc_set_opts(nullptr,
                AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 48000,
                header.channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO,
                AV_SAMPLE_FMT_S16, header.sample_rate,
                0, nullptr);
            swr_init(swr_ctx);

            last_rate = header.sample_rate;
            last_channels = header.channels;
        }

        // Resample to 48kHz stereo using libswresample
        std::vector<int16_t> resampled;
        if (swr_ctx) {
            int output_samples = av_rescale_rnd(header.num_samples,
                                                48000, header.sample_rate,
                                                AV_ROUND_UP);
            resampled.resize(output_samples * 2); // stereo

            const uint8_t* in_data[1] = { samples.data() };
            uint8_t* out_data[1] = { (uint8_t*)resampled.data() };

            int converted = swr_convert(swr_ctx, out_data, output_samples,
                                       in_data, header.num_samples);
            resampled.resize(converted * 2);
        } else {
            // Fallback: use current linear resampler
            resampled = g_audio_encoder->resample_linear(...);
        }

        // Encode to Opus
        auto opus_data = g_audio_encoder->encode_dynamic(
            resampled.data(),
            resampled.size() / 2,
            48000,
            2
        );

        // Send to WebRTC
        if (!opus_data.empty()) {
            webrtc.send_audio_to_all_peers(opus_data);
        }
    }

    if (swr_ctx) swr_free(&swr_ctx);
    fprintf(stderr, "Audio: Exiting audio loop\n");
}
```

---

## Key Design Decisions

### 1. **Always Send Frames (Even Silence)**

**Why**:
- Maintains consistent 20ms timing on both sides
- Server can always do blocking reads (simpler than timeouts)
- Prevents audio click/pop artifacts when starting/stopping
- Matches OSS driver behavior

**How**:
- Emulator sends 50 packets/sec (one every 20ms)
- Silence packets are header-only (24 bytes)
- Active packets include sample data

### 2. **Blocking I/O**

**Why**:
- Simpler than non-blocking + epoll
- Natural flow control (back-pressure)
- Kernel handles buffering

**Concerns**:
- What if server is slow? → Socket buffer provides ~100ms cushion
- What if emulator is slow? → Server blocks on read (acceptable)

### 3. **Self-Describing Packets**

**Why**:
- Format can change at any time (Mac OS feature)
- No separate handshake/negotiation needed
- Easy to debug (wireshark-style packet dumps)

**Overhead**:
- 24 bytes per packet = 1.2 KB/sec @ 50Hz
- Negligible compared to sample data (3.8 KB @ 44100Hz stereo)

### 4. **libswresample for Quality**

**Why**:
- Current linear resampler causes artifacts
- libswresample uses polyphase filters (industry standard)
- Already have FFmpeg dependency (libyuv, etc.)

**Fallback**:
- Keep linear resampler as fallback if libswresample unavailable

---

## Migration Path

### Phase 1: Implement Unix Socket (Keep Current Protocol)
- Replace shared memory with Unix socket
- Keep current IPC protocol structure
- Test that it works

### Phase 2: Simplify Protocol
- Switch to packet-based protocol
- Remove shared memory structures entirely

### Phase 3: Improve Quality
- Add libswresample
- Add jitter buffer (optional)
- Tune Opus encoder settings

---

## Error Handling

### Emulator Side
- **Socket write fails**: Log error, continue (don't crash emulator)
- **Server disconnects**: Attempt reconnect after 1 second
- **Mac audio underrun**: Set flag in packet header

### Server Side
- **Emulator disconnects**: Close socket, wait for reconnection
- **Invalid packet**: Skip and resync on next magic number
- **Format change**: Reinitialize resampler/encoder

---

## Performance Considerations

### Latency
- **Current**: eventfd (~10-50 μs) + epoll (~10-50 μs) = ~100 μs
- **Unix socket**: ~50-100 μs for write+read
- **Negligible difference**

### CPU
- **Current**: Atomic ops, memory barriers, epoll syscalls
- **Unix socket**: Simple read/write syscalls
- **Should be similar or better**

### Memory
- **Current**: 64KB shared memory (2x 16KB buffers + metadata)
- **Unix socket**: Kernel socket buffer (~16KB default)
- **Similar**

---

## Testing Strategy

1. **Unit test**: Packet serialization/deserialization
2. **Format changes**: Test 8-bit, 16-bit, mono, stereo, all sample rates
3. **Silence handling**: Verify no WebRTC packets sent during silence
4. **Quality**: A/B test with tone generator (current vs. libswresample)
5. **Robustness**: Kill/restart server, kill/restart emulator

---

## Open Questions

1. **Should we buffer multiple Mac chunks before resampling?**
   - Pro: Better resampling quality (more context)
   - Con: Adds latency

2. **Should we implement a jitter buffer on server side?**
   - Pro: Smoother playback if timing varies
   - Con: More complexity, more latency

3. **Should silence packets have sample data or header-only?**
   - Current design: Header-only (saves bandwidth)
   - Alternative: Send actual zero samples (easier server logic)

---

## Summary

This design:
- ✅ Eliminates shared memory complexity
- ✅ Eliminates eventfd/epoll complexity
- ✅ Natural flow control via blocking I/O
- ✅ Self-describing packets handle format changes
- ✅ Path to high-quality resampling (libswresample)
- ✅ Simpler debugging (packet dumps)
- ✅ Similar performance to current design
- ✅ Cleaner code (~50% less than current)

---

## Comparison: Current vs. Proposed

| Aspect | Current (SHM + eventfd) | Proposed (Unix Socket) |
|--------|-------------------------|------------------------|
| **Lines of code** | ~400 (emulator) + ~150 (server) | ~250 (emulator) + ~100 (server) |
| **IPC mechanism** | Shared memory + eventfd | Unix domain socket |
| **Synchronization** | Atomics + eventfd | Blocking I/O |
| **Flow control** | Manual (circular buffer) | Kernel (socket buffer) |
| **Format changes** | Via SHM fields | Via packet header |
| **Silence handling** | Skip writes | Header-only packets |
| **Resampling** | Linear (poor quality) | libswresample (high quality) |
| **Debugging** | Memory dumps | Packet dumps |
| **Latency** | ~100 μs | ~100 μs |
| **Complexity** | High | Low |
