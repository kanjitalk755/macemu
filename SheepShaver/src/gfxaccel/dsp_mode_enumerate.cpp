/*
 *  dsp_mode_enumerate.cpp - VModes[] -> DSpContextAttributes transform +
 *                            DSpGetFirstContextHandler.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Pattern source:
 *    - BasiliskII/src/SDL/video_sdl2.cpp (DMCModeDescFromVModesIndex
 *      pure-function transform shape: switch on viAppleMode, field-copy,
 *      no Metal dependencies)
 *    - SheepShaver/src/gfxaccel/dsp_draw_context.mm (Reserve_Core
 *      validate-populate pattern; metadata-only DSp context allocation)
 *
 *  Threading contract:
 *    The s_dsp_modes cache is single-writer emul-thread. Built once at
 *    DSpInit, cleared at DSpShutdown. NO mutex / NO _Atomic — DSp code
 *    does not share the cache with main-thread observer hooks.
 */

#include "sysdeps.h"
#include "dsp_mode_enumerate.h"
#include "dsp_engine.h"
#include "dsp_draw_context.h"   /* Metadata-only DSp context allocation */
#include "dsp_user_select_policy.h"
#include "cpu_emulation.h"       /* WriteMacInt32 */
#include "video.h"               /* VideoInfo, VModes[], APPLE_*_BIT, DIS_INVALID */

#include <vector>
#include <algorithm>
#include <cstring>

namespace {

/*
 *  Convert an APPLE_*_BIT viAppleMode to its raw bit-depth count.
 *  Same mapping as DMCModeDescFromVModesIndex (video_sdl2.cpp:1505-1521).
 *  Returns 0 for unknown inputs so callers can filter.
 */
uint32_t AppleModeToBitDepth(uint32_t apple)
{
	switch (apple) {
		case APPLE_1_BIT:  return 1;
		case APPLE_2_BIT:  return 2;
		case APPLE_4_BIT:  return 4;
		case APPLE_8_BIT:  return 8;
		case APPLE_16_BIT: return 16;
		case APPLE_32_BIT: return 32;
		default:            return 0;
	}
}

/*
 *  Map a raw bit-depth count to its DSp kDSpDepthMask_* bit. Returns 0
 *  for unknown depths so callers can filter.
 */
uint32_t DepthMaskForDepth(uint32_t d)
{
	switch (d) {
		case 1:  return kDSpDepthMask_1;
		case 2:  return kDSpDepthMask_2;
		case 4:  return kDSpDepthMask_4;
		case 8:  return kDSpDepthMask_8;
		case 16: return kDSpDepthMask_16;
		case 32: return kDSpDepthMask_32;
		default: return 0;
	}
}

/* File-scope cache — single-writer emul-thread. Built at DSpInit, cleared
 * at DSpShutdown. */
std::vector<DSpContextAttributes> s_dsp_modes;

/* Bound the VModes[] scan so a malformed / missing DIS_INVALID terminator
 * cannot spin forever. VideoInfo VModes[64] is the canonical upper bound
 * (video.cpp:67); 128 gives slack for future growth. */
const int kMaxVModesScan = 128;

bool DSpPublicModeAttributesEqual(const DSpContextAttributes &a,
                                  const DSpContextAttributes &b)
{
	return a.frequency == b.frequency &&
	       a.displayWidth == b.displayWidth &&
	       a.displayHeight == b.displayHeight &&
	       a.colorNeeds == b.colorNeeds &&
	       a.colorTable == b.colorTable &&
	       a.contextOptions == b.contextOptions &&
	       a.backBufferDepthMask == b.backBufferDepthMask &&
	       a.displayDepthMask == b.displayDepthMask &&
	       a.backBufferBestDepth == b.backBufferBestDepth &&
	       a.displayBestDepth == b.displayBestDepth &&
	       a.pageCount == b.pageCount &&
	       a.gameMustConfirmSwitch == b.gameMustConfirmSwitch;
}

}  // anonymous namespace

