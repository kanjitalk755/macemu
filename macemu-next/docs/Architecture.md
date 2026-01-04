# Architecture Overview

How macemu-next fits together: Platform API, CPU backends, memory system.

---

## Core Design Principle

**Everything goes through the Platform API**

The entire emulator is built around a single abstraction layer that separates:
- **What** the emulator needs (CPU execution, memory access, trap handling)
- **How** it's implemented (UAE, Unicorn, or future backends)

---

## Platform API (The Heart of the System)

### What Is It?

The `Platform` struct ([src/common/include/platform.h](../src/common/include/platform.h)) defines function pointers for all backend operations:

```c
typedef struct Platform {
    // CPU Execution
    bool (*cpu_init)(void);
    CPUExecResult (*cpu_execute_one)(void);
    uint32_t (*cpu_get_pc)(void);
    void (*cpu_set_pc)(uint32_t pc);
    uint32_t (*cpu_get_dreg)(int n);
    uint32_t (*cpu_get_areg)(int n);
    // ... 20+ more CPU operations

    // Trap/Exception Handling
    bool (*emulop_handler)(uint16_t opcode, bool probe);
    void (*trap_handler)(int type, uint16_t opcode, bool probe);
    void (*cpu_execute_68k_trap)(uint16_t trap, struct M68kRegisters *r);

    // Memory Operations (optional, for direct backend access)
    uint8_t (*cpu_read_byte)(uint32_t addr);
    void (*cpu_write_byte)(uint32_t addr, uint8_t val);
    // ... etc

} Platform;

extern Platform g_platform;
```

### Why This Design?

**Benefits:**
1. **Backend Independence** - Core code never calls UAE or Unicorn directly
2. **Runtime Selection** - Choose backend via `CPU_BACKEND` environment variable
3. **Easy Testing** - Can mock out backends for unit tests
4. **Future Expansion** - Add new backends (QEMU? Custom JIT?) without touching core

**Example Usage:**
```c
// Core emulation code doesn't know if it's UAE or Unicorn:
uint32_t pc = g_platform.cpu_get_pc();
CPUExecResult result = g_platform.cpu_execute_one();
if (result == CPU_EXEC_EMULOP) {
    g_platform.emulop_handler(opcode, false);
}
```

---

## Three CPU Backends

### 1. UAE (Legacy, Interpreter)

**Purpose**: Proven, stable baseline for validation

**Implementation**: [src/cpu/cpu_uae.cpp](../src/cpu/cpu_uae.cpp)

**Characteristics**:
- Original BasiliskII CPU core (C++ interpreter)
- Well-tested, reliable
- Slower than JIT but 100% compatible
- Direct memory access via `mem_banks[]`

**Role in Project**:
- Legacy compatibility
- Validation baseline for Unicorn
- Will be retained but not primary focus

### 2. Unicorn (Primary, JIT)

**Purpose**: **Primary backend** - fast JIT execution

**Implementation**: [src/cpu/cpu_unicorn.cpp](../src/cpu/cpu_unicorn.cpp) + [src/cpu/unicorn_wrapper.c](../src/cpu/unicorn_wrapper.c)

**Characteristics**:
- QEMU-based JIT compiler
- 10-50x faster than interpreter
- Efficient hook architecture (UC_HOOK_BLOCK, UC_HOOK_INSN_INVALID)
- Self-contained (no UAE dependency for trap execution)

**Role in Project**:
- **END GOAL** - This is what we're building toward
- Fast, clean, maintainable
- Validated via dual-CPU mode

**Hook Architecture** (Performance-Optimized):
```c
// UC_HOOK_BLOCK - Interrupts checked at basic block boundaries
// Called ~100k times/sec vs ~1M times/sec for per-instruction
static void hook_block(...) {
    if (PendingInterrupt) {
        // Trigger M68K interrupt
    }
}

// UC_HOOK_INSN_INVALID - EmulOps/traps on illegal instructions only
// Called only when CPU hits 0x71xx, 0xAxxx, 0xFxxx (~1k times/sec)
static bool hook_insn_invalid(...) {
    if (is_emulop) {
        g_platform.emulop_handler(opcode);
        return true;  // Continue execution
    }
}
```

### 3. DualCPU (Validation Tool)

**Purpose**: Run UAE and Unicorn in lockstep to catch bugs

**Implementation**: [src/cpu/cpu_dualcpu.cpp](../src/cpu/cpu_dualcpu.cpp)

