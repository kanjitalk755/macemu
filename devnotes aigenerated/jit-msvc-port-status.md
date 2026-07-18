# SheepShaver PPC JIT — MSVC/Win64 port status (2026-07-18)

## Goal
Enable the dyngen PPC JIT under MSVC-x64 to cut the double-interpretation tax
(our PPC interpreter running the ROM 68k emulator running QuickTime 68k code =
~10-14M interpreted PPC insns/tick during the Descent movie handoff). The JIT is
**not** just for that hitch — we want a fully working JIT for the whole tree.

## THE REAL DIRECTIVE (do this, not more byte-patching)
The byte-table approach below is a DEAD END for the stated goal. Even if every
guest-mem op is byte-patched perfectly, the JIT is only MSVC-*stitched* prebuilt
GCC machine code — the op *semantics* are opaque byte blobs from a GCC/SysV
compile, never validated by MSVC, carrying whatever ABI/addressing/register
assumptions that build baked in. Only the runtime stitcher is MSVC-compiled, so
the chain is permanently half-native and fragile.

**What the user actually wants: compile the op BODIES with MSVC.**
`ppc-dyngen-ops.cpp` / `basic-dyngen-ops.cpp` are ordinary C++ *semantics*; they
only fail under MSVC because of GCC decorations: global register vars
(`register basic_cpu *CPU asm(REG_CPU)`), `asm volatile` op-boundary markers,
computed goto / `&&label` dispatch, `__attribute__`. Replace the register-pinning
model with a context/assembler model, compile the op bodies with MSVC, and have
the JIT emit real x64 per op through the programmatic assembler
(`amd64_codegen`, `codegen_x86.h`, X86_TARGET_64BIT — already MSVC-clean). Then:
memory addressing uses this build's real VMBaseDiff *by construction* (via the
normal `vm_read_memory_*` inlines), no byte tables, whole chain native.

This is the large rewrite (register-pinning→context model, ~869 ops, the
dispatch trampolines op_execute/op_jmp_*, and the block-emitter wiring in
ppc-dyngen.cpp / ppc-translate.cpp). It IS the correct path. Start here next
session; treat everything under "IN PROGRESS" as a stopgap to be deleted once the
op bodies compile natively.

## Root diagnosis (confirmed, in the session MDs §38-40)
The handoff hitch is the 68k-emulator tax, not compositor/audio. JIT is the fix.

## What actually blocks JIT under MSVC (learned the hard way)
1. The op **bodies** (`*-dyngen-ops.cpp`) use GCC global register vars
   (`register CPU asm("rbp")`) + inline GAS. **MSVC cannot compile them.** The
   whole dyngen design instead ships **precompiled machine-code byte tables**
   (`.hpp` arrays + `copy_block`) that the runtime stitches. So "the ops" = those
   bytes; making them MSVC-correct = making the bytes Win64-correct. (The runtime
   — basic-dyngen.cpp, ppc-dyngen.cpp, ppc-jit.cpp, jit-cache.cpp, codegen_x86.h
   — is GCC-ism-free and compiles under MSVC.)
2. The committed `x86_64` tables are **System V ABI + REAL_ADDRESSING**
   (guest addr == host addr). Windows build is **Win64 ABI + DIRECT_ADDRESSING**
   with `const VMBaseDiff = NATMEM_OFFSET = 0x11000000` (guest RAM at host
   guest+0x11000000). Both differ → crashes.

## Done and working
- `jit-target-dispatch.h`: select amd64 backend on `_M_X64` (and x86 on `_M_IX86`),
  not just `__x86_64__`. **DONE.**
- `CMakeLists.txt`: removed the `MSVC AND 64-bit → JIT OFF` gate; JIT defaults on.
  **DONE.** (Set `jit true` in `SheepShaver_prefs` to actually enable at runtime —
  the JIT is gated on the `jit` pref in sheepshaver_glue.cpp:223.)
- **Helper-call ABI (source fix, guarded `#if _M_X64||(__x86_64__&&_WIN32)`):**
  rewrote `gen_invoke_*` in `basic-dyngen.cpp` to emit Win64 calls via the
  assembler: args in rcx/rdx/r8, 32-byte shadow space (`lea rsp,[rsp-0x20]`),
  and absolute `mov rax,imm64 (48 B8 + emit_64) / call rax` (the SysV fragment's
  `call rel32` overflowed 32 bits between the sub-4GB JIT cache and the ~0x7ff6
  MSVC helpers). New method `win64_gen_invoke` declared in `basic-dyngen.hpp`.
  **DONE, verified in cdb the calls now land correctly.**
- **A0/A1/A2 pointer loads (source fix, same guard) in `basic-dyngen.hpp`:**
  `gen_mov_ad_A#_im` now emits full 64-bit `movabs r12/r13/r14, imm64`. The old
  DEFINE_ALIAS took a `long` (32-bit on MSVC) AND the op emitted `mov r12d,imm32`
  — double truncation of the 64-bit `powerpc_block_info*`. **DONE, verified.**
- **op_execute trampoline + invoke ops (Win64 bytes) via
  `SheepShaver/src/Windows/gen_win64_dyngen.py`:** reads the committed SysV
  `*-x86_64.hpp`, rewrites op_execute (rcx/rdx entry, `op_exec_return_offset`
  = **0x32** = byte AFTER the `call rbx` marker — dyngen.c emits `p+sizeof(insn)`,
  getting this wrong = boot crash) and the 11 `invoke_direct*` ops, copies all
  register-only ops verbatim (identical on both ABIs: AREG pins rbp/r12/r13/r14
  are non-volatile in both). Output → `SheepShaver/src/Windows/cygwin_precompiled_dyngen/`
  (cmake already prefers that dir on WIN32; the old files there were stale 32-bit).

