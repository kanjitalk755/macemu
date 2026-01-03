# macemu Project Structure

## Codebase Architecture

This repository contains **two versions** of the macemu emulator:

### 🔴 Legacy Code (Current Master Branch)
**Location**: `BasiliskII/` and `SheepShaver/` directories

- **Status**: Stable, production-ready, actively maintained for bug fixes
- **CPU Emulation**: UAE (68k) and KPX (PowerPC)
- **Platforms**: Unix/Linux, macOS, Windows, FreeBSD, Solaris, Irix, AmigaOS, BeOS
- **Build System**: Autotools (configure/make)
- **Architecture**: Monolithic, mature codebase (~20+ years of development)

### 🟢 New Code (Rewrite Branch - In Development)
**Location**: `macemu-next/` directory (separate branch)

- **Status**: Major rewrite in progress, not production-ready
- **CPU Emulation**: **Qemu** (replacing UAE and KPX)
- **Platforms**: macOS, Windows, Linux **only** (reduced scope)
- **Build System**: Modern build system (TBD)
- **Architecture**: Refactored, modernized codebase

### 🔵 Shared Code (Used by Both)
**Location**: `web-streaming/` directory

- **IPC System**: Protocol v4+ (shared memory, Unix sockets, eventfd)
- **WebRTC Server**: Standalone server process (H.264/AV1/PNG encoding)
- **Audio**: Opus encoding with resampling
- **Status**: Active development, works with both legacy and new emulators

## Directory Layout

```
macemu/
├── BasiliskII/          # 🔴 Legacy 68k Mac emulator
│   └── src/
│       ├── Unix/        # Linux/Unix platform code
│       ├── Windows/     # Windows platform code
│       ├── MacOSX/      # macOS Xcode project
│       ├── SDL/         # SDL2/SDL3 backends
│       ├── uae_cpu/     # UAE 68k CPU emulator (legacy)
│       ├── IPC/         # IPC drivers (connects to web-streaming)
│       └── ...
│
├── SheepShaver/         # 🔴 Legacy PowerPC Mac emulator
│   └── src/
│       ├── kpx_cpu/     # KPX PowerPC CPU (legacy)
│       └── ...
│
├── web-streaming/       # 🔵 WebRTC streaming (shared)
│   ├── server/          # Standalone WebRTC server
│   ├── client/          # Browser client (JS/HTML)
│   └── libdatachannel/  # WebRTC library (submodule)
│
├── macemu-next/         # 🟢 New rewrite (separate branch)
│   └── [Qemu-based emulator - in development]
│
├── cxmon/               # Debugger/monitor tool
├── docs/                # Documentation
└── .claude/             # Claude Code agents
    └── agents/
        ├── *-legacy-*.md     # Agents for legacy code
        ├── ipc-specialist.md # Works with both
        ├── webrtc-expert.md  # Works with both
        └── crash-debugger.md # Works with both
```

## Agent Classification

### Legacy-Only Agents (BasiliskII/SheepShaver)
- **legacy-rom-patcher** - ROM patching system
- **uae-cpu-expert** - UAE 68k CPU emulation
- **kpx-cpu-expert** - KPX PowerPC CPU emulation
- **legacy-platform-integration** - Multi-platform support

### Shared Agents (Both Legacy and New)
- **ipc-specialist** - IPC protocol v4+
- **webrtc-expert** - WebRTC streaming server
- **performance-optimizer** - Profiling and optimization
- **crash-debugger** - Debugging tools

### New-Only Agents (macemu-next)
- *To be created when new codebase is ready*
- Will focus on Qemu integration
- Modern architecture patterns

## When Working on Code

### ✅ For Legacy Code (BasiliskII/SheepShaver)
1. Verify you're in `BasiliskII/` or `SheepShaver/` directories
2. Use legacy-specific agents (UAE, KPX, ROM patcher)
3. Test on multiple platforms (wider platform support)
4. Follow existing autotools build system
5. Expect UAE/KPX CPU cores

### ✅ For New Code (macemu-next)
1. Check you're on the rewrite branch
2. Work in `macemu-next/` directory
3. Use Qemu-focused agents (when available)
4. Target macOS/Windows/Linux only
5. Expect Qemu CPU emulation

### ✅ For Shared Code (web-streaming)
1. Works with both legacy and new emulators
2. Use IPC/WebRTC agents
3. Maintain backward compatibility with legacy
4. Test with both emulator versions

## Branch Strategy

- **master**: Legacy code (BasiliskII/SheepShaver) - stable
- **rewrite**: New code (macemu-next) - in development
- **web-streaming**: Shared across branches

## Migration Path

The project is transitioning:
- 🔴 **From**: UAE (68k) + KPX (PowerPC)
- 🟢 **To**: Qemu (unified CPU emulation)
- 🔵 **Keeping**: IPC system, WebRTC streaming (already modern)
