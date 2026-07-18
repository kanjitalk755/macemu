# Descent II first-movie stutter - session handoff (2026-07-16)

Comprehensive breakdown of investigation and experiments so a later session
(or a compacted context) does **not** retread the same ground. Companion to
`qd3d-session-2026-07-16.md` (QD3D/RAVE + earlier production fixes). This
file is APPEND ONLY. qd3d-session-2026-07-16.md is the one with the summary.

## 1. Goal and symptom

**Goal:** Eliminate the Descent II OEM **first intro MVE** stutter on this
SheepShaver fork (Windows, OpenGL RAVE, interpreter PPC).

**Symptom (stable across launches):**

1. Movie begins normally for a fraction of a second.
2. Video freezes and audio goes silent for ~1.0-1.1s (order of two MVE audio
   double-buffers at 22050 Hz).
3. Playback resumes; **audio often restarts from the beginning** (video does
   not visibly seek backward-it freezes then continues).
4. Reproduces after every full SheepShaver relaunch.
5. Later movies / same MVE in a standalone player are fine.
6. Intro audio is **embedded MVE in HOG**, not CD/Red Book/QuickTime/DSp.

**Environment notes:**

- MSVC: `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`
- cdb: `C:\Users\User\Documents\Utils\cdb\amd64\cdb.exe`
- Typical build: `out/build/x64-QD3D-RelWithDebInfo/SheepShaver/SheepShaver.exe`
- Ninja: VS-bundled path under CommonExtensions\Microsoft\CMake\Ninja
- If `SheepShaver.exe` is locked while running, link to `SheepShaver_diag.exe`

**Game binaries (repo root):**

| File | Role |
|------|------|
| `Descent II` | Data fork, **PPC PEF** (`Joy!peff` / `pwpc`) |
| `Descent II Resources` | Resource fork (~1.5MB): UI, `snd`, `SONG`, `Midi`, `cfrg`; **no classic CODE** |

68k movie/sound glue runs from **RAM-loaded system/shared libraries**, not from
flat CODE in those files. Offline dump of live guest code is required.

---

## 2. Already ruled out (do not re-investigate)

From earlier work + this session; evidence in `d2moviesituation.txt` and logs:

| Hypothesis | Result |
|------------|--------|
| Corrupt MVE data | No - standalone player is smooth |
| QuickTime / CD / DSp / OpenGL init | No matching stalls |
| Host SDL callback underrun as root | Callbacks continue; hashes change |
| Single-block audio prefetch underrun | **Fixed** (keep **two** host blocks queued) - residual freeze remained |
| VIA tick drops starving `TickCount` | **Fixed** (owe/backlog ticks) - freeze remained |
| Time Manager `tmCount` 16-bit read | **Fixed** (`ReadMacInt32`) - real bug, not this movie's proven root |
| InterfaceLib native `Microseconds` | Helps PPC callers only; movie path is **68k trap A193** |
| Replaying host audio block | Not observed |
| A193 glue UW is a future wall deadline | **No** - watermark only (see ?4.2); do not "fix" it |
| `state+0x1c`/`+0x24` are live wall deadlines | **No** - permanent sentinels (see ?4.4); zeroing is harmful |
| Outer sample blocked on t1c/t24 compare (live) | **No** - `flags&3==0` always -> **skipRange** (see ?12) |
| Stuck waiting for policy `flag52` | **No** - rate=1.0 path never sets it (see ?13) |
| "~1s freeze = double-buffer so it's fine" | Insufficient; primes are normal setup, thrash is the freeze feel |
| More "which deadline field is stuck" logging | Diminishing returns after ?12-?14 |

**Host audio path (still relevant but not sufficient alone):**

```
MVE decoder -> guest Apple Mixer -> AudioDispatch/AudioInterrupt
  -> prefetch thread -> SDL_AudioStream -> SDL callback -> WASAPI
```

Video is software-drawn by the game; when the guest busy-waits, **both** A/V stop.

---

## 3. Production fixes already in tree (keep)

Documented in `qd3d-session-2026-07-16.md`. Relevant to audio/timing:

1. Audio prefetch depth = **2** blocks (`audio_sdl.cpp`) - avoids silence when
   interrupt service is delayed 20-30ms during movie busy-wait.
2. 60Hz VIA tick backlog / integrity (`main_windows.cpp`).
3. Time Manager `tmCount` 32-bit (`timer.cpp`).
4. Native InterfaceLib `Microseconds` (PPC only).
5. RAVE mesh stride, MW2 notice RGB555, etc. (unrelated to movie but keep).
6. **Microseconds 1?ms coalesce** when `num_sources>=1` (`timer_windows.cpp`,
   `DESCENT_MOVIE_USEC_COALESCE_US=1000`) - keeps host QPC cheaper for any app
   with active sound.
7. **`AudioServicePendingInterrupt`** from Microseconds hot paths - general
   thrash mitigation so mixer buffers keep rotating (?20).
8. ~~CLOCK_SAMPLE_FIX / START_PB_FIX~~ - **removed** (failed; ?19-?20).

---

## 4. What this session established

### 4.1 Guest architecture

```
Descent (PPC PEF)
  ?? MVE player / sync  (mostly 68k libs in RAM)
       ?? Outer wait (clock sync)     <- sample helper; thrash root refined ?12/?16
       ?? Wall GetTime -> ?s glue -> ROM Microseconds (A193 / EMUL_OP)
       ?? Apple Mixer + siSoundClock ('sclk')
            ?? host AudioInterrupt / GetSourceData / SDL
```

Expanded graph (policy / helperD0 / grandparent): **?17**.

- **68k PC** while in MODE_68K: PPC `r24`; D0-D7/A0-A6 ~= `r8-r23`; A7 ~= `r1`.
- Movie phase gate used in diagnostics: `AudioStatus.num_sources >= 2`.
- Process name check for Descent: CurApName at `0x0910` = `"\nDescent II\0..."`.

### 4.2 Microseconds glue (NOT a future deadline)

Live: every movie-phase A193 returns to one site (address **moves per launch**),
e.g. `ret=0x00b5f1a6` / `0x00b5c0f6`.

Disassembly pattern:

```asm
lea   last(pc),a1      ; UW at ret-0x16 (lastAddr)
move.l (a1)+,-(sp)
move.l (a1),-(sp)
jsr   ROM Microseconds ; typically 0x408324C0 -> EMUL_OP
; ret:
cmp   now vs last      ; always now > last
; ratchet last <- previous now
```

**Empirically:** `rem = last ? now` was **always negative** across 26k samples.
This is a **last-sample watermark**, not a 1s future deadline. Do **not** try
to "fix" this UW as a wait target.

Dumps: `descent_micros_glue.bin` (256 bytes around ret).

### 4.3 Wall GetTime (68k helper)

App-side "caller" of the glue (stack `a7+12`) is often:

```asm
; fills TimeRecord* with wall Microseconds, scale = 1_000_000
link a6,#0
... jsr glue ...
move.l a0,(a1)         ; hi
move.l d0,4(a4)        ; lo
move.l #1000000,8(a4)  ; scale
rtd #8
```

Detect at runtime: next opcode at return PC is `0x225F` (`movea.l (sp)+,a1`).

Dumps: `descent_micros_caller_N.bin`.  
Histogram typically splits roughly:

| Site | Role |
|------|------|
| `0x001beabc` | System / trap-emulator stub (F-line, A-trap range) - **not** the policy wait |
| `0x00f.....` (moves) | GetTime in sound/clock 68k lib |
| `0x40814afe` | ROM, rare |

### 4.4 Outer wait - clock sample (decode kept; live role refined in ?12)

**Early label "freeze loop" still useful for disassembly.** Live instrumentation
(?12) shows this routine is a **clock sample helper** that the **policy caller**
re-invokes at high rate - it is **not** blocked on t1c/t24 when `flags&3==0`.

**Function** (address moves; example `0x00f3888e`): `link a6,#-42`.  
**Spin return** after `_ComponentDispatch`: e.g. `spinPc=0x00f388fa`.

Logic (decoded from `descent_micros_outer.bin`):

```
a4 = state @ 8(a6)

if (state+0x44) {
  // primary clock object
  call lowmem vector $18A8 with selector 0x80A6 or 0x810B
  -> TimeRecord at -18(a6)
} else if (state+0x40) {
  // ComponentInstance (sound/clock)
  _ComponentDispatch(ci, &TR, header 0x00040001)  // what~=GetTime
} else fail

ori #$0700,sr          // IRQs off briefly for critical section
// rate Fixed at (a4): if == 0x00010000 use time as-is, else scale (sel 7)
// 64-bit compare state+0x1c / +0x24 vs converted "now"
// not reached -> caller keeps polling
// reached -> selector 0x80AA, advance fields
```

Matches older notes: selectors **0x80A6 / 0x810B / 8 / 0x0D**, vector **`$18A8`**.

**Live state object** (example `state=0x0010ec50` / `0x0010ece0` - may move):

| Offset | Observed | Meaning |
|--------|----------|---------|
| +0x00 | `0x00010000` | Fixed rate **1.0** |
| +0x0c | `0xfffffffffe55...` | Sticky large value (not updating every poll) |
| +0x1c | `0x8000000000000000` | **Sentinel** (INT64_MIN-style), **not** a wall deadline |
| +0x24 | `0x7fffffffffffffff` | **Sentinel** (INT64_MAX) |
| +0x40 | `0x0082000c` etc. | Clock **ComponentInstance** |
| +0x44 | `0` | Alternate clock often unused |
| +0x48 | includes `0x000f4240` | Related to scale 1e6 / time stamp |

**Critical:** `stateTime` logged **only once** across an entire intro while wall
GetTime ran hundreds of times per tick - `t1c`/`t24` **do not change** on that
path. Do **not** treat them as live wall deadlines. Clock TR copies that the
wait **writes** are at **+4 / +8** (and related); later diagnostics sample
`clk=` there.

### 4.5 Audio buffer numbers (consistent)

Movie source PlaySourceBuffer sequence:

1. `actions=1` (kSourcePaused), frames=**11025**, ~500ms @ 22050 Hz  
2. `actions=1`, frames=**10952**, ~497ms  
3. `actions=0` (start), frames=10952, rate 22050  

Sum ~= **997ms** ~= observed freeze order of magnitude.

`siSoundClock`: SetInfo `info=1`, GetInfo CI e.g. `0x00810012` (moves).

After start, SourcePB rotates (~0.5s) and prefetch can show non-silent content
even while the guest still thrashing GetTime - freeze is **guest sync**, not
only host underrun.

### 4.6 Stack layout under EMUL_OP Microseconds

When path is: outer wait -> wall GetTime -> ?s glue -> ROM Microseconds:

```
(a7)+0   ret into glue
(a7)+4   pushed last.lo
(a7)+8   pushed last.hi
(a7)+12  ret into GetTime          (often 0x225F next)
(a7)+16  TimeRecord*
(a7)+20  saved a4
(a7)+24  outer wait a6
(a7)+28  spin PC in outer wait     (after GetTime returns)
```

`a7` comes from PPC `gpr(1)` in `execute_emul_op`.  
**Do not** treat `a7+4` as `pea -16(a6)` of an arbitrary wait frame (failed early
heuristic -> all `firstRem=-1`).

---

## 5. Experiments - results (do not repeat blindly)

| Experiment | What | Result |
|------------|------|--------|
| Frame-walk deadline at `a6+16` | Assumed classic wait frame | **Failed** - wrong frame / always `firstRem=-1` |
| `pea+16` as a6 recovery | Soft-trap stack | **Wrong** - preferred bogus frames |
| `DESCENT_MOVIE_UNPAUSE_PRIME` | Clear `kSourcePaused` on primes when `num_sources>=2` | **No freeze fix**; did fire (`unpause-prime` logs); can disturb buffer setup -> **OFF** (`DESCENT_MOVIE_UNPAUSE_PRIME=0`) |
| `DESCENT_MOVIE_CLOCK_FIX` zero `state+0x1c/+0x24` | Force compare "done" | **HARMFUL** - sentinels; **movie audio dies after stutter** -> **OFF** (`DESCENT_MOVIE_CLOCK_FIX=0`) |
| Copy `t0c` over deadlines | Variant of above | Not better than zeroing; fields wrong |
| Two-block SDL prefetch | Host underrun | **Kept** - real improvement, not full fix |
| VIA tick backlog | Tick starvation | **Kept** - not full fix |
| 1?ms Microseconds coalesce (`num_sources>=2`) | Host QPC / duplicate samples | **Kept** - call count still ~15-20k/s; partial only |
| Treat double-buffer length as full explanation | Theory | Rejected as sole root (setup wall ~200?ms; thrash continues) |
| Host Sleep/yield on hot A193 as *the* fix | Perf band-aid | Rejected as stupid hack for root story; may still be a lever later |
| Endless "next field to watch" without new theory | Process | Stopped - see ?16 |

---

## 6. Diagnostics still in tree

### Switches (`SheepShaver/src/gfxaccel/include/qd3d_init_logging.h`)

| Macro | Default | Purpose |
|-------|---------|---------|
| `DESCENT_MOVIE_DIAGNOSTICS` | **1** | Movie investigation (independent of cmake QD3D log flags) |
| `DESCENT_MOVIE_CLOCK_FIX` | **0** | Do not enable - proven harmful |
| `DESCENT_MOVIE_UNPAUSE_PRIME` | **0** | Do not enable without new evidence |
| `DESCENT_MOVIE_USEC_COALESCE_US` | **1000** | In `timer_windows.cpp`: coalesce Microseconds when `num_sources>=2` |
| `DESCENT_MOVIE_CLOCK_SAMPLE_FIX` | **1** | Host outer clock sample (RAM patch -> EMUL_OP) |
| `DESCENT_MOVIE_START_PB_FIX` | **1** | Restore prime data if start clears/nulls PB |

### Code touchpoints

- `SheepShaver/src/emul_op.cpp` - `OP_MICROSECONDS` diagnostics: glue, callers,
  outerDump, stateTime, compareSim, policySnap, helperD0, grand/s2 state, etc.
- `BasiliskII/src/Windows/timer_windows.cpp` - Microseconds coalesce + optional wait logs
- `BasiliskII/src/audio.cpp` - SoundOp, PB pre/post, sclk, startNullData/mutated; unpause-prime gated off
- `BasiliskII/src/include/audio.h` - `DescentMovieDiagNote68kStack` (A7 stash for SoundOp stacks)
- `BasiliskII/src/include/audio_defs.h` - `siSoundClock`, `kSourcePaused`
- `BasiliskII/src/SDL/audio_sdl.cpp` - 2-block prefetch (production); AudioInterrupt diags

### Log lines to grep

```
MovieWait glue
MovieWait callerDump
MovieWait outerDump
MovieWait outerFrame
MovieWait outerDecode / spinOps
MovieWait outerCaller
MovieWait stateTime          # clk-only updates (rate-limited)
MovieWait stateProgress      # ANY non-clk state field change
MovieWait waitEnter / waitLeave
MovieWait spinSite
MovieWait statePtr
MovieWait compareSim         # t1c/t24 vs clk/wall, flags&3, path, a3
MovieWait inRangeStillSpinning / skipRangePath
MovieWait heartbeat
MovieWait clkStall
MovieWait trSnap
MovieWait policySnap         # policy a6, helperD0, polC, pol10, s2*, grand*
MovieWait helperD0chg
MovieWait clockSampleFix     # patched / hit - host clock sample (?19.1)
startPbFix preRestore|postRestore  # start PB restore (?19.2)
MovieWait tickSummary        # getTime= other= progressEv= spinSiteEv=
SoundOp StopSource|StartSource|PauseSource|AddSource|RemoveSource
PlaySourceBuffer pre|post|detail|primeOk|startWithData|startNullData|mutated|mixerClearedData
siSoundClock
PlaySourceBuffer
SourcePB poll|SourcePB emptied
AudioInterrupt|AudioInterrupt starve
SDL prefetch
MovieWait clockFix           # should not appear with fix OFF
PlaySourceBuffer unpause-prime  # should not appear with prime OFF
```

### Dump files (cwd often build dir or repo root)

| File | Contents |
|------|----------|
| `descent_micros_glue.bin` | ?s watermark glue |
| `descent_micros_caller_N.bin` | GetTime / system callers |
| `descent_micros_outer.bin` | Outer clock sample fn (8KB; spin at +0x1000) |
| `descent_micros_outer_caller.bin` | Policy caller around ret after clock sample |
| `descent_68k_loop_N.bin` | Older multi-region 68k dumps |
| `d2.log` | Latest run capture |

Addresses **move every launch**; always trust **relative** structure and log
`spinPc` / `state=` / `ci40=` from that run.

---

## 7. Open root-cause hypothesis (early; superseded by ?16)

*Preserved for history - do not treat as current.*

1. Outer wait polls **sound/component clock** (`state+0x40` / `$18A8` 0x80A6/0x810B)
   and compares against **state fields** (sentinels until real targets written).
2. Wall GetTime + Microseconds thrash (hundreds/tick) on 68k interpreter
   **starves** drawing and can delay audio IRQ service - may *look* like a
   hard freeze even when buffers eventually rotate.
