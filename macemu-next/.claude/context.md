# macemu-next Project Context

**Auto-loaded context for all Claude sessions working on macemu-next**

---

## What Is This Project?

**macemu-next** is a modern Mac emulator focused on **Unicorn M68K CPU** with dual-CPU validation.

### Core Focus
- 🎯 **Primary Goal**: Unicorn-based Mac emulator (fast JIT execution)
- 📊 **Validation**: Dual-CPU mode (UAE + Unicorn in lockstep)
- 🏗️ **Architecture**: Clean Platform API abstraction

### Not This
- ❌ Not a BasiliskII fork (clean-room rewrite)
- ❌ Not cycle-accurate (pragmatic emulation)
- ❌ Not focused on UAE (legacy support only)

---

## Project Status (January 2026)

### Phase 1: ✅ COMPLETE - Core CPU Emulation
- ✅ Unicorn M68K backend working (68020 with JIT)
- ✅ EmulOps (0x71xx), A-line/F-line traps (0xAxxx, 0xFxxx)
- ✅ Interrupt support (timer, ADB)
- ✅ Native trap execution (no UAE dependency)
- ✅ **514,000+ instructions validated** in dual-CPU mode
- ✅ VBR register support, CPU type fix, hook optimization

### Phase 2: 🎯 CURRENT - Boot to Desktop
- ⏳ Understanding timer interrupt timing (wall-clock vs instruction-count)
- ⏳ Investigating why Unicorn stops at ~200k vs UAE 250k
- ⏳ Functional testing approach (not just trace comparison)

### Phase 3-5: ⏳ FUTURE
- Hardware emulation (VIA, SCSI, Video)
- Application support (HyperCard, games)
- Performance optimization
- SheepShaver/PowerPC (far future)

---

## Architecture Overview

### Platform API (The Heart of the System)

**Everything goes through `g_platform` struct** ([src/common/include/platform.h](../src/common/include/platform.h)):

```c
typedef struct Platform {
    // CPU Execution
    CPUExecResult (*cpu_execute_one)(void);
    uint32_t (*cpu_get_pc)(void);
    void (*cpu_set_pc)(uint32_t pc);
    uint32_t (*cpu_get_dreg)(int n);
    // ... 20+ more CPU operations

    // Trap/Exception Handling
    bool (*emulop_handler)(uint16_t opcode, bool probe);
    void (*trap_handler)(int type, uint16_t opcode, bool probe);
    void (*cpu_execute_68k_trap)(uint16_t trap, struct M68kRegisters *r);
} Platform;

extern Platform g_platform;
```

**Key Point**: Core code is backend-agnostic. All CPU operations go through `g_platform`.

### Three CPU Backends

| Backend | Purpose | Role | Status |
|---------|---------|------|--------|
| **Unicorn** | Primary (JIT) | ⭐ **The future** - what we're building | Active development |
| **UAE** | Legacy (interpreter) | 📊 **The baseline** - validation reference | Maintained, not focus |
| **DualCPU** | Validation tool | 🔍 **The validator** - catch bugs | Development tool only |

**Backend Selection**: `CPU_BACKEND=unicorn` (or `uae`, `dualcpu`)

### Memory System

**Direct Addressing** - Mac addresses map directly to host memory:
```
Mac Address              Host Memory
0x00000000 (RAM) ----→   RAMBaseHost + 0x00000000
0x40800000 (ROM) ----→   ROMBaseHost

host_ptr = mac_addr + MEMBaseDiff
```

**Endianness**:
- UAE: RAM little-endian, ROM big-endian, byte-swap on access
- Unicorn: All memory big-endian, no automatic swapping
- **Must byte-swap RAM when copying to Unicorn!**

### Trap System

Three types of traps:
1. **EmulOps (0x71xx)**: Illegal instructions that call emulator functions
2. **A-line traps (0xAxxx)**: Mac OS Toolbox calls
3. **F-line traps (0xFxxx)**: FPU emulation

**Hook Architecture** (Unicorn):
- `UC_HOOK_BLOCK` - Interrupts checked at basic block boundaries (~100k/sec)
- `UC_HOOK_INSN_INVALID` - EmulOps/traps on illegal instructions only (~1k/sec)
- ❌ No `UC_HOOK_CODE` - Removed (was 10x slower)

### Interrupt System

**Platform API Abstraction** (c388b229):
```c
// Platform API function pointer
void (*cpu_trigger_interrupt)(int level);

// Backends implement this differently:
// - UAE: Sets SPCFLAG_INT, processed by do_specialties()
// - Unicorn: Sets g_pending_interrupt_level, checked by hook_block()
```

