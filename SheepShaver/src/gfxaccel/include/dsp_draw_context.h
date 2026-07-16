/*
 *  dsp_draw_context.h - DSp per-context private-data table + Reserve/Release
 *                        lifecycle + VBL-bounded release queue. Also covers
 *                        the GetBackBuffer / SwapBuffers / SetState / GetState /
 *                        InvalBackBufferRect handlers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  C-callable interface. ObjC types (id<MTLBuffer>, id<MTLTexture>)
 *  live inside the opaque DSpContextPrivate struct defined in
 *  dsp_draw_context.mm; this header exposes only C-friendly decls so
 *  dsp_dispatch.cpp can call handlers without dragging in Metal headers.
 *
 *  Pattern analog: rave_metal_renderer.h (style) +
 *  rave_draw_context.mm (per-context table shape).
 */

#ifndef DSP_DRAW_CONTEXT_H
#define DSP_DRAW_CONTEXT_H

#include <stdint.h>

struct DSpContextPrivate;  /* opaque; full def in dsp_draw_context.mm */

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  Context-table accessors. 1-based handles; handle 0 is the NULL sentinel
 *  DrawSprocketLib emits. Max 8 simultaneous DSp contexts (more than any
 *  classic-Mac game used).
 *
 *  DSP_MAX_CONTEXTS was previously a `#define` local to
 *  dsp_draw_context.mm. dsp_host_bridge.mm (OnBackground/OnForeground
 *  bodies) walks `DSpGetContext(i + 1)` for i in 0..DSP_MAX_CONTEXTS-1
 *  and needs the bound visible cross-TU. Hoisting the constant into the
 *  public header preserves all existing uses (the original `#define` in
 *  dsp_draw_context.mm still compiles because it's compatible with the
 *  header-visible value). Keep this name — DSp tests and other cross-TU
 *  walks (DSpVBLServiceCallback, DSpHandleBackgroundFromEmulThread, etc.)
 *  can reference this same header bound without re-defining.
 */
#ifndef DSP_MAX_CONTEXTS
/* 64 (was 8): DSpGetNextContext allocates a DISTINCT metadata context per
 * enumeration step (PDF p.16 — apps may retain every enumerated ref and
 * read attributes later; Myth II does), so a full mode walk consumes
 * modes+1 slots. Stale enumeration contexts are recycled under table
 * pressure by DSpAllocMetadataContextHandle. Per-slot cost is one
 * pointer; the VBL walks over the table are nullptr-skip cheap. */
#define DSP_MAX_CONTEXTS 64
#endif
extern struct DSpContextPrivate *DSpGetContext(uint32_t handle);

/*
 *  Debug session `dsp-sims-enumeration-stall` fix (2026-04-19) — sentinel
 *  value for DSpContextPrivate::enumeration_mode_index indicating "not
 *  part of the GetFirstContext/GetNextContext enumeration chain". Applies
 *  to contexts created via DSpContext_Reserve and to contexts vended by
 *  DSpFindBestContext (they represent best-match results, not cursor
 *  positions). Calling DSpGetNextContext with such a handle returns
 *  kDSpContextNotFoundErr per DSp 1.7 PDF p.17 ("last context in the
 *  list" terminator).
 *
 *  Value == UINT32_MAX; kept as a macro (not inline constant) so C-only
 *  callers and the enumerator in dsp_mode_enumerate.cpp can reference it
 *  without any C++-specific idioms.
 */
#define DSP_ENUMERATION_INDEX_NONE 0xFFFFFFFFu

