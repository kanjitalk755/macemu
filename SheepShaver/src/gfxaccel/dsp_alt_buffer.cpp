/*
 *  dsp_alt_buffer.mm - DrawSprocket AltBuffer subsystem (sub-ops 700-705).
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Extracted verbatim from dsp_draw_context.mm (de-bloat, NO behaviour change):
 *  the alt-buffer record table + lifecycle helpers, the production handlers
 *  (New/Dispose/GetCGrafPtr/InvalRect + underlay Get/Set), and
 *  DSpSyncAltBufferStagingToBacking.
 *  Cross-TU surface (DSpGetAltBuffer / DSpSyncAltBufferStagingToBacking /
 *  DSpResetAltBufferTable) is declared in dsp_alt_buffer.h. The record struct +
 *  DSP_MAX_ALT_BUFFERS live in dsp_alt_buffer.h.
 */
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "thunks.h"                /* SheepMem::Reserve (test hook) */
#include "dsp_engine.h"
#include "dsp_draw_context.h"
#include "dsp_mode_enumerate.h"    /* DSpFindBestContextHandler delegate target */
#include "dsp_user_select_policy.h"
#include "dsp_context_private.h"   /* DSpContextPrivate full struct; shared with dsp_metal_renderer.mm */
#include "dsp_cgraf_port_policy.h"
#include "dsp_default_clut.h"
#include "dsp_display_id_policy.h"
#include "dsp_display_mode_policy.h"
#include "dsp_front_buffer_policy.h"
#include "dsp_get_attributes_policy.h"
#include "dsp_guest_address.h"
#include "dsp_pixmap_offsets.h"    /* PixMap field offsets + LMADDR_MAIN_DEVICE / GDEVICE_OFF_PMAP */
#include "dsp_metal_renderer.h"    /* shared DSp buffer allocation/backing access */
#include "gfxaccel_resources.h"    /* per-buffer owner-tag API */
#include "gfxaccel_resources_heap.h" /* kHeapEngineDSp + heap_alloc_buffer for AltBuffer backing */
#include "nqd_accel.h"               /* NQDMetalBitblt1to1 / NQDMetalBitbltScaled / NQDMetalFlush for DSpBlit */
#include "metal_compositor.h"      /* canonical screen query and pacing */
#include "metal_device_shared.h"   /* SharedMetalCommandQueue (SwapBuffers blit path) */
#include "display_mode_controller.h" /* dmc_current_snapshot (FrameDescriptor generation); DMCOwner enum + dmc_set_active_owner */
#include "dsp_engine_internal.h"   /* DSpMapStateToDMCOwnerTyped (internal-only, NOT in include/) */
#include "vbl_source.h"
#include "dsp_alt_buffer.h"

// ===== AltBuffer record table + lifecycle =================================
static DSpAltBufferRecord dsp_alt_buffer_table[DSP_MAX_ALT_BUFFERS] = {};

static void DSpResetHeapIfIdleAfterAltBufferRelease(const char *reason)
{
	uint32_t live = gfxaccel_resources_heap_live_allocation_count(kHeapEngineDSp);
	if (live != 0) {
		return;
	}
	uint64_t reclaimed = gfxaccel_resources_heap_reset(kHeapEngineDSp);
	if (reclaimed > 0) {
		DSP_LOG("DSp heap reset after %s reclaimed %llu bytes",
				reason ? reason : "alt-buffer release",
				(unsigned long long)reclaimed);
	}
}

/* Zero a record's fields WITHOUT memset (DSpAltBufferRecord holds ARC
 * id<MTLBuffer>/id<MTLTexture> fields - memset over them bypasses ARC and is
 * undefined, -Wnontrivial-memcall). ObjC fields are assigned NULL (ARC
 * releases); POD fields are explicitly reset. */