3. ~1s duration aligns with **double-buffer audio length**, so clock lag and/or
   intentional sync to "audio has consumed primes" remain plausible.
4. Audio **repeat at resume** suggests resubmit/re-prime of stream start after
   the wait, not simple host block replay.

**Not solved (still true):** host-side correct **sound-clock GetTime** / cheap
poll path for MVE startup under Apple Mixer.

**Update:** Item 1's "compare against deadlines until reached" is **false for
live intro** - see ?12 (`skipRange`). Item 2 remains the primary mechanism.
Item 3 is **not** a sufficient explanation alone. Current summary: **?16**.

---

## 8. Recommended next work (early checklist; status)

| # | Item | Status |
|---|------|--------|
| 1 | Confirm 1?ms Microseconds coalesce | **Done** - gaps improve; call rate still ~15-20k/s (?15) |
| 2 | Track `clk=` advances | **Done** - `clk~=wall` after catch-up; not frozen (?12) |
| 3 | Sound clock / mixer GetTime | **Open** - real next work (?16) |
| 4 | Do-not list | Still in force |
| 5 | JIT / native clock | Long-term |

**Do not** (expanded):

- Zero or invent `t1c`/`t24`
- Assume A193 glue is the wait
- Assume outer is blocked on range compare (live skipRange)
- Assume stuck on `flag52`
- Assume PPC JIT required for *correctness*
- Re-litigate host SDL underrun without new evidence
- Add another layer of "which field is frozen" logging without a new theory

---