**Timer/Device Code**:
```c
extern Platform g_platform;
int level = intlev();  // Get interrupt level from Mac hardware state
g_platform.cpu_trigger_interrupt(level);  // Backend-agnostic call
```

**UAE Backend**: Sets `SPCFLAG_INT`, UAE's native interrupt mechanism handles the rest

**Unicorn Backend**: Manual M68K exception handling
- Sets `g_pending_interrupt_level` (volatile int)
- `hook_block()` checks at basic block boundaries
- Manually builds exception stack frame (PC, SR)
- Updates SR (supervisor mode, interrupt mask)
- Reads vector table, jumps to handler
- Manual approach chosen over QEMU's `m68k_set_irq_level()` (requires fragile struct offsets)

---

## Key Technical Points

### Recent Achievements (with commit hashes)
- **Platform API interrupts** (c388b229): Backend-agnostic interrupt triggering, eliminated global state
- **VBR fix** (006cc0f8): Added missing Unicorn register API (+330% execution)
- **CPU type fix** (74fbd578): Fixed 68030 vs 68020 selection (enum/array mismatch)
- **Interrupt support** (1305d3b2): UC_HOOK_BLOCK for efficiency
- **Native trap execution** (d90208dc): Unicorn self-contained, no UAE dependency
- **Legacy API removal** (ebd3d1b2): Removed UC_HOOK_CODE (~236 lines, 5-10x faster)
- **514k validation** (155497f0): Massive dual-CPU validation milestone

### Current Investigation

**Timer Interrupt Timing** ([docs/deepdive/InterruptTimingAnalysis.md](../docs/deepdive/InterruptTimingAnalysis.md)):
- First divergence at instruction #29,518
- **Root cause**: Wall-clock timers, not instruction-count
- UAE (slow interpreter) vs Unicorn (fast JIT) execute at different speeds
- Same wall-clock time → different instruction counts when interrupt fires
- **Not a bug** - characteristic of wall-clock-based timing
- **Decision needed**: Accept non-determinism or add deterministic mode?

### Known Issues
1. **Interrupt timing non-determinism** - See above
2. **Unicorn stops at ~200k** (vs UAE 250k) - Under investigation
3. Both are related to cumulative effects of timer interrupt timing

---

## File Organization

### Source Code Structure
```
src/
├── common/include/        # Platform API (platform.h, cpu_emulation.h)
├── core/                  # Backend-agnostic (emul_op.cpp, main.cpp)
├── cpu/
│   ├── cpu_uae.cpp        # UAE backend
│   ├── cpu_unicorn.cpp    # Unicorn backend (PRIMARY FOCUS)
│   ├── cpu_dualcpu.cpp    # DualCPU validation
│   ├── uae_cpu/           # UAE internals (legacy)
│   ├── uae_wrapper.cpp    # UAE wrapper + shared interrupt code
│   └── unicorn_wrapper.c  # Unicorn API wrapper
└── tests/                 # Unit and boot tests
```

### Documentation Structure
```
docs/
├── README.md              # Entry point, quick start
├── Architecture.md        # Platform API, backends (READ THIS!)
├── ProjectGoals.md        # Vision, Unicorn-first focus
├── TodoStatus.md          # Checklist with ✅ and ⏳
├── Commands.md            # Build, test, trace commands
│
├── deepdive/              # Detailed technical docs (CamelCase)
│   ├── InterruptTimingAnalysis.md  # ACTIVE investigation
│   ├── MemoryArchitecture.md
│   ├── UaeQuirks.md
│   ├── UnicornQuirks.md
│   └── [13 more...]
│
└── completed/             # Historical archive
    └── [15 completion docs]
```

---

## Essential Commands

### Build
```bash
cd macemu-next
meson setup build
meson compile -C build
```

### Run
```bash
# Unicorn (primary)
CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom

# With timeout
EMULATOR_TIMEOUT=5 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom

# DualCPU validation
EMULATOR_TIMEOUT=30 CPU_BACKEND=dualcpu ./build/macemu-next ~/quadra.rom
```

### Trace & Debug
```bash
# Trace first 1000 instructions
CPU_TRACE=0-1000 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom

# Compare traces
EMULATOR_TIMEOUT=2 CPU_TRACE=0-250000 CPU_BACKEND=uae ./build/macemu-next ~/quadra.rom > uae.log
EMULATOR_TIMEOUT=2 CPU_TRACE=0-250000 CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom > uni.log
diff uae.log uni.log | head -50
```

### Environment Variables
- `CPU_BACKEND=unicorn|uae|dualcpu` - Select backend
- `EMULATOR_TIMEOUT=N` - Auto-exit after N seconds
- `CPU_TRACE=N-M` - Trace instruction range
- `CPU_TRACE_MEMORY=1` - Include memory accesses
- `EMULOP_VERBOSE=1` - Log EmulOp calls
- `DUALCPU_TRACE_DEPTH=N` - DualCPU history depth

