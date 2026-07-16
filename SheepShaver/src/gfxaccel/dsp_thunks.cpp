/*
 *  dsp_thunks.cpp - DSp PPC-to-native thunk allocation
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  DSpThunksInit populates SheepMem TVECT allocation for the 7
 *  context-lifecycle sub-opcodes (100..106): Reserve, Release,
 *  GetBackBuffer, SwapBuffers, SetState, GetState, InvalBackBufferRect.
 *  The TVECTs must exist so CFM FindLibSymbol resolution can return stable
 *  function pointers even before the handler bodies ship.
 *
 *  Pattern: mirrors rave_thunks.cpp:AllocateRaveTVECT verbatim — 32-byte
 *  TVECT (8-byte header + 24-byte thunk code that writes the sub-opcode
 *  to a scratch word, triggers NATIVE_DSP_DISPATCH, and returns).
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "thunks.h"
#include "dsp_engine.h"

#include <cstring>

/* Exported — CFM FindLibSymbol resolver reads this table when the
 * emulated app looks up "DSpContext_Reserve" etc. against the fake
 * DrawSprocketLib linkage fragment. Additional entries are populated as
 * more sub-opcode ranges land. */
uint32_t dsp_method_tvects[DSP_MAX_SUBOPCODE] = {};

/* Scratch word TVECT stubs write the sub-opcode into before the
 * NATIVE_DSP_DISPATCH opcode fires; the native handler reads it back.
 * Layout: [0..3] sub-opcode scratch, [4..7] spare, [8..15] diagnostic.
 *
 * Bug fix (sims-dsp-subop-mismatch): this used to be `static`,
 * but `DSpDispatch` (in dsp_dispatch.cpp) MUST read the sub-opcode from
 * this scratch word — NOT from r3 (which carries the guest's first real
 * function argument, e.g. a ctxRef pointer). Mirrors rave_scratch_addr
 * which is similarly file-scope-but-exported. */
uint32_t dsp_scratch_addr = 0;

/*
 *  Allocate a single DSp TVECT thunk in SheepMem. Structurally identical
 *  to AllocateRaveTVECT in rave_thunks.cpp; only the native opcode
 *  differs. See rave_thunks.cpp:68-109 for the PPC instruction-encoding
 *  commentary.
 *
 *  Layout (32 bytes total):
 *    +0:  code_ptr (= base + 8, points to first instruction)
 *    +4:  TOC (= 0, unused)
 *    +8:  lis   r11, scratch_hi16
 *   +12:  ori   r11, r11, scratch_lo16
 *   +16:  li    r12, method_id
 *   +20:  stw   r12, 0(r11)
 *   +24:  <dsp_opcode>               -- NATIVE_DSP_DISPATCH
 *   +28:  blr
 *
 *  Uses r11 and r12 as scratch registers (volatile in PPC ABI),
 *  preserving r3-r10 which carry the original DSp method arguments.
 *
 *  Returns the TVECT address (base), NOT the code address.
 */
static uint32 AllocateDSpTVECT(int method_id, uint32 dsp_opcode)
{
	uint32 scratch_hi = (dsp_scratch_addr >> 16) & 0xFFFF;
	uint32 scratch_lo = dsp_scratch_addr & 0xFFFF;

	/* Reserve 32 bytes: 8-byte TVECT header + 24 bytes (6 instructions). */
	uint32 base = SheepMem::ReserveProc(32);
	uint32 code = base + 8;

	/* TVECT header: {code_ptr, TOC}. */
	WriteMacInt32(base + 0, code);
	WriteMacInt32(base + 4, 0);

	const uint32 r11 = 11;
	const uint32 r12 = 12;

	/* lis r11, scratch_hi16 */
	WriteMacInt32(code + 0, 0x3C000000 | (r11 << 21) | (scratch_hi & 0xFFFF));

	/* ori r11, r11, scratch_lo16 */
	WriteMacInt32(code + 4, 0x60000000 | (r11 << 21) | (r11 << 16) | (scratch_lo & 0xFFFF));

	/* li r12, method_id */
	WriteMacInt32(code + 8, 0x38000000 | (r12 << 21) | (method_id & 0xFFFF));

	/* stw r12, 0(r11) */
	WriteMacInt32(code + 12, 0x90000000 | (r12 << 21) | (r11 << 16) | (0 & 0xFFFF));

	/* NATIVE_DSP_DISPATCH opcode */
	WriteMacInt32(code + 16, dsp_opcode);

	/* blr */
	WriteMacInt32(code + 20, 0x4E800020);

	return base;
}

