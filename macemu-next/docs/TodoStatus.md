# TODO Status

Track what's done and what's next.

---

## 🎉 RECENT VICTORY

### Unicorn Batch Execution Performance Bug - FIXED!

**Status**: ✅ RESOLVED (Jan 5, 2026)
**Impact**: 1.93x performance improvement achieved
**Solution**: Patched Unicorn's cpu-exec.c to handle EXCP_RTE before clearing exception_index

**Performance Results**:
- Before fix: 7.56M instructions/sec (single-step execution)
- After fix: 14.56M instructions/sec (batch execution with count=10000)
- Speedup: 1.93x (93% improvement)

**Documentation**:
- [UnicornBatchExecutionRTEBug.md](deepdive/UnicornBatchExecutionRTEBug.md) - Problem analysis
- [UnicornRTEQemuResearch.md](deepdive/UnicornRTEQemuResearch.md) - Solution research
- Commit: `da1383a7` - "Fix Unicorn RTE bug and enable batch execution"

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
- ✅ **RTE (Return from Exception) fix** - Patched Unicorn cpu-exec.c (Jan 5, 2026)
  - Fixed UC_ERR_EXCEPTION crash when returning from interrupts
  - Fixed infinite loop bug preventing batch execution
  - Unicorn now runs 146M+ instructions without crashing
  - **Batch execution enabled**: Using count=10000 for 1.93x performance boost
  - See: [UnicornBatchExecutionRTEBug.md](deepdive/UnicornBatchExecutionRTEBug.md)

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

## Phase 1.5: CPU Divergence Investigation ✅ RESOLVED

### Issue Discovery
- ✅ UAE executes 250k+ instructions successfully
- ✅ Unicorn crashes at ~145k instructions
- ✅ DualCPU shows divergence starting at instruction #3832
- ✅ Root cause identified: **Unicorn skips instructions when interrupt triggered**

### Diagnostic Tools Created
- ✅ Enhanced trace logging with interrupt/EmulOp markers
- ✅ Side-by-side trace analyzer with disassembly (trace_analyzer_v2.py)
- ✅ Event extraction (interrupts triggered/taken, EmulOps)
- ✅ Automated trace runner (run_traces.sh)

### Root Cause Analysis ✅ FIXED
**Bug**: Unicorn's interrupt handling in `hook_block()` called `uc_emu_stop()` which caused instruction skipping.

**Evidence**:
- At instruction #3831: Unicorn receives interrupt trigger
- At instruction #3832 (BEFORE FIX):
  - UAE executes PC=0x0208113A (correct)
  - Unicorn executes PC=0x02081138 (PREVIOUS instruction - skipped ahead!)
- This caused D0 register divergence (0x05 vs 0x00)

**Fix Applied**: Removed `uc_emu_stop()` call, let execution continue naturally after updating PC to interrupt handler.

**Result**: ✅ Perfect synchronization between UAE and Unicorn - all registers match exactly at instruction #3832 and beyond.

### Timer Implementation ✅ COMPLETE
**Original approach**: SIGALRM signal-based timer (126 lines)
- ❌ Failed: Blocked by signal masking (~0.2 Hz instead of 60 Hz)

