# QEMU M68K Interrupt Handling Analysis

## Purpose
This document analyzes how QEMU properly handles M68K interrupts in the Quadra 800 platform, to understand the correct approach for fixing the Unicorn interrupt handling bug in macemu-next.

## Executive Summary

**The Problem**: Our current Unicorn implementation calls `uc_emu_stop()` in the `hook_block()` callback when an interrupt is pending. This causes instruction skipping because Unicorn stops mid-block and loses instruction stream synchronization.

**QEMU's Solution**: QEMU handles interrupts **between** translation blocks, not during block execution. The interrupt check happens in the main CPU execution loop AFTER each TB completes, ensuring no instructions are skipped.

## QEMU Architecture Overview

### Interrupt Flow in QEMU

```
Platform Device (VIA, SONIC, etc.)
    ↓
GLUE_set_irq() — routes interrupt to proper level
    ↓
m68k_set_irq_level() — sets pending_level & CPU_INTERRUPT_HARD flag
    ↓
cpu_exec_loop() — main execution loop
    ↓
cpu_handle_interrupt() — checks interrupt flags BETWEEN TBs
    ↓
m68k_cpu_exec_interrupt() — checks if pending_level > SR mask
    ↓
do_interrupt_m68k_hardirq() — builds stack frame, jumps to vector
```

### Key Files Analyzed

1. **`/tmp/qemu/hw/m68k/q800.c`** (lines 317-321)
   - Platform initialization
   - Connects GLUE device to CPU

2. **`/tmp/qemu/hw/m68k/q800-glue.c`** (lines 71-153)
   - `GLUE_set_irq()`: Routes device interrupts to M68K interrupt levels
   - Line 148: `m68k_set_irq_level(s->cpu, i + 1, i + 25)` — sets interrupt pending

3. **`/tmp/qemu/target/m68k/helper.c`** (lines 942-954)
   - `m68k_set_irq_level()`: Sets `env->pending_level` and raises `CPU_INTERRUPT_HARD` flag
   - Does NOT directly modify CPU state or stop execution

4. **`/tmp/qemu/target/m68k/op_helper.c`** (lines 519-536)
   - `m68k_cpu_exec_interrupt()`: Checks if interrupt should be taken
   - Compares `env->pending_level` vs `(SR & SR_I) >> SR_I_SHIFT`
   - If interrupt accepted: calls `do_interrupt_m68k_hardirq()`

5. **`/tmp/qemu/accel/tcg/cpu-exec.c`** (lines 934-944, 841)
   - `cpu_exec_loop()`: Main TB execution loop
   - Line 944: `while (!cpu_handle_interrupt(cpu, &last_tb))`
   - Line 841: Calls `tcg_ops->cpu_exec_interrupt()` between TBs

## Critical Difference: When Interrupts Are Checked

### QEMU (Correct Approach)

```c
// Simplified QEMU main loop (cpu-exec.c)
while (!cpu_handle_exception(cpu, &ret)) {
    TranslationBlock *last_tb = NULL;

    while (!cpu_handle_interrupt(cpu, &last_tb)) {  // ← CHECK HERE
        // Find/generate TB
        tb = tb_lookup(cpu, s);

        // Execute TB to completion
        tb = cpu_tb_exec(cpu, tb, &tb_exit);

        // TB finished, loop back to interrupt check
    }
}
```

**Key insight**: `cpu_handle_interrupt()` is called **between** TBs, ensuring:
- Every TB runs to completion
- No instructions are skipped
- PC is always at a valid instruction boundary

### Unicorn (Current Broken Approach)

```c
// Current macemu-next unicorn_wrapper.c (lines 226-278)
static void hook_block(uc_engine *uc, uint64_t addr, uint32_t size, void *user_data) {
    // ... middle of TB execution ...

    if (g_pending_interrupt_level > 0) {
        // Build stack frame
        // ...

        uc_emu_stop(uc);  // ← PROBLEM: Stops mid-block!
        return;
    }
}
```

**Problem**: `hook_block()` is called at the START of each block. When we call `uc_emu_stop()`:
1. Current block execution is aborted
2. Some instructions in the block may have already executed
3. PC might not align with instruction boundaries
4. Next `uc_emu_start()` resumes at wrong offset → **instruction skipping**

## Evidence from Trace Analysis

From our trace comparison (documented in `DIVERGENCE_ROOT_CAUSE.md`):

```
[03831]
T2: >>> INTERRUPT TRIGGERED: level=1 <<<

>>>> [03832]
T1: !0208113A 102D | moveb %a5@(-13),%d0  | D0=00000005
T2: !02081138 7000 | moveq #0,%d0         | D0=00000000
                                             ^^^^^^^^^^^^ WRONG!
```

At instruction #3832:
- **UAE (correct)**: PC=0x0208113A (next instruction after interrupt setup)
- **Unicorn (wrong)**: PC=0x02081138 (PREVIOUS instruction, off by 2 bytes!)

This confirms that `uc_emu_stop()` mid-block causes PC desynchronization.

## QEMU's Interrupt Check Logic

From `/tmp/qemu/target/m68k/op_helper.c:519-536`:

