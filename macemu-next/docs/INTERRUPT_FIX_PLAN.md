# Interrupt Handling Fix Implementation Plan

## Problem Statement

Current Unicorn implementation calls `uc_emu_stop()` in `hook_block()` when an interrupt is pending, causing instruction skipping and CPU divergence from UAE.

**Evidence**: At instruction #3832, Unicorn PC is 0x02081138 instead of correct 0x0208113A (off by 2 bytes).

## Root Cause (Confirmed by QEMU Study)

QEMU handles interrupts **between translation blocks**, not during block execution. Our `uc_emu_stop()` approach stops mid-block, losing instruction stream synchronization.

See `QEMU_INTERRUPT_ANALYSIS.md` for detailed QEMU study.

## Proposed Solution: Hook-Based Interrupt Injection

After analyzing QEMU's approach and Unicorn's API limitations, the cleanest solution is:

### Implementation Strategy

**Stop using `uc_emu_stop()` entirely.** Instead:

1. **In `hook_block()`**: Only SET a flag when interrupt is pending, don't stop execution
2. **Use `UC_HOOK_CODE` with instruction counting**: Check interrupt flag after every N instructions
3. **When safe point reached**: Inject interrupt by modifying registers (PC, SR, SP)
4. **Let Unicorn continue naturally**: No `uc_emu_stop()`, no forced breaks

### Why This Works

