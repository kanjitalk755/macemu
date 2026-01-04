# Timer Refactor Plan: Unified Approach for UAE and Unicorn

## Current Architecture (SIGALRM-based)

```
SIGALRM Signal Handler (timer_interrupt.cpp)
    ↓
g_platform.cpu_trigger_interrupt(level)
    ↓
    ├─→ UAE: SPCFLAGS_SET(SPCFLAG_INT)
    │   └─→ do_specialties() checks flag in main loop
    │       └─→ Calls Interrupt(level) natively
    │
    └─→ Unicorn: g_pending_interrupt_level = level
        └─→ hook_block() checks flag before each block
            └─→ Manually builds stack frame, updates PC
```

Both backends share the **same SIGALRM timer**. Good for consistency, but has signal complexity.

## Proposed Architecture (Time-Check-based)

### Option 1: Time Check in Both Backend Loops

**For UAE** - Add to main CPU loop:
```c
// In uae_cpu_execute_one() or similar
static uint64_t last_timer_ns = 0;

// Check time every N instructions
if (instruction_count % 100 == 0) {  // Check every 100 insns
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ns = now.tv_sec * 1e9 + now.tv_nsec;

    if (now_ns - last_timer_ns >= 16667000) {  // 60 Hz
        last_timer_ns = now_ns;
        SetInterruptFlag(INTFLAG_60HZ);
        SPCFLAGS_SET(SPCFLAG_INT);  // UAE's way
    }
}
```

**For Unicorn** - Add to hook_block():
```c
// In hook_block()
static uint64_t last_timer_ns = 0;

struct timespec now;
clock_gettime(CLOCK_MONOTONIC, &now);
uint64_t now_ns = now.tv_sec * 1e9 + now.tv_nsec;

if (now_ns - last_timer_ns >= 16667000) {  // 60 Hz
    last_timer_ns = now_ns;
    SetInterruptFlag(INTFLAG_60HZ);
    g_pending_interrupt_level = intlev();  // Unicorn's way
}
```

**Pros:**
- Each backend handles its own timing
- No shared state between backends
- Simple, direct

**Cons:**
- Code duplication
- Two different `last_timer_ns` variables
- Harder to keep them in sync

### Option 2: Shared Timer Module (RECOMMENDED)

Keep timer logic separate, but replace SIGALRM with polling:

**New timer_interrupt.cpp:**
```c
// Shared timer state
static uint64_t last_timer_ns = 0;
static uint64_t interrupt_count = 0;

/**
 * Poll timer - call this from each backend's execution loop
 * Returns true if timer fired, false otherwise
 */
bool poll_timer_interrupt(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ns = now.tv_sec * 1000000000ULL + now.tv_nsec;

    if (now_ns - last_timer_ns >= 16667000ULL) {  // 60 Hz = 16.667ms
        last_timer_ns = now_ns;
        interrupt_count++;

        // Set Mac-level interrupt flag
        SetInterruptFlag(INTFLAG_60HZ);

        // Trigger CPU interrupt via platform API (works for both backends)
        extern Platform g_platform;
        if (g_platform.cpu_trigger_interrupt) {
            int level = intlev();
            if (level > 0) {
                g_platform.cpu_trigger_interrupt(level);
            }
        }

        return true;
    }

    return false;
}

void reset_timer_interrupt(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    last_timer_ns = now.tv_sec * 1000000000ULL + now.tv_nsec;
    interrupt_count = 0;
}

uint64_t get_timer_interrupt_count(void) {
    return interrupt_count;
}
```

**UAE integration** (in cpu_uae.c or main loop):
```c
int uae_cpu_execute_one(void) {
    // Every N instructions, poll timer
    static int poll_counter = 0;
    if (++poll_counter >= 100) {  // Check every 100 instructions
        poll_counter = 0;
        poll_timer_interrupt();  // May set SPCFLAG_INT
    }

    // Execute one instruction
    return do_cycles(1);
}
```

**Unicorn integration** (in unicorn_wrapper.c hook_block):
```c
static void hook_block(uc_engine *uc, uint64_t addr, uint32_t size, void *user_data) {
    // ... existing stats code ...

    // Poll timer (may set g_pending_interrupt_level)
    poll_timer_interrupt();

    // Existing interrupt check (unchanged)
    if (g_pending_interrupt_level > 0) {
        // ... handle interrupt ...
    }
}
```

**Pros:**
- ✅ Single source of truth for timer
- ✅ UAE unaffected (still uses SPCFLAG_INT)
- ✅ No signal complexity
- ✅ Easy to add debug logging in one place
- ✅ Consistent timing across backends

