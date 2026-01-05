# QEMU Research: How Does Standalone QEMU Handle RTE?

**Date**: January 4, 2026
**Research Goal**: Understand how standalone QEMU handles RTE to find a fix for Unicorn batch execution
**Status**: 🔄 IN PROGRESS

---

## The Question

If standalone QEMU can execute multiple instructions (like our desired batch execution), how does it handle RTE without the infinite loop problem that Unicorn has?

---

## Key Findings

### RTE Execution Flow in QEMU/Unicorn

#### Step 1: Translation (translate.c)

When RTE instruction (0x4E73) is encountered during translation:

```c
// translate.c:4849
DISAS_INSN(rte)
{
    if (IS_USER(s)) {
        gen_exception(s, s->base.pc_next, EXCP_PRIVILEGE);
        return;
    }
    gen_exception(s, s->base.pc_next, EXCP_RTE);
}
```

This generates a call to exception handler.

#### Step 2: Exception Generation (translate.c:312-321)

```c
static void gen_exception(DisasContext *s, uint32_t dest, int nr)
{
    TCGContext *tcg_ctx = s->uc->tcg_ctx;
    update_cc_op(s);
    tcg_gen_movi_i32(tcg_ctx, QREG_PC, dest);  // Set PC to next instruction

    gen_raise_exception(tcg_ctx, nr);  // Generate helper call

    s->base.is_jmp = DISAS_NORETURN;  // Mark TB as ending
}
```

Key: `DISAS_NORETURN` tells QEMU this TB won't continue to next instruction.

#### Step 3: Helper Call Generation (translate.c:305-309)

```c
static void gen_raise_exception(TCGContext *tcg_ctx, int nr)
{
    TCGv_i32 tmp;
    tmp = tcg_const_i32(tcg_ctx, nr);
    gen_helper_raise_exception(tcg_ctx, tcg_ctx->cpu_env, tmp);  // Embed helper call in compiled code
    tcg_temp_free_i32(tcg_ctx, tmp);
}
```

This embeds a call to `helper_raise_exception` in the compiled translation block.

#### Step 4: Helper Execution (op_helper.c:410-413)

When the compiled TB executes and reaches the helper call:

```c
void HELPER(raise_exception)(CPUM68KState *env, uint32_t tt)
{
    raise_exception(env, tt);
}

static void raise_exception(CPUM68KState *env, int tt)
{
    raise_exception_ra(env, tt, 0);
}

static void raise_exception_ra(CPUM68KState *env, int tt, uintptr_t raddr)
{
    CPUState *cs = env_cpu(env);

    cs->exception_index = tt;  // Set exception_index to EXCP_RTE (0x100)
    cpu_loop_exit_restore(cs, raddr);  // longjmp back to main loop!
}
```

#### Step 5: Return to Main Loop (cpu-exec-common.c:48-53)

```c
void cpu_loop_exit_restore(CPUState *cpu, uintptr_t pc)
{
    if (pc) {
        cpu_restore_state(cpu, pc, true);
    }
    cpu_loop_exit(cpu);  // Does siglongjmp
}

void cpu_loop_exit(CPUState *cpu)
{
    cpu->can_do_io = 1;
    siglongjmp(cpu->uc->jmp_bufs[cpu->uc->nested_level - 1], 1);  // Jump back to setjmp in cpu_exec!
}
```

This returns to `cpu_exec()` at line 577 (the setjmp point).

#### Step 6: Exception Handling Loop (cpu-exec.c:594)

```c
/* if an exception is pending, we execute it here */
while (!cpu_handle_exception(cpu, &ret)) {
    // ... TB execution loop
}
```

`cpu_handle_exception` is called to process the exception.

#### Step 7: Unicorn's Hook Interception (cpu-exec.c:405-430)

**This is where Unicorn diverges from standalone QEMU!**

```c
// Lines 405-414: Unicorn-specific code
// Unicorn: call registered interrupt callbacks
catched = false;
HOOK_FOREACH(uc, hook, UC_HOOK_INTR) {
    ((uc_cb_hookintr_t)hook->callback)(uc, cpu->exception_index, hook->user_data);
    catched = true;
}

// Lines 415-424: Unicorn-specific code
if (!catched) {
    uc->invalid_error = UC_ERR_EXCEPTION;
    cpu->halted = 1;
    *ret = EXCP_HLT;
    return true;  // Stop execution
}

// Line 426: CRITICAL!
cpu->exception_index = -1;  // Clear the exception

// Lines 429-430
*ret = EXCP_INTERRUPT;
return false;  // Continue main loop
```

**The Problem**: Line 426 clears `exception_index` BEFORE `m68k_rte()` is called!

---

## The Critical Question

**In standalone QEMU (without Unicorn hooks), where does `m68k_interrupt_all()` get called?**

The function exists in `op_helper.c:168-212`:

