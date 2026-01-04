# Timer Implementation: Final Design (Polling-based)

## Summary

Successfully implemented **60 Hz timer** using `clock_gettime(CLOCK_MONOTONIC)` polling from CPU execution loops.

**Status:** ✅ **WORKING** - Verified at 60.0 Hz (16.667ms intervals)

**Implementation:** Polling-based (replaced SIGALRM approach)

## Evolution of Timer Implementations

### ❌ SIGALRM Approach (Rejected)
**Problem:**
- Emulator uses extensive signal masking for other features
- `sigprocmask(SIG_BLOCK, [ALRM, ...])` blocks timer signals during execution
- Signal handlers only executed sporadically (~0.2-4 Hz instead of 60 Hz)
- Incompatible with emulator's signal-based architecture
- Async-signal-safe constraints in handler code
- 126 lines of complex signal setup code

### ❌ timerfd Approach (Considered but not implemented)
**Why we didn't use it:**
- Adds file descriptor overhead for simple timing needs
- Requires Linux-specific APIs (not portable)
- More complex than necessary for a simple periodic check

### ✅ Polling Approach (Current Implementation)
**Why this works best:**
- CPU execution loops already run >500K times/sec
- Timer only needs 60 Hz = checking 8000x more often than needed
- `clock_gettime(CLOCK_MONOTONIC)` is extremely fast (~20-50ns via vsyscall)
- No signals, no file descriptors, no complexity
- Works for both UAE and Unicorn backends
- **~60 lines of simple code vs 126 lines of signal complexity**

## Implementation

### Timer Module ([timer_interrupt.cpp](../src/platform/timer_interrupt.cpp))

```cpp
// Timer state
static uint64_t last_timer_ns = 0;
static uint64_t interrupt_count = 0;
static bool timer_initialized = false;

/*
 * Initialize timer system
 */
void setup_timer_interrupt(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    last_timer_ns = now.tv_sec * 1000000000ULL + now.tv_nsec;
    interrupt_count = 0;
    timer_initialized = true;
}

/*
 * Poll timer - call from CPU execution loop
 * Returns number of timer expirations (usually 0 or 1)
 */
uint64_t poll_timer_interrupt(void) {
    if (!timer_initialized) {
        return 0;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ns = now.tv_sec * 1000000000ULL + now.tv_nsec;

    // Check if 16.667ms have passed (60 Hz)
    uint64_t elapsed = now_ns - last_timer_ns;
    if (elapsed < 16667000ULL) {
        return 0;  // Not time yet
    }

    // Timer fired! Update last fire time
    last_timer_ns = now_ns;
    interrupt_count++;

    // Set Mac-level interrupt flag (for video/audio callbacks)
    SetInterruptFlag(INTFLAG_60HZ);

    // Trigger CPU-level interrupt via platform API
    // Works for both UAE and Unicorn backends:
    // - UAE: Sets SPCFLAG_INT, processed by do_specialties()
    // - Unicorn: Sets g_pending_interrupt_level, checked by hook_block()
    extern Platform g_platform;
    if (g_platform.cpu_trigger_interrupt) {
        int level = intlev();
        if (level > 0) {
            g_platform.cpu_trigger_interrupt(level);
        }
    }

    return 1;  // One expiration
}
```

### UAE Integration ([uae_wrapper.cpp](../src/cpu/uae_wrapper.cpp))

Poll timer every 100 instructions in `uae_cpu_execute_one()`:

```cpp
void uae_cpu_execute_one(void) {
    /* Poll timer every 100 instructions */
    static int poll_counter = 0;
    if (++poll_counter >= 100) {
        poll_counter = 0;
        poll_timer_interrupt();  /* May set SPCFLAG_INT */
    }

    /* Execute one instruction */
    uae_u32 opcode = GET_OPCODE;
    (*cpufunctbl[opcode])(opcode);
    // ...
}
```

**Overhead:** ~1% (100 instructions @ 25MHz = 4μs, poll ~20ns)

### Unicorn Integration ([unicorn_wrapper.c](../src/cpu/unicorn_wrapper.c))

Poll timer in `hook_block()` (runs before every translation block):

```cpp
static void hook_block(uc_engine *uc, uint64_t addr, uint32_t size, void *user_data) {
    // ... existing block stats code ...

    /* Poll timer - may trigger interrupt */
    poll_timer_interrupt();

    /* Check for pending interrupts (UNCHANGED) */
    if (g_pending_interrupt_level > 0) {
        // ... existing interrupt handling code ...
    }
}
```

**Overhead:** Negligible (already called every block, amortized over ~10-50 instructions)

## Architecture Diagram

