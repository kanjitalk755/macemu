# Video/Audio Callback Research - Answers to Questions

**Date**: January 3, 2026
**Research Goal**: Find where VideoRefresh() is called and where audio samples are written

---

## Question 1: Where is VideoRefresh() called from?

### Answer: From VideoInterrupt() which is called by the 60Hz interrupt handler

**Call Chain**:
```
Timer fires INTFLAG_60HZ
  → emul_op.cpp:EmulOp() checks InterruptFlags
    → Calls VideoInterrupt()
      → VideoInterrupt() calls VideoRefresh() (platform-specific)
```

**Evidence from Code**:

**File**: `src/core/emul_op.cpp:554-566`
```cpp
if (InterruptFlags & INTFLAG_60HZ) {
    ClearInterruptFlag(INTFLAG_60HZ);

    if (HasMacStarted()) {
        // Mac has started, execute all 60Hz interrupt functions
        #if !PRECISE_TIMING
        TimerInterrupt();
        #endif
        VideoInterrupt();  // ← THIS CALLS VideoRefresh()

        // Call DoVBLTask(0)
        if (ROMVersion == ROM_VERSION_32) {
            M68kRegisters r2;
            r2.d[0] = 0;
            Execute68kTrap(0xa072, &r2);
        }
    }
}
```

**File**: `src/drivers/platform/platform_null.cpp:386-388`
```cpp
void VideoInterrupt()
{
    // Currently a stub - needs to call VideoRefresh()
}
```

**File**: `src/drivers/dummy/video_dummy.cpp:135-141`
```cpp
/*
 *  Video VBL interrupt
 */

void VideoVBL(void)
{
    // Dummy - no VBL processing needed
}
```

### How INTFLAG_60HZ Gets Set

**Currently**: Not implemented in macemu-next! This is why VideoRefresh never gets called.

**In full BasiliskII**: A platform-specific timer thread sets this flag every 16.7ms.

**What We Need to Do**:
1. Set up timer interrupt (60 Hz = 16667 microseconds)
2. Timer handler sets `InterruptFlags |= INTFLAG_60HZ`
3. CPU execution loop checks `InterruptFlags` periodically
4. When `INTFLAG_60HZ` is set, EmulOp handler calls `VideoInterrupt()`
5. `VideoInterrupt()` should call `VideoRefresh()`

**Implementation Strategy**:
```cpp
// Timer signal handler (called by SIGALRM)
void timer_signal_handler(int signum) {
    // Set both flags
    SetInterruptFlag(INTFLAG_60HZ);  // For VideoInterrupt
    PendingInterrupt.store(true);     // For Unicorn block hook
}

// In VideoInterrupt() implementation
void VideoInterrupt() {
    VideoRefresh();  // Call the refresh function
}
```

---

## Question 2: Where are audio samples written?

### Answer: Mac writes samples via Sound Manager callbacks, we need to intercept in audio_enter_stream()

**How Mac Audio Works**:

1. **Mac Sound Manager** calls our audio component (see `audio.cpp:AudioDispatch()`)
2. Mac requests to **play source buffer** via `kSoundComponentPlaySourceBufferSelect`
3. We need to implement a **callback mechanism** that runs periodically
4. The callback reads from Mac's audio buffer and copies to our AudioOutput

**Key Functions**:

**File**: `src/core/audio.cpp:523-531`
```cpp
case kSoundComponentPlaySourceBufferSelect:
    D(bug(" PlaySourceBuffer flags %08lx\n", ReadMacInt32(p)));
    r.d[0] = ReadMacInt32(p);
    r.a[0] = ReadMacInt32(p + 4);
    r.a[1] = ReadMacInt32(p + 8);
    r.a[2] = AudioStatus.mixer;
    Execute68k(audio_data + adatPlaySourceBuffer, &r);  // ← Executes 68k code
    D(bug(" returns %08lx\n", r.d[0]));
    return r.d[0];
```

This executes **68k code in the Mac ROM** that manages the audio buffer. We need to intercept the data **before** it gets mixed.

**Current Dummy Implementation**:

**File**: `src/drivers/dummy/audio_dummy.cpp:71-82`
```cpp
void audio_enter_stream()
{
    // Currently empty - this is where we start audio streaming
}

void audio_exit_stream()
{
    // Currently empty - this is where we stop audio streaming
}

void AudioInterrupt(void)
{
    D(bug("AudioInterrupt\n"));
    // Currently empty - this is where Mac expects us to consume audio
}
```

### Where to Intercept Audio Samples

**Option 1: AudioInterrupt() callback** (RECOMMENDED)
- Mac calls `AudioInterrupt()` periodically when it needs us to consume audio
- We can read from Mac's audio buffers here
- Need to find Mac's buffer pointer in AudioStatus structure

**Option 2: Hook PlaySourceBuffer**
- Intercept when Mac writes to source buffer
- More complex, requires understanding 68k mixer code

**Option 3: Platform-specific streaming**
- Implement `audio_enter_stream()` to start a thread
- Thread periodically reads from Mac audio buffers
- Similar to how real audio drivers work

### Recommended Implementation

**Step 1**: Implement `AudioInterrupt()` to read Mac's audio buffer

