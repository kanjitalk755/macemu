# Ping/Pong Latency Tracking - Implementation Status

## Goal
Complete end-to-end latency measurement with timestamps at EVERY stage:

```
Browser (t1) → Server (t2) → Emulator (t3) → Frame Ready (t4) →
Server Read (t5) → Encode Done (t6) → Server Send (t7) → Browser Receive (t8)
```

## Implementation Status

### ✅ COMPLETE: Server-Side Timestamp Capture

#### 1. Data Structure (`codec.h`)
```cpp
struct EncodedFrame {
    // ... existing fields ...

    // Complete round-trip ping timestamps:
    uint32_t ping_sequence = 0;      // Sequence number (0 = no ping data)
    uint64_t t1_browser_ms = 0;      // Browser send (performance.now())
    uint64_t t2_server_us = 0;       // Server receive (CLOCK_REALTIME)
    uint64_t t3_emulator_us = 0;     // Emulator receive (CLOCK_REALTIME)
    uint64_t t4_frame_us = 0;        // Frame ready in SHM (CLOCK_REALTIME)
    uint64_t t5_server_read_us = 0;  // Server read from SHM (CLOCK_REALTIME) ✅ NEW
    uint64_t t6_encode_done_us = 0;  // Encoding finished (CLOCK_REALTIME) ✅ NEW
    uint64_t t7_server_send_us = 0;  // Server sending (CLOCK_REALTIME) ✅ NEW
};
```

#### 2. Timestamp Capture Points (`server.cpp`)

| Timestamp | Location | Line | Status |
|-----------|----------|------|--------|
| t1 | Browser (client.js) | 668 | ✅ Existing |
| t2 | Server receive (`ipc_connection.cpp`) | 326 | ✅ Existing |
| t3 | Emulator receive (`video_ipc.cpp`) | 426 | ✅ Existing |
| t4 | Frame ready (`video_ipc.cpp`) | 468 | ✅ Existing |
| **t5** | **Server read (after eventfd)** | **1873** | **✅ ADDED** |
| **t6** | **After encoding (H264/AV1/PNG)** | **1908, 1924, 2013** | **✅ ADDED** |
| **t7** | **Before sending (all codecs)** | **911, 722, 776** | **✅ ADDED** |
| t8 | Browser receive (client.js) | varies | ✅ Existing |

#### 3. Metadata Formats (All Codecs Unified via `EncodedFrame`)

**PNG (Data Channel):**
- Header size: 89 bytes → **113 bytes** ✅ UPDATED
- Format: `[40 base][5 cursor][68 ping]` where ping = `[seq:4][t1:8][t2:8][t3:8][t4:8][t5:8][t6:8][t7:8]`
- PNG data starts at byte 113

**H.264/AV1 (Data Channel Metadata):**
- Metadata size: 41 bytes → **65 bytes** ✅ UPDATED
- Format: `[5 cursor][4 seq][7×8 timestamps]`
- Video goes over RTP video track, metadata over data channel

#### 4. Server-Side Code Flow
```
1. epoll_wait() → eventfd triggers
2. ⏱️  t5 = clock_gettime() immediately after read (line 1873)
3. Read frame from SHM
4. Call encoder.encode_bgra()
5. ⏱️  t6 = clock_gettime() after encoding (lines 1908, 1924, 2013)
6. Call populate_frame_metadata(frame, t5, t6)
   - Reads t1-t4 from shared memory
   - Adds t5, t6 from parameters
7. Call send_xxx_frame(frame)
   - ⏱️  t7 = clock_gettime() before sending (lines 911, 722, 776)
   - Encodes all 7 timestamps into header/metadata
   - Sends to browser
```

---

### ✅ COMPLETE: Client H.264/AV1 Parsing

**File:** `client.js`

**Data Channel Handler (line 1496):**
```javascript
if (event.data.byteLength === 65) {  // Updated from 41 ✅
    const view = new DataView(event.data);
    this.handleFrameMetadata(view);
    return;
}
```

**handleFrameMetadata() (line 1730):**
- Parses all 7 timestamps from 65-byte metadata ✅
- Captures t8 (browser receive) = `performance.now()` ✅
- Calls `this.decoder.handlePingEcho(seq, t1, t2, t3, t4, t5, t6, t7, t8)` ✅

---

### ⚠️ INCOMPLETE: Client PNG Parsing

**File:** `client.js` (around line 437)

**Current State:**
```javascript
// Parse metadata header if present (ArrayBuffer with at least 89 bytes + PNG signature)
if (data instanceof ArrayBuffer && data.byteLength > 97) {  // ❌ WRONG
```

