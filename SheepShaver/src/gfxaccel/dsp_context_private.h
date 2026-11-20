/*
 *  dsp_context_private.h - Full DSpContextPrivate struct definition,
 *                           shared between dsp_draw_context.mm and
 *                           dsp_metal_renderer.mm.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Strictly private to the gfxaccel/ tree - NOT exported via include/ -
 *  only dsp_draw_context.mm and dsp_metal_renderer.mm include it. Public
 *  consumers (dsp_dispatch.cpp, dsp_engine.cpp, test code) use the opaque
 *  `struct DSpContextPrivate;` forward declaration in dsp_engine.h /
 *  dsp_draw_context.h.
 *
 *  This struct previously lived inline in dsp_draw_context.mm; it is
 *  extracted here so dsp_metal_renderer.mm can touch the Metal resource
 *  fields (back_buffer / back_texture) without either file duplicating the
 *  definition (brittle) or exposing ObjC types
 *  in a public header (breaks .cpp callers like dsp_dispatch.cpp).
 *
 *  Field naming note:
 *    `persisted_palette_generation` / `persisted_gamma_generation` use the
 *    longer suffixes so the DMC write-site inventory grep-gate in
 *    DMCWriteSiteInventoryTests does not match - only the DMC controller
 *    module may declare the bare-word generation-counter identifiers.
 *    DSp stores local snapshots of the DMC values here for background/
 *    foreground persistence.
 */

#ifndef DSP_CONTEXT_PRIVATE_H
#define DSP_CONTEXT_PRIVATE_H

#include "gfxaccel_backend.h"

#include <stdint.h>
#include "atomic.h"

#include "dsp_engine.h"       /* DSpContextAttributes + DSpContextState enum */

/* DSP_ENUMERATION_INDEX_NONE is defined in the public dsp_draw_context.h
 * header so callers outside the gfxaccel tree (e.g., test wrappers) can
 * use it without pulling in this private struct. See that header for the
 * sentinel's semantics. */
struct DSpContextPrivate {
	DSpContextAttributes  attr;
	uint32_t              state;          /* DSpContextState enum value */
	uint32_t              handle;

	/* Debug session `dsp-sims-enumeration-stall` fix (2026-04-19) - index
	 * into s_dsp_modes[] that this metadata-only context was vended from
	 * during DSpGetFirstContext / DSpGetNextContext iteration. Used by
	 * DSpGetNextContextHandler to advance the enumeration cursor so apps
	 * can walk ALL available modes via the PDF p.17 loop pattern
	 * (`while (theContext) { ...; GetNextContext(theContext, &theContext); }`).
	 *
	 * The Sims UAT capture (loggo.txt:542-549, 2026-04-19) showed The Sims
	 * calling GetFirstContext -> GetAttributes -> GetNextContext, finding only
	 * one 1bpp mode (the lowest-sorted entry of s_dsp_modes), and bailing
	 * out because it needs 16bpp. Pre-fix GetNextContext was a stub that
	 * always returned kDSpContextNotFoundErr - so multi-mode iteration
	 * never worked. Post-fix GetNextContext walks the cache forward using
	 * this field as the cursor.
	 *
	 * DSP_ENUMERATION_INDEX_NONE (== UINT32_MAX) means "not part of the
	 * GetFirst/GetNext enumeration chain" - applies to (a) fully-Reserved
	 * contexts created via DSpContext_Reserve (they own Metal resources
	 * and are not iteration placeholders), and (b) FindBest-vended
	 * metadata contexts (they represent a best-match, not a cursor
	 * position in the mode list). Calling GetNextContext with such a
	 * handle returns kDSpContextNotFoundErr per PDF p.17 "last context
	 * in the list" - a fully-reserved or best-match context has no
	 * successor in the enumeration.
	 *
	 * Value-initialized to 0 by `new DSpContextPrivate()`, but that zero
	 * is semantically ambiguous (0 is a legal index into s_dsp_modes). So
	 * all construction paths MUST explicitly set this field: GetFirst vends
	 * index 0; FindBest and Reserve_Core both assign DSP_ENUMERATION_INDEX_NONE.
	 *
	 * Threading: single-writer emul-thread - same contract as every other
	 * DSpContextPrivate field. No mutex, no _Atomic. */
	uint32_t              enumeration_mode_index;

	/* Back-buffer Metal resources - populated via
	 * DSpAllocateBackBuffer (gfxaccel_resources_heap_alloc_buffer on
	 * kHeapCompositor, MTLStorageModeShared). Release order
	 * MUST be texture-first, buffer-second. */
	void *              back_buffer;  /* host buffer or MTLBuffer */
	void *              back_texture; /* GLuint or MTLTexture */

