/*
 *  dsp_clut_gamma.mm - DrawSprocket CLUT get/set handlers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Extracted from dsp_draw_context.mm (de-bloat): the CLUT Set/Get cores +
 *  handlers and the guest parametric-colour read helper.
 *  The gamma FADE subsystem stays in dsp_draw_context.mm. Entry points
 *  are extern "C" (declared in dsp_draw_context.h); the cores/helpers the
 *  dsp_draw_context.mm code reuses are declared in dsp_clut_gamma.h.
 */
#import <Metal/Metal.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "thunks.h"                /* SheepMem::Reserve */
#include "dsp_engine.h"
#include "dsp_draw_context.h"
#include "dsp_mode_enumerate.h"    /* DSpFindBestContextHandler delegate target */
#include "dsp_context_private.h"   /* DSpContextPrivate full struct; shared with dsp_metal_renderer.mm */
#include "dsp_guest_address.h"
#include "metal_compositor.h"      /* MetalCompositorUpdatePalette (Active CLUT push) */
#include "display_mode_controller.h" /* dmc_current_snapshot + dmc_record_*_change + kDMCNoErr */
#include "dsp_engine_internal.h"   /* DSpMapStateToDMCOwnerTyped (internal-only, NOT in include/) */
#include "dsp_clut_gamma.h"

/* ============================================================== */
/*  CLUT handlers (sub-opcodes 300/301)                            */
/*                                                                  */
/*  DSpContext_SetCLUTEntriesHandler: validation + clut_bytes write */
/*  + MetalCompositorUpdatePalette + dmc_record_palette_change.     */
/*  DSpContext_GetCLUTEntriesHandler: validation + read from        */
/*  ctx->clut_bytes_latched.                                        */
/* ============================================================== */

/*
 *  Core write path shared by the SetCLUTEntries handler and the Reserve
 *  colorTable application path (dsp_draw_context.mm).
 *  `entries_host_range` points to (last - first + 1) * 3 bytes
 *  of R/G/B data — NOT a 768-byte buffer; the caller's responsibility is
 *  to lay out the partial range starting at offset 0. This core helper
 *  only handles the post-validation write + compositor push + DMC bump.
 *
 *  Validation here re-checks ctx != null + bounds/ordering as a defensive
 *  second-layer; the callers have already checked the same invariants, so
 *  the re-check is cheap.
 */
int32_t DSpSetCLUTCore(DSpContextPrivate *ctx,
                       uint32_t first, uint32_t last,
                       const uint8_t *entries_host_range)
{
	if (ctx == nullptr) return kDSpInvalidContextErr;
	if (entries_host_range == nullptr) return kDSpInvalidAttributesErr;
	if (first > 255 || last > 255) return kDSpInvalidAttributesErr;
	if (first > last) return kDSpInvalidAttributesErr;

	/* Write into per-context storage at offset first*3. The
	 * reader-visible clut_bytes_latched is NOT touched here —
	 * DSpVBLClutLatchCallback drains clut_bytes -> clut_bytes_latched at
	 * the next VBL tick, giving GetCLUTEntries last-VBL-
	 * boundary semantics. */
	const uint32_t count = last - first + 1;
	memcpy(ctx->clut_bytes + first * 3, entries_host_range, count * 3);

	/* When Active, push the FULL 256-entry table so
	 * entries outside [first..last] set by earlier Set calls survive.
	 * MetalCompositorUpdatePalette writes into the back palette buffer;
	 * MetalCompositorPaletteLatch (VBL callback) promotes
	 * back -> front at the next frame boundary. */
	if (ctx->state == (uint32_t)kDSpContextState_Active) {
		MetalCompositorUpdatePalette(ctx->clut_bytes, 256);
		/* DMC bump: dmc_record_palette_change is the public DMC
		 * API — a function call, not a direct generation-counter
		 * assignment. The DMC write-site CI gate
		 * (testNoDirectDMCWritesInDSpFiles) scans for the bare-word
		 * DMC-owned identifier set followed by an assignment, which does
		 * NOT match this function-call syntax. */
		dmc_record_palette_change();
	}
	return kDSpNoErr;
}