```cpp
// In audio_dummy.cpp or new audio_webrtc.cpp

void AudioInterrupt(void)
{
    if (!g_platform.audio_output) {
        return;  // No buffer yet
    }

    // Mac stores audio in sound buffer - need to find pointer
    // AudioStatus structure has the buffer info
    // For now, we can implement when we have actual audio to test

    // Pseudo-code (need to find actual buffer location):
    // int16_t* mac_samples = GetMacAudioBuffer();
    // int sample_count = GetMacAudioSampleCount();
    //
    // g_platform.audio_output->submit_samples(
    //     mac_samples,
    //     sample_count,
    //     AudioStatus.channels,
    //     AudioStatus.sample_rate >> 16  // Convert from fixed-point
    // );
}
```

**Step 2**: Make sure AudioInterrupt() gets called

Looking at the code, I don't see where `AudioInterrupt()` is currently called!

**Need to add to emul_op.cpp**:
```cpp
// In emul_op.cpp, inside INTFLAG_60HZ handler
if (InterruptFlags & INTFLAG_60HZ) {
    ClearInterruptFlag(INTFLAG_60HZ);

    if (HasMacStarted()) {
        #if !PRECISE_TIMING
        TimerInterrupt();
        #endif
        VideoInterrupt();
        AudioInterrupt();  // ← ADD THIS LINE

        // ...rest of code...
    }
}
```

**Step 3**: Find Mac's audio buffer pointer

Need to research `AudioStatus` structure and Apple Mixer to find where Mac stores samples.

---

## Summary of Findings

### VideoRefresh()
- ✅ **Called from**: `VideoInterrupt()` (but VideoInterrupt is currently a stub)
- ✅ **Triggered by**: `INTFLAG_60HZ` flag checked in `emul_op.cpp`
- ✅ **Frequency**: 60 Hz (every 16.7ms)
- ⚠️  **Missing**: Timer to set INTFLAG_60HZ (need to implement)
- ⚠️  **Missing**: VideoInterrupt() implementation to call VideoRefresh()

### Audio Samples
- ✅ **Mac writes via**: Sound Manager API (`audio.cpp:AudioDispatch()`)
- ✅ **We intercept in**: `AudioInterrupt()` callback
- ⚠️  **Missing**: AudioInterrupt() is defined but never called!
- ⚠️  **Missing**: Implementation to read Mac's audio buffer
- ⚠️  **Need to research**: Where Mac stores audio samples in memory

---

## Action Items for Implementation

### Immediate (Phase 2)

1. **Add timer interrupt**:
   ```cpp
   setup_timer_interrupt(16667);  // 60 Hz
   ```

2. **Timer handler sets flags**:
   ```cpp
   void timer_signal_handler(int signum) {
       SetInterruptFlag(INTFLAG_60HZ);
       PendingInterrupt.store(true);
   }
   ```

3. **Implement VideoInterrupt()**:
   ```cpp
   void VideoInterrupt() {
       VideoRefresh();  // Platform-specific implementation
   }
   ```

4. **Call AudioInterrupt() from emul_op.cpp**:
   ```cpp
   if (InterruptFlags & INTFLAG_60HZ) {
       // ...
       VideoInterrupt();
       AudioInterrupt();  // ADD THIS
   }
   ```

5. **Implement VideoRefresh() in video_webrtc.cpp**:
   ```cpp
   void VideoRefresh(void) {
       // Get Mac framebuffer
       const monitor_desc &monitor = *VideoMonitors[0];
       uint32_t* mac_pixels = (uint32_t*)Mac2HostAddr(monitor.get_mac_frame_base());

       // Copy to VideoOutput buffer
       g_platform.video_output->submit_frame(...);
   }
   ```

### Future (After basic infrastructure works)

6. **Research Mac audio buffer location**:
   - Look at `AudioStatus` structure
   - Find where Apple Mixer stores samples
   - Understand buffer format (big-endian int16_t)

7. **Implement AudioInterrupt() properly**:
   - Read from Mac's audio buffer
   - Convert endianness if needed
   - Submit to AudioOutput buffer

---

## Code Locations Reference

| What | File | Line | Notes |
|------|------|------|-------|
| INTFLAG_60HZ check | `emul_op.cpp` | 554 | Main interrupt dispatcher |
| VideoInterrupt() stub | `platform_null.cpp` | 386 | Currently empty |
| VideoRefresh() prototype | `video.h` | 275 | Needs implementation |
| AudioDispatch() | `audio.cpp` | 277 | Mac Sound Manager calls |
| AudioInterrupt() stub | `audio_dummy.cpp` | 89 | Currently empty |
| audio_enter_stream() | `audio_dummy.cpp` | 71 | Called when audio starts |
| Timer defines | `main.h` | 70 | INTFLAG_60HZ = 1 |

---

**Conclusion**: We now know exactly where to hook in! The architecture is already there, we just need to:
1. Add timer interrupt to set INTFLAG_60HZ
2. Implement VideoInterrupt() to call VideoRefresh()
3. Call AudioInterrupt() from the 60Hz handler
4. Implement the actual buffer copy code in VideoRefresh() and AudioInterrupt()
