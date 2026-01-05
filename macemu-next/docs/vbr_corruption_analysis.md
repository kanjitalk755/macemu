# VBR Corruption Problem Analysis

## Problem Summary

Unicorn backend is experiencing VBR (Vector Base Register) corruption, causing it to read trap vectors from the wrong memory location and jump to null pointers, resulting in an infinite loop.

## Current Behavior

### UAE (Working)
- Executes normally at PC ranges like 0x0200E196-0x0200E1B2
- Completes 100,000 instructions successfully
- Executes real Mac ROM code

### Unicorn (Broken)
- Gets stuck in infinite loop at PC=0x00000000-0x000000C4
- Executes garbage instruction 0x0200 repeatedly
- All 100,000 "instructions" are just looping in null handler
- VBR corrupted to values like 0x0073F400 or 0xB8713400

## Evidence

### From run_traces.sh output:
```
[Unicorn] Reset: VBR=0, CACR=0                           <- VBR initialized correctly
...
[DEBUG] VBR=0x0073F400, vector_nr=11, vector_addr=0x0073F42C  <- VBR corrupted!
[DEBUG] Read handler_addr=0x00000000 from vector table        <- Reads null from wrong addr
[DEBUG] After trap: new PC=0x00000000                         <- PC set to null
```

### From unicorn_100k.log:
```
Last instructions:
[99997] 000000BC 0200 | ...
[99998] 000000C0 0200 | ...
[99999] 000000C4 0200 | ...
```

All executing at address 0x00000000 (null), not in ROM!

### From uae_100k.log:
```
Last instructions:
[99997] 0200E1A6 642C | ...
[99998] 0200E1A8 2229 | ...
[99999] 0200E1AC 4A11 | ...
```

Executing in ROM at 0x0200xxxx addresses (correct).

## Expected Behavior

1. VBR should be 0 (or remain at whatever ROM sets it to)
2. Trap vector 10 (A-line trap) at address VBR + 0x28 = 0x00000028
3. Handler address should be read from RAM[0x28] = 0x020099B0 (correct)
4. PC should jump to 0x020099B0 to handle the trap

## Actual Behavior

1. VBR starts at 0 (correct)
2. VBR becomes 0x0073F400 (WRONG - corrupted!)
3. Trap vector 10 at address 0x0073F400 + 0x28 = 0x0073F428 (wrong address)
4. Handler address read from RAM[0x0073F428] = 0x00000000 (null/garbage)
5. PC jumps to 0x00000000 (crash!)
6. Infinite loop: null handler triggers more exceptions at PC=0

## Hypotheses

### Hypothesis 1: ROM uses MOVEC to set VBR
- M68K ROM code typically uses `MOVEC Dn,VBR` to set vector base
- Mac ROMs might relocate vector table to RAM
- We need to trace MOVEC instructions

### Hypothesis 2: Unicorn Engine bug
- Unicorn might not properly initialize VBR
- Unicorn might corrupt VBR internally
- Check if uc_reg_write for VBR actually works

### Hypothesis 3: Uninitialized memory read
- VBR might be reading uninitialized value
- Stack or register corruption might be writing to VBR
- Memory corruption during reset

### Hypothesis 4: Reset sequence issue
- VBR initialization happens too early (before Unicorn ready)
- VBR initialization happens too late (after ROM starts)
- Reset function not being called at right time

## Investigation Steps

### Step 1: Trace VBR writes
- Add hook to catch MOVEC instructions that write to VBR
- Log every VBR change with backtrace
- Find exact instruction that corrupts VBR

### Step 2: Verify VBR initialization
- Read VBR immediately after reset
- Read VBR before first instruction
- Read VBR after each instruction until corruption

### Step 3: Compare with UAE
- Check if UAE also sets VBR to non-zero
- Compare UAE VBR value vs Unicorn VBR value
- See if ROM expects specific VBR value

### Step 4: Check Unicorn Engine behavior
- Test if `uc_reg_write(UC_M68K_REG_CR_VBR)` works
- Test if `uc_reg_read(UC_M68K_REG_CR_VBR)` returns correct value
- Check Unicorn documentation for VBR handling

## Code Locations