/*
 *  Allocate a metadata-only DSp context handle for
 *  DSpGetFirstContext / DSpFindBestContext.
 *
 *  Unlike DSpContext_ReserveHandler this helper does NOT
 *  allocate a Metal back-buffer or texture. The emulated app gets a
 *  handle whose `attr` is the vended-mode metadata; resources are
 *  allocated lazily when the app calls DSpContext_Reserve with the
 *  inspected attributes, per the DSp 1.7 enumeration-then-reserve
 *  contract.
 *
 *  Takes a forward-declared `struct DSpContextAttributes *` (full def in
 *  dsp_engine.h — included by callers) to populate ctx->attr.
 *
 *  Debug session `dsp-sims-enumeration-stall` fix (2026-04-19) — adds the
 *  `enumeration_mode_index` arg. Callers on the GetFirstContext /
 *  GetNextContext path pass the 0-based index into s_dsp_modes[] that this
 *  metadata-only context was vended from; the value is stored on the
 *  context so DSpGetNextContextHandler can advance the cursor. Callers NOT
 *  on the enumeration path (DSpFindBestContext) pass
 *  DSP_ENUMERATION_INDEX_NONE (== 0xFFFFFFFFu) so the GetNextContext
 *  terminator-arm fires correctly.
 *
 *  On success returns a 1-based handle (>= 1); on table-full returns 0.
 *  The caller is responsible for checking for 0 and mapping to
 *  kDSpInternalErr (matches the Reserve_Core pattern).
 */
struct DSpContextAttributes;
extern uint32_t DSpAllocFirstContextHandle(
    const struct DSpContextAttributes *attr,
    uint32_t enumeration_mode_index);

/*
 *  Allocate a transient metadata-only DSp context handle.
 *
 *  This wraps DSpAllocFirstContextHandle with one compatibility fallback:
 *  if the small context table is full, it may recycle an inactive
 *  non-enumeration metadata-only handle left behind by probing code that
 *  called DSpFindBestContext but never reserved or released the result.
 *  Reserved contexts and live enumeration cursors are never reclaimed.
 */
extern uint32_t DSpAllocMetadataContextHandle(
    const struct DSpContextAttributes *attr,
    uint32_t enumeration_mode_index);

/*
 *  Debug session `dsp-sims-enumeration-stall` fix (2026-04-19) — read the
 *  enumeration_mode_index stored on a metadata-only context handle. Used
 *  by dsp_mode_enumerate.cpp's DSpGetNextContext_Core to advance the
 *  enumeration cursor without needing the full DSpContextPrivate struct
 *  definition (which imports <Metal/Metal.h> and is unusable in .cpp).
 *
 *  Returns the index (0..s_dsp_modes.size()-1) for a context on the
 *  enumeration chain, DSP_ENUMERATION_INDEX_NONE for non-enumeration
 *  contexts (Reserve / FindBest), and DSP_ENUMERATION_INDEX_NONE for
 *  invalid ctxRef (caller validates ctxRef->existence separately via
 *  DSpGetContext).
 */
extern uint32_t DSpGetContextEnumerationIndex(uint32_t ctxRef);

/*
 *  Public sub-opcode handlers.
 *
 *  Debug session `dsp-sims-post-reserve-black-screen` (2026-04-19) fix:
 *  DSpContext_ReserveHandler signature corrected to DSp 1.7 PDF p.25:
 *      OSStatus DSpContext_Reserve(DSpContextReference inContext,
 *                                  const DSpContextAttributesPtr inDesiredAttributes);
 *  First arg is the EXISTING ctxRef returned by FindBestContext /
 *  GetFirstContext; second arg is a Mac pointer to the desired attributes.
 *  Reserve allocates a back-buffer + back-texture ON the existing context,
 *  overriding its attributes with the desired-attributes struct. No new
 *  handle is allocated; no out-write is performed (the app already knows
 *  its own ctxRef).
 */
extern int32_t  DSpContext_ReserveHandler(uint32_t ctxRef,
                                          uint32_t attrAddr);
extern int32_t  DSpContext_ReleaseHandler(uint32_t ctxRef);
extern int32_t  DSpContext_GetBackBufferHandler(uint32_t ctxRef,
                                                uint32_t options,
                                                uint32_t outBufAddr);
extern int32_t  DSpContext_SwapBuffersHandler(uint32_t ctxRef,
                                              uint32_t busyProcAddr,
                                              uint32_t userRefCon);
extern int32_t  DSpContext_SetStateHandler(uint32_t ctxRef, uint32_t state);
extern int32_t  DSpContext_GetStateHandler(uint32_t ctxRef,
                                           uint32_t outStateAddr);
extern int32_t  DSpContext_InvalBackBufferRectHandler(uint32_t ctxRef,
                                                      uint32_t rectAddr);

