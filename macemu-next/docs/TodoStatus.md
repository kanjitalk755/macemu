# TODO Status

Track what's done and what's next.

---

## Phase 1: Core CPU Emulation ✅ COMPLETE

### Build System
- ✅ Meson build configuration
- ✅ UAE CPU compilation
- ✅ Unicorn integration (git submodule)
- ✅ Backend selection via Meson options

### Memory System
- ✅ Direct addressing mode
- ✅ ROM loading (1MB Quadra 650 ROM)
- ✅ RAM allocation (configurable size)
- ✅ Endianness handling (UAE LE RAM, BE ROM)
- ✅ Byte-swapping when copying to Unicorn

### UAE Backend
- ✅ Full 68020 interpreter integrated
- ✅ Memory system (mem_banks, get_long/put_long)
- ✅ Exception handling (A-line, F-line traps)
- ✅ EmulOp support (0x71xx traps)
- ✅ Interrupt processing (SPCFLAG_INT)

### Unicorn Backend
- ✅ Unicorn engine initialization
- ✅ Memory mapping (RAM, ROM, dummy regions)
- ✅ Register access (D0-D7, A0-A7, PC, SR)
- ✅ **VBR register support** (added missing API, commit 006cc0f8)
- ✅ **CPU type selection fix** (68020 not 68030, commit 74fbd578)
- ✅ **Hook architecture optimization** (UC_HOOK_BLOCK + UC_HOOK_INSN_INVALID)
- ✅ EmulOp handling (0x71xx traps)
- ✅ A-line/F-line trap handling (0xAxxx, 0xFxxx)
- ✅ **Interrupt support** (UC_HOOK_BLOCK for efficiency, commit 1305d3b2)
- ✅ **Native 68k trap execution** (no UAE dependency, commit d90208dc)
- ✅ **Legacy API removal** (~236 lines, commit ebd3d1b2)

### DualCPU Backend
- ✅ Lockstep execution (UAE + Unicorn)
- ✅ Register comparison after each instruction
- ✅ Divergence detection and logging
- ✅ Trace history (circular buffer)
- ✅ **514,000+ instruction validation** (commit 155497f0)

### Platform API
- ✅ Platform struct with function pointers
- ✅ Backend-independent core code
- ✅ Runtime backend selection (CPU_BACKEND env var)
- ✅ Trap handlers (emulop_handler, trap_handler)
- ✅ **68k trap execution API** (cpu_execute_68k_trap)
- ✅ **Interrupt abstraction** (cpu_trigger_interrupt, commit c388b229)

---

## Phase 2: WebRTC Integration 🎯 CURRENT FOCUS

### Planning Phase ✅ COMPLETE
- ✅ Merge master branch (WebRTC streaming code)
- ✅ Architecture design (7-thread model)
- ✅ File migration mapping (180 files mapped)
- ✅ Threading model documented
- ✅ Create comprehensive integration plan

### Platform API (In-Process Buffers)
- ⏳ Design video_output.h / audio_output.h APIs
- ⏳ Implement triple buffer (lock-free, atomic operations)
- ⏳ Implement audio ring buffer (mutex-protected)
- ⏳ Unit tests for buffers
- ⏳ Integration with emulator core

### WebRTC Server Integration
- ⏳ Copy encoders (H.264, VP9, WebP, Opus) - 13 files
- ⏳ Adapt HTTP server (remove IPC calls)
- ⏳ Create webrtc_server.cpp (coordinator)
- ⏳ Create video_encoder_thread.cpp
- ⏳ Create audio_encoder_thread.cpp
- ⏳ Wire encoders to in-process buffers

### Configuration System
- ⏳ Copy JSON config system (nlohmann/json)
- ⏳ Migrate prefs conversion logic
- ⏳ Update all code to use JsonConfig
- ⏳ Test config load/save/hot-reload

### Main Entry Point
- ⏳ Create unified main.cpp
- ⏳ Launch all 7 threads
- ⏳ Implement clean shutdown
- ⏳ Signal handling (SIGINT/SIGTERM)

### Build System
- ⏳ Update meson.build (add WebRTC dependencies)
- ⏳ Add all new source files
- ⏳ Test build on clean system
- ⏳ Update documentation

### Client Migration
- ⏳ Copy browser client (HTML/JS/CSS)
- ⏳ Test end-to-end (browser → WebRTC → emulator)

