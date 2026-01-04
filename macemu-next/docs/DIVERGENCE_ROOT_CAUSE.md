# Root Cause Analysis: UAE vs Unicorn Divergence

**Date**: 2026-01-04
**Status**: ROOT CAUSE IDENTIFIED
**Priority**: CRITICAL

## Executive Summary

Using enhanced trace analysis tools with side-by-side disassembly comparison, we have identified the exact root cause of divergence between UAE and Unicorn CPU backends.

**Root Cause**: Unicorn's interrupt handling is **skipping an instruction** when an interrupt is triggered.

## Evidence

### Event Summary (250k instruction trace)

- **UAE**:
  - Interrupts Triggered: 0 (no trigger logging in UAE currently)
  - Interrupts Taken: 2 (at instructions #175169, #199858)

- **Unicorn**:
  - Interrupts Triggered: 41 (every ~4000 instructions)
  - Interrupts Taken: 1 (at instruction #143944)

### First Divergence Point

**Location**: Instruction #3832
**Interrupt Trigger**: Instruction #3831 (Unicorn only)

#### Side-by-Side Execution:

```
[03831]
T1 (UAE):     02081134 2C2D | movel %a5@(-24),%d6  | SR: 2704
T2 (Unicorn): 02081134 2C2D | movel %a5@(-24),%d6  | SR: 2704
T2 (Unicorn): >>> INTERRUPT TRIGGERED: level=1 <<<

[03832]  <<<< DIVERGENCE STARTS HERE
T1 (UAE):     0208113A 102D | moveb %a5@(-13),%d0  | SR: 2704
T2 (Unicorn): 02081138 7000 | moveq #0,%d0         | SR: 2700
                              ^^^^^^^^ WRONG! This is the PREVIOUS instruction!
```

### What Happened

1. Both CPUs execute instruction #3831 identically
2. **Unicorn** receives interrupt trigger signal
3. At instruction #3832:
   - **UAE**: Executes PC=0x0208113A (correct next instruction)
   - **Unicorn**: Executes PC=0x02081138 (PREVIOUS instruction!)

4. **Unicorn skipped instruction at 0x02081138** completely!

### Cascade Effect

This single skipped instruction causes:
- D0 register to have wrong value (`00` instead of `05`)
- Jump table lookup to use wrong index
- Completely different code path taken
- Eventual crash at PC 0x02009B88

## Analysis

### Why This Happens

The interrupt logging shows the bug is in Unicorn's interrupt handling code at [unicorn_wrapper.c:226-278](../macemu-next/src/cpu/unicorn_wrapper.c#L226-L278).

When `hook_block()` detects a pending interrupt:

1. It calls `uc_emu_stop()` to stop emulation
2. **BUT**: The basic block has already started executing
3. Unicorn may have executed 1+ instructions before the hook fires
4. When we resume, we've lost sync with the instruction stream

### Block Boundary Issue

From the code comments:
```c
/* Check for pending interrupts (platform API) */
if (g_pending_interrupt_level > 0) {
    // Interrupt handling happens at BLOCK boundary
    // But we may have already executed instructions in this block!
```

## Solution Approaches

### Option 1: Don't Use `uc_emu_stop()`

Instead of stopping emulation, handle the interrupt inline:
- Read current PC from the block hook
- Build stack frame
- Update registers
- **Don't call `uc_emu_stop()`**

### Option 2: Track Block Instruction Count

- Count how many instructions were executed in the current block before interrupt
- When resuming, skip ahead by that count
- Complex and error-prone

### Option 3: Per-Instruction Hooks (Performance Hit)

- Use `UC_HOOK_CODE` instead of `UC_HOOK_BLOCK`
- Check interrupts on EVERY instruction
- Significant performance penalty (~10-50x slower)

### Option 4: Defer Interrupts to Next Block

- Don't take interrupt in current block
- Set flag for next block boundary
- Simpler but less accurate timing

## Recommended Fix

**Option 1** is recommended: Handle interrupts inline without `uc_emu_stop()`.

The interrupt handler already has all the information it needs:
- Current PC (from hook parameter)
- Current SR (read from register)
- Stack pointer (read from register)

We can build the exception stack frame and update PC directly without stopping emulation.

## Testing Plan

1. Implement Option 1 fix
2. Run traces with new code
3. Verify UAE and Unicorn stay in sync past instruction #3832
4. Run longer traces (1M+ instructions)
5. Boot test with Mac OS

## Files Affected

- [macemu-next/src/cpu/unicorn_wrapper.c](../macemu-next/src/cpu/unicorn_wrapper.c) - Fix interrupt handling
- [macemu-next/src/cpu/cpu_trace.c](../macemu-next/src/cpu/cpu_trace.c) - Interrupt logging (already improved)
- [trace_analyzer_v2.py](../trace_analyzer_v2.py) - Analysis tool (already improved)

## References

- Original interrupt implementation: commit c388b229
- Timer interrupt implementation: commit 4ee0ca6e
- Interrupt timing analysis: [docs/deepdive/InterruptTimingAnalysis.md](../macemu-next/docs/deepdive/InterruptTimingAnalysis.md)