---

## Working with This Codebase

### When Implementing Features
1. **Platform API first** - Add to `g_platform` if needed
2. **Unicorn focus** - Implement for Unicorn backend primarily
3. **Validate** - Run DualCPU mode to catch bugs early
4. **Document quirks** - If it's surprising, document it

### When Debugging
1. **Check TodoStatus.md** - Is this a known issue?
2. **Run DualCPU** - Find exact divergence point
3. **Trace comparison** - Generate UAE/Unicorn traces, compare
4. **Read deepdive docs** - Quirks are documented

### When Adding Documentation
1. **Top-level** - Quick reference only (README, Commands, Architecture)
2. **deepdive/** - Detailed technical docs (CamelCase)
3. **completed/** - Historical work only (after completion)

### Code Style
- **Platform API calls**: Always use `g_platform.cpu_execute_one()` not direct backend calls
- **Unicorn-first**: New features target Unicorn, UAE compatibility second
- **No UAE dependency in Unicorn**: Unicorn backend must be self-contained

---

## Common Tasks

### Task: Add CPU feature support
1. Check if Platform API needs new function pointer
2. Implement in `cpu_unicorn.cpp` (primary)
3. Implement in `cpu_uae.cpp` (for validation)
4. Test with DualCPU mode
5. Document quirks in deepdive/

### Task: Fix Unicorn divergence
1. Run: `EMULATOR_TIMEOUT=10 DUALCPU_TRACE_DEPTH=20 CPU_BACKEND=dualcpu ./build/macemu-next ~/quadra.rom`
2. Find divergence instruction number (e.g., 12345)
3. Generate detailed traces: `CPU_TRACE=12340-12350 CPU_TRACE_MEMORY=1 CPU_BACKEND=uae|unicorn`
4. Compare, identify difference
5. Fix in `cpu_unicorn.cpp` or `unicorn_wrapper.c`
6. Re-validate with DualCPU

### Task: Understand existing code
1. Read [docs/Architecture.md](../docs/Architecture.md) - Platform API overview
2. Read [docs/ProjectGoals.md](../docs/ProjectGoals.md) - Vision and roles
3. Read relevant deepdive doc (e.g., UnicornQuirks.md)
4. Trace code from `g_platform` call → backend implementation

---

## Important Files to Read

### Always Read These First
1. **[docs/Architecture.md](../docs/Architecture.md)** - Platform API, backends, memory
2. **[docs/ProjectGoals.md](../docs/ProjectGoals.md)** - Vision, Unicorn-first focus
3. **[docs/TodoStatus.md](../docs/TodoStatus.md)** - Current status, what's done

### For Specific Topics
- **Memory**: [docs/deepdive/MemoryArchitecture.md](../docs/deepdive/MemoryArchitecture.md)
- **UAE backend**: [docs/deepdive/UaeQuirks.md](../docs/deepdive/UaeQuirks.md)
- **Unicorn backend**: [docs/deepdive/UnicornQuirks.md](../docs/deepdive/UnicornQuirks.md)
- **Interrupts**: [docs/deepdive/InterruptTimingAnalysis.md](../docs/deepdive/InterruptTimingAnalysis.md)
- **Traps**: [docs/deepdive/ALineAndFLineTrapHandling.md](../docs/deepdive/ALineAndFLineTrapHandling.md)

### For Commands
- **[docs/Commands.md](../docs/Commands.md)** - Build, test, trace, debug

---

## Key Mantras

1. **Unicorn is the future** - Focus development on Unicorn backend
2. **Platform API is king** - All core code goes through `g_platform`
3. **Validate continuously** - DualCPU catches bugs immediately
4. **Document quirks** - If it surprised you, document it
5. **UAE is baseline** - When Unicorn differs, UAE is usually right

---

## Quick Orientation

**New to the project?**
→ Read: docs/README.md → docs/Architecture.md → docs/ProjectGoals.md

**Need to build/test?**
→ Read: docs/Commands.md

**Debugging issue?**
→ Check: docs/TodoStatus.md (known issues) → docs/deepdive/InterruptTimingAnalysis.md

**Understanding divergence?**
→ Run: DualCPU mode → Generate traces → Compare → Read deepdive docs

**Adding feature?**
→ Platform API first → Implement for Unicorn → Validate with DualCPU → Document

---

**Last Updated**: January 4, 2026
**Project Phase**: Phase 2 - Boot to Desktop
**Current Focus**: Platform API architecture, interrupt handling implementation