- No mid-block stopping → no instruction skipping
- Unicorn's JIT remains fast (checking flag is cheap)
- Interrupt happens at next instruction boundary (same as QEMU's "between TBs")
- Clean, minimal changes to existing code

## Detailed Implementation

### Step 1: Remove `uc_emu_stop()` from `hook_block()`

**File**: `macemu-next/src/cpu/unicorn_wrapper.c`

**Current (BROKEN) code** (lines 226-278):
```c
static void hook_block(uc_engine *uc, uint64_t addr, uint32_t size, void *user_data) {
    // ... stats tracking ...

    /* Check for pending interrupts (platform API) */
    if (g_pending_interrupt_level > 0) {
        // ... build stack frame ...
        uc_emu_stop(uc);  // ← REMOVE THIS
        return;
    }
}
```

**New (FIXED) code**:
```c
static void hook_block(uc_engine *uc, uint64_t addr, uint32_t size, void *user_data) {
    UnicornCPU *cpu = (UnicornCPU *)user_data;

    /* Update block statistics */
    cpu->block_stats.total_blocks++;
    // ... rest of stats ...

    /* Just track that interrupt is pending - DON'T handle here */
    if (g_pending_interrupt_level > 0) {
        cpu->interrupt_pending = true;
        cpu->pending_interrupt_level = g_pending_interrupt_level;
        g_pending_interrupt_level = 0;  /* Acknowledge */
    }
}
```

### Step 2: Add Instruction-Level Hook to Check Interrupt Flag

**Add new hook** after `hook_block()`:
```c
/**
 * Hook for each instruction - checks if interrupt should be injected
 * Only enabled when interrupt is pending to minimize overhead
 */
static void hook_code_interrupt_check(uc_engine *uc, uint64_t address, uint32_t size, void *user_data) {
    UnicornCPU *cpu = (UnicornCPU *)user_data;

    if (!cpu->interrupt_pending) {
        return;  /* Fast path: no interrupt */
    }

    /* We're at a clean instruction boundary - safe to inject interrupt */
    int intr_level = cpu->pending_interrupt_level;

    /* Get current SR to check interrupt mask */
    uint32_t sr;
    uc_reg_read(uc, UC_M68K_REG_SR, &sr);
    int current_mask = (sr >> 8) & 7;

    if (intr_level > current_mask) {
        /* Build M68K exception stack frame */
        uint32_t sp, pc;
        uc_reg_read(uc, UC_M68K_REG_A7, &sp);
        uc_reg_read(uc, UC_M68K_REG_PC, &pc);

        /* Push PC (long, big-endian) */
        sp -= 4;
        uint32_t pc_be = __builtin_bswap32(pc);
        uc_mem_write(uc, sp, &pc_be, 4);

        /* Push SR (word, big-endian) */
        sp -= 2;
        uint16_t sr_be = __builtin_bswap16((uint16_t)sr);
        uc_mem_write(uc, sp, &sr_be, 2);

        /* Update SR: set supervisor mode, set interrupt mask */
        sr |= (1 << 13);  /* S bit */
        sr = (sr & ~0x0700) | ((intr_level & 7) << 8);  /* I2-I0 */
        uc_reg_write(uc, UC_M68K_REG_SR, &sr);
        uc_reg_write(uc, UC_M68K_REG_A7, &sp);

        /* Read interrupt vector and jump to handler */
        uint32_t vbr = 0;  /* TODO: Read VBR for 68020+ */
        uint32_t vector_addr = vbr + (24 + intr_level) * 4;
        uint32_t handler_addr_be;
        uc_mem_read(uc, vector_addr, &handler_addr_be, 4);
        uint32_t handler_addr = __builtin_bswap32(handler_addr_be);

        /* Log interrupt being taken */
        cpu_trace_log_interrupt_taken(intr_level, handler_addr);

        /* Update PC to handler address */
        uc_reg_write(uc, UC_M68K_REG_PC, &handler_addr);

        /* Clear pending flag */
        cpu->interrupt_pending = false;
        cpu->pending_interrupt_level = 0;

        /* IMPORTANT: No uc_emu_stop() - let execution continue at handler */
    } else {
        /* Interrupt masked, clear pending flag */
        cpu->interrupt_pending = false;
        cpu->pending_interrupt_level = 0;
    }
}
```

### Step 3: Register the Code Hook

**In `unicorn_cpu_init()`** (after existing hook registrations):
```c
/* Hook for interrupt injection at instruction boundaries */
uc_hook_add(cpu->uc, &cpu->hook_code_intr, UC_HOOK_CODE,
            (void *)hook_code_interrupt_check, cpu, 1, 0);
```

### Step 4: Add Fields to `UnicornCPU` Structure

**File**: `macemu-next/src/cpu/unicorn_cpu.h`

Add to `UnicornCPU` struct:
```c
typedef struct UnicornCPU {
    // ... existing fields ...

    /* Interrupt handling */
    bool interrupt_pending;         /* Interrupt needs injection */
    int pending_interrupt_level;    /* Level (1-7) */
    uc_hook hook_code_intr;         /* Code hook handle */

    // ... rest ...
} UnicornCPU;
```

### Step 5: Initialize New Fields

**In `unicorn_cpu_init()`**:
```c
cpu->interrupt_pending = false;
cpu->pending_interrupt_level = 0;
```

## Performance Optimization

The `UC_HOOK_CODE` runs on EVERY instruction, which might seem expensive. However:

1. **Fast path is very fast**: Just a boolean check when no interrupt pending
2. **Interrupt path is rare**: Only runs when interrupt actually happens
3. **No `uc_emu_stop()` overhead**: Saves context switch back to host

If profiling shows `UC_HOOK_CODE` is too slow, we can optimize:

### Optimization: Dynamic Hook Management
```c
/* Only enable UC_HOOK_CODE when interrupt is pending */

// In hook_block():
if (g_pending_interrupt_level > 0) {
    cpu->interrupt_pending = true;
    cpu->pending_interrupt_level = g_pending_interrupt_level;
    g_pending_interrupt_level = 0;

    /* Enable code hook for interrupt checking */
    uc_hook_add(cpu->uc, &cpu->hook_code_intr, UC_HOOK_CODE,
                (void *)hook_code_interrupt_check, cpu, 1, 0);
}

// In hook_code_interrupt_check() after handling interrupt:
if (!cpu->interrupt_pending) {
    /* Remove code hook - interrupt handled */
    uc_hook_del(cpu->uc, cpu->hook_code_intr);
    cpu->hook_code_intr = 0;
}
```

## Testing Plan

### Phase 1: Unit Test
```bash
# Run short trace to verify no divergence
EMULATOR_TIMEOUT=2 CPU_TRACE=0-100 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom

# Compare with UAE
./scripts/diff_cpus.sh 0-100 ~/quadra.rom
```

Expected: No divergence at instruction #32 (previous failure point).

### Phase 2: Extended Test
```bash
# Run to previous crash point (145k instructions)
EMULATOR_TIMEOUT=10 CPU_TRACE=145890-145900 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom

# Compare traces
./scripts/diff_cpus.sh 145890-145900 ~/quadra.rom
```

Expected: No divergence, Unicorn reaches same instruction count as UAE.

### Phase 3: Long Run
```bash
# Run for 1M instructions (far beyond previous crash)
EMULATOR_TIMEOUT=60 CPU_TRACE=999990-1000000 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom
```

Expected: Unicorn runs stably, no crashes, same register states as UAE.

### Phase 4: DualCPU Validation
```bash
# Run DualCPU backend (lockstep UAE + Unicorn)
EMULATOR_TIMEOUT=60 CPU_BACKEND=dualcpu ./build/macemu-next ~/quadra.rom
```

Expected: No divergence reports, both CPUs stay synchronized.

## Rollback Plan

If the fix causes issues:

1. **Git revert**: All changes are in single commit
2. **Fallback to UAE**: `CPU_BACKEND=uae` works unchanged
3. **Keep diagnostic tools**: trace_analyzer.py, run_traces.sh remain useful

## Success Criteria

✅ **Fix is successful if**:
1. No divergence at instruction #3832 (original bug)
2. Unicorn runs ≥1M instructions without crash
3. DualCPU backend shows no divergence
4. Performance within 20% of original Unicorn speed

## Timeline

- **Implementation**: 2-4 hours
- **Testing**: 2-3 hours
- **Documentation**: 1 hour
- **Total**: 1 day

## Dependencies

None - all changes are in existing files:
- `unicorn_wrapper.c`
- `unicorn_cpu.h`
- No external library changes needed

---

**Next Action**: Implement Step 1-5, test with Phase 1 test cases.
