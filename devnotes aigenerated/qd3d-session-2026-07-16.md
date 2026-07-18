# QD3D session notes - 2026-07-16 (updated 2026-07-17)

Work on: MechWarrior 2 6500 HUD corruption (**FIXED**, user-confirmed); the
Descent II first-movie **audio** stutter (**FIXED** - native Cinepak + host
stream mixer, user-confirmed audio); and a remaining Descent II first-movie
**hitch** at the mid-movie `SetDepth(16)` (**NOT yet fixed**).

**Descent movie full handoff (do not retread experiments/logging):**  
[`descent-movie-session-2026-07-16.md`](descent-movie-session-2026-07-16.md)
(newest findings: section 37, but 37's "compositor upload divergence" theory is
now DISPROVEN - see CURRENT FOCUS below and the corrections in the "Descent II
stutter - status" section).

---

## CURRENT FOCUS (2026-07-17, ROOT-CAUSED): handoff hitch = 68k-emulator tax; fix = enable PPC JIT

The mid-movie hitch (BOTH video + audio stop ~0.8-1.0s) is now root-caused. The
earlier SetDepth(16) / compositor / host-audio theories are **all disproven** -
see descent-movie **sections 38-39** for the full write-up. Summary:

- **The SetDepth(16) depth switch is CLEAN.** In-place switch already fixed it:
  steady ~260ms present heartbeat across the `Resize depth=132`, no compositor
  Shutdown/Init, no dtUsec spike. NOT the hitch.
- **Method that finally worked:** per-VIA-tick emu-thread wall-time accounting
  (`tickProf`, gated `GFX_TICKPROF_ENABLED`) + a PPC block-execution counter in
  the interpreter hot loop. PC sampling never worked - the cost is DIFFUSE.
- **Result:** slow ticks run wallUs 32000-52000 (normal 16667) = emu thread at
  ~1/3 realtime. **~90% of each slow tick is raw PPC interpreter grind**
  (`unaccUs`); `emulOp`/`native`/`cinepak` buckets are all <=3.5ms. **10-14M PPC
  insns/tick.** Hot pages `0x40c66000`+`0x40b12000` = the ROM 68k emulator;
  **~40-48k emulated 68k insns/tick** at ~130 PPC insns each.

**Root cause: the double-interpretation tax.** Our PPC interpreter runs the ROM's
68k emulator running QuickTime's 68k movie/sound task. A/V both stop because the
emu thread genuinely cannot run ~45k 68k insns/tick at realtime. Not compositor,
not host-audio underrun, not one trap. The specific hammered 68k routine is the
ROM Time Manager queue-drain + VIA re-arm at `0x40832400` (our native TM is
bypassed - QuickTime drives the ROM 68k TM from the timer IRQ path); native
reimplementation is high-risk and deprioritized.

**CHOSEN FIX: enable the PPC JIT (dyngen) under MSVC-x64** to cut the 130x
interpretation cost. Investigation (descent-movie section 39):
- JIT forced OFF for MSVC+64-bit (`CMakeLists.txt:25`). amd64 backend exists but
  `jit-target-dispatch.h:43` selects it on `__x86_64__`, never MSVC `_M_X64`.
- The RUNTIME (`basic-dyngen.cpp`, `ppc-dyngen.cpp`, `jit-cache.cpp`,
  ppc-cpu.cpp block loop) has **ZERO GCC-isms** - it copies precompiled op bytes
  and calls them as function pointers. The GCC-only `*-dyngen-ops.cpp` are NOT
  compiled in the precompiled path.
- Real scope = (a) wire `_M_X64` -> amd64 dispatch, (b) provide **Win64-ABI**
  x86_64 op byte tables (committed x86_64 ops are SysV-extracted; Win64 differs:
  arg regs, callee-saved set, 32-byte shadow space, no red zone; pinned
  REG_CPU/REG_T0-2 = AREG0-3 must map to Win64-safe regs), (c) Win64-correct
  stack-frame/shadow-space in the runtime. NOT a full runtime rewrite.

**Fixes already landed (keep):**
- In-place depth switch (`switch_depth_in_place`, video_sdl2.cpp) - eliminates
  the compositor teardown on the mid-movie 8->16bpp switch. CONFIRMED working.
- `switch_depth_in_place` buffer reuse: `the_buffer` allocated once at 32bpp.
- `do_draw` cadence fix (`gl_compositor.cpp::MetalCompositorPresent`).
- Stray `zz` typo (build break C3646) removed from `nvmemfun.hpp:164`.

**Diagnostics added this session (gated `GFX_TICKPROF_ENABLED`, default 1 - MUST
disable before merge):** tickProf per-tick accounting, PPC/68k block counters,
hot-page runtime dump. `DESCENT_MOVIE_DIAGNOSTICS` set to 0 so accounting isn't
self-contaminated.

**Encoding note:** these .md files must stay valid UTF-8, but PowerShell
`Add-Content` mangles non-ASCII (em dash / arrows became U+FFFD). Append in pure
ASCII, or rewrite the whole file with
`[System.IO.File]::WriteAllText(path, text, [System.Text.UTF8Encoding]::new($false))`.

---

## Production fixes (keep)

1. **RAVE indexed-mesh stride** - `SheepShaver/src/gfxaccel/gl/rave_gl_renderer.cpp`
   (`NativeDrawTriMeshGouraud` / `NativeDrawTriMeshTexture`): TQAIndexedTriangle is
   16 bytes (`triangleFlags` + `vertices[3]`, indices at +4/+8/+12); the GL port
   read a 12-byte packed triple. Matches the Metal renderer and RAVE spec.
   (Not the MW2 bug in the end, but a real defect for any TriMesh client.)

2. **Time Manager width bug** - `BasiliskII/src/timer.cpp` (`PrimeTime`, time==0
   continuation): `tmCount` is written 32-bit by `RmvTime` and can be a negative
   microsecond count; it was re-read with `ReadMacInt16`, mangling value and sign.
   Now `ReadMacInt32`.

3. **Audio prefetch depth** - `BasiliskII/src/SDL/audio_sdl.cpp`: keep **two**
   host blocks queued in the prefetch stream (was one). With one block, any
   interrupt-service delay > one callback period drained the stream to zero and
   the callback emitted audible silence (observed 20-30?ms delays during Descent
   movie startup). Also removed the disproven source-count topology-clear
   experiment (discarded a full block of valid audio on every source change).

4. **60Hz tick integrity** - `SheepShaver/src/Windows/main_windows.cpp`
   (tick producer): VIA ticks that cannot be delivered are now *owed* and
   delivered at 4x rate when possible (cap: 30 = 0.5?s backlog) instead of
   silently dropped. Two loss paths covered: guest nested in interrupt handling
   (`XLM_IRQ_NEST != 0`), and an unconsumed `INTFLAG_VIA` (delivery would merge
   two ticks into one). Delivery itself is never gated on the pending flag -
   doing so wedges boot (the guest doesn't consume interrupts early in startup).

5. **MW2 HUD fix: notice-buffer pixel format** -
   `SheepShaver/src/gfxaccel/rave_draw_context.cpp`, `include/rave_engine.h`,
   `gl/rave_gl_renderer.cpp`:
   - MW2 software-renders HUD text/compass/wireframe via RAVE notice callbacks
     (BufferInitialize=3 / BufferComposite=4) writing **big-endian RGB555**
     into the CPU image buffer regardless of the advertised pixelType (verified
     by dumping the buffer around its callback: 555 pairs, e.g. 0x0340 green).
   - MW2 passes a **stack-allocated TQADevice** to QADrawContextNew, so the
     device pointer dangles by notice time. The format is now resolved once at
     context creation (`RaveDrawPrivate::noticePixelType`) and cached.
   - `RaveDeviceDrawBufferPixelType` defaults are ATI-faithful: RGB16 unless an
     explicit 32-bit Memory device or a 32-bit GDevice says otherwise (the RAGE
     back buffer was always 1555; 8-bit screens still mean a 16-bit 3D buffer).
   - **TODO:** port the same creation-time caching to the Metal path
     (`rave_metal_renderer.mm` / `rave_draw_context.mm`).

6. **Native InterfaceLib Microseconds** - `SheepShaver/src/include/thunks.h`,
   `thunks.cpp`, `kpx_cpu/sheepshaver_glue.cpp`, `macos_util.cpp`:
   new `NATIVE_MICROSECONDS` op (FN=1: the single instruction acts as the whole
   routine, returning via LR; handler reads gpr3 = UnsignedWide* and stores
   hi/lo from host `Microseconds`). `PatchInterfaceLibMicroseconds()` (called
   from `InitCallUniversalProc` at boot) overwrites the first instruction of
   InterfaceLib's Microseconds export. Saves a PPC->MixedMode->68k round trip
   (~60??s) per call for PPC callers. Does **not** help Descent's movie player
   (68k code calling the trap directly), but benefits PPC apps generally.

7. **Microseconds 1?ms coalesce (any active sound source)** -
   `BasiliskII/src/Windows/timer_windows.cpp` (`DESCENT_MOVIE_USEC_COALESCE_US=1000`):
   when `AudioStatus.num_sources >= 1`, coalesce host Microseconds samples to
   1?ms. Keeps QPC cheaper under thrash; **does not** fix intro freeze
   (guest still ~277 A193/tick - descent-movie ?21).

8. **`AudioServicePendingInterrupt` from Microseconds** - still in tree;
   **disproven for this freeze** (d2.log ?21: `audioServiceFromThrash=0`,
   AudioInterrupt hashes advance). Harmless no-op when flag already clear.
   Do **not** treat as the movie fix.

9. **Host ClockGetTime (inline A82A)** - **OFF** (`DESCENT_MOVIE_SOUND_CLOCK_WALL=0`,
   `DESCENT_MOVIE_INLINE_A82A_GETTIME=0`). Sample-site interception and the CI
   split work; the fallback that must run the real `A82A` for `ci40` wedges the
   guest from inside `EmulOp`. See descent-movie **?22.12-?22.13**, **?26**.

10. **`DESCENT_MOVIE_DIAGNOSTICS` default 0** - full dumps stretch thrash badly.

11. **Failed alone:** CLOCK_SAMPLE, global A82A, free GetTime without audio
    service, thrash Delay without host GetTime, hard 1?kHz Sleep rate-limit
    (SourcePB data=0), clearing VIA from thrash, **early TimerInterrupt from
    thrash** (`execute_illegal` abort).

12. **Windows `Delay_usec` pitfall:** `Sleep(usec/1000)` - sub-ms yields are
    no-ops (`Sleep(0)`). ?22.10's every-8th 100?s yield never rate-limited.

13. ~~Status 2026-07-17 night: no audio~~ **RESOLVED 2026-07-17**: inline A82A
    host GetTime now compile-gated off (`DESCENT_MOVIE_INLINE_A82A_GETTIME=0`);
    audio verified restored (agent run: mixer hashes advancing, SourcePB data
    nonzero).

14. **`AudioStreamHostMix` (2026-07-17)** - `BasiliskII/src/audio.cpp` +
    `audio_sdl.cpp` + `audio.h`: the Apple Mixer never services a source whose
    PlaySourceBuffer(start) had an empty PB + moreRtn (plays silence forever).
    Host now consumes/refills such a source via its moreRtn (new 68k thunk in
    adat area) and mixes it into the fetched block. Do **not** call
    completionRtn per buffer (= end-of-stream). See descent-movie **?23**.

15. **Light always-on pulses** (`audioPulse`, `mixPulse`, `vidPulse`,
    `streamHostMix`, one-shot `frameWalk`/`frameDump`) - movie-phase gated,
    capped, a few lines/sec; enough to time-line an intro without
    `DESCENT_MOVIE_DIAGNOSTICS`. See descent-movie ?23.2.

16. **Sound-clock freeze fixes - ALL DISPROVEN, gated OFF (2026-07-17).** New
    switches in `qd3d_init_logging.h`, all default 0 / no-op:
    - `DESCENT_MOVIE_QT_CLOCK_FASTPATH=0` - repatch the QuickTime `'clok'`
      GetTime glue (`jsr $408324C0` @0x00b4b130) to `OP_QT_CLOCK_MICROS`. The
      glue-repatch WORKS (logs `qtClockFastPath patched jsr=0x00b4b130`), but
      cheapening the poll did not move the freeze.
    - `DESCENT_MOVIE_SCLK_OFFSET_US=0` - a +0.9 s offset on that clock. Decisive
      NEGATIVE: freeze unchanged -> the video hold is NOT gated on the sound
      clock. This is the experiment that killed the whole sclk line.
    - `DESCENT_MOVIE_PREFETCH_DEPTH_BLOCKS=2` - tested 12 (drain source ahead);
      no change.
    - `MicrosecondsRaw()` (timer_windows.cpp) - uncoalesced clock; unused when
      fastpath off. `OP_QT_CLOCK_MICROS` handler retained, unreachable when off.
    See descent-movie **?33-?36**. Root cause is native Cinepak, not any clock.

---

## Diagnostics kept behind switches

### Movie investigation (`DESCENT_MOVIE_DIAGNOSTICS`)

Defined in `SheepShaver/src/gfxaccel/include/qd3d_init_logging.h`.

| Macro | Default | Notes |
|-------|---------|--------|
| `DESCENT_MOVIE_DIAGNOSTICS` | **0** (enable only for targeted captures) | Full dumps stretch thrash ~1s->~20s. Had drifted back to 1; reset 2026-07-17. Nothing outside a diagnostic may be defined inside its `#if` - that broke the sound-clock build (descent-movie ?26.1) |
| `DESCENT_MOVIE_SOUND_CLOCK_WALL` | **0** | Host wall time for the sclk CI. Interception works; the mandatory fallback for `ci40` wedges the guest - see descent-movie ?26.3 |
| `DESCENT_MOVIE_CLOCK_FIX` | **0** | **HARMFUL** - never enable |
| `DESCENT_MOVIE_UNPAUSE_PRIME` | **0** | No freeze fix - leave off |
| `DESCENT_MOVIE_USEC_COALESCE_US` | **1000** | ?s coalesce when any source active |

**Where it lives (high level):**

- `SheepShaver/src/emul_op.cpp` - OP_MICROSECONDS: glue, outer clock sample,
  compareSim, policySnap, helperD0, stack dumps
- `BasiliskII/src/audio.cpp` - SoundOp, PlaySourceBuffer pre/post, sclk, anomalies
- `BasiliskII/src/SDL/audio_sdl.cpp` - AudioInterrupt detail when movie phase
- `sheepshaver_glue.cpp` - optional 68k loop dumps (older)

Full log-line index, dump files, and stack layouts:
-> `descent-movie-session-2026-07-16.md` ?13.

### Other QD3D channels (cmake / header)

- Pre-existing `ENABLE_QD3D_WAIT_LOGGING` channel (PrimeTime/TimeTask/etc.)
  unchanged.
- Graphics-log improvements inside `QD3D_GRAPHICS_LOGGING` (frame draw caps,
  DrawBitmap, notice pixel type).
- Removed: MW2 notice-buffer pre/post dump (mystery solved).

---

## Descent II stutter - status (2026-07-17, revised)

**START HERE.** The intro is **stock QuickTime**: `DataCD/intro.mvl` holds
`intro.mve`, a plain `.mov` (Cinepak `cvid` video + `twos` 16-bit PCM audio;
confirmed by parsing the file, section 30). Player `FUN_100782e0` is a thin
`MoviesTask`/`IsMovieDone` shell (section 29).

The original **audio** stutter (first-intro-only ~0.85 s freeze, audio restart)
had TWO fixes that landed and are user-confirmed:
1. **Native Cinepak (`cvid`) decompressor** - host-C decode, same native-dispatch
   pattern as RAVE/DSp/GL; removes the one-time lazy-decompress freeze. (See
   descent-movie ?33-?36; the "FIX (only lever left)" paragraph below is now
   DONE.)
2. **`AudioStreamHostMix`** (fix #14) - the Apple Mixer silently ignored the
   game's phase-1 "streaming" source (empty PB + moreRtn); host now services it.

After those, audio (during normal playback) is solid. **The remaining visible
hitch is BOTH video and audio halting together for ~0.5-1.0s at the guest's
mid-movie `SetDepth(16)` switch.** It is NOT a compositor bug (proven: the
compositor reads the exact memory Cinepak writes; `macRegion == sBufRegion`; the
framebuffer content is genuinely static for the first ~12 Cinepak frames). It is
NOT a guest CPU stall (VIA tick keeps advancing; Cinepak decodes steadily). See
"CURRENT FOCUS" at top of file for the open question (host audio-callback
underrun during the GL reformat, or the movie player's one synchronous
MoviesTask doing SetDepth + PreDecompress negotiation). The lever to actually
kill it is in CURRENT FOCUS.

DISPROVEN (do NOT retry): sound-clock offset (?36, +0.9 s left freeze
unchanged); Microseconds cost (?34); audio drain rate (12-deep prefetch, no
change); CD read latency (0 reads); mixer setup calls (~40 ms); "17x slow"
(realtime, bad tick-unit assumption).

---

### EVERYTHING BELOW IS SUPERSEDED - historical context only

## Descent II stutter - old status (summary only, obsolete)

**Symptom:** ~1?s frozen video + silence on first intro MVE; audio often restarts;
later movies fine. Host underrun and tick loss fixed earlier; freeze remained.

### Root cause (REVISED 2026-07-17 - see descent-movie ?23; supersedes below)

Measured timeline (pulse instrumentation): the game starts a **silent
streaming source** (empty PB + moreRtn - never serviced by the Apple Mixer;
now host-mixed, see fix 14) at movie start, plays video normally ~0.9 s, then
builds the **real music source** (primed 2x0.5 s buffers + sound clock) and
**holds video (~0.8-1.1 s)** until the new audio catches up - that hold is the
stutter; "audio restarts" = the primed source beginning at soundtrack
position 0. The A193/GetTime thrash is the player's wait loop, not the cause;
the "policy fn" (helperD0) is just ClockGetTime in **ms** (polC=1000 is the
timescale). Host streaming shortened the hold 68->50 ticks; the residual hold
scales with guest decode speed. Next levers: decode the PEF's phase-2 wait
condition; JIT.

### Older root-cause class (d2.log ?21 - superseded by the above)

68k movie **policy + outer clock sample** busy-polls a **working**
`ComponentDispatch` GetTime clock (`ci40`, clk~=wall, always skipRange) and
hammers A193 at **~277 calls/tick (~15-20k/s)**. That interpreter cost is the
freeze feel (video/guest CPU starved). **Host audio is not starved** in the
post-?20 log: `AudioInterrupt` runs, hashes advance, `audioServiceFromThrash=0`.

**Explicitly false for live intro:**

- Outer blocked on `t1c`/`t24` - always skipRange
- Sentinels are wall deadlines - zeroing kills audio
- A193 glue is a future deadline - watermark only
- Stuck on `flag52` - never set on rate=1.0 path
- **Audio IRQ wedged by thrash** - disproven ?21 (`audioServiceFromThrash=0`)

**Secondary / run-dependent:** mixer sometimes clears PB data on start
(`PlaySourceBuffer mutated`) - **not** present in the ?21 capture.

**Failed host experiments (do not repeat):** unpause-prime; zero t1c/t24;
CLOCK_SAMPLE RAM patch; START_PB poke; "service audio from Microseconds" as
the freeze fix. Full log write-up: **descent-movie-session ?21**.

### Next real work (OBSOLETE - sclk approach disproven, see top of file + ?36)

This whole sub-section is WRONG: patching the sclk site does not help - a direct
+0.9 s clock offset left the freeze unchanged (?36). The real fix is native
Cinepak. Text kept below only for history.

Audio is solid; that item is **done**. Current state, measured with diagnostics
**off** (earlier ?24/?25 timings were taken with diagnostics on and are void):

- Baseline intro hold: **50 ticks / 0.83 s** (ticks 1674-1724), `mixPulse
  peak=32768`, `AudioStreamHostMix` healthy, 40 s runs with no crash.
- `DESCENT_MOVIE_SOUND_CLOCK_WALL` is **off**: its A82A patch also hits the
  `ci40` video-sync site, and the mandatory fallback to the real trap wedges the
  guest from inside `EmulOp` (wrong run mode - same class as the
  `Execute68kTrap` SIGSEGV). Patched runs die at tick 1700.

1. **Patch only the sclk site** so no fallback is needed - the blocker is the
   fallback, not the interception. The scanner is now reliable (?26.2), and the
   handler already reports `ci` per site, so the right site can be identified and
   the other reverted to `0xA82A`.
2. If one site serves both CIs: restore the original word and re-execute it
   (`*pc -= 2`) instead of running the trap from the handler.
3. Only then re-measure the 50-tick hold.
4. Long-term: 68k/PPC JIT.

---

## Build notes

- Build dir `out/build/x64-QD3D-RelWithDebInfo` was repointed to this tree
  2026-07-16.
- User-side reconfigure often turns cmake QD3D logging OFF; anything that must
  survive needs a **source-level default** (as `DESCENT_MOVIE_*` macros do).
- If `SheepShaver.exe` is locked while running, link/output
  `SheepShaver_diag.exe`.

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set NINJA=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
"%NINJA%" -C out\build\x64-QD3D-RelWithDebInfo SheepShaver
```

## 24. `siSoundClock` continuation (2026-07-17)

A full dedicated note was written at `siSoundClock-descent-notes.md` covering the undocumented `siSoundClock` selector and the new `OP_COMPONENT_DISPATCH` EMUL_OP handler.

### 24.1 New status

- `siSoundClock` CI tracking is implemented in `BasiliskII/src/audio.cpp` and exposed via `AudioGetSoundClockCI()`.
- `SheepShaver/src/emul_op.cpp` now has an `OP_COMPONENT_DISPATCH` handler that intercepts `ComponentDispatch` `GetTime` calls.
- The handler returns host wall time for the tracked sound-clock CI (`0x00810012`) and falls back to the original `A82A` trap for other CIs (e.g., `ci40 = 0x0082000c`).
- `EmulOp()` signature was extended to take `uint32 *pc`.
- `Execute68kTrap(0xA82A, r)` was tried and crashed (SIGSEGV); the working fallback is a ROM scratch thunk (`A82A; jmp abs.l`) in `COMPONENT_DISPATCH_PATCH_SPACE`.
- Patch scanner was broadened to scan both the discovered spin PC and the caller/`getTime` candidate over a 1 KiB window and patch every matching site.

### 24.2 Validation

- `detect_audio.ps1` now reports `audioDetected=True` consistently.
- When the patch hits the GetTime helper, both `ci40` and the sound-clock CI are intercepted correctly.
- Scanner reliability is still being tuned; some runs patch the wrong site or miss the helper entirely.

### 24.3 Next work

- Make the A82A patch scanner robust enough to hit the GetTime helper on every run, or switch to a global A82A rebind if that proves simpler.
- Verify by ear/eye that the phase-2 freeze is gone and that video sync is acceptable.

See `siSoundClock-descent-notes.md` and the appended section in `descent-movie-session-2026-07-16.md` for detailed mechanics.
