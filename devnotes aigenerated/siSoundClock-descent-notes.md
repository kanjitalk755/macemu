# `siSoundClock` and Descent II's first-movie wait loop

This doc records what is known (and inferred) about the undocumented `siSoundClock` Sound Manager selector and how *Descent II* uses it during the first intro MVE. It exists because no public spec for `siSoundClock` could be found online.

---

## 1. What `siSoundClock` is in this codebase

* `BasiliskII/src/include/audio_defs.h` defines it as the four-char code `'sclk'`:

  ```cpp
  const uint32 siSoundClock = FOURCC('s','c','l','k');
  ```

* It is a **Sound Component / Sound Manager selector**, not a documented trap or Apple Mixer public API. It appears nowhere in the surviving Inside Macintosh: Sound pages that could be located.

* In the audio component implementation it is delegated to the Apple Mixer via the standard `AudioSetInfo`/`AudioGetInfo` mixer thunk:

  ```
  SetInfo selector=sclk info=0x00000001 source=0x00ee6b90
  GetInfo selector=sclk info=0x1ec10d50 source=0x00ee6b90
  ```

* `GetInfo(siSoundClock)` returns a **ComponentInstance** pointer; the caller reads the CI from `infoPtr`. The returned CI observed in all recent runs is `0x00810012` (e.g. `d2_detect.log` tick 1652/1659).

---

## 2. How Descent II uses it

The movie player does a three-step dance when it creates the second (real music) audio source:

1. `SetInfo(siSoundClock, info = 0x00000001)`
   * The value `1` is almost certainly `kSoundClockIsSyncNoLatency` or an equivalent internal clock-mode constant. It tells the sound component *how* the new source should synchronize.

2. `GetInfo(siSoundClock, &ciOut)`
   * Retrieves the ComponentInstance that represents the source's clock. In the logs this is always `0x00810012`.

3. Later, inside the movie wait loop, it calls **ComponentDispatch GetTime** on that CI:
   * Stack layout: `[header = 0x00040001][TimeRecord *][CI = 0x00810012][result]`
   * The wait loop compares the returned time value against a target derived from the movie's media time.

The same wait loop also polls a **second ComponentInstance**, `ci40`, for video sync. In logs `ci40 = 0x0082000c`. `ci40` advances continuously (~wall µs), while the `siSoundClock` CI for the new music source starts near zero because the source is primed in a paused state.

---

## 3. Why this causes the stutter

* Phase 1: the movie starts a **silent streaming source** (empty PB + `moreRtn`). With `AudioStreamHostMix` this now actually streams silence.
* Phase 2: ~0.9 s later the game creates the real music source, primes two ~0.5 s buffers in paused state, and obtains `siSoundClock` CI `0x00810012`.
* The Apple Mixer-derived clock for that source is paused at the start of the primed buffers, i.e. near time 0.
* The movie player computes a target media time (already ~0.9 s into the movie) and spins on `ComponentDispatch GetTime` until the sound clock reaches it.
* That spin is the ~0.85–1.1 s video freeze + audio restart. The audio "restarts" because the music source is finally unpaused and begins playback from soundtrack position 0.

In real hardware the sound clock exposed by the audio output device is continuous from device open, regardless of whether an individual source is paused. Our emulation returns a per-source clock that pauses, so the player waits.

---

## 4. Interception mechanics

The wait loop is reached via 68k glue, but the actual policy code is PPC (CFM glue visible in frame dumps). The hot site is a `ComponentDispatch` `A82A` call:

```text
move.l #$00040001,-(sp)   ; GetTime selector header
moveq  #0,d0              ; result
A82A                      ; _ComponentDispatch
```

The inline patch in `SheepShaver/src/emul_op.cpp` looks for this pattern near the spin PC and replaces `A82A` with `M68K_EMUL_OP_COMPONENT_DISPATCH` (`0xFE43 + OP_COMPONENT_DISPATCH`). This routes the call into the host `OP_COMPONENT_DISPATCH` handler.

The handler receives:

* `r->a[7]` = 68k SP (parameters are at the top)
* `*pc` = 68k PC already advanced past the EMUL_OP instruction (i.e. `patched_addr + 2`)

It reads:

* `header = ReadMacInt32(sp)` — expected `0x00040001`
* `tr     = ReadMacInt32(sp + 4)` — pointer to `TimeRecord`
* `ci     = ReadMacInt32(sp + 8)` — ComponentInstance

If `ci == AudioGetSoundClockCI()` (`0x00810012`), the handler returns host wall time in `tr` and advances SP, collapsing the wait. Otherwise it must execute the original `A82A` trap for `ci40` and any other callers.

---

## 5. The fallback problem — **unsolved; this is the blocker**

The patch rewrites the `ci40` (video-sync) site as well as the sclk site, so
calls we do not want to host must still run the real `A82A`. **No known way to do
that from inside an EMUL_OP handler works:**

| Attempt | Result |
|---------|--------|
| `Execute68kTrap(0xA82A, r)` | SIGSEGV (PPC PC 0x40d80000) |
| ROM scratch `A82A; jmp abs.l *pc` | Emulator wedges on the tick it first fires |
| Same, with the jump target actually written | Same wedge |
| Same, with `d0` preserved | Same wedge |

**Why it is structural, not a coding slip:** `EmulOp` runs with
`XLM_RUN_MODE = MODE_EMUL_OP`; the glue restores `MODE_68K` only *after* it
returns (`sheepshaver_glue.cpp:283`). Redirecting `*pc` into ROM scratch makes
`A82A` execute during an EMUL_OP unwind rather than from real trap-entry state —
the same unsafe re-entry the `Execute68kTrap` SIGSEGV already demonstrated.

