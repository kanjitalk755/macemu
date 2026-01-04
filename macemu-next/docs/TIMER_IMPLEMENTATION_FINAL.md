# Timer Implementation: Final Design (timerfd-based)

## Summary

Successfully implemented **60 Hz timer** using Linux `timerfd_create()` with polling from Unicorn block hook.

**Status:** ✅ **WORKING** - Verified at 60.0 Hz (16.667ms intervals)

## Why timerfd Instead of Signals?

**Problem with SIGALRM approach:**
- Emulator uses extensive signal masking for other features (vsof, etc.)
- `sigprocmask(SIG_BLOCK, [ALRM, ...])` blocks timer signals during execution
- Signal handlers only executed sporadically (~0.2-4 Hz instead of 60 Hz)
- Incompatible with emulator's signal-based architecture

**Solution:**
- Use `timerfd_create()` (file descriptor-based timer)
- Poll timer from Unicorn block hook (every basic block)
- No signal handling = no conflicts

## Implementation

### Timer Setup ([timer_interrupt.cpp](../src/platform/timer_interrupt.cpp))

```cpp
// Create non-blocking timerfd
int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);

// Configure for 60 Hz (16.667ms = 16,667,000 nanoseconds)
struct itimerspec spec;
spec.it_value.tv_nsec = 16667000;      // Initial expiration
spec.it_interval.tv_nsec = 16667000;   // Repeat interval

timerfd_settime(timer_fd, 0, &spec, NULL);
```

### Timer Polling ([timer_interrupt.cpp](../src/platform/timer_interrupt.cpp))

