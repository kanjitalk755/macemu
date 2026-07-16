/*
 *  dsp_flatten_restore.mm - DrawSprocket context Save/Restore/Flatten
 *                           (sub-ops 739-741, PDF pp.21-23) + Queue/Switch.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Extracted verbatim from dsp_draw_context.mm (de-bloat, NO behaviour change):
 *  the Flatten/Restore/GetFlattenedSize production handlers and the
 *  Queue/Switch handlers/cores. All entry points are extern "C" and
 *  declared in dsp_draw_context.h; the internal statics have no out-of-module
 *  callers, so no extra header is needed.
 */
#import <Metal/Metal.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "thunks.h"                /* SheepMem::Reserve */
#include "dsp_engine.h"            /* DSpContextAttributes + DSP_FLAT_* layout */
#include "dsp_draw_context.h"
#include "dsp_mode_enumerate.h"    /* DSpFindBestContextHandler delegate target */
#include "dsp_context_private.h"   /* DSpContextPrivate full struct; shared with dsp_metal_renderer.mm */
#include "dsp_display_mode_policy.h" /* DSpReserveActual/BackBufferDimension (Queue desired-attr apply) */
#include "dsp_guest_address.h"
#include "nqd_accel.h"             /* NQDMetalAddrInBuffer (Queue desired-attr mapped-RAM gate) */
#include "dsp_engine_internal.h"   /* DSpMapStateToDMCOwnerTyped (internal-only, NOT in include/) */

/* ============================================================== */
/*  Save/Restore/Flatten                                          */
/*  (sub-ops 739-741, PDF pp.21-23)                               */
/*                                                                  */
/*  Self-consistent serialization: the SAME emulator Flattens its  */
/*  own context to a magic+version-headed blob and Restores it     */
/*  later (no second consumer of the byte layout exists on iOS).   */
/*  Flatten runs BEFORE the context's play state                   */
/*  goes Active (PDF p.22), so the back-buffer Metal resources do  */
/*  NOT exist yet and MUST NOT be serialized — only                */
/*  the attribute + bookkeeping subset is portable. Restore "has   */
/*  a high probability of failure" (p.22), so a magic/version      */
/*  mismatch -> kDSpContextNotFoundErr is a documented, valid      */
/*  outcome (NOT masked). Pure RAM serialization — ZERO new        */
/*  concurrency primitive.                                         */
/*                                                                  */
/*  The handlers serialize/deserialize the DSP_FLAT_* on-wire       */
/*  layout directly through WriteMacInt32 / ReadMacInt32 (big-      */
/*  endian on the Mac side) — no separate host-buffer core.        */
/* ============================================================== */

/* --- DSpContext_GetFlattenedSizeHandler (sub-op 740) ---
 *
 *  DSp 1.7 PDF p.23: DSpContext_GetFlattenedSize(inContext, UInt32 *outSize).
 *  Writes the byte count GetFlattenedSize/Flatten agree on (DSP_FLAT_SIZE).
 *  Validate outSize BEFORE the ctx lookup so a bad pointer +
 *  valid handle reports the more specific kDSpInvalidAttributesErr. */
extern "C" int32_t DSpContext_GetFlattenedSizeHandler(uint32_t ctxRef,
                                                      uint32_t outSize)
{
	if (outSize == 0) {
		DSP_LOG("GetFlattenedSize: NULL outSize (ctxRef=%u) -> "
		        "kDSpInvalidAttributesErr", ctxRef);
		return kDSpInvalidAttributesErr;
	}
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == nullptr) {
		DSP_LOG("GetFlattenedSize: invalid ctxRef=%u -> kDSpInvalidContextErr",
		        ctxRef);
		return kDSpInvalidContextErr;
	}
	WriteMacInt32(outSize, DSP_FLAT_SIZE);
	return kDSpNoErr;
}

/* --- DSpContext_FlattenHandler (sub-op 739) ---
 *
 *  DSp 1.7 PDF p.23: DSpContext_Flatten(inContext, void *outFlatContext).
 *  Serializes the portable {magic, version, size, attr, bookkeeping} subset to
 *  the guest out-buffer via WriteMacInt32 at the DSP_FLAT_* offsets. The buffer
 *  is sized by a prior GetFlattenedSize (DSP_FLAT_SIZE bytes). Runtime-only
 *  fields are NEVER serialized (Flatten is pre-Active, those
 *  resources do not exist; also the info-disclosure mitigation).
 *  Validate outFlatContext BEFORE the ctx lookup. */