**Final approach**: Polling-based timer using `clock_gettime(CLOCK_MONOTONIC)` (~60 lines)
- ✅ Simple, fast (~20-50ns overhead)
- ✅ Reliable (can't be blocked)
- ✅ Portable (POSIX)
- ✅ Works for both UAE and Unicorn
- ✅ Verified at 60.0 Hz

**Documentation**: See [TIMER_IMPLEMENTATION_FINAL.md](TIMER_IMPLEMENTATION_FINAL.md), [TIMER_IMPLEMENTATION_COMPARISON.md](TIMER_IMPLEMENTATION_COMPARISON.md)

---

## Phase 1.6: Trace Analysis Tools ✅ COMPLETE (Jan 4, 2026)

### Trace Infrastructure
- ✅ **Enhanced run_traces.sh** - Support for instruction ranges (start-end)
- ✅ **Fixed trace_analyzer.py** - Proper handling of interrupt event markers
  - Separates instructions from events before comparison
  - Prevents false divergence reports from interrupt timing
  - Shows events in context without using them for alignment
- ✅ **CPU type initialization fix** - DualCPU now respects config settings
  - Fixed unicorn_validation.cpp hardcoded 68040
  - Both backends now use CPUType/FPUType from prefs system

### Validation Results
- ✅ UAE vs Unicorn: 98.5% match (29,536 instructions identical)
- ✅ First divergence at instruction #29,537 (timing-related, expected)
- ✅ Both backends execute same instructions in same order
- ✅ Interrupt timing differences confirmed as architectural (not bugs)

---

## Phase 2: WebRTC Integration ✅ COMPLETE (Jan 5, 2026)

### Planning Phase ✅ COMPLETE
- ✅ Merge master branch (WebRTC streaming code)
- ✅ Architecture design (4-thread model - simplified from 7)
- ✅ File migration mapping (180 files mapped)
- ✅ Threading model documented
- ✅ Create comprehensive integration plan

### Configuration System ✅ COMPLETE (Jan 4, 2026)
- ✅ Copy JSON config system (nlohmann/json) - commit 19d871c8
- ✅ Implement JSON config loading/saving
- ✅ Add XDG directory support (~/.config/macemu-next/)
- ✅ Add --config and --save-config CLI options
- ✅ Human-readable CPU names ("68040" vs "4")
- ✅ Complete documentation (JSON_CONFIG.md)
- ✅ Test with all three backends (UAE, Unicorn, DualCPU)
- ✅ WebRTC-specific config options integrated

### Platform API (In-Process Buffers) ✅ COMPLETE (Jan 5, 2026)
- ✅ Design video_output.h / audio_output.h APIs
- ✅ Implement triple buffer (lock-free, atomic operations)
- ✅ Implement audio ring buffer (mutex-protected)
- ✅ Full implementation in src/platform/
- ✅ Integration with emulator core (commit eb850af5)

### WebRTC Server Integration ✅ COMPLETE (Jan 5, 2026)
- ✅ Copy encoders (H.264, VP9, WebP, PNG, Opus) - 13 files
- ✅ Adapt HTTP server for in-process model
- ✅ Create webrtc_server.cpp (coordinator)
- ✅ Create video_encoder_thread.cpp
- ✅ Create audio_encoder_thread.cpp
- ✅ Wire encoders to in-process buffers

### Main Entry Point ✅ COMPLETE (Jan 5, 2026)
- ✅ Create unified main.cpp (573 lines)
- ✅ Launch 4 threads (CPU/Main, Video Encoder, Audio Encoder, WebRTC/HTTP)
- ✅ Implement clean shutdown
- ✅ Signal handling (SIGINT/SIGTERM)

### Build System ✅ COMPLETE (Jan 5, 2026)
- ✅ Update meson.build (add WebRTC dependencies)
- ✅ Add all new source files (encoders, http, storage, utils)
- ✅ Fixed libyuv detection (Ubuntu lacks .pc file)
- ✅ Conditional compilation via -Dwebrtc=true/false
- ✅ **Verified**: Build successful with WebRTC enabled

### Client Migration ⏳ PENDING
- ⏳ Copy browser client (HTML/JS/CSS)
- ⏳ Test end-to-end (browser → WebRTC → emulator)
- ⏳ Update client for new architecture

### Testing & Validation ⏳ NEXT
- ⏳ Functional test: Video encoding works
- ⏳ Functional test: Audio encoding works
- ⏳ Functional test: HTTP API responds
- ⏳ End-to-end: Browser can connect and view emulator

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
19d871c8 - Add JSON configuration system with XDG support (Jan 4, 2026)
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

### Immediate (This Week) ✅ COMPLETE
1. ✅ WebRTC integration planning - DONE (Complete plan created)
2. ✅ Threading architecture design - DONE (4-thread model implemented)
3. ✅ File migration mapping - DONE (180 files mapped)
4. ✅ Phase 2 implementation - DONE (All core components integrated)

### Short-Term (Next 2 Weeks) ✅ COMPLETE
1. ✅ Implement video_output.cpp (triple buffer) - DONE
2. ✅ Implement audio_output.cpp (ring buffer) - DONE
3. ✅ Copy WebRTC encoders - DONE (H.264, VP9, WebP, PNG, Opus)
4. ✅ Create webrtc_server.cpp - DONE
5. ✅ Create unified main.cpp - DONE (573 lines, 4-thread architecture)

### Medium-Term (Next 2 Weeks) - CURRENT FOCUS
1. ✅ Complete WebRTC integration - DONE (commit eb850af5)
2. ✅ Migrate JSON config system - DONE (commit 19d871c8)
3. ⏳ **Copy browser client** (HTML/JS/CSS from web-streaming/)
4. ⏳ **Test end-to-end** (browser → WebRTC → emulator)
5. ⏳ Verify all encoders work (H.264, VP9, WebP, PNG, Opus)
6. ⏳ Document new architecture

### Long-Term (Next Quarter)
1. ⏳ Boot to desktop (Phase 3)
2. ⏳ Full hardware emulation (VIA, SCSI basics)
3. ⏳ Application testing framework
4. ⏳ Performance optimization

### **Current Focus**: Phase 2 Complete - Testing & Client Integration
**Major Achievement**:
- ✅ **WebRTC Integration Complete** (commit eb850af5)
  - 4-thread in-process architecture
  - Lock-free triple buffer for video
  - Mutex-protected ring buffer for audio
  - All encoders integrated (H.264, VP9, WebP, PNG, Opus)
  - Build successful with WebRTC enabled

**Next Steps**:
1. Copy browser client from web-streaming/client/
2. Test video encoding (verify frames flow CPU → encoder → output)
3. Test audio encoding (verify samples flow CPU → encoder → output)
4. Test HTTP API (verify server responds)
5. End-to-end test (browser connects and streams)

---

**Last Updated**: January 5, 2026
**Current Phase**: Phase 2 Complete - Testing & Validation
**Branch**: phoenix-mac-planning
**Focus**: WebRTC integration complete, ready for browser client and functional testing

**Major Milestones**:
- ✅ **RTE Batch Execution Fixed** (commit da1383a7)
  - 1.93x performance improvement (14.56M instructions/sec)
  - Unicorn patched to handle EXCP_RTE correctly
  - 146M+ instructions without crashing
- ✅ **JSON Configuration System** (commit 19d871c8)
  - Human-readable CPU names, XDG support
  - Complete documentation and testing
- ✅ **WebRTC Integration** (commit eb850af5)
  - 4-thread in-process architecture
  - Lock-free video, mutex-protected audio
  - All encoders integrated and building successfully
  - 38 new files, 6 new libraries integrated
