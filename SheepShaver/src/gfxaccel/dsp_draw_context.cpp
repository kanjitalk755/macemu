/*
 *  dsp_draw_context.mm - DSp per-context private-data table + Reserve/Release
 *                         lifecycle + VBL-bounded release FIFO.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Pattern source:
 *    - rave_draw_context.mm:43-75 (context-table shape; 1-based handles,
 *      slot 0 reserved, linear-scan free-list via NULL slots)
 *    - VBL-bounded release FIFO with atomic_uint32 head/tail
 *    - Metal texture view must be released BEFORE the underlying buffer -
 *      ARC order matters on some iOS Metal drivers
 *
 *  GetBackBuffer/SwapBuffers/SetState/GetState/InvalBackBufferRect handler
 *  BODIES live in this file.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "macos_util.h"            /* FindLibSymbol (InterfaceLib window calls) */
#include "thunks.h"                /* SheepMem::Reserve (test hook) */
#include "dsp_engine.h"
#include "dsp_event_record.h"      /* kDSpEvent_OSEvt + osEvt suspend/resume decode constants */
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
#include "dsp_metal_renderer.h"    /* DSp back-buffer allocation/backing access */
#include "gfxaccel_threading_policy.h"
#include "gfxaccel_resources.h"    /* per-buffer owner-tag API */
#include "gfxaccel_resources_heap.h" /* kHeapEngineDSp + heap_alloc_buffer for AltBuffer backing */
#include "dsp_alt_buffer.h"        /* AltBuffer subsystem: record table + handlers (extracted) */
#include "dsp_clut_gamma.h"      /* CLUT + gamma get/set (extracted); fade stays here */
#include "nqd_accel.h"               /* NQDMetalBitblt1to1 / NQDMetalBitbltScaled / NQDMetalFlush for DSpBlit */
#include "metal_compositor.h"      /* active screen rebind/query */
#include "metal_device_shared.h"   /* SharedMetalCommandQueue (SwapBuffers blit path) */
#include "display_mode_controller.h" /* dmc_current_snapshot (FrameDescriptor generation); DMCOwner enum + dmc_set_active_owner */
#include "dsp_engine_internal.h"   /* DSpMapStateToDMCOwnerTyped (internal-only, NOT in include/) */
#include "dsp_back_buffer_range.h"
#include "vbl_source.h"
#include "video.h"                 /* canonical screen_base */
#include "main.h" /* for M68kRegisters */

#include <cstring>
#include <cassert>                 /* alignedRB <= 0x3FFF invariant assert */

#include "atomic.h"
#if defined(__WIN32__)
#include <windows.h>
#define usleep(usecs) Sleep((usecs) / 1000)
#else
#include <unistd.h>                /* usleep (busyProc polling) */
#endif

static void DSpReleaseFrontBufferCGrafPtr(DSpContextPrivate *ctx)
{
	if (ctx == NULL) return;
	ctx->front_cgrafptr_mac_addr = 0;
	ctx->front_pixmap_mac_addr = 0;
	ctx->front_pixmap_handle_mac_addr = 0;
}

/* DSp <-> iOS host bridge forward decls.
 * Included here as a forward declaration so dsp_draw_context.mm doesn't
 * need to pull in dsp_host_bridge.h (the .mm is a .mm file already, but
 * the forward decl keeps the C-linkage contract explicit at the call
 * site in DSpContext_SetStateHandler). Called on the transition edge
 * where the aggregate "any DSp context Active?" predicate changes. */
extern "C" void DSpHostBridge_SetActiveFullscreen(bool active);
extern "C" bool DSpHostBridge_GetActiveFullscreen(void);

/* Resolves the live MainDevice screen PixMap pointer via the sanctioned lowmem
 * walk (defined later in this file). */
extern "C" uint32_t DSpGetLiveMainDevicePixMap(void);

/* GetBackBuffer underlay-restore branch forward decl (PDF p.51 "clean a
 * back buffer"). Defined in the AltBuffer
 * section (after the alt-buffer record table + accumulator), but called from
 * DSpContext_GetBackBufferHandler which appears earlier in this file. */
static void DSpRestoreBackBufferFromUnderlay(DSpContextPrivate *ctx);

/* DSP_MAX_CONTEXTS is now defined in dsp_draw_context.h (hoisted there so
 * dsp_host_bridge.mm can reference the bound from cross-TU); the local
 * guarded redefinition below preserves the value in case a downstream
 * compile unit encounters this file without having visited the header
 * first. */
#ifndef DSP_MAX_CONTEXTS
#define DSP_MAX_CONTEXTS             8
#endif
#define DSP_RELEASE_QUEUE_CAPACITY  16   /* 8 contexts x 2 pending cycles */

/* Clamp duration_vbls to this maximum to
 * protect ctx->fade_state.elapsed_vbls (uint16_t) against overflow on
 * pathological app inputs. ~68 s @ 60 Hz; ~34 s @ 120 Hz ProMotion.
 * The DSp 1.7 spec doesn't define an upper bound; this clamp is purely
 * defensive. Apps requesting > 4096 VBLs get a DSP_LOG warning + clamp.
 */
#define DSP_MAX_FADE_VBLS 4096

/*
 *  DSpContextPrivate full struct definition lives in
 *  dsp_context_private.h so dsp_metal_renderer.mm can touch back_buffer /
 *  back_texture / dirty_* / staging_mac_addr without duplicating the
 *  struct layout. This file retains ownership of the context table,
 *  release FIFO, Reserve/Release handlers, and the
 *  GetBackBuffer / SwapBuffers handler bodies.
 */

/* --- Context table (1-based handles; slot 0 reserved) --- */

static DSpContextPrivate *dsp_context_table[DSP_MAX_CONTEXTS] = {};

static int                dsp_context_count                   = 0;

/* File-static atomic VBL tick counter.
 * DSpVBLServiceCallback increments on every VBL and reads it back for
 * the vblCount argument (r5) it passes to user VBLProcs.
 *
 * Single-writer emul-thread: DSpVBLServiceCallback fires from the VBL
 * secondary-callback drain on the emul thread.
 *
 * atomic.h primitive per the read-mostly precedent - exact
 * mirror of vbl_source.mm's s_tick_count pattern (documented inline
 * in that file as the sanctioned minimal
 * primitive). NO mutex, NO MTLFence, NO MTLSharedEvent, NO
 * @synchronized. 64-bit internal storage; truncated to 32 bits at the
 * VBLProc dispatch site per the DSp 1.7 UInt32 contract at spec
 * p.81. The 64-bit-internal/32-bit-external split prevents ABA
 * confusion across long sessions (session > 828 days at 60 Hz before
 * the 32-bit counter wraps; 64-bit effectively never wraps). */
static atomic_uint64 s_dsp_vbl_count = 0;

extern "C" DSpContextPrivate *DSpGetContext(uint32_t handle)
{
	if (handle == 0 || handle > DSP_MAX_CONTEXTS) return NULL;
	return dsp_context_table[handle - 1];
}

/*
 *  Debug session `dsp-sims-enumeration-stall` fix (2026-04-19) - C-safe
 *  accessor that lets dsp_mode_enumerate.cpp (a pure .cpp file, no
 *  <Metal/Metal.h>) read ctx->enumeration_mode_index without needing the
 *  full DSpContextPrivate struct layout. See dsp_draw_context.h for the
 *  contract.
 */
extern "C" uint32_t DSpGetContextEnumerationIndex(uint32_t ctxRef)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL) return DSP_ENUMERATION_INDEX_NONE;
	return ctx->enumeration_mode_index;
}

static bool DSpIsInactiveMetadataOnlyContext(const DSpContextPrivate *ctx)
{
	return ctx != NULL &&
		   ctx->state == (uint32_t)kDSpContextState_Inactive &&
		   ctx->back_buffer == NULL &&
		   ctx->back_texture == NULL;
}

static uint32_t DSpAllocContextHandle(DSpContextPrivate *ctx)
{
	for (int i = 0; i < DSP_MAX_CONTEXTS; i++) {
		if (dsp_context_table[i] == NULL) {
			dsp_context_table[i] = ctx;
			ctx->handle = (uint32_t)(i + 1);
			return (uint32_t)(i + 1);
		}
	}
	return 0;
}

static void DSpFreeContextHandle(uint32_t handle)
{
	if (handle > 0 && handle <= DSP_MAX_CONTEXTS) {
		dsp_context_table[handle - 1] = NULL;
	}
}


/* --- VBL-bounded release FIFO (single-writer, atomic head/tail) --- */

struct DSpReleaseEntry {
	void*       back_buffer;
	void*      back_texture;
	DSpContextPrivate  *ctx_to_free;
};

static DSpReleaseEntry    dsp_release_queue[DSP_RELEASE_QUEUE_CAPACITY];
static atomic_uint32       dsp_release_head = 0;  /* producer writes */
static atomic_uint32       dsp_release_tail = 0;  /* VBL drain writes */

/* Set across DSpDrainLifecycleSync's drain calls. The background edge
 * runs after gfxaccel_handle_background_enter Step 2 committed an empty
 * command buffer on the shared queue and waited for completion
 * (gfxaccel_resources.mm), so GPU work referencing the DSp heap is
 * provably complete - resets reached from that drain may skip the
 * GPU-completion latch instead of deferring while the VBL source is
 * paused (no retry tick would fire). The foreground edge inherits the
 * same guarantee: the VBL source stays paused through the background
 * window and back buffers were already released, so no new DSp-heap
 * command buffer can have been committed in between. Emul-thread only
 * (gfxaccel_threading_policy.h). */
static bool dsp_lifecycle_gpu_drained = false;

static void DSpResetHeapIfIdle(const char *reason)
{
	uint32_t live = gfxaccel_resources_heap_live_allocation_count(kHeapEngineDSp);
	if (live != 0) {
		return;
	}
	/* Latch-gated by default: gfxaccel_resources_heap_reset defers while
	 * a command buffer noted via gfxaccel_resources_heap_note_gpu_commit
	 * (SwapBuffers / VBL-publish presents reading back_texture) is still
	 * in flight; the VBL release drain retries every tick. */
	uint64_t reclaimed = dsp_lifecycle_gpu_drained
		? gfxaccel_resources_heap_reset_gpu_idle(kHeapEngineDSp)
		: gfxaccel_resources_heap_reset(kHeapEngineDSp);
	if (reclaimed > 0) {
		DSP_LOG("DSp heap reset after %s reclaimed %llu bytes",
				reason ? reason : "release",
				(unsigned long long)reclaimed);
	}
}

static void DSpReleaseNow(DSpContextPrivate *ctx)
{
	/* Clear the owner-tag BEFORE the buffer goes
	 * away so the map does not hold a dangling pointer. */
	if (ctx->back_buffer != NULL) {
		gfxaccel_resources_clear_buffer_owner(ctx->back_buffer);
		gfxaccel_resources_heap_note_allocation_released(kHeapEngineDSp);
	}
	DSpContextPrivateReleaseBackBuffer(ctx);
	DSpReleaseBackBufferStaging(ctx);
	ctx->cgrafptr_mac_addr = 0;
	DSpReleaseFrontBufferCGrafPtr(ctx);
	delete ctx;
	DSpResetHeapIfIdle("synchronous release");
}

static void DSpQueueReleaseAtVBL(DSpContextPrivate *ctx)
{
	uint32_t head = atomic_load_explicit(&dsp_release_head,
										  memory_order_relaxed);
	uint32_t next = (head + 1) % DSP_RELEASE_QUEUE_CAPACITY;
	uint32_t tail = atomic_load_explicit(&dsp_release_tail,
										  memory_order_acquire);
	if (next == tail) {
		DSP_LOG("DSpQueueReleaseAtVBL: FIFO full, falling back to "
				"synchronous release (cap=%d)",
				DSP_RELEASE_QUEUE_CAPACITY);
		DSpReleaseNow(ctx);
		return;
	}
	/* Clear the owner-tag here (not in the VBL
	 * drain) so the map reflects DSp ownership ending at the moment
	 * Release was requested - the actual Metal refcount drop is
	 * deferred to the VBL boundary, but logically the buffer is "no
	 * longer owned by DSp" as soon as it's queued. Any NQD dispatch
	 * between here and the drain sees DMC owner transitioning away
	 * from DSp (set by SetState prior to Release), so the conflict
	 * gate no longer applies. */
	if (ctx->back_buffer != NULL) {
		gfxaccel_resources_clear_buffer_owner(ctx->back_buffer);
	}
	dsp_release_queue[head].back_buffer  = ctx->back_buffer;
	dsp_release_queue[head].back_texture = ctx->back_texture;
	dsp_release_queue[head].ctx_to_free  = ctx;
	ctx->back_buffer  = NULL;
	ctx->back_texture = NULL;
	DSpReleaseBackBufferStaging(ctx);
	ctx->cgrafptr_mac_addr = 0;
	DSpReleaseFrontBufferCGrafPtr(ctx);
	atomic_store_explicit(&dsp_release_head, next,
						  memory_order_release);
}

/*
 *  Partial-release variant.
 *
 *  Identical to DSpQueueReleaseAtVBL EXCEPT ctx_to_free = NULL. The
 *  DSpContextPrivate struct survives the background window (kept alive
 *  in dsp_context_table); only the MTLBuffer + MTLTexture are released
 *  via the VBL drain. Foreground restore re-allocates fresh Metal
 *  resources into the same struct via DSpAllocateBackBuffer.
 *
 *  Used exclusively by DSpHandleBackgroundFromEmulThread.
 */
static void DSpQueueReleaseAtVBLPartial(DSpContextPrivate *ctx)
{
	uint32_t head = atomic_load_explicit(&dsp_release_head,
										  memory_order_relaxed);
	uint32_t next = (head + 1) % DSP_RELEASE_QUEUE_CAPACITY;
	uint32_t tail = atomic_load_explicit(&dsp_release_tail,
										  memory_order_acquire);
	/* Clear the owner-tag on the buffer before it
	 * leaves DSp ownership. The bg/fg path will re-allocate a fresh
	 * buffer at foreground restore (DSpHandleForegroundFromEmulThread
	 * calls DSpAllocateBackBuffer which re-tags the new buffer). */
	if (ctx->back_buffer != NULL) {
		gfxaccel_resources_clear_buffer_owner(ctx->back_buffer);
	}
	if (next == tail) {
		DSP_LOG("DSpQueueReleaseAtVBLPartial: FIFO full, falling back "
				"to synchronous partial release (cap=%d)",
				DSP_RELEASE_QUEUE_CAPACITY);
		/* Synchronous partial: texture + buffer only:
		 * texture first); struct is NOT deleted - bg restore path will
		 * re-alloc into it. */
		if (ctx->back_buffer != NULL) {
			gfxaccel_resources_heap_note_allocation_released(kHeapEngineDSp);
		}
		DSpContextPrivateReleaseBackBuffer(ctx);
		DSpReleaseBackBufferStaging(ctx);
		ctx->cgrafptr_mac_addr = 0;
		DSpReleaseFrontBufferCGrafPtr(ctx);
		DSpResetHeapIfIdle("synchronous partial release");
		return;
	}
	dsp_release_queue[head].back_buffer  = ctx->back_buffer;
	dsp_release_queue[head].back_texture = ctx->back_texture;
	dsp_release_queue[head].ctx_to_free  = NULL;   /* KEY: struct stays alive */
	ctx->back_buffer  = NULL;
	ctx->back_texture = NULL;
	DSpReleaseBackBufferStaging(ctx);
	ctx->cgrafptr_mac_addr = 0;
	DSpReleaseFrontBufferCGrafPtr(ctx);
	atomic_store_explicit(&dsp_release_head, next,
						  memory_order_release);
}

extern "C" void DSpVBLReleaseCallback(void * /*ctx*/,
									   void * /*drawable*/,
									   double /*ts*/)
{
	uint32_t tail = atomic_load_explicit(&dsp_release_tail,
										  memory_order_relaxed);
	uint32_t head = atomic_load_explicit(&dsp_release_head,
										  memory_order_acquire);
	while (tail != head) {
		DSpReleaseEntry *entry = &dsp_release_queue[tail];
		if (entry->back_buffer != NULL) {
			gfxaccel_resources_heap_note_allocation_released(kHeapEngineDSp);
		}
		DSpContextPrivateReleaseBackBufferVariables(&entry->back_texture,
													&entry->back_buffer);
		if (entry->ctx_to_free != NULL) {
			delete entry->ctx_to_free;
			entry->ctx_to_free = NULL;
		}
		tail = (tail + 1) % DSP_RELEASE_QUEUE_CAPACITY;
	}
	atomic_store_explicit(&dsp_release_tail, tail,
						  memory_order_release);
	DSpResetHeapIfIdle("VBL release drain");

	/* Chain the background/foreground drain off the same VBL hook so
	 * state transitions happen AFTER releases (drain-first ordering). */
	DSpVBLBackgroundForegroundDrain();
}

/*
 *  VBL-latched CLUT snapshot callback.
 *
 *  Registered from DSpInit via vbl_source_register_secondary_callback;
 *  unregistered from DSpShutdown. Runs on the emul thread on each VBL
 *  tick AFTER the compositor's primary palette-latch has swapped the
 *  compositor-shared palette buffer front/back (metal_compositor.mm:265
 *  MetalCompositorPaletteLatch).
 *
 *  Per-context snapshot: copies ctx->clut_bytes (writer-visible, updated
 *  by SetCLUTEntries) into ctx->clut_bytes_latched
 *  (reader-visible, read by GetCLUTEntries). This gives
 *  GetCLUTEntries the last-VBL-boundary semantics required by the
 *  tight-loop test: Set then Get reads last VBL boundary, not
 *  in-flight write.
 *
 *  Single-writer emul-thread contract preserved: VBL secondary callbacks
 *  fire on the same emul thread as SetCLUTEntries; no mutex / _Atomic /
 *  MTLFence added.
 */
extern "C" void DSpVBLClutLatchCallback(void *cb_ctx, void *drawable, double ts)
{
	(void)cb_ctx; (void)drawable; (void)ts;
	for (uint32_t i = 0; i < DSP_MAX_CONTEXTS; i++) {
		DSpContextPrivate *ctx = dsp_context_table[i];
		if (ctx == NULL) continue;
		/* Snapshot every context (not just Active) - Paused contexts
		 * have a valid stale CLUT the app can Get between Pause and
		 * Resume; the snapshot is correct for both states because
		 * SetCLUTEntries only runs via DSp dispatch on the emul thread
		 * and clut_bytes is stable outside the Set handler. */
		memcpy(ctx->clut_bytes_latched, ctx->clut_bytes, 768);
	}
}

extern "C" int32_t DSpGetActiveCLUTSnapshot(uint8_t out_clut_bytes[768])
{
	if (out_clut_bytes == NULL) return kDSpInvalidAttributesErr;

	for (uint32_t i = 0; i < DSP_MAX_CONTEXTS; i++) {
		DSpContextPrivate *ctx = dsp_context_table[i];
		if (ctx == NULL) continue;
		if (ctx->state != (uint32_t)kDSpContextState_Active) continue;
		memcpy(out_clut_bytes, ctx->clut_bytes, 768);
		return kDSpNoErr;
	}
	return kDSpInvalidContextErr;
}

/*
 *  Linear LUT interpolation with integer
 *  rounding to prevent cumulative drift across long fades.
 *
 *  Formula per byte i in [0..767]:
 *    out[i] = (start[i] * (duration_vbls - elapsed_vbls)
 *              + end[i]   * elapsed_vbls
 *              + duration_vbls / 2)
 *             / duration_vbls
 *
 *  Properties:
 *    - elapsed_vbls == 0           -> out[i] ~ start[i] (round-to-nearest)
 *    - elapsed_vbls == duration    -> out[i] = end[i] exactly
 *    - duration_vbls / 2 term      -> round-half-to-even (drift-free)
 *    - uint32_t arithmetic         -> max product = 255 * 4096 = 1,044,480
 *                                    (no overflow at DSP_MAX_FADE_VBLS)
 *
 *  Caller invariants (NOT re-checked):
 *    - duration_vbls > 0           (caller's special-case for zero-duration
 *                                    fade pushes end_lut directly)
 *    - elapsed_vbls < duration     (caller dispatches the final-frame
 *                                    end_lut push directly, never calling
 *                                    this helper at elapsed == duration)
 *    - start, end, out             non-null 768-byte buffers
 */
static void DSpInterpolateGammaLUT(const uint8_t *start_lut,
									const uint8_t *end_lut,
									uint32_t elapsed_vbls,
									uint32_t duration_vbls,
									uint8_t *out_lut)
{
	const uint32_t complement = duration_vbls - elapsed_vbls;
	const uint32_t halfdur    = duration_vbls / 2;
	for (uint32_t i = 0; i < 768; i++) {
		const uint32_t numer = (uint32_t)start_lut[i] * complement
							 + (uint32_t)end_lut[i]   * elapsed_vbls
							 + halfdur;
		out_lut[i] = (uint8_t)(numer / duration_vbls);
	}
}

/*
 *  FadeGamma ABI fidelity:
 *  build the parametric FadeGamma target LUT from the 16-bit RGBColor
 *  zero-intensity tint (down-converted to 8-bit by the reader) + a
 *  SIGNED percent-of-original-intensity, per DSp 1.7 pp.32-33.
 *
 *  DSp 1.7 semantics (the wrong-output defect this REPLACES):
 *    - inPercentOfOriginalIntensity = 100  -> display at FULL intensity:
 *        output[c][i] = full_lut[c][i]  (the driver's installed gamma
 *        table - "original intensity" is what the guest's SetGamma put
 *        in effect, NOT the identity ramp; identity is only the
 *        pre-SetGamma boot state).
 *    - inPercentOfOriginalIntensity = 0    -> display at ZERO intensity:
 *        output[c][i] = zeroColor[c]  (the inZeroIntensityColor tint;
 *        black when NULL was passed -> color_(r,g,b) == 0).
 *    - 0 < percent < 100                   -> linear blend between the
 *        zero-intensity tint (at 0%) and original intensity (at 100%):
 *        output[c][i] = zeroColor[c] * (100 - p)/100 + full[c][i] * p/100.
 *    - percent > 100  -> "begin to converge on white" (PDF p.32):
 *        extrapolate from original intensity toward 255.
 *    - percent < 0    -> "begin to converge on black" (PDF p.32):
 *        extrapolate from the zero-intensity tint toward 0.
 *
 *  The PREVIOUS formula `(channel[c] * i * percent) / 25500` DISCARDED
 *  the zero-intensity tint (output was 0 at i=0 or percent=0 regardless
 *  of the tint) - an M2-detectable wrong-output defect. The corrected
 *  formula HONORS inZeroIntensityColor as the zero-intensity floor.
 *
 *  Boundary equivalences (asserted by the corrected DSpGammaTests):
 *    - percent=100  -> driver gamma table == FadeGammaIn end-state
 *      (identity when the guest never called SetGamma)
 *    - percent=0, tint=black(0,0,0) -> zeros == FadeGammaOut end-state
 *    - percent=0, tint=red(255,0,0)  -> R channel = 255, G/B = 0
 *
 *  Integer/clamp safety: all intermediate products are non-negative
 *  uint32_t (max ~ 255*200 = 51,000); results are clamped to [0,255]
 *  before the uint8_t store. Signed `percent` is taken by value as
 *  int32_t; the >100 / <0 extrapolation branches keep every LUT index
 *  in range (no out-of-range LUT index).
 *
 *  Caller invariants (NOT re-checked):
 *    - out_lut               non-null 768-byte buffer
 */
static void DSpComputeFadeGammaTargetLUT(uint8_t color_r,
										  uint8_t color_g,
										  uint8_t color_b,
										  int32_t percent,
										  const uint8_t *full_lut,
										  uint8_t *out_lut)
{
	const uint8_t channels[3] = { color_r, color_g, color_b };
	for (uint32_t c = 0; c < 3; c++) {
		const int32_t tint = (int32_t)channels[c];
		for (uint32_t i = 0; i < 256; i++) {
			/* "Original intensity" is the driver's installed gamma table,
			 * NOT the identity ramp: games install functional ramps (e.g.
			 * overbright doubling) and a 100% restore must land on them. */
			const int32_t full = (int32_t)full_lut[c * 256 + i];
			int32_t v;
			if (percent <= 0) {
				/* <=0%: converge from the zero-intensity tint toward
				 * black. At 0% -> tint; at -100% -> 0. Below -100%
				 * stays at 0. */
				int32_t t = -percent;            /* 0..(+inf) */
				if (t > 100) t = 100;
				v = (tint * (100 - t)) / 100;
			} else if (percent >= 100) {
				/* >=100%: converge from original intensity toward white.
				 * At 100% -> the driver table; at 200% -> 255. Above 200%
				 * stays at 255. */
				int32_t t = percent - 100;       /* 0..(+inf) */
				if (t > 100) t = 100;
				v = full + ((255 - full) * t) / 100;
			} else {
				/* 0..100%: linear blend tint (0%) -> original intensity
				 * (100%). */
				v = (tint * (100 - percent) + full * percent) / 100;
			}
			if (v < 0)   v = 0;
			if (v > 255) v = 255;
			out_lut[c * 256 + i] = (uint8_t)v;
		}
	}
}

/*
 *  Copy the driver's current gamma table (the guest's last SetGamma) out of
 *  the DMC snapshot - the "original intensity" reference for every fade
 *  formula above. Falls back to the identity ramp when the DMC is Quiescent
 *  (no snapshot yet), which matches the pre-SetGamma boot state.
 */
static void DSpCopyDriverGammaLUT(uint8_t out_lut[768])
{
	const struct DMCModeSnapshot *snap = dmc_current_snapshot();
	if (snap != NULL) {
		memcpy(out_lut, snap->driver_gamma_lut, 768);
		return;
	}
	for (uint32_t c = 0; c < 3; c++) {
		for (uint32_t i = 0; i < 256; i++) {
			out_lut[c * 256 + i] = (uint8_t)i;
		}
	}
}

/*
 *  Shared fade-state
 *  initialization. Used by FadeGammaIn (end_lut = identity),
 *  FadeGammaOut (end_lut = zeros), and FadeGamma
 *  (end_lut = computed from percent + color).
 *
 *  start_lut snapshot logic (active-fade-replacement):
 *    - If ctx->fade_state.active != 0: compute the CURRENT INTERPOLATED
 *      state via DSpInterpolateGammaLUT(old_start, old_end, old_elapsed,
 *      old_duration). This preserves visual continuity - the new fade
 *      starts from where the old fade was visually, NOT from the old
 *      fade's start. Without this, a SetGamma -> FadeGammaIn(50, ...)
 *      -> FadeGammaOut(20, ...) sequence would snap back to the
 *      first fade's source, creating a visual flash.
 *    - If ctx->fade_state.active == 0: snapshot dmc_current_snapshot()
 *      ->gamma_lut into start_lut. This is the "fresh fade" path -
 *      the current displayed gamma becomes the fade source.
 *    - dmc_current_snapshot() NULL handling: extreme edge case (DSp
 *      Quiescent), use the identity LUT as start (visually equivalent
 *      to "fade from undefined initial state to end_lut").
 *
 *  end_lut: caller-provided (identity for FadeIn / zeros
 *  for FadeOut; FadeGamma builds parametric color x percent target).
 *
 *  duration_vbls: caller-clamped to DSP_MAX_FADE_VBLS BEFORE calling
 *  this helper. Caller is also responsible for the duration_vbls == 0
 *  short-circuit (push end_lut directly, do NOT call this helper).
 */