extern "C" int32_t DSpContext_FlattenHandler(uint32_t ctxRef,
                                             uint32_t outFlatContext)
{
	if (outFlatContext == 0) {
		DSP_LOG("Flatten: NULL outFlatContext (ctxRef=%u) -> "
		        "kDSpInvalidAttributesErr", ctxRef);
		return kDSpInvalidAttributesErr;
	}
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (ctx == nullptr) {
		DSP_LOG("Flatten: invalid ctxRef=%u -> kDSpInvalidContextErr", ctxRef);
		return kDSpInvalidContextErr;
	}

	/* Serialize via WriteMacInt32 at the DSP_FLAT_* offsets — same field-
	 * serialize idiom as DSpWriteAttributesCore. */
	const DSpContextAttributes *a = &ctx->attr;
	WriteMacInt32(outFlatContext + DSP_FLAT_OFF_magic,               DSP_FLAT_MAGIC);
	WriteMacInt32(outFlatContext + DSP_FLAT_OFF_version,             DSP_FLAT_VERSION);
	WriteMacInt32(outFlatContext + DSP_FLAT_OFF_size,                DSP_FLAT_SIZE);
	WriteMacInt32(outFlatContext + DSP_FLAT_OFF_displayWidth,        a->displayWidth);
	WriteMacInt32(outFlatContext + DSP_FLAT_OFF_displayHeight,       a->displayHeight);
	WriteMacInt32(outFlatContext + DSP_FLAT_OFF_colorNeeds,          a->colorNeeds);
	WriteMacInt32(outFlatContext + DSP_FLAT_OFF_colorTable,          a->colorTable);
	WriteMacInt32(outFlatContext + DSP_FLAT_OFF_contextOptions,      a->contextOptions);
	WriteMacInt32(outFlatContext + DSP_FLAT_OFF_backBufferDepthMask, a->backBufferDepthMask);
	WriteMacInt32(outFlatContext + DSP_FLAT_OFF_displayDepthMask,    a->displayDepthMask);
	WriteMacInt32(outFlatContext + DSP_FLAT_OFF_backBufferBestDepth, a->backBufferBestDepth);
	WriteMacInt32(outFlatContext + DSP_FLAT_OFF_displayBestDepth,    a->displayBestDepth);
	WriteMacInt32(outFlatContext + DSP_FLAT_OFF_pageCount,           a->pageCount);
	WriteMacInt32(outFlatContext + DSP_FLAT_OFF_gameMustConfirmSwitch, a->gameMustConfirmSwitch);
	WriteMacInt32(outFlatContext + DSP_FLAT_OFF_max_frame_rate,      ctx->max_frame_rate);
	WriteMacInt32(outFlatContext + DSP_FLAT_OFF_dirty_grid_w,        ctx->dirty_grid_w);
	WriteMacInt32(outFlatContext + DSP_FLAT_OFF_dirty_grid_h,        ctx->dirty_grid_h);

	DSP_LOG("Flatten: ctx=%u -> %u bytes (%ux%u@%ubpp pc=%u)",
	        ctxRef, DSP_FLAT_SIZE, a->displayWidth, a->displayHeight,
	        a->backBufferBestDepth, a->pageCount);
	return kDSpNoErr;
}

/* --- DSpContext_RestoreHandler (sub-op 741) ---
 *
 *  DSp 1.7 PDF p.22: DSpContext_Restore(void *inFlatContext,
 *  DSpContextReference *outRestoredContext). Reads the blob field-by-field,
 *  validates magic+version (mismatch -> kDSpContextNotFoundErr, the documented
 *  high-probability-of-failure path), then allocates a FRESH metadata-only
 *  context (NO Metal resources — the app re-Reserves before going Active per
 *  PDF p.22) and writes its ctxRef. Both out-ptrs NULL-guarded;
 *  no field is trusted past the magic+version check. */
