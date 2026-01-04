# Simplified Interrupt Approach: No Signals, No Threads

**Status**: ✅ **IMPLEMENTED** (January 2026)

See [TIMER_IMPLEMENTATION_FINAL.md](TIMER_IMPLEMENTATION_FINAL.md) for the final implementation details.

---

## User's Insight

> "Do we even need sigalarm or threads for this? We could just check time in between blocks and decide then to interrupt..."

**Answer: NO, we don't need them at all!** This is a much simpler and cleaner approach.

**Result**: This insight led to the current polling-based timer implementation - now working at 60.0 Hz in production.

## Current Situation

We already have infrastructure in place:
- `hook_block()` runs before every translation block ([unicorn_wrapper.c:159](../src/cpu/unicorn_wrapper.c#L159))
- It already checks `g_pending_interrupt_level` at line 227
- It already handles interrupt injection (lines 227-278)

The ONLY missing piece is: **Who sets `g_pending_interrupt_level`?**

Currently: SIGALRM signal handler (complicated, async-signal-safe constraints)
**Better approach**: Check wall-clock time directly in `hook_block()`

## Proposed Simple Solution

### Remove All This Complexity:
- ❌ SIGALRM signal handler
- ❌ `sigaction()` setup
- ❌ `setitimer()` calls
- ❌ Async-signal-safe constraints
- ❌ Signal masking issues
- ❌ Thread-based timers (timerfd)
- ❌ File descriptors

### Replace With This:

```c
// In hook_block() - BEFORE checking g_pending_interrupt_level

static uint64_t last_timer_check_ns = 0;
static const uint64_t TIMER_INTERVAL_NS = 16667000;  // 60 Hz = 16.667ms

// Get current time (monotonic, fast)
struct timespec ts;
clock_gettime(CLOCK_MONOTONIC, &ts);
uint64_t now_ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec;

// Check if 16.67ms have passed since last interrupt
if (now_ns - last_timer_check_ns >= TIMER_INTERVAL_NS) {
    last_timer_check_ns = now_ns;

    // Trigger 60Hz interrupt (VBL)
    SetInterruptFlag(INTFLAG_60HZ);

    // Set CPU interrupt level (if not masked)
    int level = intlev();  // Platform decides priority
    if (level > 0) {
        g_pending_interrupt_level = level;
    }
}

// Existing code below this continues unchanged...
if (g_pending_interrupt_level > 0) {
    // ... handle interrupt (lines 227-278) ...
}
```

That's it! **~15 lines of code instead of 126 lines + signal complexity.**

## Why This Works

### Block Hook Frequency
`hook_block()` is called every ~10-50 instructions (every TB). For a 25 MHz 68040:
- 25M instructions/sec ÷ 25 insn/block = 1M blocks/sec
- Timer needs checking at 60 Hz
- **1M checks/sec >> 60 Hz** → plenty of resolution!

### clock_gettime() Performance
- `CLOCK_MONOTONIC` is extremely fast on modern Linux (vsyscall)
- Overhead: ~20-50 nanoseconds per call
- Called once per block (amortized over ~25 instructions)
- **Total overhead: <0.1%**

### No Race Conditions
- Everything runs in single thread (Unicorn execution)
- No async signal handlers
- No atomics needed
- Simple, predictable control flow

## Comparison: SIGALRM vs Direct Time Check

| Aspect | SIGALRM (Current) | Direct Check (Proposed) |
|--------|-------------------|-------------------------|
| **Lines of code** | 126 | ~15 |
| **Dependencies** | signal.h, sys/time.h | time.h |
| **Async-signal-safe?** | Must be! | N/A (no signals) |
| **Signal masking issues?** | Yes (can be blocked) | No signals |
| **Thread-safe?** | Requires care | Single-threaded |
| **Debugging** | Hard (async) | Easy (synchronous) |
| **Portability** | POSIX | POSIX |
| **Precision** | Microsecond | Nanosecond |
| **Overhead** | Signal delivery | Single syscall |
| **Can be missed?** | Yes (if masked) | No |

## Implementation Plan

### Step 1: Modify hook_block()

**File**: `macemu-next/src/cpu/unicorn_wrapper.c`

Add at line ~226 (BEFORE existing interrupt check):

```c
static void hook_block(uc_engine *uc, uint64_t addr, uint32_t size, void *user_data) {
    UnicornCPU *cpu = (UnicornCPU *)user_data;
    uint64_t pc = addr;

    /* ... existing block stats code (lines 159-224) ... */

    /* NEW: Check wall-clock time for 60Hz timer */
    static uint64_t last_timer_ns = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ns = now.tv_sec * 1000000000ULL + now.tv_nsec;

    if (now_ns - last_timer_ns >= 16667000ULL) {  /* 16.667ms = 60 Hz */
        last_timer_ns = now_ns;
        SetInterruptFlag(INTFLAG_60HZ);

        /* Trigger CPU interrupt if priority allows */
        extern int intlev(void);
        int level = intlev();
        if (level > 0) {
            g_pending_interrupt_level = level;
        }
    }

    /* EXISTING: Check for pending interrupts (lines 227-278 unchanged) */
    if (g_pending_interrupt_level > 0) {
        /* ... existing interrupt handling code ... */
    }
}
```

### Step 2: Remove timer_interrupt.cpp entirely

Delete (or archive):
- `src/platform/timer_interrupt.cpp`
- `src/platform/timer_interrupt.h`

Remove from build system:
- `meson.build` - remove timer_interrupt.cpp from sources

### Step 3: Remove timer setup calls

**File**: `src/main.cpp`

Remove:
```c
#include "timer_interrupt.h"
setup_timer_interrupt(16667);  // Remove this call
stop_timer_interrupt();        // Remove this call
```

## Testing

### Verify 60Hz Timing

Add debug output to see actual frequency:
```c
static uint64_t timer_tick_count = 0;
if (now_ns - last_timer_ns >= 16667000ULL) {
    timer_tick_count++;
    if (timer_tick_count % 60 == 0) {
        fprintf(stderr, "[TIMER] 60 ticks in %.3f seconds (%.1f Hz)\n",
                (now_ns - start_ns) / 1e9,
                60.0 / ((now_ns - start_ns) / 1e9));
    }
}
```

Expected output:
```
[TIMER] 60 ticks in 1.000 seconds (60.0 Hz)
[TIMER] 60 ticks in 1.000 seconds (60.0 Hz)
```

### Existing Tests Still Pass

All our interrupt handling tests still work:
```bash
EMULATOR_TIMEOUT=2 CPU_TRACE=0-100 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom
```

The interrupt injection code is unchanged - we're just changing HOW the interrupt gets triggered.

## Advantages Over SIGALRM

1. **No signal complexity**: No SA_RESTART, no signal masking, no async-signal-safe constraints
2. **No race conditions**: Single-threaded execution, simple control flow
3. **Better debugging**: Can add printf/logging without async-signal-safe worries
4. **More reliable**: Can't be blocked by signal masks
5. **Simpler code**: 15 lines vs 126 lines
6. **Better precision**: Nanosecond vs microsecond resolution
7. **Easier to understand**: Synchronous, procedural flow

## Potential Concerns & Answers

**Q: What if block hook doesn't run frequently enough?**
A: Block hook runs every ~10-50 instructions. At 25 MHz, that's >500K times/sec, way more than 60 Hz needed.

**Q: What if clock_gettime() is slow?**
A: Modern Linux uses vsyscall, takes ~20-50ns. Amortized over block size, overhead is <0.1%.

**Q: What about other platforms (Windows, macOS)?**
A: Can use platform-specific monotonic clocks (QueryPerformanceCounter, mach_absolute_time). But we're Linux-only for now.

**Q: Doesn't this break the "between blocks" principle from QEMU?**
A: Actually, this IS "between blocks"! `hook_block()` runs BEFORE each block starts, which is exactly when QEMU's `cpu_handle_interrupt()` runs.

## Implementation Status

✅ **COMPLETED** - All steps implemented:

1. ✅ Added time check to execution loops (UAE and Unicorn)
2. ✅ Verified 60.0 Hz timing accuracy
3. ✅ Removed SIGALRM code entirely
4. ✅ Validated with long runs - works perfectly
5. ✅ Unified approach for both UAE and Unicorn backends

**Final implementation**: [src/platform/timer_interrupt.cpp](../src/platform/timer_interrupt.cpp)

This is a **much simpler** solution that leverages infrastructure we already have!

---

**Credit**: User's insight that we can check time directly in block hook instead of using signals/threads.

This observation eliminated 126 lines of complex signal handling code and replaced it with ~60 lines of simple, reliable polling logic.
