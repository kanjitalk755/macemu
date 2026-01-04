# Timer Investigation Results

## Summary

Investigated implementing a 60 Hz timer interrupt for macemu-next. Successfully implemented a working `timerfd`-based timer, but discovered it causes **execution divergence** between backends, leading to crashes.

## What We Learned

### 1. Signal-Based Timers Don't Work (SIGALRM)

**Problem:** The emulator uses extensive signal masking for other features.

```bash
# strace shows signals being blocked:
rt_sigprocmask(SIG_BLOCK, [HUP INT QUIT ALRM TERM CHLD], [], 8)
```

**Result:**
- Timer signals queued but handlers rarely executed
- Only ~0.2-4 Hz delivery instead of 60 Hz
- Incompatible with emulator's signal-based architecture

### 2. File Descriptor Timers Work (timerfd_create)

**Implementation:**
```cpp
// Create timerfd
int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);

// Configure for 60 Hz
struct itimerspec spec;
spec.it_value.tv_nsec = 16667000;      // 16.667ms
spec.it_interval.tv_nsec = 16667000;   // Periodic

// Poll from Unicorn block hook (every basic block)
uint64_t expirations;
read(timer_fd, &expirations, sizeof(expirations));
SetInterruptFlag(INTFLAG_60HZ);
PendingInterrupt = true;
```

**Result:**
- ✅ Timer fires correctly at 60.0 Hz (verified with timestamps)
- ✅ No signal handling conflicts
- ✅ Polled from block hook (every ~10-50 instructions)

**Timestamp verification:**
```
[TIMER] Tick 1 at 794838.141
[TIMER] Tick 2 at 794838.158 (+17ms)
[TIMER] Tick 3 at 794838.174 (+16ms)
[TIMER] Tick 4 at 794838.191 (+17ms)
[TIMER] Tick 5 at 794838.208 (+17ms)
Average: 16.67ms = 60.0 Hz ✅
```

### 3. Timer Causes Execution Divergence

**Problem:** Timer interrupts cause non-deterministic execution.

**Evidence from trace comparison:**

**First divergence at instruction 29,518:**
```
PC: 0x0200CCB0, Instruction: 21C0 (MOVE.L D0, (An))

D0 value:
- UAE (no timer):        0x14700000
- Unicorn (with timer):  0x34300000
```

The same instruction at the same PC produces **different register values** due to timing.

**Cascade effect:**
1. Divergence at instruction ~29,518
2. Different code paths taken
3. At instruction 143,251, Unicorn at wrong PC (`0x0200A29A`)
4. Tries to execute `0x7129` (data, not code)
5. Crashes: "Unhandled EmulOp 7129"

**Instruction counts when stopped:**
```
UAE:      250,000 instructions (completed trace)
Unicorn:  143,259 instructions (crashed)
DualCPU:  142,514 instructions (similar crash)
```

### 4. Why Timer Causes Divergence

**Theory:** The timer affects timing-sensitive code in the ROM.

Possible mechanisms:
1. **Interrupt timing:** ROM code may be polling or waiting for specific timing
2. **Memory-mapped I/O:** Timer sets `INTFLAG_60HZ` which modifies shared memory
3. **Race conditions:** Timer fires during critical sections
4. **Instruction count dependencies:** Code that depends on precise instruction timing

The immediate value loaded by `MOVE.L #imm,D0` is different, suggesting:
- Memory contents modified by interrupt handler, OR
- PC calculation affected by interrupt processing, OR
- Timing-dependent code execution

### 5. UAE vs Unicorn Behavior

**UAE (no timer polling):**
- Executes deterministically
- Completes 250k instruction trace
- Stable, repeatable execution

**Unicorn (with timer polling):**
- Non-deterministic execution
- Diverges from UAE at instruction ~29,518
- Crashes at instruction 143,259
- Different execution each run (timer-dependent)

**DualCPU (not checked):**
- Similar crash pattern to Unicorn
- Suggests timer not the only issue