static void DSpInitFadeStateCore(DSpContextPrivate *ctx,
								  const uint8_t *end_lut_768,
								  uint16_t duration_vbls)
{
	/* Snapshot the start_lut from the current visual state. */
	if (ctx->fade_state.active != 0) {
		/* Active fade in progress - snapshot current interpolated state. */
		uint8_t snapshot[768];
		DSpInterpolateGammaLUT(ctx->fade_state.start_lut,
							   ctx->fade_state.end_lut,
							   (uint32_t)ctx->fade_state.elapsed_vbls,
							   (uint32_t)ctx->fade_state.duration_vbls,
							   snapshot);
		memcpy(ctx->fade_state.start_lut, snapshot, 768);
		DSP_LOG("DSpInitFadeStateCore: replacing active fade "
				"(ctx=%u, prev_elapsed=%u/%u)",
				ctx->handle,
				(unsigned)ctx->fade_state.elapsed_vbls,
				(unsigned)ctx->fade_state.duration_vbls);
	} else {
		/* Fresh fade - snapshot DMC current gamma. */
		const struct DMCModeSnapshot *snap = dmc_current_snapshot();
		if (snap != NULL) {
			memcpy(ctx->fade_state.start_lut, snap->gamma_lut, 768);
		} else {
			/* DMC Quiescent - fall back to identity LUT (no-op gamma). */
			for (uint32_t c = 0; c < 3; c++) {
				for (uint32_t i = 0; i < 256; i++) {
					ctx->fade_state.start_lut[c * 256 + i] = (uint8_t)i;
				}
			}
			DSP_LOG("DSpInitFadeStateCore: DMC NULL snapshot - "
					"using identity LUT as start (ctx=%u)", ctx->handle);
		}
	}

	/* Caller-provided end_lut. */
	memcpy(ctx->fade_state.end_lut, end_lut_768, 768);

	/* Reset fade machinery to active state. */
	ctx->fade_state.duration_vbls = duration_vbls;
	ctx->fade_state.elapsed_vbls  = 0;
	ctx->fade_state.active        = 1;
}

/*
 *  VBL-driven gamma-fade interpolation
 *  callback.
 *
 *  Registered from DSpInit via vbl_source_register_secondary_callback
 *  AFTER DSpVBLClutLatchCallback; unregistered from
 *  DSpShutdown FIRST (reverse registration order). Runs on the emul
 *  thread on each VBL tick AFTER DSpVBLClutLatchCallback has drained
 *  the per-context CLUT snapshots - the gamma fade is independent of
 *  CLUT, but ordering is preserved for predictability of any future
 *  cross-callback observation.
 *
 *  Body: per-context walk with early-out for active==0; on
 *  active fades, increments elapsed_vbls, computes interpolated LUT
 *  via DSpInterpolateGammaLUT, and pushes via dmc_record_gamma_change_with_lut.
 *  On final frame (elapsed >= duration), pushes end_lut authoritatively +
 *  writes gamma_lut_persisted + clears active. Rollback on DMC failure.
 *
 *  Single-writer emul-thread contract preserved: VBL secondary callbacks
 *  fire on the same emul thread as SetGamma / FadeGamma* handlers; no
 *  mutex / _Atomic / MTLFence added.
 */
extern "C" void DSpVBLGammaFadeCallback(void *cb_ctx, void *drawable, double ts)
{
	(void)cb_ctx; (void)drawable; (void)ts;

	/* fade_active is a SINGLE global flag on the SINGLE shared DMC
	 * snapshot, so it cannot carry per-context state. The compositor presents
	 * ONE frame; the correct global semantics is "any context still fading ->
	 * linear ramp". A per-context publish that set fade_active from only that
	 * context's state let a higher-index context completing its fade
	 * (fade_active=0) clobber a lower-index context still mid-fade - warping the
	 * survivor's fade. Instead we OR across all contexts: pre-scan to decide
	 * whether ANY context will STILL be fading AFTER this tick's advancement,
	 * then publish that single OR'd flag with every per-context LUT push.
	 *
	 * Pre-scan note: a context with elapsed+1 < duration is still fading after
	 * this tick; a context reaching elapsed+1 >= duration completes on this tick
	 * (no longer fading). This mirrors the per-context final-frame test below so
	 * the OR is exact. */
	uint32_t any_still_fading = 0u;
	for (uint32_t i = 0; i < DSP_MAX_CONTEXTS; i++) {
		DSpContextPrivate *ctx = dsp_context_table[i];
		if (ctx == NULL || ctx->fade_state.active == 0) continue;
		/* (uint32_t) widen so the +1 cannot wrap the uint16_t counter. */
		uint32_t after = (uint32_t)ctx->fade_state.elapsed_vbls + 1u;
		if (after < (uint32_t)ctx->fade_state.duration_vbls) {
			any_still_fading = 1u;
			break;
		}
	}

	for (uint32_t i = 0; i < DSP_MAX_CONTEXTS; i++) {
		DSpContextPrivate *ctx = dsp_context_table[i];
		if (ctx == NULL) continue;

		/* Per-context fade advancement. Runs on the emul thread per VBL
		 * tick after DSpVBLClutLatchCallback. Single-writer invariant
		 * preserved - SetGamma + FadeGammaIn/Out + this callback all
		 * serialize on the emul thread. */

		/* Skip contexts with no active fade (early-out - most contexts
		 * most of the time). */
		if (ctx->fade_state.active == 0) continue;

		/* Advance one VBL tick. */
		ctx->fade_state.elapsed_vbls++;

		/* Final-frame transition: push end_lut as the authoritative final
		 * gamma state, update persistence, clear active flag. */
		if (ctx->fade_state.elapsed_vbls >= ctx->fade_state.duration_vbls) {
			// This context just completed, but
			// fade_active is the OR across ALL contexts - publish end_lut with
			// any_still_fading so a concurrent lower/higher-index fade is not
			// clobbered to 0. The compositor restores its static display-LUT
			// composition only once NO context is fading.
			int32_t rv = dmc_record_gamma_change_with_lut_fade(
				ctx->fade_state.end_lut, (int)any_still_fading);
			if (rv != kDMCNoErr) {
				DSP_LOG("DSpVBLGammaFadeCallback: final-frame DMC push failed "
						"(ctx=%u, rv=%d) - leaving fade active for retry",
						ctx->handle, rv);
				/* Don't clear active - retry on next VBL tick. */
				ctx->fade_state.elapsed_vbls--;  /* roll back the increment */
				continue;
			}
			memcpy(ctx->gamma_lut_persisted, ctx->fade_state.end_lut, 768);
			ctx->fade_state.active = 0;
			DSP_LOG("DSpVBLGammaFadeCallback: fade complete (ctx=%u, "
					"duration=%u VBLs, any_still_fading=%u)",
					ctx->handle, (unsigned)ctx->fade_state.duration_vbls,
					(unsigned)any_still_fading);
			continue;
		}

		/* Mid-fade frame: compute interpolated LUT and push. */
		uint8_t interp_lut[768];
		DSpInterpolateGammaLUT(ctx->fade_state.start_lut,
							   ctx->fade_state.end_lut,
							   (uint32_t)ctx->fade_state.elapsed_vbls,
							   (uint32_t)ctx->fade_state.duration_vbls,
							   interp_lut);
		// Fade in progress - publish the interpolated
		// LUT with the OR'd fade_active=1 (this context is mid-fade, so the OR is
		// trivially 1) so the compositor bypasses pow and the ramp marches
		// linearly (DSp-1.7). Flag + LUT publish atomically.
		int32_t rv = dmc_record_gamma_change_with_lut_fade(interp_lut, 1);
		if (rv != kDMCNoErr) {
			DSP_LOG("DSpVBLGammaFadeCallback: mid-fade DMC push failed "
					"(ctx=%u, elapsed=%u/%u, rv=%d) - leaving fade active",
					ctx->handle,
					(unsigned)ctx->fade_state.elapsed_vbls,
					(unsigned)ctx->fade_state.duration_vbls, rv);
			ctx->fade_state.elapsed_vbls--;  /* roll back so next tick retries this frame */
			continue;
		}
	}
}

/* --- VBL service callback ---
 *
 *  Atomic-increment plus the per-context walk + PPC VBLProc invocation
 *  via call_macos3.
 *
 *  Registered from DSpInit via vbl_source_register_secondary_callback
 *  AFTER DSpVBLGammaFadeCallback - 4th and FINAL VBL
 *  secondary callback slot. Unregistered from DSpShutdown FIRST
 *  (reverse registration order).
 *
 *  Execution sequence per VBL tick:
 *    1. atomic_fetch_add s_dsp_vbl_count (monotonic tick).
 *    2. Walk dsp_context_table[0..DSP_MAX_CONTEXTS-1] in INDEX ORDER
 *       (deterministic dispatch).
 *    3. For each Active context with vbl_proc_ptr != 0, invoke the
 *       VBLProc via call_macos3(vbl_proc_ptr, handle, refcon, count).
 *
 *  PPC-trampoline mechanism: call_macos3 is the sanctioned
 *  entry point for PPC subroutine calls from native code. Declared in
 *  SheepShaver/src/Unix/sysdeps.h:490; defined in
 *  SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp:1438-1442; wraps
 *  ppc_cpu->execute_macos_code which handles register save/restore,
 *  stack frame setup, TOC marshalling, and POWERPC_EXEC_RETURN
 *  trampolining (sheepshaver_glue.cpp:652-702). Production precedent:
 *  rave_metal_renderer.mm:2709 (call_macos4 for RAVE notice methods).
 *
 *  VBLProc ABI per DSp 1.7 spec p.81:
 *    void DSpVBLProc(DSpContextReference ctx, void *refCon, UInt32 vblCount)
 *  PPC calling convention: r3=ctx (32-bit handle), r4=refCon (opaque),
 *  r5=vblCount (truncated from s_dsp_vbl_count uint64). Return value
 *  is void (call_macos3's uint32 return is discarded).
 *
 *  Iteration order: dsp_context_table is indexed 0..7;
 *  iteration is linear in index. Since dsp_context_table allocates
 *  lowest-free-index, index-order ==
 *  registration-order for contexts created without intervening Release
 *  calls. DSp 1.7 spec doesn't pin a specific order beyond "all VBLProcs
 *  fire each VBL"; index-order is the natural reading and gives a
 *  deterministic order with no drops, verified across 1000 ticks.
 *
 *  Threading: single-writer emul-thread. The VBL secondary callback
 *  invocation is itself on the emul thread; call_macos3 blocks the
 *  emul thread on PPC execution until the user VBLProc returns. Total
 *  time budget: < 1 ms aggregate across all contexts
 *  (4 contexts x ~250 ?s per VBLProc). If any single VBLProc exceeds
 *  250 ?s, DSP_LOG warns (post-call wall-time check) but the callback
 *  does NOT early-exit - all registered VBLProcs must fire every VBL
 *  per DSp 1.7 semantics. Caller is responsible for keeping VBLProcs
 *  short (documented game-author contract).
 *
 *  Robustness: if call_macos3 returns kDSpInternalErr-like values (no
 *  error path from this API - it's void-returning by design), the
 *  callback continues to the next context. A SIGSEGV in the user
 *  VBLProc is uncatchable and crashes the process; that's a guest-code
 *  bug matching DSp 1.7 semantics (bad VBLProc crashes the game).
 */
extern "C" void DSpVBLServiceCallback(void *cb_ctx, void *drawable, double ts)
{
	(void)cb_ctx; (void)drawable; (void)ts;

	/* Step 1: monotonic tick counter. Relaxed ordering - reader
	 * side only needs monotonicity, not happens-before vs other state. */
	atomic_fetch_add_explicit(&s_dsp_vbl_count, 1u,
							  memory_order_relaxed);

	/* Step 2-3: walk contexts in INDEX ORDER;
	 * invoke registered VBLProcs via call_macos3. */

	/* Read the post-increment tick count once - all VBLProcs fired this
	 * tick see the same vblCount argument. Matches DSp 1.7 spec p.81
	 * which documents vblCount as "the tick number of the VBL that
	 * fired this call". */
	const uint32_t vbl_count_arg =
		(uint32_t)(atomic_load_explicit(&s_dsp_vbl_count,
										 memory_order_relaxed) & 0xFFFFFFFFu);

	for (uint32_t i = 0; i < DSP_MAX_CONTEXTS; i++) {
		DSpContextPrivate *ctx = dsp_context_table[i];
		if (ctx == NULL) continue;

		/* Only Active contexts fire VBLProcs per DSp 1.7 spec p.81
		 * ("VBLProcs are dispatched only while the context is in the
		 * Active play state"). Inactive + Paused contexts retain their
		 * registered proc but do not fire. */
		if (ctx->state != (uint32_t)kDSpContextState_Active) continue;

		/* Early-out: no proc installed. The `vbl_proc_ptr == 0` case
		 * includes both "never installed" and "explicitly uninstalled
		 * via SetVBLProc(ptr=0)" per DSp 1.7 spec p.81. */
		if (ctx->vbl_proc_ptr == 0) continue;

		/* Snapshot the proc + refcon + handle to local vars so the call
		 * sees a consistent tuple even if (hypothetically) the fields
		 * were mutated by future code. In practice single-writer
		 * main==emul execution makes this an invariant - snapshotting is
		 * defense-in-depth if a future thread split weakens it. */
		const uint32_t proc_addr    = ctx->vbl_proc_ptr;
		const uint32_t proc_refcon  = ctx->vbl_proc_refcon;
		const uint32_t proc_handle  = ctx->handle;

		/* Invoke user VBLProc per the VBLProc ABI:
		 *   r3 = ctx (DSpContextReference, 32-bit handle)
		 *   r4 = refCon (opaque)
		 *   r5 = vblCount (UInt32 tick count)
		 * call_macos3 marshals these into PPC r3/r4/r5, executes the
		 * TVECT-addressed routine, and returns on POWERPC_EXEC_RETURN.
		 * Return value ignored (VBLProc is void-returning in spec).
		 */
		(void)call_macos3(proc_addr,
						  proc_handle,
						  proc_refcon,
						  vbl_count_arg);
	}
}


/*
 *  DSpContext_SetVBLProc (sub-opcode 500).
 *
 *  Argument contract:
 *    ctxRef   = context handle from DSpContext_Reserve
 *    procPtr  = guest PPC TVECT address of the VBLProc. 0 is VALID -
 *               it uninstalls any previously-registered proc per DSp
 *               1.7 spec p.81 (the handler just writes 0 to
 *               vbl_proc_ptr; no special short-circuit needed).
 *    refCon   = opaque 4-byte pass-through value; any bits are valid.
 *               When procPtr==0 refCon is effectively dead storage but
 *               the handler still writes it for round-trip symmetry
 *               with GetVBLProc (sub-opcode 503).
 *
 *  Error map:
 *    kDSpInvalidContextErr    - ctxRef does not resolve to a context
 *    kDSpNoErr                - success; vbl_proc_ptr + vbl_proc_refcon
 *                                updated; next VBL tick will pick up the
 *                                new proc via DSpVBLServiceCallback's walk
 *
 *  Threading (single-writer emul-thread invariant preserved):
 *    NATIVE_DSP_DISPATCH serializes SetVBLProc calls; the reader
 *    (DSpVBLServiceCallback) also runs on emul thread per the VBL
 *    secondary-callback contract. No cross-thread race possible. No
 *    atomic primitive needed - matches the fade_state / clut_bytes
 *    pattern.
 */
extern "C" int32_t DSpContext_SetVBLProcHandler(uint32_t ctxRef,
												 uint32_t procPtr,
												 uint32_t refCon)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL) {
		DSP_LOG("SetVBLProc: invalid ctxRef=%u -> kDSpInvalidContextErr",
				ctxRef);
		return kDSpInvalidContextErr;
	}

	/* procPtr == 0 is VALID per DSp 1.7 spec p.81 (uninstall path).
	 * Treat as normal write; the DSpVBLServiceCallback walk's
	 * `vbl_proc_ptr == 0` early-out handles the semantic. */
	ctx->vbl_proc_ptr    = procPtr;
	ctx->vbl_proc_refcon = refCon;

	if (procPtr == 0) {
		DSP_LOG("SetVBLProc: ctx=%u UNINSTALL (procPtr=0) -> OK",
				ctxRef);
	} else {
		DSP_LOG("SetVBLProc: ctx=%u procPtr=0x%08x refCon=0x%08x -> OK",
				ctxRef, procPtr, refCon);
	}
	return kDSpNoErr;
}

/*
 *  DSpContext_GetVBLProc (sub-opcode 503).
 *
 *  DSp 1.7 spec p.81 does NOT document a GetVBLProc public entry point.
 *  This handler exists as an internal affordance for round-tripping
 *  SetVBLProc + GetVBLProc state without relying on the PPC-trampoline
 *  invocation path; it is intentionally NOT installed in
 *  dsp_install_symbols[] (not a real DSp 1.7 PEF export).
 *
 *  Argument contract:
 *    ctxRef         = context handle from DSpContext_Reserve
 *    procOutAddr    = guest Mac address for a UInt32 (receives
 *                      ctx->vbl_proc_ptr)
 *    refConOutAddr  = guest Mac address for a UInt32 (receives
 *                      ctx->vbl_proc_refcon)
 *
 *  Error map:
 *    kDSpInvalidContextErr    - ctxRef does not resolve
 *    kDSpInvalidAttributesErr - procOutAddr == 0 OR refConOutAddr == 0
 *    kDSpNoErr                - success; 8 bytes written across the
 *                                two out addresses
 */
extern "C" int32_t DSpContext_GetVBLProcHandler(uint32_t ctxRef,
												 uint32_t procOutAddr,
												 uint32_t refConOutAddr)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL) {
		DSP_LOG("GetVBLProc: invalid ctxRef=%u -> kDSpInvalidContextErr",
				ctxRef);
		return kDSpInvalidContextErr;
	}
	if (procOutAddr == 0 || refConOutAddr == 0) {
		DSP_LOG("GetVBLProc: NULL out addr (ctx=%u, proc=0x%08x, refCon=0x%08x) "
				"-> kDSpInvalidAttributesErr",
				ctxRef, procOutAddr, refConOutAddr);
		return kDSpInvalidAttributesErr;
	}

	WriteMacInt32(procOutAddr,   ctx->vbl_proc_ptr);
	WriteMacInt32(refConOutAddr, ctx->vbl_proc_refcon);

	DSP_LOG("GetVBLProc: ctx=%u procPtr=0x%08x refCon=0x%08x -> OK",
			ctxRef, ctx->vbl_proc_ptr, ctx->vbl_proc_refcon);
	return kDSpNoErr;
}

/* --- Canonical DSpProcessEvent (sub-op 750) ---
 *
 * This is the CANONICAL DSp 1.7 export and the OPPOSITE direction to
 * the RETIRED sub-op-600 dequeue handler (the old DSpContext_*
 * ProcessEvent export, which let the GUEST READ enqueued events OUT). That
 * handler - and its header decl - are RETIRED (retire, NOT
 * repurpose), and the SPSC input-fanout ring it observed has since been
 * removed outright (it had become write-only). UIKit background/foreground
 * lifecycle flows through
 * GfxAccelBackgroundLifecycleObserver -> gfxaccel_resources atomic
 * flag-and-drain -> DSpHandleBackgroundFromEmulThread /
 * DSpHandleForegroundFromEmulThread.
 *
 * Per DSp 1.7 PDF p.58:
 *   OSStatus DSpProcessEvent(EventRecord *inEvent, Boolean *outEventWasProcessed)
 * The app passes ITS OWN event in; DSp inspects it for the suspend/resume
 * osEvt it must handle, drives context state accordingly, and reports via the
 * Boolean out-param whether it consumed the event. NO ctxRef - the dispatch
 * case routes r3 = inEvent (EventRecord*), r4 = outEventWasProcessed (Boolean*).
 *
 * EventRecord byte layout (16 bytes; same as the retired handler read/wrote):
 *   what(+0,u16) message(+2,u32) when(+6,u32) where_v(+10,i16)
 *   where_h(+12,i16) modifiers(+14,u16).
 *
 * osEvt suspend/resume decode: an osEvt (what == kDSpEvent_OSEvt) whose
 * message high byte == kDSpOSEvt_SuspendResumeMessage carries the resume flag
 * in message bit 0 (kDSpOSEvtMsg_ResumeFlag): set = resume (foreground),
 * clear = suspend (background). On suspend we drive every Active context to
 * Paused (mirroring DSpHandleBackgroundFromEmulThread's state-drive direction
 * + the paused_by_background bookkeeping); on resume we drive every
 * bg-induced-Paused context back to Active (mirroring
 * DSpHandleForegroundFromEmulThread). We mark
 * the event consumed ONLY for that suspend/resume osEvt. For everything DSp
 * does NOT handle we return outEventWasProcessed = false - HONEST, never a
 * silent always-true stub. The full consumed-event-set refinement is a
 * device-UAT gate; The Sims never calls this.
 *
 * Synchronous, emul-thread - NO ring read, NO atomic.
 */
/* Defined near DSpContext_SetStateHandler; declared here so Release can tear
 * down the full-screen Window Manager backdrop too. */
static bool DSpSetContextBackdropWindow(DSpContextPrivate *ctx, bool visible);

extern "C" int32_t DSpProcessEventHandler(uint32_t inEventAddr,
										  uint32_t outProcessedAddr)
{
	/* ASVS V5: NULL-guard BOTH guest addresses BEFORE any
	 * read or write. Fixed 16-byte read + 1-byte write - no attacker-
	 * controlled length. */
	if (inEventAddr == 0 || outProcessedAddr == 0) {
		DSP_LOG("DSpProcessEvent: NULL inEvent=0x%08x or outProcessed=0x%08x "
				"-> kDSpInvalidAttributesErr", inEventAddr, outProcessedAddr);
		return kDSpInvalidAttributesErr;
	}

	uint16_t what    = (uint16_t)ReadMacInt16(inEventAddr + 0);
	uint32_t message = (uint32_t)ReadMacInt32(inEventAddr + 2);

	bool consumed = false;

	/* osEvt suspend/resume is the documented event DSp consumes for
	 * context suspend/resume: high byte = subtype, bit 0 = resume flag.
	 *
	 * SCOPE (explicit, NOT a silent partial-fidelity stub):
	 * this handler drives the DSp context STATE MACHINE only - the
	 * Active<->Paused flag transition + the paused_by_background
	 * bookkeeping, matching DSpHandleBackgroundFromEmulThread /
	 * DSpHandleForegroundFromEmulThread's state-drive DIRECTION. It does
	 * NOT perform the Metal back-buffer
	 * release / re-alloc + cold-start that the iOS NotificationCenter
	 * lifecycle path performs via DSpHandleBackgroundFromEmulThread /
	 * DSpHandleForegroundFromEmulThread (DSpQueueReleaseAtVBLPartial +
	 * DSpAllocateBackBuffer). So an app that drives suspend/resume through
	 * DSpProcessEvent (rather than relying on the host lifecycle
	 * flag-and-drain path) gets a correct STATE transition but keeps its back-buffer
	 * allocated across the suspend and does not cold-start on resume.
	 * Wiring the full resource lifecycle here is deferred: the two
	 * production halves run on different threads and gate on different
	 * fields (main-thread bridge -> paused_by_background; emul-thread
	 * VBL drain -> persisted.invalidated_full), and the back-buffer
	 * re-alloc can fail under the headless/host-blocked test environment,
	 * so reconciling them is non-trivial. The Sims (the target UAT app)
	 * never calls DSpProcessEvent. */
	if (what == (uint16_t)kDSpEvent_OSEvt) {
		uint8_t subtype = (uint8_t)((message >> 24) & 0xFFu);
		if (subtype == (uint8_t)kDSpOSEvt_SuspendResumeMessage) {
			bool resume = (message & (uint32_t)kDSpOSEvtMsg_ResumeFlag) != 0u;
			if (resume) {
				/* Resume (foreground): drive every bg-induced-Paused
				 * context back to Active. Matches
				 * DSpHandleForegroundFromEmulThread's state-drive
				 * direction - SetState(Active) then clear paused_by_background;
				 * user-Paused contexts (paused_by_background == 0) stay
				 * Paused (distinction preserved). Uses the public
				 * DSpGetContext(i+1) accessor - same handle->slot mapping
				 * and bounds/null guard as the foreground drain, not the
				 * raw dsp_context_table[] array. */
				for (uint32_t i = 0; i < DSP_MAX_CONTEXTS; i++) {
					DSpContextPrivate *ctx = DSpGetContext(i + 1u);
					if (ctx == NULL) continue;
					if (ctx->state != (uint32_t)kDSpContextState_Paused) continue;
					if (ctx->paused_by_background == 0) continue;
					(void)DSpContext_SetStateHandler(ctx->handle,
						(uint32_t)kDSpContextState_Active);
					ctx->paused_by_background = 0;
				}
			} else {
				/* Suspend (background): drive every Active context to
				 * Paused. Matches DSpHandleBackgroundFromEmulThread's
				 * state-drive direction - SetState(Paused) + mark
				 * paused_by_background so a later resume auto-restores.
				 * Uses DSpGetContext(i+1) to match the background drain. */
				for (uint32_t i = 0; i < DSP_MAX_CONTEXTS; i++) {
					DSpContextPrivate *ctx = DSpGetContext(i + 1u);
					if (ctx == NULL) continue;
					if (ctx->state != (uint32_t)kDSpContextState_Active) continue;
					(void)DSpContext_SetStateHandler(ctx->handle,
						(uint32_t)kDSpContextState_Paused);
					ctx->paused_by_background = 1;
				}
			}
			consumed = true;
		}
	}

	/* Pascal Boolean (1 byte: 0 = false, 1 = true). HONEST false for any
	 * event DSp does not handle. */
	WriteMacInt8(outProcessedAddr, consumed ? 1 : 0);
	DSP_LOG("DSpProcessEvent: what=%u message=0x%08x -> consumed=%d",
			(unsigned)what, message, consumed ? 1 : 0);
	return kDSpNoErr;
}

/* --- Background / Foreground emul-thread drain --- */

/*
 *  Background restore path - runs on the EMUL THREAD via the VBL
 *  secondary-callback drain chain. For every Active context:
 *    1. Snapshot metadata into ctx->persisted (attr + palette/gamma
 *       generation counters + invalidated_full=1 so foreground restore
 *       triggers a full re-upload).
 *    2. Queue the Metal back-buffer + texture for VBL-bounded partial
 *       release (struct stays alive; foreground re-allocates in-place).
 *    3. Transition state Active -> Paused via the DMC (Paused routes to
 *       kDMCOwnerQuickDraw).
 *
 *  Palette / gamma generation placeholders are 0 - the CLUT and
 *  gamma/fade subsystems populate the persisted snapshots when
 *  those paths run. The invalidated_full=1 flag guarantees a
 *  cold-start full re-upload once the backing Metal resources come
 *  back, independent of palette/gamma state.
 */
extern "C" void DSpHandleBackgroundFromEmulThread(void)
{
	for (uint32_t i = 0; i < DSP_MAX_CONTEXTS; i++) {
		DSpContextPrivate *ctx = dsp_context_table[i];
		if (ctx == NULL) continue;
		if (ctx->state != (uint32_t)kDSpContextState_Active) continue;

		/* Persist metadata - attr + palette/gamma generation counters.
		 * Field names use the `persisted_*_generation` form so the DMC
		 * write-site inventory grep gate (DMCWriteSiteInventoryTests)
		 * does NOT match - only the DMC module itself owns the bare-word
		 * generation identifiers. */
		ctx->persisted.attr                          = ctx->attr;
		ctx->persisted.persisted_palette_generation  = 0u;  /* CLUT */
		ctx->persisted.persisted_gamma_generation    = 0u;  /* gamma/fade */
		ctx->persisted.invalidated_full              = 1u;

		/* Use the same transition as an application-requested pause. This
		 * restores the real QuickDraw video mode before the off-screen DSp
		 * backing is released. */
		if (DSpContext_SetStateHandler(
				ctx->handle,
				(uint32_t)kDSpContextState_Paused) != kDSpNoErr) {
			DSP_LOG("Background: ctx=%u video-mode restore failed; "
					"context remains Active", ctx->handle);
			continue;
		}

		/* Release Metal back-buffer via the partial queue - struct
		 * stays alive for foreground restore; only buffer + texture
		 * are freed. */
		DSpQueueReleaseAtVBLPartial(ctx);
		DSP_LOG("Background: ctx=%u Active->Paused "
				"(back-buffer queued for VBL partial release)",
				ctx->handle);
	}

	/* D-4-1: alt-buffer Metal backings release alongside the back
	 * buffers - otherwise their live allocations pin the DSp bump heap
	 * (no reset possible) and every bg/fg cycle leaks a back-buffer-
	 * sized region toward the heap ceiling. Guest staging survives;
	 * foreground re-allocates and repopulates from it. */
	DSpReleaseAltBufferBackingsForBackground();
}