Two genuine defects in the thunk were found and fixed en route (worth keeping,
but they were *not* the root cause — the wedge survived both):

1. **Jump target never written.** `WriteMacInt32(ROMBase + patch_base + 4, *pc)`
   hands a **host** address to a function taking a **Mac** address, so the
   `jmp abs.l` operand stayed 0. Now written via `ROMBaseHost`, rebuilt per call.
2. **`d0` clobbered.** The guest sequence is
   `move.l #$00040001,-(sp); moveq #0,d0; A82A` — **`d0` is ComponentDispatch's
   selector**, not a result slot (compare `audio.cpp:975`: `moveq #$24,d0`
   immediately before its own `A82A`). The reject paths did `r->d[0] = -1; break;`,
   synthesising an error for a call that never ran and corrupting the selector.

**Way out (avoids the fallback entirely):** patch **only** the sclk site. Patch
one site, read the `ci` the handler reports, keep the site whose
`ci == AudioGetSoundClockCI()` and restore `0xA82A` at the other. If a single
site serves both CIs, restore the original word and re-execute it (`*pc -= 2`)
instead of running the trap ourselves.

---

## 6. Current implementation status

* `audio.cpp` tracks the first `siSoundClock` CI in `audio_sound_clock_ci` and exposes it via `AudioGetSoundClockCI()`.
* `SheepShaver/src/emul_op.cpp`:
  * `OP_COMPONENT_DISPATCH` handler is CI-aware.
  * Sound-clock CI returns host wall microseconds.
  * Other CIs fall back through the ROM thunk described above.
  * Patch scanner now scans both the discovered `spin` PC and the caller/`getTime` candidate (`maybe_gt`) over a 1 KiB window and patches every matching `move.l #0x00040001,-(sp); moveq #0,d0; A82A` site.
* `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` and `emul_op.h`: `EmulOp` signature changed to take `uint32 *pc` so the handler can redirect the 68k PC when needed.
* Compile-time switch `DESCENT_MOVIE_SOUND_CLOCK_WALL` in
  `SheepShaver/src/gfxaccel/include/qd3d_init_logging.h` — **defaults OFF**
  (§7): the code paths are correct up to the fallback, which wedges the guest.
* The scanner and both thunk fixes are kept in tree behind that switch; they cost
  nothing at default-0 and are strictly better than what preceded them.

---

## 7. Validation status (revised 2026-07-17 — supersedes earlier claims)

**`DESCENT_MOVIE_SOUND_CLOCK_WALL` is now default 0. The fix does not work yet.**

Earlier "validation" in this file was invalid: `AudioGetSoundClockCI()` was
defined inside `#if DESCENT_MOVIE_DIAGNOSTICS`, so the switch only ever compiled
with diagnostics **on** — the mode that stretches the intro ~1 s → ~20 s. Any
timing claimed before this is measuring instrumentation. (Fixed: the accessor is
production code and now lives outside that block.)

What is actually true, measured with diagnostics **off**:

* **Scanner: fixed and reliable.** It used to latch after one look and usually
  burned the attempt on `spin=0x00000014` / `gt=0x40814afe`, patching nothing all
  run. It now retries until a site is patched, skips non-68k-RAM candidates, and
  scans ±0x800. Typically succeeds on ~attempt 15 and patches 2 sites.
* **Handler: reached, and the CI split works.** `compDispEnter` fires with
  `hdr=0x00040001`, correctly distinguishing `ci40` (`0x0082000c`) from sclk
  (`0x00810012`).
* **Blocker: the non-sclk fallback wedges the guest.** The patch hits the `ci40`
  video-sync site too, so a fallback that runs the real `A82A` is mandatory — and
  every form of it dies at the first `ci40` dispatch. Patched runs stop at tick
  1700 (the patch tick); baseline reaches 2402 with `mixPulse peak=32768`.

Baseline stutter, instrumentation-free: **50 ticks / 0.83 s** (ticks 1674–1724).

See `descent-movie-session-2026-07-16.md` §26 for the full evidence.

---

## 8. Open questions / risks

* Is `info = 0x00000001` really `kSoundClockIsSyncNoLatency`? No source has confirmed the constant name; this is inference from usage.
* Does the Apple Mixer return the same `siSoundClock` CI for every source on the same mixer, or does Descent only use one? Logs show the same `0x00810012` across runs, so intercepting a single CI appears sufficient.
* Returning wall time for `ci40` at the patched site (because the patch covers both clock polls) may affect video-frame timing. So far audio/video appear healthy, but this needs visual confirmation.
* If other games rely on per-source paused clocks, the `DESCENT_MOVIE_SOUND_CLOCK_WALL` fix must remain opt-in / default-off in a production build. For now it is default-on for validation.

---

## 9. Related files

* `BasiliskII/src/include/audio_defs.h` — `'sclk'` definition
* `BasiliskII/src/audio.cpp` — `AudioGetSoundClockCI()`, `siSoundClock` tracking
* `BasiliskII/src/include/audio.h` — `AudioGetSoundClockCI()` declaration
* `SheepShaver/src/emul_op.cpp` — `OP_COMPONENT_DISPATCH`, inline A82A patch scan
* `SheepShaver/src/include/emul_op.h` — `OP_COMPONENT_DISPATCH`, `M68K_EMUL_OP_COMPONENT_DISPATCH`
* `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` — `execute_emul_op`, `Execute68kTrap`
* `SheepShaver/src/include/rom_patches.h` — `COMPONENT_DISPATCH_PATCH_SPACE`
* `SheepShaver/src/rom_patches.cpp` — `COMPONENT_DISPATCH_PATCH_SPACE` reservation
* `SheepShaver/src/gfxaccel/include/qd3d_init_logging.h` — `DESCENT_MOVIE_SOUND_CLOCK_WALL`