**Problems:**
1. Still expects 89-byte header (should be 113)
2. Byte length check: `> 97` should be `> 121` (113 header + 8 PNG sig)
3. Timestamp parsing stops at t4 (line ~499)
4. Missing t5, t6, t7 parsing
5. PNG data slice: `data.slice(89)` should be `data.slice(113)` (line ~528)

**Needs to Add (after line 499):**
```javascript
// t5: Server read from SHM (CLOCK_REALTIME microseconds)
lo = view.getUint32(81, true);
hi = view.getUint32(85, true);
const ping_t5_server_read_us = lo + hi * 0x100000000;

// t6: Encoding finished (CLOCK_REALTIME microseconds)
lo = view.getUint32(89, true);
hi = view.getUint32(93, true);
const ping_t6_encode_done_us = lo + hi * 0x100000000;

// t7: Server sending (CLOCK_REALTIME microseconds)
lo = view.getUint32(97, true);
hi = view.getUint32(101, true);
const ping_t7_server_send_us = lo + hi * 0x100000000;
```

**Then update handlePingEcho call (around line 519):**
```javascript
this.handlePingEcho(pingSeq, ping_browser_send_ms, ping_server_recv_us,
                   ping_emulator_recv_us, ping_frame_ready_us,
                   ping_t5_server_read_us, ping_t6_encode_done_us,  // NEW
                   ping_t7_server_send_us, ping_browser_recv_ms);   // NEW
```

---

### ⚠️ INCOMPLETE: handlePingEcho Function Signature

**File:** `client.js` (line 596 in PNGDecoder class)

**Current Signature:**
```javascript
handlePingEcho(sequence, browser_send_ms, server_recv_us, emulator_recv_us,
               frame_ready_us, server_send_us, browser_recv_ms) {  // ❌ OLD - 7 params
```

**Should Be:**
```javascript
handlePingEcho(sequence, browser_send_ms, server_recv_us, emulator_recv_us,
               frame_ready_us, server_read_us, encode_done_us,
               server_send_us, browser_recv_ms) {  // ✅ NEW - 9 params
```

**Latency Breakdown (needs updating in function body):**
```javascript
// Total RTT (browser clock only - accurate!)
const total_rtt_ms = browser_recv_ms - browser_send_ms;

// Server-side latencies (same clock - accurate!)
const t2_to_t3_us = emulator_recv_us - server_recv_us;     // Server → Emulator IPC
const t3_to_t4_us = frame_ready_us - emulator_recv_us;      // Emulator processing → frame ready
const t4_to_t5_us = server_read_us - frame_ready_us;        // ✅ NEW: Frame ready → server wakes up
const t5_to_t6_us = encode_done_us - server_read_us;        // ✅ NEW: Encoding time
const t6_to_t7_us = server_send_us - encode_done_us;        // ✅ NEW: Packetizing/sending prep

// Convert to milliseconds
const ipc_latency_ms = t2_to_t3_us / 1000.0;
const emulator_ms = t3_to_t4_us / 1000.0;
const wake_latency_ms = t4_to_t5_us / 1000.0;     // ✅ NEW
const encode_ms = t5_to_t6_us / 1000.0;           // ✅ NEW
const send_prep_ms = t6_to_t7_us / 1000.0;        // ✅ NEW

// Network latency (estimated as remainder)
const server_side_total_ms = ipc_latency_ms + emulator_ms + wake_latency_ms +
                             encode_ms + send_prep_ms;
const network_ms = total_rtt_ms - server_side_total_ms;
```

---

## Remaining Work

### 1. Update PNG Parser (client.js ~line 437-528)
- [ ] Change header size check: `89` → `113` bytes
- [ ] Add t5, t6, t7 timestamp parsing
- [ ] Update data slice offset: `slice(89)` → `slice(113)`
- [ ] Update handlePingEcho call to pass 8 timestamps

### 2. Update handlePingEcho Signature (client.js ~line 596)
- [ ] Add parameters: `server_read_us, encode_done_us` (between frame_ready_us and server_send_us)
- [ ] Update latency calculations to show new breakdown
- [ ] Update logging/display to show all stages

### 3. Test
- [ ] Rebuild server
- [ ] Test PNG codec with new header
- [ ] Test H.264 codec with new metadata
- [ ] Test AV1 codec with new metadata
- [ ] Verify all 7 timestamps are captured and displayed correctly

---

## Timeline Summary

**Timestamps Captured:**
1. t1 - Browser send ✅
2. t2 - Server receive ✅
3. t3 - Emulator receive ✅
4. t4 - Frame ready ✅
5. **t5 - Server read** ✅ (NEW - added today)
6. **t6 - Encode done** ✅ (NEW - added today)
7. **t7 - Server send** ✅ (NEW - added today)
8. t8 - Browser receive ✅

**What's Left:** Just client-side PNG parsing + handlePingEcho signature update
