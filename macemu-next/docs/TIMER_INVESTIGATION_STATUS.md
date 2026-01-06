# Timer Interrupt Investigation - Current Status

**Date:** January 5, 2026
**Issue:** Unicorn and UAE trace divergence due to timer interrupt differences

---

## Problem Statement

When running CPU traces (`run_traces.sh`), Unicorn and UAE diverge immediately at instruction #1:

- **UAE:** Executes 250,030 instructions, logs PC=0x0200008C (normal boot flow)
- **Unicorn:** Executes 19,000-20,000 instructions, logs PC=0x02000910 (interrupt handler!)
- **Root Cause:** Unicorn takes timer interrupts during early boot, UAE does not

### Trace Divergence Details

```
Instruction #0: Both execute PC=0x0200002A (JMP)
Instruction #1: DIVERGENCE
  - UAE:     PC=0x0200008C (continues boot)
  - Unicorn: PC=0x02000910 (interrupt handler)

Instruction #29: Unicorn takes interrupt (level 1, handler 0x02000910)
Instruction #31: Unicorn takes another interrupt
```

**Event Summary:**
- UAE: 0 interrupts triggered, 0 taken
- Unicorn: 599 interrupts triggered, 1-2 taken

---

## Investigation Timeline

### 1. Initial Discovery

**Symptom:** Unicorn trace shows fewer instructions than UAE, hit 10-second timeout

**Finding:** Unicorn was polling timer on EVERY block (avg 1.01 instructions), while UAE polls every 100 instructions

**Fix Applied:** Modified `unicorn_wrapper.c:hook_block()` to poll timer every 100 instructions
- File: `src/cpu/unicorn_wrapper.c:230-236`
- Commit: `af76af38`

### 2. BasiliskII Investigation

**Discovery:** Examined `BasiliskII/src/Unix/main_unix.cpp` to understand how timer works

**Key Finding:** BasiliskII suppresses 60Hz interrupts until Mac has booted!

```cpp
// main_unix.cpp:1485-1489
static void one_tick(...)
{
    // Trigger 60Hz interrupt
    if (ROMVersion != ROM_VERSION_CLASSIC || HasMacStarted()) {
        SetInterruptFlag(INTFLAG_60HZ);
        TriggerInterrupt();
    }
}
```

**HasMacStarted() implementation:**
```cpp
// include/macos_util.h:278-280
static inline bool HasMacStarted(void)
{
    return ReadMacInt32(0xcfc) == FOURCC('W','L','S','C');  // Mac warm start flag
}
```

**Reason:** During early boot, Mac ROM initializes interrupt vectors and critical state. Taking interrupts too early causes crashes or divergence.

### 3. First Fix Attempt: Add HasMacStarted() Check

**Change:** Modified `timer_interrupt.cpp` to check `HasMacStarted()` before triggering interrupts

```cpp
// src/drivers/platform/timer_interrupt.cpp:77-92
if (HasMacStarted() && !cpu_trace_is_enabled()) {
    extern Platform g_platform;
    if (g_platform.cpu_trigger_interrupt) {
        int level = intlev();
        if (level > 0) {
            g_platform.cpu_trigger_interrupt(level);
        }
    }
}
```

**Result:** Still diverging! Interrupts still triggered during trace.

**Hypothesis:** Memory at 0xCFC might contain 'WLSC' from RAM initialization or previous run, causing `HasMacStarted()` to return true prematurely.

### 4. Second Fix Attempt: Suppress During Tracing

**Rationale:** For deterministic trace comparison, timer interrupts should be completely disabled when `CPU_TRACE` is active.

**Changes Made:**
1. Added `cpu_trace_is_enabled()` helper function
   - File: `src/cpu/cpu_trace.h:32-33`
   - File: `src/cpu/cpu_trace.c:51-53`
   - Returns `g_trace.enabled` directly (not range-dependent like `cpu_trace_should_log()`)

2. Modified timer to check both `HasMacStarted()` AND `!cpu_trace_is_enabled()`
   - File: `src/drivers/platform/timer_interrupt.cpp:84`

**Result:** STILL NOT WORKING! Interrupts still triggered.

**Evidence:**
```
Trace 2 (Unicorn):
  Interrupts Triggered: 599
  Interrupts Taken:     2
  First Interrupts: [inst #29, level 1], [inst #31, level 1]
```

**Puzzling Observation:** Trace lines ARE being logged (tracing is active), but `cpu_trace_is_enabled()` appears to return false.

---

## Commits Made

### 1. Reorganization (Earlier Session)
- **Commit:** `8bd02ee1` - "Reorganize codebase: split webrtc, consolidate drivers"
- Split webrtc folder, moved encoders to drivers/, consolidated dummy drivers

