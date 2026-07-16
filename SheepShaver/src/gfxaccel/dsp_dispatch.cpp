/*
 *  dsp_dispatch.cpp - DSp multiplexed dispatch from sub-opcode to handlers
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  DSpDispatch is invoked from sheepshaver_glue.cpp's NATIVE_DSP_DISPATCH
 *  case. The SUB-OPCODE is read from dsp_scratch_addr (Mac memory word
 *  written by the PPC TVECT thunk's `stw r12, 0(r11)` — see
 *  dsp_thunks.cpp:AllocateDSpTVECT). r3-r8 carry the guest function's raw
 *  argument registers (r3 is the FIRST real function arg — e.g. a ctxRef
 *  handle — NOT the sub-opcode). Returns the value to be written back to
 *  PPC gpr(3).
 *
 *  Signature mirrors RaveDispatch (rave_dispatch.cpp) and GLDispatch
 *  (gl_dispatch.cpp).
 *
 *  sims-dsp-subop-mismatch fix (2026-04-18): this file previously did
 *  `const uint32_t subop = r3;` which treated the guest's first real
 *  argument as the sub-opcode. That worked for zero-arg calls
 *  (Startup/Shutdown/GetVersion — r3 garbage-was-0 by chance) but broke
 *  for every Context_* call (r3 carries the ctxRef pointer → dispatch
 *  fell into `default` → kDSpInternalErr). The Sims crashed on its first
 *  Context_Reserve attempt. RAVE and GL have always read their sub-opcode
 *  from rave_scratch_addr / gl_scratch_addr; DSp now mirrors that pattern
 *  verbatim.
 *
 *  sims-dsp-subop-mismatch follow-on fix (2026-04-18 part 2): after the
 *  scratch-read correction above, The Sims re-UAT revealed a SECOND bug
 *  hiding behind the first. With subop decoupled from r3, r3 now genuinely
 *  IS the guest's first real argument — but every handler case was still
 *  reading args from r4..r7 (the positions that were correct back when r3
 *  was consumed by the subop). That produced an off-by-one on every
 *  Context_* handler: e.g. DSpGetFirstContext saw r4=&ctxRef as "displayID"
 *  (flagged 'non-zero displayID rejected'), and DSpContext_FadeGamma saw
 *  r4=percent as "ctxRef=0 invalid". Fix is purely a dispatch-site arg-index
 *  shift r4→r3, r5→r4, r6→r5, r7→r6 across all 22 Context_* cases; handler
 *  signatures are unchanged. Both fix commits (scratch-read + handler-shift)
 *  are required for end-to-end DSp correctness.
 *
 *  Debug session `dsp-sims-post-reserve-black-screen` fix (2026-04-19):
 *  The Sims evidence proved DSpContext_Reserve's signature was still
 *  wrong. Per DSp 1.7 PDF p.25, the real signature is:
 *      OSStatus DSpContext_Reserve(DSpContextReference inContext,
 *                                   const DSpContextAttributesPtr inDesiredAttributes);
 *  The FIRST argument is the EXISTING ctxRef (returned by FindBestContext
 *  or GetFirstContext), and Reserve's job is to allocate back-buffer
 *  resources ON that existing context. Our handler had been treating r3
 *  as an OUT-pointer to write a NEW handle to, producing a bogus second
 *  handle the app then ignored (since its own `gContext` local was
 *  already set by FindBestContext). The dispatch-site comment was also
 *  wrong. Post-fix: r3 is ctxRef (inContext), r4 is attrAddr
 *  (inDesiredAttributes). Reserve looks up the ctx + attaches the
 *  back-buffer + returns noErr; no out-write.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"        /* ReadMacInt32 */
#include "dsp_engine.h"           /* DSpDispatch + sub-opcode enum + DSP_LOG (pulls in accel_logging.h) */
#include "dsp_draw_context.h"
#include "dsp_mode_enumerate.h"   /* GetFirstContext handler */