/*
 *  Cheap-family query handlers (sub-ops 730-738 + 745). Each reads/writes
 *  context state, a new bookkeeping field, a constant, or walks the context
 *  table; copies of the DSpContext_GetStateHandler skeleton (DSpGetContext
 *  null-guard + outAddr==0 guard + WriteMacInt* + kDSpNoErr). See
 *  dsp_draw_context.mm.
 */
extern int32_t  DSpContext_IsBusyHandler(uint32_t ctxRef,
                                         uint32_t outBusyAddr);
extern int32_t  DSpContext_GetDisplayIDHandler(uint32_t ctxRef,
                                               uint32_t outIDAddr);
extern int32_t  DSpContext_GetDirtyRectGridUnitsHandler(uint32_t ctxRef,
                                                        uint32_t wAddr,
                                                        uint32_t hAddr);
extern int32_t  DSpContext_GetMaxFrameRateHandler(uint32_t ctxRef,
                                                  uint32_t outAddr);
extern int32_t  DSpContext_SetMaxFrameRateHandler(uint32_t ctxRef,
                                                  uint32_t inMaxFPS);
extern int32_t  DSpContext_GetMonitorFrequencyHandler(uint32_t ctxRef,
                                                      uint32_t outFixedAddr);
extern int32_t  DSpContext_SetDirtyRectGridSizeHandler(uint32_t ctxRef,
                                                       uint32_t w,
                                                       uint32_t h);
extern int32_t  DSpContext_GetDirtyRectGridSizeHandler(uint32_t ctxRef,
                                                       uint32_t wAddr,
                                                       uint32_t hAddr);
extern int32_t  DSpContext_GetFrontBufferHandler(uint32_t ctxRef,
                                                 uint32_t outCGrafPtrAddr);
extern int32_t  DSpGetCurrentContextHandler(uint32_t displayID,
                                            uint32_t outCtxRefAddr);

/*
 *  Coordinate / mouse handlers (sub-ops
 *  720-723). GetMouse has NO context param (r3 is the out-Point address);
 *  GlobalToLocal / LocalToGlobal are identity at the iOS single fullscreen
 *  origin (0,0); FindContextFromPoint takes the Point by VALUE (the v,h are
 *  unpacked in the dispatch case, NOT read from a pointer). See
 *  dsp_draw_context.mm.
 */
extern int32_t  DSpGetMouseHandler(uint32_t outGlobalPointAddr);
extern int32_t  DSpContext_GlobalToLocalHandler(uint32_t ctxRef,
                                                uint32_t ioPointAddr);
extern int32_t  DSpContext_LocalToGlobalHandler(uint32_t ctxRef,
                                                uint32_t ioPointAddr);
extern int32_t  DSpFindContextFromPointHandler(int16_t v, int16_t h,
                                               uint32_t outCtxRefAddr);

/*
 *  Discovery / multi-display handlers (sub-ops
 *  744, 746, 747, 760, 761). FindBestContextOnDisplayID validates inDisplayID
 *  then delegates to the existing FindBest matcher (3-arg ABI: attrs first,
 *  outContext second, displayID third). CanUserSelectContext writes false on
 *  the single iOS display. UserSelectContext presents no dialog and auto-picks
 *  via FindBest. SetBlankingColor / SetDebugMode have NO ctxRef. Each handler
 *  decl lands WITH its body per-export. See dsp_draw_context.mm.
 */
extern int32_t  DSpSetDebugModeHandler(uint32_t inDebugMode);
extern int32_t  DSpCanUserSelectContextHandler(uint32_t attrAddr,
                                               uint32_t outCanAddr);
extern int32_t  DSpFindBestContextOnDisplayIDHandler(uint32_t attrAddr,
                                                     uint32_t outCtxRefAddr,
                                                     uint32_t inDisplayID);
extern int32_t  DSpUserSelectContextHandler(uint32_t attrAddr,
                                            uint32_t inDialogDisplayLocation,
                                            uint32_t inEventProc,
                                            uint32_t outCtxRefAddr);
extern int32_t  DSpSetBlankingColorHandler(uint32_t inRGBColorAddr);

