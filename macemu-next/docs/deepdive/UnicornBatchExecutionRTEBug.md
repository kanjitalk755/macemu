# Unicorn Batch Execution RTE Bug

**Date**: January 4, 2026
**Status**: ❌ BLOCKED - Cannot use batch execution
**Impact**: 50-200% performance loss vs optimal batching
**Priority**: HIGH

---

## Problem Summary

Batch execution (`unicorn_execute_n(cpu, 10000)`) causes an **infinite RTE (Return from Exception) loop** in Unicorn Engine. We must use single-step execution (`unicorn_execute_one(cpu)`, which internally uses `count=1`) despite significant performance cost.

---

## The Bug

### Symptom

When using `uc_emu_start(pc, end, 0, 10000)` to execute 10,000 instructions at once:

```
Instruction [00052]: PC=0x02009B88, Opcode=0x4E73 (RTE)
Instruction [00053]: PC=0x02009B88, Opcode=0x4E73 (RTE)  ← Same PC!
Instruction [00054]: PC=0x02009B88, Opcode=0x4E73 (RTE)  ← Stuck!
...
Instruction [14476]: PC=0x02009B88, Opcode=0x4E73 (RTE) ← Still stuck!
```

RTE executes 14,424+ times at the same address until timeout. PC never advances.

### Root Cause

**Unicorn's hook execution order** prevents proper RTE handling during batch execution.

#### Execution Order in Unicorn

```
1. RTE instruction (0x4E73) → EXCP_RTE exception raised
2. cpu_handle_exception() called
3. UC_HOOK_INTR fires ← Hook runs HERE, PC still at RTE address!
4. Hook returns → exception_index cleared
5. m68k_interrupt_all() called
6. m68k_rte() executes ← PC updated from stack HERE
7. Execution continues with new PC
```

**The Problem**: Hook fires at step 3, but PC isn't updated until step 6!

#### Why `uc_emu_stop()` Doesn't Fix It

Even if we call `uc_emu_stop()` in the hook:

```c
static void hook_interrupt(uc_engine *uc, uint32_t intno, void *user_data) {
    if (intno == 0x100) {  // EXCP_RTE
        uc_emu_stop(uc);  // Try to stop execution
    }
}
```

**Result**: Still loops infinitely!

**Why**:
- Hook fires with PC = 0x02009B88 (RTE instruction address)
- `uc_emu_stop()` stops emulation immediately
- But `m68k_rte()` hasn't run yet - **PC is NOT updated**
- Next `uc_emu_start()` call fetches from same PC → infinite loop

#### Evidence from Debugging

```
[RTE_HOOK #0] intno=0x100, PC after RTE=0x02009B88, calling uc_emu_stop()
[RTE_HOOK #1] intno=0x100, PC after RTE=0x02009B88, calling uc_emu_stop()
[RTE_HOOK #2] intno=0x100, PC after RTE=0x02009B88, calling uc_emu_stop()
...
```

PC stays at 0x02009B88 (the RTE instruction itself), not the return address.

---

## Why Single-Step Works

With `uc_emu_start(pc, end, 0, 1)`:

1. Each call executes **at most 1 instruction**
2. Call returns **after full completion** (through all 7 steps above)
3. `m68k_rte()` completes and commits PC update (step 6)
4. Next `uc_emu_start()` fetches from **updated PC** ✅

**Key difference**: Full completion boundary ensures state changes commit.

---

## Performance Impact

### Real Statistics (5-second run)

From actual execution:
- **Total instructions**: 4,014,104
- **Total blocks executed**: 254,337
- **Average block size**: 15.78 instructions
- **Function calls with count=1**: 802,821 calls/sec

### Overhead Analysis

#### Current (count=1)

```
Function calls per instruction: 1
Overhead per call: ~30-50 CPU cycles
Total overhead: 0.8-1.3% of CPU time
Plus: context switching, counter updates, hook processing
```

#### Potential with Batching (count=10000)

```
Function calls per 10k instructions: 1
Overhead reduction: 10,000x fewer calls
TB chaining: ~630 blocks could execute without returning (10000/15.78)
```

### Estimated Speedup

**Conservative**: 1.5-2x faster with batching
**Optimistic**: 2-3x faster with batching

**Current sacrifice**: We accept 33-67% slower execution for correctness.

---

## QEMU/Unicorn Source Code Evidence

### translate.c (QEMU M68K)

```c
// Line 4849
DISAS_INSN(rte)
{
    if (IS_USER(s)) {
        gen_exception(s, s->base.pc_next, EXCP_PRIVILEGE);
        return;
    }
    gen_exception(s, s->base.pc_next, EXCP_RTE);
}

// Line 312
static void gen_exception(DisasContext *s, uint32_t dest, int nr)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    update_cc_op(s);
    tcg_gen_movi_i32(tcg_ctx, QREG_PC, dest);
    gen_raise_exception(tcg_ctx, nr);
    s->base.is_jmp = DISAS_NORETURN;  // ← TB ends here
}
```