**Algorithm**:
```c
while (running) {
    // 1. Save state
    uint32_t pc = uae_get_pc();
    assert(unicorn_get_pc() == pc);  // Must be in sync

    // 2. Execute on BOTH
    uae_execute_one();
    unicorn_execute_one();

    // 3. Compare ALL registers
    for (int i = 0; i < 8; i++) {
        assert(uae_get_dreg(i) == unicorn_get_dreg(i));
        assert(uae_get_areg(i) == unicorn_get_areg(i));
    }
    assert(uae_get_pc() == unicorn_get_pc());
    assert(uae_get_sr() == unicorn_get_sr());

    // If ANY differ → STOP and report divergence
}
```

**Role in Project**:
- **Validation tool** to ensure Unicorn correctness
- Caught VBR bug, CPU type bug, interrupt timing issues
- Not for end users, just for development

**Achievement**: ✅ 514,000+ instructions validated with zero divergence

---

## Memory System

### Direct Addressing Mode

BasiliskII uses "direct addressing" for maximum performance:

```
Mac Address              Host Memory
0x00000000 (RAM) ----→   RAMBaseHost + 0x00000000
0x40800000 (ROM) ----→   ROMBaseHost

# Simple arithmetic:
host_ptr = mac_addr + MEMBaseDiff
```

**Benefits**:
- Fast: No table lookup, just pointer arithmetic
- Simple: Direct memory access
- Native: C++ can work with Mac memory directly

**Drawbacks**:
- Requires contiguous memory allocation
- Less flexible than banking

See [deepdive/MemoryArchitecture.md](deepdive/MemoryArchitecture.md) for details.

### Endianness Handling

**Problem**: M68K is big-endian, x86 is little-endian

**UAE Approach**:
- RAM stored in little-endian (host native)
- ROM stored in big-endian (as loaded)
- Byte-swap on every memory access via `get_long()` / `put_long()`

**Unicorn Approach**:
- All memory in big-endian (M68K native)
- No automatic swapping
- Must byte-swap when copying from UAE's RAM

**Implication**: When initializing Unicorn, must byte-swap RAM but NOT ROM!

See [deepdive/UaeQuirks.md](deepdive/UaeQuirks.md) for details.

---

## Trap and Exception System

### Three Types of Traps

#### 1. EmulOps (0x71xx)
**Purpose**: Illegal instructions that call emulator functions

**How It Works**:
```assembly
# ROM originally had:
_OpenDriver:  ; ... many instructions ...

# BasiliskII patches ROM to:
_OpenDriver:  .word 0x7105  ; EmulOp #5 (EMUL_OP_OPENPATCH)
```

**When CPU executes 0x71xx**:
1. Unicorn raises `UC_ERR_INSN_INVALID`
2. `hook_insn_invalid()` catches it
3. Calls `g_platform.emulop_handler(0x7105)`
4. Emulator function runs (e.g., OpenDriver logic)
5. Returns to Mac code

#### 2. A-line Traps (0xAxxx)
**Purpose**: Mac OS Toolbox calls

**Examples**:
- `0xA9FF` - `_OpenDriver` (device manager)
- `0xA247` - `_SetToolTrap` (trap table manipulation)
- `0xA055` - `_SysError` (display error dialog)

**Handling**: Same as EmulOps but calls `g_platform.trap_handler()`

#### 3. F-line Traps (0xFxxx)
**Purpose**: FPU emulation

**Handling**: Same mechanism, different handler

### Native Trap Execution

When an EmulOp needs to execute 68K code (e.g., device driver):

```c
// Platform API provides backend-specific trap execution:
g_platform.cpu_execute_68k_trap(trap_number, &registers);
```