/*
 *  DSpBuildModesFromVModes - rebuild s_dsp_modes from VModes[].
 *
 *  Walks VModes until DIS_INVALID terminator or scan cap, filtering out:
 *    - Unknown bpp modes.
 *    - Modes with zero width or height (defensive).
 *
 *  Populates a DSpContextAttributes per mode:
 *    frequency=0 (PDF p.66 permits 0), w/h from VModes,
 *    backBufferBestDepth=displayBestDepth=raw_depth,
 *    backBufferDepthMask=displayDepthMask=DepthMaskForDepth(raw_depth),
 *    pageCount=2 (default),
 *    colorNeeds=(depth==8)?1:2 (Request for indexed, Require for direct),
 *    contextOptions=kDSpContextOption_PageFlip,
 *    rest zero-init.
 *
 *  After population: stable_sort by (depth asc, width asc, height asc),
 *  then coalesce adjacent public-equivalent modes. This keeps separate
 *  VModes records such as window/fullscreen 640x480 from surfacing as
 *  duplicate DSp contexts when their API-visible attributes are identical.
 */
void DSpBuildModesFromVModes(void)
{
	s_dsp_modes.clear();

	int scanned = 0;
	int filtered_depth = 0;
	int filtered_zero = 0;

	for (int i = 0; i < kMaxVModesScan; i++) {
		const VideoInfo &v = VModes[i];
		if (v.viType == DIS_INVALID) break;

		scanned++;

		uint32_t depth = AppleModeToBitDepth(v.viAppleMode);
		/* Reserve_Core now accepts 1/2/4 bpp. Filter retained to drop unknown
		 * viAppleMode values (AppleModeToBitDepth returns 0 for non-APPLE_*_BIT
		 * inputs). */
		if (depth != 1 && depth != 2 && depth != 4 && depth != 8 &&
		    depth != 16 && depth != 32) {
			filtered_depth++;
			continue;
		}
		if (v.viXsize == 0 || v.viYsize == 0) {
			filtered_zero++;
			continue;
		}

		DSpContextAttributes a;
		std::memset(&a, 0, sizeof(a));
		/* PDF p.66: frequency=0 is legal output when actual refresh is
		 * not available; avoids ProMotion 60/120 Hz quantization issues. */
		a.frequency            = 0;
		a.displayWidth         = (uint32_t)v.viXsize;
		a.displayHeight        = (uint32_t)v.viYsize;
		a.backBufferWidth      = (uint32_t)v.viXsize;
		a.backBufferHeight     = (uint32_t)v.viYsize;
		a.backBufferBestDepth  = depth;
		a.displayBestDepth     = depth;
		a.backBufferDepthMask  = DepthMaskForDepth(depth);
		a.displayDepthMask     = DepthMaskForDepth(depth);
		a.pageCount            = 2;
		a.colorNeeds           = (depth == 8) ? 1u /* Request */
		                                       : 2u /* Require */;
		a.contextOptions       = kDSpContextOption_PageFlip;
		/* colorTable, gameMustConfirmSwitch, reserved3[4]: zero-init
		 * already applied by memset. */
		s_dsp_modes.push_back(a);
	}

	/* Sort for stable enumeration order. DSp 1.7 spec is silent on order. */
	std::stable_sort(s_dsp_modes.begin(), s_dsp_modes.end(),
		[](const DSpContextAttributes &a, const DSpContextAttributes &b) {
			if (a.backBufferBestDepth != b.backBufferBestDepth)
				return a.backBufferBestDepth < b.backBufferBestDepth;
			if (a.displayWidth != b.displayWidth)
				return a.displayWidth < b.displayWidth;
			return a.displayHeight < b.displayHeight;
		});

	const size_t before_coalesce = s_dsp_modes.size();
	s_dsp_modes.erase(
	    std::unique(s_dsp_modes.begin(), s_dsp_modes.end(),
	                DSpPublicModeAttributesEqual),
	    s_dsp_modes.end());

	DSP_LOG("BuildModesFromVModes: %zu modes cached (scanned=%d, "
	        "filtered_depth=%d, filtered_zero=%d, coalesced=%zu)",
	        s_dsp_modes.size(), scanned, filtered_depth, filtered_zero,
	        before_coalesce - s_dsp_modes.size());
}

void DSpClearModes(void)
{
	s_dsp_modes.clear();
}

size_t DSpUserSelectableModeCount(const DSpContextAttributes *req)
{
	if (req == nullptr) return 0;
	return DSpCountUserSelectableContexts(s_dsp_modes.data(),
	                                      s_dsp_modes.size(), *req);
}