/*
 *  DSpContext_SetCLUTEntries (sub-opcode 300).
 *
 *  Argument contract (DSp 1.7 p.56 — corrected wire-format;
 *  pointer-before-index arg order):
 *    ctxRef          = context handle from DSpContext_Reserve
 *    entriesAddr     = guest Mac address of inEntryCount x 8-byte ColorSpec
 *                      structs (ColorSpec = { SInt16 value; RGBColor rgb })
 *                      with 16-bit big-endian channels: value@+0 (ignored
 *                      for Set), r@+2, g@+4, b@+6 — read via ReadMacInt16
 *    inStartingEntry = first CLUT index to write; must be <= 255
 *    inEntryCount    = number of entries (a COUNT, NOT an inclusive last
 *                      index); must be > 0; inStartingEntry + inEntryCount
 *                      must be <= 256 (a full-CLUT replace is start=0,
 *                      count=256)
 *
 *  Wire-format boundary: the 16->8 down-convert (>> 8)
 *  happens HERE at the guest-RAM read loop ONLY. Internal clut_bytes
 *  storage + DSpSetCLUTCore + the VBL latch stay 3-byte/8-bit, so the
 *  composited pixels (which sample the latched 8-bit values) are
 *  byte-identical and DSpIndexedDepthCompositeTests is undisturbed. The
 *  compositor-side 16/32-bpp sampling is the CC-1 boundary.
 *
 *  Error map:
 *    kDSpInvalidContextErr    - ctxRef does not resolve to a context
 *    kDSpInvalidAttributesErr - entriesAddr == 0 OR inStartingEntry > 255
 *                                OR inEntryCount == 0 OR
 *                                inStartingEntry + inEntryCount > 256
 *    kDSpNoErr                - success; compositor push if
 *                                ctx is Active, deferred otherwise
 *
 *  Compositor contract:
 *    When ctx is Active, MetalCompositorUpdatePalette pushes the full
 *    256-entry ctx->clut_bytes into the compositor's back palette
 *    buffer; the compositor's VBL-callback MetalCompositorPaletteLatch
 *    promotes back -> front at
 *    the next frame boundary. The per-context clut_bytes_latched buffer
 *    is drained by DSpVBLClutLatchCallback (not touched
 *    here); GetCLUTEntries reads clut_bytes_latched.
 *
 *  DMC contract:
 *    dmc_record_palette_change() fires AFTER a successful compositor
 *    push (Active only) to bump persisted_palette_generation — ONLY
 *    through the public DMC API, never a direct generation-counter
 *    assignment.
 *
 *  Inactive/Paused persistence:
 *    When ctx->state != Active, the write lands only in ctx->clut_bytes.
 *    The next transition to Active inside DSpContext_SetStateHandler
 *    replays the stored clut_bytes via the same
 *    MetalCompositorUpdatePalette + dmc_record_palette_change pair.
 */