extern "C" int32_t DSpContext_RestoreHandler(uint32_t inFlatContext,
                                             uint32_t outRestoredContext)
{
	if (inFlatContext == 0 || outRestoredContext == 0) {
		DSP_LOG("Restore: NULL inFlatContext=0x%08x or outRestoredContext=0x%08x"
		        " -> kDSpInvalidAttributesErr", inFlatContext, outRestoredContext);
		return kDSpInvalidAttributesErr;
	}

	/* Read the magic+version FIRST and validate before trusting any field. */
	uint32_t magic   = ReadMacInt32(inFlatContext + DSP_FLAT_OFF_magic);
	uint32_t version = ReadMacInt32(inFlatContext + DSP_FLAT_OFF_version);
	if (magic != DSP_FLAT_MAGIC || version != DSP_FLAT_VERSION) {
		DSP_LOG("Restore: blob magic=0x%08x ver=%u mismatch -> "
		        "kDSpContextNotFoundErr", magic, version);
		return kDSpContextNotFoundErr;
	}

	DSpContextAttributes attr = {};
	attr.displayWidth        = ReadMacInt32(inFlatContext + DSP_FLAT_OFF_displayWidth);
	attr.displayHeight       = ReadMacInt32(inFlatContext + DSP_FLAT_OFF_displayHeight);
	attr.colorNeeds          = ReadMacInt32(inFlatContext + DSP_FLAT_OFF_colorNeeds);
	attr.colorTable          = ReadMacInt32(inFlatContext + DSP_FLAT_OFF_colorTable);
	attr.contextOptions      = ReadMacInt32(inFlatContext + DSP_FLAT_OFF_contextOptions);
	attr.backBufferDepthMask = ReadMacInt32(inFlatContext + DSP_FLAT_OFF_backBufferDepthMask);
	attr.displayDepthMask    = ReadMacInt32(inFlatContext + DSP_FLAT_OFF_displayDepthMask);
	attr.backBufferBestDepth = ReadMacInt32(inFlatContext + DSP_FLAT_OFF_backBufferBestDepth);
	attr.displayBestDepth    = ReadMacInt32(inFlatContext + DSP_FLAT_OFF_displayBestDepth);
	attr.pageCount           = ReadMacInt32(inFlatContext + DSP_FLAT_OFF_pageCount);
	attr.gameMustConfirmSwitch = ReadMacInt32(inFlatContext + DSP_FLAT_OFF_gameMustConfirmSwitch);
	attr.backBufferWidth     = attr.displayWidth;
	attr.backBufferHeight    = attr.displayHeight;

	/* Allocate a fresh metadata-only context (no back-buffer; the restored
	 * context is pre-Active per PDF p.22). DSP_ENUMERATION_INDEX_NONE marks it
	 * off the GetFirst/GetNext chain (a restored ctx is not an enumeration
	 * cursor). A full table -> kDSpContextNotFoundErr (no slot == cannot
	 * locate/host the restored context, the documented Restore-failure path). */
	uint32_t newRef = DSpAllocFirstContextHandle(&attr, DSP_ENUMERATION_INDEX_NONE);
	if (newRef == 0) {
		DSP_LOG("Restore: context table full -> kDSpContextNotFoundErr");
		return kDSpContextNotFoundErr;
	}
	DSpContextPrivate *ctx = DSpGetContext(newRef);
	if (ctx != nullptr) {
		ctx->max_frame_rate = ReadMacInt32(inFlatContext + DSP_FLAT_OFF_max_frame_rate);
		ctx->dirty_grid_w   = ReadMacInt32(inFlatContext + DSP_FLAT_OFF_dirty_grid_w);
		ctx->dirty_grid_h   = ReadMacInt32(inFlatContext + DSP_FLAT_OFF_dirty_grid_h);
	}

	WriteMacInt32(outRestoredContext, newRef);
	DSP_LOG("Restore: blob valid -> fresh metadata ctx=%u (%ux%u@%ubpp pc=%u)",
	        newRef, attr.displayWidth, attr.displayHeight,
	        attr.backBufferBestDepth, attr.pageCount);
	return kDSpNoErr;
}


/* ===================================================================== *
 *  Queue/Switch (sub-ops 742-743).
 *
 *  DSp 1.7 PDF pp.26-27 (DSp-1.7-only deferred-context-switch exports).
 *  Queue (742) stages a child context against a parent; Switch (743) applies
 *  the staged switch. This is RAM-only single-writer bookkeeping on the emul
 *  thread (queued_child / state / vbl_proc_ptr fields) — NO cross-thread queue,
 *  NO concurrency primitive (the retired sub-op-600 cross-thread SPSC ring
 *  was the anti-pattern here, deliberately NOT copied).
 * ===================================================================== */