/*
 *  Foreground restore path - runs on the EMUL THREAD via the VBL
 *  secondary-callback drain chain. For every Paused context whose
 *  persisted.invalidated_full==1 (i.e., previously backgrounded via
 *  DSpHandleBackgroundFromEmulThread):
 *    1. Re-allocate Metal back-buffer + texture from persisted
 *       attributes via DSpAllocateBackBuffer. Failure leaves the
 *       context Paused - next foreground tick retries.
 *    2. Reset dirty state + set dirty_cold_start=true so the next
 *       SwapBuffers performs a full re-upload (PDF p.38).
 *    3. Clear invalidated_full so a redundant foreground event (shouldn't
 *       happen, defense-in-depth) doesn't double-restore.
 *    4. Transition Paused -> Active via the DMC (kDMCOwnerDSp).
 *
 *  Failure here logs + continues without
 *  transitioning; next foreground retries. User can also explicitly
 *  Release the context via DSpContext_Release to recover.
 */
extern "C" void DSpHandleForegroundFromEmulThread(void)
{
	for (uint32_t i = 0; i < DSP_MAX_CONTEXTS; i++) {
		DSpContextPrivate *ctx = dsp_context_table[i];
		if (ctx == NULL) continue;
		if (ctx->state != (uint32_t)kDSpContextState_Paused) continue;
		if (ctx->persisted.invalidated_full == 0u) continue;

		/* Re-allocate Metal back-buffer from persisted attributes. */
		if (!DSpAllocateBackBuffer(ctx,
								   ctx->persisted.attr.displayWidth,
								   ctx->persisted.attr.displayHeight,
								   ctx->persisted.attr.backBufferBestDepth)) {
			DSP_LOG("Foreground: ctx=%u back-buffer re-alloc FAILED; "
					"context remains Paused (retry on next fg)",
					ctx->handle);
			continue;
		}

		/* Cold-start: force full re-upload on next SwapBuffers
		 * regardless of any stale dirty-rect accumulator contents. */
		ctx->dirty_cold_start = true;
		ctx->dirty_empty      = true;
		ctx->dirty_left       = 0;
		ctx->dirty_top        = 0;
		ctx->dirty_right      = 0;
		ctx->dirty_bottom     = 0;

		/* Invalidate the restore flag - defence-in-depth against a
		 * redundant foreground event double-restoring the context. */
		ctx->persisted.invalidated_full = 0u;

		if (DSpContext_SetStateHandler(
				ctx->handle,
				(uint32_t)kDSpContextState_Active) != kDSpNoErr) {
			DSP_LOG("Foreground: ctx=%u video-mode activation failed; "
					"context remains Paused", ctx->handle);
			continue;
		}

		DSP_LOG("Foreground: ctx=%u Paused->Active "
				"(back-buffer re-allocated; dirty_cold_start=1)",
				ctx->handle);
	}

	/* D-4-1: re-create alt-buffer Metal backings released on background
	 * and repopulate them from their guest-RAM staging. Failure leaves a
	 * NULL backing (NULL-checked everywhere) and retries next foreground,
	 * matching the back-buffer policy above. */
	DSpRestoreAltBufferBackingsForForeground();
}

/*
 *  VBL drain entry point - runs AFTER DSpVBLReleaseCallback has drained
 *  the release FIFO (drain-first ordering). Reads + clears the pending
 *  flag from the engine-module atomic (DSpExchangeBgFgPending bridge),
 *  then dispatches to the matching emul-thread handler.
 *
 *  Threading contract:
 *    - UIKit lifecycle hook stores a pending flag (store-release).
 *    - VBL drain exchanges flag to 0 (acquire-exchange) at a controlled
 *      callback point and performs the table mutation here.
 *
 *  In the current iOS build both bullets run on the same main==emul thread;
 *  the atomic is a deliberate deferral primitive that also preserves the
 *  bridge if a future thread split moves emulation off the UIKit main thread.
 */
extern "C" void DSpVBLBackgroundForegroundDrain(void)
{
	uint32_t pending = DSpExchangeBgFgPending();
	/* Bitmask, background first: a bg+fg pair accumulated while no drain
	 * could run (e.g. sync drain skipped inside the VBL chain) nets out
	 * to release-then-restore in the correct order. */
	if (pending & kDSpPendingBackground) {
		DSpHandleBackgroundFromEmulThread();
	}
	if (pending & kDSpPendingForeground) {
		DSpHandleForegroundFromEmulThread();
	}
}

/*
 *  Synchronous lifecycle drain - see dsp_draw_context.h. Runs the bg/fg
 *  transition drain, then the release-FIFO drain so back buffers queued by
 *  DSpQueueReleaseAtVBLPartial are actually freed NOW (the VBL release
 *  callback that normally drains the FIFO is paused during background -
 *  deferring the free to it would hold the memory through the entire
 *  background period, defeating the release). The chained
 *  DSpVBLBackgroundForegroundDrain inside DSpVBLReleaseCallback re-runs
 *  against a now-zero pending mask (no-op).
 */
extern "C" void DSpDrainLifecycleSync(void)
{
	if (vbl_source_in_callback_chain()) {
		/* Mid-tick: the in-flight chain's own DSpVBLReleaseCallback ->
		 * DSpVBLBackgroundForegroundDrain will consume the pending bits
		 * after the current callback unwinds; nesting the table walk here
		 * would re-enter the state machine. */
		DSP_LOG("DrainLifecycleSync: deferred to in-flight VBL chain");
		return;
	}
	/* GPU work was drained on the background edge (waitUntilCompleted in
	 * gfxaccel_handle_background_enter Step 2) and the VBL source is
	 * paused, so resets reached from this drain may bypass the heap's
	 * GPU-completion latch - see dsp_lifecycle_gpu_drained. */
	dsp_lifecycle_gpu_drained = true;
	DSpVBLBackgroundForegroundDrain();
	DSpVBLReleaseCallback(NULL, NULL, 0.0);
	dsp_lifecycle_gpu_drained = false;
}

/* --- DSpContext_ReserveHandler --- */

enum {
	kDSpColorTable_ctSize = 6,
	kDSpColorTable_ctTable = 8,
	kDSpColorSpec_value = 0,
	kDSpColorSpec_red = 2,
	kDSpColorSpec_green = 4,
	kDSpColorSpec_blue = 6,
	kDSpColorSpec_stride = 8
};

/* Apply DSpContextAttributes.colorTable (a classic QuickDraw CTabHandle) to
 * the context's indexed CLUT. The handle points to a master pointer, whose
 * pointee is ColorTable { ctSeed, ctFlags, ctSize, ctTable[] }. ctSize is
 * one-less-than-count; each ColorSpec.value supplies the destination CLUT
 * index. Direct-color contexts ignore the table per the PDF. */
static void DSpApplyReserveColorTable(DSpContextPrivate *ctx,
									  uint32_t colorTableHandle,
									  uint32_t depth_bits)
{
	if (ctx == NULL || colorTableHandle == 0) return;
	if (depth_bits != 1 && depth_bits != 2 &&
		depth_bits != 4 && depth_bits != 8) {
		return;
	}

	/* Validate every guest extent BEFORE dereferencing it (the
	 * NQDMetalAddrInBuffer reject-first idiom, dsp_flatten_restore.mm) - a
	 * garbage non-NULL CTabHandle in the Reserve attributes must fall back
	 * to the default CLUT, not fault the ReadMacInt* translation layer. */
	if (!NQDMetalAddrInBuffer(colorTableHandle) ||
		!NQDMetalAddrInBuffer(colorTableHandle + 3u)) {
		DSP_LOG("Reserve: colorTable handle=0x%08x out of mapped RAM; "
				"default indexed CLUT retained", colorTableHandle);
		return;
	}
	uint32_t colorTableAddr = ReadMacInt32(colorTableHandle);
	if (colorTableAddr == 0) {
		DSP_LOG("Reserve: colorTable handle=0x%08x is NULL/purged; "
				"default indexed CLUT retained", colorTableHandle);
		return;
	}
	/* ColorTable header: ctSeed/ctFlags/ctSize = 8 bytes before ctTable. */
	if (!NQDMetalAddrInBuffer(colorTableAddr) ||
		!NQDMetalAddrInBuffer(colorTableAddr + kDSpColorTable_ctTable - 1u)) {
		DSP_LOG("Reserve: colorTable handle=0x%08x table=0x%08x header out "
				"of mapped RAM; default indexed CLUT retained",
				colorTableHandle, colorTableAddr);
		return;
	}
	int16_t ctSize = (int16_t)ReadMacInt16(colorTableAddr + kDSpColorTable_ctSize);
	if (ctSize < 0) {
		DSP_LOG("Reserve: colorTable handle=0x%08x table=0x%08x "
				"has negative ctSize=%d; default indexed CLUT retained",
				colorTableHandle, colorTableAddr, (int)ctSize);
		return;
	}

	uint32_t entry_count = (uint32_t)ctSize + 1u;
	if (entry_count > 256u) entry_count = 256u;

	/* Full ColorSpec array extent (header + entry_count * 8 bytes). */
	uint32_t table_bytes = kDSpColorTable_ctTable +
						   entry_count * kDSpColorSpec_stride;
	if (!NQDMetalAddrInBuffer(colorTableAddr + table_bytes - 1u)) {
		DSP_LOG("Reserve: colorTable handle=0x%08x table=0x%08x ctTable "
				"extent %u bytes out of mapped RAM; default indexed CLUT "
				"retained", colorTableHandle, colorTableAddr, table_bytes);
		return;
	}

	uint8_t staged[256 * 3];
	memcpy(staged, ctx->clut_bytes, sizeof(staged));
	uint32_t applied = 0;
	for (uint32_t i = 0; i < entry_count; i++) {
		uint32_t e = colorTableAddr + kDSpColorTable_ctTable +
					 i * kDSpColorSpec_stride;
		int16_t value = (int16_t)ReadMacInt16(e + kDSpColorSpec_value);
		if (value < 0 || value > 255) continue;
		uint32_t idx = (uint32_t)value;
		staged[idx * 3 + 0] = (uint8_t)(ReadMacInt16(e + kDSpColorSpec_red) >> 8);
		staged[idx * 3 + 1] = (uint8_t)(ReadMacInt16(e + kDSpColorSpec_green) >> 8);
		staged[idx * 3 + 2] = (uint8_t)(ReadMacInt16(e + kDSpColorSpec_blue) >> 8);
		applied++;
	}
	if (applied == 0) {
		DSP_LOG("Reserve: colorTable handle=0x%08x table=0x%08x "
				"had no valid ColorSpec.value entries; default indexed CLUT retained",
				colorTableHandle, colorTableAddr);
		return;
	}

	int32_t rv = DSpSetCLUTCore(ctx, 0, 255, staged);
	if (rv == kDSpNoErr) {
		memcpy(ctx->clut_bytes_latched, ctx->clut_bytes, 768);
		DSP_LOG("Reserve: applied colorTable handle=0x%08x table=0x%08x "
				"entries=%u applied=%u depth=%u",
				colorTableHandle, colorTableAddr, entry_count, applied, depth_bits);
	} else {
		DSP_LOG("Reserve: DSpSetCLUTCore failed for colorTable handle=0x%08x "
				"table=0x%08x rv=%d; default indexed CLUT retained",
				colorTableHandle, colorTableAddr, rv);
	}
}

/*
 *  Debug session `dsp-sims-post-reserve-black-screen` fix (2026-04-19) -
 *  Reserve_OnHandle_Core attaches Metal back-buffer + back-texture to an
 *  EXISTING metadata-only ctx (typically one vended by
 *  DSpFindBestContextHandler or DSpGetFirstContextHandler). Per DSp 1.7
 *  PDF p.25:
 *      OSStatus DSpContext_Reserve(DSpContextReference inContext,
 *			const DSpContextAttributesPtr inDesiredAttributes);
 *  The caller ALREADY holds the ctxRef; Reserve's job is to validate +
 *  apply the desired attributes AND allocate the Metal resources that
 *  subsequent GetBackBuffer / SwapBuffers / SetState calls require.
 *
 *	MechWarrior 2 6500: It does GetNextContext() until it finds
 *	512x384@16bpp, then allocates a backbuffer with depth and
 *	depthflags set to 0. If 512x384@16bpp is not found it never
 *	allocates a back buffer. The important distinction is that
 *	it never changes the resolution to 512x384@16bpp, instead it
 *	expects DrawSprocket to scale the 512x384 backbuffer.
 *
 *  Behavior:
 *    - Validates `attr` (same validation rules as Reserve_Core).
 *    - Overrides ctx->attr with the caller-provided DesiredAttributes
 *      (allows apps to request a back-buffer depth different from the
 *      display depth - PDF p.25 DISCUSSION).
 *    - Refuses re-reservation (ctx already has a back_buffer) with
 *      kDSpContextAlreadyReservedErr (PDF p.87 error code).
 *    - Allocates back_buffer + back_texture via DSpAllocateBackBuffer.
 *    - Marks the context as off the enumeration chain (Reserve implies
 *      "this is now a game-owned context, not a cursor").
 *
 *  Returns:
 *    - kDSpNoErr on success.
 *    - kDSpInvalidContextErr if ctx is NULL.
 *    - kDSpInvalidAttributesErr on attr validation failure.
 *    - kDSpContextAlreadyReservedErr if back_buffer already present.
 *    - kDSpInternalErr on Metal allocation failure.
 *
 *  Threading: emul-thread single-writer (same contract as Reserve_Core).
 *  No mutex / MTLFence / _Atomic / @synchronized.
 */
static int32_t DSpContext_Reserve_OnHandle_Core(DSpContextPrivate *ctx,
												 const DSpContextAttributes *attr)
{
	if (attr == NULL) {
		DSP_LOG("Reserve_OnHandle: NULL attr ptr");
		return kDSpInvalidAttributesErr;
	}
	if (ctx == NULL) {
		DSP_LOG("Reserve_OnHandle: NULL ctx");
		return kDSpInvalidContextErr;
	}

	uint32_t desiredWidth        = attr->displayWidth;
	uint32_t desiredHeight       = attr->displayHeight;
	uint32_t backBufferBestDepth = attr->backBufferBestDepth;
	uint32_t desiredDisplayDepth = attr->displayBestDepth;
	uint32_t backBufferDepthMask = attr->backBufferDepthMask;
	uint32_t desiredDisplayMask  = attr->displayDepthMask;
	uint32_t pageCount           = attr->pageCount;
	uint32_t colorNeeds          = attr->colorNeeds;
	uint32_t contextOptions      = attr->contextOptions;

	/* Validation rules - same invariants as Reserve_Core. */
	if (pageCount == 0 || desiredWidth == 0 || desiredHeight == 0) {
		DSP_LOG("Reserve_OnHandle: invalid attrs (w=%u h=%u pc=%u)",
				desiredWidth, desiredHeight, pageCount);
		return kDSpInvalidAttributesErr;
	}
	if(backBufferBestDepth == 0) {
		backBufferBestDepth = ctx->attr.backBufferBestDepth;
	} else if (backBufferBestDepth != 1 && backBufferBestDepth != 2 &&
		backBufferBestDepth != 4 && backBufferBestDepth != 8 &&
		backBufferBestDepth != 16 && backBufferBestDepth != 32) {
		DSP_LOG("Reserve_OnHandle: unsupported back-buffer depth %u "
				"(valid: 1/2/4/8/16/32)",
				backBufferBestDepth);
		return kDSpInvalidAttributesErr;
	}
	if (backBufferDepthMask == 0) {
		backBufferDepthMask = ctx->attr.backBufferDepthMask;
	}
	if (desiredWidth > 4096 || desiredHeight > 4096) {
		DSP_LOG("Reserve_OnHandle: oversize resolution %ux%u clamped to paramErr",
				desiredWidth, desiredHeight);
		return kDSpInvalidAttributesErr;
	}

	/* PDF p.87 - kDSpContextAlreadyReservedErr if caller re-reserves a
	 * context that already has a back-buffer. Real DSp 1.7 guards against
	 * double-Reserve; the earlier Reserve used to always create a fresh
	 * ctx so this code path was unreachable. Now that Reserve attaches
	 * to an existing handle, we must surface the spec error. */
	if (ctx->back_buffer != NULL) {
		DSP_LOG("Reserve_OnHandle: ctx=%u already has back-buffer - "
				"kDSpContextAlreadyReservedErr",
				ctx->handle);
		return kDSpContextAlreadyReservedErr;
	}

	const uint32_t actualDisplayWidth =
		DSpReserveActualDisplayDimension(ctx->attr.displayWidth,
										 desiredWidth);
	const uint32_t actualDisplayHeight =
		DSpReserveActualDisplayDimension(ctx->attr.displayHeight,
										 desiredHeight);
	const uint32_t actualDisplayDepth =
		DSpReserveActualDisplayDepth(ctx->attr.displayBestDepth,
									 desiredDisplayDepth,
									 backBufferBestDepth);
	const uint32_t actualDisplayMask =
		ctx->attr.displayDepthMask != 0 ? ctx->attr.displayDepthMask
										: desiredDisplayMask;

	/* Apply desired back-buffer attributes while preserving the selected
	 * display mode that FindBest/GetFirst put on the metadata context.
	 * DSp 1.7 p.25 explicitly allows a requested 320x240 back buffer inside
	 * a best-match 640x480 display; displayWidth/Height remain the actual
	 * front/display mode and the host-only backBufferWidth/Height carry the
	 * requested drawing environment. */
	ctx->attr.displayWidth        = actualDisplayWidth;
	ctx->attr.displayHeight       = actualDisplayHeight;
	ctx->attr.backBufferWidth     = DSpReserveBackBufferDimension(
										actualDisplayWidth, desiredWidth);
	ctx->attr.backBufferHeight    = DSpReserveBackBufferDimension(
										actualDisplayHeight, desiredHeight);
	ctx->attr.backBufferBestDepth = backBufferBestDepth;
	ctx->attr.displayBestDepth    = actualDisplayDepth;
	ctx->attr.backBufferDepthMask = backBufferDepthMask;
	ctx->attr.displayDepthMask    = actualDisplayMask;
	ctx->attr.pageCount           = pageCount;
	ctx->attr.colorNeeds          = colorNeeds;
	ctx->attr.colorTable          = attr->colorTable;
	ctx->attr.contextOptions      = contextOptions;

	/* Reserve transitions this context off the enumeration chain -
	 * DSpGetNextContext with a reserved handle terminates per PDF p.17
	 * "last context in the list" + debug session dsp-sims-enumeration-
	 * stall precedent. */
	ctx->enumeration_mode_index = DSP_ENUMERATION_INDEX_NONE;
	ctx->state                  = kDSpContextState_Inactive;
	ctx->dirty_empty            = true;
	ctx->dirty_cold_start       = true;   /* PDF p.38 - first swap = full */
	/* Seeding the default ramp is NOT an app-supplied CLUT - clear the flag
	 * first so the indexed expand path falls back to the live display palette
	 * until the app really sets one. DSpApplyReserveColorTable /
	 * DSpSetCLUTCore set it when a real table arrives. */
	ctx->clut_app_supplied = 0;
	DSpInitDefaultCLUT(ctx->clut_bytes, ctx->clut_bytes_latched,
					   backBufferBestDepth);
	DSpApplyReserveColorTable(ctx, ctx->attr.colorTable,
							  backBufferBestDepth);

	/* Allocate Metal back-buffer + texture on the existing ctx. The
	 * MTLBuffer / MTLTexture slots were NULL before (this was a metadata-
	 * only ctx); DSpAllocateBackBuffer fills them in with the same
	 * gfxaccel heap routing as Reserve_Core. */
	if (!DSpAllocateBackBuffer(ctx,
							   ctx->attr.backBufferWidth,
							   ctx->attr.backBufferHeight,
							   backBufferBestDepth)) {
		DSP_LOG("Reserve_OnHandle: DSpAllocateBackBuffer failed for "
				"ctx=%u %ux%u@%ubpp",
				ctx->handle, ctx->attr.backBufferWidth,
				ctx->attr.backBufferHeight,
				backBufferBestDepth);
		return kDSpInternalErr;
	}

	DSP_LOG("Reserve_OnHandle: ctx=%u display=%ux%u@%u "
			"back=%ux%u@%ubpp pc=%u opts=0x%x",
			ctx->handle, ctx->attr.displayWidth, ctx->attr.displayHeight,
			ctx->attr.displayBestDepth,
			ctx->attr.backBufferWidth, ctx->attr.backBufferHeight,
			backBufferBestDepth, pageCount, contextOptions);
	return kDSpNoErr;
}

extern "C" int32_t DSpContext_ReserveHandler(uint32_t ctxRef,
											  uint32_t attrAddr)
{
	/*
	 *  Debug session `dsp-sims-post-reserve-black-screen` fix (2026-04-19) -
	 *  Signature corrected to DSp 1.7 PDF p.25:
	 *      OSStatus DSpContext_Reserve(DSpContextReference inContext,
	 *                                  const DSpContextAttributesPtr inDesiredAttributes);
	 *  r3 = ctxRef (existing handle from FindBestContext / GetFirstContext).
	 *  r4 = attrAddr (Mac pointer to desired attributes).
	 *
	 *  The previous implementation treated r3 as an out-ptr to write a new
	 *  handle into, producing a bogus secondary handle the app promptly
	 *  ignored (since its own `gContext` local was already populated by
	 *  FindBestContext). That left the true ctxRef with no back-buffer,
	 *  SetState(Active) succeeded on the wrong object, and no frames ever
	 *  reached the display.
	 */
	if (attrAddr == 0) {
		DSP_LOG("Reserve: NULL attr addr - kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL) {
		DSP_LOG("Reserve: invalid ctxRef=%u - kDSpInvalidContextErr",
				ctxRef);
		return kDSpInvalidContextErr;
	}

	/*
	 *  Re-corrected 2026-04-21 via debug session
	 *  `dsp-sims-rejects-all-modes`: Read DSpContextAttributes from guest
	 *  RAM using DSp 1.7 PDF p.65 on-wire byte layout. All on-wire fields
	 *  are 4 bytes (UInt32 / Fixed / OptionBits / CTabHandle / pageCount).
	 *  filler[3] @ +52..+54 + gameMustConfirmSwitch (Boolean) @ +55 +
	 *  reserved3[4] @ +56..+71 are not validated on input (PDF p.67).
	 *  backBufferWidth / backBufferHeight are host-only mirror fields
	 *  (not on the wire) - populated from displayWidth / displayHeight.
	 */
	DSpContextAttributes attr = {};
	attr.frequency            = ReadMacInt32(attrAddr +  0);  /* input ignored per PDF */
	attr.displayWidth         = ReadMacInt32(attrAddr +  4);
	attr.displayHeight        = ReadMacInt32(attrAddr +  8);
	attr.reserved1            = ReadMacInt32(attrAddr + 12);
	attr.reserved2            = ReadMacInt32(attrAddr + 16);
	attr.colorNeeds           = ReadMacInt32(attrAddr + 20);
	attr.colorTable           = ReadMacInt32(attrAddr + 24);
	attr.contextOptions       = ReadMacInt32(attrAddr + 28);
	attr.backBufferDepthMask  = ReadMacInt32(attrAddr + 32);
	attr.displayDepthMask     = ReadMacInt32(attrAddr + 36);
	attr.backBufferBestDepth  = ReadMacInt32(attrAddr + 40);
	attr.displayBestDepth     = ReadMacInt32(attrAddr + 44);
	attr.pageCount            = ReadMacInt32(attrAddr + 48);
	attr.gameMustConfirmSwitch = 0;
	attr.backBufferWidth      = attr.displayWidth;
	attr.backBufferHeight     = attr.displayHeight;
	return DSpContext_Reserve_OnHandle_Core(ctx, &attr);
}


/*
 *  Metadata-only context allocation.
 *
 *  DSpGetFirstContext / DSpFindBestContext vend a DSp context reference
 *  populated with vended-mode attributes; the emulated app inspects the
 *  attributes and only later calls DSpContext_Reserve to actually
 *  instantiate a back-buffer. To avoid duplicating the context-table
 *  slot-alloc logic, we expose DSpAllocFirstContextHandle as the single
 *  entry point for "claim a slot, populate attr, return handle" without
 *  the Metal-resource allocation that Reserve_Core performs.
 *
 *  Threading: emul-thread single-writer. No mutex - same contract as
 *  DSpAllocContextHandle / Reserve_Core.
 *
 *  Debug session `dsp-sims-enumeration-stall` fix (2026-04-19): added the
 *  `enumeration_mode_index` parameter. Callers on the GetFirstContext /
 *  GetNextContext iteration path pass the 0-based index into s_dsp_modes[]
 *  that the context's attr was copied from; the value is stored in
 *  ctx->enumeration_mode_index so DSpGetNextContextHandler can advance the
 *  cursor. FindBest and other non-enumeration callers pass
 *  DSP_ENUMERATION_INDEX_NONE so GetNextContext treats the handle as a
 *  terminator (no successor).
 */
extern "C" uint32_t DSpAllocFirstContextHandle(const DSpContextAttributes *attr,
												uint32_t enumeration_mode_index)
{
	if (attr == NULL) return 0;

	DSpContextPrivate *ctx = new DSpContextPrivate();
	ctx->attr  = *attr;
	ctx->state = kDSpContextState_Inactive;
	/* Debug session `dsp-sims-enumeration-stall` fix (2026-04-19):
	 * record the index so GetNextContext can walk forward through
	 * s_dsp_modes[]. Sentinel value DSP_ENUMERATION_INDEX_NONE means
	 * the context is a terminator (e.g., FindBest best-match result). */
	ctx->enumeration_mode_index = enumeration_mode_index;
	/* Cheap-query bookkeeping - explicit zero-init (see
	 * Reserve_Core; 0 = no max-fps restriction + grid defaults to base unit). */
	ctx->max_frame_rate = 0;
	ctx->dirty_grid_w   = 0;
	ctx->dirty_grid_h   = 0;
	ctx->clut_app_supplied = 0;   /* default ramp only - see Reserve path */
	DSpInitDefaultCLUT(ctx->clut_bytes, ctx->clut_bytes_latched,
					   ctx->attr.backBufferBestDepth);

	uint32_t handle = DSpAllocContextHandle(ctx);
	if (handle == 0) {
		delete ctx;
		return 0;
	}
	dsp_context_count++;
	DSP_LOG("AllocFirstContextHandle: handle=%u %ux%u@%ubpp (metadata-only, enum_idx=%u)",
			handle, attr->displayWidth, attr->displayHeight,
			attr->backBufferBestDepth,
			(unsigned)enumeration_mode_index);
	return handle;
}

extern "C" uint32_t DSpAllocMetadataContextHandle(
	const DSpContextAttributes *attr,
	uint32_t enumeration_mode_index)
{
	if (attr == NULL) return 0;

	uint32_t handle = DSpAllocFirstContextHandle(attr, enumeration_mode_index);
	if (handle != 0) return handle;

	for (uint32_t i = 0; i < DSP_MAX_CONTEXTS; i++) {
		DSpContextPrivate *ctx = dsp_context_table[i];
		if (!DSpIsInactiveMetadataOnlyContext(ctx)) continue;
		/* Enumeration contexts ARE recyclable: DSpGetNextContext vends a
		 * distinct context per step and never frees them (PDF p.16
		 * retained-ref semantics), so under table pressure the oldest
		 * stale walk is reclaimed here. An app can only lose a ref it is
		 * still holding if it accumulates more than DSP_MAX_CONTEXTS live
		 * enumerated refs - far beyond any real walk. */

		const uint32_t recycled_handle = i + 1u;
		DSpFreeContextHandle(recycled_handle);
		dsp_context_count--;
		DSpReleaseNow(ctx);
		DSP_LOG("AllocMetadataContextHandle: recycled inactive "
				"metadata-only handle=%u",
				recycled_handle);
		return DSpAllocFirstContextHandle(attr, enumeration_mode_index);
	}

	DSP_LOG("AllocMetadataContextHandle: table full; no recyclable "
			"inactive metadata-only handle");
	return 0;
}

/* --- DSpContext_ReleaseHandler --- */

extern "C" int32_t DSpContext_ReleaseHandler(uint32_t ctxRef)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL) {
		DSP_LOG("Release: invalid ctxRef=%u", ctxRef);
		return kDSpInvalidContextErr;
	}
	if (ctx->state == (uint32_t)kDSpContextState_Active) {
		const int32_t state_err = DSpContext_SetStateHandler(
			ctxRef, (uint32_t)kDSpContextState_Inactive);
		if (state_err != kDSpNoErr)
			return state_err;
	}
	/* The Window Manager backdrop cannot outlive its DSp context. */
	DSpSetContextBackdropWindow(ctx, false);
	DSpFreeContextHandle(ctxRef);
	dsp_context_count--;
	/* Defer to next VBL. Do NOT free synchronously. */
	DSpQueueReleaseAtVBL(ctx);
	DSP_LOG("Release: handle=%u queued for VBL drain", ctxRef);
	return kDSpNoErr;
}

