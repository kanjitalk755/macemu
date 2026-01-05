# macemu-next

Modern Mac emulator with Unicorn M68K CPU backend and dual-CPU validation.

---

## What Is This?

**macemu-next** is a clean-room rewrite of the BasiliskII Mac emulator, focused on:

1. **Unicorn M68K CPU** - Fast JIT-compiled 68020 emulation (primary goal)
2. **Dual-CPU Validation** - Run UAE and Unicorn in parallel to catch emulation bugs
3. **Modern Architecture** - Clean platform API, modular design, Meson build
4. **Legacy Support** - UAE backend retained for compatibility

**Current Status**: ✅ Core CPU emulation working, 514k+ instructions validated

---

## Quick Start

### Build
```bash
cd macemu-next
meson setup build
meson compile -C build
```

### Configure
```bash
# Generate default config
./build/macemu-next --save-config

# Edit config file
nano ~/.config/macemu-next/config.json
```

### Run with Unicorn (primary backend)
```bash
CPU_BACKEND=unicorn ./build/macemu-next ~/quadra.rom
```

### Run with dual-CPU validation
```bash
CPU_BACKEND=dualcpu ./build/macemu-next ~/quadra.rom
```

### Run with custom config
```bash
./build/macemu-next --config myconfig.json ~/quadra.rom
```

See **[Commands.md](Commands.md)** for complete build and testing guide.
See **[JSON_CONFIG.md](JSON_CONFIG.md)** for configuration documentation.

---

## Documentation

### Essential Reading
- **[Architecture.md](Architecture.md)** - How the system fits together (Platform API, backends, memory)
- **[ProjectGoals.md](ProjectGoals.md)** - Vision and end goals (Unicorn-first approach)
- **[TodoStatus.md](TodoStatus.md)** - What's done ✅ and what's next ⏳
- **[Commands.md](Commands.md)** - Build, test, debug, trace commands
- **[JSON_CONFIG.md](JSON_CONFIG.md)** - Configuration system (NEW!)

### Deep Dive (Technical Details)
- **[deepdive/](deepdive/)** - Detailed technical documentation on specific subsystems
  - Interrupt timing analysis
  - A-line/F-line trap handling
  - Memory architecture
  - UAE and Unicorn quirks
  - Platform adapter implementation

### Completed Work (Archive)
- **[completed/](completed/)** - Historical documentation of completed fixes and implementations

---

## Project Vision

**End Goal**: Unicorn-based Mac emulator with:
- Fast JIT execution
- Clean, maintainable codebase
- Validated against proven UAE implementation
- Modern build system and tooling

**Current State**: Unicorn backend executes 200k+ instructions with proper trap/interrupt handling

**UAE's Role**: Legacy compatibility and validation baseline (will be retained but Unicorn is the focus)

**Dual-CPU's Role**: Validation tool to ensure Unicorn matches UAE behavior

See **[ProjectGoals.md](ProjectGoals.md)** for detailed vision.

---

## Key Achievements

- ✅ Unicorn M68K backend working (68020 with JIT)
- ✅ EmulOps (0x71xx) - Illegal instruction traps
- ✅ A-line/F-line traps (0xAxxx, 0xFxxx)
- ✅ Interrupt support (timer, ADB)
- ✅ Native trap execution (no UAE dependency)
- ✅ 514k instruction dual-CPU validation
- ✅ VBR register support
- ✅ Efficient hook architecture (UC_HOOK_BLOCK, UC_HOOK_INSN_INVALID)

See **[TodoStatus.md](TodoStatus.md)** for complete checklist.

---

## Current Focus

**Timer Interrupt Timing** - Understanding wall-clock vs instruction-count timing differences

See **[deepdive/InterruptTimingAnalysis.md](deepdive/InterruptTimingAnalysis.md)** for details.

---

## Directory Structure

```
macemu-next/
├── src/
│   ├── common/include/    # Shared headers (sysdeps.h, platform.h)
│   ├── core/              # Core Mac managers (emul_op.cpp, xpram.cpp)
│   ├── cpu/               # CPU backends
│   │   ├── uae_cpu/       # UAE M68K interpreter (legacy)
│   │   ├── cpu_unicorn.cpp     # Unicorn backend (primary)
│   │   ├── cpu_dualcpu.cpp     # Validation backend
│   │   └── unicorn_wrapper.c   # Unicorn API wrapper
│   └── tests/             # Unit and boot tests
├── docs/                  # Documentation (you are here!)
└── meson.build            # Build configuration
```

---

## License

GPL v2 (based on BasiliskII)

## References

- Original BasiliskII: https://github.com/kanjitalk755/macemu
- Unicorn Engine: https://www.unicorn-engine.org/
- M68K Reference: Motorola M68000 Family Programmer's Reference Manual