/* Apply a guest DSpContextAttributes block (on-wire layout, PDF p.65 — same
 * offsets DSpWriteAttributesCore writes) onto a child context's attr. Only
 * called when inDesiredAttributes is non-zero (PDF p.26: Queue's
 * inDesiredAttributes is optional). The attr block is read field-by-field via
 * ReadMacInt32 at the canonical offsets — NO host-pointer cast of the guest
 * address (arm64 >4GiB safety), NO struct overlay.
	 * The caller NULL-guards the ptr; this core trusts only the 12 meaningful
	 * UInt32 fields and ignores filler/reserved. Desired attributes describe the
	 * child drawing environment; if the child came from FindBest/GetFirst, keep
	 * its selected display mode and store the desired size in backBufferWidth /
	 * backBufferHeight. */
static bool DSpApplyDesiredAttributesToChild(DSpContextPrivate *child,
                                             uint32_t inDesiredAttributes)
{
	if (child == nullptr || inDesiredAttributes == 0) return false;
	/* Validate the whole attribute-struct extent lies in mapped RAM
	 * before reading any field (ASVS V5; NQDMetalAddrInBuffer idiom). The
	 * on-wire block matches DSpWriteAttributesCore's layout; the last field
	 * read is gameMustConfirmSwitch (1 byte @ +55), so 56 bytes must be mapped.
	 * Reject-before-mutate: on a bogus non-NULL ptr leave child->attr untouched
	 * and report failure so the caller returns kDSpInvalidAttributesErr rather
	 * than faulting the ReadMacInt* translation layer. */
	const uint32_t kDesiredAttrSize = 56u;
	if (!NQDMetalAddrInBuffer(inDesiredAttributes) ||
	    !NQDMetalAddrInBuffer(inDesiredAttributes + kDesiredAttrSize - 1u)) {
		DSP_LOG("Queue: inDesiredAttributes 0x%08x out of mapped RAM -> reject",
		        inDesiredAttributes);
		return false;
	}
	const uint32_t desiredWidth = ReadMacInt32(inDesiredAttributes +  4);
	const uint32_t desiredHeight = ReadMacInt32(inDesiredAttributes +  8);
	const uint32_t desiredBackDepth = ReadMacInt32(inDesiredAttributes + 40);
	const uint32_t desiredDisplayDepth = ReadMacInt32(inDesiredAttributes + 44);

	child->attr.displayWidth        =
	    DSpReserveActualDisplayDimension(child->attr.displayWidth,
	                                     desiredWidth);
	child->attr.displayHeight       =
	    DSpReserveActualDisplayDimension(child->attr.displayHeight,
	                                     desiredHeight);
	child->attr.colorNeeds          = ReadMacInt32(inDesiredAttributes + 20);
	child->attr.colorTable          = ReadMacInt32(inDesiredAttributes + 24);
	child->attr.contextOptions      = ReadMacInt32(inDesiredAttributes + 28);
	child->attr.backBufferDepthMask = ReadMacInt32(inDesiredAttributes + 32);
	child->attr.displayDepthMask    = ReadMacInt32(inDesiredAttributes + 36);
	child->attr.backBufferBestDepth = desiredBackDepth;
	child->attr.displayBestDepth    =
	    DSpReserveActualDisplayDepth(child->attr.displayBestDepth,
	                                 desiredDisplayDepth,
	                                 desiredBackDepth);
	child->attr.pageCount           = ReadMacInt32(inDesiredAttributes + 48);
	child->attr.gameMustConfirmSwitch = ReadMacInt8(inDesiredAttributes + 55);
	child->attr.backBufferWidth     = DSpReserveBackBufferDimension(
	                                      child->attr.displayWidth,
	                                      desiredWidth);
	child->attr.backBufferHeight    = DSpReserveBackBufferDimension(
	                                      child->attr.displayHeight,
	                                      desiredHeight);
	return true;
}

