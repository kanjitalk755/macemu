/*
 *  dsp_engine.h - DrawSprocket (DSp) engine public API
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  DSp is the fourth gfxaccel engine, peer to NQD/RAVE/GL. The
 *  engine-model decision rejected the display-owner / compositor-internal /
 *  video.cpp-stub alternatives.
 *
 *  This header is the public API surface for the DSp engine. The
 *  sub-opcode space covers:
 *    - Context lifecycle (Reserve, Release, GetBackBuffer, SwapBuffers,
 *      SetState, GetState, InvalBackBufferRect): sub-opcodes 100+.
 *    - Mode enumeration (GetFirstContext, FindBestContext, GetAttributes):
 *      sub-opcodes 200+.
 *    - Palette (SetCLUTEntries, GetCLUTEntries): sub-opcodes 300+.
 *    - Gamma + Fade (FadeGammaIn/Out, FadeGamma): sub-opcodes 400+.
 *    - VBL (SetVBLProc): sub-opcodes 500+.
 *    - The 33 real DrawSprocketLib PEF exports (AltBuffers, Blit,
 *      coords/mouse, queries/grid/framerate, save/restore/flatten,
 *      queue/switch, discovery, canonical DSpProcessEvent, SetBlankingColor,
 *      SetDebugMode): sub-opcodes 700+.
 *
 *  5 non-canonical sub-opcodes were dropped as proven ABSENT from the
 *  canonical DrawSprocketLib PEF export table (offline parse): SetGamma
 *  (400), GetGamma (401), GetVBLCount (501), BlankFill (502), and the
 *  non-canonical ProcessEvent dequeue handler (600). The canonical
 *  DSpProcessEvent is sub-opcode 750.
 */

#ifndef DSP_ENGINE_H
#define DSP_ENGINE_H

#include <stdint.h>
#include "dsp_version_policy.h"

/*
 *  DSp sub-opcode enum. 0..2 are lifecycle; 100..106 are context
 *  lifecycle; the remaining ranges (200+ / 300+ / 400+ / 500+ / 600+ /
 *  700+) follow the file-header comment.
 */
enum {
	kDSpStartup                     = 0,    /* DSpStartup() - idempotent */
	kDSpShutdown                    = 1,    /* DSpShutdown() - idempotent */
	kDSpGetVersion                  = 2,    /* Gestalt('dspv') handler */

	/* Context lifecycle. */
	kDSpContext_Reserve             = 100,
	kDSpContext_Release             = 101,
	kDSpContext_GetBackBuffer       = 102,
	kDSpContext_SwapBuffers         = 103,
	kDSpContext_SetState            = 104,
	kDSpContext_GetState            = 105,
	kDSpContext_InvalBackBufferRect = 106,

	/* Mode enumeration + best-match. */
	kDSpGetFirstContext             = 200,
	kDSpFindBestContext             = 201,
	kDSpContext_GetAttributes       = 202,

	/* Sub-opcode 203 — DSpGetNextContext. Advances the current enumeration
	 * cursor through the DSp mode cache and terminates with
	 * kDSpContextNotFoundErr at end-of-list per DSp 1.7 PDF p.17. */
	kDSpGetNextContext              = 203,

	/* Palette — SetCLUTEntries / GetCLUTEntries (300-399 reservation for CLUT). */
	kDSpContext_SetCLUTEntries      = 300,
	kDSpContext_GetCLUTEntries      = 301,

	/* Gamma + Fade — FadeGammaIn / FadeGammaOut / FadeGamma (400-499
	 * reservation for Gamma).
	 *
	 * The non-canonical kDSpContext_SetGamma (400) and kDSpContext_GetGamma
	 * (401) were dropped — DSpContext_SetGamma / DSpContext_GetGamma are
	 * proven ABSENT from the canonical DrawSprocketLib PEF export table
	 * (offline parse). 400/401 stay reserved / unused so the FadeGamma slots
	 * keep their historical numbering. */
	kDSpContext_FadeGammaIn         = 402,
	kDSpContext_FadeGammaOut        = 403,
	kDSpContext_FadeGamma           = 404,

	/* VBL Service — SetVBLProc / GetVBLProc (500-599 reservation for VBL).
	 *
	 * GetVBLProc (sub-opcode 503) is an internal round-trip helper — not
	 * called from guest Mac apps directly. DSp 1.7 spec p.81 documents
	 * SetVBLProc(ptr=0) as the uninstall path (no separate GetVBLProc in the
	 * spec); 503 is our internal affordance and is intentionally
	 * NOT installed in dsp_install_symbols[].
	 *
	 * The non-canonical kDSpContext_GetVBLCount (501) and
	 * kDSpContext_BlankFill (502) were dropped — DSpContext_GetVBLCount /
	 * DSpContext_BlankFill are proven ABSENT from the canonical
	 * DrawSprocketLib PEF export table (offline parse). 501/502 stay reserved
	 * / unused. */
	kDSpContext_SetVBLProc          = 500,
	kDSpContext_GetVBLProc          = 503,  /* round-trip helper */