extern "C" int32_t DSpContext_SetCLUTEntriesHandler(uint32_t ctxRef,
                                                     uint32_t entriesAddr,
                                                     uint32_t inStartingEntry,
                                                     uint32_t inEntryCount)
{
	/* Validate ctxRef first so a bad handle + NULL entriesAddr reports
	 * the more specific kDSpInvalidContextErr. */
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == nullptr) {
		DSP_LOG("SetCLUTEntries: invalid ctxRef=%u -> kDSpInvalidContextErr",
		        ctxRef);
		return kDSpInvalidContextErr;
	}
	/* Validate entriesAddr next — a NULL buffer on a valid context is
	 * reported as kDSpInvalidAttributesErr so the app can distinguish
	 * it from a bad handle. */
	if (entriesAddr == 0) {
		DSP_LOG("SetCLUTEntries: NULL entriesAddr (ctxRef=%u start=%u count=%u) "
		        "-> kDSpInvalidAttributesErr",
		        ctxRef, inStartingEntry, inEntryCount);
		return kDSpInvalidAttributesErr;
	}
	/* Range-check BEFORE any guest-RAM read (ASVS V5):
	 * inEntryCount is a COUNT (not an inclusive last index). Reading
	 * past entry 255 would write beyond the 768-byte clut_bytes buffer.
	 * inStartingEntry + inEntryCount must be <= 256 (a full-CLUT replace
	 * is start=0, count=256). Both operands are guest-controlled uint32
	 * (r5/r6), so the headroom MUST be compared by subtraction rather than
	 * by summing — `inStartingEntry + inEntryCount` would wrap in 32-bit
	 * unsigned arithmetic (CWE-190) and defeat the guard (e.g. start=1,
	 * count=0xFFFFFFFF -> sum=0). The first clause guarantees
	 * inStartingEntry <= 255, so `256 - inStartingEntry` is in [1..256] and
	 * never underflows. */
	if (inStartingEntry > 255 || inEntryCount == 0 ||
	    inEntryCount > 256 - inStartingEntry) {
		DSP_LOG("SetCLUTEntries: out-of-range (ctxRef=%u start=%u count=%u) "
		        "-> kDSpInvalidAttributesErr",
		        ctxRef, inStartingEntry, inEntryCount);
		return kDSpInvalidAttributesErr;
	}

	const uint32_t start = inStartingEntry;
	const uint32_t count = inEntryCount;
	const uint32_t last  = start + count - 1;  /* inclusive, for the core */

	/* Read guest-RAM entries into a host-local 3-byte/8-bit staging
	 * range. The guest passes an array of 8-byte ColorSpec structs
	 * (DSp 1.7 p.56): value@+0 (SInt16, ignored for Set), r@+2, g@+4,
	 * b@+6 — 16-bit big-endian channels via ReadMacInt16. Down-convert
	 * 16->8 (>> 8) at this guest-RAM boundary ONLY so
	 * the internal clut_bytes storage + DSpSetCLUTCore stay 8-bit and
	 * the composited pixels (which sample the latched 8-bit values) are
	 * byte-identical. The staging buffer keeps DSpSetCLUTCore pure-host
	 * (no guest-RAM reads) so the Reserve colorTable path can reuse the
	 * same validation + write core. */
	uint8_t staged[256 * 3];  /* 768 bytes — compile-time sized for worst case */
	for (uint32_t i = 0; i < count; i++) {
		uint32_t e = entriesAddr + i * 8;  /* 8-byte ColorSpec stride */
		staged[i * 3 + 0] = (uint8_t)(ReadMacInt16(e + 2) >> 8);  /* r */
		staged[i * 3 + 1] = (uint8_t)(ReadMacInt16(e + 4) >> 8);  /* g */
		staged[i * 3 + 2] = (uint8_t)(ReadMacInt16(e + 6) >> 8);  /* b */
	}

	/* Defer to core helper for the validated write + compositor push +
	 * DMC bump. ctx/start/last/staged are already validated, so
	 * DSpSetCLUTCore's defensive re-checks are no-ops on this path. The
	 * core is UNCHANGED — it still takes an inclusive (first, last) range
	 * + a 3-byte host buffer. */
	int32_t rv = DSpSetCLUTCore(ctx, start, last, staged);
	if (rv == kDSpNoErr) {
		DSP_LOG("SetCLUTEntries: ctx=%u start=%u count=%u range=[%u..%u] "
		        "state=%u -> OK",
		        ctxRef, start, count, start, last, ctx->state);
	}
	return rv;
}


/*
 *  Core read path shared by the GetCLUTEntries handler and the fade
 *  handlers (dsp_draw_context.mm). Reads
 *  (last - first + 1) * 3 bytes from ctx->clut_bytes_latched into the
 *  caller's host buffer. Defensive validation re-checks ctx != null +
 *  bounds/ordering so every caller shares the same guarantees.
 *
 *  Reads from clut_bytes_latched (NOT clut_bytes) per the VBL-barrier
 *  contract: GetCLUTEntries returns the last-VBL-boundary CLUT state, not
 *  the in-flight SetCLUTEntries write. DSpVBLClutLatchCallback drains
 *  clut_bytes -> clut_bytes_latched on every VBL tick.
 *
 *  `entries_out_host_range` points to (last - first + 1) * 3 bytes of
 *  output storage starting at offset 0 (NOT offset first*3) — the caller
 *  lays out the partial range compactly. Symmetric with DSpSetCLUTCore's
 *  input-range contract.
 */
int32_t DSpGetCLUTCore(DSpContextPrivate *ctx,
                               uint32_t first, uint32_t last,
                               uint8_t *entries_out_host_range)
{
	if (ctx == nullptr) return kDSpInvalidContextErr;
	if (entries_out_host_range == nullptr) return kDSpInvalidAttributesErr;
	if (first > 255 || last > 255) return kDSpInvalidAttributesErr;
	if (first > last) return kDSpInvalidAttributesErr;

	/* Read from the LATCHED snapshot, not the writer-visible
	 * clut_bytes. The caller's output buffer layout is (last-first+1)*3
	 * bytes starting at offset 0. */
	const uint32_t count = last - first + 1;
	memcpy(entries_out_host_range,
	       ctx->clut_bytes_latched + first * 3,
	       count * 3);
	return kDSpNoErr;
}