/*
 *  Core DSpGetFirstContext logic — factored out of the Mac-memory
 *  handler (DSpGetFirstContextHandler) so the allocation/copy logic is
 *  exercisable in isolation. Factoring matches the Reserve_Core split
 *  pattern.
 *
 *  Semantics:
 *    - s_dsp_modes empty => return kDSpContextNotFoundErr.
 *    - Allocate a metadata-only context handle via
	 *      DSpAllocMetadataContextHandle; copy s_dsp_modes[0] into its attr.
 *    - On allocation failure (table full) return kDSpInternalErr.
 *    - On success write the 1-based handle to *outHandle and return
 *      kDSpNoErr.
 */
static int32_t DSpGetFirstContext_Core(uint32_t *outHandle)
{
	if (outHandle == nullptr) return kDSpInvalidAttributesErr;

	if (s_dsp_modes.empty()) {
		DSP_LOG("GetFirstContext: mode cache empty — kDSpContextNotFoundErr");
		return kDSpContextNotFoundErr;
	}

	/* Debug session `dsp-sims-enumeration-stall` fix (2026-04-19): pass
	 * enumeration_mode_index=0 so the subsequent DSpGetNextContext can
	 * advance to s_dsp_modes[1], [2], ... and expose every available mode
	 * per DSp 1.7 PDF p.17 iteration contract. Pre-fix code passed no
	 * index; GetNextContext was a stub terminator that only ever returned
	 * kDSpContextNotFoundErr, so apps like The Sims saw only ONE mode
	 * (the lowest-sorted 1bpp entry) and bailed when it didn't match
	 * their depth requirement. */
	const DSpContextAttributes *head = &s_dsp_modes[0];
	uint32_t handle = DSpAllocMetadataContextHandle(
	    head, /*enumeration_mode_index=*/0);
	if (handle == 0) {
		DSP_LOG("GetFirstContext: table full — kDSpInternalErr");
		return kDSpInternalErr;
	}

	*outHandle = handle;
	DSP_LOG("GetFirstContext: vended handle=%u %ux%u@%ubpp (enum_idx=0 of %zu)",
	        handle, head->displayWidth, head->displayHeight,
	        head->backBufferBestDepth, s_dsp_modes.size());
	return kDSpNoErr;
}

/*
 *  DSpGetFirstContextHandler — dispatch handler for sub-opcode 200.
 *
 *  Validation:
 *    - displayID: any value accepted. iOS hosts a single backing screen,
 *      so every displayID hard-routes to that screen (displayID arg
 *      accepted; hard-routes to primary).
 *      An earlier revision rejected non-zero values; the reject was lifted
 *      after The Sims UAT (debug session sims-dsp-subop-mismatch) showed
 *      real apps pass Display-Manager IDs like 256, then SIGKILL when they
 *      cannot obtain a context.
 *    - outContextRefAddr == 0: rejected with kDSpInvalidAttributesErr.
 *
 *  After validation the core logic takes over.
 */