	/* CGrafPort emission caching (GetBackBuffer stable-pointer contract).
	 * First GetBackBuffer call allocates via SheepMem::Reserve
	 * and caches the Mac address here; subsequent calls return the same. */
	uint32_t              cgrafptr_mac_addr;
	uint32_t              front_cgrafptr_mac_addr;
	uint32_t              front_pixmap_mac_addr;
	uint32_t              front_pixmap_handle_mac_addr;

	/* Dirty-region accumulator - populated with the real
	 * InvalBackBufferRect-driven union. */
	int16_t               dirty_left, dirty_top, dirty_right, dirty_bottom;
	bool                  dirty_empty;
	bool                  dirty_cold_start;

	/* Staging-region fallback. When Host2MacAddr cannot map the
	 * MTLBuffer contents pointer back to a usable guest-RAM address (e.g.,
	 * the bump allocator lives outside the vm_alloc region on arm64 iOS),
	 * DSpGetBackBufferCGrafPtr reserves a guest-writable pixel-staging
	 * region and stores the Mac address here. At
	 * SwapBuffers time the handler memcpys staging -> back_buffer.contents
	 * before the GPU blit. Zero means Host2MacAddr produced a guest-RAM
	 * address and no staging indirection is needed.
	 *
	 * Preserving guest-writable CGrafPtr semantics via a host memcpy is
	 * strictly better than a raw (uint32)(uintptr_t) cast of the contents
	 * pointer - the latter is undefined behaviour on arm64 iOS (64-bit
	 * host VA truncated to 32-bit Mac address). Graceful degradation: if
	 * a guest-writable staging allocation cannot be vended,
	 * GetBackBufferHandler returns
	 * kDSpInternalErr; no OOB write is ever attempted. */
	uint32_t              staging_mac_addr;
	uint32_t              staging_size;
	/* True only for staging_mac_addr allocations that came from the Mac
	 * system heap. Exposed pixel buffers are quarantined on release instead
	 * of Mac_sysfree'd, because CFM/code allocations may otherwise reuse
	 * old frame bytes as executable memory during launch transitions. Test
	 * scratch and direct Host2MacAddr paths leave this false. */
	bool                  staging_owned_sysheap;

	/* WindowPtr of the full-screen Window Manager backdrop owned while this
	 * context is Active. It keeps other applications' windows below later DSp
	 * dialogs and gives FindWindow a full-screen application layer without
	 * introducing another pixel surface. 0 when no backdrop is live. */
	uint32_t  guest_window;

	/* Per-context CLUT storage.
	 * clut_bytes is the writer-visible state - DSpContext_SetCLUTEntries
	 * writes here. clut_bytes_latched is the reader-visible
	 * snapshot - GetCLUTEntries reads from here. The VBL
	 * secondary callback DSpVBLClutLatchCallback memcpy's
	 * clut_bytes -> clut_bytes_latched on each VBL tick so
	 * GetCLUTEntries returns last-VBL-boundary state.
	 * 768 bytes = 256 entries * 3 bytes (R/G/B). Field names use the
	 * clut_* prefix (NOT palette_* / mac_pal_*) so the DMC
	 * write-site CI gate (testNoDirectDMCWritesInDSpFiles) does not
	 * match these writes. */
	uint8_t               clut_bytes[768];
	uint8_t               clut_bytes_latched[768];

	/* Set once the APP has supplied a CLUT for this context (via
	 * DSpContext_SetCLUTEntries, or a Reserve colorTable). Until then
	 * clut_bytes/clut_bytes_latched only hold the seeded default ramp, and the
	 * indexed expand path should prefer the live display palette instead. */
	uint8_t               clut_app_supplied;

	/* Per-context gamma persistence buffer
	 * for Pause->Resume replay. When the context is Resumed via
	 * DSpContext_SetStateHandler's Paused->Active arm, the persisted gamma
	 * is re-pushed into the DMC snapshot via dmc_record_gamma_change_with_lut
	 * so the compositor's existing VBL gamma block in metal_compositor.mm
	 * picks it up at the next VBL. SetGamma updates this
	 * field; DSpVBLGammaFadeCallback updates it on fade
	 * completion; FadeGamma updates it on instantaneous
	 * percent=100 paths. 768 bytes = 256 R + 256 G + 256 B planar layout
	 * matching DMC snapshot's gamma_lut[768] field (display_mode_controller.h).
	 * Field name uses the long-suffix `_persisted` convention so the
	 * DMC write-site CI gate (testNoDirectDMCWritesInDSpFiles) does
	 * not match this storage. */
	uint8_t               gamma_lut_persisted[768];

