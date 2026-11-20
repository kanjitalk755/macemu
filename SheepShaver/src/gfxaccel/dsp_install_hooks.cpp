/*
 *  dsp_install_hooks.cpp - CFM symbol-table patcher for DrawSprocketLib
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "macos_util.h"          // FindLibSymbol
#include "dsp_engine.h"          // kDSp* enum + DSP_LOG + ACCEL_LOGGING_ENABLED gate (via gfx_log.h)
#include "dsp_fragment_name_policy.h"


#include <cstring>
#include <cstdio>
#include <vector>
extern uint32_t dsp_method_tvects[DSP_MAX_SUBOPCODE];

// ----- File-scope retry-guard triplet (mirrors gl_engine.cpp:1913-1917) -----
static bool dsp_hooks_installed = false;
static bool dsp_hooks_in_progress = false;

struct DSpInstallSymbol {
	const char *pascal_sym;   // \NN<name> where NN is octal length
	int sub_opcode;           // kDSp* from dsp_engine.h
	const char *name;         // For logging
};

static const DSpInstallSymbol dsp_install_symbols[] = {
	// Sub-opcodes 0-2: Startup / Shutdown / Version (DSp 1.7 PDF pp.10-11)
	{ "\012DSpStartup",                      kDSpStartup,                      "DSpStartup" },
	{ "\013DSpShutdown",                     kDSpShutdown,                     "DSpShutdown" },
	{ "\015DSpGetVersion",                   kDSpGetVersion,                   "DSpGetVersion" },

	// Sub-opcodes 100-106: Context lifecycle (DSp 1.7 PDF pp.13-20)
	{ "\022DSpContext_Reserve",              kDSpContext_Reserve,              "DSpContext_Reserve" },
	{ "\022DSpContext_Release",              kDSpContext_Release,              "DSpContext_Release" },
	{ "\030DSpContext_GetBackBuffer",        kDSpContext_GetBackBuffer,        "DSpContext_GetBackBuffer" },
	{ "\026DSpContext_SwapBuffers",          kDSpContext_SwapBuffers,          "DSpContext_SwapBuffers" },
	{ "\023DSpContext_SetState",             kDSpContext_SetState,             "DSpContext_SetState" },
	{ "\023DSpContext_GetState",             kDSpContext_GetState,             "DSpContext_GetState" },
	{ "\036DSpContext_InvalBackBufferRect",  kDSpContext_InvalBackBufferRect,  "DSpContext_InvalBackBufferRect" },

	// Sub-opcodes 200-203: Mode enumeration + best-match (DSp 1.7 PDF pp.13, 16, 17, 18, 22)
	// Sub-op 203 DSpGetNextContext is the stub terminator (PDF p.17).
	// strlen("DSpGetNextContext") = 17; octal 021 = decimal 17.
	{ "\022DSpGetFirstContext",              kDSpGetFirstContext,              "DSpGetFirstContext" },
	{ "\022DSpFindBestContext",              kDSpFindBestContext,              "DSpFindBestContext" },
	{ "\030DSpContext_GetAttributes",        kDSpContext_GetAttributes,        "DSpContext_GetAttributes" },
	{ "\021DSpGetNextContext",               kDSpGetNextContext,               "DSpGetNextContext" },

	// Sub-opcodes 300-301: Palette / CLUT (DSp 1.7 PDF pp.76-77)
	{ "\031DSpContext_SetCLUTEntries",       kDSpContext_SetCLUTEntries,       "DSpContext_SetCLUTEntries" },
	{ "\031DSpContext_GetCLUTEntries",       kDSpContext_GetCLUTEntries,       "DSpContext_GetCLUTEntries" },

	// Sub-opcodes 402-404: Gamma + Fade (DSp 1.7 PDF pp.80-84)
	// DSpContext_SetGamma (400) + DSpContext_GetGamma (401) DROPPED - proven
	// ABSENT from the canonical DrawSprocketLib PEF export table (offline
	// parse). They were never DSp 1.7 exports at all.
	{ "\026DSpContext_FadeGammaIn",          kDSpContext_FadeGammaIn,          "DSpContext_FadeGammaIn" },
	{ "\027DSpContext_FadeGammaOut",         kDSpContext_FadeGammaOut,         "DSpContext_FadeGammaOut" },
	{ "\024DSpContext_FadeGamma",            kDSpContext_FadeGamma,            "DSpContext_FadeGamma" },

	// Sub-opcode 500: VBL service (DSp 1.7 PDF p.81)
	// NOTE: sub-opcode 503 (DSpContext_GetVBLProc) OMITTED - internal test-support
	// affordance per dsp_engine.h (not a PEF export).
	// DSpContext_GetVBLCount (501) + DSpContext_BlankFill (502) DROPPED - proven
	// ABSENT from the canonical DrawSprocketLib PEF export table (offline parse).
	{ "\025DSpContext_SetVBLProc",           kDSpContext_SetVBLProc,           "DSpContext_SetVBLProc" },

	// Sub-opcode 600: Events (empty)

	// Sub-opcodes 700-705: AltBuffers - underlay/overlay (PDF pp.48-53)
	{ "\020DSpAltBuffer_New",                kDSpAltBuffer_New,                "DSpAltBuffer_New" },
	{ "\024DSpAltBuffer_Dispose",            kDSpAltBuffer_Dispose,            "DSpAltBuffer_Dispose" },
	{ "\030DSpAltBuffer_GetCGrafPtr",        kDSpAltBuffer_GetCGrafPtr,        "DSpAltBuffer_GetCGrafPtr" },
	{ "\026DSpAltBuffer_InvalRect",          kDSpAltBuffer_InvalRect,          "DSpAltBuffer_InvalRect" },
	{ "\037DSpContext_GetUnderlayAltBuffer", kDSpContext_GetUnderlayAltBuffer, "DSpContext_GetUnderlayAltBuffer" },
	{ "\037DSpContext_SetUnderlayAltBuffer", kDSpContext_SetUnderlayAltBuffer, "DSpContext_SetUnderlayAltBuffer" },

	// Sub-opcodes 710-711: Blit (PDF pp.68-69)
	{ "\016DSpBlit_Faster",                  kDSpBlit_Faster,                  "DSpBlit_Faster" },
	{ "\017DSpBlit_Fastest",                 kDSpBlit_Fastest,                 "DSpBlit_Fastest" },

	// Sub-opcodes 720-723: coords / mouse (PDF pp.40-46)
	{ "\013DSpGetMouse",                     kDSpGetMouse,                     "DSpGetMouse" },
	{ "\030DSpContext_GlobalToLocal",        kDSpContext_GlobalToLocal,        "DSpContext_GlobalToLocal" },
	{ "\030DSpContext_LocalToGlobal",        kDSpContext_LocalToGlobal,        "DSpContext_LocalToGlobal" },
	{ "\027DSpFindContextFromPoint",         kDSpFindContextFromPoint,         "DSpFindContextFromPoint" },

	// Sub-opcodes 730-738: queries / dirty-rect grid / frame-rate (PDF pp.44-46)
	{ "\021DSpContext_IsBusy",               kDSpContext_IsBusy,               "DSpContext_IsBusy" },
	{ "\027DSpContext_GetDisplayID",         kDSpContext_GetDisplayID,         "DSpContext_GetDisplayID" },
	{ "\031DSpContext_GetFrontBuffer",       kDSpContext_GetFrontBuffer,       "DSpContext_GetFrontBuffer" },
	{ "\036DSpContext_GetMonitorFrequency",  kDSpContext_GetMonitorFrequency,  "DSpContext_GetMonitorFrequency" },
	{ "\032DSpContext_GetMaxFrameRate",      kDSpContext_GetMaxFrameRate,      "DSpContext_GetMaxFrameRate" },
	{ "\032DSpContext_SetMaxFrameRate",      kDSpContext_SetMaxFrameRate,      "DSpContext_SetMaxFrameRate" },
	{ "\037DSpContext_GetDirtyRectGridSize", kDSpContext_GetDirtyRectGridSize, "DSpContext_GetDirtyRectGridSize" },
	{ "\037DSpContext_SetDirtyRectGridSize", kDSpContext_SetDirtyRectGridSize, "DSpContext_SetDirtyRectGridSize" },
	{ "\040DSpContext_GetDirtyRectGridUnits", kDSpContext_GetDirtyRectGridUnits, "DSpContext_GetDirtyRectGridUnits" },

	// Sub-opcodes 739-747: save/restore/flatten + queue/switch + discovery (PDF pp.27, 36-39)
	{ "\022DSpContext_Flatten",              kDSpContext_Flatten,              "DSpContext_Flatten" },
	{ "\033DSpContext_GetFlattenedSize",     kDSpContext_GetFlattenedSize,     "DSpContext_GetFlattenedSize" },
	{ "\022DSpContext_Restore",              kDSpContext_Restore,              "DSpContext_Restore" },
	{ "\020DSpContext_Queue",                kDSpContext_Queue,                "DSpContext_Queue" },
	{ "\021DSpContext_Switch",               kDSpContext_Switch,               "DSpContext_Switch" },
	{ "\035DSpFindBestContextOnDisplayID",   kDSpFindBestContextOnDisplayID,   "DSpFindBestContextOnDisplayID" },
	{ "\024DSpGetCurrentContext",            kDSpGetCurrentContext,            "DSpGetCurrentContext" },
	{ "\027DSpCanUserSelectContext",         kDSpCanUserSelectContext,         "DSpCanUserSelectContext" },
	{ "\024DSpUserSelectContext",            kDSpUserSelectContext,            "DSpUserSelectContext" },

	// Sub-opcode 750: canonical DSpProcessEvent (PDF p.58) - replaces
	// the dropped non-canonical 600 dequeue handler.
	{ "\017DSpProcessEvent",                 kDSpProcessEvent,                 "DSpProcessEvent" },

	// Sub-opcodes 760-761: blanking color + debug mode (PDF pp.45, 85)
	{ "\023DSpSetBlankingColor",             kDSpSetBlankingColor,             "DSpSetBlankingColor" },
	{ "\017DSpSetDebugMode",                 kDSpSetDebugMode,                 "DSpSetDebugMode" },
};
static const int num_dsp_symbols = sizeof(dsp_install_symbols) / sizeof(dsp_install_symbols[0]);
// num_dsp_symbols MUST == 53 - the canonical DrawSprocketLib PEF export count.

/*
 *  dsp_install_patch_one - per-symbol 4-instruction PPC overwrite + FlushCodeCache.
 *  Extracted from the inner patch loop so the patch mechanics are
 *  exercisable in isolation from the export-table walk.
 *
 *  Returns 1 on success, 0 if hook_tvect is zero OR orig_code deref is zero
 *  (either case logged via DSP_LOG; null-guarded).
 */
