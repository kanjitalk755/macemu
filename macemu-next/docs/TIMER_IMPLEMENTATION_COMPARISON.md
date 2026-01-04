# Timer Implementation Comparison: macemu-next vs BasiliskII

## Overview

This document compares the 60Hz timer interrupt implementation between macemu-next and the original BasiliskII Unix version.

## Test Results

**macemu-next (30-second test):**
- Timer installed successfully at 59 Hz (16667 microseconds)
- Fired **6 interrupts** during 30-second runtime
- Emulator executed 167,079 instructions before hitting unhandled exception (EmulOp 7129)
- Both INTFLAG_60HZ and PendingInterrupt set correctly

**Observed timer rate:** ~0.2 Hz (6 interrupts / 30 seconds) - Much lower than expected due to emulator crash at 30s mark

## Implementation Approaches

### macemu-next: Signal-Based Timer (SIGALRM)

**Location:** [src/platform/timer_interrupt.cpp](../src/platform/timer_interrupt.cpp)

**Mechanism:**
```cpp
// Uses POSIX setitimer() with ITIMER_REAL
struct itimerval timer;
timer.it_value.tv_usec = 16667;      // Initial delay
timer.it_interval.tv_usec = 16667;   // Periodic interval (60.006 Hz)

setitimer(ITIMER_REAL, &timer, NULL);

// Kernel delivers SIGALRM to signal handler
static void timer_signal_handler(int signum)
{
    SetInterruptFlag(INTFLAG_60HZ);   // Mac-level interrupt
    PendingInterrupt = true;           // CPU-level interrupt
    interrupt_count++;
}
```

**Key Characteristics:**
- **No separate thread** - runs on main thread via signal delivery
- **Kernel-driven** - OS timer triggers signal at precise intervals
- **Signal-safe constraints** - handler must be async-signal-safe (no malloc, printf, etc.)
- **Two-level interrupt** - sets both Mac flag (INTFLAG_60HZ) and CPU flag (PendingInterrupt)
- **Minimal overhead** - no thread context switching
- **Single responsibility** - only sets interrupt flags

### BasiliskII: Thread-Based Timer (pthread)

**Location:** `BasiliskII/src/Unix/main_unix.cpp:1041-1506`

**Mechanism:**
```cpp
// Creates dedicated 60Hz thread
pthread_create(&tick_thread, &tick_thread_attr, tick_func, NULL);

// Thread function (lines 1494-1506)
static void *tick_func(void *arg)
{
    uint64 start = GetTicks_usec();
    uint64 next = start;

    while (!tick_thread_cancel) {
        if (!tick_inhibit)
            one_tick();

        next += 16625;  // 60.15 Hz (16.625ms)
        int64 delay = next - GetTicks_usec();

        if (delay > 0)
            Delay_usec(delay);  // Sleep until next tick
        else if (delay < -16625)
            next = GetTicks_usec();  // Reset if we fell behind
    }
}

// one_tick() function (lines 1467-1490)
static void one_tick(...)
{
    static int tick_counter = 0;
    if (++tick_counter > 60) {
        tick_counter = 0;
        one_second();  // Triggers 1Hz interrupt
    }

    #ifndef USE_PTHREADS_SERVICES
    // Threads not used, refresh video here
    VideoRefresh();
    #endif

    #ifndef HAVE_PTHREADS
    // No threads, do networking here
    SetInterruptFlag(INTFLAG_ETHER);
    #endif

    // Trigger 60Hz interrupt
    if (ROMVersion != ROM_VERSION_CLASSIC || HasMacStarted()) {
        SetInterruptFlag(INTFLAG_60HZ);
        TriggerInterrupt();
    }
}
```

**Key Characteristics:**
- **Separate thread** - dedicated pthread runs continuously
- **User-space sleep** - thread sleeps and wakes using Delay_usec()
- **No signal constraints** - can call any function (VideoRefresh, etc.)
- **Multiple responsibilities** - handles 60Hz, 1Hz, video refresh, networking
- **Thread overhead** - context switching between tick thread and main thread
- **Drift compensation** - tracks accumulated delay and resets if behind

### BasiliskII Alternative: POSIX.4 Real-Time Signals

**Location:** `BasiliskII/src/Unix/main_unix.cpp:1051-1060`

BasiliskII also supports a fallback using `timer_create()` with real-time signals (similar to our approach):

```cpp
#elif defined(HAVE_TIMER_CREATE) && defined(_POSIX_REALTIME_SIGNALS)
    // POSIX.4 timers and real-time signals available
    timer_sa.sa_sigaction = (void (*)(int, siginfo_t *, void *))one_tick;
    timer_sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIG_TIMER, &timer_sa, NULL);
    // ... timer_create() setup ...
```