/*
 *  AltBuffer handlers (sub-ops 700-705).
 *  Real Metal-backed implementations reusing the engine-blind
 *  gfxaccel infra with zero new concurrency primitives.
 *
 *  New (700)        : r3=ctxRef, r4=inVRAMBuffer, r5=inAttributes,
 *                     r6=outAltBuffer. NULL inAttributes => underlay-capable;
 *                     heap-backed via gfxaccel_resources_heap_alloc_buffer
 *                     (kHeapEngineDSp). Writes the new handle to outAltBuffer.
 *  Dispose (701)    : r3=altBuffer. Releases the heap backing + clears record.
 *  GetCGrafPtr (702): r3=altBuffer, r4=bufferKind, r5=outCGrafPtr. Only
 *                     kDSpBufferKind_Normal valid; else kDSpInvalidAttributesErr.
 *  InvalRect (703)  : r3=altBuffer, r4=inInvalidRect. Clamps + unions the
 *                     dirty rect (ASVS V5).
 *  GetUnderlay (704): r3=ctxRef, r4=outUnderlay. Writes ctx->underlay_alt_buffer.
 *  SetUnderlay (705): r3=ctxRef, r4=inNewUnderlay. Designates the underlay.
 *
 *  See dsp_draw_context.mm. Each handler decl lands WITH its body per-export.
 */
extern int32_t  DSpAltBuffer_NewHandler(uint32_t ctxRef,
                                        uint32_t inVRAMBuffer,
                                        uint32_t inAttributesAddr,
                                        uint32_t outAltBufferAddr);
extern int32_t  DSpAltBuffer_DisposeHandler(uint32_t altBuffer);
extern int32_t  DSpAltBuffer_GetCGrafPtrHandler(uint32_t altBuffer,
                                                uint32_t bufferKind,
                                                uint32_t outCGrafPtrAddr);
extern int32_t  DSpAltBuffer_InvalRectHandler(uint32_t altBuffer,
                                              uint32_t inInvalidRectAddr);
extern int32_t  DSpContext_GetUnderlayAltBufferHandler(uint32_t ctxRef,
                                                       uint32_t outUnderlayAddr);
extern int32_t  DSpContext_SetUnderlayAltBufferHandler(uint32_t ctxRef,
                                                       uint32_t inNewUnderlay);

/*
 *  Blit handlers (sub-ops 710-711).
 *
 *  DSpBlit_Faster (710)  : r3=inBlitInfo, r4=inAsyncFlag. Scales srcRect ->
 *                          dstRect via the NEW nqd_bitblt_scaled kernel
 *                          (nearest default, bilinear when Interpolation set;
 *                          color-key per SrcKey/DstKey).
 *  DSpBlit_Fastest (711) : r3=inBlitInfo, r4=inAsyncFlag. Strict 1:1 copy via
 *                          the proven nqd_bitblt kernel.
 *
 *  Both read DSpBlitInfo via DSP_BLITINFO_OFF_*, resolve src/dst CGrafPtr +
 *  Rect, reject OOB baseAddr / unsupported depth -> kDSpInternalErr, NULL
 *  inBlitInfo -> kDSpInvalidAttributesErr. Async = synchronous-then-complete:
 *  set completionFlag + invoke completionProc via call_macos1 (PPC-trampoline,
 *  NEVER a raw C cast). ZERO new concurrency primitive. Decl lands WITH
 *  the body per-export.
 */
extern int32_t  DSpBlit_FasterHandler(uint32_t inBlitInfo, uint32_t inAsyncFlag);
extern int32_t  DSpBlit_FastestHandler(uint32_t inBlitInfo, uint32_t inAsyncFlag);