/*
 *  DSpContext_GetCLUTEntries (sub-opcode 301).
 *
 *  Argument contract (DSp 1.7 p.57 — corrected wire-format;
 *  symmetric with SetCLUTEntries, pointer-before-index arg order):
 *    ctxRef          = context handle from DSpContext_Reserve
 *    entriesOutAddr  = guest Mac address to receive inEntryCount x 8-byte
 *                      ColorSpec structs (value@+0 = 0, 16-bit big-endian
 *                      r@+2/g@+4/b@+6 via WriteMacInt16)
 *    inStartingEntry = first CLUT index to read; must be <= 255
 *    inEntryCount    = number of entries (a COUNT, NOT an inclusive last
 *                      index); must be > 0; inStartingEntry + inEntryCount
 *                      must be <= 256
 *
 *  Wire-format boundary: the internal clut_bytes_latched
 *  storage stays 3-byte/8-bit; the 8->16 up-convert (high byte preserved,
 *  (value << 8) | value) happens HERE at the guest-RAM write loop ONLY.
 *
 *  Error map:
 *    kDSpInvalidContextErr    - ctxRef does not resolve to a context
 *    kDSpInvalidAttributesErr - entriesOutAddr == 0 OR inStartingEntry > 255
 *                                OR inEntryCount == 0 OR
 *                                inStartingEntry + inEntryCount > 256
 *    kDSpNoErr                - success; inEntryCount x 8-byte ColorSpec
 *                                written to guest RAM at entriesOutAddr
 *
 *  VBL-latched read contract:
 *    Reads from ctx->clut_bytes_latched — the snapshot maintained by
 *    DSpVBLClutLatchCallback (memcpy clut_bytes ->
 *    clut_bytes_latched on every VBL tick). A SetCLUTEntries +
 *    GetCLUTEntries pair within the same VBL window sees the PREVIOUS
 *    VBL's palette via Get — DSp 1.7's palette-animation contract.
 *
 *  Initial-state contract:
 *    ctx->clut_bytes_latched is zero-initialized by `new
 *    DSpContextPrivate()` (POD default-init). A
 *    GetCLUTEntries before any SetCLUTEntries returns all-zero bytes.
 *
 *  Non-mutating: Get does not call MetalCompositorUpdatePalette and does
 *  not call dmc_record_palette_change — DMC write-site CI gate stays
 *  zero-match because this handler touches only ctx->clut_bytes_latched
 *  (long-suffix name not in the forbidden-identifier regex).
 */