### Legacy Code Removal
- ⏳ Delete IPC layer (~3,000 lines)
- ⏳ Delete legacy drivers (~10,000 lines)
- ⏳ Clean up build artifacts

---

## Phase 3: Boot to Desktop ⏳ FUTURE (After WebRTC Integration)

### Hardware Emulation (Basic)
- ⏳ VIA timer chip basics
- ⏳ SCSI stubs (enough for boot)
- ⏳ Video framebuffer basics

### Boot Testing
- ⏳ Boot Mac OS 7.0 to desktop
- ⏳ Mouse cursor visible
- ⏳ Basic responsiveness

---

## Phase 4: Application Support ⏳ FUTURE

### Full Hardware Emulation
- ⏳ VIA (Versatile Interface Adapter) complete
- ⏳ SCSI (disk access) functional
- ⏳ Video (framebuffer, display modes)
- ⏳ Audio (sound output)
- ⏳ Serial (modem, printer ports)
- ⏳ Ethernet (networking)

### ROM Patching
- ⏳ Identify all ROM patches needed
- ⏳ Implement trap optimization
- ⏳ Mac OS API emulation completeness

### Application Testing
- ⏳ HyperCard stacks run
- ⏳ Classic game playable (e.g., Dark Castle, Marathon)
- ⏳ Productivity software (MacWrite, PageMaker)

### Stability
- ⏳ 30+ minute sessions without crash
- ⏳ Save/restore state
- ⏳ Error recovery

---

## Phase 5: Performance & Polish ⏳ FUTURE

### Performance Optimization
- ⏳ Profile Unicorn backend
- ⏳ Optimize hot paths
- ⏳ JIT tuning
- ⏳ Reduce hook overhead further (if possible)

### User Interface
- ⏳ SDL-based window/input
- ⏳ Preferences UI
- ⏳ Debugger integration (step, breakpoints)

### Testing & CI
- ⏳ Automated testing suite
- ⏳ Regression tests
- ⏳ Continuous integration (GitHub Actions)

---

## Phase 6: PowerPC Support ⏳ FAR FUTURE

### SheepShaver Integration
- ⏳ PowerPC CPU backend
- ⏳ Mac OS 8.5-9.0.4 support
- ⏳ Mixed-mode (68K + PPC) execution

**Note**: Very far out, 68K focus first

---

## Bug Fixes & Investigations

### Completed ✅
- ✅ **VBR corruption** (missing Unicorn register API, commit 006cc0f8)
  - Symptom: VBR reads returned garbage (0xCEDF1400, etc.)
  - Fix: Added UC_M68K_REG_CR_VBR to reg_read/reg_write
  - Impact: +330% execution (23k → 100k instructions)

- ✅ **CPU type mismatch** (enum/array confusion, commit 74fbd578)
  - Symptom: Unicorn created 68030 instead of 68020
  - Fix: Use array indices not UC_CPU_M68K_* enum values
  - Impact: Both backends now correctly create 68020

- ✅ **Interrupt support** (Unicorn ignored interrupts, commit 1305d3b2)
  - Symptom: Divergence at ~29k instructions, crash at ~175k
  - Fix: UC_HOOK_BLOCK for interrupts, shared PendingInterrupt flag
  - Impact: Both backends process timer/ADB interrupts

- ✅ **Platform API interrupt abstraction** (Global state elimination, commit c388b229)
  - Replaced: PendingInterrupt global flag with platform API
  - UAE: Uses native SPCFLAG_INT mechanism
  - Unicorn: Manual M68K exception stack frame building
  - Impact: Backend-agnostic interrupt triggering, cleaner architecture
  - See: docs/deepdive/PlatformAPIInterrupts.md

- ✅ **Hybrid execution crash** (UAE dependency, commit d90208dc)
  - Symptom: Unicorn crashed at 175k when EmulOps called Execute68kTrap
  - Fix: Unicorn-native 68k trap execution
  - Impact: +24,696 instructions (175k → 200k), no UAE dependency

- ✅ **Performance overhead** (UC_HOOK_CODE, commit ebd3d1b2)
  - Symptom: 10x slowdown from per-instruction hook
  - Fix: UC_HOOK_INSN_INVALID for EmulOps, UC_HOOK_BLOCK for interrupts
  - Impact: Expected 5-10x performance improvement