static int dsp_install_patch_one(uint32_t orig_tvect, uint32_t hook_tvect, const char *name)
{
	if (hook_tvect == 0) {
		DSP_LOG("  hook TVECT for %s not allocated!", name);
		return 0;
	}

	uint32_t orig_code = ReadMacInt32(orig_tvect);
	uint32_t hook_code = ReadMacInt32(hook_tvect);

	if (orig_code == 0) {
		DSP_LOG("  orig_code for %s is zero (TVECT 0x%08x dereferences to 0)",
		        name, orig_tvect);
		return 0;
	}

	const uint32_t r11 = 11;
	uint32_t hook_hi = (hook_code >> 16) & 0xFFFF;
	uint32_t hook_lo = hook_code & 0xFFFF;

	// Overwrite first 4 instructions at orig_code (same encoding as GLInstallHooks)
	// lis r11, hook_code_hi
	WriteMacInt32(orig_code + 0,  0x3C000000 | (r11 << 21) | hook_hi);
	// ori r11, r11, hook_code_lo
	WriteMacInt32(orig_code + 4,  0x60000000 | (r11 << 21) | (r11 << 16) | hook_lo);
	// mtctr r11
	WriteMacInt32(orig_code + 8,  0x7C0903A6 | (r11 << 21));
	// bctr
	WriteMacInt32(orig_code + 12, 0x4E800420);

#if EMULATED_PPC
	FlushCodeCache(orig_code, orig_code + 16);
#endif

	DSP_LOG("  patched %s: orig_code=0x%08x -> hook_code=0x%08x",
	        name, orig_code, hook_code);
	return 1;
}