	/* DSp Event Integration (600-699 reservation for events).
	 *
	 * The non-canonical kDSpContext_ProcessEvent (600 — the SPSC-ring DEQUEUE
	 * direction) was dropped. DSpContext_ProcessEvent is proven ABSENT from
	 * the canonical DrawSprocketLib PEF export table; the real, canonical
	 * export is DSpProcessEvent(EventRecord*, Boolean*) — the OPPOSITE
	 * direction (the app passes its event IN; DSp inspects for
	 * suspend/resume). The canonical kDSpProcessEvent lands at 750 below.
	 * 600 stays reserved / unused. */

	/* The 33 real DrawSprocketLib PEF exports. The SYMBOLS install so the
	 * CFM patch site resolves; the dispatch cases are routable. Numbering:
	 * 700-705 AltBuffers, 710-711 Blit, 720-723 coords/mouse, 730-739
	 * queries/grid/framerate, 739-747 save-restore-flatten + queue/switch +
	 * discovery, 750 canonical DSpProcessEvent, 760 SetBlankingColor, 761
	 * SetDebugMode. */

	/* 700-705 — AltBuffers (underlay/overlay). */
	kDSpAltBuffer_New                 = 700,
	kDSpAltBuffer_Dispose             = 701,
	kDSpAltBuffer_GetCGrafPtr         = 702,
	kDSpAltBuffer_InvalRect           = 703,
	kDSpContext_GetUnderlayAltBuffer  = 704,
	kDSpContext_SetUnderlayAltBuffer  = 705,

	/* 710-711 — Blit. */
	kDSpBlit_Faster                   = 710,
	kDSpBlit_Fastest                  = 711,

	/* 720-723 — coords / mouse. */
	kDSpGetMouse                      = 720,
	kDSpContext_GlobalToLocal         = 721,
	kDSpContext_LocalToGlobal         = 722,
	kDSpFindContextFromPoint          = 723,

	/* 730-739 — queries / dirty-rect grid / frame-rate. */
	kDSpContext_IsBusy                = 730,
	kDSpContext_GetDisplayID          = 731,
	kDSpContext_GetFrontBuffer        = 732,
	kDSpContext_GetMonitorFrequency   = 733,
	kDSpContext_GetMaxFrameRate       = 734,
	kDSpContext_SetMaxFrameRate       = 735,
	kDSpContext_GetDirtyRectGridSize  = 736,
	kDSpContext_SetDirtyRectGridSize  = 737,
	kDSpContext_GetDirtyRectGridUnits = 738,

	/* 739-747 — save/restore/flatten + queue/switch + discovery. */
	kDSpContext_Flatten               = 739,
	kDSpContext_GetFlattenedSize      = 740,
	kDSpContext_Restore               = 741,
	kDSpContext_Queue                 = 742,
	kDSpContext_Switch                = 743,
	kDSpFindBestContextOnDisplayID    = 744,
	kDSpGetCurrentContext             = 745,
	kDSpCanUserSelectContext          = 746,
	kDSpUserSelectContext             = 747,

	/* 750 — canonical DSpProcessEvent (replaces the dropped non-canonical
	 * 600 dequeue handler). */
	kDSpProcessEvent                  = 750,

	/* 760-761 — blanking color + debug mode. */
	kDSpSetBlankingColor              = 760,
	kDSpSetDebugMode                  = 761,

	kDSpMethodCount                 = 762
};

#define DSP_MAX_SUBOPCODE 762

/*
 *  AltBuffer dimension + backing-size caps.
 *
 *  Guest-supplied alt-buffer width/height (DSpAltBuffer_New attributes) are
 *  otherwise unbounded, so a crafted request overflows the uint32 row-bytes /
 *  buffer-size products (under-allocating for a record whose width/height are
 *  huge) and/or hands a multi-GiB allocation to the GPU heap (iOS low-memory
 *  termination — a hard constraint). These caps bound both failure modes.
 *
 *  DSP_ALT_MAX_DIM = 4032 is the LARGEST 32-bpp BGRA8 width whose 256-aligned
 *  rowBytes still fits the classic 15-bit QuickDraw PixMap rowBytes field:
 *      4032 * 4            = 16128 = 0x3F00  (already 256-aligned)
 *      (0x3F00 + 255) & ~255 = 0x3F00 <= 0x3FFF   (15-bit ceiling)
 *  4033 px would align to 0x4000 and overflow the 16-bit WriteMacInt16 in
 *  DSpAltBuffer_GetCGrafPtr. Capping width at DSP_ALT_MAX_DIM makes that
 *  truncation structurally impossible. Height shares the same cap for
 *  symmetry (it does not feed rowBytes, but bounds the buffer-size product).
 *
 *  DSP_ALT_MAX_BACKING_BYTES = 64 MiB is a sane single-alt-buffer ceiling
 *  (4032*4*4032 ~= 65 MiB worst case sits just above it, so the dim cap is the
 *  binding constraint for square surfaces; the byte cap guards skewed dims).
 */