**Unicorn Implementation** ([cpu_unicorn.cpp:462-548](../src/cpu/cpu_unicorn.cpp#L462-L548)):
1. Save current PC/SR
2. Copy registers to Unicorn
3. Push trap number + return marker (0x7100) on stack
4. Execute until hitting return marker
5. Copy registers back
6. Restore PC/SR

**Key Point**: Unicorn is **self-contained** - no UAE dependency!

---

## Interrupt System

### Platform API Abstraction (c388b229)

**Interrupts are triggered through the Platform API**, eliminating backend-specific global state.

**Timer/Device Code** ([src/platform/timer_interrupt.cpp](../src/platform/timer_interrupt.cpp)):
```c
extern Platform g_platform;
int level = intlev();  // Get interrupt level from Mac hardware state
if (level > 0) {
    g_platform.cpu_trigger_interrupt(level);  // Backend-agnostic
}
```

**Platform API** ([src/common/include/platform.h](../src/common/include/platform.h)):
```c
typedef struct Platform {
    // ... other function pointers ...
    void (*cpu_trigger_interrupt)(int level);  // Trigger M68K interrupt
} Platform;
```

### Backend Implementations

**UAE Backend** ([src/cpu/cpu_uae.c](../src/cpu/cpu_uae.c)):
```c
static void uae_backend_trigger_interrupt(int level) {
    // Set UAE's native interrupt flag
    if (level > 0 && level <= 7) {
        SPCFLAGS_SET(SPCFLAG_INT);  // UAE checks this in do_specialties()
    }
    // UAE's Interrupt() function handles:
    // - Building M68K exception stack frame (SR, PC, Format/Vector)
    // - Setting supervisor mode
    // - Updating interrupt mask
    // - Reading vector table
    // - Jumping to handler
}
```

**Unicorn Backend** ([src/cpu/unicorn_wrapper.c](../src/cpu/unicorn_wrapper.c)):
```c
static volatile int g_pending_interrupt_level = 0;  // 0=none, 1-7=level

void unicorn_trigger_interrupt_internal(int level) {
    if (level >= 1 && level <= 7) {
        g_pending_interrupt_level = level;
    }
}

// In hook_block() - checked at every basic block boundary
if (g_pending_interrupt_level > 0) {
    int intr_level = g_pending_interrupt_level;
    g_pending_interrupt_level = 0;

    // Check interrupt mask in SR
    if (intr_level > current_mask) {
        // Manually execute M68K interrupt sequence:
        // 1. Push PC (4 bytes, big-endian) to stack
        // 2. Push SR (2 bytes, big-endian) to stack
        // 3. Update SR: set supervisor bit, update interrupt mask
        // 4. Read vector from (VBR + (24 + level) * 4)
        // 5. Update PC to vector address
        uc_emu_stop(uc);  // Stop to apply register changes
    }
}
```

### Design Decision: Manual vs QEMU Interrupt Handling

Unicorn uses **manual M68K exception stack frame building** rather than QEMU's `m68k_set_irq_level()`.

**Why not QEMU's function?**
- Requires accessing internal structs: `uc_engine` → `uc_struct` → `CPUState` → `M68kCPU`
- Struct field offsets vary by architecture, compiler, QEMU version
- Including QEMU headers creates complex build dependencies
- Hardcoded offsets are fragile (0x140 caused segfault, actual offset unknown)

**Why manual approach is better:**
- Uses only public Unicorn API (`uc_reg_read`, `uc_mem_write`, etc.)
- Portable across architectures, compilers, Unicorn versions
- Explicit and debuggable - exact M68K interrupt sequence visible
- Matches QEMU's `do_interrupt_m68k_hardirq()` behavior
- Tested and validated with 60Hz timer interrupts

### Interrupt Flow

1. **Timer polling** (checked in CPU execution loops at 60Hz)
   - `poll_timer_interrupt()` checks wall-clock time using `clock_gettime(CLOCK_MONOTONIC)`
   - Called from UAE (every 100 instructions) and Unicorn (every block)
2. **Timer fires** (when 16.667ms elapsed)
   - Calls `g_platform.cpu_trigger_interrupt(level)`
3. **Backend-specific**:
   - **UAE**: Sets `SPCFLAG_INT`, processed by `do_specialties()`
   - **Unicorn**: Sets `g_pending_interrupt_level`, checked by `hook_block()`
4. **M68K interrupt**:
   - Check interrupt mask in SR (level must exceed mask)
   - Build exception stack frame (PC, SR)
   - Update SR (supervisor mode, interrupt mask)
   - Read autovector from vector table (vectors 25-31 for interrupts 1-7)
   - Jump to interrupt handler
5. **RTE instruction** restores PC and SR from stack, returns from interrupt

See [TIMER_IMPLEMENTATION_FINAL.md](TIMER_IMPLEMENTATION_FINAL.md) for timer details.

---

## Backend Selection Flow

### Startup Sequence

```c
// 1. main.cpp parses CPU_BACKEND environment variable
const char *backend = getenv("CPU_BACKEND");  // "uae", "unicorn", "dualcpu"

// 2. Initialize appropriate backend
if (strcmp(backend, "unicorn") == 0) {
    unicorn_backend_init();
} else if (strcmp(backend, "dualcpu") == 0) {
    dualcpu_backend_init();
} else {
    uae_backend_init();
}

// 3. Backend fills in g_platform function pointers
g_platform.cpu_init = unicorn_backend_cpu_init;
g_platform.cpu_execute_one = unicorn_backend_execute_one;
g_platform.cpu_get_pc = unicorn_backend_get_pc;
// ... etc

// 4. Core emulation code uses g_platform
while (running) {
    CPUExecResult result = g_platform.cpu_execute_one();
    // ... handle EmulOps, interrupts, etc.
}
```

---

## Data Flow Diagram

```
┌─────────────────────────────────────────────────────────┐
│ Core Emulation (emul_op.cpp, main.cpp, xpram.cpp)      │
│                                                         │
│  Uses: g_platform.cpu_execute_one()                    │
│        g_platform.emulop_handler()                     │
│        g_platform.trap_handler()                       │
└────────────────────┬───────────────────────────────────┘
                     │
          ┌──────────┴──────────┐
          │  Platform API       │
          │  (function pointers)│
          └──────────┬──────────┘
                     │
     ┌───────────────┼───────────────┐
     │               │               │
┌────▼─────┐  ┌─────▼──────┐  ┌────▼──────┐
│   UAE    │  │  Unicorn   │  │ DualCPU   │
│ Backend  │  │  Backend   │  │ Backend   │
│          │  │            │  │           │
│ cpu_uae  │  │cpu_unicorn │  │cpu_dualcpu│
│   .cpp   │  │    .cpp    │  │   .cpp    │
└────┬─────┘  └─────┬──────┘  └────┬──────┘
     │              │               │
     │              │          ┌────┴────┐
     │              │          │ Calls   │
     │              │          │ BOTH    │
     │              │          └─────────┘
     │              │               │
     ▼              ▼               ▼
┌──────────┐  ┌──────────┐   ┌──────────┐
│UAE M68K  │  │ Unicorn  │   │UAE + UNI │
│Interpret │  │  Engine  │   │ in sync  │
│  (C++)   │  │  (JIT)   │   │          │
└──────────┘  └──────────┘   └──────────┘
```

---

## File Organization

### Core Platform Code
```
src/common/include/
├── platform.h          # Platform API struct
├── cpu_emulation.h     # CPU types, registers
└── main.h              # InterruptFlags, global state

src/common/
└── platform.cpp        # Platform implementation
```

### Backend Implementations
```
src/cpu/
├── cpu_uae.cpp         # UAE backend (fills g_platform)
├── cpu_unicorn.cpp     # Unicorn backend (fills g_platform)
├── cpu_dualcpu.cpp     # DualCPU backend (fills g_platform)
│
├── uae_cpu/            # UAE internals (newcpu.cpp, memory.cpp, etc.)
├── uae_wrapper.cpp     # UAE wrapper + shared interrupt code
├── unicorn_wrapper.c   # Unicorn API wrapper
└── unicorn_validation.cpp  # DualCPU validation logic
```

### Core Emulation (Backend-Agnostic)
```
src/core/
├── emul_op.cpp         # EmulOp handlers (uses g_platform)
├── main.cpp            # Main loop (uses g_platform)
├── xpram.cpp           # XPRAM storage
└── ... other managers
```

---

## Key Takeaways

1. **Platform API is the abstraction boundary** - Everything goes through it
2. **Unicorn is the primary goal** - UAE is legacy, DualCPU is validation
3. **Backends are swappable at runtime** - via `CPU_BACKEND` env var
4. **Hook optimization is critical** - UC_HOOK_BLOCK + UC_HOOK_INSN_INVALID for performance
5. **Native trap execution** - Unicorn is self-contained, no UAE dependency

---

## Related Documentation

- [deepdive/MemoryArchitecture.md](deepdive/MemoryArchitecture.md) - Direct addressing, endianness
- [deepdive/UaeQuirks.md](deepdive/UaeQuirks.md) - UAE memory model, byte-swapping
- [deepdive/UnicornQuirks.md](deepdive/UnicornQuirks.md) - Hook types, register persistence
- [deepdive/InterruptTimingAnalysis.md](deepdive/InterruptTimingAnalysis.md) - Timer interrupt timing
- [deepdive/PlatformAdapterImplementation.md](deepdive/PlatformAdapterImplementation.md) - Detailed platform code