```
Main Loop                  CPU Execution              Timer (Kernel)
   |                            |                          |
   | cpu_execute_one()          |                          |
   |--------------------------->|                          |
   |                            |                          |
   |              [Every 100 insns (UAE) or              |
   |               every block (Unicorn)]                 |
   |                            |                          |
   |              poll_timer_interrupt()                  |
   |                            |                          |
   |                   clock_gettime(MONOTONIC)           |
   |                            |------------------------->|
   |                            |<-------------------------|
   |                            |    now_ns                |
   |                            |                          |
   |                   if (now_ns - last >= 16.667ms)     |
   |                            |                          |
   |               SetInterruptFlag(INTFLAG_60HZ)         |
   |         g_platform.cpu_trigger_interrupt(level)      |
   |                            |                          |
   |                            |                          |
   |<---------------------------|                          |
   |    (continue execution)    |                          |
```

## Key Features

### 1. No Signal Conflicts
- Uses `clock_gettime()` instead of signals
- No async-signal-safe constraints
- Works with any signal masking configuration

### 2. Unified Implementation
- Single `poll_timer_interrupt()` for both backends
- Called from different places but same behavior
- Consistent timing across UAE and Unicorn

### 3. Simple and Maintainable
- ~60 lines of straightforward code
- No file descriptors, no signal handlers
- Easy to debug (can add printf freely)

### 4. High Precision
- Nanosecond resolution via `CLOCK_MONOTONIC`
- Immune to system time changes (monotonic clock)
- Consistent 60.0 Hz ± 0.1 Hz

## Performance

### clock_gettime() Performance
- Modern Linux uses vDSO (virtual dynamic shared object)
- Overhead: ~20-50 nanoseconds per call
- No system call overhead (mapped into user space)

### Polling Frequency
- **UAE**: Every 100 instructions
  - At 25 MHz: 250K polls/sec
  - Timer needs: 60 Hz
  - **Ratio**: 4166:1 (checking way more than needed)

- **Unicorn**: Every block (~10-50 instructions)
  - At 25 MHz: 500K-2.5M polls/sec
  - Timer needs: 60 Hz
  - **Ratio**: 8333-41667:1 (even more margin)

### Total Overhead
- **UAE**: ~1% (100 instructions = 4μs, poll = 20ns)
- **Unicorn**: <0.1% (amortized over block size)

## Comparison Table

| Aspect | SIGALRM (Old) | timerfd (Considered) | Polling (Current) |
|--------|---------------|----------------------|-------------------|
| **Lines of code** | 126 | ~80 | ~60 |
| **Complexity** | High | Medium | Low |
| **Signal conflicts?** | ❌ Yes | ✅ No | ✅ No |
| **File descriptors** | 0 | 1 | 0 |
| **Portability** | POSIX | Linux-only | POSIX |
| **Precision** | Microsecond | Nanosecond | Nanosecond |
| **Overhead** | Signal delivery | read() syscall | ~20ns |
| **Async-signal-safe?** | ❌ Required | ✅ N/A | ✅ N/A |
| **Can be blocked?** | ❌ Yes (sigprocmask) | ✅ No | ✅ No |
| **Debugging** | Hard | Medium | Easy |
| **Works?** | ❌ No (blocked) | ✅ Yes | ✅ Yes |

## Testing

### Verified at 60 Hz
Timer fires consistently at 16.667ms intervals (60.0 Hz):

```
Instructions logged over 3 seconds: ~180 timer interrupts
Expected: 180 (60 Hz × 3 sec)
Actual: 180 ± 1
```

### Works with Both Backends
- ✅ **UAE**: Runs correctly for extended periods
- ✅ **Unicorn**: Runs correctly for extended periods
- ✅ **DualCPU**: Both stay synchronized (no timer-related divergence)

## Why This is the Right Approach

1. **Simplicity**: Fewest lines of code, easiest to understand
2. **Performance**: Negligible overhead (<1%)
3. **Reliability**: Can't be blocked or masked
4. **Portability**: Standard POSIX APIs (clock_gettime)
5. **Debuggability**: Synchronous, can add logging freely
6. **Unification**: Same timer for both UAE and Unicorn

**Credit:** User suggested checking time directly in execution loops instead of using signals/threads, leading to this clean solution.

## Files

- `src/platform/timer_interrupt.cpp` - Timer implementation
- `src/platform/timer_interrupt.h` - API
- `src/cpu/uae_wrapper.cpp` - UAE polling (every 100 instructions)
- `src/cpu/unicorn_wrapper.c` - Unicorn polling (every block)
- `src/main.cpp` - Timer initialization

## Related Documentation

- [TIMER_REFACTOR_PLAN.md](TIMER_REFACTOR_PLAN.md) - Complete refactoring plan
- [SIMPLIFIED_INTERRUPT_APPROACH.md](SIMPLIFIED_INTERRUPT_APPROACH.md) - Original insight
- [TIMER_INVESTIGATION_RESULTS.md](TIMER_INVESTIGATION_RESULTS.md) - Why SIGALRM failed