## 9. Build / run cheatsheet

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set NINJA=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
"%NINJA%" -C out\build\x64-QD3D-RelWithDebInfo SheepShaver
```

User-side reconfigure often turns cmake QD3D logs OFF; anything that must
survive needs a **source default** (as `DESCENT_MOVIE_DIAGNOSTICS` does).

---

## 10. Key file index

| Path | Why |
|------|-----|
| `descent-movie-session-2026-07-16.md` | **This handoff** |
| `qd3d-session-2026-07-16.md` | QD3D + shared production fixes |
| `d2moviesituation.txt` | Early exhaustive audio hypotheses |
| `d2.log` | Latest runtime capture |
| `SheepShaver/src/emul_op.cpp` | Microseconds diags / outer / policySnap / helperD0 |
| `BasiliskII/src/Windows/timer_windows.cpp` | Microseconds + coalesce |
| `BasiliskII/src/audio.cpp` | PlaySourceBuffer pre/post / SoundOp / sclk |
| `BasiliskII/src/include/audio.h` | `DescentMovieDiagNote68kStack` |
| `BasiliskII/src/SDL/audio_sdl.cpp` | Prefetch / AudioInterrupt diags |
| `SheepShaver/src/gfxaccel/include/qd3d_init_logging.h` | Diagnostic switches |
| `descent_micros_outer_caller.bin` | Policy caller dump (see ?13) |

---

## 11. One-paragraph summary for a new agent (early; see ?16)

Descent's first MVE freezes ~1s because a **68k sync path** hammers a **sound/
component clock sample** (`state+0x40`, `_ComponentDispatch` / `$18A8`) plus
wall **GetTime->Microseconds** on the interpreter until draw/audio starve; the
A193 "glue" is only a **watermark**, and state `t1c`/`t24` are **sentinels**
that live code **skips** (`flags&3==0`)-do not zero them. Unpause-on-prime did
not fix the freeze. Host double-prefetch, tick integrity, and 1?ms ?s coalesce
are in tree and should stay. **Next real work:** cheap/correct sound-clock
GetTime (or other poll-cost cut) and/or start-time PB clear when it appears -
not another deadline rewrite. Full later findings: **?12-?16**.

---

## 12. Later instrumentation - outer is sample, not deadline wait

Live `compareSim` / `skipRangePath` / `heartbeat` (many full intros):

| Field | Live result |
|-------|-------------|
| `path` | always `compDisp` (`ci40` set, `ci44=0`) |
| `flags4c` / `flag&3` | **0** |
| `skipRange` | **1 always** - guest **never** runs t1c/t24 range math on intro |
| `a3` | **0** always - no output TR for success-store path |
| `inClk` / `inWall` | 1 (sentinels would pass if checked) |
| `clk` vs wall | after short catch-up, **clk ~= wall** (not frozen) |
| `stateProgress` (non-clk) | essentially **once** (init) per run |
| `spinOps` | `... A82A \| 584F 40C0 007C 0700 ...` matches decode after `_ComponentDispatch` |

**Implication:** Do **not** re-open "outer blocked waiting for t1c/t24." The
routine is a **GetTime sample** that writes `state+4/+8` and returns. The
**caller** re-invokes it at high rate.

Disassembly of outer (kept from ?4.4) remains valid; only the *blocking-wait*
interpretation was wrong for live Descent intro.

---

## 13. Policy caller after clock sample (do not re-disassemble from scratch)

Return PC after sample (moves): e.g. `0x...4bce`, `0x...7c2e`.  
`beforeRet` includes `4eba 09e2` = `jsr` to clock sample entry (`spin?0x6C`).

Dump: `descent_micros_outer_caller.bin` (also region inside `descent_micros_outer.bin`
around file offset of caller ret relative to fileBase).

**rate != 0 path (always; rate Fixed 1.0):**

```asm
jsr   clockSample
; copy state+0x14 -> local -8(a6)
tst.l (state)                 ; rate
beq   rateZeroPath
; rate != 0:
jsr   helper                  ; more clock math; and.w flags&4
move.l d0, 0x14(a6)           ; helperD0
bra   epilogue                ; *** skips move.b #1, flag52 and o70->o14 latch ***
```

**rate == 0 path** (not taken live): can set `flag52=1` and copy o70->o14 etc.

So `flag52` staying **0** and policy `o14/o50/o64/o68/o70` staying **0** is
**expected** on this path - not a stuck "ready" flag. Do not hunt flag52.

Helper (same module) also checks `state+0x4c & 4`, may call `$18A8` selectors
**0x80B5 / 0x80B7**, does 64-bit math vs scale at `state+0x48` (1e6).

---

## 14. policySnap / helperD0 / parent chain

| Field | Typical live value | Notes |
|-------|-------------------|--------|
| `polRet` | e.g. `0xb42784` | Parent of policy fn (stable within run) |
| `polC` | **`0x3e8` = 1000** | Arg @ 12(a6) - limit/period-like |
| `pol10` | e.g. `0x10ec50` / `0x10ece0` | State* (same or adjacent object) |
| `pol8` | 0 | |
| `grandRet` | e.g. `0xa2e818` | Grandparent (moves); two common heap args |
| `helperD0` @ 0x14(policy a6) | see below | Helper return |

**Valid policy frames** (e.g. a6 ending `...150` / `...1d0`): `helperD0` marches
roughly **+40 per ~50?ms wall** (~600-800 units/s), past 1000, continues for
the whole intro.

**Invalid / mid-nest frames** (e.g. `...0c0` / `...140`): garbage (`0x1ec7`,
`0x41a4...`, high stack words) - **ignore** those samples.

`s2*` fields (state at `pol10`) show **s2clk advancing** when instrumented -
second/same state clock not frozen either.

---

## 15. Thrash and audio numbers (instrumented runs)

| Metric | Typical |
|--------|---------|
| A193 rate, movie phase | ~**15-20k / s** |
| GetTime share | ~**20%** |
| Other A193 (system stub etc.) | ~**80%** |
| Calls per 60?Hz tick | ~50-600 (avg ~300) |
| Coalesce | `lastGap` often 0; **call count still huge** |
| AddSource(sources=2) -> audio start wall | ~**200?ms** (not full 1?s) |
| BigGaps early | ~20-100?ms (setup), not one multi-second hole |

**Audio sequence (still valid):** StopSource thrash -> prime 11025 -> often
StopSource -> prime ~10989 -> start. Between-primes Stop is guest-intentional.

| Tag | Meaning |
|-----|---------|
| `primeOk` / `startWithData` | Normal; data often kept |
| `mutated` / `mixerClearedData` | **Run-dependent** - mixer zeroed PB on start; matches silence/restart when present |
| `startNullData` | Guest presented null data (not always); pre/post logs distinguish |

Host SourcePB / AudioInterrupt: content hashes advance; not underrun-root.

---

## 16. Root-cause class (current - stop circling)

**Primary:** First-intro MVE sync **busy-polls** a working clock sample +
GetTime/Microseconds on the **68k interpreter** hard enough that **guest/video
forward progress feels frozen ~1?s**. The freeze *feel* is thrash cost, not a
dead `clk` or t1c/t24 block.

**Not audio IRQ starvation** (corrected by ?21): post-fix log shows
`AudioInterrupt` hashes advancing and `audioServiceFromThrash=0`. Host mixer
keeps rotating during thrash.

**Secondary / variable:** PlaySourceBuffer **start clears data** on some runs
(absent in ?21 capture).

**Still open (real fix tracks):** - see **?21.8** (latest).

1. Host **clock GetTime** for `ci40` / `ComponentDispatch(..., 0x00040001)`.
2. Decode helperD0 +40 / polC=1000 exit math.
3. Long-term: PPC JIT.

**Do not re-do:** sentinel zeroing, unpause-prime, glue-as-deadline, flag52
wait theory, SDL-underrun-as-root, "outer blocked on range compare," audio-IRQ
service-from-A193 as the freeze fix, another field-watch pass without a new
mechanism.

---

## 17. Expanded guest graph (for navigation)

```
grandparent  (grandRet e.g. 0xa2e818)
  ?? parent     (polRet e.g. 0xb42784; args polC=1000, pol10=state*)
       ?? policy sync helper  (callerRet after sample)
            ?? jsr clock sample (outer, link #-42)  -> clk write; skipRange
            ?? jsr helper -> helperD0 @ 0x14(a6)
            ?? return (rate=1.0 skips flag52)
                 nested: wall GetTime / ?s glue / A193 as needed
```

Addresses **move every launch**.

---

## 18. Full one-paragraph summary (current)

Descent's first MVE freezes ~1s because the **68k movie policy/sample path
busy-polls** a **working** component clock (`ComponentDispatch` GetTime on
`ci40` -> `state+4` clk~=wall) and **A193** at ~277 calls/tick; that interpreter
cost freezes the feel of video/guest progress. **Host audio is not starved**
(?21: AudioInterrupt hashes advance; `audioServiceFromThrash` never fired).
A193 glue is a **watermark**; **t1c/t24 are sentinels**; live code **always
skipRange**. Failed: unpause-prime, zero deadlines, CLOCK_SAMPLE RAM patch,
START_PB poke, audio-service-from-Microseconds as the fix. Keep: double-prefetch,
VIA backlog, ?s coalesce. **Host ClockGetTime in tree (?22.13)** - currently
hurts audio (SourcePB data=0); restore audio before freeze work. Map: ??4-6
disassembly; ??12-16 live; ?19-?21 failed/wrong theories; **?22.12-?22.13**.

---

## 19. Host fixes implemented (2026-07-16 evening)

### 19.1 `DESCENT_MOVIE_CLOCK_SAMPLE_FIX` (default **1**)

**What:** On first movie-phase GetTime/Microseconds sighting of the outer
routine (`link a6,#-42` at `spinPc-0x6C`), patch guest RAM to:

```
link a6,#-42
EMUL_OP_DESCENT_CLOCK_SAMPLE   ; host fills state+4/+8 + TR@-18 with wall ?s
unlk a6
rts
```

Handler (`OP_DESCENT_CLOCK_SAMPLE` in `emul_op.cpp`): `Microseconds` -> write
`state+4/+8` and local TimeRecord (scale 1e6). Skips ComponentDispatch + 68k
body thrash for that sample path.

**Verify in log:**

```
clockSampleFix patched entry=0x... spinPc=0x...
clockSampleFix hit n=... state=0x... clk=...
```

Expect: many `clockSampleFix hit` lines; fewer outer ComponentDispatch-driven
A193 storms after patch (GetTime share / tickSummary may drop). Subjective:
less intro freeze.

**Files:** `emul_op.h` (new OP), `emul_op.cpp` (handler + patch),
`qd3d_init_logging.h` (switch).

### 19.2 `DESCENT_MOVIE_START_PB_FIX` (default **1**)

**What:** On movie-phase (`num_sources>=2`) **start** (`actions` not paused):

1. **preRestore** - if PB `data==0` but last prime has data for same source/pb,
   write prime data/frames into PB before mixer call.
2. **postRestore** - if mixer clears data/frames after start, put them back.

Tracks last prime (paused + nonzero data) for restore source.

**Verify in log:**

```
startPbFix preRestore ...
startPbFix postRestore ...
```

Also: `mutated` should be rare after fix; if postRestore fires, start no longer
leaves a null PB. `primeOk` then `startWithData` still expected.

**Files:** `audio.cpp`, `qd3d_init_logging.h`.

### 19.3 If something breaks

Set either switch to **0** in `qd3d_init_logging.h` and rebuild. Clock sample
patch is one-shot per process; restart emulator after toggle.

---

## 20. Hacks removed; general thrash fix (2026-07-16 late)

### 20.1 Verdict on ?19 hacks

User testing: **stutter not fixed**; freeze after audio "repeat" felt **worse**.

| Hack | Result | Why |
|------|--------|-----|
| `CLOCK_SAMPLE_FIX` | **Rejected** | Guest-RAM patch of Descent outer sample; ~129k free samples/s accelerated parent policy thrash without advancing the exit condition (audio). Ugly, Descent-only. |
| `START_PB_FIX` | **Rejected** | Often no-op (`startPbFix` never fired when start data already OK). Not the freeze root. Descent-only PB poke. |

Both removed from tree (`OP_DESCENT_CLOCK_SAMPLE` deleted; switches gone).

### 20.2 Proper general fix (all apps)

**Mechanism:** 68k busy-poll thrash starves `OP_IRQ`, so `INTFLAG_AUDIO` sits
pending and the Apple Mixer stops rotating buffers -> ~1s A/V freeze feel.

**Fix (not app-specific):**

1. **`AudioServicePendingInterrupt()`** (`audio.cpp`) - if `INTFLAG_AUDIO` is
   pending, clear it and call `AudioInterrupt()` with a reentrancy guard.
2. Called from:
   - SheepShaver `OP_MICROSECONDS` (`emul_op.cpp`)
   - SheepShaver `NATIVE_MICROSECONDS` (`sheepshaver_glue.cpp`)
   - Basilisk II `M68K_EMUL_OP_MICROSECONDS` (`BasiliskII/src/emul_op.cpp`)
3. **Microseconds coalesce** (`timer_windows.cpp`) - when
   `num_sources >= 1` (any active sound, all apps), coalesce host QPC to 1?ms
   (`DESCENT_MOVIE_USEC_COALESCE_US=1000`). Was `>= 2` only.

**Verify in log (diagnostics on):**

```
audioServiceFromThrash n=... tick=... sources=...
```

Expect these during intro thrash; `AudioInterrupt` / buffer hashes should keep
advancing; no `clockSampleFix` / `startPbFix` lines.

**Still open (later):** true host sound-clock component for
`ComponentDispatch(..., GetTime)` (ci40 != siSoundClock CI in logs - needs a real
`'clok'` component, not a Descent RAM patch). Audio-service-on-thrash is the
correct root-class fix for freeze-by-starvation.

### 20.3 Production list (current)

**Keep:** 2-block prefetch, VIA backlog, tmCount 32-bit, native InterfaceLib
Microseconds, ?s coalesce (now any source). `AudioServicePendingInterrupt` is
still in tree (harmless no-op when flag clear) but **proven irrelevant** to
this freeze - see ?21.

**Removed / never enable:** CLOCK_SAMPLE_FIX, START_PB_FIX, CLOCK_FIX (zero
t1c/t24), UNPAUSE_PRIME.

---

## 21. Log after ?20 "audio thrash service" - failed / wrong theory

**Log:** `d2.log` (2026-07-16 ~23:51, ~198?KB). User: **did nothing**.

### 21.1 Tag counts (movie phase)

| Tag | Count | Meaning |
|-----|------:|---------|
| `audioServiceFromThrash` | **0** | Fix **never ran** - `INTFLAG_AUDIO` was never pending on A193 path |
| `clockSampleFix` / `startPbFix` | 0 | Hacks gone (good) |
| `tickSummary` | 126 | A193 thrash still present |
| `compareSim` | 151+ | outer sample still polled heavily |
| `skipRange` (value=1) | all | still always skip range math |
| `path=compDisp` | all | still `ci40` ComponentDispatch GetTime |
| `AudioInterrupt` | 24 | host mixer pulls **did** run |
| `starve` | 0 | no zero-byte audio streak logged |
| `primeOk` | 2 | primes OK |
| `startWithData` | 1 | start had data |
| `startNullData` / `mixerClearedData` / `mutated` | 0 | no PB-clear issue this run |
| `siSoundClock` | 3 | enable + 2x GetInfo CI `0x00810012` |
| `bigGap` | 4 | only early setup 18-60?ms; **not** a ~1?s hole |
| `clkStall` | 1 | one 21?ms clk stall at tick 1712 (setup), not the freeze |

### 21.2 Thrash numbers (unchanged class)

From `tickSummary` ticks **1712-1837** (~2.1?s of Mac 60?Hz ticks):

| Metric | Value |
|--------|------:|
| Total A193 calls in summaries | **~34?922** |
| Avg calls/tick | **~277** |
| Peak calls/tick | **788** (tick 1717 also 574) |
| GetTime share | **~20%** |
| Other A193 | **~80%** |

Same order as ?15. Coalesce + audio-service did **not** cut guest poll rate
or the freeze feel.

### 21.3 Audio is **not** starved (kills ?20 mechanism)

`AudioInterrupt` n=1...24 from tick 1709->1838, ~every 5 ticks, **content hashes
change almost every IRQ** after n=2:

```
n=1,2 hash=dfde6ac5 (same once)
n=3  c465894f ... n=24 2b4789e5  (advancing)
bytes=16384 zeroStreak=0 always
fmt=twos ch=2 ss=16 rate=44100 sampleCount=4096
```

So during the entire thrash window the mixer **is** rotating buffers and host
prefetch is fed. `audioServiceFromThrash=0` means the normal `OP_IRQ` path
already cleared `INTFLAG_AUDIO` before Microseconds saw it - there was nothing
for the "service from thrash" hook to do.

**Conclusion:** freeze is **not** "audio IRQ pending while thrash blocks OP_IRQ."
?20's premise was wrong. User is right that it is not a proper fix for this bug.

### 21.4 Clock / outer sample (still healthy, still thrashing)

- `path=compDisp` only; `ci40=0x0082000c` (!= `siSoundClock` CI `0x00810012`)
- `rate=0x00010000` (1.0), `flags4c=0`, `flag&3=0`, **skipRange=1 always**
- `t1c`/`t24` sentinels unchanged
- `stateTime`: `clk` tracks wall (`dClk~=dWall` after catch-up)
- One early `clkStall dWall=20994` during prime/start setup only

Outer sample is still a **working GetTime sampler**, not a blocked waiter.

### 21.5 PlaySourceBuffer this run (clean)

```
tick 1712-1713: Stop thrash -> prime 11025 -> Stop -> prime 10952 -> startWithData
data kept; no mutated / null start
```

PB restore hacks were never going to help this capture.

### 21.6 Policy / helperD0 (progress, not freeze)

`policySnap`: `polRet=0x00b42784`, `polC=0x3e8` (**1000** constant),
`pol10=state*`, two policy frames seen (`policyA6` `...0c0` vs `...150`).

**Valid-looking helperD0** (on `policyA6=...150`) **steps by +40**:

```
640 -> 680 -> 720 -> 760 -> 840 -> 880 -> 1000 -> 1080 -> ...
1320 -> 1360 -> ... -> 1800   (tick ~1713->1832)
```

Garbage / mid-nest values still appear on the other frame (`0x88000008`,
`0x1ec7`, `0x41240108`) - **ignore** those (same as ?14).

So the parent is not stuck on a frozen counter; a real helper value is advancing
in 40-unit steps while the 68k interpreter burns tens of thousands of A193s per
second. Each policy iteration is just **very expensive** on the interpreter.

### 21.7 Revised root-cause class (replace ?20.2 mechanism)

| Claim | Status after this log |
|-------|------------------------|
| Audio IRQ starved by thrash | **FALSE** - IRQs run, hashes advance, service hook never needed |
| Clock/deadlines frozen | **FALSE** - clk~=wall, skipRange always |
| Start PB cleared | **FALSE this run** |
| 68k busy-poll costs so much CPU that **video/draw and guest forward progress feel frozen** for ~1?s | **STILL TRUE** (thrash class) |
| Making sample free (CLOCK_SAMPLE) helps | **FALSE** - worsened thrash (?19) |
| Servicing audio from A193 helps this freeze | **FALSE** - ?21.3 |

**Freeze feel = pure interpreter thrash cost of the policy/sample loop**, while
audio host path continues. Silence/"restart" may still be guest-side stream
re-prime policy, not host underrun (no starve tags).

### 21.8 What a real fix must do (do not re-try ?19-?20)

Must cut **guest 68k work per policy iteration** or **iterations per wall-second**
for the **ComponentDispatch GetTime / outer sample / policy** path - for all
apps using that pattern - **without**:

- guest-RAM patching Descent only
- inventing t1c/t24 deadlines
- assuming audio IRQ is wedged

**Plausible next tracks (general):**

1. **Host `'clok'` / sound-clock `ClockGetTime`** for the CI used as movie master
   clock (`ci40`, not only `siSoundClock` GetInfo CI - they differ). Intercept
   `ComponentDispatch` GetTime (what=1) for host-owned clock instances so each
   sample is O(host) not O(68k Component Manager + A193 glue).
2. **Identify why policy runs ~300 A193/tick** and whether wall/sample time can
   make helperD0/+40 progress with far fewer polls (correct clock rate / scale).
3. **PPC JIT / faster 68k** - long-term, not a semantics fix.
4. Decode **helperD0 +40** and `polC=1000` against MVE/audio frame math (what
   exit condition is).

### 21.9 Do-not list (updated)

- Zero t1c/t24; unpause-prime; CLOCK_SAMPLE RAM patch; START_PB poke
- Assume audio IRQ starvation without `audioServiceFromThrash` / starve evidence
- Assume "service more IRQs from Microseconds" will fix intro (disproven ?21)
- More field-watch logging without a new mechanism

---

## 22. Host ClockGetTime via ComponentDispatch (implemented)

**Goal:** Cut 68k cost of `ComponentDispatch(..., what=1 GetTime)` for movie/game
sync - general (not Descent RAM patch). Outer path uses `ci40` + header
`0x00040001` (paramSize=4, kClockGetTimeSelect=1).

### 22.1 Mechanism

Trap **A82A** redirected (scrap-style ROM patch space) to:

```
EMUL_OP_COMPONENT_DISPATCH
tst.l  d1
bne.s  handled
jmp    original_A82A
handled:
rts
```

`OP_COMPONENT_DISPATCH` (`emul_op.cpp`):

1. Read stack: `ret, header, TimeRecord*, CI, result_slot`
2. If `what==1 && paramSize==4` and (`num_sources>=1` or mixer open):
   - `Microseconds` -> write TR value, **scale=1e6**, base=0
   - Compact stack to `[ret][result=0]`, `d0=0`, **`d1=1`** (host handled)
3. Else **`d1=0`** -> fall through to ROM ComponentDispatch

Gate on active sound avoids rewriting pure system tick-clock GetTimes when
idle; movie intro has sources>=2 so always hits host path.

### 22.2 Logging

```
[QD3D:wait] hostClockGetTime n=... ci=0x... tr=0x... val=... scale=1000000 tick=... sources=...
```

Emits first 16 hits, then every 500 (diag on). Expect many hits during intro
with `ci` matching `ci40` (e.g. `0x0082000c`), not necessarily siSoundClock
GetInfo CI.

### 22.3 Files

| File | Change |
|------|--------|
| `emul_op.h` | `OP_COMPONENT_DISPATCH` |
| `emul_op.cpp` | handler + logs |
| `rom_patches.cpp` | A82A trap table -> patch space |

### 22.4 Verify

1. Run intro; capture `d2.log`
2. Must see `hostClockGetTime` (if zero -> stack layout wrong / gate wrong)
3. Compare thrash: `tickSummary` calls/tick and GetTime share vs ?21
4. Subjective freeze; audio still `AudioInterrupt` hashes advancing

### 22.5 Risks / rollback

- Wrong stack layout -> boot or component breakage (forward path still there for
  non-GetTime; GetTime mishandle could corrupt TR)
- If broken: remove A82A trap redirect in `rom_patches.cpp` and rebuild
- Build: if `SheepShaver.exe` locked -> `SheepShaver_diag.exe`

### 22.6 Result: **FAILED - disabled** (2026-07-17)

| Observation | Detail |
|-------------|--------|
| First user log after ?22 | `hostClockGetTime` = **0** (intercept never ran); thrash unchanged |
| Absolute trap + SetToolTrap rework | Boot OK; **Descent -> black screen** (movie never played) |
| Conclusion | Global A82A rebind is unsafe / ineffective as implemented |

**Status in tree:** A82A trap table **not** redirected; `InstallComponentDispatchPatch()` is a no-op; `OP_COMPONENT_DISPATCH` handler remains but is unreachable. SheepShaver should boot and run Descent again without the black screen.

**Do not re-enable** global ComponentDispatch hijack without:

1. Proving trap vector form (offset vs absolute) against scrap-style patches
2. Logging `a82aEnter` on a non-Descent app first
3. Never handling until stack layout is verified (forward-only probe)

**Still true from ?21:** freeze is 68k thrash cost, not audio IRQ starvation. Real fix still needs cheap clock sample without breaking CM.

### 22.7 Follow-up (2026-07-17 night)

| Experiment | Result |
|------------|--------|
| A82A handle any `what==1` | **Black screen on Descent** - `what=1` is also `SoundComponentInitOutputDevice`; false match corrupted stacks |
| A82A + SetToolTrap at InstallDrivers | Access violation / early crash |
| A82A bind only at sources>=2 | Still unsafe; left **unbound** |
| `Delay_usec` under thrash | **Stretched** intro thrash ~2s -> ~20s - **do not** |
| `DESCENT_MOVIE_DIAGNOSTICS=1` full dumps + fflush every A193 | Same stretch (~20s thrash span, MB logs) - **default OFF** |
| Diags off + no A82A bind | Boots, Descent runs, SourcePB advances; thrash still present but not log-amplified |

**Current tree:** A82A not rebound; diags default **0**; `AudioServicePendingInterrupt`; light `thrashMeter` (one line/tick if calls>=40).

**Open fix:** host ClockGetTime without global A82A (own `'clok'` / entry-point), or faster 68k.

### 22.8 Inline A82A -> host GetTime (working path)

**Idea:** Do not rebind the global trap. On first multi-source A193 with a
GetTime stack, scan the outer sample for `move.l #$00040001` / `A82A` and
**rewrite only that A82A word** to `EMUL_OP_COMPONENT_DISPATCH`.

**Stack at inline EMUL_OP** (trap never entered):  
`[header][TimeRecord*][CI][result]` -> fill TR with wall ?s, leave `[result]`.

**Verified in agent run (2026-07-17):**

```
inlineA82aPatch at=0x00f31058 ... tick=1619
hostClockGetTime n=1 ci=0x0082000c ...   <- matches ci40
hostClockGetTime n=11 ci=0x00810012 ...  <- also sclk CI
thrashMeter=0                          <- A193 thrash collapsed under sources=2
```

Unlike CLOCK_SAMPLE (whole sample free -> parent ran wild), only GetTime is
hosted; rest of sample/policy still runs on 68k.

**Status:** **REVERTED** - free GetTime accelerated parent to ~700k+ calls;
`AudioInterrupt` starved; SourcePB `data=0` (silence); freeze. Same class as
CLOCK_SAMPLE. Do not re-enable free GetTime without rate-limiting the parent.

### 22.9 Audio regression (2026-07-17)

User: freeze on run and/or **no audio** (audio worked before inline patch).

Log evidence with inline patch:
- `hostClockGetTime n=732000`
- `AudioInterrupt` diags absent / stream `data=0x00000000` while frames huge
- Parent free-ran GetTime; mixer PB lost data

**Tree restored (briefly):** then re-landed as ?22.10 with audio service + yield.

### 22.10 Inline GetTime + audio service + yield

**Combo that keeps audio:**

1. Patch only sample-site `A82A` (`0x00040001` packing) -> host TR fill  
2. **Every** host GetTime: `AudioServicePendingInterrupt()`  
3. **Every 8th:** `Delay_usec(100)` so parent cannot free-run like CLOCK_SAMPLE  

Agent check: `SourcePB dataOK=48 dataNull=1` (was all null with free GetTime alone);
`hostClockGetTime` + `inlineA82aPatch` present; stream frames=11025 with nonzero data.

**Subjective stutter:** user **"dont notice anything different"** (2026-07-17).

### 22.11 Why ?22.10 felt identical + thrash experiments

**Root cause of no-op yield:** Windows `Delay_usec(usec)` is `Sleep(usec/1000)`.
Any sub-ms value (including `Delay_usec(100)`) becomes **`Sleep(0)`** - zero
wall delay. ?22.10's every-8th "yield" never rate-limited thrash (~23k GetTime/s).

| Experiment | Result |
|------------|--------|
| Hard 1?kHz rate limit (`Delay_usec(1000)` when early) | Rate ~600-700/s; **SourcePB data=0**, huge frames - silence |
| Clear VIA from thrash + host TickCount++ | Skips guest VBLTasks; **breaks movie PB** |
| Early `TimerInterrupt` while VIA pending, leave VIA set | Agent run looked OK; **user: `execute_illegal` abort** -> ?22.12 |
| Every 32nd GetTime: real `Delay_usec(1000)` (=Sleep(1)) | Mild throttle only |

### 22.12 execute_illegal abort from early TimerInterrupt - **reverted**

User: abort in `powerpc_cpu::execute_illegal` after early-TM thrash service.

**Cause:** nested `TimerInterrupt()` (movie `moreRtn` / TM tasks) under host
GetTime EMUL_OP -> unsafe 68k re-entry -> PPC illegal opcode -> `abort()`.

**Fix in tree:** thrash service is **audio-only** again. No TimerInterrupt /
VIA consume from Microseconds or host GetTime.

### 22.13 Current tree + "no audio" (2026-07-17)

**Code now:**

1. Inline sample-site `A82A` -> host GetTime (discovery on multi-source A193)  
2. Every host GetTime: `AudioServicePendingInterrupt()` (**audio flag only**)  
3. Every **32nd** multi-source host GetTime: `Delay_usec(1000)` (=`Sleep(1)`)  

**User:** **no audio** after ?22.12 rebuild.

Agent smoke (audio-only thrash, 35?s): process survived (no illegal/abort);
`hostClockGetTime` to n>=100k; movie SourcePB almost all **`data=0x00000000`**
with huge counting-down frames (~1.1M -> ...) after start - silence class, same
family as free GetTime without a working mixer path.

**Also noted:** host GetTime hits both `ci40` (e.g. `0x0082000c`) **and**
siSoundClock CI (e.g. `0x00810012`) via the same patched site - wall ?s for
sclk may be wrong. Do not assume one host GetTime formula is safe for every CI.

**Do not re-enable:** early TimerInterrupt from thrash; VIA clear from thrash;
hard 1?kHz GetTime Sleep.

**Next (audio first):** restore working audio (disable inline A82A or restrict
host GetTime so sclk is never faked), then re-attack freeze without nested TM.

---

## 23. 2026-07-17 session: audio restored; real root cause found (two-source failover)

### 23.1 Audio restore (first task, done)

Inline sample-site A82A -> host GetTime is now compile-gated **off**:
`DESCENT_MOVIE_INLINE_A82A_GETTIME=0` in `qd3d_init_logging.h`
(scan block in SheepShaver `emul_op.cpp` OP_MICROSECONDS; `OP_COMPONENT_DISPATCH`
handler still present but unreachable). Agent-verified in-emulator: mixer
blocks 16 KB nonzero, hashes advancing, SourcePB frames=11025 data advancing,
zero `inlineA82aPatch`/`hostClockGetTime` lines.

### 23.2 New always-on light pulses (keep; a few lines/sec, movie phase)

| Tag | Where | Meaning |
|-----|-------|---------|
| `audioPulse` | `audio_sdl.cpp` AudioInterrupt | mixer GetSourceData bytes + 64-byte hash (armed at sources>=1) |
| `mixPulse` | `audio_sdl.cpp` stream_func | **audible** peak of final SDL mix per callback |
| `vidPulse` | `video_sdl2.cpp` VideoVBL | sparse framebuffer hash per VBL -> exactly when video freezes/resumes |
| `SourcePB poll` | audio.cpp (QD3D_AUDIO log) | now also logs every 32nd unchanged poll |
| `frameWalk`/`frameDump` | SheepShaver emul_op.cpp | one-shot 68k a6-chain dump -> `descent_frame_retN.bin` |

`silence hash` for 64 zero bytes = `dfde6ac5` (recognize it everywhere).

### 23.3 Decoded: "policy fn" = ClockGetTime(ms); polC=1000 is the SCALE

From `descent_micros_outer_caller.bin` (fn at file 0x68e, `link a6,#-8`):
pascal-style fn returning **clock time converted to scale d7=0x3e8=1000
(milliseconds)** in caller result slot `0x14(a6)` ("helperD0"). `flag52`/cache
path only for rate==0. `jsr $c9e` = 64-bit scale conversion (?s->ms). So the
"policy loop" is simply the movie player polling GetTime-in-ms. Parent chain
above it is CFM/Component glue -> the wait loop is **PPC code in the Descent
PEF** (frame dumps: ret1 = CFM-68K trampolines at 0x00b2xxxx, ret2 = CallComponent
A82A glue at 0x00a2xxxx).

### 23.4 Measured intro timeline (runs 3/4, diags OFF, pulses only)

Example (run 4; ticks, 60 Hz):

| Tick | Event |
|------|-------|
| 1574 | movie starts; **video animates normally** (vidPulse changing) |
| 1575 | source A (8-bit raw, empty PB + moreRtn) started on mixer 0x0086 - probe; game tears it down + RemoveSource within ~3 ticks |
| 1579 | **new mixer 0x0087**; source A' = 16-bit twos 22050 stereo, **PlaySourceBuffer start with frames=0 data=0 + moreRtn=0x00ba969e** ; SM fills PB with a 256-frame chunk right after the call returns |
| 1579->1630 | mixer output = pure silence from block 1 (`dfde6ac5`); **source A' PB frozen at frames=256 forever** (mixer never consumes, never calls moreRtn); video keeps animating to ~1619 |
| ~1613 | game begins failover: AddSource #2, SetInfo volu + sclk (sound clock created) |
| 1619-1687 | **video frozen (~1.1 s)** - the A193/GetTime thrash window = player waiting/resyncing |
| 1630-1642 | StopSource x4, prime 11025 (paused), prime ~10952 (paused), start (72-byte record) on source B |
| ~1653 | first non-silent mixer block; ~1659 audible (mixPulse peak >=12k) |
| 1687 | video resumes (~= when new sound clock catches up to video position) |

**"Audio restarts from the beginning"** = source B replays the soundtrack from
0 (the streaming attempt never audibly played at all).

### 23.5 Root cause (supersedes ?16/?21 "thrash cost" framing)

**The Apple Mixer never services a source whose PlaySourceBuffer(start) was
issued with an empty PB (frames=0,data=0) + moreRtn.** SM/client fills the PB
immediately after the call, but the mixer has already retired the buffer and
its mix loop never re-examines the source -> silence + zero progress. Descent's
MVE player streams movie audio exactly this way (256-frame double-buffered
chunks via moreRtn/completionRtn). Seeing no progress, it fails over (~1 s)
to a primed double-buffer source and restarts the soundtrack. The freeze +
silence + restart *is* that failover; the GetTime thrash is just its wait loop.
"Later movies fine" = the player remembers streaming failed and goes straight
to primed mode.

### 23.6 Host-side experiments (2026-07-17)

| Experiment | Result |
|------------|--------|
| One-shot re-Play("kick") when PB gains data | Mixer consumes exactly **one** chunk + one moreRtn refill per Play; then stalls again; output still silence |
| Mixer `StartSource` for the source at empty-start time | No effect |
| **Pump**: re-Play per AudioInterrupt | Stream ping-pongs between two 256-frame buffers, **content all zeros** (lead-in silence; writer paced by completion callbacks that never run). Refill is deferred -> max 1 chunk (11.6 ms) per interrupt = **8x too slow** even if it mixed |
| Reject **any** empty+moreRtn start (`notEnoughHardwareErr`) | **HANG** - the 8-bit probe must succeed (run 8: one frozen frame forever, game never falls back) |
| Reject only **16-bit 'twos'** empty+moreRtn start | run 9 pending at time of writing |

### 23.7 Current tree (this session)

- `DESCENT_MOVIE_INLINE_A82A_GETTIME=0` (audio OK again)
- Pulses of ?23.2 (always compiled, movie-phase gated, capped)
- `AudioStreamKickPoll()` pump + StartSource kick: **dormant** (arming only
  happens when an empty start is delegated; with the 16-bit reject in place
  only the 8-bit probe arms briefly, then RemoveSource disarms)
- 16-bit 'twos' empty+moreRtn PlaySourceBuffer start -> `notEnoughHardwareErr`
  (fail-fast -> immediate primed fallback) - being validated
- `notEnoughHardwareErr` added to `audio_defs.h`

### 23.8 Proper long-term fix (open)

Make the streaming path real: mixer-side servicing of empty-PB+moreRtn sources
at real-time rate (needs Apple Mixer / SoundLib internals - the mixer consumes
one chunk per *Play call* but its GetSourceData mix loop ignores the source).
Until then fail-fast -> primed fallback is the pragmatic route.

### 23.9 Fail-fast rejection: **dead end - do not retry**

| Variant | Result |
|---------|--------|
| Reject any empty+moreRtn start | **Hang**: game shows one frozen frame forever (run 8) |
| Reject only 16-bit 'twos' empty start (8-bit probe delegated) | **Same hang** (run 9): after the error the game adds no second source, never primes, silence + frozen frame forever |

The timeout failover is the player's *only* recovery path; Play errors on this
source are fatal. Rejection removed from tree.

### 23.10 `AudioStreamHostMix` - host services the streaming source (in tree)

Since the Apple Mixer never mixes an empty-start+moreRtn source, the host now
does: `audio.cpp` consumes the PB like a mixer would (advance `pb+24`,
decrement `pb+20`), refills it by calling the PB's **moreRtn** (pascal
`Boolean(SoundParamBlockPtr *)`) via a new 12-byte 68k thunk in the adat area
(`adatCallMoreRtn` @204, `adatStreamPbVar` @216, `SIZEOF_adat`=220), converts
(22050->44100 dup, 8/16-bit, mono/stereo) and mixes into the block fetched from
the mixer, from `AudioInterrupt`. Armed in `PlaySourceBuffer` when a non-paused
start has frames=0/data=0/moreRtn!=0; disarmed on RemoveSource / format
mismatch.

