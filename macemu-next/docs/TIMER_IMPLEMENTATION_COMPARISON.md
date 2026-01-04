# Timer Implementation Comparison

## Overview

This document compares timer interrupt implementations that were considered or implemented in macemu-next.

## Current Implementation: Polling-Based ✅

**Status:** WORKING - 60.0 Hz verified

**Mechanism:**
```cpp
// Check wall-clock time directly in CPU execution loops
uint64_t poll_timer_interrupt(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ns = now.tv_sec * 1000000000ULL + now.tv_nsec;

    if (now_ns - last_timer_ns >= 16667000ULL) {  // 60 Hz
        last_timer_ns = now_ns;
        SetInterruptFlag(INTFLAG_60HZ);
        g_platform.cpu_trigger_interrupt(intlev());
        return 1;
    }
    return 0;
}
```

**Integration:**
- **UAE**: Called every 100 instructions in `uae_cpu_execute_one()`
- **Unicorn**: Called every block in `hook_block()`

**Advantages:**
- ✅ Simple: ~60 lines of code
- ✅ Fast: ~20-50ns overhead per check
- ✅ Reliable: Can't be blocked by signal masks
- ✅ Portable: Standard POSIX `clock_gettime()`
- ✅ Debuggable: Synchronous, can add printf freely
- ✅ Unified: Works for both UAE and Unicorn

**Performance:**
- UAE overhead: ~1% (polling every 100 instructions)
- Unicorn overhead: <0.1% (amortized over block)

---

## Previous Attempts

### ❌ SIGALRM Approach (Rejected)

**Mechanism:**
```cpp
// Set up periodic SIGALRM timer
struct itimerval timer;
timer.it_value.tv_usec = 16667;      // 60 Hz
timer.it_interval.tv_usec = 16667;
setitimer(ITIMER_REAL, &timer, NULL);

// Signal handler
static void timer_signal_handler(int signum) {
    SetInterruptFlag(INTFLAG_60HZ);
    g_platform.cpu_trigger_interrupt(intlev());
}
```

**Why it failed:**
- Emulator uses extensive `sigprocmask()` for other features
- Timer signals get blocked during execution
- Actual rate: ~0.2-4 Hz instead of 60 Hz
- Async-signal-safe constraints in handler code
- Complex: 126 lines of signal setup

**Test results:**
- Expected: 1800 interrupts over 30 seconds (60 Hz)
- Actual: 6 interrupts over 30 seconds (0.2 Hz)
- **Blocked by signal masking ❌**

### ❌ timerfd Approach (Considered but rejected)

**Mechanism:**
```cpp
// Create file descriptor-based timer
int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);

struct itimerspec spec;
spec.it_value.tv_nsec = 16667000;      // 60 Hz
spec.it_interval.tv_nsec = 16667000;
timerfd_settime(timer_fd, 0, &spec, NULL);

// Poll from block hook
uint64_t expirations;
read(timer_fd, &expirations, sizeof(expirations));
```

**Why we didn't use it:**
- Requires managing file descriptors
- Linux-specific (not portable)
- More complex than necessary
- Adds `read()` syscall overhead
- Still need polling from execution loop anyway

**Comparison to polling:**
- timerfd: ~80 lines, file descriptor, Linux-only
- Polling: ~60 lines, no FDs, POSIX portable

### ⚠️ pthread Thread Approach (BasiliskII uses this)

**Mechanism:**
```cpp
// Separate thread sleeps and triggers interrupts
void *timer_thread(void *arg) {
    while (running) {
        usleep(16667);  // 60 Hz
        SetInterruptFlag(INTFLAG_60HZ);
    }
}
pthread_create(&timer_tid, NULL, timer_thread, NULL);
```

**Why we didn't use it:**
- Adds threading complexity
- Thread context switch overhead
- Need synchronization between thread and CPU execution
- macemu-next is single-threaded by design
- Polling is simpler and just as effective

---

## Comparison Table

| Aspect | SIGALRM | timerfd | pthread | Polling (Current) |
|--------|---------|---------|---------|-------------------|
| **Lines of code** | 126 | ~80 | ~100 | ~60 |
| **Complexity** | High | Medium | Medium | Low |
| **Dependencies** | signal.h | Linux timerfd | pthread | time.h (POSIX) |
| **Portability** | POSIX | Linux-only | POSIX | POSIX |
| **Threading** | No | No | Yes | No |
| **File descriptors** | 0 | 1 | 0 | 0 |
| **Can be blocked?** | ❌ Yes | ✅ No | ✅ No | ✅ No |
| **Overhead** | Signal delivery | read() syscall | Thread switch | ~20ns |
| **Precision** | μs | ns | μs | ns |
| **Debugging** | Hard (async) | Medium | Medium | Easy (sync) |
| **Works?** | ❌ No | ✅ Yes | ✅ Yes | ✅ Yes |
| **Used by** | macemu-next (old) | Considered | BasiliskII | macemu-next (current) |

---

## Why Polling Won

The polling approach emerged as the best solution because:

1. **Simplicity**: Fewest lines of code, easiest to understand
2. **Performance**: Lowest overhead (~20ns per check)
3. **Reliability**: Can't be blocked or masked
4. **Portability**: Standard POSIX, no platform-specific code
5. **Debuggability**: Synchronous, can add logging freely
6. **Unification**: Same implementation for both UAE and Unicorn

The key insight (from user suggestion) was that **CPU execution loops already run >500K times/sec**, so checking time is essentially free when amortized over instruction execution cost.

---

## Migration Path

If you have old SIGALRM code:

**Before (SIGALRM):**
```cpp
// Setup
setup_timer_interrupt(16667);  // microseconds

// Signal handler (async, complex)
static void timer_signal_handler(int signum) {
    SetInterruptFlag(INTFLAG_60HZ);
    // ... async-signal-safe code only ...
}
```

**After (Polling):**
```cpp
// Setup (simpler)
setup_timer_interrupt();  // no parameters needed

// Poll from execution loop (sync, simple)
void uae_cpu_execute_one(void) {
    static int poll_counter = 0;
    if (++poll_counter >= 100) {
        poll_counter = 0;
        poll_timer_interrupt();  // Can do anything here!
    }
    // ... execute instruction ...
}
```

---

## References

- [TIMER_IMPLEMENTATION_FINAL.md](TIMER_IMPLEMENTATION_FINAL.md) - Current implementation details
- [TIMER_REFACTOR_PLAN.md](TIMER_REFACTOR_PLAN.md) - Refactoring plan
- [TIMER_INVESTIGATION_RESULTS.md](TIMER_INVESTIGATION_RESULTS.md) - Why SIGALRM failed
- [SIMPLIFIED_INTERRUPT_APPROACH.md](SIMPLIFIED_INTERRUPT_APPROACH.md) - User's polling insight

## Credits

**User insight:** "Do we even need sigalarm or threads for this? We could just check time in between blocks and decide then to interrupt..."

This simple observation led to eliminating all timer complexity in favor of a clean, fast, reliable polling solution.