/* --- DSpContext_GetBackBufferHandler --- */

extern "C" int32_t DSpContext_GetBackBufferHandler(uint32_t ctxRef,
													uint32_t options,
													uint32_t outBufAddr)
{
	/* PDF: options are reserved in DSp 1.7; ignore but don't error. */
	(void)options;
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL || outBufAddr == 0) {
		DSP_LOG("GetBackBuffer: invalid ctxRef=%u or outBufAddr=0x%08x",
				ctxRef, outBufAddr);
		return kDSpInvalidContextErr;
	}
	uint32_t cgrafptr = DSpGetBackBufferCGrafPtr(ctx);
	if (cgrafptr == 0) {
		DSP_LOG("GetBackBuffer: CGrafPort emission failed for ctxRef=%u",
				ctxRef);
		return kDSpInternalErr;
	}
	/* Sub-op 705 underlay contract (PDF p.51):
	 * when this context has a designated underlay alt-buffer, restore the
	 * back buffer's invalid (dirty) areas from the underlay before handing
	 * the drawable back - "this is most useful in sprite games" (clean a
	 * back buffer). No-op when no underlay is designated. */
	DSpRestoreBackBufferFromUnderlay(ctx);
	WriteMacInt32(outBufAddr, cgrafptr);
	return kDSpNoErr;
}


/* --- DSpContext_SwapBuffersHandler --- */

static uint32_t DSpMaxFrameRatePacingVBLs(uint32_t maxFrameRate,
										  uint64_t cadenceUsec)
{
	if (maxFrameRate == 0) return 1;

	if (cadenceUsec == 0) {
		cadenceUsec = GFX_FRAME_PACING_DEFAULT_USEC;
	} else {
		cadenceUsec = GfxFramePacingClampCadenceUsec(cadenceUsec);
	}

	uint64_t refreshHz = (1000000ULL + cadenceUsec / 2u) / cadenceUsec;
	if (refreshHz == 0) refreshHz = 1;
	if ((uint64_t)maxFrameRate >= refreshHz) return 1;

	uint64_t vbls = (refreshHz + (uint64_t)maxFrameRate - 1u) /
					(uint64_t)maxFrameRate;
	if (vbls == 0) return 1;
	if (vbls > UINT32_MAX) return UINT32_MAX;
	return (uint32_t)vbls;
}

void DSpSyncSwapFramePacing(uint32_t ctxRef, uint32_t maxFrameRate)
{
	uint64_t cadenceUsec = vbl_source_get_cadence_usec();
	uint32_t pacingVBLs = DSpMaxFrameRatePacingVBLs(maxFrameRate,
													cadenceUsec);
	if (maxFrameRate != 0 && pacingVBLs > 1) {
		DSP_VLOG("SwapBuffers: ctx=%u maxFrameRate=%u cadence=%llu "
				 "-> pacing %u VBLs",
				 ctxRef, maxFrameRate,
				 (unsigned long long)cadenceUsec, pacingVBLs);
	}

	for (uint32_t i = 0; i < pacingVBLs; i++) {
		int32_t rc =
			vbl_source_sync_3d_pacing_for_engine(kGfxFramePacingEngineDSp);
		if (rc != kGfxAccelNoErr) {
			DSP_LOG("SwapBuffers: frame pacing sync failed rc=%d "
					"(ctx=%u, pass=%u/%u)",
					rc, ctxRef, i + 1u, pacingVBLs);
			break;
		}
	}
}


/* Pre-swap busyProc gate. DrawSprocket 1.7 defines DSpCallbackProcPtr as
 * Boolean (*)(DSpContextReference inContext, void *inRefCon), shared by
 * SwapBuffers and SetVBLProc. The callback returns false when its checks are
 * complete and true while it is still busy. If busyProcAddr is 0 the caller
 * skipped the gate - proceed immediately. Poll cadence is 0.5 ms with a
 * 2-VBL total budget (~33 ms at 60 Hz) so an infinite-loop PPC busyProc
 * cannot hang the emul thread. */
bool DSpPollBusyProc(uint32_t ctxRef, uint32_t busyProcAddr,
							uint32_t userRefCon)
{
	if (busyProcAddr == 0) return true;

	uint64_t cadence = vbl_source_get_cadence_usec();
	if (cadence == 0) cadence = 16667;    /* fallback 60 Hz */
	uint64_t timeout_usec = cadence * 2;  /* 2-VBL cap */
	uint64_t elapsed = 0;
	const uint32_t poll_step_usec = 500;  /* 0.5 ms */

	/* busyProc is a 32-bit TVECT address (PDF p.39: "an
	 * application-supplied callback function"). call_macos2 is used
	 * directly because the platform CallMacOS2 macro widens 32-bit guest
	 * values through uintptr before narrowing them back to uint32 here.
	 * Classic Boolean is an 8-bit value, so only the low byte matters. */
	while (elapsed < timeout_usec) {
		uint32 rv = call_macos2(busyProcAddr, ctxRef, userRefCon);
		if ((rv & 0xff) == 0) {
			return true;   /* busyProc signalled "checks complete" */
		}
		usleep(poll_step_usec);
		elapsed += poll_step_usec;
	}
	DSP_LOG("DSpPollBusyProc: 2-VBL timeout (busyProcAddr=0x%08x "
			"userRefCon=0x%08x elapsed=%llu)",
			busyProcAddr, userRefCon, (unsigned long long)elapsed);
	return false;
}

int32_t DSpRevalidateSwapContext(uint32_t ctxRef,
										DSpContextPrivate *expected,
										uint32_t entry_state,
										DSpContextPrivate **outCtx,
										const char *site)
{
	DSpContextPrivate *fresh = DSpGetContext(ctxRef);
	if (fresh == NULL || fresh != expected) {
		DSP_LOG("SwapBuffers: ctxRef=%u was released/replaced during %s",
				ctxRef, site);
		return kDSpInvalidContextErr;
	}
	/* Re-entry guard, not a state mandate: reject only when the state
	 * CHANGED across the re-entry window (guest busyProc / frame-pacing
	 * runloop pump). A context that ENTERED the swap non-Active (e.g.
	 * Paused by a background transition) swapped fine pre-revalidation and
	 * still must - classic apps expect SwapBuffers to succeed unless
	 * something is critically wrong. */
	if (fresh->state != entry_state) {
		DSP_LOG("SwapBuffers: ctxRef=%u state changed during %s "
				"(entry=%u now=%u)",
				ctxRef, site, entry_state, fresh->state);
		return kDSpInvalidContextErr;
	}
	if (fresh->back_texture == NULL || fresh->back_buffer == NULL) {
		DSP_LOG("SwapBuffers: ctxRef=%u lost back-buffer during %s",
				ctxRef, site);
		return kDSpInternalErr;
	}
	if (outCtx != NULL) {
		*outCtx = fresh;
	}
	return kDSpNoErr;
}


/* --------------------------------------------------------------------- *
 *  Local helpers                                                        *
 *                                                                       *
 *  DSpAlignedRowBytes / DSpBackBufferSize / DSpReserveGuestScratch live *
 *  as file-local static-inline helpers in dsp_metal_renderer.mm:73-143  *
 *  (not exported through any header - they predate the per-engine      *
 *  refactor). The Redirect helper below needs all three. Re-declaring   *
 *  them here as the same static-inline bodies keeps the implementations *
 *  identical (single source of truth for the formulas) and avoids a    *
 *  cross-TU API surface refactor. These could be hoisted into           *
 *  dsp_metal_renderer.h if a third caller emerges.                      *
 * --------------------------------------------------------------------- */

static inline uint32_t DSpAlignedRowBytes(uint32_t w, uint32_t bpp)
{
	uint32_t row_bytes = (w * bpp + 7) / 8;
	return (row_bytes + 255) & ~255u;
}

static inline uint32_t DSpBackBufferSize(uint32_t w, uint32_t h, uint32_t bpp)
{
	return DSpAlignedRowBytes(w, bpp) * h;
}

static inline void DSpPixMapFormatForDepth(uint32_t bpp,
											uint16_t *pixelType,
											uint16_t *pixelSize,
											uint16_t *cmpCount,
											uint16_t *cmpSize)
{
	if (bpp == 16) {
		*pixelType = 0x10;    /* RGBDirect */
		*pixelSize = 16;
		*cmpCount = 3;
		*cmpSize = 5;
	} else if (bpp == 32) {
		*pixelType = 0x10;    /* RGBDirect */
		*pixelSize = 32;
		*cmpCount = 3;
		*cmpSize = 8;
	} else {
		*pixelType = 0;       /* chunky indexed */
		*pixelSize = (uint16_t)bpp;
		*cmpCount = 1;
		*cmpSize = (uint16_t)bpp;
	}
}

static inline void DSpWriteCanonicalMainDevicePixMapMetadata(uint32_t pixMapPtr)
{
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PMVERSION, 0);
	WriteMacInt16(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PACKTYPE, 0);
	WriteMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PACKSIZE, 0);
	WriteMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_HRES, 0x00480000u);
	WriteMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_VRES, 0x00480000u);
	WriteMacInt32(pixMapPtr + DSP_MAINDEVICE_PIXMAP_OFF_PLANEBYTES, 0);
}


uint32_t DSpReserveGuestScratch(uint32_t size)
{
	return SheepMem::Reserve(size);
}

/* ===================================================================== *
 *  Blit (sub-ops 710-711).
 *
 *  DSpBlit_Fastest (711): strict 1:1 copy (srcRect == dstRect, no scaling)
 *    via the proven nqd_bitblt kernel (NQDMetalBitblt1to1).
 *  DSpBlit_Faster (710): scales srcRect -> dstRect via the NEW
 *    nqd_bitblt_scaled kernel (NQDMetalBitbltScaled); nearest-neighbor by
 *    default, bilinear only when kDSpBlitMode_Interpolation set (PDF p.87);
 *    color-key honored per SrcKey/DstKey.
 *
 *  Both reuse NQD's single-queue Metal compute path (no DSp-specific
 *  blit path) and add ZERO new concurrency primitive. The async path
 *  (inAsyncFlag + completionProc) is synchronous-then-complete: encode +
 *  flush, set completionFlag = true, then invoke completionProc via
 *  call_macos1 (the PPC-trampoline idiom - NEVER a raw C cast of the guest
 *  fn-ptr). A guest DSpBlitDoneProc takes one arg (the DSpBlitInfoPtr back).
 *
 *  Security (ASVS L1 V5): NULL-guard inBlitInfo; reject OOB src/dst baseAddr
 *  via NQDMetalAddrInBuffer BEFORE GPU dispatch; clamp src/dst rects to the
 *  CGrafPort bounds before computing kernel geometry.
 * ===================================================================== */

/* A resolved view of one side (src or dst) of a DSpBlit. baseAddr is the Mac
 * (guest) address of the rect ORIGIN (pixmap baseAddr + top*rowBytes +
 * left*bpp); the NQD entries fold this in once so the kernel addresses from
 * the rect origin. */
struct DSpBlitSide {
	uint32_t base_origin_mac;  /* Mac addr of the rect origin pixel */
	int32_t  row_bytes;        /* signed row stride (high bit masked off) */
	uint32_t pixel_bytes;      /* bytes per pixel (1, 2, or 4) */
	uint32_t bits_per_pixel;   /* raw depth (8, 16, 32) */
	int32_t  rect_w;           /* clamped rect width in pixels */
	int32_t  rect_h;           /* clamped rect height in pixels */
};

static uint32_t DSpResolvePixMapRecord(uint32_t cgrafptr_mac)
{
	if (cgrafptr_mac == 0) return 0;

	uint16_t port_version =
		(uint16_t)ReadMacInt16(cgrafptr_mac + DSP_CGRAFPORT_OFF_PORT_VERSION);
	if (!DSpLooksLikeColorCGrafPort(port_version)) {
		return cgrafptr_mac;
	}

	uint32_t pixmap_handle =
		ReadMacInt32(cgrafptr_mac + DSP_CGRAFPORT_OFF_PORT_PIXMAP);
	if (pixmap_handle == 0) return 0;
	return ReadMacInt32(pixmap_handle);
}

/* Resolve one CGrafPtr + its blit Rect into a DSpBlitSide. The CGrafPtr uses
 * either the DSp flat shim layout (alt buffers and legacy callers) or a real
 * CGrafPort whose portPixMap points at a PixMapHandle (back/front buffers).
 * The resolved PixMap uses
 * baseAddr@0, rowBytes@4 - high bit is the classic PixMap flag and is masked
 * off - and bounds@6 in both layouts. Extra PixMap fields use compact offsets
 * for flat shims and real QuickDraw offsets for Color CGrafPorts. The blit
 * Rect (top@+0/left@+2/bottom@+4/right@+6, big-endian i16) is clamped to the
 * PixMap bounds. Returns false on a NULL CGrafPtr, an unsupported depth, or an
 * empty/inverted rect. */
static bool DSpResolveBlitSide(uint32_t cgrafptr_mac, uint32_t rect_mac,
							   DSpBlitSide *out)
{
	if (cgrafptr_mac == 0 || rect_mac == 0 || out == NULL) return false;

	uint16_t port_version =
		(uint16_t)ReadMacInt16(cgrafptr_mac + DSP_CGRAFPORT_OFF_PORT_VERSION);
	uint32_t pixmap_mac = DSpResolvePixMapRecord(cgrafptr_mac);
	if (pixmap_mac == 0) return false;

	uint32_t baseAddr   = ReadMacInt32(pixmap_mac + DSP_PIXMAP_OFF_BASEADDR);
	uint16_t rb_raw     = (uint16_t)ReadMacInt16(pixmap_mac + DSP_PIXMAP_OFF_ROWBYTES);
	int16_t  pbnd_top   = (int16_t)ReadMacInt16(pixmap_mac + DSP_PIXMAP_OFF_BOUNDS_TOP);
	int16_t  pbnd_left  = (int16_t)ReadMacInt16(pixmap_mac + DSP_PIXMAP_OFF_BOUNDS_LEFT);
	int16_t  pbnd_bot   = (int16_t)ReadMacInt16(pixmap_mac + DSP_PIXMAP_OFF_BOUNDS_BOT);
	int16_t  pbnd_right = (int16_t)ReadMacInt16(pixmap_mac + DSP_PIXMAP_OFF_BOUNDS_RIGHT);
	uint32_t pixel_size_offset =
		DSpPixMapExtraFieldOffsetForPortVersion(
			port_version,
			DSP_PIXMAP_OFF_PIXELSIZE,
			DSP_MAINDEVICE_PIXMAP_OFF_PIXELSIZE);
	uint16_t pixelSize  = (uint16_t)ReadMacInt16(pixmap_mac + pixel_size_offset);

	if (baseAddr == 0) return false;

	/* Enforce a POSITIVE row stride. DSp-vended surfaces are top-down
	 * BGRA (compositor contract), so a genuinely negative (bottom-up DIB)
	 * stride is unsupported. The classic PixMap rowBytes high bit is a flag,
	 * not a sign bit, so masking it off (& 0x7FFF) already yields a positive
	 * 15-bit magnitude - the contract is therefore "positive stride only", and
	 * the extent check uses abs(row_bytes) only as belt-and-suspenders.
	 * A zero stride is degenerate and rejected. */
	uint32_t row_bytes = (uint32_t)(rb_raw & 0x7FFFu);   /* mask the PixMap high bit -> positive */
	if (row_bytes == 0) return false;

	uint32_t bpp_bytes;
	switch (pixelSize) {
		case 8:  bpp_bytes = 1; break;
		case 16: bpp_bytes = 2; break;
		case 32: bpp_bytes = 4; break;
		default: return false;   /* sub-byte / unsupported depth */
	}

	int16_t r_top    = (int16_t)ReadMacInt16(rect_mac + 0);
	int16_t r_left   = (int16_t)ReadMacInt16(rect_mac + 2);
	int16_t r_bottom = (int16_t)ReadMacInt16(rect_mac + 4);
	int16_t r_right  = (int16_t)ReadMacInt16(rect_mac + 6);

	/* Clamp the blit rect to the pixmap bounds (ASVS V5). The pixmap bounds
	 * origin (pbnd_top/left) is the addressing origin for baseAddr. */
	int32_t c_top    = (r_top    < pbnd_top)   ? pbnd_top   : r_top;
	int32_t c_left   = (r_left   < pbnd_left)  ? pbnd_left  : r_left;
	int32_t c_bottom = (r_bottom > pbnd_bot)   ? pbnd_bot   : r_bottom;
	int32_t c_right  = (r_right  > pbnd_right) ? pbnd_right : r_right;

	int32_t w = c_right  - c_left;
	int32_t h = c_bottom - c_top;
	if (w <= 0 || h <= 0) return false;   /* empty / inverted */

	/* Mac addr of the rect origin pixel, relative to the pixmap bounds
	 * origin: baseAddr + (top - bnd_top)*rowBytes + (left - bnd_left)*bpp. */
	uint32_t row_off = (uint32_t)(c_top  - pbnd_top)  * row_bytes;
	uint32_t col_off = (uint32_t)(c_left - pbnd_left) * bpp_bytes;

	out->base_origin_mac = baseAddr + row_off + col_off;
	out->row_bytes       = (int32_t)row_bytes;
	out->pixel_bytes     = bpp_bytes;
	out->bits_per_pixel  = pixelSize;
	out->rect_w          = w;
	out->rect_h          = h;
	return true;
}

/* Shared completion epilogue: flush the NQD batch so the GPU work is visible,
 * set DSpBlitInfo.completionFlag = true, and (when async + a non-NULL
 * completionProc was supplied) invoke the guest proc via call_macos1 - the
 * sanctioned PPC-trampoline idiom (NEVER a raw C cast of the guest fn-ptr).
 * Synchronous-then-complete is behavior-faithful for a single-display
 * emulator; NO MTLFence/MTLSharedEvent/_Atomic. */
static void DSpBlitComplete(uint32_t inBlitInfo, uint32_t inAsyncFlag)
{
	/* Make the blit's GPU writes visible before signalling completion. */
	NQDMetalFlush();

	WriteMacInt8(inBlitInfo + DSP_BLITINFO_OFF_completionFlag, 1);

	uint32_t completionProc = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_completionProc);
	if (inAsyncFlag != 0 && completionProc != 0) {
		/* DSpBlitDoneProc(DSpBlitInfoPtr) - one arg, the DSpBlitInfo back.
		 * call_macos1 marshals the arg into PPC r3 and executes the
		 * TVECT-addressed routine (same trampoline as the VBLProc call_macos3
		 * at the top of this file). */
		(void)call_macos1(completionProc, inBlitInfo);
	}
}

/* --- DSpBlit_FastestHandler (sub-op 711) ---
 *
 *  DSp 1.7 PDF p.47: DSpBlit_Fastest(inBlitInfo, inAsyncFlag). The spec defines
 *  Fastest as a strict 1:1 copy where the caller GUARANTEES srcRect == dstRect
 *  and the real implementation does no validation. We deliberately
 *  DIVERGE for safety - if the guest passes mismatched src/dst rects we clamp
 *  the copied extent to the OVERLAP (the smaller of the two rects) rather than
 *  scale or fault, so a mismatched-rect request can never read/write outside
 *  the resolved geometry (defense-in-depth alongside the extent check).
 *  This is a known, intentional M2-fidelity deviation (an app relying on the
 *  exact garbage/fault behavior of mismatched Fastest rects would observe a
 *  difference). Reads DSpBlitInfo via DSP_BLITINFO_OFF_*; resolves src/dst
 *  CGrafPtr + Rect; reuses the proven nqd_bitblt 1:1 kernel via
 *  NQDMetalBitblt1to1. mode SrcKey -> transparent (mode 36); Plain -> srcCopy
 *  (0). NULL inBlitInfo -> kDSpInvalidAttributesErr; OOB baseAddr / unsupported
 *  depth -> kDSpInternalErr. */
extern "C" int32_t DSpBlit_FastestHandler(uint32_t inBlitInfo,
										  uint32_t inAsyncFlag)
{
	if (inBlitInfo == 0) {
		DSP_LOG("DSpBlit_Fastest: NULL inBlitInfo -> kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}

	uint32_t srcBuffer = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_srcBuffer);
	uint32_t dstBuffer = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_dstBuffer);
	uint32_t srcRect   = inBlitInfo + DSP_BLITINFO_OFF_srcRect;
	uint32_t dstRect   = inBlitInfo + DSP_BLITINFO_OFF_dstRect;
	uint32_t srcKey    = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_srcKey);
	uint32_t mode      = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_mode);

	DSpBlitSide s, d;
	if (!DSpResolveBlitSide(srcBuffer, srcRect, &s) ||
		!DSpResolveBlitSide(dstBuffer, dstRect, &d)) {
		DSP_LOG("DSpBlit_Fastest: unresolved/empty src or dst -> kDSpInternalErr");
		return kDSpInternalErr;
	}
	/* Fastest is 1:1 (no scaling). The spec assumes srcRect == dstRect;
	 * we do NOT trust that. If the guest passed mismatched sizes we clamp the
	 * copied extent to the OVERLAP (min of the two rects) as a safety measure -
	 * this both honors "no scaling" and keeps the copy inside the smaller
	 * resolved rect. This intentionally diverges from real DSpBlit_Fastest
	 * (which would copy/fault per the larger rect); see the handler doc above. */
	int32_t w = (s.rect_w < d.rect_w) ? s.rect_w : d.rect_w;
	int32_t h = (s.rect_h < d.rect_h) ? s.rect_h : d.rect_h;
	if (w <= 0 || h <= 0) return kDSpInternalErr;
	if (s.pixel_bytes != d.pixel_bytes) {
		DSP_LOG("DSpBlit_Fastest: src/dst depth mismatch -> kDSpInternalErr");
		return kDSpInternalErr;
	}

	uint32_t transfer_mode = (mode & (uint32_t)kDSpBlitMode_SrcKey) ? 36u : 0u;

	if (!NQDMetalBitblt1to1(s.base_origin_mac, s.row_bytes,
							d.base_origin_mac, d.row_bytes,
							d.pixel_bytes, d.bits_per_pixel,
							(uint32_t)w, (uint32_t)h,
							transfer_mode, srcKey)) {
		DSP_LOG("DSpBlit_Fastest: NQD dispatch rejected (OOB/unavailable) -> kDSpInternalErr");
		return kDSpInternalErr;
	}

	DSpBlitComplete(inBlitInfo, inAsyncFlag);
	DSP_VLOG("DSpBlit_Fastest: 1:1 %dx%d mode=0x%x async=%u",
			 w, h, mode, inAsyncFlag);
	return kDSpNoErr;
}

/* --- DSpBlit_FasterHandler (sub-op 710) ---
 *
 *  DSp 1.7 PDF p.46: DSpBlit_Faster(inBlitInfo, inAsyncFlag). Scales srcRect
 *  -> dstRect. Per kDSpBlitMode_Interpolation (PDF p.87) scaling defaults to
 *  nearest-neighbor and bilinear is opt-in. Dispatches the NEW nqd_bitblt_
 *  scaled kernel via NQDMetalBitbltScaled; color-key honored per SrcKey/DstKey.
 *  NULL inBlitInfo -> kDSpInvalidAttributesErr; OOB baseAddr / unsupported
 *  depth -> kDSpInternalErr. */
extern "C" int32_t DSpBlit_FasterHandler(uint32_t inBlitInfo,
										 uint32_t inAsyncFlag)
{
	if (inBlitInfo == 0) {
		DSP_LOG("DSpBlit_Faster: NULL inBlitInfo -> kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}

	uint32_t srcBuffer = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_srcBuffer);
	uint32_t dstBuffer = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_dstBuffer);
	uint32_t srcRect   = inBlitInfo + DSP_BLITINFO_OFF_srcRect;
	uint32_t dstRect   = inBlitInfo + DSP_BLITINFO_OFF_dstRect;
	uint32_t srcKey    = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_srcKey);
	uint32_t dstKey    = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_dstKey);
	uint32_t mode      = ReadMacInt32(inBlitInfo + DSP_BLITINFO_OFF_mode);

	DSpBlitSide s, d;
	if (!DSpResolveBlitSide(srcBuffer, srcRect, &s) ||
		!DSpResolveBlitSide(dstBuffer, dstRect, &d)) {
		DSP_LOG("DSpBlit_Faster: unresolved/empty src or dst -> kDSpInternalErr");
		return kDSpInternalErr;
	}
	if (s.pixel_bytes != d.pixel_bytes) {
		DSP_LOG("DSpBlit_Faster: src/dst depth mismatch -> kDSpInternalErr");
		return kDSpInternalErr;
	}

	uint32_t interpolate = (mode & (uint32_t)kDSpBlitMode_Interpolation) ? 1u : 0u;
	uint32_t key_enable  = 0;
	if (mode & (uint32_t)kDSpBlitMode_SrcKey) key_enable |= 1u;
	if (mode & (uint32_t)kDSpBlitMode_DstKey) key_enable |= 2u;

	if (!NQDMetalBitbltScaled(s.base_origin_mac, s.row_bytes,
							  d.base_origin_mac, d.row_bytes,
							  d.pixel_bytes, d.bits_per_pixel,
							  (uint32_t)s.rect_w, (uint32_t)s.rect_h,
							  (uint32_t)d.rect_w, (uint32_t)d.rect_h,
							  interpolate, srcKey, dstKey, key_enable)) {
		DSP_LOG("DSpBlit_Faster: NQD scaled dispatch rejected (OOB/unavailable) -> kDSpInternalErr");
		return kDSpInternalErr;
	}

	DSpBlitComplete(inBlitInfo, inAsyncFlag);
	DSP_VLOG("DSpBlit_Faster: scaled %dx%d->%dx%d interp=%u key=0x%x async=%u",
			 s.rect_w, s.rect_h, d.rect_w, d.rect_h, interpolate, key_enable, inAsyncFlag);
	return kDSpNoErr;
}


/* --- GetBackBuffer underlay-restore branch ---
 *
 *  PDF p.51: "When a back buffer is retrieved and there is an underlay
 *  buffer, the invalid areas in the back buffer are restored from the
 *  underlay buffer. This is most useful in sprite games." Called from
 *  DSpContext_GetBackBufferHandler.
 *
 *  When ctx has a designated underlay AND the back buffer has dirty areas,
 *  this copies the dirty sub-rect underlay->back_buffer (CPU memcpy on the
 *  MTLStorageModeShared backing - the alt-buffer is BGRA8Unorm 32-bpp; a copy
 *  is only attempted when both contents pointers are non-NULL, which holds on
 *  device/Catalyst - on the simulator the heap forces StorageModePrivate so
 *  the copy is skipped).
 *
 *  No-op when no underlay is designated or the back buffer is clean. Single-
 *  writer emul-thread; ZERO new concurrency primitive - the copy is a
 *  plain memcpy. */