/* Core deferred-switch staging for the Queue handler. Resolves both ctxRefs,
 * passes the same-display check (trivially true on the single iOS fullscreen
 * display), records parent->queued_child = childRef.
 * inDesiredAttributes (a guest Mac address, 0 == none) is applied to the child
 * by the production handler BEFORE calling this core; the host helper passes 0.
 * Returns kDSpInvalidContextErr if either ctxRef is unresolved
 * (never deref a context pointer before the null-guard). */
static int32_t DSpQueueCore(uint32_t parentCtx, uint32_t childCtx)
{
	DSpContextPrivate *parent = DSpGetContext(parentCtx);
	DSpContextPrivate *child  = DSpGetContext(childCtx);
	if (parent == nullptr || child == nullptr) {
		DSP_LOG("Queue: unresolved parent=%u (%p) or child=%u (%p) -> "
		        "kDSpInvalidContextErr", parentCtx, (void *)parent,
		        childCtx, (void *)child);
		return kDSpInvalidContextErr;
	}
	/* Same-display check (PDF p.26): both contexts must be on the same
	 * display. PocketShaver has exactly one fullscreen display, so any two
	 * resolvable contexts trivially pass. Recorded explicitly so the intent
	 * is documented; there is no incompatible-display path on iOS. */
	parent->queued_child = childCtx;
	DSP_LOG("Queue: parent=%u staged child=%u", parentCtx, childCtx);
	return kDSpNoErr;
}

/* --- DSpContext_QueueHandler (sub-op 742) ---
 *
 *  DSp 1.7 PDF p.26: DSpContext_Queue(inParentContext, inChildContext,
 *  inDesiredAttributes). Queues a context to switch to. "DrawSprocket will
 *  check that both contexts are on the same display" — trivially true on the
 *  single iOS display. inDesiredAttributes (optional) is applied to the child
 *  when non-zero. Unresolved ctxRefs -> kDSpInvalidContextErr. */
extern "C" int32_t DSpContext_QueueHandler(uint32_t parentCtx, uint32_t childCtx,
                                           uint32_t inDesiredAttributes)
{
	/* Resolve both BEFORE any mutation (no deref before the
	 * null-guard inside DSpQueueCore). Apply optional attributes to the child
	 * only when the ptr is non-zero (never deref NULL). */
	DSpContextPrivate *child = DSpGetContext(childCtx);
	if (DSpGetContext(parentCtx) == nullptr || child == nullptr) {
		DSP_LOG("Queue: unresolved parent=%u or child=%u -> "
		        "kDSpInvalidContextErr", parentCtx, childCtx);
		return kDSpInvalidContextErr;
	}
	if (inDesiredAttributes != 0) {
		/* Reject-before-mutate — if the attribute struct is out of
		 * mapped RAM, fail without staging the switch. */
		if (!DSpApplyDesiredAttributesToChild(child, inDesiredAttributes)) {
			DSP_LOG("Queue: inDesiredAttributes=0x%08x invalid -> "
			        "kDSpInvalidAttributesErr", inDesiredAttributes);
			return kDSpInvalidAttributesErr;
		}
		DSP_LOG("Queue: applied inDesiredAttributes=0x%08x to child=%u",
		        inDesiredAttributes, childCtx);
	}
	return DSpQueueCore(parentCtx, childCtx);
}

/* Core deferred-switch apply for the Switch handler. Requires a prior Queue
 * (old->queued_child == newRef, else kDSpInternalErr per PDF p.27 "returns an
 * error" — no partial
 * switch, reject-before-mutate). Routes the OLD context out through
 * SetState(Inactive), then kills its piggyback VBL proc (old->vbl_proc_ptr
 * = 0 — the VBL service walk at dsp_metal_renderer.mm early-outs on ==0),
 * routes the NEW context in through SetState(Active), then clears
 * old->queued_child. Failure paths unwind: a failed deactivation leaves old
 * untouched; a failed activation restores old's VBL proc and re-activates
 * it (best effort), so the switch never strands the display with no Active
 * context or a killed VBL proc on an Active context. */