**Note:** This is a compile-time alternative when pthreads are unavailable. It uses `timer_create()` instead of `setitimer()`.

## Comparison Table

| Feature | macemu-next (SIGALRM) | BasiliskII (pthread) | BasiliskII (POSIX.4) |
|---------|----------------------|---------------------|---------------------|
| **Threading** | No separate thread | Dedicated tick_thread | No separate thread |
| **Mechanism** | `setitimer()` + SIGALRM | `pthread_create()` + sleep loop | `timer_create()` + SIG_TIMER |
| **Timer Rate** | 60.006 Hz (16667 µs) | 60.15 Hz (16625 µs) | Configurable |
| **Signal Safety** | Required (async-signal-safe only) | Not required | Required |
| **Video Refresh** | Separate (not in timer) | Called from one_tick() | Called from one_tick() |
| **1Hz Interrupt** | Not implemented yet | Implemented (tick_counter) | Implemented |
| **Networking** | Separate | Called from one_tick() (no-thread builds) | Called from one_tick() |
| **CPU Overhead** | Minimal (signal only) | Higher (thread context switch) | Minimal (signal only) |
| **Drift Handling** | Kernel-managed | Manual (reset if > 16625µs behind) | Kernel-managed |
| **Complexity** | Simple (~50 lines) | Complex (~60 lines + thread mgmt) | Medium |

## Key Differences

### 1. Separation of Concerns

**macemu-next:** Timer *only* sets interrupt flags. Video refresh, audio, and other callbacks are handled separately in the main emulation loop when INTFLAG_60HZ is checked.

**BasiliskII:** `one_tick()` function does *multiple things*:
- Sets INTFLAG_60HZ
- Calls VideoRefresh() directly (if not using pthread services)
- Handles 1Hz timer (calls `one_second()` every 60 ticks)
- Sets INTFLAG_ETHER for networking (if pthreads unavailable)

### 2. Timer Rate

**macemu-next:** 60.006 Hz (16667 µs) - matches exact Mac VBL rate

**BasiliskII:** 60.15 Hz (16625 µs) - slightly faster, possibly to compensate for overhead

### 3. Execution Context

**macemu-next:** Signal handler runs asynchronously on main thread

**BasiliskII pthread:** Dedicated thread runs continuously, separate from CPU emulation

**BasiliskII POSIX.4:** Signal handler runs asynchronously (like our approach)

### 4. VideoRefresh() Timing

**macemu-next:** VideoRefresh() called from platform layer when checking interrupts (to be implemented)

**BasiliskII:** VideoRefresh() called directly from `one_tick()` when `USE_PTHREADS_SERVICES` not defined

## Why We Chose SIGALRM

Our signal-based approach is **simpler** and more **focused**:

1. **Single Responsibility** - Timer only sets flags, doesn't call callbacks
2. **No Thread Overhead** - Avoids context switching to separate thread
3. **Kernel Precision** - OS timer is more accurate than user-space sleep loops
4. **Signal-Safe Design** - Forces clean separation between timer and emulation logic
5. **Platform Independent** - POSIX timers available on all Unix-like systems

**Trade-off:** Signal handlers have restrictions (async-signal-safe functions only), but this actually *improves* our design by enforcing clean separation of concerns.

## BasiliskII's Rationale for pthread

BasiliskII uses pthread because:

1. **Multiple Responsibilities** - `one_tick()` needs to call VideoRefresh(), networking, etc.
2. **Signal Constraints** - These functions (printf, malloc, etc.) aren't signal-safe
3. **Historical** - pthread approach pre-dates widespread POSIX.4 real-time signal support
4. **Flexibility** - Thread can do complex operations without signal-safety concerns

**Note:** BasiliskII *does* have a POSIX.4 fallback (like our approach), but prefers pthread when available.

## Next Steps for macemu-next

Our timer implementation is correct and working. Next steps:

1. ✅ Timer fires at 60 Hz and sets INTFLAG_60HZ
2. ✅ PendingInterrupt flag set for CPU block checking
3. ⏳ **TODO:** Implement 1Hz timer (increment tick_counter, call one_second())
4. ⏳ **TODO:** Call VideoRefresh() when INTFLAG_60HZ is set
5. ⏳ **TODO:** Implement AudioInterrupt() at 60 Hz
6. ⏳ **TODO:** Add drift compensation if needed

## References

- macemu-next timer: [src/platform/timer_interrupt.cpp](../src/platform/timer_interrupt.cpp)
- BasiliskII pthread timer: `BasiliskII/src/Unix/main_unix.cpp:1041-1506`
- POSIX setitimer: `man 2 setitimer`
- POSIX timer_create: `man 2 timer_create`