extern "C" int32_t DSpGetFirstContextHandler(uint32_t displayID,
                                              uint32_t outContextRefAddr)
{
	if (displayID != 0) {
		/* Diagnostic: surface the non-zero path so future loggo captures
		 * show real-world Display-Manager IDs reaching the single-screen
		 * mapping. */
		DSP_LOG("GetFirstContext: displayID=%u accepted "
		        "(single-screen iOS — mapped to backing screen)",
		        displayID);
	}
	if (outContextRefAddr == 0) {
		DSP_LOG("GetFirstContext: NULL outContextRefAddr — "
		        "kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}

	uint32_t handle = 0;
	int32_t rc = DSpGetFirstContext_Core(&handle);
	if (rc != kDSpNoErr) return rc;

	WriteMacInt32(outContextRefAddr, handle);
	return kDSpNoErr;
}


/* =========================================================================
 *  DSpGetNextContext (sub-opcode 203).
 *
 *  Advances a GetFirst/GetNext enumeration cursor through s_dsp_modes. The
 *  same public handle is advanced in place so apps can walk more modes than
 *  the small DSp context table can hold.
 *
 *  Validation chain (mirrors DSpGetFirstContextHandler):
 *    - outContextRefAddr == 0: return kDSpInvalidAttributesErr.
 *    - prevCtxRef != 0 && DSpGetContext(prevCtxRef) == nullptr:
 *      return kDSpInvalidContextErr.
 *    - Last context writes 0 to outContextRefAddr and returns
 *      kDSpContextNotFoundErr.
 * =========================================================================
 */
/*
 *  Debug session `dsp-sims-enumeration-stall` fix (2026-04-19): core
 *  logic for DSpGetNextContextHandler. Walks
 *  s_dsp_modes forward from the prev context's enumeration_mode_index,
 *  mutating that metadata-only cursor in place.
 *
 *  Semantics per DSp 1.7 PDF p.17:
 *    - prevCtxRef resolves to a context with enumeration_mode_index = N:
 *        if N + 1 < s_dsp_modes.size(): update the same handle to
 *            s_dsp_modes[N+1] with enumeration_mode_index = N + 1; return
 *            kDSpNoErr.
 *        else (N is last mode): *outHandle = 0;
 *                                return kDSpContextNotFoundErr (terminator).
 *    - prevCtxRef resolves to a context with
 *      enumeration_mode_index == DSP_ENUMERATION_INDEX_NONE (a FindBest
 *      result or Reserved context): *outHandle = 0;
 *      return kDSpContextNotFoundErr. Such a context is not on the
 *      iteration chain — PDF p.17 treats this as "last context in the
 *      list" and the while-loop example terminates cleanly.
 *
 *  Caller responsibilities:
 *    - Validate prevCtxRef's existence in the handle table (pre-Core).
 *    - Validate outHandle != nullptr (pre-Core).
 *
 *  Returns (kDSpNoErr | kDSpContextNotFoundErr | kDSpInternalErr). The
 *  handler wrappers round-trip *outHandle through guest RAM /
 *  test-harness out-param.
 */
static int32_t DSpGetNextContext_Core(uint32_t prevCtxRef, uint32_t *outHandle)
{
	/* prevCtxRef == 0: PDF p.17 "should be a reference that was just
	 * returned by DSpGetFirstContext or DSpGetNextContext" — 0 is
	 * not a valid prev handle, but the accept-any policy lets us treat
	 * it as "iteration over" (terminator). Matches historical lift
	 * behavior. */
	if (prevCtxRef == 0) {
		*outHandle = 0;
		DSP_LOG("GetNextContext: prevCtxRef=0 — terminator "
		        "(kDSpContextNotFoundErr)");
		return kDSpContextNotFoundErr;
	}

	DSpContextPrivate *prev_ctx = DSpGetContext(prevCtxRef);
	if (prev_ctx == nullptr) {
		DSP_LOG("GetNextContext: invalid prevCtxRef=0x%08x — "
		        "kDSpInvalidContextErr", prevCtxRef);
		return kDSpInvalidContextErr;
	}

	/* Debug session `dsp-sims-enumeration-stall` fix (2026-04-19): the full
	 * DSpContextPrivate struct requires <Metal/Metal.h> which this .cpp
	 * file does not pull in. Use the C-safe accessor
	 * DSpGetContextEnumerationIndex instead of reading the field
	 * directly. The accessor returns DSP_ENUMERATION_INDEX_NONE on invalid
	 * ctxRef; we've already validated prev_ctx != nullptr above so this
	 * path returns the true field value. */
	uint32_t prev_idx = DSpGetContextEnumerationIndex(prevCtxRef);

	/* Prev context is not on the enumeration chain (FindBest result or
	 * Reserved context): terminate per PDF p.17 "last context in the
	 * list". */
	if (prev_idx == DSP_ENUMERATION_INDEX_NONE) {
		*outHandle = 0;
		DSP_LOG("GetNextContext: prevCtxRef=%u has no enumeration cursor "
		        "(FindBest/Reserved) — terminator (kDSpContextNotFoundErr)",
		        prevCtxRef);
		return kDSpContextNotFoundErr;
	}

	/* Defensive — if enum index points past the cache (should not happen
	 * given single-writer emul-thread + immutable s_dsp_modes between
	 * DSpInit/DSpShutdown, but a shrink of s_dsp_modes while a handle
	 * outlives it WOULD land here): treat as terminator. */
	size_t next_idx = (size_t)prev_idx + 1;
	if (next_idx >= s_dsp_modes.size()) {
		*outHandle = 0;
		/* Do NOT release prevCtxRef here. PDF p.16: enumerated,
		 * un-Reserved refs remain valid for DSpContext_GetAttributes /
		 * GetDisplayID / Flatten — apps (Myth II) retain every ref from
		 * the walk and read attributes only after it completes. The old
		 * terminal release left those refs dangling: GetAttributes
		 * returned kDSpInvalidContextErr before writing a byte, and the
		 * app formatted its own uninitialized memory (the
		 * "13107 x 13107, 13107 Hz" resolution list). Stale enumeration
		 * contexts are reclaimed by DSpAllocMetadataContextHandle
		 * recycling under table pressure instead. */
		DSP_LOG("GetNextContext: prevCtxRef=%u at last mode (idx=%u of %zu) "
		        "— terminator, ref stays valid; kDSpContextNotFoundErr",
		        prevCtxRef, (unsigned)prev_idx, s_dsp_modes.size());
		return kDSpContextNotFoundErr;
	}

	/* Each step vends a DISTINCT metadata context (PDF p.16 retained-ref
	 * semantics — see the terminal comment above). History: the
	 * 2026-04-21 `dsp-enum-context-table-exhaustion` fix advanced the
	 * cursor IN PLACE because a 36-mode walk overflowed the then-8-slot
	 * table (The Sims bailed before its 16bpp modes) — but in-place
	 * advance aliases every previously returned ref onto the final mode
	 * (audit DSP-08), and the terminal release then dangled them all.
	 * Exhaustion is now solved structurally instead: DSP_MAX_CONTEXTS=64
	 * plus recycling of stale enumeration contexts in
	 * DSpAllocMetadataContextHandle. */
	const DSpContextAttributes *next_mode = &s_dsp_modes[next_idx];
	uint32_t next_handle = DSpAllocMetadataContextHandle(next_mode,
	                                                     (uint32_t)next_idx);
	if (next_handle == 0) {
		DSP_LOG("GetNextContext: metadata context alloc failed at idx=%zu "
		        "(table full, nothing recyclable) — kDSpInternalErr",
		        next_idx);
		return kDSpInternalErr;
	}

	*outHandle = next_handle;
	DSP_LOG("GetNextContext: handle=%u %ux%u@%ubpp "
	        "(enum_idx=%zu of %zu; distinct handle, prev=%u untouched)",
	        next_handle, next_mode->displayWidth, next_mode->displayHeight,
	        next_mode->backBufferBestDepth,
	        next_idx, s_dsp_modes.size(), prevCtxRef);
	return kDSpNoErr;
}

extern "C" int32_t DSpGetNextContextHandler(uint32_t prevCtxRef,
                                             uint32_t outContextRefAddr)
{
	/* Null out-ptr: mirrors GetFirstContext validation (PDF p.87
	 * kDSpInvalidAttributesErr "Some field in an attributes structure
	 * has an invalid value"). */
	if (outContextRefAddr == 0) {
		DSP_LOG("GetNextContext: NULL outContextRefAddr — "
		        "kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}

	uint32_t out_handle = 0;
	int32_t rc = DSpGetNextContext_Core(prevCtxRef, &out_handle);

	/* Always write the out-ptr — for kDSpNoErr we write the new handle;
	 * for kDSpContextNotFoundErr we write 0 so the PDF p.17 while-loop
	 * example terminates cleanly. On kDSpInvalidContextErr we leave the
	 * out-ptr untouched per the "validation errors do not write out" PDF
	 * convention (matches Reserve / FindBest behavior). */
	if (rc == kDSpNoErr || rc == kDSpContextNotFoundErr) {
		WriteMacInt32(outContextRefAddr, out_handle);
	}
	return rc;
}


/* =========================================================================
 *  DSpFindBestContext (sub-opcode 201).
 *
 *  Implements the three-tier selection algorithm. Tier 0 filters by
 *  displayDepthMask overlap; Tier 1 picks display bit-depth (exact →
 *  deeper ≥ requested → deepest available); Tier 2 picks resolution (exact →
 *  smallest-upper-bound → closest-by-area-delta).
 *
 *  Tier 3 (refresh-rate tiebreaker) is intentionally a no-op:
 *  s_dsp_modes always has frequency=0 per PDF p.66 which permits
 *  "value is 0 if actual frequency is not available". Revisit if
 *  ProMotion 60/120 Hz quantization matters for any DSp-1.7-era app.
 *
 *  Threading: pure function over s_dsp_modes; same single-writer
 *  emul-thread contract as DSpGetFirstContext_Core. No mutex, no _Atomic,
 *  no MTLFence. Bounded by s_dsp_modes.size() (< VModes[64]);
 *  nested loops are each O(n); worst-case O(n²) ~4k iterations on a
 *  fully-populated cache.
 *
 *  Routing: nullptr Core return maps to kDSpContextNotFoundErr
 *  (PDF p.87 authoritative; see dsp_engine.h doc block).
 * ========================================================================= */

namespace {

/*
 *  DSpFindBestContext_Core — pure function over s_dsp_modes.
 *
 *  Returns:
 *    - nullptr when s_dsp_modes is empty, when no mode's displayBestDepth
 *      overlaps the requested display-depth mask, or when depth_filtered would be
 *      empty after Tier 1 (theoretically unreachable given Tier 1's deepest-
 *      available fallback, defensive guard retained).
 *    - pointer INTO s_dsp_modes (never allocates, never mutates the cache)
 *      on success.
 *
 *  Caller responsibilities:
 *    - Validate req != nullptr before calling.
 *    - Map nullptr to kDSpContextNotFoundErr.
 */
const DSpContextAttributes *DSpFindBestContext_Core(
    const DSpContextAttributes *req)
{
	/* --- Tier 0: display-depth-mask overlap ----------------------------
	 * s_dsp_modes represents display contexts. The requested display mask
	 * selects the front/display mode; the requested back-buffer depth is
	 * applied later by DSpContext_Reserve_OnHandle_Core.
	 * If no mode satisfies the display mask, bail out
	 * with nullptr so the caller can map to kDSpContextNotFoundErr. */
	std::vector<const DSpContextAttributes *> pool;
	pool.reserve(s_dsp_modes.size());
	const uint32_t display_depth_mask =
	    DSpUserSelectRequestedDisplayDepthMask(*req);
	for (const auto &m : s_dsp_modes) {
		uint32_t mode_depth_mask = DepthMaskForDepth(m.displayBestDepth);
		if ((display_depth_mask & mode_depth_mask) != 0) {
			pool.push_back(&m);
		}
	}
	if (pool.empty()) {
		DSP_LOG("FindBest: Tier 0 empty — req displayDepthMask=0x%08x yields "
		        "no candidates; kDSpContextNotFoundErr",
		        display_depth_mask);
		return nullptr;
	}

	/* --- Tier 1: Bit-depth preference ----------------------------------
	 * Primary: exact displayBestDepth match.
	 * Fallback A: deepest depth that is >= requested.
	 * Fallback B: deepest available depth overall (requested exceeded
	 *             all available — still return a usable mode).
	 * PDF p.25 example: "320x240x16 requested, best match is 640x480x32"
	 * demonstrates the depth-UP preference. */
	std::vector<const DSpContextAttributes *> depth_filtered;
	depth_filtered.reserve(pool.size());
	uint32_t exact_depth = DSpUserSelectRequestedDisplayBestDepth(*req);
	for (auto *c : pool) {
		if (c->displayBestDepth == exact_depth) {
			depth_filtered.push_back(c);
		}
	}
	if (depth_filtered.empty()) {
		uint32_t best_depth = 0;
		for (auto *c : pool) {
			if (c->displayBestDepth >= exact_depth &&
			    c->displayBestDepth > best_depth) {
				best_depth = c->displayBestDepth;
			}
		}
		if (best_depth == 0) {
			/* Requested depth exceeds every mode — use deepest available. */
			for (auto *c : pool) {
				if (c->displayBestDepth > best_depth) {
					best_depth = c->displayBestDepth;
				}
			}
		}
		for (auto *c : pool) {
			if (c->displayBestDepth == best_depth) {
				depth_filtered.push_back(c);
			}
		}
	}
	if (depth_filtered.empty()) {
		/* Defensive: Tier 1 fallback logic should always produce a
		 * non-empty set given pool was non-empty. If it ever doesn't,
		 * surface via kDSpContextNotFoundErr rather than fronting an
		 * out-of-bounds read. */
		DSP_LOG("FindBest: Tier 1 unexpectedly empty — "
		        "kDSpContextNotFoundErr");
		return nullptr;
	}

	/* --- Tier 2: Resolution match --------------------------------------
	 * Primary: exact (displayWidth, displayHeight) match.
	 * Fallback A: smallest mode whose (w >= req.w AND h >= req.h) — i.e.
	 *             smallest-upper-bound by display area.
	 * Fallback B: closest-by-absolute-area-delta when no upper-bound
	 *             exists (all candidates are strictly smaller than
	 *             requested). */
	std::vector<const DSpContextAttributes *> resolution_filtered;
	resolution_filtered.reserve(depth_filtered.size());
	for (auto *c : depth_filtered) {
		if (c->displayWidth == req->displayWidth &&
		    c->displayHeight == req->displayHeight) {
			resolution_filtered.push_back(c);
		}
	}
	if (resolution_filtered.empty()) {
		/* Fallback A: smallest-upper-bound. */
		uint64_t best_area = UINT64_MAX;
		for (auto *c : depth_filtered) {
			if (c->displayWidth >= req->displayWidth &&
			    c->displayHeight >= req->displayHeight) {
				uint64_t a = (uint64_t)c->displayWidth *
				             (uint64_t)c->displayHeight;
				if (a < best_area) best_area = a;
			}
		}
		if (best_area != UINT64_MAX) {
			for (auto *c : depth_filtered) {
				uint64_t a = (uint64_t)c->displayWidth *
				             (uint64_t)c->displayHeight;
				if (a == best_area) resolution_filtered.push_back(c);
			}
		} else {
			/* Fallback B: closest-by-absolute-area-delta. */
			uint64_t req_area = (uint64_t)req->displayWidth *
			                    (uint64_t)req->displayHeight;
			uint64_t best_delta = UINT64_MAX;
			for (auto *c : depth_filtered) {
				uint64_t a = (uint64_t)c->displayWidth *
				             (uint64_t)c->displayHeight;
				uint64_t delta = a > req_area ? a - req_area
				                                : req_area - a;
				if (delta < best_delta) best_delta = delta;
			}
			for (auto *c : depth_filtered) {
				uint64_t a = (uint64_t)c->displayWidth *
				             (uint64_t)c->displayHeight;
				uint64_t delta = a > req_area ? a - req_area
				                                : req_area - a;
				if (delta == best_delta) {
					resolution_filtered.push_back(c);
				}
			}
		}
	}
	/* resolution_filtered is non-empty by construction:
	 *   - depth_filtered was non-empty entering Tier 2.
	 *   - Fallback A either produces >=1 hit (some mode >= requested) or
	 *     falls through to Fallback B, which always produces >=1 hit
	 *     (req_area-to-mode_area minimum exists over a non-empty set).
	 */

	/* --- Tier 3: Refresh-rate tiebreaker -------------------------------
	 * frequency==0 for all modes (VModes has no refresh; PDF p.66 permits
	 * 0). Tier 3 is a no-op and returns the head of resolution_filtered.
	 * Can be extended with quantization. */
	const DSpContextAttributes *best = resolution_filtered.front();
	DSP_LOG("FindBest: req=%ux%u back@%ubpp/bmask=0x%08x "
	        "display@%ubpp/dmask=0x%08x pc=%u => best display=%ux%u@%ubpp "
	        "(pool=%zu depth=%zu res=%zu)",
	        req->displayWidth, req->displayHeight,
	        req->backBufferBestDepth, req->backBufferDepthMask,
	        req->displayBestDepth, req->displayDepthMask, req->pageCount,
	        best->displayWidth, best->displayHeight,
	        best->displayBestDepth,
	        pool.size(), depth_filtered.size(),
	        resolution_filtered.size());
	return best;
}

/*
 *  DSpFindBestContext_AllocAndWriteBack — shared alloc+populate helper.
 *
 *  Allocates a metadata-only DSp context via DSpAllocMetadataContextHandle
 *  and writes the 1-based handle to *outHandle.
 *  The underlying DSpAllocFirstContextHandle copies *best into ctx->attr
 *  and sets ctx->state = kDSpContextState_Inactive.
 *
 *  Returns true on success (handle written to *outHandle, >= 1);
 *  false on table-full. The caller maps false to kDSpInternalErr.
 */
bool DSpFindBestContext_AllocAndWriteBack(const DSpContextAttributes *best,
                                          uint32_t *outHandle)
{
	/* Debug session `dsp-sims-enumeration-stall` fix (2026-04-19):
	 * FindBest returns a best-match result, not a cursor into the
	 * enumeration. Pass DSP_ENUMERATION_INDEX_NONE so GetNextContext
	 * treats the handle as a terminator (PDF p.17 "last context in the
	 * list"). */
	uint32_t handle = DSpAllocMetadataContextHandle(
	    best, DSP_ENUMERATION_INDEX_NONE);
	if (handle == 0) {
		DSP_LOG("FindBest: AllocMetadataContextHandle failed — "
		        "table full (kDSpInternalErr upstream)");
		return false;
	}
	*outHandle = handle;
	return true;
}

}  // anonymous namespace

/*
 *  DSpFindBestContextHandler — dispatch handler for sub-opcode 201.
 *
 *  Validation + guest-RAM round-trip per PDF p.65 layout. On nullptr Core
 *  return the handler maps to kDSpContextNotFoundErr; on alloc failure it
 *  maps to kDSpInternalErr.
 *
 *  attrAddr and outContextRefAddr are both explicitly checked for 0 before
 *  any ReadMacInt* / WriteMacInt32.
 */
extern "C" int32_t DSpFindBestContextHandler(uint32_t attrAddr,
                                              uint32_t outContextRefAddr)
{
	if (attrAddr == 0) {
		DSP_LOG("FindBest: NULL attrAddr — kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}
	if (outContextRefAddr == 0) {
		DSP_LOG("FindBest: NULL outContextRefAddr — "
		        "kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}

	/*
	 *  Read the requested DSpContextAttributes from guest RAM using the
	 *  DSp 1.7 PDF p.65 on-wire byte layout (same offsets as Reserve_Core).
	 *  All UInt32 / Fixed / OptionBits fields are 4 bytes. filler[3] @
	 *  +52..+54 + gameMustConfirmSwitch (Boolean) @ +55 + reserved3[4] @
	 *  +56..+71 are not consulted by the three-tier algorithm and are
	 *  not read. Re-corrected 2026-04-21 via debug session
	 *  `dsp-sims-rejects-all-modes`; backBufferWidth / backBufferHeight
	 *  are host-only mirror fields (not on the wire) so they are
	 *  populated from displayWidth / displayHeight below for any
	 *  downstream Tier 2 size-match logic that inspects them.
	 */
	DSpContextAttributes req = {};
	req.frequency             = ReadMacInt32(attrAddr +  0);  /* input ignored per PDF */
	req.displayWidth          = ReadMacInt32(attrAddr +  4);
	req.displayHeight         = ReadMacInt32(attrAddr +  8);
	req.reserved1             = ReadMacInt32(attrAddr + 12);
	req.reserved2             = ReadMacInt32(attrAddr + 16);
	req.colorNeeds            = ReadMacInt32(attrAddr + 20);
	req.colorTable            = ReadMacInt32(attrAddr + 24);
	req.contextOptions        = ReadMacInt32(attrAddr + 28);
	req.backBufferDepthMask   = ReadMacInt32(attrAddr + 32);
	req.displayDepthMask      = ReadMacInt32(attrAddr + 36);
	req.backBufferBestDepth   = ReadMacInt32(attrAddr + 40);
	req.displayBestDepth      = ReadMacInt32(attrAddr + 44);
	req.pageCount             = ReadMacInt32(attrAddr + 48);
	/* filler[3] @ +52..+54 skipped. gameMustConfirmSwitch (Boolean) @ +55
	 * is input-ignored per PDF p.67. reserved3[4] @ +56..+71 skipped. */
	req.gameMustConfirmSwitch = 0;
	/* Host-only mirror fields — populate so any downstream code that
	 * inspects backBufferWidth/Height sees consistent values. */
	req.backBufferWidth       = req.displayWidth;
	req.backBufferHeight      = req.displayHeight;
	const DSpContextAttributes *best = DSpFindBestContext_Core(&req);
	if (best == nullptr) {
		/* No mode satisfies the attribute mask. */
		return kDSpContextNotFoundErr;
	}

	uint32_t handle = 0;
	if (!DSpFindBestContext_AllocAndWriteBack(best, &handle)) {
		return kDSpInternalErr;
	}

	WriteMacInt32(outContextRefAddr, handle);
	return kDSpNoErr;
}