/*
 *  Save/Restore/Flatten handlers (sub-ops 739-741, PDF pp.21-23).
 *
 *  DSpContext_GetFlattenedSize (740) : r3=ctxRef, r4=outSize. Writes the
 *                          DSP_FLAT_SIZE byte count of the flattened format.
 *                          NULL outSize -> kDSpInvalidAttributesErr;
 *                          bad ctxRef -> kDSpInvalidContextErr.
 *  DSpContext_Flatten (739) : r3=ctxRef, r4=outFlatContext. Serializes the
 *                          {magic 'DSpF', version, size, DSpContextAttributes
 *                          attr, max_frame_rate, dirty_grid_w/h} subset to the
 *                          guest out-buffer via WriteMacInt*. Runtime-only
 *                          fields are NEVER serialized (Flatten is
 *                          pre-Active per PDF p.22). NULL outFlatContext ->
 *                          kDSpInvalidAttributesErr (validated BEFORE ctx
 *                          lookup); bad ctxRef -> kDSpInvalidContextErr.
 *  DSpContext_Restore (741) : r3=inFlatContext, r4=outRestoredContext. Reads +
 *                          validates magic+version, allocates a fresh metadata
 *                          context, writes its ctxRef. magic/version mismatch
 *                          or no-display-match -> kDSpContextNotFoundErr
 *                          (documented valid per PDF p.22, NOT masked). NULL
 *                          either ptr -> kDSpInvalidAttributesErr.
 *
 *  Self-consistent round-trip: Flatten then Restore reproduces
 *  the attr + max_frame_rate + dirty_grid_w/h subset. Pure RAM serialization —
 *  ZERO new concurrency primitive. Decl lands WITH the body per-export.
 */
extern int32_t  DSpContext_FlattenHandler(uint32_t ctxRef, uint32_t outFlatContext);
extern int32_t  DSpContext_GetFlattenedSizeHandler(uint32_t ctxRef, uint32_t outSize);
extern int32_t  DSpContext_RestoreHandler(uint32_t inFlatContext, uint32_t outRestoredContext);

/*
 *  Queue/Switch deferred-context-switch handlers
 *  (sub-ops 742-743, DSp 1.7 PDF pp.26-27; DSp-1.7-only exports).
 *
 *  DSpContext_Queue (742)  : r3=parentCtx, r4=childCtx, r5=inDesiredAttributes.
 *                          Resolves both ctxRefs (unresolved ->
 *                          kDSpInvalidContextErr); passes the same-display check
 *                          (trivially true on the single iOS fullscreen
 *                          display); optionally applies inDesiredAttributes to
 *                          the child when non-zero; records
 *                          parent->queued_child = childRef. Returns kDSpNoErr.
 *  DSpContext_Switch (743) : r3=oldCtx, r4=newCtx. Requires a prior Queue
 *                          (old->queued_child == newRef, else kDSpInternalErr
 *                          per PDF p.27 "returns an error"); kills the OLD
 *                          context's piggyback VBL proc (old->vbl_proc_ptr = 0 —
 *                          the VBL service walk early-outs on ==0); deactivates
 *                          OLD through SetState(Inactive); activates NEW through
 *                          SetState(Active); clears old->queued_child.
 *
 *  queued_child is a RAM-only single-writer emul-thread field — ZERO new
 *  concurrency primitive. Decl lands WITH the body per-export.
 */
extern int32_t  DSpContext_QueueHandler(uint32_t parentCtx, uint32_t childCtx,
                                        uint32_t inDesiredAttributes);
extern int32_t  DSpContext_SwitchHandler(uint32_t oldCtx, uint32_t newCtx);
extern void     DSpContext_SetStateSwitchHandoff(uint32_t oldCtxRef);

/*
 *  CLUT handlers.
 *
 *  The DSp 1.7 pp.56-57 wire-format —
 *  8-byte ColorSpec / 16-bit big-endian channels, pointer-before-index arg
 *  order (entriesAddr, inStartingEntry, inEntryCount); inEntryCount is a
 *  COUNT, not an inclusive last index. The 16<->8 conversion is confined
 *  to the guest-RAM boundary; internal clut_bytes storage stays 8-bit.
 */
extern int32_t  DSpContext_SetCLUTEntriesHandler(uint32_t ctxRef,
                                                 uint32_t entriesAddr,
                                                 uint32_t inStartingEntry,
                                                 uint32_t inEntryCount);
extern int32_t  DSpContext_GetCLUTEntriesHandler(uint32_t ctxRef,
                                                 uint32_t entriesOutAddr,
                                                 uint32_t inStartingEntry,
                                                 uint32_t inEntryCount);
extern int32_t  DSpGetActiveCLUTSnapshot(uint8_t out_clut_bytes[768]);