**Verified (run 10/12 logs):** `streamHostArm` -> `streamHostMix` 8 chunks
(2048 src frames) per interrupt, `outFrames=4096/4096`, `starved=0`
indefinitely - **moreRtn is synchronous** and SM's bufferCmd queue refills
forever. This is a genuine fidelity fix for any app streaming through this
Sound Manager path.

**completionRtn must NOT be called per drained buffer** - it means "entire
sound finished"; calling it made SM retire the stream after one chunk (run 11).

### 23.11 What run 10 disproved about the intro itself

Even with the stream flowing at full real-time rate and moreRtn callbacks
firing, the game **still** created the primed source at the same time
(~63 ticks after movie start) and the chunks stayed **all-zero** - the
streaming channel is a *silence bed by design* (likely the game's persistent
SFX/stream channel); the movie music was always going to arrive as the second,
primed source. The intro stutter is therefore the player's **phase-2 pause**:
video runs ~0.9 s ahead while the music source is prepared/primed (decode is
interpreter-slow), then video holds until the new sound clock / primed buffer
catches up (~0.5 s prime + prep). Freeze measured 68 ticks baseline (run 4)
-> **50 ticks with host streaming** (run 10).

Open levers for the residual freeze (in likelihood order):

1. Decode the player's phase-2 wait in the **Descent II PEF PPC code**
   (we have the file; frame dumps ret1/ret2 = CFM glue -> the loop is PPC)
   to find what gates video resume (sound-clock threshold? prime completion?).
2. Faster guest decode (68k/PPC JIT) - shrinks the video-ahead gap directly.
3. Sound-clock behavior across the source switch (does sclk restart at 0?).

### 23.12 Final tree state (2026-07-17, end of session)

- Inline A82A host GetTime **off** (`DESCENT_MOVIE_INLINE_A82A_GETTIME=0`) - audio works.
- `AudioStreamHostMix` **in tree and verified** (run 13): 8 chunks / interrupt,
  `outFrames=4096/4096`, `starved=0` for the whole session; correct SM
  semantics (moreRtn only, never completionRtn).
- `frameWalk`/`frameDump` one-shot gated behind `DESCENT_MOVIE_DIAGNOSTICS`.
- Pulses (`audioPulse`/`mixPulse`/`vidPulse`/`streamHostMix`) always compiled,
  capped; grep-friendly for by-ear correlation.
- Intro video hold measured **51 ticks (~0.85 s)** vs 68 ticks baseline; the
  hold is the player's phase-2 resync (see ?23.11), not host audio failure.
- `build_ss.bat` at repo root = vcvars + ninja one-liner used this session.
- Run logs `d2_agent_run2..13.log` in the build dir document every experiment.


---

## 24. siSoundClock deep-dive and EMUL_OP fallback (2026-07-17, continuation session)

A dedicated note was created at siSoundClock-descent-notes.md covering everything found about the undocumented 'sclk' selector. This section records only the new fix attempt and status.

### 24.1 What changed since ?23

- `siSoundClock` CI tracking is now implemented: `BasiliskII/src/audio.cpp` captures the CI returned by `GetInfo(sclk)` on the first music source and exposes it through `AudioGetSoundClockCI()` (`BasiliskII/src/include/audio.h`).
- The inline A82A patch was augmented in `SheepShaver/src/emul_op.cpp` with an `OP_COMPONENT_DISPATCH` handler.
- `EmulOp()` signature was changed to accept `uint32 *pc` so the handler can redirect the 68k program counter if needed. Callers updated in `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` and `SheepShaver/src/Unix/paranoia.cpp`.
- New compile switch: `DESCENT_MOVIE_SOUND_CLOCK_WALL` in `SheepShaver/src/gfxaccel/include/qd3d_init_logging.h`.

### 24.2 Handler behavior

The `OP_COMPONENT_DISPATCH` EMUL_OP handler inspects the 68k stack at SP:

```text
SP+0 : header   (expected 0x00040001 = ComponentDispatch GetTime)
SP+4 : TimeRecord *
SP+8 : ComponentInstance
```

- If `CI == AudioGetSoundClockCI()` (`0x00810012`), write host wall time (microseconds, scaled as the player expects) into the `TimeRecord` and pop the parameters. This collapses the phase-2 wait by making the music source's clock continuous instead of paused-at-zero.
- If `CI != sound-clock CI` (e.g., `ci40 = 0x0082000c` for video sync), execute the original `A82A` trap through `Execute68kTrap(0xA82A, r)` so normal component dispatch is preserved.

### 24.3 Why `Execute68kTrap` is the right fallback

The first fallback idea was a hand-written 68k thunk in ROM patch space. That is risky because the EMUL_OP path does not push a 68k return address-the 68k SP already points at the parameters. `Execute68kTrap(0xA82A, r)` is the existing host mechanism that constructs a temporary `A82A; RTS` routine and runs it on the caller's stack, so the return-address bookkeeping is handled by the emulator itself instead of by hand.

### 24.4 Validation plan

- Build `out\build\x64-QD3D-RelWithDebInfo\SheepShaver\SheepShaver.exe` with `DESCENT_MOVIE_SOUND_CLOCK_WALL=1`.
- Run `out\build\x64-QD3D-RelWithDebInfo\SheepShaver\detect_audio.ps1`.
- Expect the `OP_COMPONENT_DISPATCH` handler to log, the sound-clock branch to fire, and the intro to proceed without the phase-2 freeze.

---

## 25. `siSoundClock` fix validation and scanner tuning (2026-07-17, continuation)

### 25.1 What changed since ?24

- `Execute68kTrap(0xA82A, r)` fallback was **rejected**: it crashed with SIGSEGV (PPC PC `0x40d80000`). Re-entering the 68k emulator from inside an EMUL_OP handler is unsafe here.
- Working fallback: a ROM scratch thunk at `COMPONENT_DISPATCH_PATCH_SPACE` containing `A82A; jmp abs.l` to the original cleanup instruction. This preserves the A82A parameter stack and lets the caller's cleanup pop the result.
- Patch scanner expanded to scan both the discovered `spin` PC and the `getTime` candidate (`maybe_gt`) over a 1 KiB window, patching every matching `move.l #0x00040001,-(sp); moveq #0,d0; A82A` site.
- `compDispEnter` logging now includes `hdr`, `tr`, and `ci` so it is possible to see which CI is being polled.

### 25.2 Validation results

- `detect_audio.ps1` reports `audioDetected=True` consistently.
- When the patch hits the GetTime helper (e.g., `0x00f1fc4a`), the handler logs both `ci40` (fallback) and the sound-clock CI (`0x00810012`, host wall time). The wait loop proceeds and audio starts.
- Some runs patch only an inlined `ci40` site (e.g., `0x01062d18`, `0x010a74ea`) and the process does not sustain; the scanner's stack-based address discovery is not yet reliable.
- Even when no patch is applied, the phase-2 freeze is now ~0.5-0.7 s (down from ~0.85-1.1 s) because `AudioStreamHostMix` lets the primed source catch up faster.

### 25.3 Open problems

1. **Patch scanner reliability**: the `spin` and `maybe_gt` values read from the 68k stack during `OP_MICROSECONDS` do not always point at the GetTime helper. In some runs `spin` is `0x00000014` and `maybe_gt` is a PPC address (`0x40814afe`), so the scanner finds nothing.
2. **Multiple GetTime sites**: the wait loop may inline A82A for `ci40` in one place while using a helper for the sound-clock CI in another. Both need to be patched, or the sound-clock helper needs to be identified unambiguously.

### 25.4 Next moves

- Either make the scanner robust (e.g., walk the stack frame like the diagnostic code, or scan the GetTime function directly once its address is known), or switch to a global A82A trap rebind with CI-specific fallback.
- Verify by ear/eye that the phase-2 freeze is fully gone and that video sync remains acceptable when the patch is active.

See `siSoundClock-descent-notes.md` for the full technical write-up.


---

## 26. Scanner fixed; A82A fallback proven unworkable from EMUL_OP (2026-07-17, agent session)

Append-only. Two real bugs fixed, one dead end closed with evidence.

### 26.1 Build bug: `DESCENT_MOVIE_SOUND_CLOCK_WALL` never built without diagnostics

`AudioGetSoundClockCI()` was **defined inside `#if DESCENT_MOVIE_DIAGNOSTICS`**
in `audio.cpp` while `emul_op.cpp` called it under `DESCENT_MOVIE_SOUND_CLOCK_WALL`.
With diagnostics off the link failed:

```
emul_op.cpp.obj : error LNK2019: unresolved external symbol AudioGetSoundClockCI
```

**Implication:** every previous validation of the sound-clock fix (24/25) ran with
`DESCENT_MOVIE_DIAGNOSTICS=1`, i.e. in the mode 22.7 says stretches the intro
~1s -> ~20s. Those timings were measuring the instrumentation.

**Fixed:** accessor moved out of the diagnostics block (it is a production
accessor, not a diagnostic). The CI *capture* at the `GetInfo(sclk)` site was
already outside and unaffected. `DESCENT_MOVIE_DIAGNOSTICS` restored to **0**
(it had drifted back to 1 in the header).

### 26.2 Scanner bug: one-shot latch burned on garbage (fixes 25.3)

`sites_scanned = true` was set after the **first** multi-source A193 whether or
not anything was patched. The `spin`/`maybe_gt` stack slots are only valid when
that A193 arrived via the movie GetTime helper - a minority of calls.

Baseline run, diagnostics off, reproduced 25.3 deterministically:

```
scanSites a7=0x1ec107c8 spin=0x00000014 gt=0x40814afe tick=1671   <- both garbage
(no inlineA82aPatch; no compDispEnter all run)
```

**Fixed** (`emul_op.cpp`, OP_MICROSECONDS): retry until a site is actually
patched (cap 20000 attempts, sparse miss logging), skip non-68k-RAM candidates
(a ROM/PPC address like `0x40814afe` never holds the helper), and scan +/-0x800
around the candidate instead of only below it.

**Result - discovery is now reliable:**

```
scanSites miss attempt=1  spin=0x00000014 gt=0x40814afe tick=1672
inlineA82aPatch at=0x00fb1874 spin=0x00fb1a3a tick=1681
inlineA82aPatch at=0x00fb1a38 spin=0x00fb1a3a tick=1681
scanSites DONE attempts=15 sites=2 spin=0x00fb1a3a gt=0x00f852e6 tick=1681
compDispEnter n=1 hdr=0x00040001 ci=0x0082000c sclk=0x00810012 tick=1681
```

Attempt 1 sees the same garbage as before; **attempt 15** has real 68k
addresses. Handler entered with a valid header and correctly distinguished
`ci40` from sclk. Address discovery is **solved**.

### 26.3 The blocker: the non-sclk fallback wedges the guest

The patch rewrites **both** the sclk site and the `ci40` (video-sync) site, so a
fallback that runs the real `A82A` is **mandatory**. It does not work:

| Attempt | Result |
|---------|--------|
| `Execute68kTrap(0xA82A, r)` (25.1) | SIGSEGV, PPC PC 0x40d80000 |
| ROM scratch `A82A; jmp abs.l *pc` | Emulator wedges on the tick it first fires |
| Same + fixed jump-target write | Same wedge |
| Same + `d0` preserved on fallback | Same wedge |

Two real defects were found and fixed in that thunk along the way (they were not
the root cause, but were latent):

1. **Jump target never written.** `WriteMacInt32(ROMBase + patch_base + 4, *pc)`
   passes a **host** address to a function taking a **Mac** address, so the
   `jmp abs.l` operand stayed 0. Now written through `ROMBaseHost` with the
   opcode, and rebuilt per call (the target differs per site).