## Files Modified During Investigation

### Created:
- `src/platform/timer_interrupt.cpp` - timerfd implementation
- `src/platform/timer_interrupt.h` - Timer API
- `docs/TIMER_IMPLEMENTATION_FINAL.md` - Documentation
- `docs/TIMER_IMPLEMENTATION_COMPARISON.md` - Comparison docs

### Modified:
- `src/cpu/unicorn_wrapper.c` - Added `poll_timer_interrupt()` call in block hook
- `src/main.cpp` - Added timer setup/teardown, main loop polling
- `src/drivers/meson.build` - Added platform sources
- `meson.build` - Added platform include directory

## Conclusions

### What Works:
1. ✅ timerfd-based timer fires accurately at 60 Hz
2. ✅ Non-blocking polling from block hook
3. ✅ No signal handling conflicts
4. ✅ Technically correct implementation

### What Doesn't Work:
1. ❌ Timer causes execution divergence
2. ❌ Non-deterministic behavior
3. ❌ Unicorn crashes after ~143k instructions
4. ❌ Cannot trace/debug with timer active

### Why This Matters:

**The emulator needs deterministic execution for:**
- Debugging (comparing UAE vs Unicorn traces)
- Testing (reproducible results)
- Validation (dual-CPU verification)

**Timer breaks determinism** because:
- Interrupts fire asynchronously
- Timing affects execution path
- Different backends process interrupts differently

## Recommendations

### Option 1: Defer Timer Implementation
- Keep UAE working as stable reference
- Fix Unicorn execution issues first
- Add timer later when execution is stable
- **This is the current choice**

### Option 2: Make Timer Optional
- Add `ENABLE_TIMER` environment variable
- Disable for debugging/tracing
- Enable only when needed for Mac OS boot

### Option 3: Synchronize Timer Across Backends
- Make timer fire at specific instruction counts (not time-based)
- Ensures deterministic interrupt delivery
- More complex implementation

### Option 4: Fix Root Cause
- Investigate why timer causes divergence
- May be ROM code expects no interrupts during init
- May need to defer timer until Mac OS is booted

## Current Status

**REVERTED** - Timer implementation removed to restore stable UAE execution.

**Reason:** Need deterministic execution for debugging. Timer will be re-added after:
1. Unicorn execution stabilized
2. UAE vs Unicorn traces match
3. Understanding of ROM timing requirements

## Technical Details Preserved

### Timer Implementation (for future reference)

**Setup:**
```cpp
timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
struct itimerspec spec = {
    .it_value = {0, 16667000},     // Initial: 16.667ms
    .it_interval = {0, 16667000}   // Period: 16.667ms (60 Hz)
};
timerfd_settime(timer_fd, 0, &spec, NULL);
```

**Polling:**
```cpp
// From unicorn_wrapper.c hook_block()
uint64_t expirations;
if (read(timer_fd, &expirations, sizeof(expirations)) == sizeof(expirations)) {
    SetInterruptFlag(INTFLAG_60HZ);
    PendingInterrupt = true;
}
```

**Integration points:**
- `main.cpp:421` - Timer setup after CPU init
- `unicorn_wrapper.c:222` - Poll from block hook
- `main.cpp:447` - Poll from main loop (belt-and-suspenders)

## Lessons Learned

1. **Timing matters:** Emulators need careful timing control
2. **Test with tracing:** Always compare execution with/without new features
3. **Determinism first:** Get stable execution before adding timing
4. **Signal masking is real:** Can't use SIGALRM in this emulator
5. **timerfd works well:** Good alternative to signals when it's time to re-add

## References

- Initial timer plan: `docs/phase2-implementation-plan.md`
- Trace comparison script: `compare_traces.sh`
- Run traces script: `run_traces.sh`
- Original BasiliskII timer: `BasiliskII/src/Unix/main_unix.cpp:1041-1506`