/*
 *  Gamma-fade handlers.
 *
 *  Argument layout per dsp_dispatch.cpp r4..r7 marshalling:
 *    FadeGammaIn  (ctxRef, durationVbls)                 — sub-opcode 402
 *    FadeGammaOut (ctxRef, durationVbls)                 — sub-opcode 403
 *    FadeGamma    (ctxRef, percent, durationVbls, colorAddr) — sub-opcode 404
 *
 *  Debug session `dsp-sims-post-reserve-black-screen` (2026-04-19) fix:
 *  FadeGammaIn / FadeGammaOut / FadeGamma now accept ctxRef == 0 as the
 *  DSp 1.7 PDF p.32 "ambient display gamma" no-op, returning kDSpNoErr
 *  without mutating any per-context state. Real Mac apps (including The
 *  Sims) use `DSpContext_FadeGammaOut(NULL, NULL)` during Startup +
 *  Shutdown to fade the system-wide gamma before/after their own Reserved
 *  context is active. Rejecting NULL ctxRef produced kDSpInvalidContextErr
 *  and poisoned The Sims's internal "DSp healthy" flag.
 */
/*
 *  FadeGamma trio per the
 *  DSp 1.7 ABI (pp.32-35). The parametric FadeGamma takes a SIGNED
 *  inPercent + an RGBColor* zero-intensity tint (NO durationVbls); the
 *  In/Out variants take an RGBColor* tint and fade over a fixed 1 second
 *  derived from the device-native VBL cadence.
 */
extern int32_t  DSpContext_FadeGammaInHandler(uint32_t ctxRef,
                                              uint32_t colorAddr);
extern int32_t  DSpContext_FadeGammaOutHandler(uint32_t ctxRef,
                                               uint32_t colorAddr);
extern int32_t  DSpContext_FadeGammaHandler(uint32_t ctxRef,
                                            int32_t  inPercent,
                                            uint32_t colorAddr);

/*
 *  VBL Service handlers.
 *
 *  Argument layout per dsp_dispatch.cpp r4..r6 marshalling:
 *    SetVBLProc   (ctxRef, procPtr, refCon)               — sub-opcode 500
 *    GetVBLProc   (ctxRef, procOutAddr, refConOutAddr)    — sub-opcode 503
 */
extern int32_t  DSpContext_SetVBLProcHandler(uint32_t ctxRef,
                                             uint32_t procPtr,
                                             uint32_t refCon);
extern int32_t  DSpContext_GetVBLProcHandler(uint32_t ctxRef,
                                             uint32_t procOutAddr,
                                             uint32_t refConOutAddr);

/*
 *  Canonical DSpProcessEvent
 *  (sub-opcode 750). DSp 1.7 PDF p.58:
 *    OSStatus DSpProcessEvent(EventRecord *inEvent, Boolean *outEventWasProcessed)
 *  NO ctxRef — the app passes its OWN event in; DSp inspects it for the
 *  suspend/resume osEvt it must handle, drives context state, and reports via
 *  the Boolean out-param whether it consumed the event (honest false for
 *  unhandled events). The dispatch case routes r3 = inEventAddr
 *  (EventRecord*), r4 = outProcessedAddr (Boolean*).
 *
 *  This is the OPPOSITE direction to the retired sub-op-600 dequeue
 *  handler (the old DSpContext_* ProcessEvent reader export), which was
 *  retired (retire, NOT repurpose). The SPSC input-fanout ring it
 *  observed is KEPT write-only — it currently has no reader of any
 *  kind — pending a decision on the iOS input-fanout design.
 */
extern int32_t  DSpProcessEventHandler(uint32_t inEventAddr,
                                       uint32_t outProcessedAddr);

/*
 *  VBL-bounded release — registered as a vbl_source secondary callback in
 *  DSpInit (dsp_engine.cpp). Drains the release FIFO; releases each
 *  entry's back-texture before its back-buffer.
 */
extern void     DSpVBLReleaseCallback(void *ctx, void *drawable, double ts);

/*
 *  VBL-latched CLUT snapshot callback. Registered
 *  from DSpInit via vbl_source_register_secondary_callback; unregistered
 *  from DSpShutdown. Drains clut_bytes -> clut_bytes_latched per-context.
 */
extern void     DSpVBLClutLatchCallback(void *ctx, void *drawable, double ts);