void DSpThunksInit(void)
{
	/* Reserve a 16-byte scratch region; same shape as RAVE's scratch. */
	dsp_scratch_addr = SheepMem::Reserve(16);
	WriteMacInt32(dsp_scratch_addr, 0);

	/* NativeOpcode() is only declared when EMULATED_PPC=1 (see
	 * include/thunks.h). PocketShaverTests builds this translation unit
	 * with EMULATED_PPC=0 so the test process can link the dsp_testing_*
	 * helpers defined below without pulling in the full PPC emulator.
	 * Under that mode the TVECT body would not be executable anyway, so
	 * substituting 0 for the fake-opcode slot is safe for the test-time
	 * code paths (tvect tables are populated, but never actually invoked
	 * through PPC emulation). */
#if EMULATED_PPC
	uint32 dsp_opcode = NativeOpcode(NATIVE_DSP_DISPATCH);
#else
	uint32 dsp_opcode = 0;
#endif

	std::memset(dsp_method_tvects, 0, sizeof(dsp_method_tvects));

	int tvect_count = 0;

	/* TVECTs for sub-opcodes 0/1/2 (Startup, Shutdown, GetVersion).
	 * These three entry points are the CFM availability-probe path per DSp 1.7
	 * PDF p.10 — apps call FindSymbol("DSpStartup") FIRST, before any context
	 * API. These TVECTs let DSpInstallHooks patch them into the emulated
	 * DrawSprocketLib symbol table. */
	for (int op = kDSpStartup; op <= kDSpGetVersion; op++) {
		uint32 tvect = AllocateDSpTVECT(op, dsp_opcode);
		if (tvect == 0) {
			DSP_LOG("DSpThunksInit: TVECT alloc failed for subop %d", op);
			continue;
		}
		dsp_method_tvects[op] = tvect;
		tvect_count++;
	}

	/* Allocate one TVECT per public sub-opcode in the 100..106 range.
	 * An earlier revision left this empty; allocating unused TVECTs earlier
	 * risked CFM prematurely resolving them to no-op handlers. */
	for (int op = kDSpContext_Reserve;
	     op <= kDSpContext_InvalBackBufferRect; op++) {
		uint32 tvect = AllocateDSpTVECT(op, dsp_opcode);
		if (tvect == 0) {
			DSP_LOG("DSpThunksInit: TVECT alloc failed for subop %d", op);
			continue;
		}
		dsp_method_tvects[op] = tvect;
		tvect_count++;
	}

	/* Mode-enumeration sub-opcodes 200..202: GetFirstContext,
	 * FindBestContext, GetAttributes. dsp_method_tvects[] is already sized
	 * by DSP_MAX_SUBOPCODE, so no array-size change. */
	for (int op = kDSpGetFirstContext;
	     op <= kDSpContext_GetAttributes; op++) {
		uint32 tvect = AllocateDSpTVECT(op, dsp_opcode);
		if (tvect == 0) {
			DSP_LOG("DSpThunksInit: TVECT alloc failed for subop %d", op);
			continue;
		}
		dsp_method_tvects[op] = tvect;
		tvect_count++;
	}

	/* TVECT for sub-opcode 203 (DSpGetNextContext).
	 * Single-iteration loop preserves the grep-counts-monotonically pattern
	 * (same as sub-op 600). dsp_method_tvects[] is sized by
	 * DSP_MAX_SUBOPCODE so no array-size change. */
	for (int op = kDSpGetNextContext; op <= kDSpGetNextContext; op++) {
		uint32 tvect = AllocateDSpTVECT(op, dsp_opcode);
		if (tvect == 0) {
			DSP_LOG("DSpThunksInit: TVECT alloc failed for subop %d", op);
			continue;
		}
		dsp_method_tvects[op] = tvect;
		tvect_count++;
	}

	/* Palette sub-opcodes 300..301. dsp_method_tvects[] is already sized by
	 * DSP_MAX_SUBOPCODE, so no array-size change. */
	for (int op = kDSpContext_SetCLUTEntries;
	     op <= kDSpContext_GetCLUTEntries; op++) {
		uint32 tvect = AllocateDSpTVECT(op, dsp_opcode);
		if (tvect == 0) {
			DSP_LOG("DSpThunksInit: TVECT alloc failed for subop %d", op);
			continue;
		}
		dsp_method_tvects[op] = tvect;
		tvect_count++;
	}

	/* Gamma+fade sub-opcodes 402..404 (FadeGammaIn / FadeGammaOut /
	 * FadeGamma).
	 *
	 * The non-canonical SetGamma (400) / GetGamma (401) sub-opcodes were
	 * DROPPED (proven absent from the canonical DrawSprocketLib PEF export
	 * table), so this loop now starts at kDSpContext_FadeGammaIn (402)
	 * instead of kDSpContext_SetGamma (400). 400/401 stay reserved / unused. */
	for (int op = kDSpContext_FadeGammaIn;
	     op <= kDSpContext_FadeGamma; op++) {
		uint32 tvect = AllocateDSpTVECT(op, dsp_opcode);
		if (tvect == 0) {
			DSP_LOG("DSpThunksInit: TVECT alloc failed for subop %d", op);
			continue;
		}
		dsp_method_tvects[op] = tvect;
		tvect_count++;
	}

	/* VBL service sub-opcodes 500 (SetVBLProc) + 503 (GetVBLProc internal
	 * test helper).
	 *
	 * The non-canonical GetVBLCount (501) / BlankFill (502) sub-opcodes were
	 * DROPPED (proven absent from the canonical DrawSprocketLib PEF export
	 * table), so 501/502 stay reserved / unused and are no longer allocated a
	 * TVECT. Two single-iteration loops alloc only the surviving 500 + 503
	 * slots; the loop form keeps the grep-counts-monotonically structural
	 * pattern intact. */
	for (int op = kDSpContext_SetVBLProc;
	     op <= kDSpContext_SetVBLProc; op++) {
		uint32 tvect = AllocateDSpTVECT(op, dsp_opcode);
		if (tvect == 0) {
			DSP_LOG("DSpThunksInit: TVECT alloc failed for subop %d", op);
			continue;
		}
		dsp_method_tvects[op] = tvect;
		tvect_count++;
	}
	for (int op = kDSpContext_GetVBLProc;
	     op <= kDSpContext_GetVBLProc; op++) {
		uint32 tvect = AllocateDSpTVECT(op, dsp_opcode);
		if (tvect == 0) {
			DSP_LOG("DSpThunksInit: TVECT alloc failed for subop %d", op);
			continue;
		}
		dsp_method_tvects[op] = tvect;
		tvect_count++;
	}

	/* TVECTs for the 33 real-but-not-yet-installed DrawSprocketLib PEF
	 * exports (sub-opcodes 700..761). DSpInstallHooks needs a non-zero
	 * dsp_method_tvects[subop] for every installed symbol so the
	 * 4-instruction CFM patch can branch into it — so the symbols must have
	 * TVECTs allocated here even though their handler BODIES land elsewhere.
	 * dsp_method_tvects[] is sized by DSP_MAX_SUBOPCODE so no array-size
	 * change beyond the enum bump.
	 *
	 * The enum range is sparse (700-705 AltBuffers, 710-711 Blit, 720-723
	 * coords/mouse, 730-739 queries/grid, 739-747 save-restore-flatten +
	 * queue/switch, 750 DSpProcessEvent, 760-761 blanking/debug),
	 * so each contiguous sub-band is allocated by its own loop to avoid
	 * reserving TVECTs for unused integers between bands. */
	for (int op = kDSpAltBuffer_New;
	     op <= kDSpContext_SetUnderlayAltBuffer; op++) {       /* 700..705 */
		uint32 tvect = AllocateDSpTVECT(op, dsp_opcode);
		if (tvect == 0) {
			DSP_LOG("DSpThunksInit: TVECT alloc failed for subop %d", op);
			continue;
		}
		dsp_method_tvects[op] = tvect;
		tvect_count++;
	}
	for (int op = kDSpBlit_Faster;
	     op <= kDSpBlit_Fastest; op++) {                        /* 710..711 */
		uint32 tvect = AllocateDSpTVECT(op, dsp_opcode);
		if (tvect == 0) {
			DSP_LOG("DSpThunksInit: TVECT alloc failed for subop %d", op);
			continue;
		}
		dsp_method_tvects[op] = tvect;
		tvect_count++;
	}
	for (int op = kDSpGetMouse;
	     op <= kDSpFindContextFromPoint; op++) {                /* 720..723 */
		uint32 tvect = AllocateDSpTVECT(op, dsp_opcode);
		if (tvect == 0) {
			DSP_LOG("DSpThunksInit: TVECT alloc failed for subop %d", op);
			continue;
		}
		dsp_method_tvects[op] = tvect;
		tvect_count++;
	}
	for (int op = kDSpContext_IsBusy;
	     op <= kDSpUserSelectContext; op++) {                   /* 730..747 */
		uint32 tvect = AllocateDSpTVECT(op, dsp_opcode);
		if (tvect == 0) {
			DSP_LOG("DSpThunksInit: TVECT alloc failed for subop %d", op);
			continue;
		}
		dsp_method_tvects[op] = tvect;
		tvect_count++;
	}
	for (int op = kDSpProcessEvent;
	     op <= kDSpProcessEvent; op++) {                        /* 750 */
		uint32 tvect = AllocateDSpTVECT(op, dsp_opcode);
		if (tvect == 0) {
			DSP_LOG("DSpThunksInit: TVECT alloc failed for subop %d", op);
			continue;
		}
		dsp_method_tvects[op] = tvect;
		tvect_count++;
	}
	for (int op = kDSpSetBlankingColor;
	     op <= kDSpSetDebugMode; op++) {                        /* 760..761 */
		uint32 tvect = AllocateDSpTVECT(op, dsp_opcode);
		if (tvect == 0) {
			DSP_LOG("DSpThunksInit: TVECT alloc failed for subop %d", op);
			continue;
		}
		dsp_method_tvects[op] = tvect;
		tvect_count++;
	}

	DSP_LOG("DSpThunksInit: %d TVECTs allocated (sub-opcodes %d..%d + %d..%d + %d..%d + %d..%d + %d..%d + %d..%d + %d + %d + %d..%d + %d..%d + %d..%d + %d..%d + %d + %d..%d), "
	        "scratch at 0x%08x",
	        tvect_count,
	        (int)kDSpStartup,
	        (int)kDSpGetVersion,
	        (int)kDSpContext_Reserve,
	        (int)kDSpContext_InvalBackBufferRect,
	        (int)kDSpGetFirstContext,
	        (int)kDSpContext_GetAttributes,
	        (int)kDSpGetNextContext,
	        (int)kDSpGetNextContext,
	        (int)kDSpContext_SetCLUTEntries,
	        (int)kDSpContext_GetCLUTEntries,
	        (int)kDSpContext_FadeGammaIn,
	        (int)kDSpContext_FadeGamma,
	        (int)kDSpContext_SetVBLProc,
	        (int)kDSpContext_GetVBLProc,
	        (int)kDSpAltBuffer_New,
	        (int)kDSpContext_SetUnderlayAltBuffer,
	        (int)kDSpBlit_Faster,
	        (int)kDSpBlit_Fastest,
	        (int)kDSpGetMouse,
	        (int)kDSpFindContextFromPoint,
	        (int)kDSpContext_IsBusy,
	        (int)kDSpUserSelectContext,
	        (int)kDSpProcessEvent,
	        (int)kDSpSetBlankingColor,
	        (int)kDSpSetDebugMode,
	        (unsigned)dsp_scratch_addr);
}