static void DSpClearAltBufferRecord(DSpAltBufferRecord *rec)
{
	/* Release the guest-RAM pixel-staging buffer that backed the CGrafPort
	 * (if any). It was exposed to the guest as a PixMap baseAddr, so it must
	 * be quarantined (retained, never freed back to the app heap), like the
	 * guest-visible back-buffer fallback staging. */
	if (rec->baseaddr_owned_staging && rec->baseaddr_mac != 0) {
		DSpQuarantineGuestPixelStaging(rec->baseaddr_mac,
									   rec->baseaddr_size, true);
	}
	rec->in_use            = false;
	rec->texture           = NULL;   /* texture is a view over backing - drop it first */
	if (rec->backing != NULL) {
		gfxaccel_resources_clear_buffer_owner(rec->backing);
		gfxaccel_resources_heap_note_allocation_released(kHeapEngineDSp);
	}
	rec->backing           = NULL;   /* DSp heap resets when all DSp buffers are idle */
	rec->cgrafptr_mac_addr = 0;
	rec->baseaddr_mac      = 0;
	rec->baseaddr_size     = 0;
	rec->baseaddr_owned_staging = false;
	rec->width             = 0;
	rec->height            = 0;
	rec->depth             = 0;
	rec->seed_pixmap_mac   = 0;
	rec->options           = 0;
	rec->underlay_capable  = false;
	rec->dirty_left        = 0;
	rec->dirty_top         = 0;
	rec->dirty_right       = 0;
	rec->dirty_bottom      = 0;
	rec->dirty_empty       = true;
}

/* Resolve a 1-based alt-buffer handle to its record (NULL if invalid or
 * not in use). Mirrors DSpGetContext. */
DSpAltBufferRecord *DSpGetAltBuffer(uint32_t handle)
{
	if (handle == 0 || handle > DSP_MAX_ALT_BUFFERS) return NULL;
	DSpAltBufferRecord *rec = &dsp_alt_buffer_table[handle - 1];
	return rec->in_use ? rec : NULL;
}

/* Allocate a free alt-buffer slot, returning its 1-based handle (0 if the
 * table is full). The caller fills the record fields. Single-writer. */
static uint32_t DSpAllocAltBufferHandle(void)
{
	for (int i = 0; i < DSP_MAX_ALT_BUFFERS; i++) {
		if (!dsp_alt_buffer_table[i].in_use) {
			DSpAltBufferRecord *rec = &dsp_alt_buffer_table[i];
			DSpClearAltBufferRecord(rec);
			rec->in_use      = true;
			rec->dirty_empty = true;
			return (uint32_t)(i + 1);
		}
	}
	return 0;
}

/* Release an alt-buffer record's heap backing + clear the slot. Texture
 * first, buffer second (mirrors DSpReleaseNow). */
static void DSpFreeAltBuffer(uint32_t handle)
{
	if (handle == 0 || handle > DSP_MAX_ALT_BUFFERS) return;
	DSpClearAltBufferRecord(&dsp_alt_buffer_table[handle - 1]);
	DSpResetHeapIfIdleAfterAltBufferRelease("alt-buffer free");
}

/* --------------------------------------------------------------------- *
 *  AltBuffer exports (sub-ops 700-705)                                   *
 *                                                                       *
 *  Real Metal-backed AltBuffer implementations reusing the              *
 *  engine-blind gfxaccel infra: each alt-buffer backs onto the          *
 *  DSp heap (kHeapEngineDSp) - NOT the one-per-engine overlay slot, so   *
 *  N alt-buffers coexist. A designated underlay feeds the                *
 *  DSpRestoreBackBufferFromUnderlay CPU dirty-band restore path when     *
 *  host-visible depth-matched backing is available. ZERO new concurrency *
 *  primitives:                                                          *
 *  every record field is single-writer emul-thread RAM + the            *
 *  existing single MTLCommandQueue.                                      *
 *                                                                       *
 *  Alt-buffers inherit the OWNING CONTEXT's back-buffer depth (real DSp  *
 *  semantics: an alt buffer is an alternate surface of the context, so   *
 *  NULL inAttributes means "same attributes" INCLUDING depth). The       *
 *  CGrafPort GetCGrafPtr emits describes that same depth, which keeps    *
 *  DSpBlit_* src/dst depths equal (Diablo II software mode draws its     *
 *  8-bpp frame into an alt buffer and Blit_Fastest's it to the front     *
 *  buffer - a fixed 32-bpp alt backing made every such blit fail with    *
 *  a depth mismatch and the game presented nothing).                     *
 * --------------------------------------------------------------------- */


/* --- DSpAltBuffer_NewHandler (sub-op 700) ---
 *
 *  DSp 1.7 PDF p.48: DSpAltBuffer_New(inContext, inVRAMBuffer,
 *  inAttributes, outAltBuffer). inAttributes==NULL => "same attributes as
 *  the specified context" + the alt-buffer IS usable as an underlay (PDF
 *  p.49). A non-NULL inAttributes => overlay-kind (NOT underlay-usable).
 *  inVRAMBuffer is advisory (we always heap-back via kHeapEngineDSp).
 *  Writes the new 1-based handle to outAltBuffer. NULL outAltBuffer =>
 *  kDSpInvalidAttributesErr (ASVS V5 - guard out-ptr before write). */
