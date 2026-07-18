# Descent II PPC disassembly notes

Live disassembly of the `Descent II` PEF data fork to understand the first-movie stutter.

## PEF container summary

- File: `C:\Users\User\Documents\Shared\macemureal\Descent II`
- Format: `Joy!peffpwpc` (PowerPC PEF)
- Code section: 981,556 bytes at file offset `0x1800`, loaded at image base `0x10000000`
- Data section: 78,728 bytes unpacked at file offset `0xf1240`
- Loader section: 6,004 bytes at file offset `0x80`
- Imports: exactly **5 libraries**, 311 symbols total

| Library | Symbols | First index |
|---------|--------:|------------:|
| InputSprocketLib | 11 | 0 |
| QuickDraw(tm) 3D Accelerator | 8 | 11 |
| QuickTimeLib | 18 | 19 |
| MathLib | 5 | 37 |
| InterfaceLib | 269 | 42 |

There is **no MVE/movie library** — the player is statically linked into the PEF code section.

## Key InterfaceLib imports

| Index | Name | Notes |
|------:|------|-------|
| 76 | `SndDisposeChannel` | |
| 102 | `SndNewChannel` | `synth=5` (`sampledSynth`) in `FUN_100e1560` |
| 114 | `SndDoImmediate` | |
| 150 | `Delay` | |
| 153 | `TickCount` | |
| 160 | `InsXTime` | |
| 166 | `PrimeTime` | |
| 194 | `NewRoutineDescriptor` | Used to wrap `dbhDoubleBack` |
| 254 | `Gestalt` | Used by `FUN_100dec18`/`FUN_100deba4` to probe sound hardware |
| 282 | `RmvTime` | |
| 291 | `SndPlayDoubleBuffer` | **Only call site is `FUN_100e1560`** (line `100e1770`) |
| 308 | `Microseconds` | Likely the movie wait loop's wall clock source |

## `dbhDoubleBack` callback — `FUN_100e137c`

- Address: `0x100e137c`
- Referenced from TOC slot `0x100F2610` via `NewRoutineDescriptor(proc, 0x3c0, 1)` in `FUN_100e1560`.
- Arguments (PPC, MixedMode): `r3` = sound channel, `r4` = `SndDoubleBuffer *`.
- Behavior:
  1. If `r3 == 0` or `r4 == 0`, return immediately.
  2. Read global `-0x4ba8(r2)` (playing flag). If zero, go to **stop path**.
  3. Read global `-0x4ba4(r2)` (buffer-active flag). If non-zero, go to **stop path**.
  4. Increment a byte counter at `state + 0xa` (reentrancy guard).
  5. Update a 16-bit counter at `state + 0x116a4` and a 32-bit counter at `state + 0x116a0`.
     - The 16-bit value is incremented, then `0x4fd4` (20,436) is subtracted.
     - If the result exceeds `0xffff`, the 32-bit counter is incremented and the low 16 bits are stored.
     - `20,436 / 22,050 Hz ≈ 0.927 s`, which matches the observed ~0.9 s video lead time before the phase-2 audio switch.
  6. Call `FUN_100e569c(r31 + 0x10)` — sets up audio conversion parameters for the buffer.
  7. Call `FUN_100df688()` — currently a no-op (`blr`).
  8. Set `buffer->dbFlags |= 1` (`dbBufferReady`).
  9. Decrement the byte counter at `state + 0xa`.
  10. Return.
- **Stop path:** `buffer->dbFlags |= 4` (`dbLastBuffer`) and clear `-0x4ba8`.

So the callback is normal `SndPlayDoubleBuffer` bookkeeping. The freeze is not here.

## `FUN_100e569c` — audio conversion / mixer setup

- Called only from `dbhDoubleBack` (`FUN_100e137c`) at `0x100e1400`.
- Takes `r3` = pointer to buffer payload (`SndDoubleBuffer` data at offset `0x10`).
- Reads `state[0]` (a format/playback-state selector with values 0, 1, 2, 3, or other) and `state[9]` (stereo flag).
- Based on those, writes converter function pointers into the sound state:
  - `state + 0xd20`, `0xd1c`, `0xd28`, `0xd24`
  - `state + 0xd2c`, `0xd34`, `0xd30`, `0xd38`
- The constants are loaded from TOC slots (`-0x667c`, `-0x6678`, `-0x666c`, `-0x6668`, `-0x66a0`, `-0x66a4`, etc.) and are almost certainly function pointers to sample-format converters (8-bit mono, 8-bit stereo, 16-bit mono, 16-bit stereo, etc.).
- If `state[0xb]` is zero:
  - Zeros two mixer channel arrays (`state + 0xfa3c`? and `state + 0x22fc`? — exact offsets need verification).
  - Iterates over active channels (`state + 0xc12` + `state + 0xc16` channels, step `0x710`).
  - Calls `FUN_100ecd5c`.