#define DSP_ALT_MAX_DIM            4032u
#define DSP_ALT_MAX_BACKING_BYTES  (64u * 1024u * 1024u)

/*
 *  DSp 1.7 PDF p.87 enumerates ALL 11 Result Codes in -30440..-30450.
 *  There is NO "resolution-not-supported" constant in the spec.
 *  `kDSpContextNotFoundErr = -30446` is used for unsupported-mode returns
 *  from both DSpFindBestContext (PDF p.13 explicit) and DSpGetFirstContext
 *  (symmetric with FindBest). Do NOT add a new kDSp*NotSupported* constant —
 *  it would fail the behavior-exact fidelity gate against DrawSprocketLib.
 *
 *  DSp 1.7 Result Codes — authoritative range from
 *  resources/DrawSprocket1.7.pdf p.87. An earlier `kDSpErrGeneric = -24000`
 *  placeholder was a transcription error; apps probe return codes directly
 *  against DSp 1.7's documented header, so the range MUST be -30440..-30450
 *  for behavior-exact diff against fixtures.
 */
/*
 *  On macOS / Mac Catalyst builds, CarbonCore/MacErrors.h (reached by any
 *  Foundation import) already defines the -30440..-30450 DSp enumerators
 *  with these exact values, and C forbids redefining an enumerator. TUs
 *  that pull in Foundation/Metal do so before this header (system-first
 *  include order), so gate on MacErrors.h's include guard. kDSpNoErr and
 *  the kDSpErrGeneric alias are not in MacErrors.h and are always ours.
 */
#ifndef __MACERRORS__
enum {
	kDSpNotInitializedErr         = -30440,
	kDSpSystemSWTooOldErr         = -30441,
	kDSpInvalidContextErr         = -30442,
	kDSpInvalidAttributesErr      = -30443,
	kDSpContextAlreadyReservedErr = -30444,
	kDSpContextNotReservedErr     = -30445,
	kDSpContextNotFoundErr        = -30446,
	kDSpFrameRateNotReadyErr      = -30447,
	kDSpConfirmSwitchWarning      = -30448,
	kDSpInternalErr               = -30449,
	kDSpStereoContextErr          = -30450
};
#endif

enum {
	kDSpNoErr                     = 0,

	/*
	 *  Back-compat alias so existing call sites compile; the dispatch-default
	 *  path in dsp_dispatch.cpp uses kDSpInternalErr. New call sites SHOULD
	 *  use the specific kDSp*Err constant, not the alias.
	 */
	kDSpErrGeneric                = kDSpInternalErr
};

/*
 *  Context-loss reason codes (NOT error codes; positive values per DSp 1.7
 *  PDF p.~92 spec). The spec passes these as the "message" field of an osEvt
 *  EventRecord enqueued ahead of the suspend osEvt when a context-loss event
 *  fires.
 *
 *  Only kDSpContextReason_Lost is needed (the iOS-equivalent trigger is app
 *  entering background — there's no display-disconnect on iOS, so the bg
 *  event is the only context-loss vector). Other reason codes (display
 *  disconnect, user-toggled fullscreen-off) have no iOS equivalent and are
 *  deferred.
 *
 *  Value 1 per DSp 1.7 PDF p.~92 default. If a DrawSprocketLib decompile
 *  reveals a different value, change the constant.
 *
 *  Mirrored in dsp_event_record.h for cross-header use; the canonical
 *  definition lives there. This header re-declares the constant so DSp
 *  engine call sites that already #include dsp_engine.h don't need an
 *  extra #include of dsp_event_record.h just for the reason code.
 *  DSP_CONTEXT_REASON_LOST_DEFINED guards against double-definition when
 *  both headers are transitively included in the same translation unit.
 */
#ifndef DSP_CONTEXT_REASON_LOST_DEFINED
#define DSP_CONTEXT_REASON_LOST_DEFINED 1
enum {
	kDSpContextReason_Lost        = 1
};
#endif

/*
 *  DSp play states. Verified against DrawSprocket1.7.pdf pp.75, 86.
 *  Initial state on Reserve is Inactive (PDF p.29). The full state machine
 *  has 9 valid transitions + 3 invalid.
 */