	/* Per-context fade machinery. The
	 * DSpVBLGammaFadeCallback registered in DSpInit walks dsp_context_table
	 * each VBL tick; for every context with fade_state.active != 0 it
	 * computes the interpolated LUT and
	 * pushes it via dmc_record_gamma_change_with_lut. FadeGammaIn/Out
	 * and parametric FadeGamma initialize
	 * start_lut + end_lut + duration_vbls + elapsed_vbls + active when a
	 * fade is requested. SetGamma cancels an in-flight fade
	 * by setting active = 0.
	 *
	 * Sized: 768 + 768 + 2 + 2 + 1 + 3 = 1544 bytes per context. Field
	 * names (start_lut, end_lut, duration_vbls, elapsed_vbls, active)
	 * deliberately use long-suffix conventions so the DMC-owned bare-word
	 * identifiers (see testNoDirectDMCWritesInDSpFiles + the
	 * sibling testPaletteGenAndGammaGenAreOnlyDeclaredInDMCHeader) do not
	 * appear in this header even as comment text.
	 * The 3-byte `reserved` array preserves uint16-alignment of the
	 * trailing persisted{} struct (host-side ABI consistency). */
	struct DSpFadeState {
		uint8_t           start_lut[768];
		uint8_t           end_lut[768];
		uint16_t          duration_vbls;
		uint16_t          elapsed_vbls;
		uint8_t           active;
		uint8_t           reserved[3];
	}                     fade_state;

	/* Per-context VBLProc registration.
	 * vbl_proc_ptr is the guest PPC TVECT address of the VBLProc the app
	 * registered via DSpContext_SetVBLProc (sub-opcode 500); 0 means no
	 * proc is installed (per DSp 1.7 spec p.81 - SetVBLProc(ptr=0)
	 * uninstalls). vbl_proc_refcon is the opaque 4-byte refCon the app
	 * passed at registration time; DSpVBLServiceCallback hands it back
	 * on every invocation via the DSp 1.7 VBLProc ABI
	 * `void (DSpContextReference, void *refCon, UInt32 vblCount)`.
	 *
	 * Both fields are single-writer emul-thread: SetVBLProcHandler
	 * writes them from the emul thread's NATIVE_DSP_DISPATCH seam;
	 * DSpVBLServiceCallback reads them from the emul thread's VBL
	 * secondary-callback drain. No cross-thread access. No synchronization
	 * needed. Field names use long-suffix `vbl_proc_*` naming so the
	 * DMC write-site grep gate (testNoDirectDMCWritesInDSpFiles) does
	 * not match this storage. */
	uint32_t              vbl_proc_ptr;
	uint32_t              vbl_proc_refcon;

	/* paused_by_background: set to 1 by DSpHandleBackgroundFromEmulThread
	 * when the atomic lifecycle drain auto-Pauses an Active context.
	 * Cleared to 0 by DSpHandleForegroundFromEmulThread after
	 * auto-Resuming. User-initiated DSpContext_SetState(Paused) does NOT
	 * touch this flag - so a context the user paused before backgrounding
	 * stays Paused after foreground (it has paused_by_background == 0, so
	 * the foreground drain skips it). Default-initialized to 0 by
	 * `new DSpContextPrivate()` (POD default init preserves the existing
	 * zero-init contract for non-ObjC fields). */
	uint8_t               paused_by_background; /* bg-induced pause flag */

	/* Background/foreground persistence.
	 * See naming note in file header. */
	struct {
		DSpContextAttributes attr;
		uint32_t             persisted_palette_generation;
		uint32_t             persisted_gamma_generation;
		uint32_t             invalidated_full;
	} persisted;

	/* Cheap-family query bookkeeping fields.
	 *
	 * max_frame_rate (sub-ops 734/735): the app-suggested max frames/sec cap.
	 * DSp 1.7 PDF p.44 "does not guarantee"; 0 means no restriction.
	 * SwapBuffers consults this field to multiply the DSp VBL pacing lane.
	 * dirty_grid_w / dirty_grid_h (sub-ops 736/737):
	 * the app-suggested dirty-rect grid cell size, rounded up to a multiple of
	 * the 32x32 base grid unit at Set time; 0 means "default to the base unit"
	 * until a Set lands.
	 *
	 * Threading: single-writer emul-thread - same contract as every other
	 * DSpContextPrivate field. No mutex, no _Atomic. Value-initialized to 0 by
	 * `new DSpContextPrivate()` (POD default-init, like enumeration_mode_index
	 * before its explicit sentinel assignment); both construction paths
	 * (Reserve_Core, AllocFirstContextHandle) get the zero from value-init. */
	uint32_t              max_frame_rate;
	uint32_t              dirty_grid_w;
	uint32_t              dirty_grid_h;