**Note**: `DISAS_NORETURN` tells QEMU to end the translation block. This is correct behavior, but doesn't help with the hook ordering issue.

### op_helper.c (QEMU M68K)

```c
// Line 40
static void m68k_rte(CPUM68KState *env)
{
    uint32_t sp;
    uint16_t fmt;
    uint16_t sr;

    sp = env->aregs[7];
throwaway:
    sr = cpu_lduw_mmuidx_ra(env, sp, MMU_KERNEL_IDX, 0);
    sp += 2;
    env->pc = cpu_ldl_mmuidx_ra(env, sp, MMU_KERNEL_IDX, 0);  // ← PC updated HERE
    sp += 4;
    // ... handle exception frame formats ...
    env->aregs[7] = sp;
    cpu_m68k_set_sr(env, sr);
}

// Line 182 (called from m68k_interrupt_all)
case EXCP_RTE:
    /* Return from an exception.  */
    m68k_rte(env);
    return;
```

**Critical**: `m68k_rte()` is where the PC actually gets updated from the stack.

### cpu-exec.c (Unicorn Exception Handling)

```c
static inline bool cpu_handle_exception(CPUState *cpu, int *ret)
{
    bool catched = false;
    struct uc_struct *uc = cpu->uc;
    struct hook *hook;

    // ... other exception handling ...

    // Unicorn: call registered interrupt callbacks
    catched = false;
    HOOK_FOREACH(uc, hook, UC_HOOK_INTR) {
        ((uc_cb_hookintr_t)hook->callback)(uc, cpu->exception_index, hook->user_data);
        catched = true;
    }

    // ← Hook called HERE, BEFORE m68k_interrupt_all()

    if (!catched) {
        uc->invalid_error = UC_ERR_EXCEPTION;
        cpu->halted = 1;
        *ret = EXCP_HLT;
        return true;
    }

    cpu->exception_index = -1;  // Clear exception
}
// Later: m68k_interrupt_all() called, which calls m68k_rte()
```

**Hook ordering confirmed**: UC_HOOK_INTR fires before the RTE handler executes.

---

## Attempted Fixes (All Failed)

### Attempt 1: Call `uc_emu_stop()` in Hook

**Code**:
```c
static void hook_interrupt(uc_engine *uc, uint32_t intno, void *user_data) {
    if (intno == 0x100) {  // EXCP_RTE
        uc_emu_stop(uc);
    }
}
```

**Result**: ❌ Still loops infinitely
**Why**: PC not updated yet when hook fires

### Attempt 2: Manually Read PC from Stack

**Idea**: Read return PC from stack in hook, update PC register manually

**Problem**: Race condition - `m68k_rte()` will also update PC
**Risk**: Double PC update, corrupted state

**Not attempted** due to high risk of additional bugs.

### Attempt 3: Patch Unicorn Source

**Idea**: Modify Unicorn to call hook AFTER `m68k_rte()` completes

**Blocker**:
- Requires maintaining Unicorn fork
- Affects all exception types, not just RTE
- Complex QEMU internals

**Not attempted** - too invasive.

---

## Current Solution

### Implementation

Use `unicorn_execute_one()` which calls:

```c
uc_emu_start(cpu->uc, pc, 0xFFFFFFFFFFFFFFFFULL, 0, 1);
//                                                    ^
//                                                    count=1
```

**Location**: `src/cpu/cpu_unicorn.cpp:414`

### Why This Works

- Each `uc_emu_start()` call completes fully before next call
- All 7 execution steps complete
- `m68k_rte()` runs and commits PC update
- Next instruction fetches from correct PC

### JIT Still Active

Despite `count=1`, Unicorn still:
- ✅ Compiles translation blocks
- ✅ Caches compiled code
- ✅ Reuses TBs across calls
- ✅ Uses native code execution

**Only overhead**: Function call boundary + context switching

---

## Comparison: UAE vs Unicorn vs Unicorn+Batching

| Backend | Execution | Speed | RTE Support |
|---------|-----------|-------|-------------|
| UAE | Pure interpreter | 1x (baseline) | ✅ Perfect |
| Unicorn (count=1) | JIT, single-step | 10-20x faster | ✅ Perfect |
| Unicorn (count=10000) | JIT, batched | 15-60x faster | ❌ Infinite loop |

**Current choice**: Unicorn (count=1) - Best working solution.

---

## Related Files

### Implementation Files

- `src/cpu/unicorn_wrapper.c:320-345` - UC_HOOK_INTR implementation with comments
- `src/cpu/cpu_unicorn.cpp:393-414` - Execution loop with RTE limitation documented
- `src/cpu/unicorn_wrapper.h:61` - `unicorn_execute_one()` declaration

### QEMU/Unicorn Source (Read-Only)

- `external/unicorn/qemu/target/m68k/translate.c:4849` - RTE instruction translation
- `external/unicorn/qemu/target/m68k/op_helper.c:40` - `m68k_rte()` implementation
- `external/unicorn/qemu/accel/tcg/cpu-exec.c` - Exception handling and hook calls