**Cons:**
- Still need to call from two places (but that's minimal)

## Detailed Implementation (Option 2)

### Step 1: Modify timer_interrupt.cpp

**File**: `src/platform/timer_interrupt.cpp`

Replace entire file with:
```c
/*
 *  timer_interrupt.cpp - 60Hz timer via polling
 *
 *  Checks wall-clock time to generate periodic interrupts.
 *  Called from CPU backend execution loops (UAE and Unicorn).
 */

#include "sysdeps.h"
#include "main.h"
#include "platform.h"
#include "timer_interrupt.h"
#include <time.h>
#include <stdio.h>

// Timer state
static uint64_t last_timer_ns = 0;
static uint64_t interrupt_count = 0;
static bool timer_initialized = false;

extern "C" {

/**
 * Initialize timer system
 */
void setup_timer_interrupt(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    last_timer_ns = now.tv_sec * 1000000000ULL + now.tv_nsec;
    interrupt_count = 0;
    timer_initialized = true;

    printf("Timer: Initialized 60 Hz timer (polling-based)\n");
}

/**
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
    extern Platform g_platform;
    if (g_platform.cpu_trigger_interrupt) {
        extern int intlev(void);  // From uae_wrapper.h
        int level = intlev();
        if (level > 0) {
            g_platform.cpu_trigger_interrupt(level);
        }
    }

    return 1;  // One expiration
}

/**
 * Stop timer
 */
void stop_timer_interrupt(void) {
    if (!timer_initialized) {
        return;
    }

    timer_initialized = false;
    printf("Timer: Stopped after %llu interrupts\n",
           (unsigned long long)interrupt_count);
}

/**
 * Get statistics
 */
uint64_t get_timer_interrupt_count(void) {
    return interrupt_count;
}

}  // extern "C"
```

### Step 2: Update timer_interrupt.h

**File**: `src/platform/timer_interrupt.h`

```c
#ifndef TIMER_INTERRUPT_H
#define TIMER_INTERRUPT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize timer system */
void setup_timer_interrupt(void);

/* Poll timer - call from CPU execution loop
 * Returns number of timer expirations (0 if not ready, 1 if fired) */
uint64_t poll_timer_interrupt(void);

/* Stop timer */
void stop_timer_interrupt(void);

/* Get statistics */
uint64_t get_timer_interrupt_count(void);

#ifdef __cplusplus
}
#endif

#endif /* TIMER_INTERRUPT_H */
```

### Step 3: Update Unicorn hook_block()

**File**: `src/cpu/unicorn_wrapper.c`

Add at line ~226 (before interrupt check):
```c
static void hook_block(uc_engine *uc, uint64_t addr, uint32_t size, void *user_data) {
    // ... existing block stats code ...

    /* Poll timer - may trigger interrupt */
    extern uint64_t poll_timer_interrupt(void);
    poll_timer_interrupt();

    /* Check for pending interrupts (UNCHANGED) */
    if (g_pending_interrupt_level > 0) {
        // ... existing interrupt handling code (lines 227-278) ...
    }
}
```

### Step 4: Update UAE main loop

**File**: `src/cpu/cpu_uae.c`

Find the `uae_cpu_execute_one()` or main loop function and add:
```c
int uae_cpu_execute_one(void) {
    /* Poll timer every 100 instructions */
    static int poll_counter = 0;
    if (++poll_counter >= 100) {
        poll_counter = 0;
        extern uint64_t poll_timer_interrupt(void);
        poll_timer_interrupt();  // May set SPCFLAG_INT
    }

    /* Execute one instruction (existing code) */
    return do_cycles(1);
}
```

Or if there's a better place in the UAE loop, use that. The key is to call `poll_timer_interrupt()` frequently (every 100 instructions or so).

### Step 5: Update main.cpp

**File**: `src/main.cpp`

Change from:
```c
setup_timer_interrupt(16667);  // Old SIGALRM version
```

To:
```c
setup_timer_interrupt();  // New polling version (no interval param needed)
```

## Testing

### Test Both Backends

**UAE:**
```bash
EMULATOR_TIMEOUT=5 CPU_BACKEND=uae ./build/macemu-next ~/quadra.rom
```

**Unicorn:**
```bash
EMULATOR_TIMEOUT=5 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom
```

**Expected**: Both should get ~300 timer interrupts (60 Hz × 5 seconds)

### Verify Timing

Add debug output to `poll_timer_interrupt()`:
```c
if (elapsed >= 16667000ULL) {
    static uint64_t first_ns = 0;
    if (interrupt_count == 0) {
        first_ns = now_ns;
    }

    if (interrupt_count % 60 == 0 && interrupt_count > 0) {
        double actual_hz = 60.0 / ((now_ns - first_ns) / 1e9);
        fprintf(stderr, "[TIMER] %llu ticks, actual rate: %.2f Hz\n",
                interrupt_count, actual_hz);
        first_ns = now_ns;  // Reset for next measurement
    }
}
```

Expected output:
```
[TIMER] 60 ticks, actual rate: 60.00 Hz
[TIMER] 120 ticks, actual rate: 60.01 Hz
```

## Performance Impact

### UAE
- Added: One function call every 100 instructions
- Overhead: ~1% (100 instructions at 25 MHz = 4μs, function call ~20ns)

### Unicorn
- Already calling hook_block() every block (~10-50 instructions)
- Added: One function call per block
- Overhead: Negligible (amortized over block size)

## Why This Preserves UAE

1. **Same API**: `g_platform.cpu_trigger_interrupt(level)` unchanged
2. **Same mechanism**: Still sets `SPCFLAG_INT`, checked by `do_specialties()`
3. **Same behavior**: Interrupt handled identically by UAE interpreter
4. **Only change**: Timer triggered by polling instead of SIGALRM

UAE doesn't care HOW the interrupt flag gets set, just that it gets set at 60 Hz.

## Rollback Plan

If issues arise:
1. Git revert to restore SIGALRM version
2. Both backends continue working with old timer
3. All other interrupt handling work is independent

## Summary

- ✅ Keeps UAE unchanged (same interrupt mechanism)
- ✅ Keeps Unicorn unchanged (same interrupt mechanism)
- ✅ Eliminates SIGALRM complexity
- ✅ Single timer implementation (DRY principle)
- ✅ Easy to debug (synchronous, add printfs freely)
- ✅ Consistent timing across backends

**Next Action**: Implement Step 1-5, test with both backends.