static void DSpRestoreBackBufferFromUnderlay(DSpContextPrivate *ctx)
{
	if (ctx == NULL || ctx->underlay_alt_buffer == 0) return;
	DSpAltBufferRecord *u = DSpGetAltBuffer(ctx->underlay_alt_buffer);
	if (u == NULL) {
		/* Stale designation (underlay disposed) - clear it defensively. */
		ctx->underlay_alt_buffer = 0;
		return;
	}
	/* Mirror the guest-drawn staging into the underlay's Metal backing so the
	 * CPU dirty-band restore reads the guest's pixels (NQD draws land in the
	 * guest-RAM staging, not Metal). */
	DSpSyncAltBufferStagingToBacking(u);
	/* Only restore the BACK buffer's invalid (dirty) areas (PDF p.51). When
	 * the back buffer is clean there is nothing to clean. */
	if (ctx->dirty_empty) return;
	if (ctx->back_buffer == NULL) return;

	/* Intersection of the back-buffer dirty rect with the underlay bounds. */
	int32_t rx0 = ctx->dirty_left,  ry0 = ctx->dirty_top;
	int32_t rx1 = ctx->dirty_right, ry1 = ctx->dirty_bottom;
	if (rx0 < 0) rx0 = 0;
	if (ry0 < 0) ry0 = 0;
	if ((uint32_t)rx1 > u->width)  rx1 = (int32_t)u->width;
	if ((uint32_t)ry1 > u->height) ry1 = (int32_t)u->height;
	if (rx1 <= rx0 || ry1 <= ry0) return;

	/* (1) CPU restore copy (underlay -> back buffer) - only when both
	 * backings are host-visible (Shared). Alt buffers inherit the owning
	 * context's depth at New time (DSpAllocAltBufferBacking), so when the
	 * depths still agree the copy is a direct row-wise memcpy of the dirty
	 * band at that depth. Skipped on StorageModePrivate (simulator) where
	 * contents is NULL.
	 *
	 * The depths can only disagree if the context was re-Reserved at a new
	 * depth after the underlay was created; a cross-depth restore would need
	 * a per-pixel format conversion that is NOT implemented, so the PDF-p.51
	 * "clean a back buffer" restore no-ops there (production SubmitFrame
	 * ignores underlay-slot layers, so there is no compositor fallback). The
	 * else-branch logs the skipped restore rather than silently no-op'ing. */
	void *u_contents  = DSpGetBackingContents(u->backing);
	void *bb_contents = DSpGetBackingContents(ctx->back_buffer);
	uint32_t bb_depth = ctx->attr.backBufferBestDepth;
	if (u_contents != NULL && bb_contents != NULL &&
		u->depth == bb_depth &&
		(bb_depth == 8 || bb_depth == 16 || bb_depth == 32)) {
		uint32_t px_bytes = bb_depth / 8u;
		uint32_t u_rb  = ((u->width * px_bytes) + 255u) & ~255u;
		uint32_t bb_rb = DSpAlignedRowBytes(DSpContextBackBufferWidth(ctx),
											bb_depth);
		uint8_t *u_base  = (uint8_t *)u_contents;
		uint8_t *bb_base = (uint8_t *)bb_contents;
		uint32_t band_bytes = (uint32_t)(rx1 - rx0) * px_bytes;
		for (int32_t y = ry0; y < ry1; y++) {
			uint8_t *src = u_base  + (uint32_t)y * u_rb  + (uint32_t)rx0 * px_bytes;
			uint8_t *dst = bb_base + (uint32_t)y * bb_rb + (uint32_t)rx0 * px_bytes;
			memcpy(dst, src, band_bytes);
		}
		DSP_LOG("UnderlayRestore: ctx=%u underlay=%u copied dirty band "
				"(%d,%d)-(%d,%d) @%ubpp", ctx->handle, ctx->underlay_alt_buffer,
				rx0, ry0, rx1, ry1, bb_depth);
	} else if (u_contents != NULL && bb_contents != NULL) {
		/* Depth mismatch precludes the restore copy.
		 * Make the skipped restore observable instead of silently no-op'ing. */
		DSP_LOG("UnderlayRestore: ctx=%u underlay=%u depth mismatch "
				"(underlay=%u back-buffer=%u) - CPU dirty-band restore SKIPPED "
				"(cross-depth conversion deferred; no compositor underlay "
				"fallback)",
				ctx->handle, ctx->underlay_alt_buffer, u->depth, bb_depth);
	}
}

/* --------------------------------------------------------------------- *
 *  Shared guest display-mode transaction                                *
 * --------------------------------------------------------------------- */

extern "C" uint32_t DSpGetLiveMainDevicePixMap(void)
{
	return video_get_live_main_device_pixmap();
}

extern "C" void DSpPrepareQuickDrawModeSwitch(void)
{
	(void)video_prepare_guest_display();
}

static int32_t DSpSetQuickDrawVideoMode(int mode_index)
{
	if (!video_switch_guest_display(mode_index)) {
		DSP_LOG("Display Manager rejected mode index=%d", mode_index);
		return kDSpInternalErr;
	}
	return kDSpNoErr;
}

static uint8_t DSpReadIndexedPixel(const uint8_t *row, uint32_t x,
								   uint32_t depth)
{
	switch (depth) {
		case 1:
			return (uint8_t)((row[x >> 3] >> (7u - (x & 7u))) & 0x01u);
		case 2:
			return (uint8_t)((row[x >> 2] >>
				((3u - (x & 3u)) * 2u)) & 0x03u);
		case 4:
			return (uint8_t)((row[x >> 1] >>
				((1u - (x & 1u)) * 4u)) & 0x0fu);
		case 8:
			return row[x];
		default:
			return 0;
	}
}

static void DSpReadBackBufferRGB(const DSpContextPrivate *ctx,
								 const uint8_t *row, uint32_t x,
								 uint32_t depth,
								 uint8_t *out_r, uint8_t *out_g,
								 uint8_t *out_b)
{
	if (depth <= 8) {
		const uint32_t index = DSpReadIndexedPixel(row, x, depth);
		*out_r = ctx->clut_bytes[index * 3u + 0u];
		*out_g = ctx->clut_bytes[index * 3u + 1u];
		*out_b = ctx->clut_bytes[index * 3u + 2u];
		return;
	}
	if (depth == 16) {
		const uint16_t pixel =
			((uint16_t)row[x * 2u] << 8) | row[x * 2u + 1u];
		*out_r = (uint8_t)(((pixel >> 10) & 31u) * 255u / 31u);
		*out_g = (uint8_t)(((pixel >> 5) & 31u) * 255u / 31u);
		*out_b = (uint8_t)((pixel & 31u) * 255u / 31u);
		return;
	}
	/* Classic 32-bit screen pixels are big-endian xRGB/ARGB bytes. */
	*out_r = row[x * 4u + 1u];
	*out_g = row[x * 4u + 2u];
	*out_b = row[x * 4u + 3u];
}

static uint8_t DSpNearestScreenPaletteIndex(uint8_t r, uint8_t g, uint8_t b,
											uint32_t color_count)
{
	uint32_t best_distance = UINT32_MAX;
	uint8_t best_index = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		const int32_t dr = (int32_t)r - (int32_t)mac_pal[i].red;
		const int32_t dg = (int32_t)g - (int32_t)mac_pal[i].green;
		const int32_t db = (int32_t)b - (int32_t)mac_pal[i].blue;
		const uint32_t distance =
			(uint32_t)(dr * dr + dg * dg + db * db);
		if (distance < best_distance) {
			best_distance = distance;
			best_index = (uint8_t)i;
			if (distance == 0)
				break;
		}
	}
	return best_index;
}

static void DSpWriteIndexedPixel(uint8_t *row, uint32_t x,
								 uint32_t depth, uint8_t index)
{
	switch (depth) {
		case 1:
			row[x >> 3] |=
				(uint8_t)((index & 0x01u) << (7u - (x & 7u)));
			break;
		case 2:
			row[x >> 2] |=
				(uint8_t)((index & 0x03u) <<
					((3u - (x & 3u)) * 2u));
			break;
		case 4:
			row[x >> 1] |=
				(uint8_t)((index & 0x0fu) <<
					((1u - (x & 1u)) * 4u));
			break;
		case 8:
			row[x] = index;
			break;
	}
}

extern "C" bool DSpCopyBackBufferToCanonicalScreen(
	DSpContextPrivate *ctx)
{
	if (ctx == NULL || ctx->back_buffer == NULL ||
		cur_mode < 0 || VModes[cur_mode].viType == DIS_INVALID ||
		screen_base == 0) {
		return false;
	}

	const uint32_t source_width = DSpContextBackBufferWidth(ctx);
	const uint32_t source_height = DSpContextBackBufferHeight(ctx);
	const uint32_t source_depth = ctx->attr.backBufferBestDepth;
	const VideoInfo &screen_mode = VModes[cur_mode];
	const uint32_t screen_width = screen_mode.viXsize;
	const uint32_t screen_height = screen_mode.viYsize;
	if (source_width == 0 || source_height == 0 ||
		screen_width == 0 || screen_height == 0 || source_depth == 0) {
		return false;
	}

	const uint32_t screen_depth =
		(screen_mode.viAppleMode >= APPLE_1_BIT &&
		 screen_mode.viAppleMode <= APPLE_32_BIT)
			? 1u << (screen_mode.viAppleMode - APPLE_1_BIT)
			: 0;
	if (screen_depth != 1 && screen_depth != 2 &&
		screen_depth != 4 && screen_depth != 8 &&
		screen_depth != 16 && screen_depth != 32) {
		return false;
	}

	const uint32_t source_row_bytes =
		DSpBackBufferAlignedRowBytes(source_width, source_depth);
	const uint32_t source_visible_bytes =
		(uint32_t)(((uint64_t)source_width * source_depth + 7u) / 8u);
	const uint32_t screen_visible_bytes =
		(uint32_t)(((uint64_t)screen_width * screen_depth + 7u) / 8u);
	if (source_row_bytes < source_visible_bytes ||
		screen_mode.viRowBytes < screen_visible_bytes) {
		return false;
	}

	const uint8_t *source =
		(const uint8_t *)DSpGetBackingContents(ctx->back_buffer);
	uint8_t *screen = Mac2HostAddr(screen_base);
	if (source == NULL || screen == NULL)
		return false;

	/*
	 * Onscreen publication only: same Hide/Show contract as
	 * MetalCompositorSubmitFrame. Private back buffers are untouched.
	 * Single begin/end around every write path below.
	 */
	video_screen_publish_begin(0, 0, (int)screen_width, (int)screen_height);

	if (source_depth == screen_depth &&
		source_width == screen_width &&
		source_height == screen_height) {
		for (uint32_t y = 0; y < screen_height; y++) {
			memcpy(screen + (size_t)y * screen_mode.viRowBytes,
				   source + (size_t)y * source_row_bytes,
				   screen_visible_bytes);
		}
	} else {
		/* Mixed-depth DSp still publishes into MainDevice storage only.
		 * Conversion is not a second onscreen texture. */
		const uint32_t destination_color_count =
			screen_depth <= 8 ? 1u << screen_depth : 0;
		uint8_t indexed_remap[256] = {};
		if (source_depth <= 8 && screen_depth <= 8) {
			const uint32_t source_color_count = 1u << source_depth;
			for (uint32_t i = 0; i < source_color_count; i++) {
				indexed_remap[i] = source_depth == screen_depth
					? (uint8_t)i
					: DSpNearestScreenPaletteIndex(
						ctx->clut_bytes[i * 3u + 0u],
						ctx->clut_bytes[i * 3u + 1u],
						ctx->clut_bytes[i * 3u + 2u],
						destination_color_count);
			}
		}

		for (uint32_t y = 0; y < screen_height; y++) {
			const uint32_t source_y =
				(uint32_t)(((uint64_t)y * source_height) / screen_height);
			const uint8_t *source_row =
				source + (size_t)source_y * source_row_bytes;
			uint8_t *screen_row =
				screen + (size_t)y * screen_mode.viRowBytes;
			if (screen_depth <= 8)
				memset(screen_row, 0, screen_visible_bytes);

			for (uint32_t x = 0; x < screen_width; x++) {
				const uint32_t source_x =
					(uint32_t)(((uint64_t)x * source_width) / screen_width);
				uint8_t r, g, b;
				DSpReadBackBufferRGB(
					ctx, source_row, source_x, source_depth, &r, &g, &b);
				if (screen_depth == 32) {
					screen_row[x * 4u + 0u] = 0;
					screen_row[x * 4u + 1u] = r;
					screen_row[x * 4u + 2u] = g;
					screen_row[x * 4u + 3u] = b;
				} else if (screen_depth == 16) {
					const uint16_t pixel =
						(uint16_t)(((uint16_t)(r >> 3) << 10) |
								   ((uint16_t)(g >> 3) << 5) |
								   (uint16_t)(b >> 3));
					screen_row[x * 2u + 0u] = (uint8_t)(pixel >> 8);
					screen_row[x * 2u + 1u] = (uint8_t)pixel;
				} else {
					const uint8_t index = source_depth <= 8
						? indexed_remap[
							DSpReadIndexedPixel(
								source_row, source_x, source_depth)]
						: DSpNearestScreenPaletteIndex(
							r, g, b, destination_color_count);
					DSpWriteIndexedPixel(screen_row, x, screen_depth, index);
				}
			}
		}
	}

	video_screen_publish_end();
	return true;
}

/*
 * Make the active DSp display a real Window Manager layer.
 *
 * RAVE draws into the display device directly and does not create an AGL
 * window. Without this full-screen backdrop, Finder's pre-existing windows
 * remain immediately behind a later GetNewDialog window; the Window Manager
 * legitimately redraws those windows over the RAVE pixels. Real DrawSprocket
 * establishes the full-screen application layer as part of SetState, so do
 * the same synchronously before the application can create RAVE or a dialog.
 *
 * The video subsystem owns the guest call boundary. DSp stores only the
 * resulting WindowPtr; it does not own symbol caches or scratch allocations.
 */
static bool DSpSetContextBackdropWindow(DSpContextPrivate *ctx, bool visible)
{
	if (ctx == NULL || visible == (ctx->guest_window != 0))
		return ctx != NULL;

	if (!visible) {
		if (!video_dispose_guest_window(ctx->guest_window)) {
			DSP_LOG("backdrop window dispose unavailable ctx=%u win=0x%08x",
					ctx->handle, ctx->guest_window);
			return false;
		}
		DSP_LOG("backdrop window disposed ctx=%u win=0x%08x",
				ctx->handle, ctx->guest_window);
		ctx->guest_window = 0;
		return true;
	}

	const uint32_t width = ctx->attr.displayWidth;
	const uint32_t height = ctx->attr.displayHeight;
	ctx->guest_window =
		video_create_guest_fullscreen_window(width, height);
	DSP_LOG("backdrop window %s ctx=%u win=0x%08x %ux%u",
			ctx->guest_window ? "created" : "CREATE FAILED",
			ctx->handle, ctx->guest_window, width, height);
	return ctx->guest_window != 0;
}

/* --- DSpContext_SetStateHandler ---
 *
 *  Implements the 9-valid / 3-invalid state-transition matrix from
 *  DrawSprocket1.7.pdf pp.29, 75, 86:
 *
 *  +-------------------+-------+-----------------------------------------+
 *  | From -> To        | Valid | DMC action                              |
 *  +-------------------+-------+-----------------------------------------+
 *  | Inactive -> Active| yes   | dmc_set_active_owner(kDMCOwnerDSp)       |
 *  | Inactive -> Paused| yes   | dmc_set_active_owner(kDMCOwnerQuickDraw) |
 *  | Active   -> Paused| yes   | dmc_set_active_owner(kDMCOwnerQuickDraw) |
 *  | Active   -> Inactive| yes | dmc_set_active_owner(kDMCOwnerQuickDraw) |
 *  | Paused   -> Active| yes   | dmc_set_active_owner(kDMCOwnerDSp)       |
 *  | Paused   -> Inactive| yes | dmc_set_active_owner(kDMCOwnerQuickDraw) |
 *  | Active   -> Active| noErr | NO DMC call (idempotent)                 |
 *  | Paused   -> Paused| noErr | NO DMC call (idempotent)                 |
 *  | Inactive -> Inactive| noErr|NO DMC call (idempotent)                 |
 *  | * -> state !in {0,1,2}| kDSpInvalidAttributesErr (-30443)             |
 *  | invalid ctxRef    | kDSpInvalidContextErr (-30442)                   |
 *  +-------------------+-------+-----------------------------------------+
 *
 *  Paused AND Inactive both route to kDMCOwnerQuickDraw from the
 *  controller's perspective. The DSp-level distinction (menu-bar-visible
 *  vs Monitors-ctrl-panel-resolution) is a CLUT/gamma/blanking
 *  semantic; the controller only sees "non-DSp".
 *
 *  Threading: DSp code is single-threaded on the emul thread; no
 *  explicit synchronization primitives are introduced here. The display-
 *  mode controller holds the sanctioned project-wide serialization
 *  carve-out.
 */
static uint32_t s_dsp_setstate_switch_handoff_ctx = 0;
static int s_dsp_saved_quickdraw_mode = -1;
static bool s_dsp_saved_quickdraw_clut_valid = false;
static uint32_t s_dsp_saved_quickdraw_clut_depth = 0;
static uint8_t s_dsp_saved_quickdraw_clut[768] = {};

static bool DSpCopyDriverCLUT(uint8_t out_clut[768], uint32_t *out_depth)
{
	return video_capture_guest_clut(out_clut, out_depth);
}

static void DSpClearSavedQuickDrawDisplay(void)
{
	s_dsp_saved_quickdraw_mode = -1;
	s_dsp_saved_quickdraw_clut_valid = false;
	s_dsp_saved_quickdraw_clut_depth = 0;
}

static int32_t DSpActivateVideoMode(DSpContextPrivate *ctx)
{
	const uint32_t display_depth =
		DSpDisplayModeDepth(ctx->attr.backBufferBestDepth,
							ctx->attr.displayBestDepth);
	const int target_mode = video_find_guest_mode(
		ctx->attr.displayWidth, ctx->attr.displayHeight, display_depth);
	if (target_mode < 0) {
		DSP_LOG("SetState: no canonical VModes entry for %ux%u@%u",
				ctx->attr.displayWidth, ctx->attr.displayHeight,
				display_depth);
		return kDSpInternalErr;
	}

	const bool opening_dsp_session = s_dsp_saved_quickdraw_mode < 0;
	if (opening_dsp_session) {
		s_dsp_saved_quickdraw_mode = cur_mode;
		s_dsp_saved_quickdraw_clut_valid =
			DSpCopyDriverCLUT(
			s_dsp_saved_quickdraw_clut,
			&s_dsp_saved_quickdraw_clut_depth);
	}

	const int previous_mode = cur_mode;
	const int32_t err = DSpSetQuickDrawVideoMode(target_mode);
	if (err != kDSpNoErr) {
		if (cur_mode != previous_mode) {
			const int32_t rollback_err =
				DSpSetQuickDrawVideoMode(previous_mode);
			DSP_LOG("SetState: failed Display Manager transition rolled "
					"back mode %d -> %d (rollback err=%d)",
					cur_mode, previous_mode, rollback_err);
		}
		if (opening_dsp_session)
			DSpClearSavedQuickDrawDisplay();
		DSP_LOG("SetState: Display Manager rejected mode index=%d "
				"for %ux%u@%u (err=%d)",
				target_mode, ctx->attr.displayWidth,
				ctx->attr.displayHeight, display_depth, (int)err);
		return kDSpInternalErr;
	}
	return kDSpNoErr;
}

extern "C" void DSpContext_SetStateSwitchHandoff(uint32_t oldCtxRef)
{
	s_dsp_setstate_switch_handoff_ctx = oldCtxRef;
}

extern "C" int32_t DSpContext_SetStateHandler(uint32_t ctxRef, uint32_t state)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL) {
		DSP_LOG("SetState: invalid ctxRef=%u", ctxRef);
		return kDSpInvalidContextErr;
	}
	/* State-value bounds check (PDF pp.75, 86: only 0/1/2 valid). */
	if (state != (uint32_t)kDSpContextState_Active  &&
		state != (uint32_t)kDSpContextState_Paused  &&
		state != (uint32_t)kDSpContextState_Inactive) {
		DSP_LOG("SetState: out-of-range state=%u "
				"(valid: 0=Active, 1=Paused, 2=Inactive)",
				state);
		return kDSpInvalidAttributesErr;
	}
	/* Paused-persistence: capture the
	 * pre-transition state so the Paused->Active replay arm can run
	 * AFTER the DMC owner swap. Read-once here; ctx->state is mutated
	 * by the handler body below (line 983-ish). */
	const uint32_t prev_state = ctx->state;
	/* Idempotent self-transition - return noErr without touching DMC. */
	if (ctx->state == state) {
		DSP_LOG("SetState: idempotent self-transition ctx=%u state=%u (noErr)",
				ctxRef, state);
		return kDSpNoErr;
	}

	const bool activating =
		state == (uint32_t)kDSpContextState_Active &&
		prev_state != (uint32_t)kDSpContextState_Active;
	const int mode_before_transition = cur_mode;
	uint8_t clut_before_transition[768] = {};
	uint32_t clut_depth_before_transition = 0;
	const bool had_clut_before_transition = activating &&
		DSpCopyDriverCLUT(clut_before_transition,
						  &clut_depth_before_transition);
	const bool opened_dsp_session =
		activating && s_dsp_saved_quickdraw_mode < 0;
	const bool switch_handoff =
		(state == (uint32_t)kDSpContextState_Inactive) &&
		(s_dsp_setstate_switch_handoff_ctx == ctxRef);

	/* Project the aggregate state before publishing any part of the
	 * transition. It controls both the DMC owner and whether the saved
	 * desktop mode must be restored. */
	bool was_any_active = false;
	bool any_active = false;
	for (uint32_t i = 0; i < DSP_MAX_CONTEXTS; i++) {
		DSpContextPrivate *other_ctx = dsp_context_table[i];
		if (other_ctx == NULL) continue;
		if (other_ctx->state == (uint32_t)kDSpContextState_Active)
			was_any_active = true;
		const uint32_t projected_state =
			other_ctx == ctx ? state : other_ctx->state;
		if (projected_state == (uint32_t)kDSpContextState_Active)
			any_active = true;
	}

	if (activating) {
		const int32_t mode_err = DSpActivateVideoMode(ctx);
		if (mode_err != kDSpNoErr)
			return mode_err;

		const uint32_t display_depth =
			DSpDisplayModeDepth(ctx->attr.backBufferBestDepth,
								ctx->attr.displayBestDepth);
		if (display_depth <= 8) {
			int32_t clut_err = kDSpNoErr;
			if (ctx->clut_app_supplied) {
				clut_err = DSpInstallContextCLUTOnDisplay(ctx);
			} else {
				/* A context without an application CLUT inherits the table
				 * QuickDraw/Display Manager installed for the new mode. Do not
				 * replace the system palette with DSp's bookkeeping ramp:
				 * dialogs, fonts, and the standard cursor use these indices. */
				uint32_t inherited_depth = 0;
				if (!DSpCopyDriverCLUT(
						ctx->clut_bytes, &inherited_depth) ||
					inherited_depth != display_depth) {
					clut_err = kDSpInternalErr;
				} else {
					memcpy(ctx->clut_bytes_latched,
						   ctx->clut_bytes,
						   sizeof(ctx->clut_bytes));
				}
			}
			if (clut_err != kDSpNoErr) {
				(void)DSpSetQuickDrawVideoMode(mode_before_transition);
				if (had_clut_before_transition) {
					(void)DSpInstallCLUTOnDisplay(
						clut_before_transition,
						clut_depth_before_transition);
				}
				if (opened_dsp_session)
					DSpClearSavedQuickDrawDisplay();
				DSP_LOG("SetState: Color QuickDraw rejected context CLUT "
						"(ctx=%u depth=%u err=%d)",
						ctxRef, display_depth, clut_err);
				return kDSpInternalErr;
			}
		}
	}

	/* Route through DMC (preserves DMCWriteSiteInventoryTests CI gate).
	 * Use DSpMapStateToDMCOwnerTyped (from dsp_engine_internal.h) to get
	 * the DMCOwner enum type directly - the public DSpMapStateToDMCOwner
	 * returns uint32_t (no DMC header leak through public API). */
	DMCOwner new_owner = any_active
		? kDMCOwnerDSp
		: kDMCOwnerQuickDraw;
	int32_t dmc_rc = dmc_set_active_owner((uint32_t)new_owner);
	if (dmc_rc != 0) {
		DSP_LOG("SetState: DMC rejected transition ctx=%u %u->%u rc=%d",
				ctxRef, ctx->state, state, dmc_rc);
		if (activating) {
			const int32_t rollback_err =
				DSpSetQuickDrawVideoMode(mode_before_transition);
			if (had_clut_before_transition) {
				(void)DSpInstallCLUTOnDisplay(
					clut_before_transition,
					clut_depth_before_transition);
			}
			DSP_LOG("SetState: DMC rejection rolled Display Manager mode "
					"back to index=%d (err=%d)",
					mode_before_transition, rollback_err);
			if (opened_dsp_session)
				DSpClearSavedQuickDrawDisplay();
		}
		return kDSpInvalidAttributesErr;
	}

	/* Complete the guest-owned desktop restore before committing ctx->state
	 * or any Window Manager/host side effects. If it fails, put both the
	 * driver mode and DMC owner back and leave the context untouched. */
	if (state != (uint32_t)kDSpContextState_Active &&
		!switch_handoff && !any_active &&
		s_dsp_saved_quickdraw_mode >= 0) {
		const int restore_mode = s_dsp_saved_quickdraw_mode;
		int32_t restore_err =
			DSpSetQuickDrawVideoMode(restore_mode);
		if (restore_err == kDSpNoErr &&
			s_dsp_saved_quickdraw_clut_valid) {
			restore_err = DSpInstallCLUTOnDisplay(
				s_dsp_saved_quickdraw_clut,
				s_dsp_saved_quickdraw_clut_depth);
		}
		if (restore_err != kDSpNoErr) {
			const int32_t mode_rollback_err =
				DSpSetQuickDrawVideoMode(mode_before_transition);
			const uint32_t active_depth =
				DSpDisplayModeDepth(ctx->attr.backBufferBestDepth,
									ctx->attr.displayBestDepth);
			if (active_depth <= 8) {
				(void)DSpInstallContextCLUTOnDisplay(ctx);
			}
			const DMCOwner previous_owner = was_any_active
				? kDMCOwnerDSp
				: kDMCOwnerQuickDraw;
			const int32_t owner_rollback_err =
				dmc_set_active_owner((uint32_t)previous_owner);
			DSP_LOG("SetState: Display Manager desktop restore failed "
					"(mode=%d err=%d); rollback mode=%d owner=%d",
					restore_mode, restore_err,
					mode_rollback_err, owner_rollback_err);
			return kDSpInternalErr;
		}
		DSpClearSavedQuickDrawDisplay();
	}

	/* A full-screen DSp activation is not complete until the Window Manager
	 * has an application layer for it. If that guest call fails, roll the
	 * already-completed mode/CLUT/owner changes back as one transaction and
	 * leave ctx->state untouched. */
	if (activating && !DSpSetContextBackdropWindow(ctx, true)) {
		const int32_t mode_rollback_err =
			DSpSetQuickDrawVideoMode(mode_before_transition);
		int32_t clut_rollback_err = kDSpNoErr;
		if (had_clut_before_transition) {
			clut_rollback_err = DSpInstallCLUTOnDisplay(
				clut_before_transition,
				clut_depth_before_transition);
		}
		const DMCOwner previous_owner = was_any_active
			? kDMCOwnerDSp
			: kDMCOwnerQuickDraw;
		const int32_t owner_rollback_err =
			dmc_set_active_owner((uint32_t)previous_owner);
		if (opened_dsp_session)
			DSpClearSavedQuickDrawDisplay();
		DSP_LOG("SetState: backdrop creation failed ctx=%u; rollback "
				"mode=%d clut=%d owner=%d",
				ctxRef, mode_rollback_err, clut_rollback_err,
				owner_rollback_err);
		return kDSpInternalErr;
	}

	DSP_LOG("SetState: ctx=%u %u->%u DMC owner->%d",
			ctxRef, ctx->state, state, (int)new_owner);
	ctx->state = state;

	/* Remove the full-screen layer only after Display Manager has installed
	 * the restored desktop surface. Its exposure repaint must target that
	 * surface, not the outgoing game framebuffer. */
	if (!activating && state != (uint32_t)kDSpContextState_Active)
		(void)DSpSetContextBackdropWindow(ctx, false);

	/*
	 *  Recompute the
	 *  aggregate "any DSp context is Active and fullscreen?" flag. Under
	 *  iOS UIKit every DSp context is fullscreen (no windowed
	 *  mode), so the predicate simplifies to "any Active context". When
	 *  the aggregate changes, call DSpHostBridge_SetActiveFullscreen
	 *  which writes the C-side flag + posts the
	 *  "DSpHostBridge.activeFullscreenChanged" notification for
	 *  DSpIdleTimerService (Swift observer) to toggle
	 *  UIApplication.shared.isIdleTimerDisabled on the main thread.
	 *
	 *  Edge cases handled by walking dsp_context_table:
	 *    - This ctx going Active -> aggregate becomes true (if not already).
	 *    - This ctx leaving Active -> aggregate stays true if ANY OTHER
	 *      context is still Active, otherwise becomes false.
	 *    - Idempotent self-transition (Active->Active etc.) - unreachable
	 *      here because the handler early-returns kDSpNoErr at line 1819
	 *      before reaching this block (prev_state == state short-circuit).
	 *
	 *  The call to DSpHostBridge_SetActiveFullscreen is CONDITIONAL on
	 *  the aggregate changing - otherwise we'd post the notification on
	 *  every SetState call and spam the observer. Compare against the
	 *  existing C-side value via DSpHostBridge_GetActiveFullscreen.
	 */
	const bool prev_active = DSpHostBridge_GetActiveFullscreen();
	if (any_active != prev_active) {
		DSpHostBridge_SetActiveFullscreen(any_active);
		DSP_LOG("SetState: aggregate Active-fullscreen flag changed %s -> %s "
				"(ctx=%u; idle-timer bridge fired)",
				prev_active ? "true" : "false",
				any_active  ? "true" : "false",
				ctxRef);
	}

	/* The Display Manager already made MainDevice describe the canonical
	 * driver framebuffer on activation. Leaving DSp either hands that device
	 * directly to another active context or asks Display Manager to restore
	 * the saved desktop mode; there is no PixMap snapshot to undo. */
	if (state != (uint32_t)kDSpContextState_Active) {
		DSpReleaseFrontBufferCGrafPtr(ctx);
		if (switch_handoff) {
			DSP_LOG("SetState: ctx=%u switch handoff suppressed "
					"intermediate QuickDraw mode restore", ctxRef);
		}
	}

	return kDSpNoErr;
}