### VBR Initialization
- File: [macemu-next/src/cpu/cpu_unicorn.cpp](macemu-next/src/cpu/cpu_unicorn.cpp#L214-L219)
```cpp
uc_engine *uc = (uc_engine *)unicorn_get_uc(unicorn_cpu);
uint32_t zero = 0;
uc_reg_write(uc, UC_M68K_REG_CR_VBR, &zero);
uc_reg_write(uc, UC_M68K_REG_CR_CACR, &zero);
fprintf(stderr, "[Unicorn] Reset: VBR=0, CACR=0\n");
```

### VBR Reading (Exception Handling)
- File: [macemu-next/src/cpu/unicorn_exception.c](macemu-next/src/cpu/unicorn_exception.c#L136-L141)
```cpp
fprintf(stderr, "[DEBUG] VBR=0x%08X, vector_nr=%d, vector_addr=0x%08X\n",
        vbr, vector_nr, vbr + (vector_nr * 4));
uint32_t handler_addr = read_long(cpu, vbr + (vector_nr * 4));
fprintf(stderr, "[DEBUG] Read handler_addr=0x%08X from vector table\n", handler_addr);
```

### Trap Handler
- File: [macemu-next/src/cpu/cpu_unicorn.cpp](macemu-next/src/cpu/cpu_unicorn.cpp#L89-L105)
```cpp
static bool unicorn_platform_trap_handler(int vector, uint16_t opcode, bool is_primary) {
	fprintf(stderr, "[DEBUG] Trap handler called: vector=%d, opcode=0x%04X, PC=0x%08X\n",
	        vector, opcode, unicorn_get_pc(unicorn_cpu));
	extern void unicorn_simulate_exception(UnicornCPU *cpu, int vector_nr, uint16_t opcode);
	unicorn_simulate_exception(unicorn_cpu, vector, opcode);
	fprintf(stderr, "[DEBUG] After trap: new PC=0x%08X\n", unicorn_get_pc(unicorn_cpu));
	return true;
}
```

## Questions to Answer

1. **When does VBR get corrupted?**
   - After how many instructions?
   - What instruction causes it?
   - Is it a MOVEC or something else?

2. **What should VBR be?**
   - Does Mac ROM expect VBR=0?
   - Does Mac ROM set VBR to a specific value?
   - What does UAE's VBR contain?

3. **Is the corruption deterministic?**
   - Same VBR value each run?
   - Same instruction count before corruption?
   - Same pattern in different ROM files?

4. **Is Unicorn's VBR register working?**
   - Can we read what we write?
   - Is the register ID correct?
   - Does Unicorn support VBR on M68K?

## Next Actions

1. Add VBR change detection hook in Unicorn
2. Log VBR value before/after each instruction for first 1000 instructions
3. Compare UAE vs Unicorn VBR values at same instruction count
4. Check Unicorn source code for VBR implementation
5. Test simple program that sets VBR to known value

## Related Files

- [macemu-next/src/cpu/cpu_unicorn.cpp](macemu-next/src/cpu/cpu_unicorn.cpp) - Unicorn backend
- [macemu-next/src/cpu/unicorn_wrapper.c](macemu-next/src/cpu/unicorn_wrapper.c) - Unicorn hooks
- [macemu-next/src/cpu/unicorn_exception.c](macemu-next/src/cpu/unicorn_exception.c) - Exception simulation
- [macemu-next/src/main.cpp](macemu-next/src/main.cpp) - Main loop
- [trace_analyzer.py](trace_analyzer.py) - Trace analysis tool

## ROOT CAUSE FOUND! 🎯

**Unicorn Engine does NOT implement VBR register for M68K!**

Evidence:
```
WARNING: Your register accessing on id 21 is deprecated and will get UC_ERR_ARG
in the future release (2.2.0) because the accessing is either no-op or not defined.
```

Register ID 21 = UC_M68K_REG_CR_VBR

When we:
- Write VBR=0: **Unicorn ignores it** (no-op)
- Read VBR: **Unicorn returns garbage** (uninitialized memory, often a host pointer fragment)

This explains:
1. Why VBR reads back as 0 immediately after write (might be cached in our buffer)
2. Why VBR becomes 0xED21A400 or 0xCEDF1400 (host pointers from Unicorn internals)
3. Why trap vectors are read from wrong addresses
4. Why handler address is garbage/null

## Solution

Since Unicorn doesn't support VBR, we need to:
1. **Emulate VBR ourselves** - track VBR value in our own variable
2. **Don't use uc_reg_read/write for VBR** - they don't work
3. **Use our tracked VBR in exception handling** - read from our variable, not Unicorn's

Alternative: File bug report with Unicorn and use UAE-only for now.

## Status

- [x] Exception messages moved to stdout
- [x] Identified VBR corruption as root cause
- [x] Documented problem and evidence
- [x] **FOUND ROOT CAUSE: Unicorn doesn't implement VBR!**
- [ ] Implement software VBR emulation
- [ ] Test with software VBR
- [ ] Verify Unicorn matches UAE behavior