enum DSpContextState {
	kDSpContextState_Active   = 0L,   /* owns display; DMC owner = kDMCOwnerDSp */
	kDSpContextState_Paused   = 1L,   /* menu bar visible; DMC owner = kDMCOwnerQuickDraw */
	kDSpContextState_Inactive = 2L    /* default play state; no blanking */
};

/*
 *  DSp depth masks (bit-per-supported-depth). PDF pp.66-67 documents the
 *  masks as OptionBits; context lifecycle uses only the 8/16/32 subset.
 *  CLUT support expands this to 1/2/4/8 indexed-depth variants.
 */
enum {
	kDSpDepthMask_1  = 1u <<  0,
	kDSpDepthMask_2  = 1u <<  1,
	kDSpDepthMask_4  = 1u <<  2,
	kDSpDepthMask_8  = 1u <<  3,
	kDSpDepthMask_16 = 1u <<  4,
	kDSpDepthMask_32 = 1u <<  5
};

/*
 *  DSp context options — bit-or at Reserve time via
 *  DSpContextAttributes.contextOptions. Values per DrawSprocket1.7.pdf
 *  p.74. SwapBuffers consults kDSpContextOption_DontSyncVBL to decide
 *  whether to block on the DSp frame-pacing lane before submitting the
 *  composite layer. PageFlip and QD3DAccel are accepted
 *  for attribute-struct round-trip fidelity; their behaviors are covered
 *  by other infrastructure (CAMetalLayer.maximumDrawableCount=3 already
 *  implements PageFlip semantics; QD3DAccel is "Not implemented" per PDF).
 */
enum {
	kDSpContextOption_QD3DAccel   = 1u << 0,  /* "Not implemented" per PDF; accept + no-op */
	kDSpContextOption_PageFlip    = 1u << 1,  /* CAMetalLayer.maximumDrawableCount=3 handles */
	kDSpContextOption_DontSyncVBL = 1u << 2   /* SwapBuffers skips DSp frame pacing */
};

/*
 *  DSpContextAttributes struct — corrected 2026-04-21 via debug session
 *  `dsp-sims-rejects-all-modes` (The Sims read displayWidth=0 for every
 *  advertised mode and rejected all of them under the original layout —
 *  that layout mis-read the PDF).
 *
 *  Field ORDER matches DrawSprocket 1.7 PDF pp.65-67 on-wire byte layout:
 *    Offset 0  (4, Fixed)         frequency
 *    Offset 4  (4, UInt32)        displayWidth
 *    Offset 8  (4, UInt32)        displayHeight
 *    Offset 12 (4, UInt32)        reserved1
 *    Offset 16 (4, UInt32)        reserved2
 *    Offset 20 (4, UInt32)        colorNeeds
 *    Offset 24 (4, CTabHandle)    colorTable
 *    Offset 28 (4, OptionBits)    contextOptions
 *    Offset 32 (4, OptionBits)    backBufferDepthMask
 *    Offset 36 (4, OptionBits)    displayDepthMask
 *    Offset 40 (4, UInt32)        backBufferBestDepth
 *    Offset 44 (4, UInt32)        displayBestDepth
 *    Offset 48 (4, UInt32)        pageCount
 *    Offset 52 (3, char[3])       filler[3]
 *    Offset 55 (1, Boolean)       gameMustConfirmSwitch
 *    Offset 56 (16, UInt32[4])    reserved3[0..3]
 *  Total on-wire size: 72 bytes.
 *
 *  In-struct widening rule: every on-wire field is stored as `uint32_t`
 *  for API stability — host-struct consumers read via `attr.fieldName`
 *  without caring about on-wire width. Byte-level PDF correctness is
 *  preserved only when the struct is populated from / read back to Mac
 *  memory via the ReadMacInt{8,16,32} / WriteMacInt{8,16,32} helpers in
 *  `DSpContext_ReserveHandler` etc., which use the PDF-exact offsets above.
 *
 *  Host-only fields (NOT on the wire; PDF has no backBufferWidth/Height):
 *    backBufferWidth / backBufferHeight — mirror displayWidth /
 *    displayHeight for the host-side dirty-rect clip code in
 *    dsp_draw_context.mm (DSpInvalBackBufferRect).
 *    Populated from displayWidth / displayHeight at Reserve_Core +
 *    mode-cache construction time. These fields live AFTER the PDF-
 *    reserved trailer so the on-wire prefix (bytes 0..71) stays PDF-
 *    exact when the struct is byte-serialized.
 *
 *  filler1 (offset 50, UInt16) from the pre-correction layout no longer
 *  exists — under the PDF-exact layout that storage belongs to filler[3]
 *  (+52..+54) + gameMustConfirmSwitch (+55) + reserved3 padding. Any code
 *  that wrote `.filler1 = 0` should be migrated to set the appropriate
 *  fields (gameMustConfirmSwitch = 0 + reserved3 = 0).
 */