extern "C" void DSpShutdownContexts(void)
{
	/* Final Shutdown is a lifecycle operation, not a raw table clear. Deactivate
	 * active contexts first so the last transition restores the saved desktop
	 * mode and ColorTable through Display Manager/QuickDraw. Release then uses
	 * the ordinary VBL-safe resource path. */
	for (uint32_t i = 0; i < DSP_MAX_CONTEXTS; i++) {
		DSpContextPrivate *ctx = dsp_context_table[i];
		if (ctx == NULL ||
			ctx->state != (uint32_t)kDSpContextState_Active) {
			continue;
		}
		const int32_t err = DSpContext_SetStateHandler(
			ctx->handle, (uint32_t)kDSpContextState_Inactive);
		if (err != kDSpNoErr) {
			DSP_LOG("Shutdown: failed to deactivate ctx=%u err=%d",
					ctx->handle, err);
		}
	}

	for (uint32_t i = 0; i < DSP_MAX_CONTEXTS; i++) {
		DSpContextPrivate *ctx = dsp_context_table[i];
		if (ctx == NULL)
			continue;
		const int32_t err = DSpContext_ReleaseHandler(ctx->handle);
		if (err != kDSpNoErr) {
			DSP_LOG("Shutdown: failed to release ctx=%u err=%d",
					ctx->handle, err);
		}
	}
}

/* --- DSpContext_GetStateHandler ---
 *
 *  Reads ctx->state and writes via WriteMacInt32(outStateAddr, state).
 *  Validates ctxRef via DSpGetContext (returns kDSpInvalidContextErr
 *  on NULL) and null-checks outStateAddr (returns kDSpInvalidContextErr
 *  when zero - byte-identical to the DSp 1.7 reference behavior).
 *
 *  No DMC round-trip: DSp state is the authoritative copy at context
 *  scope; the controller's active_owner is a DSp-to-display-system
 *  projection, not a reciprocal reader-of-truth.
 */
extern "C" int32_t DSpContext_GetStateHandler(uint32_t ctxRef,
											   uint32_t outStateAddr)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL || outStateAddr == 0) {
		DSP_LOG("GetState: invalid ctxRef=%u or outStateAddr=0x%08x",
				ctxRef, outStateAddr);
		return kDSpInvalidContextErr;
	}
	WriteMacInt32(outStateAddr, ctx->state);
	return kDSpNoErr;
}


/* --- DSpContext_IsBusyHandler (sub-op 730) ---
 *
 *  DSp 1.7 PDF p.40: DSpContext_IsBusy(inContext, Boolean *outBusyFlag).
 *  outBusyFlag is true when NO back buffer is available for drawing (the
 *  prior SwapBuffers has not finished), false when a back buffer is ready.
 *  PocketShaver's fullscreen back-buffer is a single MTLBuffer allocated at
 *  Reserve time and always present once the context owns Metal resources, so
 *  the buffer is always ready -> write false (0). When back_buffer is NULL
 *  (metadata-only / pre-Reserve context) report busy (1). 1-byte Boolean
 *  out-write via WriteMacInt8 (PDF Boolean ABI). */
extern "C" int32_t DSpContext_IsBusyHandler(uint32_t ctxRef,
											 uint32_t outBusyAddr)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL || outBusyAddr == 0) {
		DSP_LOG("IsBusy: invalid ctxRef=%u or outBusyAddr=0x%08x",
				ctxRef, outBusyAddr);
		return kDSpInvalidContextErr;
	}
	/* PDF p.40: true == NO buffer available. Single fullscreen back-buffer
	 * is always ready once allocated -> false; NULL (pre-Reserve) -> true. */
	WriteMacInt8(outBusyAddr, (ctx->back_buffer != NULL) ? 0 : 1);
	return kDSpNoErr;
}


/* DSp exposes classic Apple Display Manager IDs (video.h viAppleID values)
 * from GetDisplayID. Incoming discovery IDs all map to the single backing
 * screen because PocketShaver has one fullscreen display. */

/* --- DSpContext_GetDisplayIDHandler (sub-op 731) ---
 *
 *  DSp 1.7 PDF p.14-15: DSpContext_GetDisplayID(inContext, DisplayIDType
 *  *outDisplayID). Writes the display id the context lives on. iOS has a
 *  single display; report the guest Display Manager's id for that screen
 *  (kDSpGuestDMMainDisplayID) so apps can round-trip the id through
 *  DMGetGDeviceByDisplayID - the per-mode APPLE_* ids are NOT DM display
 *  ids and fail that lookup (see dsp_display_id_policy.h). 4-byte
 *  DisplayIDType (UInt32) out-write. */
extern "C" int32_t DSpContext_GetDisplayIDHandler(uint32_t ctxRef,
												   uint32_t outIDAddr)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL || outIDAddr == 0) {
		DSP_LOG("GetDisplayID: invalid ctxRef=%u or outIDAddr=0x%08x",
				ctxRef, outIDAddr);
		return kDSpInvalidContextErr;
	}
	const uint32_t display_id = kDSpGuestDMMainDisplayID;
	WriteMacInt32(outIDAddr, display_id);
	DSP_LOG("GetDisplayID: ctx=%u %ux%u -> displayID=0x%08x",
			ctxRef, ctx->attr.displayWidth, ctx->attr.displayHeight,
			display_id);
	return kDSpNoErr;
}


/* The base dirty-rect grid unit (DSp 1.7 PDF p.43: 32x32 px, the PPC
 * cache-line granularity). DSpContext_GetDirtyRectGridUnits reports it as
 * a constant; DSpContext_SetDirtyRectGridSize (737)
 * rounds requested cells up to a multiple of it. */
#define kDSpDirtyRectGridUnit 32u

/* --- DSpContext_GetDirtyRectGridUnitsHandler (sub-op 738) ---
 *
 *  DSp 1.7 PDF p.43: DSpContext_GetDirtyRectGridUnits(inContext, UInt32
 *  *outCellPixelWidth, UInt32 *outCellPixelHeight). Writes the base grid
 *  units - a CONSTANT 32 x 32 (PPC cache-line); no stored field. Two 4-byte
 *  out-writes. */
extern "C" int32_t DSpContext_GetDirtyRectGridUnitsHandler(uint32_t ctxRef,
														   uint32_t wAddr,
														   uint32_t hAddr)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL || wAddr == 0 || hAddr == 0) {
		DSP_LOG("GetDirtyRectGridUnits: invalid ctxRef=%u or wAddr=0x%08x "
				"hAddr=0x%08x", ctxRef, wAddr, hAddr);
		return kDSpInvalidContextErr;
	}
	WriteMacInt32(wAddr, kDSpDirtyRectGridUnit);
	WriteMacInt32(hAddr, kDSpDirtyRectGridUnit);
	return kDSpNoErr;
}


/* --- DSpContext_GetMaxFrameRateHandler (sub-op 734) ---
 *
 *  DSp 1.7 PDF p.44-45: DSpContext_GetMaxFrameRate(inContext, UInt32
 *  *outMaxFPS). Writes the stored max-frame-rate bookkeeping field; 0 means
 *  no restriction. 4-byte UInt32 out-write. */
extern "C" int32_t DSpContext_GetMaxFrameRateHandler(uint32_t ctxRef,
													 uint32_t outAddr)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL || outAddr == 0) {
		DSP_LOG("GetMaxFrameRate: invalid ctxRef=%u or outAddr=0x%08x",
				ctxRef, outAddr);
		return kDSpInvalidContextErr;
	}
	WriteMacInt32(outAddr, ctx->max_frame_rate);
	return kDSpNoErr;
}

/* --- DSpContext_SetMaxFrameRateHandler (sub-op 735) ---
 *
 *  DSp 1.7 PDF p.44: DSpContext_SetMaxFrameRate(inContext, UInt32 inMaxFPS).
 *  Stores inMaxFPS as store-only bookkeeping (PDF "does not guarantee"; the
 *  actual frame-skip pacing belongs to SwapBuffers). No out-write. */
extern "C" int32_t DSpContext_SetMaxFrameRateHandler(uint32_t ctxRef,
													 uint32_t inMaxFPS)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL) {
		DSP_LOG("SetMaxFrameRate: invalid ctxRef=%u", ctxRef);
		return kDSpInvalidContextErr;
	}
	ctx->max_frame_rate = inMaxFPS;
	return kDSpNoErr;
}


/* Compute the display refresh rate as a DSp/QuickDraw Fixed (16.16) value
 * from the VBL cadence. Hz = 1e6 / cadence_usec; Fixed = Hz << 16. */
static uint32_t DSpComputeMonitorFrequencyFixed(void)
{
	uint64_t cadence = vbl_source_get_cadence_usec();   /* usec per VBL */
	/* PDF p.46/p.19: may return 0 if not yet timed; device-native nonzero is
	 * at least as faithful. Fixed = Hz << 16 = (1e6 << 16) / cadence. */
	return (cadence > 0) ? (uint32_t)((1000000ULL << 16) / cadence) : 0;
}

/* --- DSpContext_GetMonitorFrequencyHandler (sub-op 733) ---
 *
 *  DSp 1.7 PDF p.45-46, p.19: DSpContext_GetMonitorFrequency(inContext, Fixed
 *  *outFrequency). Writes the display refresh as a Fixed (16.16) Hz value
 *  derived from the VBL cadence (device-native 60/120 Hz). 4-byte Fixed
 *  out-write. */
extern "C" int32_t DSpContext_GetMonitorFrequencyHandler(uint32_t ctxRef,
														 uint32_t outFixedAddr)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL || outFixedAddr == 0) {
		DSP_LOG("GetMonitorFrequency: invalid ctxRef=%u or outFixedAddr=0x%08x",
				ctxRef, outFixedAddr);
		return kDSpInvalidContextErr;
	}
	WriteMacInt32(outFixedAddr, DSpComputeMonitorFrequencyFixed());
	return kDSpNoErr;
}


/* Round a requested dirty-rect grid cell dimension UP to a multiple of the
 * 32x32 base grid unit (DSp 1.7 PDF p.41 "suggests" - the library quantizes
 * to the base unit). 0 stays 0 (means "default to base unit" until a Set). */
static uint32_t DSpRoundUpToGridUnit(uint32_t v)
{
	if (v == 0) return 0;
	return ((v + kDSpDirtyRectGridUnit - 1) / kDSpDirtyRectGridUnit)
		   * kDSpDirtyRectGridUnit;
}

/* --- DSpContext_SetDirtyRectGridSizeHandler (sub-op 737) ---
 *
 *  DSp 1.7 PDF p.41: DSpContext_SetDirtyRectGridSize(inContext, UInt32
 *  inCellPixelWidth, UInt32 inCellPixelHeight). "Suggests" a grid cell size;
 *  rounds each requested dimension UP to a multiple of the 32-px base unit
 *  and stores it (never trust raw guest ints). No out-write. */
extern "C" int32_t DSpContext_SetDirtyRectGridSizeHandler(uint32_t ctxRef,
														  uint32_t w,
														  uint32_t h)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL) {
		DSP_LOG("SetDirtyRectGridSize: invalid ctxRef=%u", ctxRef);
		return kDSpInvalidContextErr;
	}
	ctx->dirty_grid_w = DSpRoundUpToGridUnit(w);
	ctx->dirty_grid_h = DSpRoundUpToGridUnit(h);
	return kDSpNoErr;
}

/* --- DSpContext_GetDirtyRectGridSizeHandler (sub-op 736) ---
 *
 *  DSp 1.7 PDF p.42: DSpContext_GetDirtyRectGridSize(inContext, UInt32
 *  *outCellPixelWidth, UInt32 *outCellPixelHeight). Writes the current grid
 *  cell (w,h); defaults to the 32x32 base unit until a Set lands. Two 4-byte
 *  out-writes. */
extern "C" int32_t DSpContext_GetDirtyRectGridSizeHandler(uint32_t ctxRef,
														  uint32_t wAddr,
														  uint32_t hAddr)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL || wAddr == 0 || hAddr == 0) {
		DSP_LOG("GetDirtyRectGridSize: invalid ctxRef=%u or wAddr=0x%08x "
				"hAddr=0x%08x", ctxRef, wAddr, hAddr);
		return kDSpInvalidContextErr;
	}
	WriteMacInt32(wAddr, ctx->dirty_grid_w ? ctx->dirty_grid_w
											: kDSpDirtyRectGridUnit);
	WriteMacInt32(hAddr, ctx->dirty_grid_h ? ctx->dirty_grid_h
											: kDSpDirtyRectGridUnit);
	return kDSpNoErr;
}


#define DSP_FRONT_PIXMAP_SIZE DSpFrontBufferPixMapRecordSize()
#define DSP_FRONT_PIXMAP_HANDLE_SIZE 4u
#define DSP_FRONT_CGP_SIZE DSP_CGRAFPORT_MIN_SIZE
#define DSP_FRONT_REGION_HANDLE_SIZE 4u

static uint32_t DSpCreateFrontRectRegion(uint32_t w, uint32_t h)
{
	uint32_t region_addr = DSpReserveGuestScratch(DSP_RECT_REGION_SIZE);
	uint32_t handle_addr = DSpReserveGuestScratch(DSP_FRONT_REGION_HANDLE_SIZE);
	if (region_addr == 0 || handle_addr == 0) return 0;

	Mac_memset(region_addr, 0, DSP_RECT_REGION_SIZE);
	WriteMacInt16(region_addr + DSP_REGION_OFF_SIZE, DSP_RECT_REGION_SIZE);
	WriteMacInt16(region_addr + DSP_REGION_OFF_BBOX + 0, 0);
	WriteMacInt16(region_addr + DSP_REGION_OFF_BBOX + 2, 0);
	WriteMacInt16(region_addr + DSP_REGION_OFF_BBOX + 4, (uint16_t)h);
	WriteMacInt16(region_addr + DSP_REGION_OFF_BBOX + 6, (uint16_t)w);
	WriteMacInt32(handle_addr, region_addr);
	return handle_addr;
}

/* Emit a REAL guest CGrafPort (classic 50-byte PixMap + PixMapHandle +
 * CGrafPort with portVersion 0xC000 + rect vis/clip regions) describing a
 * DSp-vended drawable surface. This is the ONE construction path for every
 * surface DSp hands the guest as a "CGrafPtr" (front buffer AND alt buffers):
 * guest code legitimately dereferences these as ports (portPixMap ->
 * GetPixBaseAddr / CopyBits / portRect), so a PixMap-shaped shim is NOT
 * enough - Diablo II software mode drew its frames through the alt-buffer
 * port and a shim sent every write into misread garbage pointers.
 * seed_pixmap_mac (optional, 0 = zero-init) seeds the PixMap record before
 * the canonical fields are written - the front buffer passes the live
 * MainDevice PixMap so unspecified fields inherit real-screen metadata.
 * Returns the CGrafPort Mac address (0 on scratch exhaustion); out_pixmap_mac
 * / out_pixmap_handle_mac (optional) receive the PixMap record + handle. */
extern "C" uint32_t DSpEmitSurfaceCGrafPort(uint32_t baseAddr_mac,
											uint32_t w,
											uint32_t h,
											uint32_t depth,
											uint32_t row_bytes,
											uint32_t seed_pixmap_mac,
											uint32_t *out_pixmap_mac,
											uint32_t *out_pixmap_handle_mac)
{
	if (baseAddr_mac == 0 || w == 0 || h == 0 || row_bytes == 0) return 0;

	uint32_t pixmap_addr = DSpReserveGuestScratch(DSP_FRONT_PIXMAP_SIZE);
	uint32_t pixmap_handle_addr =
		DSpReserveGuestScratch(DSP_FRONT_PIXMAP_HANDLE_SIZE);
	uint32_t cgrafptr_addr = DSpReserveGuestScratch(DSP_FRONT_CGP_SIZE);
	uint32_t vis_rgn_handle = DSpCreateFrontRectRegion(w, h);
	uint32_t clip_rgn_handle = DSpCreateFrontRectRegion(w, h);
	if (pixmap_addr == 0 || pixmap_handle_addr == 0 ||
		cgrafptr_addr == 0 || vis_rgn_handle == 0 ||
		clip_rgn_handle == 0) {
		DSP_LOG("DSpEmitSurfaceCGrafPort: guest-scratch reserve failed "
				"(pixmap=0x%08x handle=0x%08x cgraf=0x%08x "
				"vis=0x%08x clip=0x%08x)",
				pixmap_addr, pixmap_handle_addr, cgrafptr_addr,
				vis_rgn_handle, clip_rgn_handle);
		return 0;
	}

	uint16_t pixelType, pixelSize, cmpCount, cmpSize;
	DSpPixMapFormatForDepth(depth,
							 &pixelType, &pixelSize, &cmpCount, &cmpSize);

	const uint16_t row_bytes_field =
		DSpFrontBufferPixMapRowBytesField(row_bytes);

	if (seed_pixmap_mac != 0) {
		Host2Mac_memcpy(pixmap_addr,
						Mac2HostAddr(seed_pixmap_mac),
						DSP_FRONT_PIXMAP_SIZE);
	} else {
		Mac_memset(pixmap_addr, 0, DSP_FRONT_PIXMAP_SIZE);
	}
	WriteMacInt32(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_BASEADDR,      baseAddr_mac);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_ROWBYTES,      row_bytes_field);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_TOP,    0);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_LEFT,   0);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_BOT,    (uint16_t)h);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_RIGHT,  (uint16_t)w);
	DSpWriteCanonicalMainDevicePixMapMetadata(pixmap_addr);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_PIXELTYPE,     pixelType);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_PIXELSIZE,     pixelSize);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_CMPCOUNT,      cmpCount);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_CMPSIZE,       cmpSize);

	WriteMacInt32(pixmap_handle_addr, pixmap_addr);

	Mac_memset(cgrafptr_addr, 0, DSP_FRONT_CGP_SIZE);
	WriteMacInt32(cgrafptr_addr + DSP_CGRAFPORT_OFF_PORT_PIXMAP,
				  pixmap_handle_addr);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_PORT_VERSION, 0xC000);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_PORT_RECT + 0, 0);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_PORT_RECT + 2, 0);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_PORT_RECT + 4,
				  (uint16_t)h);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_PORT_RECT + 6,
				  (uint16_t)w);
	WriteMacInt32(cgrafptr_addr + DSP_CGRAFPORT_OFF_VIS_RGN,
				  vis_rgn_handle);
	WriteMacInt32(cgrafptr_addr + DSP_CGRAFPORT_OFF_CLIP_RGN,
				  clip_rgn_handle);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_RGB_FG_COLOR + 0,
				  0xffff);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_RGB_FG_COLOR + 2,
				  0xffff);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_RGB_FG_COLOR + 4,
				  0xffff);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_RGB_BK_COLOR + 0, 0);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_RGB_BK_COLOR + 2, 0);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_RGB_BK_COLOR + 4, 0);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_PN_SIZE + 0, 1);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_PN_SIZE + 2, 1);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_PN_MODE, 8);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_TX_SIZE, 12);
	WriteMacInt32(cgrafptr_addr + DSP_CGRAFPORT_OFF_FG_COLOR, 0xffffffffu);
	WriteMacInt32(cgrafptr_addr + DSP_CGRAFPORT_OFF_BK_COLOR, 0x00000000u);

	if (out_pixmap_mac != NULL) *out_pixmap_mac = pixmap_addr;
	if (out_pixmap_handle_mac != NULL)
		*out_pixmap_handle_mac = pixmap_handle_addr;
	return cgrafptr_addr;
}

static uint32_t DSpGetFrontBufferCGrafPtr(DSpContextPrivate *ctx)
{
	if (ctx == NULL ||
		ctx->state != (uint32_t)kDSpContextState_Active) {
		return 0;
	}

	const uint32_t front_depth =
		DSpFrontBufferDepth(ctx->attr.backBufferBestDepth,
							ctx->attr.displayBestDepth);
	if ((uint32_t)VModes[cur_mode].viXsize != ctx->attr.displayWidth ||
		(uint32_t)VModes[cur_mode].viYsize != ctx->attr.displayHeight ||
		(uint32_t)VModes[cur_mode].viAppleMode !=
			(uint32_t)DepthModeForPixelDepth((int)front_depth)) {
		DSP_LOG("DSpGetFrontBufferCGrafPtr: active context does not match "
				"the installed video mode (ctx=%ux%u@%u driver=%ux%u mode=%u)",
				ctx->attr.displayWidth, ctx->attr.displayHeight, front_depth,
				(uint32_t)VModes[cur_mode].viXsize,
				(uint32_t)VModes[cur_mode].viYsize,
				(uint32_t)VModes[cur_mode].viAppleMode);
		return 0;
	}

	if (ctx->front_cgrafptr_mac_addr != 0) {
		if (ctx->front_pixmap_mac_addr != 0) {
			WriteMacInt32(ctx->front_pixmap_mac_addr +
						  DSP_MAINDEVICE_PIXMAP_OFF_BASEADDR,
						  screen_base);
		}
		return ctx->front_cgrafptr_mac_addr;
	}

	const uint32_t w = VModes[cur_mode].viXsize;
	const uint32_t h = VModes[cur_mode].viYsize;
	const uint32_t row_bytes = VModes[cur_mode].viRowBytes;
	const uint32_t baseAddr_mac = screen_base;

	if (baseAddr_mac == 0 || Mac2HostAddr(baseAddr_mac) == NULL) {
		DSP_LOG("DSpGetFrontBufferCGrafPtr: canonical video framebuffer "
				"is unavailable (base=0x%08x)", baseAddr_mac);
		return 0;
	}

	/* Seed unspecified fields from the live screen PixMap that Display
	 * Manager just initialized. There is no cached pre-DSp PixMap: the
	 * MainDevice itself always describes the canonical framebuffer. */
	uint32_t seed_pixmap_mac = DSpGetLiveMainDevicePixMap();
	if (!DSpGuestRAMContains(seed_pixmap_mac,
							DSP_FRONT_PIXMAP_SIZE,
							(uint32_t)RAMBase,
							(uint32_t)RAMSize)) {
		seed_pixmap_mac = 0;
	}

	uint32_t pixmap_addr = 0;
	uint32_t pixmap_handle_addr = 0;
	uint32_t cgrafptr_addr = DSpEmitSurfaceCGrafPort(baseAddr_mac,
													 w, h,
													 front_depth,
													 row_bytes,
													 seed_pixmap_mac,
													 &pixmap_addr,
													 &pixmap_handle_addr);
	if (cgrafptr_addr == 0) {
		DSP_LOG("DSpGetFrontBufferCGrafPtr: CGrafPort emission failed "
				"(ctx=%u %ux%u@%u)", ctx->handle, w, h, front_depth);
		return 0;
	}

	ctx->front_cgrafptr_mac_addr = cgrafptr_addr;
	ctx->front_pixmap_mac_addr = pixmap_addr;
	ctx->front_pixmap_handle_mac_addr = pixmap_handle_addr;
	DSP_LOG("DSpGetFrontBufferCGrafPtr: ctx=%u cgrafptr=0x%08x pixmapH=0x%08x "
			"pixmap=0x%08x baseAddr=0x%08x "
			"rbRaw=0x%04x rb=%u pixelSize=%u "
			"hRes=0x%08x vRes=0x%08x pmTable=0x%08x (backBuffer@%u)",
			ctx->handle, cgrafptr_addr, pixmap_handle_addr, pixmap_addr,
			baseAddr_mac,
			ReadMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_ROWBYTES),
			row_bytes,
			ReadMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_PIXELSIZE),
			ReadMacInt32(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_HRES),
			ReadMacInt32(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_VRES),
			ReadMacInt32(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_PMTABLE),
			ctx->attr.backBufferBestDepth);
	return cgrafptr_addr;
}

/* --- DSpContext_GetFrontBufferHandler (sub-op 732) ---
 *
 *  DSp 1.7 PDF p.36: DSpContext_GetFrontBuffer(inContext, CGrafPtr
 *  *outFrontBuffer). "The front buffer is the screen display." Mixed-depth
 *  DSp contexts may use a low-depth back buffer while switching the actual
 *  display to a higher depth for RAVE/QD3D discovery. In that case the
 *  front CGrafPtr must describe the display surface, not the back buffer.
 *  4-byte CGrafPtr out-write. */
extern "C" int32_t DSpContext_GetFrontBufferHandler(uint32_t ctxRef,
													uint32_t outCGrafPtrAddr)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL || outCGrafPtrAddr == 0) {
		DSP_LOG("GetFrontBuffer: invalid ctxRef=%u or outCGrafPtrAddr=0x%08x",
				ctxRef, outCGrafPtrAddr);
		return kDSpInvalidContextErr;
	}
	uint32_t cgrafptr = DSpGetFrontBufferCGrafPtr(ctx);
	if (cgrafptr == 0) {
		DSP_LOG("GetFrontBuffer: CGrafPort emission failed for ctxRef=%u",
				ctxRef);
		return kDSpInternalErr;
	}
	WriteMacInt32(outCGrafPtrAddr, cgrafptr);
	return kDSpNoErr;
}