### 2. Timer Polling Fix
- **Commit:** `af76af38` - "Fix Unicorn timer polling frequency to match UAE"
- Poll timer every 100 instructions instead of every block
- Tightened EmulOp detection range (0x7100-0x713F)

### 3. Timer Suppression (WIP)
- **Commit:** `5c0c3d2e` - "Add timer interrupt suppression during CPU tracing (WIP - not fully working)"
- Added `HasMacStarted()` check
- Added `cpu_trace_is_enabled()` helper
- **Status:** NOT working yet

---

## Current Code State

### Timer Interrupt Code (`src/drivers/platform/timer_interrupt.cpp`)

```cpp
uint64_t poll_timer_interrupt(void)
{
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

    // IMPORTANT: Only trigger interrupts after Mac has booted!
    // ALSO: Disable interrupts during CPU tracing for deterministic traces.
    if (HasMacStarted() && !cpu_trace_is_enabled()) {
        extern Platform g_platform;
        if (g_platform.cpu_trigger_interrupt) {
            int level = intlev();
            if (level > 0) {
                g_platform.cpu_trigger_interrupt(level);
            }
        }
    }

    return 1;  // One expiration
}
```

### Unicorn Hook Block (`src/cpu/unicorn_wrapper.c:230-236`)

```c
/* Poll timer every 100 instructions (same as UAE for timing consistency) */
static uint64_t total_instructions = 0;
total_instructions += insn_count;
if (total_instructions >= 100) {
    total_instructions = 0;
    poll_timer_interrupt();
}
```

### UAE Wrapper (`src/cpu/uae_wrapper.cpp:248-253`)

```cpp
/* Poll timer every 100 instructions */
static int poll_counter = 0;
if (++poll_counter >= 100) {
    poll_counter = 0;
    poll_timer_interrupt();  /* May set SPCFLAG_INT */
}
```

---

## Remaining Issues

### Issue #1: cpu_trace_is_enabled() Returns False During Active Tracing

**Evidence:**
- Trace lines ARE being logged: `[00000] 0200002A 4EFA | ...`
- Interrupts ARE being triggered: "599 interrupts triggered"
- Yet timer code is calling `cpu_trigger_interrupt()`, implying `cpu_trace_is_enabled()` returned false

**Possible Causes:**

1. **Initialization Order Problem**
   - `cpu_trace_init()` might be called AFTER `setup_timer_interrupt()`
   - Timer starts before trace state is initialized
   - Early timer polls see `g_trace.enabled = false`

2. **Thread Safety Issue**
   - Timer code runs in CPU execution context
   - Trace state set in initialization context
   - Memory visibility/ordering problem (unlikely but possible)

3. **Multiple Initialization**
   - UAE and Unicorn might call `cpu_trace_init()` separately
   - Unicorn's wrapper might not be seeing the initialized state
   - Static variable in `.c` file only visible within that compilation unit?

4. **Wrong Function Check**
   - `cpu_trace_is_enabled()` checks `g_trace.enabled`
   - But maybe we need a different check (environment variable directly?)

### Issue #2: HasMacStarted() Might Return True Prematurely

**Problem:** RAM at 0xCFC might already contain 'WLSC' from:
- Previous emulator run (if RAM not cleared)
- Random initialization
- ROM copying data during startup

**Evidence Needed:** Check actual value at 0xCFC during early boot

---

## Next Steps (Recommended)

### Option A: Debug cpu_trace_is_enabled()

1. Add debug logging to `poll_timer_interrupt()`:
   ```cpp
   static bool logged_once = false;
   if (!logged_once) {
       fprintf(stderr, "[TIMER] HasMacStarted=%d, cpu_trace_is_enabled=%d\n",
               HasMacStarted(), cpu_trace_is_enabled());
       logged_once = true;
   }
   ```

2. Check initialization order in `main.cpp`:
   - Where is `cpu_trace_init()` called?
   - Where is `setup_timer_interrupt()` called?
   - Which happens first?

3. Verify `g_trace.enabled` is actually being set:
   - Add logging to `cpu_trace_init()`
   - Confirm CPU_TRACE environment variable is parsed correctly

### Option B: Alternative Suppression Method

Instead of checking `cpu_trace_is_enabled()`, add explicit flag:

1. **Add global flag:**
   ```cpp
   // In timer_interrupt.cpp
   static bool g_suppress_interrupts_for_tracing = false;

   void set_suppress_timer_interrupts(bool suppress) {
       g_suppress_interrupts_for_tracing = suppress;
   }
   ```

