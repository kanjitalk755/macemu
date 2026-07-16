/*
 *  dsp_mode_enumerate.h - DSp mode cache + enumeration handler public API.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Provides a pure-function transform from SheepShaver's VideoInfo VModes[]
 *  array to a file-scope cache of DSpContextAttributes records, and a
 *  DSpGetFirstContextHandler (sub-opcode 200) that vends the head of that
 *  cache as a fresh DSp context handle. FindBest and GetAttributes handlers
 *  are backed by the same cache.
 *
 *  Threading contract:
 *    - The s_dsp_modes cache is single-writer emul-thread. Built once at
 *      DSpInit, cleared at DSpShutdown. NO mutex / NO _Atomic. DSp code
 *      does not share this cache with main-thread observer hooks.
 *
 *  Pattern source:
 *    - BasiliskII/src/SDL/video_sdl2.cpp (DMCModeDescFromVModesIndex
 *      pure-function shape)
 *    - SheepShaver/src/gfxaccel/dsp_draw_context.mm (Reserve_Core
 *      validate-populate pattern; DSpAllocContextHandle reuse for
 *      GetFirstContext handle allocation)
 */

#ifndef DSP_MODE_ENUMERATE_H
#define DSP_MODE_ENUMERATE_H

#include <stdint.h>
#include <stddef.h>
#include "dsp_engine.h"     /* DSpContextAttributes + kDSp*Err */

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  Cache build/clear. Called by DSpInit / DSpShutdown. Both
 *  are idempotent: a second DSpBuildModesFromVModes() clears and rebuilds;
 *  DSpClearModes() on an already-empty cache is a no-op.
 */
extern void    DSpBuildModesFromVModes(void);
extern void    DSpClearModes(void);

/*
 *  DSpGetFirstContext dispatch handler (sub-opcode 200).
 *
 *  Semantics (PDF p.20):
 *    - displayID: any value accepted; iOS is single-screen so every
 *      displayID hard-routes to the backing screen. Lifted after The Sims
 *      UAT (debug session sims-dsp-subop-mismatch) showed real-world apps
 *      pass Display-Manager IDs (e.g. 256) and SIGKILL when they cannot
 *      obtain a context.
 *    - outContextRefAddr == 0: return kDSpInvalidAttributesErr.
 *    - s_dsp_modes empty: return kDSpContextNotFoundErr.
 *    - Otherwise: allocate a DSpContextPrivate via the
 *      DSpAllocFirstContextHandle helper, copy s_dsp_modes[0] into its
 *      ctx->attr, write the 1-based handle to outContextRefAddr via
 *      WriteMacInt32, return kDSpNoErr.
 */
extern int32_t DSpGetFirstContextHandler(uint32_t displayID,
                                          uint32_t outContextRefAddr);

/*
 *  DSpGetNextContext dispatch handler (sub-opcode 203).
 *
 *  Advances a GetFirst/GetNext enumeration cursor through the DSp mode
 *  cache. The handle is reused in place so long enumerations do not exhaust
 *  the small DSp context table. When the cursor reaches the end, writes 0
 *  to outContextRefAddr and returns kDSpContextNotFoundErr, matching the
 *  canonical "end of list" signal. Validates outContextRefAddr != 0
 *  (kDSpInvalidAttributesErr) and prevCtxRef-in-handle-table when non-zero
 *  (kDSpInvalidContextErr).
 */
extern int32_t DSpGetNextContextHandler(uint32_t prevCtxRef,
                                         uint32_t outContextRefAddr);

/*
 *  DSpFindBestContext dispatch handler (sub-opcode 201).
 *
 *  Semantics (three-tier algorithm, PDF pp.25, 65-67):
 *    - attrAddr == 0: return kDSpInvalidAttributesErr.
 *    - outContextRefAddr == 0: return kDSpInvalidAttributesErr.
 *    - Reads the requested DSpContextAttributes from guest RAM using the
 *      PDF-p.65 offsets (frequency@0, displayWidth@4, displayHeight@8,
 *      colorNeeds@20, colorTable@24, contextOptions@28,
 *      backBufferDepthMask@32, displayDepthMask@36,
 *      backBufferBestDepth@40, displayBestDepth@44, pageCount@48).
 *    - Invokes DSpFindBestContext_Core (Tier 0 = depth-mask overlap, Tier 1
 *      = bit-depth preference, Tier 2 = resolution match, Tier 3 = refresh
 *      no-op under the frequency=0 policy).
 *    - Core returning nullptr → kDSpContextNotFoundErr (canonical).
 *    - Core returning a mode → allocate a DSpContextPrivate via
 *      DSpAllocFirstContextHandle, populate ctx->attr from the matched
 *      mode, write handle to outContextRefAddr via WriteMacInt32, return
 *      kDSpNoErr.  Table-full → kDSpInternalErr.
 */
extern int32_t DSpFindBestContextHandler(uint32_t attrAddr,
                                          uint32_t outContextRefAddr);

/*
 *  DSpCanUserSelectContext support.
 *
 *  Counts cached modes that satisfy the requested minimum attributes. The
 *  caller maps count > 1 to true, per DSpCanUserSelectContext's "meaningful
 *  user choice" contract.
 */
extern size_t DSpUserSelectableModeCount(
    const DSpContextAttributes *req);

/*
 *  DSpContext_GetAttributes dispatch handler (sub-opcode 202).
 *
 *  Lives in dsp_draw_context.mm (NOT dsp_mode_enumerate.cpp) because it
 *  operates on DSpContextPrivate — the context table is private to
 *  dsp_draw_context.mm per the DMC-invariant discipline. Prototype kept in
 *  this header for parity / discoverability with the other sub-opcode
 *  handlers (DSpGetFirstContextHandler, DSpFindBestContextHandler).
 *
 *  Semantics (PDF p.65-67):
 *    - outAttrAddr == 0: return kDSpInvalidAttributesErr.
 *    - DSpGetContext(ctxRef) == nullptr: return kDSpInvalidContextErr.
 *    - Otherwise: write ctx->attr through PDF-p.65 offsets via
 *      WriteMacInt32/WriteMacInt16/WriteMacInt8 and return kDSpNoErr.
 *
 *  Vends ctx->attr verbatim (stored as-reserved at Reserve time). PDF p.18
 *  "as-active" reading is observationally equivalent because FindBest
 *  returns exact-match or kDSpContextNotFoundErr — no substitution.
 *  Revisit if substitution is ever introduced.
 */
extern int32_t DSpContext_GetAttributesHandler(uint32_t ctxRef,
                                                uint32_t outAttrAddr);


#ifdef __cplusplus
}
#endif

#endif /* DSP_MODE_ENUMERATE_H */