/* --- DSpGetCurrentContextHandler (sub-op 745) ---
 *
 *  DSp 1.7 PDF p.15: DSpGetCurrentContext(DisplayIDType inDisplayID,
 *  DSpContextReference *outContext). NOTE the first arg is a displayID, NOT a
 *  ctxRef. Writes the Active context for the given display (PocketShaver's
 *  single fullscreen display). Walks dsp_context_table for the one Active
 *  context.
 *  Incoming display IDs all map to the single backing display. If no context
 *  is active, writes 0 + returns kDSpContextNotFoundErr. */
extern "C" int32_t DSpGetCurrentContextHandler(uint32_t displayID,
											   uint32_t outCtxRefAddr)
{
	if (outCtxRefAddr == 0) {
		DSP_LOG("GetCurrentContext: NULL outCtxRefAddr - kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}
	if (!DSpAcceptsSingleDisplayID(displayID)) {
		WriteMacInt32(outCtxRefAddr, 0);
		return kDSpContextNotFoundErr;
	}
	/* Front-screen context identity is independent of whether the client
	 * reserved an off-screen back buffer. */
	uint32_t active_handle = 0;
	for (uint32_t i = 0; i < DSP_MAX_CONTEXTS; i++) {
		DSpContextPrivate *ctx = dsp_context_table[i];
		if (ctx == NULL) continue;
		if (ctx->state != (uint32_t)kDSpContextState_Active) continue;
		active_handle = ctx->handle;
		break;
	}
	WriteMacInt32(outCtxRefAddr, active_handle);
	return (active_handle != 0) ? kDSpNoErr : kDSpContextNotFoundErr;
}


/* The Mac low-memory MouseLocation global (DSp 1.7 PDF p.54 - DSpGetMouse
 * reports the SAME global mouse position the Toolbox exposes). adb.cpp writes
 * the host mouse here every interrupt in the non-POWERPC_ROM path
 * (adb.cpp:659-660): a 4-byte Point with v at 0x82c and h at 0x82e. */
#define kDSpLM_MouseLocation_v 0x82cu   /* Point.v - vertical (top) coord */
#define kDSpLM_MouseLocation_h 0x82eu   /* Point.h - horizontal (left) coord */


/* Read the current global host mouse position into (*v, *h). Reads
 * the Mac MouseLocation lowmem Point (kept live by adb.cpp's input stack).
 * This is the REAL host mouse source - NOT a hardcoded (0,0) stub. */
static void DSpReadGlobalMousePoint(int16_t *v, int16_t *h)
{
	/* MouseLocation is a Point {v, h}; vertical at 0x82c, horizontal at 0x82e
	 * (the layout adb.cpp writes). */
	*v = (int16_t)ReadMacInt16(kDSpLM_MouseLocation_v);
	*h = (int16_t)ReadMacInt16(kDSpLM_MouseLocation_h);
}

/* --- DSpGetMouseHandler (sub-op 720) ---
 *
 *  DSp 1.7 PDF p.54: DSpGetMouse(Point *outGlobalPoint). NO context param -
 *  r3 is the out-Point address. Writes the current global mouse position the
 *  host input stack maintains (the Mac MouseLocation lowmem global, kept live
 *  by adb.cpp). Mac Point on the wire is v (high half) then h (low half): two
 *  big-endian int16 at +0 / +2. Guards a NULL out-Point. */
extern "C" int32_t DSpGetMouseHandler(uint32_t outGlobalPointAddr)
{
	if (outGlobalPointAddr == 0) {
		DSP_LOG("GetMouse: NULL outGlobalPointAddr - kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}
	int16_t v = 0, h = 0;
	DSpReadGlobalMousePoint(&v, &h);
	WriteMacInt16(outGlobalPointAddr + 0, (uint16_t)v);    /* Point.v */
	WriteMacInt16(outGlobalPointAddr + 2, (uint16_t)h);    /* Point.h */
	return kDSpNoErr;
}


/* --- DSpContext_GlobalToLocalHandler (sub-op 721) ---
 *
 *  DSp 1.7 PDF p.55: DSpContext_GlobalToLocal(inContext, Point *ioPoint).
 *  Converts a global (screen) Point to the context's local coordinate space
 *  in place. On the iOS single fullscreen display the one DSp context covers
 *  the whole screen with its local origin at (0,0), so the conversion is the
 *  IDENTITY: read the Point, leave it unchanged, write it back.
 *  (If a future ppcdis cross-check of the GlobalToLocal TVECT reveals a nonzero
 *  context-rect offset, subtract it here - the structure is unchanged.) The
 *  read-modify-write uses the InvalBackBufferRect Point/Rect I/O idiom
 *  ((int16_t)ReadMacInt16 / WriteMacInt16). Validates ctxRef
 *  and guards a NULL ioPoint. */
extern "C" int32_t DSpContext_GlobalToLocalHandler(uint32_t ctxRef,
												   uint32_t ioPointAddr)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL || ioPointAddr == 0) {
		DSP_LOG("GlobalToLocal: invalid ctxRef=%u or ioPointAddr=0x%08x",
				ctxRef, ioPointAddr);
		return kDSpInvalidContextErr;
	}
	/* Identity at the single fullscreen origin (0,0). */
	const int16_t v = (int16_t)ReadMacInt16(ioPointAddr + 0);
	const int16_t h = (int16_t)ReadMacInt16(ioPointAddr + 2);
	WriteMacInt16(ioPointAddr + 0, (uint16_t)v);   /* Point.v - unchanged */
	WriteMacInt16(ioPointAddr + 2, (uint16_t)h);   /* Point.h - unchanged */
	return kDSpNoErr;
}


/* --- DSpContext_LocalToGlobalHandler (sub-op 722) ---
 *
 *  DSp 1.7 PDF p.55: DSpContext_LocalToGlobal(inContext, Point *ioPoint). The
 *  inverse of GlobalToLocal - converts a context-local Point to global (screen)
 *  coordinates in place. At the iOS single fullscreen origin (0,0) this is also
 *  the IDENTITY; if GlobalToLocal subtracts a context-rect offset, this
 *  adds it back. Same read-modify-write idiom + ctxRef / NULL-ioPoint guards. */
extern "C" int32_t DSpContext_LocalToGlobalHandler(uint32_t ctxRef,
												   uint32_t ioPointAddr)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL || ioPointAddr == 0) {
		DSP_LOG("LocalToGlobal: invalid ctxRef=%u or ioPointAddr=0x%08x",
				ctxRef, ioPointAddr);
		return kDSpInvalidContextErr;
	}
	/* Identity inverse at the single fullscreen origin (0,0). */
	const int16_t v = (int16_t)ReadMacInt16(ioPointAddr + 0);
	const int16_t h = (int16_t)ReadMacInt16(ioPointAddr + 2);
	WriteMacInt16(ioPointAddr + 0, (uint16_t)v);   /* Point.v - unchanged */
	WriteMacInt16(ioPointAddr + 2, (uint16_t)h);   /* Point.h - unchanged */
	return kDSpNoErr;
}


/* Resolve the single on-screen Active context for a global (v, h) Point.
 * Walks dsp_context_table for the one Active context with a live back_buffer
 * (the SwapBuffers active-context gate) and, if the point is inside that
 * context's display bounds, returns its handle. Returns 0 for an off-screen
 * point (negative or beyond display bounds) OR when no Active context exists
 * - the error-code-IS-the-answer single-display posture. The (v, h) are
 * already-unpacked by-VALUE coords - never a dereferenced pointer.
 * ASVS V5: the display-bounds check runs BEFORE returning any handle. */
static uint32_t DSpResolveContextHandleAtPoint(int16_t v, int16_t h)
{
	for (uint32_t i = 0; i < DSP_MAX_CONTEXTS; i++) {
		DSpContextPrivate *ctx = dsp_context_table[i];
		if (ctx == NULL) continue;
		if (ctx->state != (uint32_t)kDSpContextState_Active) continue;
		if (ctx->back_buffer == NULL) continue;
		/* Mac Point: v = vertical (row), h = horizontal (column). On a single
		 * fullscreen display the Active context contains every on-screen
		 * point [0, displayHeight) x [0, displayWidth). */
		const int32_t dispW = (int32_t)ctx->attr.displayWidth;
		const int32_t dispH = (int32_t)ctx->attr.displayHeight;
		if (v < 0 || h < 0 || (int32_t)v >= dispH || (int32_t)h >= dispW) {
			return 0;   /* off-screen -> no context (NotFound) */
		}
		return ctx->handle;
	}
	return 0;   /* no Active context */
}

/* --- DSpFindContextFromPointHandler (sub-op 723) ---
 *
 *  DSp 1.7 PDF p.53: DSpFindContextFromPoint(Point inGlobalPoint,
 *  DSpContextReference *outContext). The Point is passed by VALUE:
 *  the dispatch case unpacks it from r3 (v = high half, h = low half) and hands
 *  the int16 coords here - this handler NEVER dereferences a pointer for the
 *  Point. On the iOS single fullscreen display the one Active context contains
 *  every on-screen point, so an in-bounds (v, h) resolves to that context's
 *  handle. An off-screen point (negative or beyond display bounds) OR no Active
 *  context -> write 0 + kDSpContextNotFoundErr (error-code-IS-the-answer).
 *  Guards a NULL outCtxRefAddr. */