extern "C" int32_t DSpContext_GetCLUTEntriesHandler(uint32_t ctxRef,
                                                     uint32_t entriesOutAddr,
                                                     uint32_t inStartingEntry,
                                                     uint32_t inEntryCount)
{
	/* Validate ctxRef first so a bad handle + NULL entriesOutAddr reports
	 * the more specific kDSpInvalidContextErr. */
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == nullptr) {
		DSP_LOG("GetCLUTEntries: invalid ctxRef=%u -> kDSpInvalidContextErr",
		        ctxRef);
		return kDSpInvalidContextErr;
	}
	/* Validate entriesOutAddr — NULL output buffer on a valid context is
	 * reported as kDSpInvalidAttributesErr so the app can distinguish it
	 * from a bad handle. */
	if (entriesOutAddr == 0) {
		DSP_LOG("GetCLUTEntries: NULL entriesOutAddr (ctxRef=%u start=%u count=%u) "
		        "-> kDSpInvalidAttributesErr",
		        ctxRef, inStartingEntry, inEntryCount);
		return kDSpInvalidAttributesErr;
	}
	/* Range-check BEFORE the write loop (ASVS V5):
	 * inEntryCount is a COUNT (not an inclusive last index). Compare the
	 * headroom by subtraction, never by summing the two guest-controlled
	 * uint32 operands — `inStartingEntry + inEntryCount` would wrap in
	 * 32-bit unsigned arithmetic (CWE-190) and defeat the guard. The first
	 * clause bounds inStartingEntry <= 255, so 256 - inStartingEntry is in
	 * [1..256] and cannot underflow. */
	if (inStartingEntry > 255 || inEntryCount == 0 ||
	    inEntryCount > 256 - inStartingEntry) {
		DSP_LOG("GetCLUTEntries: out-of-range (ctxRef=%u start=%u count=%u) "
		        "-> kDSpInvalidAttributesErr",
		        ctxRef, inStartingEntry, inEntryCount);
		return kDSpInvalidAttributesErr;
	}

	const uint32_t start = inStartingEntry;
	const uint32_t count = inEntryCount;
	const uint32_t last  = start + count - 1;  /* inclusive, for the core */

	/* Read from the latched snapshot into a host-local 3-byte
	 * staging buffer. DSpGetCLUTCore re-validates as a defensive second
	 * layer (cheap; no-op on this path because we just validated above)
	 * and is UNCHANGED (still takes an inclusive (first, last) range +
	 * a 3-byte output buffer). */
	uint8_t staged[256 * 3];  /* 768 bytes — compile-time sized for worst case */
	int32_t rv = DSpGetCLUTCore(ctx, start, last, staged);
	if (rv != kDSpNoErr) {
		/* Shouldn't happen — we validated above — but defensive return. */
		return rv;
	}

	/* Egress: write the staged bytes into guest RAM as 8-byte ColorSpec
	 * structs (DSp 1.7 p.57): value@+0 = 0 (the SInt16 ColorSpec value
	 * field), 16-bit big-endian r@+2/g@+4/b@+6 via WriteMacInt16. The
	 * 8->16 up-convert preserves the high byte ((value << 8) | value) so
	 * a Set->Get round-trip returns the same ColorSpec at 8-bit precision
	 * (wire-format boundary). */
	for (uint32_t i = 0; i < count; i++) {
		uint32_t e = entriesOutAddr + i * 8;  /* 8-byte ColorSpec stride */
		uint16_t r = (uint16_t)((staged[i * 3 + 0] << 8) | staged[i * 3 + 0]);
		uint16_t g = (uint16_t)((staged[i * 3 + 1] << 8) | staged[i * 3 + 1]);
		uint16_t b = (uint16_t)((staged[i * 3 + 2] << 8) | staged[i * 3 + 2]);
		WriteMacInt16(e + 0, 0);  /* ColorSpec.value (SInt16) */
		WriteMacInt16(e + 2, r);
		WriteMacInt16(e + 4, g);
		WriteMacInt16(e + 6, b);
	}

	DSP_LOG("GetCLUTEntries: ctx=%u start=%u count=%u range=[%u..%u] "
	        "state=%u -> OK",
	        ctxRef, start, count, start, last, ctx->state);
	return kDSpNoErr;
}

/* ================================================================== */
/*                                                                    */
/*  Gamma-fade support helper.                                        */
/*  The fade handlers (sub-opcodes 402..404) live in                  */
/*  dsp_draw_context.mm; the guest parametric-colour read they        */
/*  share is defined here.                                            */
/*                                                                    */
/* ================================================================== */

/*
 *  FadeGamma ABI fidelity:
 *  read the classic Mac 16-bit RGBColor zero-intensity tint from guest
 *  Mac RAM at colorAddr.
 *
 *  RGBColor = { UInt16 red; UInt16 green; UInt16 blue; } = 6 bytes,
 *  big-endian (DSp 1.7 pp.32-35; standard Toolbox layout). The DSp 1.7
 *  FadeGamma trio passes a `RGBColor *inZeroIntensityColor`, NOT the
 *  iOS-simplified 3-byte/8-bit packed color the earlier prototype read.
 *  Channels are read via ReadMacInt16 at r@+0, g@+2, b@+4 (NOT
 *  ReadMacInt8 at +0/+1/+2) so guest addresses stay in the emulated
 *  address space and the VM layer bounds-checks them
 *  (arm64 >4GiB UB avoidance — never host-pointer casts).
 *
 *  The internal LUT machinery (DSpComputeFadeGammaTargetLUT) stays
 *  8-bit, so we down-convert each 16-bit channel to 8-bit by taking the
 *  high byte (>> 8) — the classic Mac convention where the 8-bit value
 *  lives in the high byte of each 16-bit channel. This is the SINGLE
 *  replacement point flagged by the original in-code comment.
 *
 *  Defensive: caller has already validated colorAddr != 0; this helper
 *  trusts that invariant. (NULL colorAddr -> black is handled by the
 *  caller BEFORE this helper is reached.)
 */
void DSpReadParametricColorFromGuest(uint32_t colorAddr,
                                              uint8_t *out_r,
                                              uint8_t *out_g,
                                              uint8_t *out_b)
{
	*out_r = (uint8_t)(ReadMacInt16(colorAddr + 0) >> 8);
	*out_g = (uint8_t)(ReadMacInt16(colorAddr + 2) >> 8);
	*out_b = (uint8_t)(ReadMacInt16(colorAddr + 4) >> 8);
}