/*
 *  VBL-driven gamma-fade interpolation callback.
 *  Registered from DSpInit via vbl_source_register_secondary_callback
 *  AFTER DSpVBLClutLatchCallback; unregistered from DSpShutdown FIRST
 *  (reverse registration order). The body walks the context table and
 *  applies the per-frame linear-interpolation +
 *  dmc_record_gamma_change_with_lut push.
 */
extern void     DSpVBLGammaFadeCallback(void *ctx, void *drawable, double ts);

/*
 *  VBL service callback. Registered from DSpInit
 *  via vbl_source_register_secondary_callback AFTER DSpVBLGammaFadeCallback.
 *  Unregistered from DSpShutdown FIRST (reverse registration order). The
 *  body atomic-increments s_dsp_vbl_count (the monotonic tick counter) and
 *  runs the per-context walk + user VBLProc invocation via call_macos3.
 *  Runs on the emul thread per the VBL secondary-callback contract.
 */
extern void     DSpVBLServiceCallback(void *ctx, void *drawable, double ts);

/*
 *  Background/foreground emul-thread drain. Declared here so it can be
 *  registered in the same VBL secondary-callback chain without touching
 *  headers again.
 */
extern void     DSpVBLBackgroundForegroundDrain(void);

/*
 *  Pending-lifecycle bitmask bits carried in the dsp_engine.cpp atomic and
 *  returned by DSpExchangeBgFgPending. Accumulated with fetch_or so a
 *  foreground event cannot erase an undrained background event.
 */
enum {
	kDSpPendingBackground = 1u << 0,
	kDSpPendingForeground = 1u << 1
};

/*
 *  Synchronous lifecycle drain. Runs the background/foreground drain plus
 *  the VBL release-FIFO drain immediately on the calling (main==emul)
 *  thread. Called from the UIKit lifecycle hooks (dsp_engine.cpp) because
 *  gfxaccel_handle_background_enter pauses the VBL source before invoking
 *  them — the VBL drain chain cannot run while backgrounded. No-ops
 *  (leaving the pending bits set for the in-flight tick's own drain) when
 *  called from inside the VBL callback chain.
 */
extern void     DSpDrainLifecycleSync(void);

/*
 *  Emul-thread handler bodies invoked from the VBL
 *  drain. Exported so test-harness code (and the draw-context drain
 *  dispatcher) can invoke them directly. Both iterate the DSp context
 *  table on the emul thread — callers MUST NOT invoke from the main
 *  thread; use the NotificationCenter / gfxaccel_set_dsp_*_hook path for
 *  external triggers.
 */
extern void     DSpHandleBackgroundFromEmulThread(void);
extern void     DSpHandleForegroundFromEmulThread(void);

/*
 *  Atomic bridge. Implementation lives in
 *  dsp_engine.cpp so the _Atomic uint32_t flag is owned by the engine
 *  lifecycle module; dsp_draw_context.mm calls this from the drain paths
 *  to read + clear the pending state in one acquire-ordered exchange.
 *  Returns a kDSpPending* bitmask (0 = none pending).
 */
extern uint32_t DSpExchangeBgFgPending(void);

/*
 *  Map DSp play state -> Display-Mode-Controller owner value. This helper
 *  is the single point where the DSp-to-controller owner mapping lives;
 *  the SetState handler, the bg/fg hooks, and the multi-engine canary
 *  all call through here.
 *
 *  The public export is uint32_t (not the controller's typed
 *  enum) so this public DSp header stays free of the controller's own
 *  header. The returned value is binary-compatible with the controller
 *  owner enum — consumers that have already pulled in that header can
 *  cast directly, or use the typed inline wrapper
 *  DSpMapStateToDMCOwnerTyped() exported by the private helper header
 *  dsp_engine_internal.h (gfxaccel-tree-internal only).
 *
 *  Valid inputs: kDSpContextState_{Active,Paused,Inactive} from
 *  dsp_engine.h. Any other value returns the controller's Quiescent
 *  sentinel; call sites must reject invalid state values BEFORE calling
 *  this helper (SetState bounds-checks first).
 */
extern uint32_t DSpMapStateToDMCOwner(uint32_t dsp_state);


#ifdef __cplusplus
}
#endif

#endif /* DSP_DRAW_CONTEXT_H */