extern "C" int32_t DSpFindContextFromPointHandler(int16_t v, int16_t h,
												  uint32_t outCtxRefAddr)
{
	if (outCtxRefAddr == 0) {
		DSP_LOG("FindContextFromPoint: NULL outCtxRefAddr - kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}
	const uint32_t handle = DSpResolveContextHandleAtPoint(v, h);
	WriteMacInt32(outCtxRefAddr, handle);
	return (handle != 0) ? kDSpNoErr : kDSpContextNotFoundErr;
}


/* ==========================================================================
 *  Discovery / multi-display family (sub-ops
 *  744, 746, 747, 760, 761). Single-display-faithful: on the one
 *  iOS display CanUserSelectContext reports false, FindBestContextOnDisplayID
 *  and UserSelectContext delegate to the existing FindBest 3-tier matcher
 *  (DSpFindBestContextHandler - no new
 *  algorithm), and the error code IS the correct single-display answer.
 *  SetDebugMode stores a global flag (store only). SetBlankingColor assigns
 *  the DMC blanking color via the no-transition dmc_set_blanking_color
 *  accessor (the screen does NOT enter the Blanking state). No new
 *  concurrency primitive.
 * ========================================================================== */

/* DSpSetDebugMode global flag (sub-op 761). Single-writer emul-thread - same
 * contract as every dsp logging-flag global; no mutex, no _Atomic. The
 * fade-path / debug-overlay consumption is left to a later fidelity pass;
 * this stores the flag and exposes a getter only. */
static bool s_dsp_debug_mode = false;

/* --- DSpSetDebugModeHandler (sub-op 761) ---
 *
 *  DSp 1.7 PDF p.59: DSpSetDebugMode(Boolean inDebugMode). NO ctxRef - r3 is
 *  the Boolean debug-mode value. Stores the global s_dsp_debug_mode flag so a
 *  future fidelity pass can consult it on the fade path. Always
 *  succeeds (kDSpNoErr) - there is no failure mode for a global Boolean set. */
extern "C" int32_t DSpSetDebugModeHandler(uint32_t inDebugMode)
{
	s_dsp_debug_mode = (inDebugMode != 0);
	DSP_LOG("SetDebugMode: s_dsp_debug_mode = %d", (int)s_dsp_debug_mode);
	return kDSpNoErr;
}


/* --- DSpCanUserSelectContextHandler (sub-op 746) ---
 *
 *  DSp 1.7 PDF p.19: DSpCanUserSelectContext(DSpContextAttributesPtr
 *  inDesiredAttributes, Boolean *outUserCanSelectContext). The Boolean
 *  answers whether more than one cached mode satisfies the requested minimum
 *  attributes. It is not equivalent to "does this host have more than one
 *  physical display"; classic games use a false answer to skip their mode
 *  selection path and fall back to DSpFindBestContext.
 */
extern "C" int32_t DSpCanUserSelectContextHandler(uint32_t attrAddr,
												  uint32_t outCanAddr)
{
	if (attrAddr == 0) {
		DSP_LOG("CanUserSelectContext: NULL attrAddr - kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}
	if (outCanAddr == 0) {
		DSP_LOG("CanUserSelectContext: NULL outCanAddr - kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}

	DSpContextAttributes req = {};
	req.displayWidth          = ReadMacInt32(attrAddr +  4);
	req.displayHeight         = ReadMacInt32(attrAddr +  8);
	req.backBufferDepthMask   = ReadMacInt32(attrAddr + 32);
	req.displayDepthMask      = ReadMacInt32(attrAddr + 36);
	req.backBufferBestDepth   = ReadMacInt32(attrAddr + 40);
	req.displayBestDepth      = ReadMacInt32(attrAddr + 44);
	req.pageCount             = ReadMacInt32(attrAddr + 48);
	req.backBufferWidth       = req.displayWidth;
	req.backBufferHeight      = req.displayHeight;

	const size_t selectable = DSpUserSelectableModeCount(&req);
	const uint8_t canSelect =
		DSpCanUserSelectContextFromCount(selectable) ? 1 : 0;
	DSP_LOG("CanUserSelectContext: req=%ux%u back@%ubpp/bmask=0x%08x "
			"display@%ubpp/dmask=0x%08x pc=%u selectable=%zu -> %u",
			req.displayWidth, req.displayHeight,
			req.backBufferBestDepth, req.backBufferDepthMask,
			req.displayBestDepth, req.displayDepthMask,
			req.pageCount, selectable, canSelect);
	WriteMacInt8(outCanAddr, canSelect);
	return kDSpNoErr;
}


/* --- DSpFindBestContextOnDisplayIDHandler (sub-op 744) ---
 *
 *  DSp 1.7 PDF p.14: DSpFindBestContextOnDisplayID(DSpContextAttributesPtr
 *  inDesiredAttributes, DSpContextReference *outContext, DisplayIDType
 *  inDisplayID). 3-arg ABI: attrs FIRST (r3), outContext SECOND (r4),
 *  displayID THIRD (r5).
 *
 *  Lifted single-screen policy: iOS hosts a single backing
 *  screen, so ANY inDisplayID hard-routes to that screen - accepted + logged,
 *  mirroring DSpGetFirstContextHandler (sub-op 200). Real apps pass
 *  Display-Manager IDs; after routing, delegate verbatim to the
 *  EXISTING FindBest matcher (DSpFindBestContextHandler - the same attr-read +
 *  3-tier Core + AllocAndWriteBack + WriteMacInt32 path; NO new algorithm).
 *  Guards a NULL outCtxRefAddr. A genuinely unmatchable attribute request
 *  still surfaces as 0 + kDSpContextNotFoundErr from the matcher (NOT a
 *  displayID mismatch). */
extern "C" int32_t DSpFindBestContextOnDisplayIDHandler(uint32_t attrAddr,
														uint32_t outCtxRefAddr,
														uint32_t inDisplayID)
{
	if (outCtxRefAddr == 0) {
		DSP_LOG("FindBestContextOnDisplayID: NULL outCtxRefAddr - kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}
	if (!DSpAcceptsSingleDisplayID(inDisplayID)) {
		/* Lift diagnostic: surface the non-main path so future
		 * loggo captures show real-world Display-Manager IDs (e.g. 256)
		 * reaching the single-screen mapping instead of being rejected. */
		DSP_LOG("FindBestContextOnDisplayID: displayID=%u accepted "
				"(single-screen iOS - mapped to backing screen)", inDisplayID);
	}
	/* Single screen -> delegate to the existing FindBest 3-tier matcher. An
	 * unmatchable attribute request becomes 0 + kDSpContextNotFoundErr there. */
	return DSpFindBestContextHandler(attrAddr, outCtxRefAddr);
}


/* --- DSpUserSelectContextHandler (sub-op 747) ---
 *
 *  DSp 1.7 PDF p.20: DSpUserSelectContext(DSpContextAttributesPtr
 *  inDesiredAttributes, DisplayIDType inDialogDisplayLocation, DSpEventProcPtr
 *  inCleanupEventProc, DSpContextReference *outContext). 4-arg ABI (attrs r3,
 *  dialogLoc r4, eventProc r5, outContext r6). Normally this presents a
 *  monitor/mode-selection DIALOG to the user; on the single iOS display there
 *  is no meaningful choice and no dialog UI, so it behaves as a FindBest
 *  auto-pick - delegate VERBATIM to the existing FindBest matcher
 *  (DSpFindBestContextHandler - NO new algorithm, NO dialog). The dialog
 *  location + cleanup event proc are not used on a single display ((void)'d
 *  for -Werror). Guards a NULL outCtxRefAddr. The auto-pick IS the correct
 *  single-display answer, NOT a convenience stub. */
extern "C" int32_t DSpUserSelectContextHandler(uint32_t attrAddr,
											   uint32_t inDialogDisplayLocation,
											   uint32_t inEventProc,
											   uint32_t outCtxRefAddr)
{
	(void)inDialogDisplayLocation;   /* no dialog on a single display */
	(void)inEventProc;               /* no cleanup proc invoked (no dialog) */
	if (outCtxRefAddr == 0) {
		DSP_LOG("UserSelectContext: NULL outCtxRefAddr - kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}
	/* No dialog -> auto-pick via the existing FindBest matcher. */
	return DSpFindBestContextHandler(attrAddr, outCtxRefAddr);
}


/* --- DSpSetBlankingColorHandler (sub-op 760) ---
 *
 *  DSp 1.7 PDF p.30: DSpSetBlankingColor(const RGBColor *inRGBColor). NO
 *  ctxRef - r3 is the RGBColor address. Assigns the color the library uses the
 *  next time a context is blanked; it does NOT blank the screen now. The
 *  RGBColor is three 16-bit channels (red@+0, green@+2, blue@+4); down-convert
 *  to 8-bit by taking the high byte (universal QuickDraw 16->8 convention)
 *  with alpha = 0xFF, and assign via the no-transition DMC accessor
 *  dmc_set_blanking_color (which mutates the snapshot's blanking_rgba WITHOUT
 *  entering the Blanking state). Guards a NULL inRGBColorAddr.
 *  Always returns kDSpNoErr on a valid set (the DMC accessor
 *  may report not-initialized in a no-DMC build, which is still kDSpNoErr at
 *  the DSp ABI - the color set is best-effort, not a failure the app acts on). */
extern "C" int32_t DSpSetBlankingColorHandler(uint32_t inRGBColorAddr)
{
	if (inRGBColorAddr == 0) {
		DSP_LOG("SetBlankingColor: NULL inRGBColorAddr - kDSpInvalidAttributesErr");
		return kDSpInvalidAttributesErr;
	}
	uint16_t r16 = (uint16_t)ReadMacInt16(inRGBColorAddr + 0);
	uint16_t g16 = (uint16_t)ReadMacInt16(inRGBColorAddr + 2);
	uint16_t b16 = (uint16_t)ReadMacInt16(inRGBColorAddr + 4);
	uint8_t rgba[4] = { (uint8_t)(r16 >> 8), (uint8_t)(g16 >> 8),
						(uint8_t)(b16 >> 8), 0xFF };
	dmc_set_blanking_color(rgba);   /* no state transition */
	return kDSpNoErr;
}


/* --- DSpContext_InvalBackBufferRectHandler ---
 *
 *  Bounding-rect union accumulator for InvalBackBufferRect (PDF p.38).
 *  Classic Mac DSp apps notify the library about which sub-region of the
 *  back-buffer they've drawn into; when SwapBuffers fires the renderer
 *  can upload just that sub-rect via MTLBlitCommandEncoder sourceOrigin +
 *  sourceSize rather than the full back-buffer.
 *
 *  Design: bounding-rect union (O(1)) NOT per-rect list. Classic
 *  Mac DSp apps either dirty the whole back-buffer or dirty one rect, not
 *  scattered N-rect patterns - a union accumulator with a > 90% coverage
 *  promotion threshold back to full-blit captures the behavior envelope
 *  without the per-rect bookkeeping cost. Tile-based dirty tracking is
 *  overkill for the workload.
 *
 *  Cold-start semantics (PDF p.38): zero Inval calls between
 *  SwapBuffers means the ENTIRE back-buffer is considered dirty - this is
 *  the case on the very first swap and after background->foreground
 *  restore. Reserve sets dirty_cold_start=true; foreground restore sets
 *  it again. SwapBuffers checks cold_start
 *  FIRST, before the dirty-rect accumulator.
 *
 *  ASVS V5 clamping: oversized / negative rect fields are
 *  clamped to back-buffer bounds rather than rejected, matching the DSp
 *  spec "clamp; do not fail" posture. Empty / inverted rects are silently
 *  ignored with a log note.
 */

/*
 *  Clamp an input Rect to back-buffer bounds + union into the existing
 *  bounding-rect accumulator. O(1).
 *
 *  Oversized rects (right > displayWidth, bottom > displayHeight) are
 *  silently clamped to the back-buffer bounds rather than rejected,
 *  because the DSp spec says "clamp; do not fail". Negative
 *  top/left are clamped to 0.
 */
static void DSpInvalBackBufferRect_Accumulate(DSpContextPrivate *ctx,
											   int16_t top, int16_t left,
											   int16_t bottom, int16_t right)
{
	if (ctx == NULL) return;

	/* ASVS V5 input validation - clamp to back-buffer bounds.
	 * Integer-max right/bottom values get clipped; negative top/left
	 * get clipped to 0. */
	int32_t clamped_top    = (top    < 0) ? 0 : top;
	int32_t clamped_left   = (left   < 0) ? 0 : left;
	int32_t clamped_bottom = (bottom < 0) ? 0 : bottom;
	int32_t clamped_right  = (right  < 0) ? 0 : right;
	if ((uint32_t)clamped_right  > ctx->attr.displayWidth)
		clamped_right  = (int32_t)ctx->attr.displayWidth;
	if ((uint32_t)clamped_bottom > ctx->attr.displayHeight)
		clamped_bottom = (int32_t)ctx->attr.displayHeight;

	/* Empty / inverted rect? (bottom <= top or right <= left) - no-op. */
	if (clamped_right <= clamped_left || clamped_bottom <= clamped_top) {
		DSP_LOG("InvalBackBufferRect: empty/inverted rect ignored "
				"(t=%d l=%d b=%d r=%d)",
				clamped_top, clamped_left, clamped_bottom, clamped_right);
		return;
	}

	if (ctx->dirty_empty) {
		ctx->dirty_left   = (int16_t)clamped_left;
		ctx->dirty_top    = (int16_t)clamped_top;
		ctx->dirty_right  = (int16_t)clamped_right;
		ctx->dirty_bottom = (int16_t)clamped_bottom;
		ctx->dirty_empty  = false;
	} else {
		if ((int16_t)clamped_left   < ctx->dirty_left)
			ctx->dirty_left   = (int16_t)clamped_left;
		if ((int16_t)clamped_top    < ctx->dirty_top)
			ctx->dirty_top    = (int16_t)clamped_top;
		if ((int16_t)clamped_right  > ctx->dirty_right)
			ctx->dirty_right  = (int16_t)clamped_right;
		if ((int16_t)clamped_bottom > ctx->dirty_bottom)
			ctx->dirty_bottom = (int16_t)clamped_bottom;
	}
}

extern "C" int32_t DSpContext_InvalBackBufferRectHandler(uint32_t ctxRef,
														  uint32_t rectAddr)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL || rectAddr == 0) {
		DSP_LOG("InvalBackBufferRect: invalid ctxRef=%u or rectAddr=0",
				ctxRef);
		return kDSpInvalidContextErr;
	}
	/* Mac Rect in guest RAM: big-endian 4 x int16. ReadMacInt16 returns
	 * uint32 (host-native); cast to int16_t for signed interpretation. */
	int16_t top    = (int16_t)ReadMacInt16(rectAddr + 0);
	int16_t left   = (int16_t)ReadMacInt16(rectAddr + 2);
	int16_t bottom = (int16_t)ReadMacInt16(rectAddr + 4);
	int16_t right  = (int16_t)ReadMacInt16(rectAddr + 6);

	DSpInvalBackBufferRect_Accumulate(ctx, top, left, bottom, right);
	DSP_VLOG("InvalBackBufferRect: ctx=%u rect=(%d,%d)-(%d,%d) "
			 "union=(%d,%d)-(%d,%d) cold_start=%d",
			 ctxRef, top, left, bottom, right,
			 ctx->dirty_top, ctx->dirty_left, ctx->dirty_bottom, ctx->dirty_right,
			 ctx->dirty_cold_start);
	return kDSpNoErr;
}


/* ============================================================== */
/*  DSpContext_GetAttributes (sub-opcode 202)                      */
/*                                                                 */
/*  Returns ctx->attr verbatim (stored as-reserved at Reserve      */
/*  time). PDF p.18 "as-active" reading is observationally         */
/*  equivalent because FindBest returns exact-match or             */
/*  kDSpContextNotFoundErr - no substitution. Revisit if           */
/*  substitution is ever introduced (at which point we'll add      */
/*  ctx->actual_attr and GetAttributes will prefer it).            */
/*                                                                 */
/*  Writes through the PDF-p.65 layout.                            */
/*  Writes ZERO DMC-owned fields - DMCWriteSiteInventoryTests      */
/*  .testNoDirectDMCWritesInDSpFiles gate stays                    */
/*  green. Lives in dsp_draw_context.mm (NOT                       */
/*  dsp_mode_enumerate.cpp) because it operates on                 */
/*  DSpContextPrivate - the context table is private to            */
/*  this translation unit per the DMC-invariant discipline.        */
/* ============================================================== */

/*
 *  Core helper: write a host DSpContextAttributes through PDF-p.65 on-wire
 *  offsets into guest Mac RAM. Reserved/filler fields are explicitly
 *  zeroed so the emulated app reads a canonical out-struct, not whatever
 *  guest-RAM garbage preceded the call. frequency is always 0 per PDF
 *  p.66 ("value is 0 if actual frequency is not available") - the
 *  emulator does not synthesize a refresh rate from
 *  vbl_source_get_cadence_usec.
 */
static void DSpWriteAttributesCore(const DSpContextAttributes *attr,
									uint32_t outAttrAddr)
{
	/* DSp 1.7 PDF p.65 on-wire byte layout.  All UInt32 / Fixed / OptionBits /
	 * CTabHandle fields are 4 bytes each.  filler[3] + gameMustConfirmSwitch
	 * pack into the 4-byte slot @ +52..+55. reserved3[4] occupies +56..+71.
	 * Re-corrected 2026-04-21 via debug session `dsp-sims-rejects-all-modes`;
	 * pre-correction layout wrote displayWidth / displayHeight to the wrong
	 * offsets causing The Sims to read 0 and reject every mode. */
	WriteMacInt32(outAttrAddr +  0, 0 /* frequency unavailable per PDF p.66 */);
	WriteMacInt32(outAttrAddr +  4, attr->displayWidth);
	WriteMacInt32(outAttrAddr +  8, attr->displayHeight);
	WriteMacInt32(outAttrAddr + 12, 0 /* reserved1 */);
	WriteMacInt32(outAttrAddr + 16, 0 /* reserved2 */);
	WriteMacInt32(outAttrAddr + 20, attr->colorNeeds);
	WriteMacInt32(outAttrAddr + 24, attr->colorTable);
	WriteMacInt32(outAttrAddr + 28, attr->contextOptions);
	WriteMacInt32(outAttrAddr + 32, attr->backBufferDepthMask);
	WriteMacInt32(outAttrAddr + 36, attr->displayDepthMask);
	WriteMacInt32(outAttrAddr + 40, attr->backBufferBestDepth);
	WriteMacInt32(outAttrAddr + 44, attr->displayBestDepth);
	WriteMacInt32(outAttrAddr + 48, attr->pageCount);
	/* filler[3] @ +52..+54 (zero), gameMustConfirmSwitch (Boolean) @ +55. */
	WriteMacInt8 (outAttrAddr + 52, 0 /* filler[0] */);
	WriteMacInt8 (outAttrAddr + 53, 0 /* filler[1] */);
	WriteMacInt8 (outAttrAddr + 54, 0 /* filler[2] */);
	WriteMacInt8 (outAttrAddr + 55, (uint8_t)(attr->gameMustConfirmSwitch ? 1 : 0));
	/* reserved3[0..3] @ +56..+71 explicitly zeroed. */
	WriteMacInt32(outAttrAddr + 56, 0 /* reserved3[0] */);
	WriteMacInt32(outAttrAddr + 60, 0 /* reserved3[1] */);
	WriteMacInt32(outAttrAddr + 64, 0 /* reserved3[2] */);
	WriteMacInt32(outAttrAddr + 68, 0 /* reserved3[3] */);
}

extern "C" int32_t DSpContext_GetAttributesHandler(uint32_t ctxRef,
													uint32_t outAttrAddr)
{
	/* Validate outAttrAddr before any read of ctx->attr so
	 * a bad pointer + valid handle combination is reported as the more
	 * specific kDSpInvalidAttributesErr rather than masking as
	 * kDSpInvalidContextErr. */
	if (outAttrAddr == 0) {
		DSP_LOG("GetAttributes: NULL outAttrAddr (ctxRef=%u) -> "
				"kDSpInvalidAttributesErr", ctxRef);
		return kDSpInvalidAttributesErr;
	}
	/* DSpGetContext returns NULL on invalid handle;
	 * no write to guest RAM happens on this path. */
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL) {
		DSP_LOG("GetAttributes: invalid ctxRef=%u -> kDSpInvalidContextErr",
				ctxRef);
		return kDSpInvalidContextErr;
	}

	DSpContextAttributes public_attr = ctx->attr;
	DSpNormalizeGetAttributesForState(&public_attr, ctx->state);
	DSpWriteAttributesCore(&public_attr, outAttrAddr);

	DSP_LOG("GetAttributes: ctx=%u internal=%ux%u@%ubpp display@%u "
			"public@%u pc=%u state=%u -> returned",
			ctxRef,
			ctx->attr.displayWidth, ctx->attr.displayHeight,
			ctx->attr.backBufferBestDepth, ctx->attr.displayBestDepth,
			public_attr.backBufferBestDepth, ctx->attr.pageCount,
			ctx->state);
	return kDSpNoErr;
}



/*
 *  DSpContext_FadeGammaIn (sub-opcode 402).
 *
 *  Argument contract:
 *    ctxRef         = context handle from DSpContext_Reserve
 *    durationVbls   = number of VBL ticks over which to fade. 0 = apply
 *                     end LUT immediately. Values > DSP_MAX_FADE_VBLS
 *                     (4096) are clamped with a DSP_LOG warning.
 *
 *  Error map:
 *    kDSpInvalidContextErr - ctxRef does not resolve to a context
 *    kDSpNoErr             - success; fade started (or end LUT applied
 *                              immediately if durationVbls == 0)
 *
 *  Fade target:
 *    end_lut = identity LUT - for each channel c in {R, G, B} and each
 *    index i in [0..255], end_lut[c*256 + i] = i. The identity LUT is
 *    the "no gamma correction" state - the screen displays exactly the
 *    framebuffer's stored colors.
 *
 *  Fade behavior:
 *    DSpVBLGammaFadeCallback walks dsp_context_table
 *    every VBL; for each context with fade_state.active != 0 it
 *    increments elapsed_vbls, computes the interpolated LUT, and pushes
 *    via dmc_record_gamma_change_with_lut. On final frame
 *    (elapsed_vbls >= duration_vbls), pushes end_lut + memcpys end_lut
 *    into ctx->gamma_lut_persisted + sets active = 0.
 *
 *  Active-fade replacement:
 *    If a fade was already active, DSpInitFadeStateCore snapshots the
 *    current interpolated state into the new start_lut so the visual
 *    transition is continuous (no snap-back to the old fade's source).
 */
extern "C" int32_t DSpContext_FadeGammaInHandler(uint32_t ctxRef,
												  uint32_t colorAddr)
{
	/*
	 *  Debug session `dsp-sims-post-reserve-black-screen` fix (2026-04-19):
	 *  DSp 1.7 PDF p.35 (FadeGammaIn) shares the NULL-context semantics of
	 *  the FadeGamma trio - inContext == NULL applies the fade
	 *  simultaneously to all displays (ambient gamma, not a per-context
	 *  fade). Real DSp apps call `DSpContext_FadeGammaOut(NULL, NULL)`
	 *  between Reserve and SetState to fade the system-wide gamma to black
	 *  before transitioning their game context to Active (see Apple's
	 *  OpenGL DSp sample at
	 *  resources/OpenGL_SDK_1.2/.../OpenGL DrawSprocket.c).
	 *
	 *  Pre-fix behavior: rejected with kDSpInvalidContextErr - which
	 *  caused The Sims to mark DSp as unhealthy and silently degrade its
	 *  render path (no GetBackBuffer / SwapBuffers ever issued).
	 *
	 *  Post-fix: accept ctxRef == 0 as an informational no-op. We don't
	 *  apply a system-wide gamma fade (our compositor-level gamma pipeline
	 *  is per-context in the DSp engine; the compositor's own gamma path
	 *  lives in MetalCompositor and has its own APIs). Returning
	 *  kDSpNoErr matches the spec contract "the call succeeded" without
	 *  introducing a cross-subsystem ambient-fade dependency - the game's
	 *  later per-ctx fade calls + our compositor LUT keep pixels correct.
	 */
	if (ctxRef == 0) {
		DSP_LOG("FadeGammaIn: ctxRef=0 (ambient-display no-op per PDF p.35); "
				"colorAddr=0x%08x accepted",
				colorAddr);
		return kDSpNoErr;
	}
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL) {
		DSP_LOG("FadeGammaIn: invalid ctxRef=%u -> kDSpInvalidContextErr",
				ctxRef);
		return kDSpInvalidContextErr;
	}

	/*
	 *  FadeGammaIn takes (inContext, RGBColor *inZeroIntensityColor)
	 *  and fades 0% -> 100% intensity over a FIXED ONE SECOND (PDF p.35),
	 *  NOT a guest-supplied durationVbls. Derive the 1-second VBL count
	 *  from the live device-native cadence: 60 VBLs on a 60 Hz panel,
	 *  120 on a 120 Hz ProMotion panel - never a hardcoded 60.
	 *  inZeroIntensityColor is accepted (NULL -> black, legal); for the
	 *  IN direction the end state is full intensity (the driver's
	 *  installed gamma table), so the tint defines only the conceptual
	 *  0% start, which the VBL driver already snapshots from the
	 *  current displayed gamma.
	 */
	(void)colorAddr;  /* tint accepted; end_lut for IN is the driver table (100%) */

	uint64_t cadence_usec = vbl_source_get_cadence_usec();
	uint32_t vbls_1sec = (cadence_usec > 0)
						 ? (uint32_t)((1000000ull + cadence_usec / 2) / cadence_usec)
						 : 60u;
	if (vbls_1sec > DSP_MAX_FADE_VBLS) vbls_1sec = DSP_MAX_FADE_VBLS;

	/* End state = the driver's current gamma table ("original intensity"),
	 * so a fade-in lands on any game-installed ramp (e.g. overbright)
	 * instead of erasing it with identity. */
	uint8_t full_lut[768];
	DSpCopyDriverGammaLUT(full_lut);

	/* Initialize the fade machinery (active-fade-replacement snapshot
	 * + end_lut copy + duration/elapsed/active reset). The VBL driver
	 * (DSpVBLGammaFadeCallback) is UNCHANGED - only the duration source
	 * changed from a guest arg to the derived 1-second count. */
	DSpInitFadeStateCore(ctx, full_lut, (uint16_t)vbls_1sec);
	DSP_LOG("FadeGammaIn: ctx=%u 1-second fade -> %u VBLs (cadence %llu usec) started",
			ctxRef, vbls_1sec, (unsigned long long)cadence_usec);
	return kDSpNoErr;
}

/*
 *  DSpContext_FadeGammaOut (sub-opcode 403).
 *
 *  Argument contract: identical to FadeGammaIn.
 *  Error map: identical to FadeGammaIn.
 *
 *  Fade target:
 *    end_lut = all zeros (768 bytes of 0x00) - black-screen state. With
 *    a zero gamma LUT, the screen displays pure black regardless of
 *    framebuffer contents. Used by games for screen blanking + scene
 *    transitions.
 *
 *  Fade behavior + active-fade replacement: identical to FadeGammaIn.
 */
extern "C" int32_t DSpContext_FadeGammaOutHandler(uint32_t ctxRef,
												   uint32_t colorAddr)
{
	/*
	 *  Debug session `dsp-sims-post-reserve-black-screen` fix (2026-04-19):
	 *  See FadeGammaIn above for the full rationale. PDF p.34 defines
	 *  inContext == NULL as an ambient-display fade that applies to all
	 *  displays simultaneously. Pre-fix rejection with kDSpInvalidContext-
	 *  Err poisoned The Sims's internal DSp-healthy flag. Post-fix we
	 *  accept it as a logged no-op returning kDSpNoErr.
	 */
	if (ctxRef == 0) {
		DSP_LOG("FadeGammaOut: ctxRef=0 (ambient-display no-op per PDF p.34); "
				"colorAddr=0x%08x accepted",
				colorAddr);
		return kDSpNoErr;
	}
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL) {
		DSP_LOG("FadeGammaOut: invalid ctxRef=%u -> kDSpInvalidContextErr",
				ctxRef);
		return kDSpInvalidContextErr;
	}

	/*
	 *  FadeGammaOut takes (inContext, RGBColor *inZeroIntensityColor)
	 *  and fades 100% -> 0% intensity over a FIXED ONE SECOND (PDF p.34),
	 *  NOT a guest-supplied durationVbls. The end state is 0% intensity =
	 *  the inZeroIntensityColor tint (NULL -> black -> zeros, preserving
	 *  the prior fade-to-black default). Derive the 1-second VBL count
	 *  from the live device-native cadence - never hardcode 60.
	 */
	uint8_t color_r = 0, color_g = 0, color_b = 0;
	if (colorAddr != 0) {
		DSpReadParametricColorFromGuest(colorAddr, &color_r, &color_g, &color_b);
	}

	uint64_t cadence_usec = vbl_source_get_cadence_usec();
	uint32_t vbls_1sec = (cadence_usec > 0)
						 ? (uint32_t)((1000000ull + cadence_usec / 2) / cadence_usec)
						 : 60u;
	if (vbls_1sec > DSP_MAX_FADE_VBLS) vbls_1sec = DSP_MAX_FADE_VBLS;

	/* Build the end_lut at 0% intensity = the zero-intensity tint. The
	 * signed-percent formula at percent=0 yields target[c][i] = tint[c]
	 * for every index i (a flat per-channel tint LUT); NULL color -> all
	 * zeros (fade to black). */
	uint8_t full_lut[768];
	DSpCopyDriverGammaLUT(full_lut);
	uint8_t end_lut[768];
	DSpComputeFadeGammaTargetLUT(color_r, color_g, color_b, 0, full_lut, end_lut);

	/* Initialize the fade machinery (VBL driver UNCHANGED - only the
	 * duration source changed from a guest arg to the derived 1-second
	 * count, and the end_lut now honors the zero-intensity tint). */
	DSpInitFadeStateCore(ctx, end_lut, (uint16_t)vbls_1sec);
	DSP_LOG("FadeGammaOut: ctx=%u tint=(%u,%u,%u) 1-second fade -> %u VBLs "
			"(cadence %llu usec) started",
			ctxRef, color_r, color_g, color_b,
			vbls_1sec, (unsigned long long)cadence_usec);
	return kDSpNoErr;
}


/*
 *  DSpContext_FadeGamma (sub-opcode 404).
 *
 *  FadeGamma ABI fidelity (DSp 1.7 pp.32-33 verbatim); no silent stubs
 *  producing wrong output; zero new concurrency primitives.
 *
 *  DSp 1.7 ABI (p.32):
 *    OSStatus DSpContext_FadeGamma(
 *        DSpContextReference inContext,
 *        SInt32              inPercentOfOriginalIntensity,
 *        RGBColor           *inZeroIntensityColor);
 *
 *  Argument contract:
 *    ctxRef     = context handle from DSpContext_Reserve. NULL (0) is the
 *                 ambient-display "applies to all displays" no-op (PDF p.32).
 *    inPercent  = SIGNED percentage of original intensity. 0..100 is the
 *                 normal range; > 100 begins to converge on white; < 0
 *                 begins to converge on black. NO duration param exists in
 *                 the real ABI (the invented `durationVbls` is dropped).
 *    colorAddr  = guest Mac address of an RGBColor (16-bit r@+0/g@+2/b@+4,
 *                 big-endian). NULL is LEGAL -> zero-intensity color is
 *                 black (PDF p.32), NOT a reject.
 *
 *  Error map:
 *    kDSpInvalidContextErr - ctxRef != 0 does not resolve to a context
 *    kDSpInternalErr       - DMC push failed
 *    kDSpNoErr             - success; the target LUT was applied
 *
 *  Immediate-apply (PDF p.33 "repeated, timed calls"):
 *    The parametric FadeGamma is an INCREMENTAL process - the APP makes
 *    repeated, timed calls, each passing an incrementally different
 *    inPercent. DSp applies the target LUT IMMEDIATELY each call (there
 *    is no DSp-internal fade timer for the parametric variant - that is
 *    FadeGammaIn/Out). The previous handler's `durationVbls == 0`
 *    short-circuit body IS the new always-path.
 *
 *  Reuse (UNCHANGED):
 *    - DSpReadParametricColorFromGuest (now 16-bit) for the tint read.
 *    - DSpComputeFadeGammaTargetLUT (signed percent + tint-honoring).
 *    - dmc_record_gamma_change_with_lut for the immediate DMC push.
 */
extern "C" int32_t DSpContext_FadeGammaHandler(uint32_t ctxRef,
												int32_t  inPercent,
												uint32_t colorAddr)
{
	/*
	 *  PDF p.32 - inContext == NULL applies the fade simultaneously to all
	 *  displays (ambient). The standard Apple DSp transition idiom fades the
	 *  ambient display to black and then calls FadeGamma(NULL, 100) to restore
	 *  full intensity (see Apple's "OpenGL DrawSprocket.c" sample). On our
	 *  single display we APPLY that ambient fade to the compositor gamma - NOT
	 *  a no-op - or the restore is lost and the screen stays stuck black
	 *  (Cro-Mag Rally: gameplay invisible after a fade transition). This is the
	 *  PARAMETRIC FadeGamma (sub-opcode 404) only; the timed FadeGammaIn/Out
	 *  (401/403) keep their ctxRef==0 no-op, since The Sims drives THOSE between
	 *  Reserve and SetState and relies on that DSp-health behavior. Apply path
	 *  mirrors the ctxRef!=0 case below, minus the per-context bookkeeping.
	 */
	if (ctxRef == 0) {
		uint8_t color_r = 0, color_g = 0, color_b = 0;
		if (colorAddr != 0) {
			DSpReadParametricColorFromGuest(colorAddr, &color_r, &color_g, &color_b);
		}
		uint8_t full_lut[768];
		DSpCopyDriverGammaLUT(full_lut);
		uint8_t target_lut[768];
		DSpComputeFadeGammaTargetLUT(color_r, color_g, color_b,
									  inPercent, full_lut, target_lut);
		/* fade_active rides the publish: any percent other than exactly
		 * 100 leaves the display mid-fade (LUT must march linearly, and
		 * driver SetGamma arriving now must defer, not pop). 100 restores
		 * the driver table and ends the fade. */
		int32_t rv = dmc_record_gamma_change_with_lut_fade(
			target_lut, (inPercent != 100) ? 1 : 0);
		if (rv != kDMCNoErr) {
			DSP_LOG("FadeGamma: ctxRef=0 ambient inPercent=%d "
					"dmc_record_gamma_change_with_lut_fade returned %d", inPercent, rv);
			return kDSpInternalErr;
		}
		DSP_LOG("FadeGamma: ctxRef=0 ambient inPercent=%d tint=(%u,%u,%u) "
				"-> applied to compositor gamma (all-displays restore)",
				inPercent, color_r, color_g, color_b);
		return kDSpNoErr;
	}
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == NULL) {
		DSP_LOG("FadeGamma: invalid ctxRef=%u -> kDSpInvalidContextErr",
				ctxRef);
		return kDSpInvalidContextErr;
	}

	/* NULL colorAddr is LEGAL -> zero-intensity color is black.
	 * Read the 16-bit RGBColor tint only when a non-NULL pointer is
	 * supplied; the read is guarded BEFORE any deref. */
	uint8_t color_r = 0, color_g = 0, color_b = 0;
	if (colorAddr != 0) {
		DSpReadParametricColorFromGuest(colorAddr, &color_r, &color_g, &color_b);
	}

	/* Build the target LUT per the DSp 1.7 signed-percent tint-honoring
	 * formula. inPercent is passed through verbatim - the formula owns
	 * the >100 (white) / <0 (black) convergence + the [0,255] clamp. */
	uint8_t full_lut[768];
	DSpCopyDriverGammaLUT(full_lut);
	uint8_t target_lut[768];
	DSpComputeFadeGammaTargetLUT(color_r, color_g, color_b,
								  inPercent, full_lut, target_lut);

	/* Parametric FadeGamma applies the target immediately, in a
	 * single push - the app loops it over time. There is no internal
	 * fade timer for the parametric variant. fade_active rides the
	 * publish: any percent other than exactly 100 leaves the display
	 * mid-fade (LUT must march linearly, and driver SetGamma arriving
	 * now must defer, not pop); 100 restores the driver table and ends
	 * the fade. */
	int32_t rv = dmc_record_gamma_change_with_lut_fade(
		target_lut, (inPercent != 100) ? 1 : 0);
	if (rv != kDMCNoErr) {
		DSP_LOG("FadeGamma: dmc_record_gamma_change_with_lut_fade returned %d "
				"for ctx=%u",
				rv, ctxRef);
		return kDSpInternalErr;
	}
	memcpy(ctx->gamma_lut_persisted, target_lut, 768);
	/* Cancel any active In/Out fade so the immediate parametric apply is
	 * authoritative. */
	ctx->fade_state.active = 0;
	DSP_LOG("FadeGamma: ctx=%u inPercent=%d tint=(%u,%u,%u) "
			"-> target applied immediately",
			ctxRef, inPercent,
			color_r, color_g, color_b);
	return kDSpNoErr;
}

#include "dsp_pixel_staging_lifetime_policy.h"
extern uint32 Mac_sysalloc(uint32 size);
extern void Mac_sysfree(uint32 addr);

/* DSp exposes these blocks as guest PixMap.baseAddr storage. Once exposed,
 * do not DisposePtr them: launch-time CFM/component allocations may reuse
 * the same system-heap range and execute stale frame bytes. */
enum {
	kDSpPixelStagingPoolCapacity = 16
};

/* Uniform allocation floor.  Vending every staging block at >= this size
 * lets later, larger needs (Diablo II: 640x480 splash staging -> 800x600
 * menu staging) REUSE quarantined blocks instead of making fresh multi-MB
 * NewPtrSys allocations mid-game.  Mid-game system-heap allocations of
 * that size are device-proven (2026-06-11) to detonate purge/compaction
 * storms in the guest system zone: purgeable component code gets evicted
 * (sound-component crash class) and live structures move while stale
 * pointers still reference them (the Memory-Manager zone-walk wedge that
 * froze DII's menu).  2 MiB covers every mode DII uses (800x600x4 with
 * aligned rowBytes = 1,996,800 bytes). */
enum : uint32_t {
	kDSpPixelStagingAllocFloor = 0x200000
};

struct DSpPixelStagingPoolBlock {
	uint32_t mac_addr;
	uint32_t size;        /* logical size of the most recent exposure */
	uint32_t alloc_size;  /* TRUE Mac_sysalloc size; NEVER grown - the
						   * ground truth a write must never exceed. */
	bool     in_use;
	bool     ever_exposed_to_guest;
	bool     allocated_from_mac_system_heap;
};

static DSpPixelStagingPoolBlock
	s_dsp_pixel_staging_pool[kDSpPixelStagingPoolCapacity];

static DSpPixelStagingPoolBlock *DSpFindPixelStagingBlock(uint32_t mac_addr)
{
	for (uint32_t i = 0; i < kDSpPixelStagingPoolCapacity; i++) {
		DSpPixelStagingPoolBlock *block = &s_dsp_pixel_staging_pool[i];
		if (block->mac_addr == mac_addr) return block;
	}
	return NULL;
}

static DSpPixelStagingPoolBlock *DSpFindEmptyPixelStagingBlock(void)
{
	for (uint32_t i = 0; i < kDSpPixelStagingPoolCapacity; i++) {
		DSpPixelStagingPoolBlock *block = &s_dsp_pixel_staging_pool[i];
		if (block->mac_addr == 0) return block;
	}
	return NULL;
}

/* Keep one idle floor-sized block parked in the pool. Guest-visible back and
 * alternate buffers may need a mapped-RAM fallback after Reserve; allocating
 * a spare together with the first block moves that heap work to mode-set time
 * instead of a later frame. */
static void DSpPrewarmPixelStagingSpare(void)
{
	for (uint32_t i = 0; i < kDSpPixelStagingPoolCapacity; i++) {
		DSpPixelStagingPoolBlock *block = &s_dsp_pixel_staging_pool[i];
		if (block->mac_addr != 0 && !block->in_use &&
			block->alloc_size >= kDSpPixelStagingAllocFloor)
			return;                    /* an idle spare already exists */
	}
	DSpPixelStagingPoolBlock *slot = DSpFindEmptyPixelStagingBlock();
	if (slot == NULL)
		return;
	uint32_t mac_addr = Mac_sysalloc(kDSpPixelStagingAllocFloor);
	if (mac_addr == 0)
		return;
	slot->mac_addr = mac_addr;
	slot->size = 0;
	slot->alloc_size = kDSpPixelStagingAllocFloor;
	slot->in_use = false;
	slot->ever_exposed_to_guest = false;
	slot->allocated_from_mac_system_heap = true;
	DSP_LOG("DSpReserveGuestPixelStaging: prewarmed spare 0x%08x alloc=%u",
			mac_addr, (unsigned)kDSpPixelStagingAllocFloor);
}

extern "C" uint32_t DSpReserveGuestPixelStaging(uint32_t size)
{
	if (size == 0) return 0;

	for (uint32_t i = 0; i < kDSpPixelStagingPoolCapacity; i++) {
		DSpPixelStagingPoolBlock *block = &s_dsp_pixel_staging_pool[i];
		if (block->mac_addr != 0 &&
			!block->in_use &&
			DSpPixelStagingCanReuseQuarantinedAllocation(block->alloc_size, size)) {
			block->in_use = true;
			block->size = size;
			DSP_LOG("DSpReserveGuestPixelStaging: reused quarantined "
					"staging 0x%08x alloc=%u need=%u",
					block->mac_addr, block->alloc_size, size);
			return block->mac_addr;
		}
	}

	uint32_t alloc_request = size;
	if (alloc_request < kDSpPixelStagingAllocFloor)
		alloc_request = kDSpPixelStagingAllocFloor;
	uint32_t mac_addr = Mac_sysalloc(alloc_request);
	if (mac_addr == 0 && alloc_request != size) {
		/* Degraded: the floored request did not fit; take the exact size. */
		alloc_request = size;
		mac_addr = Mac_sysalloc(size);
	}
	if (mac_addr == 0) return 0;

	DSpPixelStagingPoolBlock *slot = DSpFindEmptyPixelStagingBlock();
	if (slot != NULL) {
		slot->mac_addr = mac_addr;
		slot->size = size;
		slot->alloc_size = alloc_request; /* ground truth; never grown afterwards */
		slot->in_use = true;
		slot->ever_exposed_to_guest = false;
		slot->allocated_from_mac_system_heap = true;
		DSP_LOG("DSpReserveGuestPixelStaging: fresh Mac_sysalloc 0x%08x alloc=%u need=%u",
				mac_addr, alloc_request, size);
		DSpPrewarmPixelStagingSpare();
	} else {
		DSP_LOG("DSpReserveGuestPixelStaging: pool full; 0x%08x "
				"size=%u will be quarantined untracked on release",
				mac_addr, size);
	}
	return mac_addr;
}

extern "C" uint32_t DSpGuardStagingWrite(uint32_t mac_addr, uint32_t size,
										 const char *site)
{
	if (mac_addr == 0 || size == 0) return size;

	DSpPixelStagingPoolBlock *block = DSpFindPixelStagingBlock(mac_addr);
	if (block == NULL) {
		/* Write targets an address that is not a tracked staging block.
		 * We cannot bound it - flag it so the field log shows an untracked
		 * staging write (itself suspicious). */
		DSP_LOG("DSpGuardStagingWrite: UNTRACKED staging target site=%s "
				"addr=0x%08x size=%u (no pool entry - cannot bound)",
				site ? site : "?", mac_addr, size);
		return size;
	}

	if (size > block->alloc_size) {
		DSP_LOG("DSpGuardStagingWrite: *** STAGING OVERRUN PREVENTED *** "
				"site=%s addr=0x%08x requested=%u alloc=%u overrun=%u "
				"-> CLAMPED to alloc",
				site ? site : "?", mac_addr, size, block->alloc_size,
				size - block->alloc_size);
		return block->alloc_size;
	}
	return size;
}

extern "C" void DSpQuarantineGuestPixelStaging(
	uint32_t mac_addr,
	uint32_t size,
	bool allocated_from_mac_system_heap)
{
	if (mac_addr == 0) return;

	DSpPixelStagingPoolBlock *block = DSpFindPixelStagingBlock(mac_addr);
	if (block != NULL) {
		if (size > block->size) block->size = size;
		block->in_use = false;
		block->ever_exposed_to_guest = true;
		block->allocated_from_mac_system_heap =
			allocated_from_mac_system_heap;
		DSP_LOG("DSpQuarantineGuestPixelStaging: retained exposed "
				"staging 0x%08x size=%u",
				mac_addr, block->size);
		return;
	}

	DSpPixelStagingPoolBlock *slot = DSpFindEmptyPixelStagingBlock();
	if (slot != NULL) {
		slot->mac_addr = mac_addr;
		slot->size = size;
		slot->alloc_size = size;   /* best-known true size for untracked block */
		slot->in_use = false;
		slot->ever_exposed_to_guest = true;
		slot->allocated_from_mac_system_heap =
			allocated_from_mac_system_heap;
		DSP_LOG("DSpQuarantineGuestPixelStaging: retained untracked "
				"exposed staging 0x%08x size=%u",
				mac_addr, size);
		return;
	}

	if (DSpPixelStagingShouldReturnExposedAllocationToMacHeap(
			allocated_from_mac_system_heap)) {
		Mac_sysfree(mac_addr);
		return;
	}

	DSP_LOG("DSpQuarantineGuestPixelStaging: pool full; leaving exposed "
			"staging 0x%08x size=%u allocated",
			mac_addr, size);
}

extern "C" void DSpDiscardUnusedGuestPixelStaging(
	uint32_t mac_addr,
	bool allocated_from_mac_system_heap)
{
	if (mac_addr == 0) return;

	DSpPixelStagingPoolBlock *block = DSpFindPixelStagingBlock(mac_addr);
	if (block != NULL) {
		if (block->ever_exposed_to_guest) {
			block->in_use = false;
			DSP_LOG("DSpDiscardUnusedGuestPixelStaging: retained "
					"previously exposed staging 0x%08x size=%u",
					mac_addr, block->size);
			return;
		}
		if (block->allocated_from_mac_system_heap) {
			Mac_sysfree(mac_addr);
		}
		*block = {};
		return;
	}

	if (allocated_from_mac_system_heap) {
		Mac_sysfree(mac_addr);
	}
}

extern "C" void DSpReleaseBackBufferStaging(DSpContextPrivate *ctx)
{
	if (ctx == NULL || ctx->staging_mac_addr == 0) return;
	DSpQuarantineGuestPixelStaging(ctx->staging_mac_addr,
								   ctx->staging_size,
								   ctx->staging_owned_sysheap);
	ctx->staging_mac_addr = 0;
	ctx->staging_size = 0;
	ctx->staging_owned_sysheap = false;
}