typedef struct DSpContextAttributes {
	uint32_t  frequency;              /* Offset 0  (4 bytes, Fixed) — Input ignored per PDF p.66; output 0 if unavailable */
	uint32_t  displayWidth;           /* Offset 4  (4, UInt32) — display width in pixels */
	uint32_t  displayHeight;          /* Offset 8  (4, UInt32) — display height in pixels */
	uint32_t  reserved1;              /* Offset 12 (4) — Always 0 */
	uint32_t  reserved2;              /* Offset 16 (4) — Always 0 */
	uint32_t  colorNeeds;             /* Offset 20 (4) — kDSpColorNeeds_{DontCare=0,Request=1,Require=2} */
	uint32_t  colorTable;             /* Offset 24 (4, CTabHandle) — 0 for direct modes, output ignored per PDF p.66 */
	uint32_t  contextOptions;         /* Offset 28 (4, OptionBits) — bit-or kDSpContextOption_* */
	uint32_t  backBufferDepthMask;    /* Offset 32 (4, OptionBits) — kDSpDepthMask_* */
	uint32_t  displayDepthMask;       /* Offset 36 (4, OptionBits) — kDSpDepthMask_* */
	uint32_t  backBufferBestDepth;    /* Offset 40 (4, UInt32) — raw depth (1/2/4/8/16/32) */
	uint32_t  displayBestDepth;       /* Offset 44 (4, UInt32) — raw depth */
	uint32_t  pageCount;              /* Offset 48 (4, UInt32) — 1/2/3 video pages */
	uint32_t  gameMustConfirmSwitch;  /* Offset 55 (1, Boolean on-wire; widened in-struct) — output-only; PDF p.67 */
	uint32_t  reserved3[4];           /* Offset 56..71 (16, UInt32[4]) — PDF trailer reserved; Always 0 */
	/* --- host-only fields below — NOT on the wire, NOT read / written
	 *     via ReadMacInt / WriteMacInt. Populated from displayWidth /
	 *     displayHeight at Reserve_Core + mode-cache builder time. */
	uint32_t  backBufferWidth;        /* host-only mirror of displayWidth for dirty-rect clip */
	uint32_t  backBufferHeight;       /* host-only mirror of displayHeight for dirty-rect clip */
} DSpContextAttributes;

/*
 *  AltBuffer types (sub-ops 700-705). These three types are the AltBuffer
 *  ABI surface. The sub-opcode enum members (kDSpAltBuffer_New=700 ..
 *  kDSpAltBuffer_Dispose=701, etc.) and all result codes
 *  (kDSpInvalidAttributesErr=-30443 etc.) are declared above.
 *
 *  In-struct widening rule (mirrors DSpContextAttributes above): every
 *  on-wire field is stored as uint32_t so the .cpp/.mm/test code reads a
 *  consistent host-native width regardless of the Mac UInt32/OptionBits
 *  on-wire encoding.
 *
 *  DSpBufferKind (PDF p.50): the kind selector DSpAltBuffer_GetCGrafPtr
 *  takes. DSp 1.7 documents exactly ONE supported kind — kDSpBufferKind_
 *  Normal — and the handler returns kDSpInvalidAttributesErr for any other
 *  value.
 */
enum DSpBufferKind {
	kDSpBufferKind_Normal = 0   /* only supported kind (PDF p.50) */
};

/*
 *  DSpAltBufferOption (PDF p.68): option bits for DSpAltBufferAttributes.
 *  kDSpAltBufferOption_RowBytesEqualsWidth requests a tightly-packed
 *  backing (rowBytes == width * bytesPerPixel, no alignment padding).
 */
enum DSpAltBufferOption {
	kDSpAltBufferOption_RowBytesEqualsWidth = 1 << 0   /* PDF p.68 */
};

/*
 *  DSpAltBufferAttributes (PDF p.68): the attributes struct DSpAltBuffer_New
 *  takes (NULL => "same attributes as the specified context"; non-NULL =>
 *  overlay-kind, NOT usable as an underlay per PDF p.49). On-wire layout:
 *  width@+0, height@+4, options@+8, reserved[4]@+12 (28 bytes total).
 *  Stored as uint32_t in-struct per the widening rule.
 */
struct DSpAltBufferAttributes {
	uint32_t  width;        /* Offset 0  (4, UInt32) — alt-buffer width in pixels */
	uint32_t  height;       /* Offset 4  (4, UInt32) — alt-buffer height in pixels */
	uint32_t  options;      /* Offset 8  (4, OptionBits) — bit-or DSpAltBufferOption */
	uint32_t  reserved[4];  /* Offset 12..27 (16, UInt32[4]) — PDF trailer reserved; Always 0 */
};