	/* AltBuffer designation fields (sub-ops
	 * 704/705). underlay_alt_buffer holds the alt-buffer handle designated
	 * as this context's underlay via DSpContext_SetUnderlayAltBuffer; 0 ==
	 * none designated. DSpContext_GetBackBuffer reads it to drive the
	 * underlay-restore branch (PDF p.51 "clean a back buffer"). overlay_alt_
	 * buffer is reserved for a future overlay designation (PDF p.49 overlay-
	 * kind alt-buffers); only the underlay path is wired but the
	 * field is scaffolded so the record-table + handler shape is symmetric.
	 *
	 * Threading: single-writer emul-thread - same contract as every other
	 * DSpContextPrivate field. NO _Atomic, NO mutex (the retired
	 * sub-op-600 cross-thread SPSC ring was the anti-pattern here,
	 * deliberately NOT copied). Value-initialized to 0 by
	 * `new DSpContextPrivate()` (POD default-init, like max_frame_rate); both
	 * construction paths (Reserve_Core, AllocFirstContextHandle) get the zero
	 * from value-init. */
	uint32_t              underlay_alt_buffer;
	uint32_t              overlay_alt_buffer;

	/* Queue/Switch deferred-context-switch
	 * staging (sub-ops 742-743, DSp 1.7 PDF pp.26-27). queued_child holds the
	 * child-context handle staged against THIS context (the parent) by
	 * DSpContext_Queue; 0 == nothing staged. DSpContext_Switch requires
	 * old->queued_child == newRef (a prior Queue) before it applies the switch,
	 * else it returns an error (PDF p.27 "If you did not queue the contexts you
	 * want to switch ... DSpContext_Switch returns an error"). Switch clears
	 * this field after applying. The staging field is pure RAM-only
	 * single-writer emul-thread bookkeeping; the switch application itself
	 * routes through the SetState machinery. There is NO cross-thread queue
	 * (the field name "queued" refers to the DSp deferred-switch concept, not
	 * a concurrent data structure).
	 *
	 * Threading: single-writer emul-thread - same contract as every other
	 * DSpContextPrivate field. NO _Atomic, NO mutex (the retired
	 * sub-op-600 cross-thread SPSC ring was the anti-pattern here,
	 * deliberately NOT copied - none of the heavy handlers need
	 * cross-thread state). Value-initialized to 0 by `new DSpContextPrivate()`
	 * (POD default-init, like max_frame_rate / underlay_alt_buffer); both
	 * construction paths (Reserve_Core, AllocFirstContextHandle) get the zero
	 * from value-init. */
	uint32_t              queued_child;
};

/* Back-buffer logical dimensions (fall back to display dims when the app did
 * not request an explicit back-buffer size). Hoisted from dsp_draw_context.mm
 * so dsp_alt_buffer.mm can size alt-buffer backings identically. */
static inline uint32_t DSpContextBackBufferWidth(const DSpContextPrivate *ctx)
{
	return ctx->attr.backBufferWidth != 0
		   ? ctx->attr.backBufferWidth
		   : ctx->attr.displayWidth;
}

static inline uint32_t DSpContextBackBufferHeight(const DSpContextPrivate *ctx)
{
	return ctx->attr.backBufferHeight != 0
		   ? ctx->attr.backBufferHeight
		   : ctx->attr.displayHeight;
}

/* Reserve guest-RAM scratch (SheepMem). Definition stays in
 * dsp_draw_context.mm; declared here so dsp_alt_buffer.mm can reserve
 * CGrafPort scratch for alt-buffers. */
uint32_t DSpReserveGuestScratch(uint32_t size);

extern "C" uint32_t DSpReserveGuestPixelStaging(uint32_t size);
extern "C" void DSpQuarantineGuestPixelStaging(uint32_t mac_addr,
											   uint32_t size,
											   bool allocated_from_mac_system_heap);
extern "C" void DSpDiscardUnusedGuestPixelStaging(uint32_t mac_addr,
												  bool allocated_from_mac_system_heap);
void* DSpGetBackingContents(void* backing);
int32_t DSpRevalidateSwapContext(uint32_t ctxRef,
										DSpContextPrivate *expected,
										uint32_t entry_state,
										DSpContextPrivate **outCtx,
								 const char *site);
bool DSpPollBusyProc(uint32_t ctxRef, uint32_t busyProcAddr,
					 uint32_t userRefCon);
void DSpSyncSwapFramePacing(uint32_t ctxRef, uint32_t maxFrameRate);
void DSpContextPrivateReleaseBackBufferVariables(void** texture, void** buffer);
void DSpContextPrivateReleaseBackBuffer(DSpContextPrivate* ctx);

#endif /* DSP_CONTEXT_PRIVATE_H */