extern "C" int32_t DSpAltBuffer_NewHandler(uint32_t ctxRef,
										   uint32_t inVRAMBuffer,
										   uint32_t inAttributesAddr,
										   uint32_t outAltBufferAddr)
{
	(void)inVRAMBuffer;   /* advisory; we always heap-back (PDF "may fall back to heap") */
	if (outAltBufferAddr == 0) {
		DSP_LOG("AltBuffer_New: NULL outAltBuffer -> kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL) {
		DSP_LOG("AltBuffer_New: invalid ctxRef=%u -> kDSpInvalidContextErr", ctxRef);
		return kDSpInvalidContextErr;
	}

	/* Determine dimensions + underlay-capability. NULL inAttributes =>
	 * context dims + underlay-capable; non-NULL => attribute dims +
	 * overlay-kind (PDF p.49). */
	uint32_t w, h, options;
	bool underlay_capable;
	if (inAttributesAddr == 0) {
		w = DSpContextBackBufferWidth(ctx);
		h = DSpContextBackBufferHeight(ctx);
		options = 0;
		underlay_capable = true;
	} else {
		/* Validate the whole attribute-struct extent lies in mapped RAM
		 * before dereferencing any field (ASVS V5; matches the
		 * NQDMetalAddrInBuffer idiom). DSpAltBufferAttributes is 3 contiguous
		 * UInt32s (width@0, height@4, options@8) -> 12 bytes. A bogus non-NULL
		 * pointer would otherwise fault the ReadMacInt32 translation layer. */
		const uint32_t kAltAttrSize = 12u;
		if (!NQDMetalAddrInBuffer(inAttributesAddr) ||
			!NQDMetalAddrInBuffer(inAttributesAddr + kAltAttrSize - 1u)) {
			DSP_LOG("AltBuffer_New: inAttributes 0x%08x out of mapped RAM -> "
					"kDSpInvalidAttributesErr", inAttributesAddr);
			return kDSpInvalidAttributesErr;
		}
		w = ReadMacInt32(inAttributesAddr + 0);   /* DSpAltBufferAttributes.width  */
		h = ReadMacInt32(inAttributesAddr + 4);   /* .height */
		options = ReadMacInt32(inAttributesAddr + 8); /* .options */
		underlay_capable = false;
	}
	/* Reject degenerate AND out-of-range guest dims up front with the
	 * documented attributes error (the backing allocator also rejects, but a
	 * dim error is the honest answer for a bad attribute block). The cap keeps
	 * alignedRB <= 0x3FFF so the 16-bit GetCGrafPtr rowBytes write is exact
	 * and the row-bytes/size products cannot overflow. */
	if (w == 0 || h == 0 || w > DSP_ALT_MAX_DIM || h > DSP_ALT_MAX_DIM) {
		DSP_LOG("AltBuffer_New: out-of-range dims %ux%u (cap=%u) -> "
				"kDSpInvalidAttributesErr", w, h, (uint32_t)DSP_ALT_MAX_DIM);
		return kDSpInvalidAttributesErr;
	}

	uint32_t handle = DSpAllocAltBufferHandle();
	if (handle == 0) {
		DSP_LOG("AltBuffer_New: alt-buffer table full -> kDSpInternalErr");
		return kDSpInternalErr;
	}
	DSpAltBufferRecord *rec = &dsp_alt_buffer_table[handle - 1];
	rec->options          = options;
	rec->underlay_capable = underlay_capable;

	/* An alt buffer is an alternate surface OF THE CONTEXT, so it inherits
	 * the context's back-buffer depth (DSp 1.7 PDF p.48 "same attributes").
	 * The drawable CGrafPort and Metal backing both use this depth, keeping
	 * DSpBlit_* alt<->back/front transfers depth-matched. Depths outside
	 * 8/16/32 (never produced by Reserve) normalize to 32. */
	uint32_t depth = ctx->attr.backBufferBestDepth;
	if (depth != 8 && depth != 16 && depth != 32) depth = 32;
	rec->depth = depth;
	/* Alternate buffers are PixMaps for the owning display, not anonymous
	 * bitmaps. Preserve the screen PixMap's ColorTable handle and the other
	 * QuickDraw metadata that is independent of base/stride/bounds. This is
	 * essential at 8 bpp: a zero pmTable makes indexed QuickDraw dereference
	 * invalid palette state after Diablo's opening movies. */
	const uint32_t live_pixmap = DSpGetLiveMainDevicePixMap();
	rec->seed_pixmap_mac =
		DSpGuestRAMContains(live_pixmap,
						   DSpFrontBufferPixMapRecordSize(),
						   (uint32_t)RAMBase,
						   (uint32_t)RAMSize)
			? live_pixmap : 0;

	if (!DSpAllocAltBufferBacking(rec, w, h)) {
		DSpFreeAltBuffer(handle);
		DSP_LOG("AltBuffer_New: backing alloc failed -> kDSpInternalErr");
		return kDSpInternalErr;
	}

	WriteMacInt32(outAltBufferAddr, handle);
	DSP_LOG("AltBuffer_New: ctx=%u %ux%u@%ubpp handle=%u underlay_capable=%d",
			ctxRef, w, h, depth, handle, underlay_capable);
	return kDSpNoErr;
}


/* --- DSpAltBuffer_DisposeHandler (sub-op 701) ---
 *
 *  DSp 1.7 PDF p.49: DSpAltBuffer_Dispose(inAltBuffer). Releases the
 *  heap backing + clears the record. Unknown handle =>
 *  kDSpInvalidAttributesErr (the spec has no dedicated alt-buffer error;
 *  the attributes-error is the closest honest answer). Any context that
 *  designated this alt-buffer as its underlay has the designation cleared
 *  so GetBackBuffer does not dereference a freed record. */
extern "C" int32_t DSpAltBuffer_DisposeHandler(uint32_t altBuffer)
{
	DSpAltBufferRecord *rec = DSpGetAltBuffer(altBuffer);
	if (rec == NULL) {
		DSP_LOG("AltBuffer_Dispose: unknown handle=%u -> kDSpInvalidAttributesErr",
				altBuffer);
		return kDSpInvalidAttributesErr;
	}
	/* Clear any context underlay/overlay designation pointing at this
	 * handle (single-writer emul thread; no primitive). */
	for (int i = 0; i < DSP_MAX_CONTEXTS; i++) {
		DSpContextPrivate *c = DSpGetContext((uint32_t)(i + 1));
		if (c == NULL) continue;
		if (c->underlay_alt_buffer == altBuffer) c->underlay_alt_buffer = 0;
		if (c->overlay_alt_buffer  == altBuffer) c->overlay_alt_buffer  = 0;
	}
	DSpFreeAltBuffer(altBuffer);
	DSP_LOG("AltBuffer_Dispose: handle=%u released", altBuffer);
	return kDSpNoErr;
}


/* Bytes per pixel / Metal format for an alt-buffer depth. Same depth->format
 * mapping as the back buffer (dsp_metal_renderer.mm DSpPixelFormatForDepthBits):
 * 8 -> R8Uint (indexed, CLUT unpack), 16 -> R16Uint (xRGB1555), 32 ->
 * BGRA8Unorm. Anything else is normalized to 32 at New time. */
uint32_t DSpAltBytesPerPixel(uint32_t depth)
{
	switch (depth) {
		case 8:  return 1u;
		case 16: return 2u;
		default: return 4u;
	}
}

/* --- DSpAltBuffer_GetCGrafPtrHandler (sub-op 702) ---
 *
 *  DSp 1.7 PDF p.50: DSpAltBuffer_GetCGrafPtr(inAltBuffer, inBufferKind,
 *  outCGrafPtr). Returns a stable guest-RAM CGrafPort describing the alt-
 *  buffer's drawable surface (the app draws into it, then calls InvalRect).
 *  "Currently the only supported buffer kind is kDSpBufferKind_Normal" - any
 *  other kind => kDSpInvalidAttributesErr. SheepMem reserve + cache the Mac
 *  address on the record + W1 staging fallback when Host2MacAddr cannot map
 *  the MTLBuffer contents pointer (arm64 iOS bump-allocator outside vm_alloc).
 *  The CGrafPort is a REAL port at the record's depth, built by the shared
 *  DSpEmitSurfaceCGrafPort emitter (classic PixMap + handle + portVersion
 *  0xC000 + vis/clip regions) - apps draw into the alt buffer by
 *  dereferencing this as a genuine port (portPixMap -> GetPixBaseAddr /
 *  CopyBits), so the former compact PixMap-shaped shim sent Diablo II's
 *  software-renderer writes through misread garbage pointers (noise on
 *  screen). NULL outCGrafPtr => kDSpInvalidAttributesErr. */
extern "C" int32_t DSpAltBuffer_GetCGrafPtrHandler(uint32_t altBuffer,
												   uint32_t bufferKind,
												   uint32_t outCGrafPtrAddr)
{
	if (outCGrafPtrAddr == 0) {
		DSP_LOG("AltBuffer_GetCGrafPtr: NULL outCGrafPtr -> kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}
	if (bufferKind != (uint32_t)kDSpBufferKind_Normal) {
		DSP_LOG("AltBuffer_GetCGrafPtr: unsupported bufferKind=%u -> "
				"kDSpInvalidAttributesErr (only kDSpBufferKind_Normal)", bufferKind);
		return kDSpInvalidAttributesErr;
	}
	DSpAltBufferRecord *rec = DSpGetAltBuffer(altBuffer);
	if (rec == NULL) {
		DSP_LOG("AltBuffer_GetCGrafPtr: unknown handle=%u -> kDSpInvalidAttributesErr",
				altBuffer);
		return kDSpInvalidAttributesErr;
	}

	/* Stable-pointer contract: cache the CGrafPort Mac address on first call. */
	if (rec->cgrafptr_mac_addr != 0) {
		WriteMacInt32(outCGrafPtrAddr, rec->cgrafptr_mac_addr);
		return kDSpNoErr;
	}

	uint32_t w         = rec->width;
	uint32_t h         = rec->height;
	uint32_t row_bytes = w * DSpAltBytesPerPixel(rec->depth);
	uint32_t alignedRB = (row_bytes + 255u) & ~255u;

	/* rec->width was bounded to DSP_ALT_MAX_DIM at allocation time,
	 * so alignedRB is guaranteed <= 0x3FFF and the emitter's 16-bit PixMap
	 * rowBytes-field write is always exact (no truncation). Assert the
	 * invariant so a future cap change that breaks it is caught immediately. */
	assert(alignedRB <= 0x3FFFu);

	uint32_t buffer_size = alignedRB * h;

	/* baseAddr: Mac-address view of the backing's CPU-side contents pointer.
	 * On arm64 iOS the heap bump-allocator can live outside guest RAM.
	 * Treat non-guest mapped values the same as 0 - W1 staging fallback
	 * reserves a guest-RAM region the size of the backing. NEVER
	 * (uint32)(uintptr_t) - UB on arm64. */
	uint32_t baseAddr_mac = 0;
	bool baseaddr_owned_staging = false;
	void *contents = DSpGetBackingContents(rec->backing);
	if (contents != NULL) { /* NULL on StorageModePrivate */
		baseAddr_mac = DSpUsableGuestBaseOrZero(
			Host2MacAddr((uint8 *)contents),
			buffer_size,
			(uint32_t)RAMBase,
			(uint32_t)RAMSize);
	}
	if (baseAddr_mac == 0) {
		/* The Metal backing's contents pointer is outside guest RAM (the iOS
		 * MEM_BULK case), so expose a guest-RAM pixel-staging buffer the size
		 * of the alt-buffer - the SAME framebuffer-staging allocator the back
		 * buffer uses (Mac system heap via NewPtrSys). NEVER
		 * DSpReserveGuestScratch here: that is the 512 KB SheepMem thunk
		 * stack, and a framebuffer-sized request (up to ~1.2 MB) overruns it,
		 * tripping the SheepMem::Reserve assert (the Diablo II software-mode
		 * crash). */
		baseAddr_mac = DSpReserveGuestPixelStaging(buffer_size);
		if (baseAddr_mac == 0) {
			DSP_LOG("AltBuffer_GetCGrafPtr: neither Host2MacAddr nor pixel "
					"staging reserve(%u) vended a guest baseAddr", buffer_size);
			return kDSpInternalErr;
		}
		baseaddr_owned_staging = true;
	}
	rec->baseaddr_mac           = baseAddr_mac;
	rec->baseaddr_size          = buffer_size;
	rec->baseaddr_owned_staging = baseaddr_owned_staging;

	/* Emit a REAL CGrafPort at the record's depth (shared emitter - same
	 * construction as the front buffer). The guest dereferences this as a
	 * genuine port (portPixMap -> pixel writes / CopyBits). */
	uint32_t cgp_addr = DSpEmitSurfaceCGrafPort(baseAddr_mac,
												w, h,
												rec->depth,
												alignedRB,
												rec->seed_pixmap_mac,
												NULL, NULL);
	if (cgp_addr == 0) {
		DSP_LOG("AltBuffer_GetCGrafPtr: CGrafPort emission failed "
				"(handle=%u %ux%u@%u)", altBuffer, w, h, rec->depth);
		return kDSpInternalErr;
	}

	rec->cgrafptr_mac_addr = cgp_addr;
	WriteMacInt32(outCGrafPtrAddr, cgp_addr);
	DSP_LOG("AltBuffer_GetCGrafPtr: handle=%u cgrafptr=0x%08x baseAddr=0x%08x "
			"rb=%u bpp=%u (real port)", altBuffer, cgp_addr, baseAddr_mac,
			alignedRB, rec->depth);
	return kDSpNoErr;
}


/* Per-alt-buffer dirty-rect accumulator. Clamps to the alt-buffer's bounds
 * (ASVS V5) then unions into the record's dirty rect - same shape + semantics
 * as DSpInvalBackBufferRect_Accumulate (the back-buffer dirty accumulator),
 * single-writer, NO sync primitive. The union flows into the GetBackBuffer
 * underlay-restore on the next retrieval (sub-op 705 commit). */
static void DSpAltBufferInvalRect_Accumulate(DSpAltBufferRecord *rec,
											 int16_t top, int16_t left,
											 int16_t bottom, int16_t right)
{
	if (rec == NULL) return;

	/* Clamp to alt-buffer bounds (overscan mitigation). */
	int32_t c_top    = (top    < 0) ? 0 : top;
	int32_t c_left   = (left   < 0) ? 0 : left;
	int32_t c_bottom = (bottom < 0) ? 0 : bottom;
	int32_t c_right  = (right  < 0) ? 0 : right;
	if ((uint32_t)c_right  > rec->width)  c_right  = (int32_t)rec->width;
	if ((uint32_t)c_bottom > rec->height) c_bottom = (int32_t)rec->height;

	/* Empty / inverted rect -> no-op. */
	if (c_right <= c_left || c_bottom <= c_top) {
		return;
	}

	if (rec->dirty_empty) {
		rec->dirty_left   = (int16_t)c_left;
		rec->dirty_top    = (int16_t)c_top;
		rec->dirty_right  = (int16_t)c_right;
		rec->dirty_bottom = (int16_t)c_bottom;
		rec->dirty_empty  = false;
	} else {
		if ((int16_t)c_left   < rec->dirty_left)   rec->dirty_left   = (int16_t)c_left;
		if ((int16_t)c_top    < rec->dirty_top)    rec->dirty_top    = (int16_t)c_top;
		if ((int16_t)c_right  > rec->dirty_right)  rec->dirty_right  = (int16_t)c_right;
		if ((int16_t)c_bottom > rec->dirty_bottom) rec->dirty_bottom = (int16_t)c_bottom;
	}
}

/* --- DSpAltBuffer_InvalRectHandler (sub-op 703) ---
 *
 *  DSp 1.7 PDF p.52: DSpAltBuffer_InvalRect(inAltBuffer, inInvalidRect).
 *  "You must invalidate areas of an underlay you have changed so that the
 *  changes are transferred to the back buffer on the next SwapBuffers."
 *  Reads the Mac Rect (big-endian 4 x int16: top@+0, left@+2, bottom@+4,
 *  right@+6), clamps to the alt-buffer bounds, then unions into the record's
 *  dirty rect. NULL inInvalidRect / unknown handle ->
 *  kDSpInvalidAttributesErr. */
extern "C" int32_t DSpAltBuffer_InvalRectHandler(uint32_t altBuffer,
												 uint32_t inInvalidRectAddr)
{
	if (inInvalidRectAddr == 0) {
		DSP_LOG("AltBuffer_InvalRect: NULL rect -> kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}
	DSpAltBufferRecord *rec = DSpGetAltBuffer(altBuffer);
	if (rec == NULL) {
		DSP_LOG("AltBuffer_InvalRect: unknown handle=%u -> kDSpInvalidAttributesErr",
				altBuffer);
		return kDSpInvalidAttributesErr;
	}
	int16_t top    = (int16_t)ReadMacInt16(inInvalidRectAddr + 0);
	int16_t left   = (int16_t)ReadMacInt16(inInvalidRectAddr + 2);
	int16_t bottom = (int16_t)ReadMacInt16(inInvalidRectAddr + 4);
	int16_t right  = (int16_t)ReadMacInt16(inInvalidRectAddr + 6);
	DSpAltBufferInvalRect_Accumulate(rec, top, left, bottom, right);
	DSP_LOG("AltBuffer_InvalRect: handle=%u rect=(%d,%d)-(%d,%d) "
			"union=(%d,%d)-(%d,%d) empty=%d",
			altBuffer, top, left, bottom, right,
			rec->dirty_top, rec->dirty_left, rec->dirty_bottom, rec->dirty_right,
			rec->dirty_empty);
	return kDSpNoErr;
}


/* --- DSpContext_SetUnderlayAltBufferHandler (sub-op 705) ---
 *
 *  DSp 1.7 PDF p.51: DSpContext_SetUnderlayAltBuffer(inContext, inNewUnderlay).
 *  Designates an alt-buffer as the context's underlay. "When a back buffer is
 *  retrieved and there is an underlay buffer, the invalid areas in the back
 *  buffer are restored from the underlay buffer" - that restore is the
 *  GetBackBuffer branch below. inNewUnderlay == 0 clears the designation
 *  (no underlay). A non-zero handle must resolve to an alt-buffer that is
 *  underlay-capable (NULL-attributes kind, PDF p.49) - else
 *  kDSpInvalidAttributesErr. Bad ctxRef -> kDSpInvalidContextErr. */
extern "C" int32_t DSpContext_SetUnderlayAltBufferHandler(uint32_t ctxRef,
														  uint32_t inNewUnderlay)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL) {
		DSP_LOG("SetUnderlayAltBuffer: invalid ctxRef=%u -> kDSpInvalidContextErr",
				ctxRef);
		return kDSpInvalidContextErr;
	}
	if (inNewUnderlay != 0) {
		DSpAltBufferRecord *rec = DSpGetAltBuffer(inNewUnderlay);
		if (rec == NULL) {
			DSP_LOG("SetUnderlayAltBuffer: unknown alt-buffer=%u -> "
					"kDSpInvalidAttributesErr", inNewUnderlay);
			return kDSpInvalidAttributesErr;
		}
		if (!rec->underlay_capable) {
			DSP_LOG("SetUnderlayAltBuffer: alt-buffer=%u is overlay-kind (created "
					"with non-NULL attributes) -> kDSpInvalidAttributesErr (PDF p.49)",
					inNewUnderlay);
			return kDSpInvalidAttributesErr;
		}
	}
	ctx->underlay_alt_buffer = inNewUnderlay;
	DSP_LOG("SetUnderlayAltBuffer: ctx=%u underlay=%u",
			ctxRef, inNewUnderlay);
	return kDSpNoErr;
}


/* --- DSpContext_GetUnderlayAltBufferHandler (sub-op 704) ---
 *
 *  DSp 1.7 PDF p.52: DSpContext_GetUnderlayAltBuffer(inContext, outUnderlay).
 *  Writes the currently-designated underlay alt-buffer handle (0 if none).
 *  NULL outUnderlay -> kDSpInvalidAttributesErr; bad ctxRef ->
 *  kDSpInvalidContextErr. */
extern "C" int32_t DSpContext_GetUnderlayAltBufferHandler(uint32_t ctxRef,
														  uint32_t outUnderlayAddr)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL) {
		DSP_LOG("GetUnderlayAltBuffer: invalid ctxRef=%u -> kDSpInvalidContextErr",
				ctxRef);
		return kDSpInvalidContextErr;
	}
	if (outUnderlayAddr == 0) {
		DSP_LOG("GetUnderlayAltBuffer: NULL out -> kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}
	WriteMacInt32(outUnderlayAddr, ctx->underlay_alt_buffer);   /* 0 if none */
	return kDSpNoErr;
}


// ===== Underlay staging -> backing sync ===================================
/* Mirror an alt-buffer's guest-RAM staging (where the guest draws through the
 * GetCGrafPtr CGrafPort) into its Metal backing, so the CPU
 * underlay-restore copy reads the guest's pixels. NQD/QuickDraw draws land in
 * the guest-RAM staging; the Metal backing is a separate allocation that would
 * otherwise stay empty. Both sides are the record's depth at the same
 * 256-aligned row stride, so a flat copy of the backing extent is exact. No-op
 * on StorageModePrivate (simulator - contents NULL) or when the alt-buffer has
 * no owned guest staging. */
void DSpSyncAltBufferStagingToBacking(DSpAltBufferRecord *rec)
{
	if (rec == NULL || rec->backing == NULL) return;
	if (rec->baseaddr_mac == 0 || !rec->baseaddr_owned_staging) return;
	void *dst = DSpGetBackingContents(rec->backing);
	if (dst == NULL) return; /* NULL on StorageModePrivate */
	uint8_t *src = Mac2HostAddr(rec->baseaddr_mac);
	if (src == NULL) return;
	uint32_t alignedRB  = ((rec->width * DSpAltBytesPerPixel(rec->depth))
						   + 255u) & ~255u;
	uint32_t copy_bytes = alignedRB * rec->height;
	if (rec->baseaddr_size != 0 && copy_bytes > rec->baseaddr_size)
		copy_bytes = rec->baseaddr_size;       /* never read past the staging block */
	memcpy(dst, src, copy_bytes);
}

/* --- Background/foreground backing lifecycle (D-4-1) ----------------------
 *
 * The Metal backing is a derived cache: the guest's pixel source of truth is
 * the guest-RAM staging block (baseaddr_mac) it draws through via the
 * GetCGrafPtr CGrafPort, and DSpSyncAltBufferStagingToBacking rebuilds the
 * backing from it. Alt-buffer backings therefore release on background and
 * re-create on foreground exactly like back buffers. Without this, any title
 * holding one alt buffer kept the DSp heap live count nonzero forever - the
 * bump heap could never reset, and every background/foreground cycle leaked
 * a back-buffer-sized region toward the 32 MiB ceiling (CORE-09 ratchet). */
void DSpReleaseAltBufferBackingsForBackground(void)
{
	uint32_t released = 0;
	for (int i = 0; i < DSP_MAX_ALT_BUFFERS; i++) {
		DSpAltBufferRecord *rec = &dsp_alt_buffer_table[i];
		if (!rec->in_use || rec->backing == NULL) continue;
		/* Texture is a view over the backing - drop it first. Guest
		 * staging, dims, and in_use stay intact for foreground restore. */
		rec->texture = NULL;
		gfxaccel_resources_clear_buffer_owner(rec->backing);
		gfxaccel_resources_heap_note_allocation_released(kHeapEngineDSp);
		rec->backing = NULL;
		released++;
	}
	if (released > 0) {
		DSP_LOG("Background: released %u alt-buffer backing(s)", released);
		DSpResetHeapIfIdleAfterAltBufferRelease("background alt-buffer release");
	}
}

void DSpRestoreAltBufferBackingsForForeground(void)
{
	for (int i = 0; i < DSP_MAX_ALT_BUFFERS; i++) {
		DSpAltBufferRecord *rec = &dsp_alt_buffer_table[i];
		if (!rec->in_use || rec->backing != NULL) continue;
		if (rec->width == 0 || rec->height == 0) continue;
		if (!DSpAllocAltBufferBacking(rec, rec->width, rec->height)) {
			/* Leave backing NULL - sync/blit paths NULL-check it; the next
			 * foreground retries, matching the back-buffer policy. */
			DSP_LOG("Foreground: alt-buffer %d backing re-alloc FAILED "
					"(retry on next fg)", i + 1);
			continue;
		}
		/* Repopulate from the guest-RAM staging (source of truth). */
		DSpSyncAltBufferStagingToBacking(rec);
		DSP_LOG("Foreground: alt-buffer %d backing restored (%ux%u)",
				i + 1, rec->width, rec->height);
	}
}

/* Free every in-use alt-buffer record (test-harness context reset). Replaces
 * the inline loop formerly in dsp_testing_reset_contexts. */
void DSpResetAltBufferTable(void)
{
	for (int i = 0; i < DSP_MAX_ALT_BUFFERS; i++) {
		if (dsp_alt_buffer_table[i].in_use) {
			DSpFreeAltBuffer((uint32_t)(i + 1));
		}
	}
	DSpResetHeapIfIdleAfterAltBufferRelease("alt-buffer table reset");
}