/*
 *  Blit types (sub-ops 710-711).
 *
 *  DSpBlitMode (PDF p.87): the transfer-mode bit field carried in
 *  DSpBlitInfo.mode. The Interpolation bit selects filtered (bilinear) vs
 *  nearest-neighbor scaling for DSpBlit_Faster — ABSENT means
 *  nearest-neighbor. SrcKey / DstKey enable per-pixel color-key transparency
 *  (skip src pixels matching srcKey; skip overwriting dst pixels matching
 *  dstKey). These bit values match the DSp 1.7 spec verbatim and must not be
 *  renumbered.
 */
enum DSpBlitMode {
	kDSpBlitMode_Plain         = 0,        /* opaque srcCopy, no key, no filter */
	kDSpBlitMode_SrcKey        = 1 << 0,   /* Bit 0: source color-key transparency (PDF p.87) */
	kDSpBlitMode_DstKey        = 1 << 1,   /* Bit 1: dest color-key transparency (PDF p.87) */
	kDSpBlitMode_Interpolation = 1 << 2    /* Bit 2: FILTERED scaling; ABSENT = nearest-neighbor (PDF p.87) */
};

/*
 *  DSpBlitInfo on-wire field offsets (PDF p.68-69, big-endian guest layout,
 *  68 bytes total). The DSpBlit_* handlers read the guest blob field-by-field
 *  via ReadMacInt8/16/32 at these fixed offsets — there is intentionally NO
 *  host struct cast of the guest blob (the same precedent as DSP_CGP_OFF_* /
 *  DSP_PIXMAP_OFF_* for the CGrafPort: read the wire layout directly, never
 *  reinterpret-cast a host struct over big-endian guest memory).
 *
 *  Layout:
 *    completionFlag +0  (Boolean, 1 byte; set true on output when done)
 *    filler[3]      +1  (alignment padding)
 *    completionProc +4  (DSpBlitDoneProc, u32 guest PPC fn-ptr; 0 = no proc)
 *    srcContext     +8  (DSpContextReference, u32)
 *    srcBuffer      +12 (CGrafPtr, u32)
 *    srcRect        +16 (Rect, 8 bytes: top,left,bottom,right i16 each)
 *    srcKey         +24 (UInt32 source color key)
 *    dstContext     +28 (DSpContextReference, u32)
 *    dstBuffer      +32 (CGrafPtr, u32)
 *    dstRect        +36 (Rect, 8 bytes)
 *    dstKey         +44 (UInt32 dest color key)
 *    mode           +48 (DSpBlitMode, u32)
 *    reserved[4]    +52 (UInt32[4])
 */
#define DSP_BLITINFO_OFF_completionFlag   0   /* Boolean (1 byte) */
#define DSP_BLITINFO_OFF_completionProc   4   /* u32 guest PPC fn-ptr */
#define DSP_BLITINFO_OFF_srcContext       8   /* u32 */
#define DSP_BLITINFO_OFF_srcBuffer       12   /* u32 CGrafPtr */
#define DSP_BLITINFO_OFF_srcRect         16   /* Rect: top@+0 left@+2 bottom@+4 right@+6 */
#define DSP_BLITINFO_OFF_srcKey          24   /* u32 */
#define DSP_BLITINFO_OFF_dstContext      28   /* u32 */
#define DSP_BLITINFO_OFF_dstBuffer       32   /* u32 CGrafPtr */
#define DSP_BLITINFO_OFF_dstRect         36   /* Rect: top@+0 left@+2 bottom@+4 right@+6 */
#define DSP_BLITINFO_OFF_dstKey          44   /* u32 */
#define DSP_BLITINFO_OFF_mode            48   /* u32 DSpBlitMode */
#define DSP_BLITINFO_OFF_reserved        52   /* u32[4] */
#define DSP_BLITINFO_SIZE                68   /* total bytes (PDF p.69) */