### Active Investigations ⏳
- ✅ **Timer interrupt timing** (wall-clock vs instruction-count) - RESOLVED
  - Status: Fully understood (see deepdive/InterruptTimingAnalysis.md and JIT_Block_Size_Analysis.md)
  - Not a bug, but a design characteristic
  - Decision: **Accept non-determinism** for 5-10x performance gain

- ✅ **Unicorn execution length** (200k limit) - RESOLVED
  - Status: No longer an issue - DualCPU validates indefinitely
  - The "200k limit" was from pre-interrupt-support era (commit 1305d3b2)
  - After native trap execution (commit d90208dc), execution is stable
  - DualCPU now runs without divergence until timeout

---

## Documentation

### Completed ✅
- ✅ README.md - Quick start guide
- ✅ Architecture.md - Platform API, backend abstraction
- ✅ ProjectGoals.md - Vision, Unicorn-first focus
- ✅ TodoStatus.md - This file
- ✅ Commands.md - Build, test, trace commands
- ✅ completed/ folder - Archived historical docs
- ✅ deepdive/ folder - Detailed technical docs
  - ✅ PlatformAPIInterrupts.md - Interrupt abstraction design & implementation

### Needed ⏳
- ⏳ Testing guide (functional testing approach)
- ⏳ Contributing guide (code style, PR process)
- ⏳ Troubleshooting guide (common issues, solutions)

---

## Recent Commits (Dec 2025 - Jan 2026)

```
c388b229 - Platform API interrupt abstraction (Jan 4, 2026)
a3712b98 - WebRTC integration planning (initial documents)
309d4fab - Merge master branch with WebRTC improvements
66f5d428 - Resolve "200k execution limit" investigation
1ddf847d - Claude instructions with Michael's preferences
30d604ee - JIT block size measurement and analysis
74347217 - Add interrupt timing divergence analysis and reorganize documentation
449d34bf - Document interrupt timing divergence root cause analysis
d90208dc - Implement Unicorn-native 68k trap execution to eliminate UAE dependency
ebd3d1b2 - Remove legacy per-CPU hook API and UC_HOOK_CODE implementation
1305d3b2 - WIP: Interrupt support implementation (needs optimization)
```

---

## Next Actions

### Immediate (This Week)
1. ✅ WebRTC integration planning - DONE (Complete plan created)
2. ✅ Threading architecture design - DONE (7-thread model documented)
3. ✅ File migration mapping - DONE (180 files mapped)
4. ⏳ Begin Phase 2 implementation (Platform API)

### Short-Term (Next 2 Weeks)
1. ⏳ Implement video_output.cpp (triple buffer)
2. ⏳ Implement audio_output.cpp (ring buffer)
3. ⏳ Copy WebRTC encoders
4. ⏳ Create webrtc_server.cpp
5. ⏳ Create unified main.cpp

### Medium-Term (This Month)
1. ⏳ Complete WebRTC integration
2. ⏳ Migrate JSON config system
3. ⏳ Remove IPC layer
4. ⏳ Remove legacy drivers
5. ⏳ End-to-end testing (browser → emulator)

### Long-Term (Next Quarter)
1. ⏳ Boot to desktop (after WebRTC integration)
2. ⏳ Full hardware emulation (VIA, SCSI basics)
3. ⏳ Application testing framework

### **Current Focus**: Phase 2 - WebRTC Integration
**Planning Complete**:
- ✅ Architecture design (7 threads)
- ✅ File migration plan (180 files mapped)
- ✅ Threading model documented
- ✅ Integration strategy defined

**Next Steps**:
1. Create src/platform/ directory
2. Implement VideoOutput class (triple buffer)
3. Implement AudioOutput class (ring buffer)
4. Unit test buffers
5. Copy WebRTC encoders

---

**Last Updated**: January 3, 2026
**Current Phase**: Phase 2 (WebRTC Integration)
**Branch**: phoenix-mac-planning
**Focus**: Implementing in-process video/audio buffers, integrating WebRTC server

**Major Milestone**: Planning phase complete - comprehensive integration plan created with:
- 7-thread architecture design
- File-by-file migration mapping (180 files)
- Clear implementation phases (10-14 days estimated)
- Success criteria and testing strategy