void DSpInstallHooks(void)
{
	if (dsp_hooks_installed) return;
	if (dsp_hooks_in_progress) {
		DSP_LOG("DSpInstallHooks: skipped (re-entrant call)");
		return;
	}
	dsp_hooks_in_progress = true;

	DSP_LOG("DSpInstallHooks: installing FindLibSymbol hooks for DrawSprocketLib");

	// ---- Pick library name from known candidates ----
	const char *dsp_lib = NULL;
	uint32_t probe_tvect = 0;
	for (int c = 0; c < DSpFragmentCandidateCount(); c++) {
		const char *candidate = DSpFragmentCandidateAt(c);
		DSP_LOG("DSpInstallHooks: trying library \"%s\" (%d chars)",
		        candidate + 1, (int)((unsigned char)candidate[0]));
		probe_tvect = FindLibSymbol(candidate, dsp_install_symbols[0].pascal_sym);
		if (probe_tvect != 0) {
			dsp_lib = candidate;
			DSP_LOG("DSpInstallHooks: found library \"%s\" (probe TVECT for %s = 0x%08x)",
			        dsp_lib + 1, dsp_install_symbols[0].name, probe_tvect);
			break;
		}
	}

	if (dsp_lib == NULL) {
		DSP_LOG("DSpInstallHooks: no DrawSprocketLib candidate resolved yet");
		dsp_hooks_in_progress = false;
		return;
	}

	struct CachedTVECT {
		uint32_t tvect;
		int sub_opcode;
		const char *name;
	};
	std::vector<CachedTVECT> cached_tvects;
	int found_count = 0;

	// ---- First pass: resolve all symbols (CFM re-entrancy mitigation) ----
	DSP_LOG("DSpInstallHooks: unresolved-symbol-diagnostic begin "
	        "(candidate lib = \"%s\")", dsp_lib + 1);
	{
		int length_mismatches = 0;
		for (int i = 0; i < num_dsp_symbols; i++) {
			const char *psym = dsp_install_symbols[i].pascal_sym;
			int pascal_len_octal = (unsigned char)psym[0];  /* already-decoded-to-decimal */
			int ascii_len = (int)strlen(psym + 1);
			int name_len = (int)strlen(dsp_install_symbols[i].name);
			bool length_match = (pascal_len_octal == ascii_len) &&
			                    (ascii_len == name_len);
			if (!length_match) length_mismatches++;

			uint32_t tvect = FindLibSymbol(dsp_lib, psym);
			DSP_LOG("[diagnostic] %-32s pascal_len=%d strlen(ascii)=%d "
			        "strlen(name)=%d match=%s FindLibSymbol=0x%08x",
			        dsp_install_symbols[i].name, pascal_len_octal,
			        ascii_len, name_len,
			        length_match ? "OK" : "MISMATCH", tvect);

			if (tvect != 0) {
				cached_tvects.push_back({ tvect, dsp_install_symbols[i].sub_opcode,
				                           dsp_install_symbols[i].name });
				found_count++;
			}
		}
		DSP_LOG("DSpInstallHooks: unresolved-symbol-diagnostic end "
		        "(%d / %d resolved; %d length mismatches)",
		        found_count, num_dsp_symbols, length_mismatches);
	}

	// ---- Second pass: patch all resolved symbols ----
	int patched_count = 0;
	for (size_t i = 0; i < cached_tvects.size(); i++) {
		uint32_t hook_tvect = dsp_method_tvects[cached_tvects[i].sub_opcode];
		patched_count += dsp_install_patch_one(cached_tvects[i].tvect, hook_tvect, cached_tvects[i].name);
	}

	DSP_LOG("DSpInstallHooks: patched %d functions total (target = %d)",
	        patched_count, num_dsp_symbols);

	// The resolve pass above asked the live fragment for every row, so a
	// symbol that is still missing is not exported by this DrawSprocketLib
	// variant and never will be. Commit on any non-zero patch count; the
	// symbols we did patch are the ones the guest can call.
	dsp_hooks_in_progress = false;

	if (patched_count == 0) {
		DSP_LOG("DSpInstallHooks: resolved \"%s\" but patched 0 functions",
		        dsp_lib + 1);
		return;
	}

	dsp_hooks_installed = true;
	if (patched_count == num_dsp_symbols)
		DSP_LOG("DSpInstallHooks: FULL SUCCESS - all %d symbols patched",
		        num_dsp_symbols);
	else
		DSP_LOG("DSpInstallHooks: PARTIAL COMMIT - %d / %d symbols patched; "
		        "%d not exported by this variant (see diagnostics)",
		        patched_count, num_dsp_symbols,
		        num_dsp_symbols - patched_count);
}

void DSpResetForReboot(void)
{ 
	DSP_LOG("DSpResetForReboot: hooksInstalled=%d", dsp_hooks_installed);
	dsp_hooks_installed   = false;
	dsp_hooks_in_progress = false;
}