/*
 *  Flatten/Restore self-consistent serialization format (sub-ops 739-741,
 *  PDF pp.21-23).
 *
 *  DSpContext_Flatten "converts a context into a format suitable for saving to
 *  disk" (PDF p.23); GetFlattenedSize "determines how much memory is required"
 *  (p.23); Restore reads it back. CRITICAL ordering (PDF p.22): Flatten is
 *  called BEFORE the context's play state goes Active — so the back-buffer
 *  Metal resources do NOT exist yet and MUST NOT be serialized.
 *  Restore "has a high probability of failure" (p.22), so returning
 *  kDSpContextNotFoundErr on a magic/version/no-display-match is a documented,
 *  valid outcome — NOT an error to mask.
 *
 *  On iOS the SAME emulator Flattens->disk->Restores its own blob (no second
 *  consumer of the byte layout exists), so a magic+version-headed
 *  attribute+bookkeeping format suffices; we do NOT match a fixed on-disk
 *  DSp 1.7 byte layout.
 *
 *  Serialized subset:
 *    +0   u32 magic   ('DSpF' / 0x44537046)
 *    +4   u32 version (DSP_FLAT_VERSION)
 *    +8   u32 size    (DSP_FLAT_SIZE — the total flattened byte count)
 *    +12  the 12 meaningful DSpContextAttributes UInt32 fields (the Reserve-
 *         relevant subset; frequency/reserved/filler are NOT round-tripped —
 *         Restore re-derives them via Reserve)
 *    +56  u32 max_frame_rate
 *    +60  u32 dirty_grid_w
 *    +64  u32 dirty_grid_h
 *  Total = DSP_FLAT_SIZE bytes. Runtime-only fields (back_buffer/back_texture/
 *  cgrafptr_mac_addr/state/staging_mac_addr/fade_state/events ring/alt-buffer
 *  handles) are NEVER serialized.
 */
#define DSP_FLAT_MAGIC                   0x44537046u  /* 'DSpF' big-endian */
#define DSP_FLAT_VERSION                 1u

/* On-wire offsets for the flattened format. The 12 attr fields mirror the
 * Reserve-relevant subset of the PDF-p.65 attr layout (frequency/reserved/
 * filler are NOT round-tripped — Restore re-derives them via Reserve). */
#define DSP_FLAT_OFF_magic                0   /* u32 */
#define DSP_FLAT_OFF_version              4   /* u32 */
#define DSP_FLAT_OFF_size                 8   /* u32 */
#define DSP_FLAT_OFF_displayWidth        12   /* u32 */
#define DSP_FLAT_OFF_displayHeight       16   /* u32 */
#define DSP_FLAT_OFF_colorNeeds          20   /* u32 */
#define DSP_FLAT_OFF_colorTable          24   /* u32 */
#define DSP_FLAT_OFF_contextOptions      28   /* u32 */
#define DSP_FLAT_OFF_backBufferDepthMask 32   /* u32 */
#define DSP_FLAT_OFF_displayDepthMask    36   /* u32 */
#define DSP_FLAT_OFF_backBufferBestDepth 40   /* u32 */
#define DSP_FLAT_OFF_displayBestDepth    44   /* u32 */
#define DSP_FLAT_OFF_pageCount           48   /* u32 */
#define DSP_FLAT_OFF_gameMustConfirmSwitch 52 /* u32 (widened from the 1-byte on-wire Boolean) */
#define DSP_FLAT_OFF_max_frame_rate      56   /* u32 bookkeeping */
#define DSP_FLAT_OFF_dirty_grid_w        60   /* u32 bookkeeping */
#define DSP_FLAT_OFF_dirty_grid_h        64   /* u32 bookkeeping */
#define DSP_FLAT_SIZE                    68   /* total flattened byte count */

/* Forward declare the opaque context-private struct; full definition lives
 * in dsp_draw_context.mm. */
struct DSpContextPrivate;

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  Public lifecycle entry points.
 *
 *  DSpInit: called from gfxaccel.cpp:VideoInstallAccel() after
 *    GLInstallHooks(). Idempotent - a subsequent call bumps the refcount and
 *    returns without re-registering.
 *  DSpIsRegistered: returns true once DSpInit has set its registered
 *    flag. Used by tests and by future DMC integration checks.
 */
extern void DSpInit(void);
extern bool DSpIsRegistered(void);

/*
 *  DSpInstallHooks - CFM symbol-table patcher.
 *
 *  Called from gfxaccel.cpp:VideoInstallAccel() inside the existing
 *  if (PrefsFindBool("dspaccel")) branch. Resolves DrawSprocketLib CFM
 *  exports via FindLibSymbol and overwrites the first 4 PPC instructions at
 *  each resolved orig_code to branch into dsp_method_tvects[subop].
 *  Idempotent + retry-guarded: up to 3 accRun ticks before giving up if
 *  DrawSprocketLib is not yet loaded.
 *
 *  See SheepShaver/src/gfxaccel/dsp_install_hooks.cpp for the byte-for-byte
 *  mirror of GLInstallHooks() in gl_engine.cpp.
 */
extern void DSpInstallHooks(void);