## IN PROGRESS — the current blocker: guest-memory op addressing
DIRECT_ADDRESSING means every guest deref needs `+VMBaseDiff`. The SysV bytes
don't add it → AV reading a bare guest address (e.g. `mov eax,[rax]`,
rax=0x68ffe038).

`gen_win64_dyngen.py` rebases guest derefs by adding 0x11000000 as a disp32.
Progress through several wrong cuts:
- First only 27 integer `load/store_*_T0_T1_*` ops → missed vector/FP data ops.
- Added vector/FP data ops; had to EXCLUDE the `[r12]` value-read in FP **store**
  ops (r12=A0=&fpr = a HOST pointer, not a guest EA).
- `lmw/stmw/lwarx/stwcx/dcbz` reuse a scratch register for BOTH guest EA and
  context/reservation pointers, and dcbz stores at disp 0/8/0x10/0x18. A naive
  "bias every `[reg]` with disp==0" is wrong for these.

**Current approach (just written, UNTESTED — did not build):**
Replaced the naive biaser in `gen_win64_dyngen.py` with **taint tracking**:
T0/T1/T2 (r12/r13/r14) start tainted; taint propagates through mov/lea/and/add/
etc. between GP regs, is cleared on load-from-memory or context; any memory
access whose base reg is tainted gets rebased (ANY displacement, via
`_add_vmbase` which widens mod=00/01/10 → disp32 and adds VMBaseDiff to the
existing disp). `_is_guest_mem_op` now includes lmw/stmw/lwarx/stwcx/dcbz again
(NOT interpreter fallback — we want them JIT'd).

### NEXT STEPS (resume here)
1. **Finish `rewrite_guest_mem_block` patch-offset shifting.** It still uses the
   old single-`shift_at` logic; with variable-length disp changes (0/1/4 → 4
   bytes) the `_im`-patch `code_ptr()+N` offsets must shift by the actual bytes
   inserted before each patch site. Recompute from real per-offset byte growth.
2. `python SheepShaver/src/Windows/gen_win64_dyngen.py SheepShaver/src/Unix/dyngen_precompiled SheepShaver/src/Windows/cygwin_precompiled_dyngen`
3. **Verify each family in the generated ppc-dyngen-ops.hpp** (disasm with
   capstone): integer loads/stores, load/store_word/vect_VD, load/store_double/
   single, lmw/stmw (guest deref biased, GPR-file `[rbp+rax*4+0x10]` NOT), lwarx/
   stwcx (guest deref biased, `[rbp+0x3b8]`/`[rax+0x3bc]`-context NOT), dcbz (ALL
   FOUR stores at disp 0/8/0x10/0x18 biased). FP stores: `[r12]` value-read NOT
   biased, guest write biased.
4. Rebuild (op tables are `#include`d into *-dyngen.cpp — needs recompile, not
   just restage). Run Descent.
5. **cdb recipe that works** (Git-Bash cwd breaks DLL load; use full exe path):
   ```
   $cmds="sxe av`ng`nr`nu @rip-20 L18`nq"
   & cdb.exe -cf cdb_cmds.txt "C:\...\SheepShaver\SheepShaver.exe"
   ```
   cdb path: `C:\Users\User\Documents\Utils\cdb\amd64\cdb.exe`.

### KEY FACTS to not re-derive
- AREG mapping (this build): CPU=rbp, T0=r12, T1=r13, T2=r14, A#=same phys as T#.
- VMBaseDiff = 0x11000000 (compile constant; DIRECT_ADDRESSING + NATMEM_OFFSET).
- Guest addr is wrapped to 32-bit before base add (ops already `mov e**,r1Xd`).
- `op_exec_return_offset` MUST be the offset AFTER `call rbx` (=0x32), not on it.
- Windows `vm_acquire` already honors VM_MAP_32BIT + PAGE_EXECUTE_READWRITE.
- Files touched: jit-target-dispatch.h, CMakeLists.txt, basic-dyngen.cpp/.hpp,
  gen_win64_dyngen.py (NEW), cygwin_precompiled_dyngen/*.hpp (GENERATED),
  qd3d_init_logging.h (GFX_TICKPROF_ENABLED — diagnostic, turn OFF before merge),
  ppc-translate.cpp (interpreter-fallback guards were ADDED then REVERTED — verify
  they're gone).

### Open design question the user raised
Byte-patching precompiled tables is fragile (this whole §"IN PROGRESS" is why).
The fully-correct long-term path is emitting ALL ops through the programmatic
assembler (amd64_codegen, X86_TARGET_64BIT, MSVC-clean) so addressing is correct
by construction — but that's a large op-by-op rewrite. Taint-tracking the bytes
is the pragmatic bridge; if it stays fragile, escalate to assembler emission for
the memory ops at minimum.

## Diagnostics still on (turn OFF before merge)
`GFX_TICKPROF_ENABLED` (per-tick wall accounting + PPC/68k block counters +
hotpage dump) and `DESCENT_MOVIE_DIAGNOSTICS` was set to 0. All in
qd3d_init_logging.h / sheepshaver_glue.cpp / ppc-cpu.cpp.