2. **Call from trace runner:**
   - Have `run_traces.sh` set environment variable: `SUPPRESS_TIMER_INTERRUPTS=1`
   - Check this variable in `poll_timer_interrupt()`
   - Simpler, more explicit, no dependency on cpu_trace state

### Option C: Fix Initialization Order

Ensure `cpu_trace_init()` is called BEFORE timer starts:

1. Find where `setup_timer_interrupt()` is called in `main.cpp`
2. Ensure `cpu_trace_init()` is called first
3. Document the initialization order dependency

### Option D: Disable Timer in Trace Config

Modify `run_traces.sh` to pass a flag that disables the timer entirely:

```bash
CPU_TRACE=0-250000 DISABLE_TIMER=1 ./macemu-next ...
```

Then check `DISABLE_TIMER` in `poll_timer_interrupt()` before setting interrupt flag.

---

## Files Modified

### Core Changes
- `src/cpu/unicorn_wrapper.c` - Poll timer every 100 instructions (line 230-236)
- `src/drivers/platform/timer_interrupt.cpp` - Add HasMacStarted() and cpu_trace_is_enabled() checks (line 77-92)
- `src/cpu/cpu_trace.h` - Add cpu_trace_is_enabled() declaration (line 32-33)
- `src/cpu/cpu_trace.c` - Add cpu_trace_is_enabled() implementation (line 51-53)

### Build System (Earlier)
- `src/cpu/meson.build` - Add drivers/platform include path
- `src/drivers/meson.build` - Reorganize sources
- `meson.build` - Update subdirs and link_with

---

## Test Results

### Latest Test Run (After All Fixes)

```
Trace Statistics:
  UAE:     250,030 instructions (exit: 0)
  Unicorn:  20,616 instructions (exit: 0)
  DualCPU:      37 instructions (exit: 0)

Event Summary:
  Trace 1 (UAE):
    Interrupts Triggered: 0
    Interrupts Taken:     0
    EmulOps:              0

  Trace 2 (Unicorn):
    Interrupts Triggered: 599
    Interrupts Taken:     2
    EmulOps:              0

First Interrupts Taken:
  Trace 1: []
  Trace 2: [inst #29, level 1, handler 0x02000910]
           [inst #31, level 1, handler 0x02000910]
```

**Conclusion:** Divergence still occurs. Timer suppression is not working.

---

## References

### BasiliskII Timer Implementation
- `BasiliskII/src/Unix/main_unix.cpp:1447-1515` - 60Hz thread (tick_func)
- `BasiliskII/src/Unix/main_unix.cpp:1485-1489` - HasMacStarted() check
- `BasiliskII/src/include/macos_util.h:278-280` - HasMacStarted() definition
- `BasiliskII/src/timer.cpp` - Time Manager emulation (Mac OS software timers)

### macemu-next Timer Implementation
- `src/drivers/platform/timer_interrupt.cpp` - Polling-based 60Hz timer
- `src/cpu/uae_wrapper.cpp:248-253` - UAE timer polling (every 100 instructions)
- `src/cpu/unicorn_wrapper.c:230-236` - Unicorn timer polling (every 100 instructions)

### CPU Tracing
- `src/cpu/cpu_trace.h` - Tracing infrastructure API
- `src/cpu/cpu_trace.c` - Tracing state management
- Environment variable: `CPU_TRACE=start-end` enables tracing

---

## Open Questions

1. **Why does `cpu_trace_is_enabled()` return false during active tracing?**
   - Is there an initialization order issue?
   - Is the function checking the wrong state?

2. **What is the actual value at RAM address 0xCFC during early boot?**
   - Does it contain 'WLSC' prematurely?
   - Should we add additional checks beyond HasMacStarted()?

3. **Should timer interrupts be completely disabled during tracing?**
   - Or should we fix the timing so both backends take interrupts consistently?
   - What is the intended behavior for trace comparison?

4. **Why does Unicorn take 2 interrupts while UAE takes 0?**
   - Timing difference in when poll_timer_interrupt() is called?
   - Different execution speeds causing timer to fire at different points?
   - Is this a fundamental architectural difference that can't be fixed?

---

## Conclusion

The timer interrupt investigation has uncovered the root cause (timer firing during early boot) and identified the solution used by BasiliskII (`HasMacStarted()` check). However, the implementation of timer suppression during CPU tracing is not working as expected.

**Current Status:** WIP - Needs debugging of `cpu_trace_is_enabled()` or alternative suppression method.

**Recommended Next Action:** Debug initialization order and add logging to determine why `cpu_trace_is_enabled()` returns false during active tracing.