static int32_t DSpSwitchCore(uint32_t oldCtx, uint32_t newCtx)
{
	DSpContextPrivate *old = DSpGetContext(oldCtx);
	DSpContextPrivate *neu = DSpGetContext(newCtx);
	if (old == nullptr || neu == nullptr) {
		DSP_LOG("Switch: unresolved old=%u (%p) or new=%u (%p) -> "
		        "kDSpInvalidContextErr", oldCtx, (void *)old,
		        newCtx, (void *)neu);
		return kDSpInvalidContextErr;
	}
	/* PDF p.27: "If you did not queue the contexts you want to switch (via
	 * DSpContext_Queue), DSpContext_Switch returns an error." Reject BEFORE
	 * any state mutation (no partial switch). */
	if (old->queued_child != newCtx) {
		DSP_LOG("Switch: old=%u was not queued to new=%u (queued_child=%u) -> "
		        "kDSpInternalErr (PDF p.27 switch-without-queue)",
		        oldCtx, newCtx, old->queued_child);
		return kDSpInternalErr;
	}
	/* Route through SetState so DMC ownership/mode, MainDevice PixMap
	 * redirect/restore, and aggregate Active-fullscreen state all follow the
	 * same path as direct DSpContext_SetState calls. The handoff guard keeps
	 * old->Inactive from restoring the intermediate QuickDraw display mode;
	 * Switch then activates the new context immediately per PDF p.27. */
	DSpContext_SetStateSwitchHandoff(oldCtx);
	int32_t rc = DSpContext_SetStateHandler(
	    oldCtx, (uint32_t)kDSpContextState_Inactive);
	DSpContext_SetStateSwitchHandoff(0);
	if (rc != kDSpNoErr) {
		/* Nothing mutated yet — old stays Active with its VBL proc
		 * intact (no partial switch). */
		DSP_LOG("Switch: old=%u SetState(Inactive) failed rc=%d",
		        oldCtx, rc);
		return rc;
	}
	/* PDF p.27: "switching contexts will kill any piggyback VBL routines
	 * attached to the context you are switching out." Clearing vbl_proc_ptr is
	 * the SetVBLProc(0) uninstall path (p.81) — the DSpVBLServiceCallback walk
	 * skips contexts with vbl_proc_ptr == 0. Killed only AFTER the old
	 * context's deactivation succeeds, so a SetState failure can never leave
	 * an Active context with its VBL proc already destroyed. */
	uint32_t saved_vbl_proc   = old->vbl_proc_ptr;
	uint32_t saved_vbl_refcon = old->vbl_proc_refcon;
	old->vbl_proc_ptr    = 0;
	old->vbl_proc_refcon = 0;
	rc = DSpContext_SetStateHandler(
	    newCtx, (uint32_t)kDSpContextState_Active);
	if (rc != kDSpNoErr) {
		/* Unwind the half-completed switch: restore the old context's VBL
		 * proc and re-activate it (best effort) so a failed activation never
		 * leaves the display with NO Active context. queued_child stays
		 * staged so the app may retry the Switch. */
		old->vbl_proc_ptr    = saved_vbl_proc;
		old->vbl_proc_refcon = saved_vbl_refcon;
		int32_t rb = DSpContext_SetStateHandler(
		    oldCtx, (uint32_t)kDSpContextState_Active);
		DSP_LOG("Switch: new=%u SetState(Active) failed rc=%d; rolled back "
		        "old=%u to Active (rollback rc=%d)",
		        newCtx, rc, oldCtx, rb);
		(void)rb; /* log-only when ACCEL_LOGGING_ENABLED=0 */
		return rc;
	}
	/* Clear the staged switch — a subsequent Switch needs a fresh Queue. */
	old->queued_child = 0;
	DSP_LOG("Switch: old=%u inactive -> new=%u active via SetState, "
	        "old VBL proc killed, queued_child cleared", oldCtx, newCtx);
	return kDSpNoErr;
}

/* --- DSpContext_SwitchHandler (sub-op 743) ---
 *
 *  DSp 1.7 PDF p.27: DSpContext_Switch(inOldContext, inNewContext). Switches
 *  the display context immediately. Requires a prior Queue (else returns an
 *  error). Kills the OLD context's piggyback VBL proc, deactivates OLD through
 *  SetState, and activates NEW through SetState. Unresolved ctxRefs ->
 *  kDSpInvalidContextErr. */
extern "C" int32_t DSpContext_SwitchHandler(uint32_t oldCtx, uint32_t newCtx)
{
	return DSpSwitchCore(oldCtx, newCtx);
}