/*
 *  DSpInstallHooksSweepComplete - retry-driver gate.
 *
 *  Returns true once DSpInstallHooks() has either committed (FULL SUCCESS
 *  or FINAL PARTIAL COMMIT) OR exhausted DSP_HOOKS_MAX_ATTEMPTS without
 *  patching anything. Used by sony.cpp's accRun periodic-action gate
 *  (case 65) so the disk driver keeps invoking PatchAfterStartup() until
 *  the DSp install sweep has had all 3 chances to resolve late-bound CFM
 *  symbols. Without this gate the accRun action gets disabled as soon as
 *  RaveIsRegistered() returns true, which pinned DSpInstallHooks to a single
 *  attempt — masking the late-CFM-binding case.
 *
 *  Idempotent + thread-safe under the emul-thread single-writer model
 *  (sony.cpp's accRun is invoked on the emul thread via the same trap
 *  path that drives DSpInstallHooks itself, so no locking required).
 */
extern bool DSpInstallHooksSweepComplete(void);

/*
 *  Thunk-table init - called from thunks.cpp:ThunksInit() after
 *  GLThunksInit(). Allocates SheepMem TVECTs for the public DSp entry points
 *  (DSpStartup, DSpShutdown, DSpContext_Reserve, etc.) mirroring the
 *  RaveThunksInit() TVECT-allocation pattern.
 */
extern void DSpThunksInit(void);

/*
 *  Dispatch multiplexer - called from sheepshaver_glue.cpp for the new
 *  NATIVE_DSP_DISPATCH opcode. Signature mirrors RaveDispatch and
 *  GLDispatch: r3-r8 carry the function's raw register state. DSpDispatch
 *  reads the SUB-OPCODE from dsp_scratch_addr (written by the PPC TVECT
 *  thunk's `stw r12, 0(r11)`), NOT from r3 — r3 carries the guest's first
 *  real function argument (e.g., ctxRef). This contract was locked in by the
 *  sims-dsp-subop-mismatch regression. Returns the value to be written back
 *  to PPC gpr(3).
 */
extern uint32_t DSpDispatch(uint32_t r3, uint32_t r4, uint32_t r5,
                            uint32_t r6, uint32_t r7, uint32_t r8);

/*
 *  Lifecycle handlers:
 *   - DSpStartupHandler: idempotent init, bumps refcount, returns noErr
 *   - DSpShutdownHandler: decrements refcount, releases resources on 0
 *   - DSpGetVersionHandler: returns kDSpVersion_Current and writes it to
 *     outVersionAddr when r3 is a valid NumVersion return buffer
 */
extern int32_t DSpStartupHandler(void);
extern int32_t DSpShutdownHandler(void);
extern uint32_t DSpGetVersionHandler(uint32_t outVersionAddr);

#ifdef __cplusplus
}
#endif

/* Scratch word Mac address — used to pass sub-opcode from the PPC TVECT
 * thunk (via `stw r12, 0(r11)`) to native DSpDispatch (via
 * `ReadMacInt32(dsp_scratch_addr)`). Mirrors `rave_scratch_addr` in
 * rave_engine.h. Populated at boot by DSpThunksInit. The sims-dsp-subop-
 * mismatch fix promoted this from `static` file-scope to an exported global
 * so dsp_dispatch.cpp can read it. */
extern uint32_t dsp_scratch_addr;


/*
 *  Logging. Follows the RAVE/GL pattern - an ACCEL_LOGGING_ENABLED gate
 *  on top of an os_log-backed (Apple) or printf-backed (non-Apple)
 *  macro, with a runtime bool for per-subsystem on/off toggling.
 */
#include "accel_logging.h"

#if ACCEL_LOGGING_ENABLED
#ifdef __APPLE__
#include <os/log.h>
extern os_log_t dsp_log;
#endif
extern bool dsp_logging_enabled;

#ifdef __APPLE__
#define DSP_LOG(fmt, ...) do { \
    if (dsp_logging_enabled) os_log(dsp_log, fmt, ##__VA_ARGS__); \
} while (0)
#define DSP_VLOG(fmt, ...) do { \
    if (dsp_logging_enabled && ACCEL_LOG_VERBOSE) os_log(dsp_log, fmt, ##__VA_ARGS__); \
} while (0)
#else
#define DSP_LOG(fmt, ...) do { \
    if (dsp_logging_enabled) printf("DSp: " fmt "\n", ##__VA_ARGS__); \
} while (0)
#define DSP_VLOG(fmt, ...) do { \
    if (dsp_logging_enabled && ACCEL_LOG_VERBOSE) printf("DSp: " fmt "\n", ##__VA_ARGS__); \
} while (0)
#endif
#else
static constexpr bool dsp_logging_enabled = false;
#define DSP_LOG(fmt, ...) do {} while (0)
#define DSP_VLOG(fmt, ...) do {} while (0)
#endif

#endif /* DSP_ENGINE_H */