- Finally dispatches to one of four mixer/render functions based on `state[8]` (8-bit vs 16-bit) and `state[9]` (mono vs stereo):
  - `FUN_100e6bd4`
  - `FUN_100e6d74`
  - `FUN_100e6e74`
  - `FUN_100e7318`
- This is the digital mixer's "fill one double-buffer" routine.

## `FUN_100df688` — empty callback helper

- Address: `0x100df688`
- Body is a single `blr`.
- Probably a placeholder or a debug hook that the retail build does not fill.

## `FUN_100e1560` — `SndPlayDoubleBuffer` setup

- Already in `dissassem.asm`.
- Builds a standard `SndDoubleBufferHeader` at `state + 0x116ac`.
- Calls `SndNewChannel(&chan, 5, initFlags, 0)` then `SndPlayDoubleBuffer(chan, hdr)`.
- Called from three places in the digital mixer init/restart path.

## Sound engine call graph

The only `SndPlayDoubleBuffer` call is inside `FUN_100e1560`. Its three direct callers are digital-mixer lifecycle functions:

| Function | Called from | Role |
|----------|-------------|------|
| `FUN_100e1560` | `FUN_100e7674`, `FUN_100e7834`, `FUN_100e7d60` | Build + play `SndDoubleBufferHeader` |
| `FUN_100e7674` | `0x100df28c` | Restart after stop |
| `FUN_100e7834` | `0x100df4bc` | Init (allocates 0x11728 state) |
| `FUN_100e7d60` | `0x100df464` | Reconfigure rate/format |

`0x100df28c`, `0x100df4bc`, and `0x100df464` appear to be dispatch tables for the sound engine — they handle many selectors/cases and call into the digital mixer. They are **not** the MVE movie player itself.

## Movie playback function — `FUN_100782e0` (FULLY DISASSEMBLED — dissassem.asm)

- Address: `0x100782e0`
- Called from `FUN_10078d74` (at `0x10078fb0`) with the string at `0x100f9f31` (`"intro.mve"`).
- Also called from `FUN_10035d58`, `FUN_100545ec`, `FUN_10055338`, and `FUN_10073430`.
- **CONFIRMED: pure QuickTime; NOT a frame decoder, and NOT related to the
  SndPlayDoubleBuffer game-sound engine (FUN_100e1560).** They are separate
  subsystems. The movie's audio is QuickTime's own sound media handler, reached
  only through `MoviesTask`.
- Full call sequence (dissassem.asm): `GetGWorld` → build `intro.mve` path
  (strcpy/strchr/strcat) → `FUN_100780fc` (open, 4 dir specs) → `SetDepth(16)` →
  `GetMovieBox`/`OffsetRect`/`SizeWindow` → `SetGWorld`/`SetMovieGWorld`/
  `SetMovieBox` → `SetMovieTimeValue(0)` → `ShowWindow` → 2x `MoviesTask` →
  `StartMovie` → **[loop] MoviesTask/IsMovieDone** → StopMovie/HideWindow/
  DisposeMovie.
- The playback loop uses standard QuickTime movie APIs:
  - `GetGWorld`, `SetGWorld`
  - `GetMovieBox`, `SetMovieBox`, `OffsetRect`
  - `SetMovieTimeValue`, `SetMovieGWorld`
  - `ShowWindow`, `HideWindow`, `SizeWindow`
  - `MoviesTask`, `StartMovie`, `StopMovie`, `IsMovieDone`, `DisposeMovie`
  - `GetCursor`, `SetCursor`
- Playback loop (`0x100785b4`..`0x10078664`) is a busy loop that:
  1. Calls `FUN_10078d70` each iteration (which is an empty `blr` placeholder).
  2. Calls `GetOSEvent` to detect a key press (checks for `'5'` / `0x35` to skip).
  3. Calls `MoviesTask` to pump QuickTime.
  4. Calls `IsMovieDone` and loops until the movie is done.
  5. Has a special `0x71` pause/resume branch (`0x1007860c`..`0x1007863c`) that stops the movie, waits for a key press, then restarts it.

### RESOLVED: `intro.mve` is a plain QuickTime movie (option 1)

Confirmed by dumping `descentdata/DataCD/intro.mvl` directly:

- **MVL = a thin archive.** Header `DMVL` + count(2) + directory entries
  (13-byte name field, then offset+length dwords) naming `intro.mve` and
  `end.mve`. `$DMVL` is Descent's **movie-library archive format**, NOT a
  QuickTime component. The `.mvl` maps logical names to `.mve` blobs.