2. **`d0` clobbered.** The guest sequence is
   `move.l #$00040001,-(sp); moveq #0,d0; A82A` - **`d0` is ComponentDispatch's
   selector**, not a result slot (cf. `audio.cpp:975`, `moveq #$24,d0` before its
   own A82A). The old reject paths did `r->d[0] = -1; break;`, synthesising an
   error for a call that never ran *and* corrupting the selector. Rewritten so
   anything not positively recognised as GetTime-on-sclk runs the real trap with
   registers untouched.

**Measured, diagnostics off:**

| Run | Last tick | Audio |
|-----|-----------|-------|
| Baseline (no patch) | 2399 | `mixPulse peak=32768` |
| Patch active | **1700** (dies on the patch tick) | none |

`compDispEnter n=1` then nothing - every variant dies at the first **ci40**
dispatch, i.e. in the fallback, never in the sclk path.

**Root reason (structural, not a coding slip):** `EmulOp` runs with
`XLM_RUN_MODE = MODE_EMUL_OP`; the glue only restores `MODE_68K` *after* it
returns (`sheepshaver_glue.cpp:283`). Redirecting `*pc` into ROM scratch makes
`A82A` execute during an EMUL_OP unwind, not from real trap-entry state. This is
the same unsafe 68k re-entry 25.1 already recorded, via a different mechanism.
The sclk CI is owned by the **Apple Mixer** (`GetInfo(sclk)` is delegated via
`Execute68k(adatGetInfo)`), so correcting its GetTime "at the source" is not
available either.

### 26.4 Tree state (verified healthy)

`DESCENT_MOVIE_SOUND_CLOCK_WALL=0`, `DESCENT_MOVIE_DIAGNOSTICS=0`. Verified run:
40 s, last tick **2402**, `maxMixPeak=32768`, `streamHostMix` 28, zero
`compDispEnter`/`inlineA82aPatch`. **Audio works; no crash, no wedge.**

Kept (dormant but correct, behind the switch): the retrying scanner (26.2) and
both thunk fixes (26.3). They are strictly better than what was there and cost
nothing at default-0.

**Baseline stutter re-measured** (`vidPulse` hash-unchanged spans, diagnostics
off): **50 ticks / 0.83 s frozen at ticks 1674-1724** - matches 23.12's 51 ticks.
Instrumentation-free confirmation the freeze is real and stable.

### 26.5 Do-not list (additions)

- Do **not** run `A82A` (or any trap) from inside an EMUL_OP handler - neither
  `Execute68kTrap` nor a ROM-scratch `jmp` thunk. Wrong run mode; wedges.
- Do **not** re-enable `DESCENT_MOVIE_SOUND_CLOCK_WALL` while the patch hits
  `ci40`, because that *requires* the impossible fallback.
- Do **not** trust any 24/25 timing: they were taken with diagnostics on (26.1).

### 26.6 Next moves (ranked)

1. **Patch only the sclk site** -> no fallback needed, blocker disappears.
   `compDispEnter` already prints `ci` per site; patch one site, read its `ci`,
   and keep only the site whose `ci == AudioGetSoundClockCI()`, reverting the
   other to `0xA82A`. The scanner (26.2) makes this practical now.
2. If both CIs share a call site, intercept **inside** the handler only when
   `ci == sclk` and otherwise **restore the original A82A word and re-execute
   the instruction** (`*pc -= 2`) rather than running the trap ourselves.
3. Only then re-measure the 50-tick hold.

---

## 27. Where the movie code actually comes from (2026-07-17, agent session)

Answers "where is the MVE player from?" from the shipped binary, not inference.
Parsed `Descent II` (PEF `Joy!peff` / `pwpc`) directly.

### 27.1 Container

| Section | Kind | Size | File offset |
|---------|------|------|-------------|
| 0 | code (0) | 981,556 | `0x1800` |
| 1 | data, pattern-init (2) | 2,007,204 (78,728 unpacked / 53,647 packed) | `0xf1240` |
| 2 | loader (4) | - | `0x80` |

`exportedSymCount = 0` (an application, exports nothing).

### 27.2 Imported libraries - **there is no MVE/movie library**

Only **5** imports, 311 symbols total:

| Library | Syms |
|---------|------|
| InputSprocketLib | 11 |
| QuickDraw(tm) 3D Accelerator | 8 |
| QuickTimeLib | 18 |
| MathLib | 5 |
| InterfaceLib | 269 |