/*
 *  Caller LR + r11 stash.
 *
 *  Populated by sheepshaver_glue.cpp at NATIVE_DSP_DISPATCH entry (before
 *  calling DSpDispatch) so we can correlate a guest's call site. r11 is the
 *  CFM TVECT register convention: PPC CFM uses r11 to pass the TVECT address
 *  into the stub, so if a guest's CFM lazy-binding TVECT points at our hook
 *  code, r11 carries the address of THAT TVECT (and therefore tells us which
 *  DSp symbol the guest was attempting). Single-writer (emul thread) /
 *  single-reader (same thread inside DSpDispatch) — no concurrency primitives
 *  needed.
 *
 *  Mirrors the gl_ppc_sp pattern in gl_dispatch.cpp.
 */
uint32_t dsp_caller_lr = 0;
uint32_t dsp_caller_r11 = 0;


uint32_t DSpDispatch(uint32_t r3, uint32_t r4, uint32_t r5,
                     uint32_t r6, uint32_t r7, uint32_t r8)
{
	/* subop comes from the SheepMem scratch word written by the TVECT thunk,
	 * NOT from r3 (which is the guest's first real arg, e.g. ctxRef). Mirrors
	 * rave_dispatch.cpp. */
	const uint32_t subop = ReadMacInt32(dsp_scratch_addr);

	/* Per-sub-opcode entry log, observable in console when dspaccel=true and
	 * an app calls through the DSp TVECT thunks. Logs the FIRST FOUR real
	 * guest args (r3..r6) in the canonical slots apps actually use. r7/r8
	 * dropped from the log line — they're unused by every current handler
	 * case post-shift. */
	DSP_LOG("DSpDispatch: subop %u (r3=0x%08x r4=0x%08x r5=0x%08x r6=0x%08x)",
	        (unsigned)subop, r3, r4, r5, r6);

	/* dsp_caller_lr / dsp_caller_r11 remain populated by sheepshaver_glue.cpp
	 * and available for any future caller-mapping diagnostic. */


	/* r3..r6 carry the first four guest args under the scratch-based-subop
	 * contract. r7 and r8 are unused by every case post-shift (FadeGamma +
	 * SetCLUTEntries/GetCLUTEntries, which were the only 4-arg handlers, now
	 * consume through r6). Suppress-on-purpose. */
	(void)r7;
	(void)r8;

	switch (subop) {
		case kDSpStartup:
			return (uint32_t)DSpStartupHandler();
		case kDSpShutdown:
			return (uint32_t)DSpShutdownHandler();
		case kDSpGetVersion:
			return DSpGetVersionHandler(r3);

		/* Context lifecycle — Reserve + Release.
		 * dsp-sims-post-reserve-black-screen (2026-04-19): Reserve signature
		 * corrected to DSp 1.7 PDF p.25: (inContext, inDesiredAttributes).
		 * r3 = ctxRef (from FindBestContext / GetFirstContext), r4 = attrAddr. */
		case kDSpContext_Reserve:
			return (uint32_t)DSpContext_ReserveHandler(r3 /* ctxRef */,
			                                            r4 /* attrAddr */);
		case kDSpContext_Release:
			return (uint32_t)DSpContext_ReleaseHandler(r3 /* ctxRef */);

		case kDSpContext_GetBackBuffer:
			return (uint32_t)DSpContext_GetBackBufferHandler(r3, r4, r5);
		case kDSpContext_SwapBuffers:
			return (uint32_t)DSpContext_SwapBuffersHandler(r3, r4, r5);
		case kDSpContext_SetState:
			return (uint32_t)DSpContext_SetStateHandler(r3, r4);
		case kDSpContext_GetState:
			return (uint32_t)DSpContext_GetStateHandler(r3, r4);
		case kDSpContext_InvalBackBufferRect:
			return (uint32_t)DSpContext_InvalBackBufferRectHandler(r3, r4);

		/* sub-opcode 200 — DSpGetFirstContext. */
		case kDSpGetFirstContext:
			return (uint32_t)DSpGetFirstContextHandler(r3 /* displayID */,
			                                            r4 /* outContextRefAddr */);

		/* sub-opcode 201 — DSpFindBestContext.
		 * Three-tier algorithm:
		 *   Tier 0 — backBufferDepthMask overlap filter,
		 *   Tier 1 — bit-depth preference (exact → deeper ≥ req → deepest),
		 *   Tier 2 — resolution (exact → smallest-upper-bound → closest-by-area),
		 *   Tier 3 — refresh no-op (frequency=0). */
		case kDSpFindBestContext:
			return (uint32_t)DSpFindBestContextHandler(r3 /* attrAddr */,
			                                            r4 /* outContextRefAddr */);

		/* sub-opcode 202 — DSpContext_GetAttributes.
		 * Vends ctx->attr (cached at Reserve time) to guest RAM using the
		 * PDF-p.65 on-wire byte layout. Handler body lives in
		 * dsp_draw_context.mm (not dsp_mode_enumerate.cpp) because it reads
		 * DSpContextPrivate — extern prototype in dsp_mode_enumerate.h. */
		case kDSpContext_GetAttributes:
			return (uint32_t)DSpContext_GetAttributesHandler(r3 /* ctxRef */,
			                                                  r4 /* outAttrAddr */);

		/* sub-opcode 203 — DSpGetNextContext stub terminator per DSp 1.7 PDF
		 * p.17. iOS single-display returns kDSpContextNotFoundErr + writes 0
		 * to outContextRefAddr. */
		case kDSpGetNextContext:
			return (uint32_t)DSpGetNextContextHandler(r3 /* prevCtxRef */,
			                                           r4 /* outContextRefAddr */);

		/* sub-opcodes 300/301. Arg order follows the DSp 1.7 pp.56-57
		 * wire-format: pointer-before-index. r3=ctxRef, r4=entries address
		 * (the ColorSpec POINTER — in for Set, out for Get), r5=inStartingEntry,
		 * r6=inEntryCount (a COUNT, NOT an inclusive last index). The entries
		 * are 8-byte ColorSpec structs with 16-bit big-endian channels; the
		 * handler converts 16<->8 at the guest-RAM boundary. Internal storage
		 * lives in DSpContextPrivate.clut_bytes (writer, 8-bit) +
		 * clut_bytes_latched (reader, 8-bit). */
		case kDSpContext_SetCLUTEntries:
			return (uint32_t)DSpContext_SetCLUTEntriesHandler(r3 /* ctxRef */,
			                                                   r4 /* inEntries (ptr) */,
			                                                   r5 /* inStartingEntry */,
			                                                   r6 /* inEntryCount */);
		case kDSpContext_GetCLUTEntries:
			return (uint32_t)DSpContext_GetCLUTEntriesHandler(r3 /* ctxRef */,
			                                                   r4 /* outEntries (ptr) */,
			                                                   r5 /* inStartingEntry */,
			                                                   r6 /* inEntryCount */);

		/* Gamma fade sub-opcodes 402/403/404 per the DSp 1.7 ABI (pp.32-35).
		 * There is no `durationVbls` register; the FadeGamma trio matches the
		 * PDF verbatim.
		 *
		 * kDSpContext_SetGamma (400) + kDSpContext_GetGamma (401) cases are
		 * absent — proven absent from the canonical DrawSprocketLib PEF export
		 * table; their enum members + install rows are dropped.
		 *
		 * Argument marshalling (DSp 1.7 ABI):
		 *   FadeGammaIn  (402): r3 = ctxRef, r4 = inZeroIntensityColor (RGBColor*)
		 *   FadeGammaOut (403): r3 = ctxRef, r4 = inZeroIntensityColor (RGBColor*)
		 *   FadeGamma    (404): r3 = ctxRef, r4 = inPercent (SInt32),
		 *                       r5 = inZeroIntensityColor (RGBColor*)
		 *                       (r6 durationVbls dropped — never existed in the
		 *                        real ABI)
		 */
		case kDSpContext_FadeGammaIn:
			return (uint32_t)DSpContext_FadeGammaInHandler(r3 /* ctxRef */,
			                                                r4 /* colorAddr */);
		case kDSpContext_FadeGammaOut:
			return (uint32_t)DSpContext_FadeGammaOutHandler(r3 /* ctxRef */,
			                                                 r4 /* colorAddr */);
		case kDSpContext_FadeGamma:
			return (uint32_t)DSpContext_FadeGammaHandler(r3 /* ctxRef */,
			                                              (int32_t)r4 /* inPercent (SInt32) */,
			                                              r5 /* colorAddr */);

		/* sub-opcodes 500/503 — SetVBLProc + GetVBLProc, with
		 * DSpVBLServiceCallback's per-context walk + PPC VBLProc invocation.
		 *
		 * kDSpContext_GetVBLCount (501) + kDSpContext_BlankFill (502) cases are
		 * absent — proven absent from the canonical DrawSprocketLib PEF export
		 * table; their enum members + install rows are dropped.
		 *
		 * Argument marshalling per dsp_engine.h sub-opcode contract:
		 *   SetVBLProc   (r3=ctxRef, r4=procPtr, r5=refCon)
		 *   GetVBLProc   (r3=ctxRef, r4=procOutAddr, r5=refConOutAddr)
		 * r6/r7 unused by these handlers; r8 still reserved. */
		case kDSpContext_SetVBLProc:
			return (uint32_t)DSpContext_SetVBLProcHandler(r3 /* ctxRef */,
			                                               r4 /* procPtr */,
			                                               r5 /* refCon */);
		case kDSpContext_GetVBLProc:
			return (uint32_t)DSpContext_GetVBLProcHandler(r3 /* ctxRef */,
			                                               r4 /* procOutAddr */,
			                                               r5 /* refConOutAddr */);

		/* ==================================================================
		 * Cheap-family query bodies. Sub-ops 730-738 + 745. r3 = ctxRef (or
		 * displayID for 745), r4/r5 = out-pointer(s).
		 * ================================================================== */
		case kDSpContext_IsBusy:                                    /* sub-op 730 */
			return (uint32_t)DSpContext_IsBusyHandler(r3 /* ctxRef */,
			                                           r4 /* outBusyAddr */);
		case kDSpContext_GetDisplayID:                              /* sub-op 731 */
			return (uint32_t)DSpContext_GetDisplayIDHandler(r3 /* ctxRef */,
			                                                 r4 /* outIDAddr */);
		case kDSpContext_GetDirtyRectGridUnits:                     /* sub-op 738 */
			return (uint32_t)DSpContext_GetDirtyRectGridUnitsHandler(r3 /* ctxRef */,
			                                                          r4 /* outWAddr */,
			                                                          r5 /* outHAddr */);
		case kDSpContext_GetMaxFrameRate:                           /* sub-op 734 */
			return (uint32_t)DSpContext_GetMaxFrameRateHandler(r3 /* ctxRef */,
			                                                    r4 /* outMaxFPSAddr */);
		case kDSpContext_SetMaxFrameRate:                           /* sub-op 735 */
			return (uint32_t)DSpContext_SetMaxFrameRateHandler(r3 /* ctxRef */,
			                                                    r4 /* inMaxFPS */);
		case kDSpContext_GetMonitorFrequency:                       /* sub-op 733 */
			return (uint32_t)DSpContext_GetMonitorFrequencyHandler(r3 /* ctxRef */,
			                                                        r4 /* outFixedAddr */);
		case kDSpContext_GetDirtyRectGridSize:                      /* sub-op 736 */
			return (uint32_t)DSpContext_GetDirtyRectGridSizeHandler(r3 /* ctxRef */,
			                                                         r4 /* outWAddr */,
			                                                         r5 /* outHAddr */);
		case kDSpContext_SetDirtyRectGridSize:                      /* sub-op 737 */
			return (uint32_t)DSpContext_SetDirtyRectGridSizeHandler(r3 /* ctxRef */,
			                                                         r4 /* inW */,
			                                                         r5 /* inH */);
		case kDSpContext_GetFrontBuffer:                            /* sub-op 732 */
			return (uint32_t)DSpContext_GetFrontBufferHandler(r3 /* ctxRef */,
			                                                   r4 /* outCGrafPtrAddr */);
		case kDSpGetCurrentContext:                                 /* sub-op 745 */
			/* NOTE: r3 is a displayID (NOT a ctxRef); r4 is the out-ctxRef
			 * address. Single-display-faithful active-context walk. */
			return (uint32_t)DSpGetCurrentContextHandler(r3 /* displayID */,
			                                              r4 /* outCtxRefAddr */);

		/* ==================================================================
		 * Cheap-family coord/mouse bodies. Sub-ops 720-723. GetMouse (720)
		 * has NO ctxRef — r3 is the out-Point address. GlobalToLocal/
		 * LocalToGlobal (721/722) take r3=ctxRef, r4=ioPoint address; they are
		 * identity at the iOS single fullscreen origin (0,0).
		 * FindContextFromPoint (723) takes the Point by VALUE in r3 — the case
		 * UNPACKS it (v=high half, h=low half), never dereferences r3 as a
		 * pointer.
		 * ================================================================== */
		case kDSpGetMouse:                                          /* sub-op 720 */
			/* NOTE: r3 is the out-Point address (NOT a ctxRef). */
			return (uint32_t)DSpGetMouseHandler(r3 /* outGlobalPointAddr */);
		case kDSpContext_GlobalToLocal:                             /* sub-op 721 */
			return (uint32_t)DSpContext_GlobalToLocalHandler(r3 /* ctxRef */,
			                                                  r4 /* ioPointAddr */);
		case kDSpContext_LocalToGlobal:                             /* sub-op 722 */
			return (uint32_t)DSpContext_LocalToGlobalHandler(r3 /* ctxRef */,
			                                                  r4 /* ioPointAddr */);
		case kDSpFindContextFromPoint: {                            /* sub-op 723 */
			/* The Point is passed by VALUE in r3 (Mac Point packed
			 * v << 16 | h & 0xFFFF) — UNPACK it, NEVER dereference r3 as a
			 * pointer (that would index past the table for attacker coords).
			 * r4 is the out-ctxRef address. */
			const int16_t v = (int16_t)(r3 >> 16);   /* Mac Point: v high half */
			const int16_t h = (int16_t)(r3 & 0xFFFF); /* h low half */
			return (uint32_t)DSpFindContextFromPointHandler(v, h,
			                                                r4 /* outCtxRefAddr */);
		}

		/* ==================================================================
		 * Cheap-family discovery bodies. Sub-ops 744, 746, 747, 760, 761.
		 * Single-display-faithful: FindBestContextOnDisplayID (744) takes 3
		 * args (attrs r3, outCtxRef r4, displayID r5) and delegates to the
		 * existing FindBest matcher; CanUserSelectContext (746) writes false;
		 * UserSelectContext (747) takes 4 args (attrs r3, dialogLoc r4,
		 * eventProc r5, outCtxRef r6) and auto-picks via FindBest with no
		 * dialog. SetBlankingColor (760) and SetDebugMode (761) have NO
		 * ctxRef — r3 is the first real arg (RGBColor address / Boolean value).
		 * ================================================================== */
		case kDSpFindBestContextOnDisplayID:                        /* sub-op 744 */
			return (uint32_t)DSpFindBestContextOnDisplayIDHandler(r3 /* attrAddr */,
			                                                       r4 /* outCtxRefAddr */,
			                                                       r5 /* inDisplayID */);
		case kDSpCanUserSelectContext:                              /* sub-op 746 */
			return (uint32_t)DSpCanUserSelectContextHandler(r3 /* attrAddr */,
			                                                 r4 /* outCanAddr */);
		case kDSpUserSelectContext:                                 /* sub-op 747 */
			return (uint32_t)DSpUserSelectContextHandler(r3 /* attrAddr */,
			                                              r4 /* dialogLoc */,
			                                              r5 /* eventProc */,
			                                              r6 /* outCtxRefAddr */);
		case kDSpSetBlankingColor:                                  /* sub-op 760 */
			/* NOTE: r3 is the RGBColor address (NOT a ctxRef). */
			return (uint32_t)DSpSetBlankingColorHandler(r3 /* inRGBColorAddr */);
		case kDSpSetDebugMode:                                      /* sub-op 761 */
			/* NOTE: r3 is the Boolean debug-mode value (NOT a ctxRef). */
			return (uint32_t)DSpSetDebugModeHandler(r3 /* inDebugMode */);

		/* ==================================================================
		 * Canonical DSpProcessEvent (sub-op 750). DSp 1.7 PDF p.58:
		 *   OSStatus DSpProcessEvent(EventRecord *inEvent,
		 *                            Boolean *outEventWasProcessed)
		 * NO ctxRef: r3 is inEvent (EventRecord*), r4 is
		 * outEventWasProcessed (Boolean*) — the app passes its OWN event
		 * in and DSp inspects it for suspend/resume to drive context state.
		 * This is the OPPOSITE direction to the retired non-canonical
		 * sub-op-600 dequeue handler (retire, NOT repurpose). The SPSC
		 * input-fanout ring is KEPT.
		 * ================================================================== */
		case kDSpProcessEvent:                                      /* sub-op 750 */
			return (uint32_t)DSpProcessEventHandler(r3 /* inEventAddr */,
			                                        r4 /* outProcessedAddr */);

		/* ==================================================================
		 * The 33 real DrawSprocketLib PEF exports (sub-opcodes 700..761).
		 *
		 * Any sub-op without an explicit `case kDSp*:` below is ROUTED THROUGH
		 * `default:`. These sub-ops are fully wired at the SYMBOL level —
		 * their enum members (dsp_engine.h 700-761), install-table rows
		 * (dsp_install_hooks.cpp), and SheepMem TVECT allocations
		 * (dsp_thunks.cpp) all stay in place, so the CFM patch site resolves
		 * and a guest call reaches DSpDispatch. A call with an unhandled
		 * sub-opcode falls into the `default:` arm below, which does the
		 * honest-deferral (DSP_LOG + return kDSpInternalErr): the symbol
		 * resolves + the dispatch is routable, but the call fails loudly with
		 * kDSpInternalErr rather than fabricating success or wrong output.
		 *
		 * Family -> owning area:
		 *   700-705 AltBuffers (New/Dispose/GetCGrafPtr/InvalRect/
		 *           Get+SetUnderlayAltBuffer) ............... heavy
		 *   710-711 Blit (Faster/Fastest) ................... heavy
		 *   720-723 coords/mouse (GetMouse/GlobalToLocal/
		 *           LocalToGlobal/FindContextFromPoint) ..... cheap
		 *           FindContextFromPoint (723) takes the Point by VALUE in r3
		 *           (unpacked in the case).
		 *   730-738 queries/dirty-rect-grid/frame-rate
		 *           (IsBusy/GetDisplayID/GetFrontBuffer/
		 *           Get+SetMonitorFrequency/MaxFrameRate/
		 *           DirtyRectGrid{Size,Units}) ............... cheap
		 *   739-743 save/restore/flatten + queue/switch
		 *           (Flatten/GetFlattenedSize/Restore/
		 *           Queue/Switch) ........................... heavy
		 *   744-747 discovery (FindBestContextOnDisplayID/
		 *           GetCurrentContext/Can+UserSelectContext) ... cheap
		 *   750     canonical DSpProcessEvent
		 *           (replaces dropped non-canonical 600) ..... fidelity
		 *   760-761 SetBlankingColor / SetDebugMode ......... cheap
		 * ================================================================== */

		/* ==================================================================
		 * Heavy-family AltBuffer bodies. Sub-ops 700-705. Real Metal-backed
		 * implementations reusing the engine-blind gfxaccel infra.
		 *
		 * Arg marshalling per the DSp 1.7 PDF ABI:
		 *   New (700)        r3=ctxRef, r4=inVRAMBuffer, r5=inAttributes, r6=outAltBuffer
		 *   Dispose (701)    r3=altBuffer
		 *   GetCGrafPtr (702) r3=altBuffer, r4=bufferKind, r5=outCGrafPtr
		 *   InvalRect (703)  r3=altBuffer, r4=inInvalidRect
		 *   GetUnderlay (704) r3=ctxRef, r4=outUnderlay
		 *   SetUnderlay (705) r3=ctxRef, r4=inNewUnderlay
		 * ================================================================== */
		case kDSpAltBuffer_New:                                     /* sub-op 700 */
			return (uint32_t)DSpAltBuffer_NewHandler(r3 /* ctxRef */,
			                                         r4 /* inVRAMBuffer */,
			                                         r5 /* inAttributes */,
			                                         r6 /* outAltBuffer */);
		case kDSpAltBuffer_Dispose:                                 /* sub-op 701 */
			return (uint32_t)DSpAltBuffer_DisposeHandler(r3 /* altBuffer */);
		case kDSpAltBuffer_GetCGrafPtr:                             /* sub-op 702 */
			return (uint32_t)DSpAltBuffer_GetCGrafPtrHandler(r3 /* altBuffer */,
			                                                 r4 /* bufferKind */,
			                                                 r5 /* outCGrafPtr */);
		case kDSpAltBuffer_InvalRect:                              /* sub-op 703 */
			return (uint32_t)DSpAltBuffer_InvalRectHandler(r3 /* altBuffer */,
			                                               r4 /* inInvalidRect */);
		case kDSpContext_GetUnderlayAltBuffer:                     /* sub-op 704 */
			return (uint32_t)DSpContext_GetUnderlayAltBufferHandler(r3 /* ctxRef */,
			                                                        r4 /* outUnderlay */);
		case kDSpContext_SetUnderlayAltBuffer:                     /* sub-op 705 */
			return (uint32_t)DSpContext_SetUnderlayAltBufferHandler(r3 /* ctxRef */,
			                                                        r4 /* inNewUnderlay */);

		/* ==================================================================
		 * Heavy-family Blit bodies. Sub-ops 710-711. DSpBlit_Faster scales via
		 * the nqd_bitblt_scaled kernel; DSpBlit_Fastest reuses the 1:1
		 * nqd_bitblt kernel. Async = synchronous-then-complete.
		 *
		 * Arg marshalling per the DSp 1.7 PDF ABI:
		 *   Faster (710)  r3=inBlitInfo, r4=inAsyncFlag
		 *   Fastest (711) r3=inBlitInfo, r4=inAsyncFlag
		 * ================================================================== */
		case kDSpBlit_Faster:                                      /* sub-op 710 */
			return (uint32_t)DSpBlit_FasterHandler(r3 /* inBlitInfo */,
			                                       r4 /* inAsyncFlag */);
		case kDSpBlit_Fastest:                                     /* sub-op 711 */
			return (uint32_t)DSpBlit_FastestHandler(r3 /* inBlitInfo */,
			                                        r4 /* inAsyncFlag */);

		/* ==================================================================
		 * Heavy-family Save/Restore/Flatten bodies. Sub-ops 739-741.
		 * Self-consistent magic+version round-trip; Flatten is pre-Active so
		 * the runtime/Metal fields are NOT serialized; a Restore magic/version
		 * mismatch -> kDSpContextNotFoundErr (documented valid, PDF p.22). Pure
		 * RAM serialization.
		 *
		 * Arg marshalling per the DSp 1.7 PDF ABI:
		 *   Flatten (739)         r3=ctxRef, r4=outFlatContext
		 *   GetFlattenedSize (740) r3=ctxRef, r4=outSize
		 *   Restore (741)         r3=inFlatContext, r4=outRestoredContext
		 * ================================================================== */
		case kDSpContext_Flatten:                                  /* sub-op 739 */
			return (uint32_t)DSpContext_FlattenHandler(r3 /* ctxRef */,
			                                           r4 /* outFlatContext */);
		case kDSpContext_GetFlattenedSize:                         /* sub-op 740 */
			return (uint32_t)DSpContext_GetFlattenedSizeHandler(r3 /* ctxRef */,
			                                                    r4 /* outSize */);
		case kDSpContext_Restore:                                  /* sub-op 741 */
			return (uint32_t)DSpContext_RestoreHandler(r3 /* inFlatContext */,
			                                           r4 /* outRestoredContext */);

		/* ==================================================================
		 * Queue/Switch (sub-ops 742-743), the DSp 1.7 deferred-context-switch
		 * family (PDF pp.26-27). RAM-only single-writer bookkeeping
		 * (queued_child / state / vbl_proc_ptr).
		 *
		 * Arg marshalling per the DSp 1.7 PDF ABI:
		 *   Queue (742)  r3=parentCtx, r4=childCtx, r5=inDesiredAttributes
		 *   Switch (743) r3=oldCtx,    r4=newCtx
		 * ================================================================== */
		case kDSpContext_Queue:                                    /* sub-op 742 */
			return (uint32_t)DSpContext_QueueHandler(r3 /* parentCtx */,
			                                         r4 /* childCtx */,
			                                         r5 /* inDesiredAttributes */);
		case kDSpContext_Switch:                                   /* sub-op 743 */
			return (uint32_t)DSpContext_SwitchHandler(r3 /* oldCtx */,
			                                          r4 /* newCtx */);

		default:
			DSP_LOG("DSpDispatch: unknown sub-opcode %u - returning kDSpInternalErr",
			        (unsigned)subop);
			return (uint32_t)kDSpInternalErr;
	}
}