- **The `.mve` inside is a standard QuickTime `.mov`.** `.mve` is only the file
  extension. Layout `[mdat 55.5 MB][moov 24 KB]` with a full atom tree:
  `moov → mvhd + trak → tkhd/edts(elst)/mdia → mdhd/hdlr/minf →
  vmhd|smhd/dinf(dref)/stbl → stsd/stts/stss/stsc/stsz/stco`.
- **Video track:** codec `cvid` = **Cinepak**, 592×312, 24-bit (stock Apple QT
  codec).
- **Audio track:** codec `twos` = **big-endian 16-bit PCM, 2 channels** (matches
  the "16-bit twos 2ch" movie audio format seen throughout the emulator logs —
  §21/§23 — confirming those logs are this QuickTime track, not the game mixer).

**No custom QuickTime importer/component exists.** Stock QuickTime opens the
`.mve`, decodes Cinepak, and plays the `twos` PCM through the Sound Manager. The
earlier "$DMVL importer component" hypothesis is **wrong** — disregard it.

### Implications for the stutter (CONFIRMED — final, §33-§36)

The `~50 tick` freeze is **not** inside `FUN_100782e0` — it is a thin QuickTime
shell that just spins on `MoviesTask`/`IsMovieDone`. The freeze is **inside a
single `MoviesTask` call**, doing one-time QuickTime **video** work (lazy Cinepak
decompressor instantiation on the first keyframe) on the interpreter.

- The `A82A ComponentDispatch GetTime` / `siSoundClock` polling during the freeze
  is QuickTime SPINNING while it waits — NOT the cause. Proven: a +0.9 s offset
  on that clock left the freeze unchanged (§36).
- The `siSoundClock`/Apple Mixer/`PlaySourceBuffer` path is QuickTime's audio
  output, NOT Descent's `SndPlayDoubleBuffer` — but it is not the freeze gate.
- Call-gap analysis: ~0.5 s inside one MoviesTask with NO sound/mixer calls =
  pure video. `FUN_100782e0` skips `LoadMovieIntoRam`/`PrerollMovie` (unlike the
  game's other movie path `FUN_100787e4`), so the decompressor instantiates
  mid-playback. Caches after -> "first intro only".

**Fix target:** native Cinepak (`cvid`) decompressor — hook QuickTime's image
decompress and decode in host C (RAVE/DSp/GL native-dispatch pattern). NOT the
sound clock, NOT Microseconds, NOT CD reads — all disproven by direct test. See
descent-movie-session §33-§36.

### RESOLVED: the freeze is one-time QuickTime video setup in MoviesTask

**Correcting an earlier wrong conclusion:** §32 claimed the freeze was the
Microseconds-trap crossing cost (QuickTime's `'clok'` GetTime polling ~20k/s).
That was DISPROVEN — see descent-movie §33-§36. A +0.9 s offset applied directly
to that clock left the freeze unchanged, so the video hold is not gated on the
sound clock at all; the 20k/s polling is QuickTime spinning WHILE it waits.

**Actual cause:** one-time QuickTime video setup inside a single `MoviesTask`
call — almost certainly lazy **Cinepak (`cvid`) decompressor instantiation** on
the first keyframe — on the interpreter. The intro player `FUN_100782e0` does
not `LoadMovieIntoRam`/`PrerollMovie`, so it happens mid-playback. Call-gap
analysis: a ~0.5 s window inside one MoviesTask with no sound/mixer calls (pure
video). Caches after first play -> "first intro only, later movies fine".

**Fix:** native Cinepak decompressor (hook `cvid` image decompress, decode in
host C; the fork's RAVE/DSp/GL native-dispatch pattern). Not the clock, not
Microseconds, not CD reads, not the mixer setup calls — all eliminated by direct
test (descent-movie §32-§36).

(The `$DMVL` custom-importer question is closed: no custom component; the MVE is
stock QuickTime — Cinepak video + `twos` PCM audio.)

## Next targets

- **Find the custom QuickTime movie importer component.** Best anchors:
  - Search for `RegisterComponent`/`RegisterComponentResource` in the import list.
  - Search for the `$DMVL` four-char-code used as a component manufacturer or subtype.
  - Look for a large function with a switch table on a selector parameter (typical component dispatcher).
  - Look for references to `intro.mvl` / MVL file loading.
- **Find the MVE file opener.** `FUN_100780fc` is a candidate: it iterates over 4 directory specs and calls `0x10077fd8` (likely `OpenMovieFile` or similar), then `0x100ef0b0` and `0x100ef0c8` (QuickTime file APIs). Dumping it will confirm whether the HOG archive is involved.
- **Find the `$DMVL` component's GetTime/A82A path.** The ComponentDispatch call the emulator patches is somewhere in the importer's clock/sync logic, not in the movie playback shell.

---

*Generated from live Capstone disassembly of `Descent II`.*