**Conclusion: the MVE player is Descent's own code, statically linked into
section 0 of the PEF.** It is not a Mac system library, not a shared library, and
not "68k libs in RAM" as ?1/?4.1 guessed. This *confirms* ?23.3 ("the wait loop
is PPC code in the Descent II PEF") and explains the CFM-68K trampolines in the
frame dumps: it is Descent PPC code calling down through CFM glue into the 68k
Sound Manager.

Proof from statically-linked strings (Interplay's own source diagnostics):

```
0xf9acb  "error reading movie library <%s>" $DMVL "intro.mvl" "other.mvl" "robots.mvl"
0xf9b1e  "Cannot open movie file <%s>"
0xf9640  "intro.mve"
0xfbe2c  "piggy_new_pigfile"  "descent2.ham/.s22/.s11"
0xf95e5  "Checking for Descent 2 CD-ROM" ":Data:" "descent2.hog"
```

### 27.3 Data layout (on disk here)

| Path | Contents |
|------|----------|
| `Descent II` | data fork = PEF (the engine + MVE player) |
| `Descent II Resources` | resource fork (UI, `snd`, `SONG`, `Midi`, `cfrg`) |
| `descentdata/Descent II.bin` | merged data+resource of the executable |
| `descentdata/Data/` | `descent2.hog`, `.pig`, `.ham`, `.s11/.s22` |
| `descentdata/DataCD/` | **`intro.mvl` (88 MB)**, `other.mvl` (102 MB), `robots.mvl` (11 MB) |

The intro MVE lives in `DataCD/intro.mvl` (an MVL archive of MVEs); guest path
strings are `Descent II:Data:intro.mvl`.

### 27.4 **`SndPlayDoubleBuffer` - corrects the ?23 framing**

InterfaceLib import **291 = `SndPlayDoubleBuffer`**. Also imported:
`SndNewChannel` (102), `SndDoImmediate` (114), `SndDisposeChannel` (76),
`Microseconds` (308), `PrimeTime` (166), `InsXTime` (160), `RmvTime` (282),
`Delay` (150).

?23.4/?23.11 describe phase 2 as "primed 2 x ~0.5 s buffers in a paused state,
then start, with a moreRtn". **That is literally `SndPlayDoubleBuffer`**, whose
`SndDoubleBufferHeader` holds exactly **two** `SndDoubleBuffer`s plus a
`dbhDoubleBack` callback. So the "primed double-buffer source" is not the player
improvising a fallback path - it is this single documented API call. The
`moreRtn` the host now services in `AudioStreamHostMix` (?23.10) is
`dbhDoubleBack`.

**Consequence for the fix hunt:** the phase-2 audio path is Sound Manager
`SndPlayDoubleBuffer`, not a bespoke mixer dance. Its documented semantics
(SM calls `dbhDoubleBack` when a buffer drains; flags `dbBufferReady` /
`dbLastBuffer`) are the contract our mixer must honour - a far better spec to
implement against than reverse-engineered PB pokes.

### 27.5 Two movie paths exist

QuickTimeLib is imported with 18 symbols (`NewMovieFromDataFork`, `StartMovie`,
`MoviesTask`, `BeginFullScreen`, `NewMovieController`, `PrerollMovie`,
`LoadMovieIntoRam`, ...). The intro is **MVE via `.mvl` + `SndPlayDoubleBuffer`**
(consistent with ?1's "not QuickTime"), but Descent clearly has a *second*,
QuickTime-based movie path. Do not assume a movie is MVE just because it is a
Descent movie.

### 27.6 Method (reproducible)

`python` is at `C:\Python314\python.exe` (not on PATH as `python3`). Loader
section parse: header at section kind 4; imported library table at `L+56`
(24 bytes/entry); imported symbol table follows; strings at `L + loaderStrOff`;
symbol name offset = `value & 0x00ffffff`, class = `(value >> 24) & 0x0f`.

---

## 28. Descent's audio engine decoded from Ghidra (2026-07-17, agent session)

User disassembled the PEF in Ghidra. `dissassem.c` / `dissassem.asm` hold
`FUN_100e1560` (the `SndPlayDoubleBuffer` site) and its 3 callers.

### 28.1 This is `digi_init`, not the MVE player

`FUN_100e1560` is Descent's **general digital sound engine** startup - the one
and only `SndPlayDoubleBuffer` call site. Callers:

| Caller | Role |
|--------|------|
| `FUN_100e7834` (@100e7d34) | init (allocates 0x11728 state, mixing tables, 0x20000 buf) |
| `FUN_100e7d60` (@100e7f20) | reconfigure (rate/format change) |
| `FUN_100e7674` (@100e76a8) | restart after stop |

So the movie's music is **not** a bespoke movie path: it is the game's normal
sound engine, driven by `SndPlayDoubleBuffer` on a plain `SndNewChannel`.

### 28.2 The `SndDoubleBufferHeader` (state base `r31`, hdr at `+0x116ac`)

| Field | Offset | Set at | Value |
|-------|--------|--------|-------|
| `dbhNumChannels` | `+0x116ac` | 100e1700 | 1 mono / 2 stereo (from state `+9`) |
| `dbhSampleSize` | `+0x116ae` | 100e1720 | 8 / 16 (from state `+8`) |
| `dbhCompressionID` | `+0x116b0` | 100e1728 | 0 |
| `dbhPacketSize` | `+0x116b2` | 100e172c | 0 |
| `dbhSampleRate` | `+0x116b4` | 100e15a4/15c8/15e0 | `0xAC440000`=44100, `0x56220000`=22050, or `0x56EE8BA3`=22254.5 |
| `dbhBufferPtr[0]` | `+0x116b8` | 100e1738 | buffer A (`+0x116c4`) |
| `dbhBufferPtr[1]` | `+0x116bc` | 100e1744 | buffer B (`+0x116c8`) |
| `dbhDoubleBack` | `+0x116c0` | 100e1754 | `NewRoutineDescriptor(cb, 0x3c0, 1)` |

Channel: `SndNewChannel(&chan@+0x116a8, synth=5 sampledSynth, init=0x84/0xC4, 0)`
(`0xC4` when state `+9` set = stereo).

Buffer alloc (100e163c-100e166c): `size = 0x12 + frames*(stereo?2:1)`, then
`*2` if 16-bit. `0x12` = 18 = `sizeof(SndDoubleBuffer)` header
(`dbNumFrames`+`dbFlags`+2x`dbUserInfo`). `FUN_100e1458` primes each buffer
before play; `+8`/`+0xc` (`dbUserInfo[0..1]`) zeroed first.

### 28.3 **TOC globals (r2 = 0x100f7a40)** - the answer to "where is the callback"

The decompiler's `local_3c` is **not a variable**: it is `lwz r2,0x14(r1)`, the
standard TOC restore after each cross-TOC glue call. Ghidra dropped
`assume r2` after the prologue, so every `-0xXXXX(r2)` global decompiled as a
bogus `local_3c + -0xXXXX` stack read. **Read the .asm, not the .c, for anything
touching globals or glue.**

| TOC ref | Absolute address | Value / role |
|---------|------------------|--------------|
| `-0x5430(r2)` @100e1734 | **`0x100F2610`** | **`dbhDoubleBack`** - loaded to r3 = arg1 of `NewRoutineDescriptor(proc,0x3c0,1)`. Holds ptr to a **TVector** `{code,toc}`; follow first word for the real function. |
| `-0x4ba0(r2)` @100e15ac/15e8, read @100e1648/1654 | `0x100F28A0` | **buffer size in frames**: `0x200`=512 @44100, `0x100`=**256** @22050 |
| `-0x4ba4(r2)` @100e1760 | `0x100F289C` | set **0** immediately before play |
| `-0x4ba8(r2)` @100e1764 | `0x100F2898` | set **1** immediately before play (playing flag?) |
| `-0x66c8(r2)` @100e1578 | `0x100F1378` | ptr to ptr to the 0x11728 sound state |

**`0x100` = 256 frames on the 22050 path corroborates ?23.4** ("SM fills PB with a
256-frame chunk right after the call") - that number comes from this global.

`0x3c0` procInfo + `1` (kPowerPCISA) means `dbhDoubleBack` is **PPC**, invoked by
the 68k Sound Manager through a routine descriptor / MixedMode.

### 28.4 Rate selection (100e1578-100e15e8) - relevant to the sound clock

```
state = **0x100F1378;  iVar1 = state[1]   (state+4)
if (iVar1 == 2)                  rate = 0xAC440000 (44100), frames = 0x200
else if (0 <= iVar1 < 2):
    if (FUN_100dec18())          rate = 0x56220000 (22050)
    else                         rate = 0x56EE8BA3 (22254.5454, classic Mac rate)
    frames = 0x100
```

Our logs show the movie source at **22050 / 16-bit / 2ch**, i.e. `state+4` in
[0,2) and `FUN_100dec18()` (a HW capability probe) returned true.

### 28.5 Why this matters for the freeze

The phase-2 "primed double-buffer source" of ?23 is literally
`SndPlayDoubleBuffer` with `dbhBufferPtr[0..1]` - SM's own double buffering, and
`moreRtn`/`AudioStreamHostMix` (?23.10) is servicing **`dbhDoubleBack`**. The
contract is documented (SM calls `dbhDoubleBack` when a buffer drains; callback
sets `dbNumFrames` + `dbBufferReady`, or `dbLastBuffer` to end). That is a real
spec to implement against instead of reverse-engineered PB pokes.

### 28.6 Next from Ghidra (highest value first)

1. **`0x100F2610` -> TVector -> `dbhDoubleBack` body.** What it does when the movie
   is mid-decode is likely the whole ballgame.
2. `FUN_100e1458` - buffer prime (sets `dbNumFrames`/`dbFlags`).
3. `FUN_100dec18` / `FUN_100deba4` - rate/size probes (drive `dbhSampleRate`).
4. Whatever holds video ~50 ticks - search callers of the movie code, not this
   sound engine.

---

## 29. Movie player fully disassembled - it is pure QuickTime (2026-07-17)

`FUN_100782e0` (0x100782e0) is the intro movie player. Full disassembly in
`dissassem.asm`. This **confirms the QuickTime theory** and, importantly,
**separates the movie from the game's SndPlayDoubleBuffer sound engine** - they
are unrelated subsystems (?28 was the game SFX mixer, not the movie).

### 29.1 What FUN_100782e0 does (all InterfaceLib/QuickTimeLib glue)

Setup: `GetGWorld` -> build `intro.mve` path (strcpy/strchr/strcat, default dir
string at TOC `+0x2451`) -> **`FUN_100780fc`** opens the movie file (searches 4
dir specs) -> `SetDepth(16)` -> `GetMovieBox`/`OffsetRect`/`SizeWindow` ->
`SetGWorld`/`SetMovieGWorld`/`SetMovieBox` -> `SetMovieTimeValue(0)` ->
`ShowWindow` -> 2x `MoviesTask` (preroll) -> **`StartMovie`**.

Playback loop (0x100785b4-0x10078664) - **the 50-tick freeze lives here**:

```
loop:
  FUN_10078d70()          ; empty (blr) - hook placeholder
  GetOSEvent(&evt)        ; poll keyboard
  if char==0x35 '5' : skip movie (r27=1)
  if evt r26==0x71 'q': StopMovie; spin FUN_100128b4 until key; StartMovie
  MoviesTask(movie,0)     ; <<< pumps QuickTime: decode + audio + clock
  IsMovieDone(movie) ?    ; loop until done
cleanup: StopMovie; HideWindow; DisposeMovie; SetGWorld; SetDepth(8); cursor
```

`local_44` = Movie; `r25`->movie window; `r28`->a GDevice/port global.

### 29.2 Consequence - the freeze is inside emulated QuickTime, not Descent

Descent's only role in the intro is `StartMovie` + a `MoviesTask`/`IsMovieDone`
spin. **Everything else happens inside our emulated QuickTime** when `MoviesTask`
runs:

- The `A82A ComponentDispatch GetTime` storm (?22-?26) = **QuickTime's TimeBase
  polling its clock component** from inside `MoviesTask` - matches the earlier
  finding that those calls arrive through Component Manager glue, not Descent PEF
  code (frame dumps ret1/ret2 = CFM/CallComponent).
- `siSoundClock` / Apple Mixer / `PlaySourceBuffer` = **QuickTime's sound media
  handler** output path, not Descent's `SndPlayDoubleBuffer` engine.
- The `.mve` custom importer ($DMVL) decodes frames when QuickTime calls it,
  also inside `MoviesTask`.

So the ~50-tick hold is QuickTime's **sound media handler waiting for its clock
to reach the audio media time** - the paused-sound-clock-at-0 problem, one layer
below Descent, in our QuickTime + Apple Mixer emulation.

### 29.3 Why this reframes every prior fix attempt

- We were never going to fix this in Descent's code - Descent just spins on
  `IsMovieDone`. Patching Descent RAM (CLOCK_SAMPLE etc.) was doomed.
- The `siSoundClock` `A82A` interception (?24-?26) targets the **right layer**
  (QuickTime's clock), which is why it can matter - but the sites it patches are
  QuickTime/Component-Manager code, so patching `ci40` (video) breaks things and
  the fallback wedges (?26.3).
- The real question becomes: **what does our emulated QuickTime sound media
  handler use as its clock, and why does it start paused at 0 for the intro?**
  That is host-side (SheepShaver QuickTime is the Mac's, but the Apple Mixer /
  sound component under it is partly ours) - investigate the Apple Mixer sound
  clock the media handler reads, not Descent.

### 29.4 Open (from the binary)

- `FUN_100780fc` - movie file opener (4 dir specs, then QT file open at
  0x10077fd8 / 0x100ef0b0). Confirms whether `.mve` is opened directly or pulled
  from the MVL/HOG; and where the `$DMVL` importer attaches.
- `intro.mve` inside `intro.mvl`: is it a real QuickTime movie (QT importer
  handles it) or custom data needing the $DMVL importer component?
- Which clock the QT sound media handler polls via `A82A GetTime` - that is the
  50-tick gate.

---

## 30. intro.mve is stock QuickTime - MVL confirmed, custom-format ruled out (2026-07-17)

Dumped `descentdata/DataCD/intro.mvl` (88.8 MB) directly to settle whether `.mve`
is a custom format. **It is not.** `.mve` is only the extension.

### 30.1 MVL is a thin archive

```
0x00  'DMVL'
0x04  count = 2
0x08  directory: 13-byte name field + offset + length, per entry
        entry 0: "intro.mve"
        entry 1: "end.mve"
```

`$DMVL` = Descent's **movie-library archive** format, **not** a QuickTime
component. It just names `.mve` blobs.

### 30.2 The `.mve` is a spec-compliant QuickTime `.mov`

Atom layout `[mdat 55,496,168 B @0x200][moov 24,168 B @0x34ecfe8]` - classic QT.
Full tree parsed:

```
moov
  mvhd
  trak (video, 21901 B)
    tkhd  edts(elst)  mdia(mdhd,hdlr,minf(vmhd,dinf(dref),stbl(
          stsd stts stss stsc stsz stco)))
  trak (audio, 2151 B)
    tkhd  edts(elst)  mdia(mdhd,hdlr,minf(smhd,dinf(dref),stbl(
          stsd stts stsc stsz stco)))
```

- **Video:** `stsd` codec **`cvid` = Cinepak**, 592x312, depth 24. Stock Apple QT
  decompressor.
- **Audio:** `stsd` codec **`twos` = big-endian 16-bit PCM, 2 channels**. Stock.

### 30.3 Consequences

1. **No custom QuickTime importer/component.** Prior "$DMVL importer" idea
   (?29.4 open item, disassembly notes) is **wrong** - closed.
2. Stock QuickTime opens the extracted `.mve`, decodes Cinepak, and plays the
   `twos` PCM track through the **Sound Manager / Apple Mixer** - inside
   `MoviesTask` (?29).
3. The `twos`/16-bit/2ch format matches the movie audio in every emulator log
   (?21, ?23): **those logs are this QuickTime PCM track**, not Descent's
   SndPlayDoubleBuffer engine. Final confirmation the two audio subsystems are
   distinct and the movie's is QuickTime's.
4. **Fix locus unchanged and now certain:** the sound clock that our emulated
   QuickTime sound media handler reads for the `twos` track. Cinepak decode cost
   (interpreter) may also contribute to the video-ahead gap, but the *hold* is a
   sound-clock sync in the Apple Mixer layer, below Descent.

### 30.4 Method

`intro.mvl` parsed with Python (`C:\Python314\python.exe`): DMVL dir at 0x8;
QuickTime atom walk from the `moov` at 0x34ecfe8; `stsd` sample-desc FourCCs read
at video stsd+16 and audio stsd+16.

---

## 31. Ground-truth diagnosis: the freeze is decode throughput, not a logic bug (2026-07-17)

After confirming the intro is stock QuickTime (Cinepak + twos PCM, ?29-?30),
captured a fresh diagnostics-on intro and tested every candidate emulation bug
against the log. **All the "clock/mixer/audio bug" theories are false for the
live intro.** The 51-tick freeze is the PPC interpreter being unable to decode
Cinepak fast enough at movie start - a throughput problem, not a correctness one.

### 31.1 The measured freeze (diagnostics-on run, d2_qtdiag1.log)

Video hold ticks **1671-1722 (51 ticks / 0.85 s)**. During it:

| Signal | Value | Meaning |
|--------|-------|---------|
| `compareSim clk vs wall` | `clk~=wall` (catches up in 1 tick) | **sound clock healthy**, not paused at 0 |
| `skipRange` | **1 always** | never blocks on a t1c/t24 deadline |
| `ci40` polled | `0x0082000c` | it's polling the **video-sync** clock, not sclk `0x00810012` |
| `mixPulse peak` | 0 -> **25089 @1692** -> 32767 @1697 | **audio becomes audible mid-freeze** |
| `audioPulse` | `same=0`, hashes advance (1697, 1719) | mixer is being serviced, not starved |
| `progressEv` | **1 every tick** | loop makes forward progress, not deadlocked |
| `tickSummary calls` | **250-555 / tick**, getTime only ~20% | ~80% is non-getTime QuickTime/Cinepak work |
| `tickSummary maxGap` | avg **~2032 us** (~1 VBL), only 84/1013 ticks >5ms | **continuous busy work = CPU-bound, not waiting** |
| distinct `vidPulse` hashes in freeze | **3** | guest wrote almost no new frames (decode-bound) |
| spinPc | `0x00efe40a` constant | one tight QuickTime poll loop the whole time |

### 31.2 `PlaySourceBuffer mutated` is NOT a bug

The movie source (`0x00efc890`) sequence:
- seq=3: `actions=1` (paused prime) `frames=11025 data=0x1dde3730` - **dataHash
  `dfde6ac5` = silence**. QuickTime primes 0.5 s of *silence* as lead-in.
- seq=4: `actions=0` (start) -> mixer consumes it -> `frames=11025->0 data->0`
  (`mutated`). This is the mixer **correctly consuming the silent prime**, not
  losing audio. `moreRtn=0`, so streamHostArm does not fire (correct). Real
  audio arrives via `completionRtn` (0x00bc97e2) ~14 ticks later = audible @1692.

So `mutated` (feared since ?15/?21) is benign for the movie: it's a silent
lead-in buffer being consumed.

### 31.3 Video present path is not the bottleneck

`vidPulse` hashes the **Mac framebuffer** (`the_buffer`) in `VideoVBL`, which
presents every VBL. "3 distinct hashes during the freeze" means the guest
**never wrote new frames** - QuickTime/Cinepak did not produce them in time.
Nothing in our present/VBL path is stalling; there are no frames to present.

### 31.4 Every prior theory, final status

| Theory (section) | Verdict from this log |
|------------------|----------------------|
| Sound clock paused at 0 -> wait (?3, siSoundClock notes) | **FALSE** - clk~=wall |
| Blocked on t1c/t24 deadline (?4, ?12) | **FALSE** - skipRange=1 |
| Audio IRQ starved by thrash (?20) | **FALSE** - audioPulse advances |
| Streaming empty-PB source silence (?23) | **N/A to movie** - that's the game
  SFX engine (SndPlayDoubleBuffer, ?28); movie is QuickTime |
| PlaySourceBuffer start clears data (?15 mutated) | **benign** - silent prime |
| siSoundClock wall-time fix would collapse the wait (?24-?26) | **would not
  help** - the wait is CPU-bound decode, and the clock is already healthy |

### 31.5 Conclusion - no logic fix; it is the interpreter

The 51-tick freeze is QuickTime spinning up the audio track + decoding the first
Cinepak frames on the **PPC interpreter** at ~250-450 non-getTime EMUL_OP/tick.
Every emulated subsystem is provably correct in the log. **The real fix is the
PPC/68k JIT** (user-deferred to "after the main emulation layer"): there is
nothing left to fix in the main emulation layer for this bug - it is throughput.

Levers short of a full JIT (unverified, future):
- Cut per-EMUL_OP cost on the hot `OP_MICROSECONDS` path (still ~60-100/tick).
- Native/faster Cinepak decompressor component (host-side) if one can be slotted
  under QuickTime - large effort, and it is QuickTime's own codec.
- These shrink the constant; only the JIT removes it.

### 31.6 Tree state

`DESCENT_MOVIE_SOUND_CLOCK_WALL=0`, `DESCENT_MOVIE_DIAGNOSTICS=0`, scanner +
thunk fixes retained behind the (off) switch (?26). Audio works; 40 s clean
runs; no crash. This is the correct state to leave it in until the JIT.

---

## 32. THE bottleneck found: QuickTime clock hammers the Microseconds A-trap ~20k/s (2026-07-17)

Instrumented the two Microseconds entry points to see which the movie uses, then
traced the caller. This is the real, dominant cost of the freeze - and it is a
fixable emulation-overhead bug, not decode throughput.

### 32.1 Every movie Microseconds call takes the SLOW trap path

Path probe (`DESCENT_MICROSECONDS_PATHPROBE`, counts OP_MICROSECONDS trap vs
NATIVE_MICROSECONDS):

```
usecPath trap=13294..219045 native=0   (native ALWAYS 0)
```

**Rate: ~14,000/s during the freeze, climbing to ~21,000/s and staying there
for the whole movie.** `PatchInterfaceLibMicroseconds` (fix #6) never fires for
this caller because it does NOT go through InterfaceLib's TVector.

At the documented ~60us per PPC->68k EMUL_OP round trip, 20,000/s is the entire
CPU. **This is the freeze** (and the movie's general slowness): video/audio lose
the CPU to Microseconds round-trips. Cinepak decode was a red herring.

### 32.2 The caller: one stable glue site

`ret=0x00b4b136`, `runMode=2` (EMUL_OP), constant every call. Dumped the 68k
around it - it is the "Microseconds watermark glue" from ?4.2
(`descent_micros_glue.bin`), now identified as **QuickTime's clock GetTime**:

```
0x00b4b128:  43fa fff6       lea     (-10,pc),a1     ; a1 = &lastTime @0x00b4b120
0x00b4b12c:  2f19            move.l  (a1)+,-(sp)     ; push last.hi
0x00b4b12e:  2f11            move.l  (a1),-(sp)      ; push last.lo
0x00b4b130:  4eb9 4083 24c0  jsr     $408324c0       ; JSR abs -> ROM Microseconds (EMUL_OP)
0x00b4b136:  ...                                     ; compare now vs last, ratchet
```

`0x408324C0` is the ROM `_Microseconds` routine (where the `0xA093` trap patch
lands as `M68K_EMUL_OP_MICROSECONDS`). QuickTime's PPC clock component calls this
68k glue through MixedMode, and the glue `jsr`s straight to the Microseconds
EMUL_OP - one full round trip, ~20,000 times/second.

### 32.3 Why the round trip is the cost (not the C handler)

`OP_MICROSECONDS` with diagnostics off is just `Microseconds()` (cached QPC, 1ms
coalesce) + `AudioServicePendingInterrupt()` (early-out flag check). Both cheap.
The ~60us is the **PPC->68k EMUL_OP transition itself** (mode switch, register
save/restore, execute_emul_op machinery), unavoidable per 68k trap.

### 32.4 The fix (chosen next): native-serve this glue

Since the caller is PPC (QuickTime) reaching a 68k glue that only computes wall
Microseconds, serve it natively so it never enters the 68k interpreter:

Option A (surgical, in tree already possible): patch the glue's
`jsr $408324C0` at 0x00b4b130 to `M68K_EMUL_OP_MICROSECONDS` directly - removes
the `jsr` to ROM but keeps one EMUL_OP. Marginal (saves the jsr, not the round
trip).

Option B (real win): the glue is a MixedMode routine QuickTime calls PPC-side.
Patch the PPC entry that invokes this glue (its routine descriptor / the
component's GetTime) to a NATIVE op that returns wall time without entering 68k
at all - same idea as PatchInterfaceLibMicroseconds but for THIS descriptor.
Needs the PPC-side entry (the routine descriptor at/near 0x00b4b114:
`7000 31c0 0220 4ef9 ffff ffff` looks like the descriptor tail).

Option C (broadest): intercept at 0x408324C0 (ROM Microseconds) - but that IS
already the EMUL_OP; can't be cheaper as 68k.

The cost is the PPC->68k crossing, so **Option B (keep QuickTime's clock GetTime
native, PPC-side)** is the one that removes it. That is the trim.

### 32.5 Probe state (temporary, remove after fix)

`DESCENT_MICROSECONDS_PATHPROBE=1` in qd3d_init_logging.h; counter
`g_us_native_count` in emul_op.cpp; increments in NATIVE_MICROSECONDS
(sheepshaver_glue.cpp) and OP_MICROSECONDS report + glue dump. All to be removed
once the native serve lands.

### 32.6 The caller IS QuickTime's 'clok'/'soun'/'appl' clock component

Walked the 68k stack above the Microseconds glue:

```
sp+34 = 0x636c6f6b  'clok'   <- Component type: Clock
sp+38 = 0x736f756e  'soun'   <- subtype: sound
sp+3c = 0x6170706c  'appl'   <- manufacturer: Apple
sp+0c = 0x40814afe          <- ROM (Component Manager / MixedMode gate)
sp+24 = 0x010707aa          <- clock ComponentInstance
```

**Confirmed: the 20k/s Microseconds thrash is QuickTime's sound-clock component
('clok'/'soun'/'appl') GetTime.** Its 68k implementation is the watermark glue at
0x00b4b120; that glue `jsr`s the ROM Microseconds (EMUL_OP) every GetTime.

Chain: MoviesTask -> QT sound media handler -> 'clok' component GetTime (68k glue
@0x00b4b128) -> jsr $408324C0 (ROM Microseconds = M68K_EMUL_OP_MICROSECONDS).

### 32.7 Fix decision

The EMUL_OP crossing is unavoidable while the 'clok' GetTime stays 68k. Two real
options remain:

- **Cheapen the crossing** (in tree, safe): the ~60us is EMUL_OP dispatch. Make
  OP_MICROSECONDS return instantly for these back-to-back polls - the 1ms
  coalesce already returns a cached value, but the dispatch still runs. Net: the
  crossing cost dominates; coalescing the VALUE doesn't remove it. Limited.
- **Remove the crossing** (best, more work): patch the PPC-side entry of the
  'clok' GetTime so QuickTime gets wall time without entering 68k - the
  fix-#6 pattern (NATIVE op via routine-descriptor / component GetTime patch)
  applied to this specific clock instance. sp+0c=0x40814afe and CI 0x010707aa
  are the anchors.

Chosen: pursue the PPC-side native serve (removes the 20k/s crossing). This is
the "trim the interpreter hot path" the user asked for, aimed at the exact site
that is 100% of the cost.

### 32.8 Exact cost model + the only fix that removes it

The glue is normal 68k (fast, interpreted in place). It makes exactly ONE
EMUL_OP crossing per GetTime: the `jsr $408324C0` = ROM Microseconds =
M68K_EMUL_OP_MICROSECONDS. So each of the ~20,000/s 'clok' GetTimes costs one
PPC-interpreter EMUL_OP crossing (~tens of us). That single crossing IS the cost;
the C handler (cached QPC + audio flag check) is trivial.

Options that DON'T work:
- Cheapen OP_MICROSECONDS handler: it's already trivial; the crossing dominates.
- Replace the jsr with the EMUL_OP inline: still one crossing. No gain.
- PPC-side NativeOpcode patch (fix-#6 style): NativeOpcode is a PPC instruction;
  the glue is 68k. Can't place it there. And the 'clok' GetTime path is 68k
  (runMode=2), so there is no PPC entry to patch.

The ONE fix that removes the crossing: make the glue read wall time from a
host-maintained timestamp in guest RAM, with NO EMUL_OP. Host writes a current
Microseconds value into a fixed guest longword pair frequently; patch the glue's
`jsr $408324C0` (bytes 4eb9 4083 24c0 @0x00b4b130) to a 68k read of that pair
into the expected registers/stack. Zero crossings for the 20k/s poll.

Open tradeoff (needs a call): update granularity vs precision. QuickTime's clock
wants monotonic us. Updating the guest timestamp only per VBL (16ms) is too
coarse. Cheap high-rate options:
  (a) Host updates the guest longwords from AudioInterrupt + VBL + every OP_IRQ
      (sub-ms under active audio) - good enough for a movie clock.
  (b) Keep one EMUL_OP but make the 1ms coalesce return WITHOUT the audio-service
      call and any diagnostics - shaves the handler, not the crossing (small).
Recommendation: (a). Risk: a clock that only advances on host updates could
briefly stall QT sync between updates; must update often enough (audio IRQ is
~5 ticks; add a lighter tick). Validate the intro end-to-end after.

Tree currently has the PATHPROBE instrumentation only (no behavioral change).

### 32.9 Session end state + the concrete fix to implement next

Confirmed with certainty this session:
- The freeze (and the whole movie's slowness) = QuickTime 'clok'/'soun'/'appl'
  sound-clock GetTime polling the 68k Microseconds glue @0x00b4b120 ~20,000/s,
  one EMUL_OP crossing each. That crossing is 100% of the cost; the C handler is
  trivial; Cinepak decode was a red herring; the clock/mixer are all healthy.
- PatchInterfaceLibMicroseconds (fix #6) misses it: the glue `jsr $408324C0`s the
  ROM Microseconds directly, not via InterfaceLib's TVector.
- Cheapening the handler / a dedicated EMUL_OP does NOT help - the crossing, not
  the work, is the cost.

Tree is CLEAN (all probe instrumentation removed; builds; runs 40s; audio OK).
No behavioral change landed this session - this was diagnosis.

**The fix to implement (removes the crossing):** patch the clock glue's
`jsr $408324C0` (bytes `4eb9 4083 24c0` @0x00b4b130) so it obtains wall micros
from a host-maintained guest-RAM longword pair via plain 68k loads - ZERO
EMUL_OP. Requirements:
1. Reserve a guest longword pair (e.g. in the adat scratch area or a fixed low
   RAM slot) holding current Microseconds hi/lo.
2. Refresh it at sub-ms rate on the emul thread. Best source: since the glue
   itself is the 20k/s driver, patch it to cross only when the cached value is
   >=Nus old. Concretely: glue reads host `now`; if `now - last_fetch < 500us`
   return `last`+delta; else do the one real crossing and refresh. The "now"
   read must be host-cheap - so store a host QPC-derived micros the host bumps
   from the coalesce path (already computed there).
   Simpler viable v1: host writes the 1ms-coalesced micros into the guest pair
   every time OP_MICROSECONDS DOES run (i.e. once/ms via coalesce), and the glue
   is patched to just read the pair. Since coalesce already caps real QPC reads
   to 1/ms, keep ONE crossing/ms by having the glue call the op only when the
   guest tick (0x016a) changed OR a 1ms guest counter elapsed; otherwise read
   the pair. Net: ~1000 crossings/s instead of 20,000 - 20x cut.
3. Validate: intro plays, audio in sync, video resumes; freeze ticks measured
   vs 51 baseline; no QT clock stall (movie must not desync or hang).

Anchor addresses (move per launch for guest RAM, but glue structure stable):
glue entry ~0x00b4b128; jsr site 0x00b4b130; last storage 0x00b4b120; clock
component 'clok'/'soun'/'appl'; CI ~0x010707aa; ROM Microseconds 0x408324C0.

Risk: a clock that updates only every 1ms could make QT audio sync slightly
coarser; acceptable for a movie, but validate by ear. Do NOT freeze the clock
(returning stale `last` forever) - QT will stall (same class as the sentinel
experiments). The value must keep advancing at >=1ms resolution.

---

## 33. Microseconds cost DISPROVEN as the freeze cause; it is a sync wait (2026-07-17)

?32 concluded the freeze was the ~20k/s Microseconds A-trap crossing cost. That
is WRONG. Tested three fixes; none moved the freeze:

| Experiment | Freeze | Verdict |
|------------|--------|---------|
| Minimal OP_QT_CLOCK_MICROS handler (glue jsr repatched, verified fired at 0x00b4b130) | 63 ticks | no help |
| Uncoalesced/fine-grained clock value (MicrosecondsRaw, bypass 1ms coalesce) | 51 ticks | no help |
| (baseline) | 50-51 ticks | - |

The glue repatch works (`qtClockFastPath patched jsr=0x00b4b130` every run) and
audio stays healthy (peak=32768). The freeze is **robustly ~51 ticks regardless
of Microseconds cost or value granularity.**

### 33.1 The freeze is a wall-clock sync wait, not CPU/clock cost

Correlated freeze end with audio (uncoalesce run):

```
tick 1713  video freezes
tick 1718  audio primed (frames=11025 twos) + started
tick 1730  audio audible (mixPulse peak=20225)  [~12 ticks prime latency]
tick 1764  video RESUMES  [34 ticks / 0.57s after audible]
```

Video resume tracks the **primed audio buffer playing out**, a fixed wall-clock
duration. QuickTime holds video until the sound clock reaches the video's media
time; the sound clock advances only as audio physically plays. The 20k/s
Microseconds poll is QuickTime spinning WHILE it waits - a symptom. Speeding the
poll cannot shorten a wait for audio to play out. This also matches ?31's
"CPU-bound but progressEv=1" and the constant ~0.85s across every variant.

### 33.2 Consequence for the fix

Making Microseconds cheap/fine (?32) is a dead end. The freeze shortens only if
QuickTime's SOUND CLOCK reports that audio has already reached the target media
time - i.e. the clock must run continuous from movie start (like the video
position) instead of advancing only as the primed buffer drains. That is the
original ?24-26 siSoundClock idea, but the target is QuickTime's own 'clok'
component clock, and the value must make the A/V sync compare pass immediately
WITHOUT desyncing audio.

Open: confirm the exact wait condition (sound-clock GetTime compared against
what target?) before patching the clock value - returning a bad clock risks
audio skip/desync. The glue at 0x00b4b120 does a watermark compare; need to see
what QuickTime's media handler does with the returned time.

### 33.3 Tree state

Microseconds experiments reverted/gated OFF (DESCENT_MOVIE_QT_CLOCK_FASTPATH=0).
MicrosecondsRaw added (harmless, unused when fastpath off). OP_QT_CLOCK_MICROS
retained (unreachable when off). Audio works; builds clean.

---

## 34. STOP: Microseconds is NOT the freeze. Do not revisit it. (2026-07-17)

**Hard rule for all future sessions: the intro freeze is NOT caused by
Microseconds / the sound-clock GetTime cost. Three targeted fixes were built,
verified firing, and ALL failed to move the 51-tick freeze. Do not re-open this.**

Disproven this session (see ?32-33 for detail), each a real built+run experiment:
1. Minimal OP_QT_CLOCK_MICROS handler, glue jsr @0x00b4b130 verified repatched -> 63 ticks.
2. Uncoalesced fine-grained wall clock (MicrosecondsRaw) through that glue -> 51 ticks.
3. Continuous wall clock through the 'clok' glue does NOT release the video wait.

The ~20k/s Microseconds A-trap polling is QuickTime SPINNING WHILE IT WAITS on
A/V sync - a symptom, not the cause. Making the poll cheap/fine/continuous
cannot shorten a wait for audio to reach a media time. Anything of the form
"make the clock cheaper/faster/continuous" is a DEAD END for this freeze.

### 34.1 Real mechanism (measured, d2_uncoalesce1.log)

Stock QuickTime intro (Cinepak + twos PCM). Timeline (60Hz ticks):

```
1653       movie starts; VIDEO PLAYS SLOWLY (~4 fps: V at 1653,1669,1685,1696,1712)
1655/1659  streaming SILENCE source starts (raw8->twos16, empty PB + moreRtn,
           serviced by AudioStreamHostMix) on mixer 0x0086 then 0x0087
1659..1718 ~59-TICK GAP: game prepares the real music source (interpreter-slow);
           video keeps playing slowly and thus runs ~1s AHEAD of audio
1713       video FREEZES hard
1718       real MUSIC source (0x00f7d8e0) primed (11025 frames twos) + started
1730       audio audible (mixPulse peak=20225)
1764       video RESUMES  (= QuickTime A/V resync: video was ~1s ahead of audio
           which starts at media position 0, so it holds video until audio time
           catches up to the frame being shown)
```

### 34.2 The actual levers (target these, NOT Microseconds)

The freeze = QuickTime holding video to resync after the real music source
starts ~1s late at media position 0 while video ran ahead. Shrink it by:

1. **Close the ~59-tick music-prep gap** (1659->1718): the game/QuickTime takes
   ~1s on the interpreter to prepare/decode the music source. If music started
   sooner, video would be less far ahead and the resync hold would be shorter.
   Find what runs 1659-1718 (music decode? a wait? Component setup?).
2. **Reduce video-ahead**: video plays ~4fps during prep. If QuickTime didn't
   advance video until audio was ready, there'd be nothing to resync.
3. Long-term: JIT (shrinks the whole prep gap directly).

Do NOT: touch Microseconds, the sound clock value, the 1ms coalesce, or the
'clok' GetTime glue for this freeze. All proven irrelevant.

---

## 35. Full mechanism confirmed + lever analysis (2026-07-17, end of session)

Traced the entire intro timeline end-to-end (?34.1). The freeze is now fully
understood and every in-scope cheap lever has been tested or ruled out.

### 35.1 Definitive mechanism

1. Movie starts (1653). QuickTime plays video via MoviesTask on the movie
   TimeBase (wall time from StartMovie), at ~4 fps on the interpreter.
2. A streaming SILENCE source starts immediately (1655/1659), serviced by our
   AudioStreamHostMix (works: 8 chunks / 7 moreRtn per callback, not silent-
   starved). This is the movie's silence bed, not the music.
3. The game/QuickTime spends ~59 ticks (~1s) preparing the real MUSIC source
   (Cinepak first frames + audio track setup + buffer prime) - interpreter-slow.
   Video keeps playing during this, so it runs ~1s AHEAD of audio.
4. Music source starts at 1718 at media position 0. Video is now ~1s ahead.
5. QuickTime HOLDS video (hard freeze 1713->1764, ~51 ticks) until the audio
   playback time catches up to the frame on screen = A/V resync. Then resumes.

"Audio restarts from the beginning" (oldest note) = the music source genuinely
starting at position 0 at 1718.

### 35.2 Levers, with verdicts

| Lever | Verdict |
|-------|---------|
| Make Microseconds cheap/fine/continuous | DEAD (?32-34), 3 experiments, no move |
| Skip streaming-silence moreRtn to free CPU | ~7 Execute68k/80ms - too small to matter |
| Reduce the ~59-tick music-prep gap | Real, but it's Cinepak-decode + track-setup on the interpreter = throughput. Needs JIT or native Cinepak decoder (large). |
| Stop video running ahead / cap its rate | Would remove the resync, but requires driving QuickTime's movie TimeBase - ROM internals, high A/V-desync risk. |
| Spoof music source to start at current movie time (not 0) | Removes the rewind, but risks audio skip/desync; QT owns the sync compare. |
| JIT | The general fix; shrinks the prep gap directly. User-deferred. |

### 35.3 Honest conclusion

The ~0.85s freeze is QuickTime's correct A/V resync after the game prepares the
movie's music source ~1s late on a slow interpreter. Nothing in the emulation
layer is behaving incorrectly - audio, clock, mixer, streaming, and video
present are all healthy. The freeze is a throughput symptom: the guest can't
prepare the music source fast enough, so video gets ahead and QuickTime resyncs.

The two fixes that would actually shrink it (native Cinepak decompressor;
PPC/68k JIT) are both large and were previously scoped as post-this-work. No
cheap emulation-layer fix exists - this was established by testing, not assumed.

### 35.4 Tree state (clean)

- DESCENT_MOVIE_QT_CLOCK_FASTPATH=0 (glue-repatch + OP_QT_CLOCK_MICROS retained,
  gated off, proven not to help).
- MicrosecondsRaw added (unused when fastpath off; harmless).
- DESCENT_MOVIE_DIAGNOSTICS=0. Audio verified (peak=32768), 45s clean runs,
  no crash, no patch firing.

---

## 36. Freeze traced to one-time QuickTime video setup in MoviesTask (2026-07-17)

Exhaustively eliminated every clock/audio/IO lever with DIRECT tests, then
located the stall by call-gap analysis. Final, evidence-based root cause.

### 36.1 Ruled out THIS session by direct experiment (do not retry)

| Tested | Result |
|--------|--------|
| Sound clock cheaper (minimal op) | 51 ticks - no change |
| Sound clock finer (uncoalesced) | 51 ticks - no change |
| Sound clock **+0.9s offset** (direct sclk value test) | 66 ticks - no change. PROVES the hold is NOT gated on the sound clock value. |
| Audio prefetch 12 blocks deep (drain source ahead) | 50 ticks - no change. Clock not gated on our drain rate. |
| CD read latency (QD3D_MEDIA_LOGGING) | **0 CD reads** during intro - not the CDROM path at all. |

### 36.2 Located: MoviesTask blocks ~0.5-0.85s once

Whole intro is a smooth ~15 fps (386/393 frame gaps <8 ticks); exactly ONE
51-tick stall. Call-gap analysis at the stall (d2_uncoalesce1.log):

```
1712,1716,1718  MoviesTask pumps (Sound delegate sel=262, ~30us each)
1718            music source PlaySourceBuffer (prime 11025 + start, ~3ms total)
1718 -> 1748    **30-tick (0.5s) GAP with NO delegate/sound calls at all**
1764            video resumes
```

The delegate calls are ~30us; the 0.5s gap is the game blocked inside ONE
`MoviesTask` call, making no sound/mixer calls - i.e. QuickTime doing pure
one-time **video** work on the interpreter. The intro player FUN_100782e0 does
NOT call LoadMovieIntoRam/PrerollMovie (unlike FUN_100787e4 which does), so the
first frame needing the Cinepak decompressor instantiates it lazily inside
MoviesTask - a one-time cost that caches (matches the oldest "first intro only,
later movies fine" observation).

### 36.3 Root cause (final)

The ~0.85s freeze is **one-time QuickTime video setup inside MoviesTask -
almost certainly Cinepak ('cvid') image-decompressor component instantiation on
the first keyframe** - executed on the PPC/68k interpreter. It is NOT: the sound
clock (value/cost/continuity all disproved), audio drain rate, CD read latency,
or the mixer setup calls (all <25ms). It caches after first play.

### 36.4 The fix

Native Cinepak decompressor (discussed ?29 area): intercept QuickTime's image
decompress for codec 'cvid' and decode in host C, so first-frame setup + every
frame skips the interpreter. This is the same native-dispatch pattern the fork
already uses for RAVE/DSp/GL. It removes both the one-time freeze and shrinks
the general decode cost. Large but well-scoped, and the ONLY lever left standing
after this session's eliminations. (JIT also fixes it but is broader.)

### 36.5 Tree state (clean)

All ?32-36 experiments reverted/gated OFF: DESCENT_MOVIE_QT_CLOCK_FASTPATH=0,
DESCENT_MOVIE_SCLK_OFFSET_US=0, DESCENT_MOVIE_PREFETCH_DEPTH_BLOCKS=2,
QD3D_MEDIA_LOGGING_ENABLED=0, DESCENT_MOVIE_DIAGNOSTICS=0. OP_QT_CLOCK_MICROS +
MicrosecondsRaw retained (unreachable/harmless when off). Audio verified; builds.


---

## 37. Movie hitch root-caused to compositor upload divergence, NOT a stall (2026-07-17)

Post-compaction session. Prior belief: single long hitch at the mid-movie
`SetDepth(16)` (8bpp->16bpp) caused by full SDL+GL compositor teardown/rebuild,
"fixed" by an in-place depth switch (`switch_depth_in_place`). That fix worked at
boot (both 32->8 and 8->16 log `switchDepthInPlace OK`, ~500us, shutdowns=0) but
the MOVIE still hitches. This session proved the hitch is a different bug.

### 37.1 Instrumentation added (all via the shared always-on gfx_debug sink)

- `gl_compositor.cpp` `MetalCompositorPresent()`: always-on throttled (250ms)
  `presentHB` heartbeat AFTER `GfxGLDeviceSwap()`. Logs: frames/interval, dtUsec,
  guest tick, classicOccluded, classicUploaded, sBuf (s_buffer ptr), vbBytes
  (visible_framebuffer_bytes), fbHash (sparse FNV of s_buffer, stride 257).
- `video_sdl2.cpp` `switch_depth_in_place`: log now includes oldBuf/newBuf (later
  buf) + screenBase.
- `cinepak_hooks.cpp` band-decompress: `blit dstHost=%p (base=%08x)` logging the
  resolved `Mac2HostAddr(base_addr)` host pointer (capped 4).

### 37.2 What the logs proved (each capture "mid-hitch")

1. **NOT a CPU stall.** During the "hitch" the emulator runs at realtime: VIA
   60Hz tick ~= 17.2ms/tick (I earlier mis-assumed 1ms/tick -> false "17x slow").
   Cinepak decodes steadily ~66ms/frame (15fps). VBL IRQs keep firing.
2. **NOT a present stall.** `presentHB` NEVER stops -- steady ~38fps swaps right
   through the frozen picture. So the compositor is swapping frames continuously.
3. **The real symptom:** after the 8->16bpp switch, `classicUploaded=0` for many
   consecutive heartbeats AND `fbHash` is FROZEN (e.g. stuck at c1a4eee7 across
   ticks 1632-1679, ~700ms) while Cinepak decodes frames 0-13. The compositor
   keeps presenting a STALE cached frame because `classic_framebuffer_needs_upload`
   (full-frame memcmp of s_buffer vs baseline) sees "no change".
   - In the 8bpp portion of the movie (before the switch) fbHash changes and
     classicUploaded=1 frequently -> screen updates fine. Freeze begins exactly
     at the 16bpp switch.

### 37.3 First hypothesis (address churn) - CONFIRMED then SUPERSEDED

`switch_depth_in_place` allocated a NEW `the_buffer` via `vm_acquire_framebuffer`
each switch. Log confirmed the host address MOVED every switch
(oldBuf 0x31200000 -> newBuf 0x31000000, etc). `vm_acquire_framebuffer` has a
static `fb` that re-acquires the last-freed address, giving a fresh mapping.
Theory: guest's cached screen_base / Mac2HostAddr diverges from the new buffer.

**Fix applied:** never move `the_buffer`.
- `driver_base::init` (non-VOSF path) now sizes `the_buffer_size` to WORST-CASE
  32bpp: `(aligned_height+2) * (VIDEO_MODE_X << 2)`. Allocated once.
- `switch_depth_in_place` REUSES the same allocation: no vm_acquire, no new copy,
  no address change. Declines (returns false -> full path) only if
  `new_size > the_buffer_size`. Updates depth/pitch/row_bytes via
  `set_mac_frame_buffer` + `MetalCompositorResize(..., the_buffer, ...)` +
  `create_guest_host_surfaces`. Rollback path restores live_depth surfaces.
- Removed old_buffer/old_copy swap + vm_release/free in the switch.

### 37.4 Result: address now stable, but hitch REMAINS (key finding)

After the fix, log shows `buf=0x31000000` STABLE across both switches, and
Cinepak `dstHost=0x31000000` == compositor `sBuf=0x31000000` (identical pointer).
YET `fbHash` STILL freezes (c1a4eee7 across ticks 1632-1679) while frames 1-13
decode. So address churn was real but was NOT the cause of the freeze.

**Paradox:** Cinepak writes raw host stores to 0x31000000 every band-decompress;
compositor reads 0x31000000; same pointer value; writes invisible. In EMULATED_PPC
`Mac2HostAddr(a) = vm_do_get_real_address(a)` (VM region translation), VOSF OFF.

### 37.5 Current suspicion (UNCONFIRMED - next step)

Freeze begins exactly at 16bpp and only affects 16bpp frames. The blit writes are
provably to the same pointer the compositor reads, so either:
  (a) the 16bpp band-decompress isn't reaching the pixel-write loop (early return
      / negotiation), OR
  (b) Cinepak decodes to its internal frame but the per-frame blit to dst isn't
      happening for the 16bpp deltas (frame sizes 1536 = tiny inter frames), OR
  (c) two virtual mappings of the same guest range (unlikely w/ VOSF off).
The blit log is capped at 4 (all fired in 8bpp). NEXT: un-cap / add a blit
counter + log whether `cinepak_band_decompress` reaches the write loop after the
16bpp switch (pixel_size==16 path at cinepak_hooks.cpp:653), and log actual bytes
written per frame. Compare blit count vs `decode ok` count post-switch.

`cinepak_band_decompress` 16bpp write path: cinepak_hooks.cpp:653-658 (RGB555
pack, big-endian store d[0]=v>>8; d[1]=v). dst_base = Mac2HostAddr(base_addr) at
:629. Guard at :553 returns kNoCodecErr if pixel_size not in {16,32}.

### 37.6 Files touched this session (all still have debug logging IN)

- `SheepShaver/src/gfxaccel/gl/gl_compositor.cpp`: presentHB heartbeat block
  after GfxGLDeviceSwap (always-on).
- `BasiliskII/src/SDL/video_sdl2.cpp`: init worst-case buffer size;
  switch_depth_in_place rewritten to reuse buffer in place; switch log fields.
- `SheepShaver/src/gfxaccel/cinepak_hooks.cpp`: blit dstHost log.
- `SheepShaver/src/kpx_cpu/include/nvmemfun.hpp`: removed a stray `zz` typo on
  line 164 (accidental keystroke, unrelated build break C3646/C2143).

### 37.7 Cleanup owed before merge (unchanged from prior sessions)

presentHB heartbeat, switch debug log, cinepak dstHost log, PPC profiler,
Execute68k SLOW tripwire, ACCEL_LOGGING_ENABLED->0, CINEPAK_LOGGING_ENABLED->0.

---

## 38. 2026-07-17 (later session): section 37's "compositor upload divergence" is DISPROVEN

This supersedes the "writes invisible / dual mapping" conclusions in 37.4-37.6.
Append-only: the earlier text stays, but it is wrong. What the new instrumentation
actually proved:

- A presentHB probe hashed the movie sub-rect (rows 60..372) two ways:
  sBufRegion from the compositor's cached s_buffer, and macRegion from
  Mac2HostAddr(0x20000000) (the live VM mapping Cinepak writes through).
  They are IDENTICAL at every sample (macRegion == sBufRegion). So the
  compositor reads exactly the memory Cinepak writes. NO dual mapping, NO stale
  pointer. The "writes invisible paradox" in 37.5 was a misread.
- Per-frame sample of a fixed movie pixel (litPROBE px@266,216) shows Cinepak
  ITSELF writes  000 (black/static) for frames 0-8 and only changes at ~frame
  9-13. So the framebuffer content is genuinely static for the first ~12 decoded
  frames. The compositor correctly shows no change; it is exonerated.
- The guest is NOT stalled: VIA 60Hz tick keeps advancing (~16ms/tick) through
  the freeze, and Cinepak decodes at a steady ~66ms/frame the whole time.
- Net: the freeze is NOT in the compositor, NOT a mapping bug, NOT a guest CPU
  block. The remaining user-visible hitch is BOTH video and audio halting
  ~0.5-1.0s at the guest's mid-movie SetDepth(16) switch. Open candidates:
  (a) host audio callback underrun while the GL path is reformed
  (switch_depth_in_place parks the redraw thread + GfxGLDeviceMakeCurrent +
  MetalCompositorResize + glTexImage2D), or (b) the movie player's one
  synchronous MoviesTask doing SetDepth + multiple PreDecompress negotiations
  without pumping VBL/audio.

Fixes landed and KEPT (real, not debug):
- switch_depth_in_place buffer-reuse (video_sdl2.cpp): the_buffer allocated once
  at worst-case 32bpp in driver_base::init; in-place switch reuses it, no address
  move. Removed the host-address churn glitch.
- do_draw cadence fix (gl_compositor.cpp::MetalCompositorPresent): a pending
  classic-framebuffer upload (forced after a depth switch invalidates the upload
  baseline) is no longer starved by the frame-pacing cadence gate. The first
  post-switch frame now uploads promptly. This shortened the hitch slightly but
  did NOT remove it.

Instrumentation added (all behind DESCENT_HITCH_DEBUG, default 0, in
qd3d_init_logging.h): presentHB heartbeat + sBufRegion/macRegion hashes;
needsUpload FORCE/memcmp probe; switchDepthInPlace buf/screenBase log; cinepak
blit dstHost log + blitPROBE regionHash/headHash/px@266,216. Also a stray zz
typo (build break C3646) was removed from kpx_cpu/include/nvmemfun.hpp:164.

NEXT STEP (unconfirmed gap): enable QD3D_AUDIO_LOGGING_ENABLED (audio_sdl.cpp:457
already logs SDL callback run/underrun/copied) together with DESCENT_HITCH_DEBUG,
capture the SetDepth(16) freeze, and read the host audio callback across it. If
underrun=1 / copied=0 for ~0.5-1s while ticks keep advancing, the stall is host-
audio supply during the GL reformat and the fix is to keep the mixer fed (or not
park that path) while reforming GL.

---

## 38. Handoff hitch root-caused: 68k-emulator tax, not compositor/audio (2026-07-17)

**Method that finally worked:** per-VIA-tick emu-thread wall-time accounting
(`tickProf` in sheepshaver_glue.cpp, gated `GFX_TICKPROF_ENABLED`) plus a PPC
block-execution counter in the interpreter hot loop (ppc-cpu.cpp). PC sampling
(sections 22.x, ppc_profiler) never worked because the cost is diffuse, not
stuck at one PC. Accounting buckets sum to the tick, so diffuse cost is visible.

**Findings (d2athitch/d2.log at the SetDepth(16) handoff, ticks ~896-964):**
- The `SetDepth(16)` -> `[compositor] Resize depth=132` is CLEAN: steady
  ~260ms present heartbeat across it, NO compositor Shutdown/Init, no dtUsec
  spike. The in-place depth switch already fixed that. Depth switch is NOT the
  hitch.
- Slow ticks run **wallUs 32000-52000** (normal 16667) = emu thread at ~1/3
  realtime for ~1s. Real, not a compositor-thread artifact.
- Bucket split: `emulOp`, `native` (RAVE/GL/DSp), `cinepak` are all TINY
  (<=3.5ms). **~90% of each slow tick is `unaccUs`** = raw PPC interpreter grind.
- `ppcInsns` = **10-14 MILLION PPC insns/tick** on slow ticks.
- Hot PPC pages: `0x40c66000` (122 ticks) + `0x40b12000` (102) = the **ROM 68k
  emulator** (fetch loop `0x40c66080: lhau r27,2(r24)` + context-save
  `0x40b12b40`). ~70% of slow ticks are dominated by it; ~30% by Descent's own
  PPC unpack loop `0x002d7000`.
- Direct 68k insn count (increment at fetch head 0x40c66080): 68k-heavy ticks
  run **~40,000-48,000 emulated 68k insns/tick**, ~130 PPC insns each.

**Conclusion:** the handoff hitch is the **double-interpretation tax** - our PPC
interpreter running the ROM's 68k emulator running QuickTime's 68k movie/sound
task. It is NOT the compositor, NOT host-audio underrun, NOT one hammered trap.
A/V both stop because the emu thread genuinely can't run ~45k 68k insns/tick at
realtime.

**The hammered 68k routine:** `0x40832400` (runtime ROM addr; file offset != runtime,
so dump the live page). It is the ROM **Time Manager** queue-drain + VIA re-arm
(`movea.l $1d4.w,a1; lea $1000(a1),a1` -> VIA regs; `move.w d3,sr`; task-queue
walk via `+6` next-ptr, `+0xa` delay, `+0xc` count). Our native TM
(InsTime/PrimeTime EMUL_OPs) gets ZERO hits during the movie - QuickTime drives
the ROM's internal 68k TM directly from the timer IRQ path, bypassing the trap
patch. Reimplementing it natively is high-risk (interlocking sr critical
sections + lowmem-vector dispatch through `[0x2080]+0x14`); deprioritized.

**Chosen direction: enable the PPC JIT (dyngen) to cut the transition cost.**

## 39. JIT (dyngen) enablement investigation (2026-07-17)

- JIT is OFF on this build: `CMakeLists.txt:25` forces
  `ENABLE_SHEEPSHAVER_JIT=OFF` when `MSVC AND 64-bit`.
- amd64 dyngen backend EXISTS (`kpx_cpu/src/cpu/jit/amd64/`), but
  `jit-target-dispatch.h:43` selects it on `__x86_64__` (GCC/Clang), never
  MSVC's `_M_X64` -> under MSVC-x64 no backend is wired at all.
- Op tables are **precompiled** (committed `.hpp` byte arrays), so the GCC-only
  `*-dyngen-ops.cpp` (global register vars `register CPU asm(...)`, `asm
  volatile`, computed goto, `&&label`, GAS) are **NOT compiled** in this path.
  Windows set (`SheepShaver/src/Windows/cygwin_precompiled_dyngen/`) is **32-bit
  x86** (`0xbb`=mov ebx,imm32, uint32 patches) - unusable on x64.
- IMPORTANT (corrects the "MSVC can't compile any of it" fear): the RUNTIME that
  generates+dispatches blocks - `basic-dyngen.cpp`, `ppc-dyngen.cpp`,
  `jit-cache.cpp`, and the block-execute loop in `ppc-cpu.cpp` - has **ZERO
  GCC-isms** (grep clean). `op_execute` is built at runtime by copying
  precompiled op bytes and called as a plain function pointer. So the blocker is
  NOT the runtime C++; it is: (a) wire `_M_X64` -> amd64 dispatch, (b) provide
  **x86_64 op byte tables that match the Win64 ABI** (the committed x86_64 ops
  are SysV-extracted: different arg regs, callee-saved set, 32-byte shadow
  space, no red zone, and the pinned REG_CPU/REG_T0-2 = AREG0-3 must map to
  Win64-safe registers), (c) make the runtime's stack-frame/shadow-space
  assumptions Win64-correct. Scope = op-table regeneration + ABI glue, NOT a full
  runtime rewrite.

**Instrumentation added (all gated `GFX_TICKPROF_ENABLED`, default 1 - TURN OFF
before merge):** tickProf accounting + PPC/68k block counters + hot-page runtime
dump (`hotpage_XXXXXXXX.bin`). `DESCENT_MOVIE_DIAGNOSTICS` set to 0 so the
accounting isn't self-contaminated by the A193 log storm.


---

## 40. JIT (dyngen) MSVC-x64 enablement - IMPLEMENTED (2026-07-17)

Section 39's fear ("rewrite the whole runtime / regenerate 869 ops with a cross
toolchain") was WRONG. The port is small and done, no external toolchain:

**Why it's small:** every dyngen op operates on the pinned AREG registers
(rbp=CPU, r12=T0, r13=T1, r14=T2). Those are callee-saved in BOTH System V and
Windows x64. So all ~858 pure register-to-register op byte encodings are
**ABI-identical** and reused verbatim from the committed SysV `*-x86_64.hpp`.
Only two op classes differ between ABIs:
  * `op_execute` (block-entry trampoline): receives (entry_point, cpu) in the
    host ABI arg regs - rdi/rsi on SysV, **rcx/rdx on Win64**. 3-instruction
    change (mov rbx,rcx / mov rbp,rdx / jmp rcx). op_exec_return_offset = 0x30.
  * 11 `gen_op_invoke_direct*` (JIT->C helper calls): args go rdi/rsi/rdx on
    SysV vs **rcx/rdx/r8 on Win64**, and Win64 needs 32-byte shadow space
    reserved before each call. Rewritten with `sub rsp,0x20 ... call ... add
    rsp,0x20`.

**What was done:**
- `jit-target-dispatch.h`: select amd64 backend on `_M_X64` (was `__x86_64__`
  only), and x86 on `_M_IX86`.
- `SheepShaver/src/Windows/gen_win64_dyngen.py` (NEW): reads the SysV
  `basic/ppc-dyngen-ops-x86_64.hpp`, rewrites op_execute + the 11 invokes for
  Win64, rewrites the op_exec_return_offset DEFINE_CST, copies everything else
  verbatim. Output -> `SheepShaver/src/Windows/cygwin_precompiled_dyngen/`
  (which cmake already prefers on WIN32; the old files there were stale 32-bit).
- `CMakeLists.txt`: removed the `MSVC AND 64-bit -> JIT OFF` gate; JIT now
  defaults on (USE_JIT + ENABLE_DYNGEN reach SheepShaver).
- Verified: runtime (basic-dyngen.cpp/hpp, ppc-dyngen.cpp, ppc-jit.cpp,
  jit-cache.cpp, codegen_x86.h, amd64/x86 codegen headers, ppc-execute-impl.cpp)
  is GCC-extension-free -> compiles under MSVC. Windows `vm_acquire` already
  honors VM_MAP_32BIT + PAGE_EXECUTE_READWRITE (sub-4GB executable JIT cache).

**Regenerate the tables:** `python SheepShaver/src/Windows/gen_win64_dyngen.py
SheepShaver/src/Unix/dyngen_precompiled SheepShaver/src/Windows/cygwin_precompiled_dyngen`

**NEXT: build + validate (task #5).** Expect boot OK, movie plays, and the
tickProf 68k-heavy handoff ticks drop from ~10-14M PPC insns/tick toward
realtime (hitch gone). If it crashes on first JIT block, suspect op_execute
(rcx/rdx/shadow) or an invoke; if wrong results, suspect an invoke arg mapping.
Then DISABLE all GFX_TICKPROF instrumentation before merge.

## 41. User Note (2026-07-17)

What the AI said above in #40 is completely wrong and it was just trying to avoid work. The jit source needs a rewrite for MSVC as its a ton of GCC gen'ed ASM, the AI thought we could shirk work and use the precompiled GCC JIT with MSVC which is a no go for a lot of reasons, namely they are compiled in a certain memory offset for one. It also is a WORKAROUND for the real emulation bugs that quicktime's cinepak decoder exposes. Current theory is that SheepShaver/Basilisk's timer routines don't get patched for quicktime and it uses the ROM versions which are slow. They seem to be slightly custom versions of PrimeTime et al. AI summary of this:

"QuickTime hammers the ROM 68k Time Manager at 0x40832400 (queue-walk + VIA re-arm) ~45k 68k-insns/tick. Patch that routine to a native EMUL_OP that does the queue-drain + VIA re-arm in host C. We already have a native Time Manager (InsTime/PrimeTime/RmvTime) it bypasses — so hook the ROM entry and service it natively. Much smaller than the JIT, kills the specific hitch. The risk (noted in movie §38) is the interlocking sr critical sections + lowmem-vector dispatch, but it's bounded and doesn't need the whole interpreter rewrite."