```c
bool m68k_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    CPUM68KState *env = cpu_env(cs);

    if (interrupt_request & CPU_INTERRUPT_HARD
        && ((env->sr & SR_I) >> SR_I_SHIFT) < env->pending_level) {
        /*
         * Real hardware gets the interrupt vector via an IACK cycle
         * at this point.  Current emulated hardware doesn't rely on
         * this, so we provide/save the vector when the interrupt is
         * first signalled.
         */
        cs->exception_index = env->pending_vector;
        do_interrupt_m68k_hardirq(env);
        return true;
    }
    return false;
}
```

**Key properties**:
1. Only called when `CPU_INTERRUPT_HARD` flag is set (by `m68k_set_irq_level()`)
2. Checks SR interrupt mask vs pending level
3. If accepted: builds stack frame, updates SR, jumps to vector
4. Returns `true` → causes `last_tb = NULL` in main loop → prevents TB chaining
5. Next iteration starts fresh at interrupt handler PC

## Unicorn API Limitations

After studying QEMU, the fundamental issue is that **Unicorn does not expose the TCG main loop**.

### What Unicorn Provides
- `uc_emu_start(address, until, timeout, count)` — execute until condition
- Hooks: `UC_HOOK_BLOCK`, `UC_HOOK_CODE`, `UC_HOOK_INTR`
- `uc_emu_stop()` — abort current execution

### What Unicorn Does NOT Provide
- Access to `cpu_handle_interrupt()` equivalent
- Ability to insert logic between TBs
- Control over the TB execution loop

## Proposed Fix Strategy

Since we cannot replicate QEMU's "check between TBs" approach in Unicorn, we need a different strategy:

### Option 1: Use `UC_HOOK_CODE` with Single-Step when Interrupt Pending (REJECTED)

```c
// When interrupt becomes pending:
1. Set single-step mode in Unicorn
2. Use UC_HOOK_CODE to check after EVERY instruction
3. When interrupt accepted, handle and disable single-step
```

**Pros**: Guarantees instruction-level granularity
**Cons**: Severe performance penalty (100-1000x slower)

### Option 2: Defer Interrupt to Next `uc_emu_start()` Call (RECOMMENDED)

```c
// In hook_block():
if (g_pending_interrupt_level > 0) {
    // DON'T call uc_emu_stop() here!
    // Set a flag and return
    g_interrupt_pending_deferred = true;
    return;  // Let block finish
}

// In main CPU loop (unicorn_cpu.cpp):
while (running) {
    uc_err err = uc_emu_start(uc, pc, 0xFFFFFFFF, 0, 1);  // ← Execute 1 instruction

    // Check for interrupt BETWEEN instructions
    if (g_interrupt_pending_deferred) {
        handle_interrupt();
        g_interrupt_pending_deferred = false;
    }

    uc_reg_read(uc, UC_M68K_REG_PC, &pc);
}
```

**Pros**:
- Mimics QEMU's "check between TBs" approach
- No instruction skipping
- Reasonable performance (each `uc_emu_start()` can still execute multi-insn TBs)

**Cons**:
- Requires refactoring main CPU loop
- More complex control flow

### Option 3: Use `UC_HOOK_INTR` (Unicorn's Official Mechanism) — INVESTIGATE

Unicorn has a `UC_HOOK_INTR` hook specifically for interrupts. We should investigate if this provides the TB-boundary semantics we need.

```c
static void hook_intr(uc_engine *uc, uint32_t intno, void *user_data) {
    // Called when interrupt is about to be taken
    // Check if this is called at TB boundaries
}

uc_hook_add(uc, &hook_intr_handle, UC_HOOK_INTR, hook_intr, cpu, 1, 0);
```

**Action Required**: Test if `UC_HOOK_INTR` is called at safe instruction boundaries.

## Comparison Table

| Aspect | QEMU | Current Unicorn | Proposed Fix |
|--------|------|-----------------|--------------|
| Interrupt check timing | Between TBs | Mid-block | Between `uc_emu_start()` calls |
| Instruction skipping | Never | Yes (bug) | Never |
| Performance | Fast | Fast | Moderate (more `uc_emu_start()` overhead) |
| Code complexity | High (full platform) | Low (hooks only) | Medium (custom loop) |
| Uses `emu_stop()` | No | Yes (broken) | No |

## Recommended Next Steps

1. **Investigate `UC_HOOK_INTR`**: Test if it provides TB-boundary safety
2. **Prototype Option 2**: Implement deferred interrupt with custom loop
3. **Benchmark**: Compare performance of different approaches
4. **Validate**: Run extended traces (1M+ instructions) to confirm no divergence

## Key Takeaway

**QEMU's interrupt handling works because it NEVER interrupts a translation block mid-execution.** All interrupt checks happen at clean boundaries (between TBs), ensuring PC is always at a valid instruction start.

Our fix must replicate this property, even though Unicorn's API doesn't directly support it. The most promising approach is to avoid `uc_emu_stop()` entirely and instead use a custom execution loop with deferred interrupt handling.

---

**References**:
- QEMU source: `/tmp/qemu/`
- Current broken implementation: `macemu-next/src/cpu/unicorn_wrapper.c:226-278`
- Trace evidence: `macemu-next/docs/DIVERGENCE_ROOT_CAUSE.md`