### Documentation

- `docs/deepdive/RTE_FIX.md` - Original RTE crash fix (UC_HOOK_INTR)
- `docs/TodoStatus.md` - Project status tracking

---

## Future Possibilities

### Option 1: Patch Unicorn Engine

**Approach**: Modify Unicorn to call UC_HOOK_INTR AFTER exception handler completes

**Pros**:
- Would enable batch execution
- Clean solution if done right

**Cons**:
- Requires maintaining Unicorn fork
- Complex QEMU internals (exception handling)
- Affects all exception types
- Upstream unlikely to accept (breaks existing behavior)

**Effort**: HIGH (2-4 weeks)
**Risk**: HIGH (subtle bugs in exception handling)

### Option 2: Hybrid Execution

**Approach**: Detect RTE in advance, temporarily switch to count=1

**Pseudo-code**:
```c
uint16_t next_opcode = peek_next_instruction(pc);
int count = (next_opcode == 0x4E73) ? 1 : 10000;  // RTE detection
uc_emu_start(uc, pc, end, 0, count);
```

**Pros**:
- Most instructions execute in batches
- RTE handled safely with count=1
- No Unicorn modifications needed

**Cons**:
- Requires reading ahead (memory access overhead)
- RTE frequency unknown (might be common)
- Adds complexity to execution loop

**Effort**: MEDIUM (3-5 days)
**Risk**: MEDIUM (edge cases, performance testing needed)

### Option 3: Different JIT Backend

**Approach**: Switch from Unicorn to alternative JIT

**Options**:
- Dynarec (custom M68K JIT)
- LLVM-based JIT
- Binary translation

**Pros**:
- Full control over execution
- Can design RTE handling correctly

**Cons**:
- Massive effort (months)
- M68K JIT is complex
- Lose Unicorn's stability

**Effort**: VERY HIGH (3-6 months)
**Risk**: VERY HIGH (new bugs, compatibility issues)

### Option 4: Accept Current Performance

**Approach**: Do nothing, document the limitation

**Pros**:
- Already implemented
- Stable and working
- Still 10-20x faster than UAE

**Cons**:
- Leaves 50-200% performance on table
- Suboptimal for users

**Effort**: NONE
**Risk**: NONE

**Current status**: ✅ This is what we're doing

---

## Recommendations

### Short Term (Current)

✅ **Keep using `count=1` execution**
- Stable and correct
- Good enough performance (10-20x vs interpreter)
- Low risk

### Medium Term (Next 3-6 months)

**Priority**: Investigate **Option 2: Hybrid Execution**

**Rationale**:
- Moderate effort and risk
- Could recover significant performance
- No Unicorn fork needed
- Reversible if doesn't work

**Research needed**:
1. Profile RTE frequency in real Mac OS usage
2. Measure overhead of instruction peeking
3. Prototype hybrid approach
4. Benchmark performance gain

### Long Term (6+ months)

**If Mac OS uses RTE heavily**: Consider Option 1 (Patch Unicorn)
**If RTE is rare**: Option 2 (Hybrid) is probably optimal
**If neither works**: Accept current solution (Option 4)

---

## Testing

### Verify RTE Works (Single-Step)

```bash
cd /home/mick/macemu-dual-cpu/macemu-next
./scripts/run_traces.sh 0 50000
```

**Expected**: Unicorn executes 50k+ instructions without RTE loops

### Verify Batch Execution Fails

Modify `src/cpu/cpu_unicorn.cpp:414`:
```cpp
// Change from:
if (!unicorn_execute_one(unicorn_cpu)) {

// To:
if (!unicorn_execute_n(unicorn_cpu, 10000)) {
```

Rebuild and run:
```bash
meson compile -C build
./scripts/run_traces.sh 0 50000
```

**Expected**: Unicorn logs ~20k instructions (stuck in RTE loop)

### Performance Baseline

```bash
CPU_BACKEND=unicorn EMULATOR_TIMEOUT=10 ./build/macemu-next /home/mick/quadra.rom
```

**Record**:
- Total instructions executed
- Total blocks executed
- Average block size
- Execution time

---

## Lessons Learned

1. **Hook timing matters**: UC_HOOK_INTR fires before exception handler, not after
2. **`uc_emu_stop()` isn't magic**: It stops execution but doesn't commit pending state changes
3. **Unicorn internals are complex**: QEMU's exception handling has subtle ordering
4. **Correctness > Performance**: Better to be slow and right than fast and wrong
5. **Document architectural limitations**: This bug is unfixable without major changes

---

## See Also

- [RTE_FIX.md](RTE_FIX.md) - Original RTE crash fix documentation
- [UnicornBugSrLazyFlags.md](UnicornBugSrLazyFlags.md) - Another Unicorn quirk
- [TodoStatus.md](../TodoStatus.md) - Project status and priorities

---

**Last Updated**: January 4, 2026
**Author**: Investigation by Claude Code
**Status**: Documented limitation, no fix planned for immediate term