```c
static void m68k_interrupt_all(CPUM68KState *env, int is_hw)
{
    CPUState *cs = env_cpu(env);

    if (!is_hw) {
        switch (cs->exception_index) {
        case EXCP_RTE:
            /* Return from an exception.  */
            m68k_rte(env);  // ← THIS is where PC gets updated!
            return;
        // ... other cases
        }
    }
    // ... handle other interrupts
}
```

It's called from:
- `do_interrupt_all(env, is_hw)` at line 311
- Which is called from `m68k_cpu_do_interrupt(cs)` at line 322

But **when** is `m68k_cpu_do_interrupt()` called?

### Exception Value Analysis

```c
#define EXCP_RTE        0x100      // M68K RTE exception
#define EXCP_INTERRUPT  0x10000    // Generic interrupt marker
```

In `cpu_handle_exception` at line 372:
```c
if (cpu->exception_index >= EXCP_INTERRUPT) {
    *ret = cpu->exception_index;
    // ... handle EXCP_DEBUG, etc.
    cpu->exception_index = -1;
    return true;
}
```

**RTE (0x100) < EXCP_INTERRUPT (0x10000)**, so RTE goes through the `else` block!

But in Unicorn, the `else` block has hooks that intercept and clear the exception.

---

## Hypothesis: Unicorn's Hook Replaces QEMU's Native Handler

**Theory**: In standalone QEMU, there must be code in the `else` block (lines 380-430) that calls `m68k_cpu_do_interrupt()` or `m68k_interrupt_all()` directly.

But Unicorn's modifications (lines 405-430) **REPLACE** that code with hook handling, which doesn't call the native M68K interrupt handler!

### Evidence Needed

Need to check:
1. Original QEMU source (not Unicorn's fork) to see what lines 380-430 look like
2. Whether there's a direct call to `cc->do_interrupt()` that Unicorn removed
3. How exceptions < EXCP_INTERRUPT are normally processed

---

## Potential Solutions Based on Research

### Solution 1: Call m68k_interrupt_all() from Hook

**Idea**: In our UC_HOOK_INTR handler, manually call `m68k_interrupt_all()`:

```c
static void hook_interrupt(uc_engine *uc, uint32_t intno, void *user_data) {
    if (intno == 0x100) {  // EXCP_RTE
        // Get M68K environment
        CPUM68KState *env = (CPUM68KState *)uc->cpu->env_ptr;

        // Call QEMU's native RTE handler
        m68k_interrupt_all(env, 0);  // is_hw = 0 for software exception

        // DON'T call uc_emu_stop() - let execution continue naturally
    }
}
```

**Pros**:
- Uses QEMU's existing, tested RTE implementation
- Should update PC correctly
- Might allow batch execution to work

**Cons**:
- Requires accessing Unicorn internals (CPUM68KState)
- m68k_interrupt_all is static - would need to make it accessible
- May have side effects we don't understand

**Risk**: MEDIUM-HIGH (touching QEMU internals)

### Solution 2: Don't Clear exception_index in Hook

**Idea**: Let QEMU's normal exception path continue:

```c
static void hook_interrupt(uc_engine *uc, uint32_t intno, void *user_data) {
    if (intno == 0x100) {  // EXCP_RTE
        // Do nothing - let QEMU handle it
        // Don't clear exception_index
        return;
    }
}
```

Then modify Unicorn's `cpu_handle_exception` to NOT clear `exception_index` after hook, allowing QEMU's native handler to run.

**Pros**:
- Minimal changes
- Uses QEMU's native code path

**Cons**:
- Requires patching Unicorn's cpu-exec.c
- May break other exception types
- Need to understand full exception flow

**Risk**: HIGH (changes core Unicorn exception handling)

### Solution 3: Inline RTE Handling in Generated Code

**Idea**: Instead of generating `helper_raise_exception`, generate inline code that:
1. Reads SR and PC from stack
2. Updates registers directly
3. Continues execution

This would avoid the exception mechanism entirely.

**Pros**:
- No exception overhead
- Direct execution, no longjmp

**Cons**:
- Requires modifying QEMU's M68K translator
- Complex to implement correctly
- Must handle all RTE stack frame formats

**Risk**: VERY HIGH (complex M68K emulation logic)

---

## Next Steps

1. ✅ Understand QEMU's RTE execution flow
2. ⏳ Find original QEMU source to see unmodified `cpu_handle_exception`
3. ⏳ Test Solution 1: Call `m68k_interrupt_all()` from hook
4. ⏳ If Solution 1 works, measure performance impact
5. ⏳ Consider upstreaming fix to Unicorn project

---

## Related Files

- `external/unicorn/qemu/accel/tcg/cpu-exec.c` - Main execution loop, exception handling
- `external/unicorn/qemu/target/m68k/translate.c` - RTE translation (line 4849)
- `external/unicorn/qemu/target/m68k/op_helper.c` - RTE handler (line 40, 168, 410)
- `external/unicorn/qemu/accel/tcg/cpu-exec-common.c` - longjmp handling

---

**Last Updated**: January 4, 2026
**Status**: Research in progress, potential solutions identified