Called from Unicorn block hook ([unicorn_wrapper.c:222](../src/cpu/unicorn_wrapper.c#L222)):

```cpp
uint64_t poll_timer_interrupt(void) {
    uint64_t expirations = 0;

    // Non-blocking read (returns immediately if no expirations)
    if (read(timer_fd, &expirations, sizeof(expirations)) != sizeof(expirations)) {
        return 0;  // No timer events
    }

    // Handle each expiration (usually 1, but could be >1 if we fell behind)
    for (uint64_t i = 0; i < expirations; i++) {
        SetInterruptFlag(INTFLAG_60HZ);   // Mac-level interrupt flag
        PendingInterrupt = true;           // CPU-level interrupt flag
        interrupt_count++;
    }

    return expirations;
}
```

### Integration Points

1. **Setup:** [main.cpp:421](../src/main.cpp#L421) - Called after CPU initialization
2. **Polling:** [unicorn_wrapper.c:222](../src/cpu/unicorn_wrapper.c#L222) - Called from block hook
3. **Interrupt Check:** [unicorn_wrapper.c:228](../src/cpu/unicorn_wrapper.c#L228) - PendingInterrupt checked after polling
4. **Teardown:** [main.cpp:exit_loop](../src/main.cpp) - Called at shutdown

## Test Results

**Timestamp verification (EMULATOR_TIMEOUT=2):**
```
[TIMER] Tick 1 at 794838.141 (expirations=1)
[TIMER] Tick 2 at 794838.158 (expirations=1)  ← +17ms
[TIMER] Tick 3 at 794838.174 (expirations=1)  ← +16ms
[TIMER] Tick 4 at 794838.191 (expirations=1)  ← +17ms
[TIMER] Tick 5 at 794838.208 (expirations=1)  ← +17ms
[TIMER] Tick 6 at 794838.224 (expirations=1)  ← +16ms
[TIMER] Tick 7 at 794838.241 (expirations=1)  ← +17ms
[TIMER] Tick 8 at 794838.258 (expirations=1)  ← +17ms
[TIMER] Tick 9 at 794838.274 (expirations=1)  ← +16ms
[TIMER] Tick 10 at 794838.291 (expirations=1) ← +17ms
```

**Average interval:** (794838.291 - 794838.141) / 9 = **16.67ms** = **60.0 Hz** ✅

**Key observations:**
- Consistent 16-17ms intervals (exactly as expected for 60 Hz)
- `expirations=1` every time (not falling behind)
- Timer fires correctly even with signal masking active
- No "Caught up" messages (would indicate timer delays)

## Architecture Diagram

```
Main Loop                  Unicorn Execution           Timer (Kernel)
   |                              |                          |
   | cpu_execute_one()           |                          |
   |----------------------------->|                          |
   |                              |                          |
   |                     [Block Hook Fires]                  |
   |                              |                          |
   |                   poll_timer_interrupt()                |
   |                              |------------------------->|
   |                              |    read(timer_fd)        |
   |                              |<-------------------------|
   |                              |    expirations=1         |
   |                              |                          |
   |               SetInterruptFlag(INTFLAG_60HZ)            |
   |               PendingInterrupt = true                   |
   |                              |                          |
   |                    [Check PendingInterrupt]             |
   |                    [Handle M68K interrupt if needed]    |
   |                              |                          |
   |<-----------------------------|                          |
   |    (return to main loop)     |                          |
```

## Key Features

### 1. No Signal Conflicts
- Uses file descriptor instead of signals
- Compatible with emulator's signal masking
- No async-signal-safe constraints

### 2. Frequent Polling
- Polled from Unicorn block hook (every ~10-50 instructions)
- Fast enough to catch all timer expirations
- Non-blocking so doesn't slow emulation

### 3. Catchup Detection
- `expirations` count shows if timer fell behind
- Can handle multiple expirations at once
- Debug logging warns if catchup happens

### 4. Precision
- Nanosecond resolution (struct itimerspec)
- Kernel-driven timing (not user-space sleep)
- Monotonic clock (immune to system time changes)

## Comparison: timerfd vs pthread vs SIGALRM

| Feature | timerfd (macemu-next) | pthread (BasiliskII) | SIGALRM (failed) |
|---------|----------------------|----------------------|------------------|
| **Threading** | No separate thread | Dedicated thread | No thread |
| **Polling** | From block hook | N/A (thread sleeps) | N/A (signal) |
| **Conflicts** | None | None | ❌ Signal masking |
| **Complexity** | Low (~80 lines) | Medium (~100 lines) | Low (~60 lines) |
| **Overhead** | Minimal | Thread context switch | N/A (blocked) |
| **Portability** | Linux only | POSIX | POSIX |
| **Precision** | Nanosecond | Microsecond | Microsecond |
| **Catchup** | Automatic | Manual drift check | N/A |
| **Working?** | ✅ Yes | ✅ Yes | ❌ No (blocked) |

## Future Work

1. ⏳ **1Hz timer:** Add tick_counter to call `one_second()` every 60 ticks
2. ⏳ **Video refresh:** Call `VideoRefresh()` when INTFLAG_60HZ is set
3. ⏳ **Audio interrupt:** Implement `AudioInterrupt()` at 60 Hz
4. ⏳ **Performance:** Consider using `epoll()` if adding more file descriptors

## Files Modified

- `src/platform/timer_interrupt.cpp` - Timer implementation (switched from setitimer to timerfd)
- `src/platform/timer_interrupt.h` - API (added poll_timer_interrupt, extern "C" linkage)
- `src/cpu/unicorn_wrapper.c` - Added poll_timer_interrupt() call in block hook
- `src/main.cpp` - Calls poll_timer_interrupt() in main loop (belt-and-suspenders)

## Lessons Learned

1. **Signal-based timers don't work with signal masking** - Emulators that use signals for synchronization need file-descriptor-based or thread-based timers
2. **Poll from execution loop, not main loop** - CPU backends may execute many instructions between returning to main loop
3. **timerfd is cleaner than pthread for simple periodic timers** - No thread overhead, simpler code
4. **Timestamp debugging is essential** - Only way to verify actual timer rate vs. theoretical rate

## References

- Linux `timerfd_create(2)` man page
- Original BasiliskII timer: `BasiliskII/src/Unix/main_unix.cpp:1041-1506`
- Signal masking investigation: strace output showing `SIG_BLOCK [ALRM]`
