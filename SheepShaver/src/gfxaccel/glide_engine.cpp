/*
 *  glide_engine.cpp - CFM symbol-table patcher for 3Dfx Glide (DSp twin)
 *
 *  Same model as dsp_install_hooks.cpp
 *
 *	(C) 2026 RandoOnSteam (battlemageloveryt@gmail.com)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "macos_util.h"
#include "rom_patches.h"
#include "thunks.h"
#include "glide_engine.h"
#include "gfx_log.h"
#include "prefs.h"

#include <cstring>
#include <vector>

extern uint32_t glide_method_tvects[GLIDE_MAX_SUBOPCODE];
extern uint32_t glide_scratch_addr;

static bool glide_hooks_installed = false;
static bool glide_hooks_in_progress = false;

struct GlideInstallSymbol {
	const char *pascal_sym;
	int sub_opcode;
	const char *name;
};

static const GlideInstallSymbol glide_symbols[] = {
#if 1
	{ "\013grGlideInit",              kGlide_grGlideInit,              "grGlideInit" },
#endif
#if 1
	{ "\017grGlideShutdown",          kGlide_grGlideShutdown,          "grGlideShutdown" },
	{ "\021grGlideGetVersion",        kGlide_grGlideGetVersion,        "grGlideGetVersion" },
	{ "\020grSstQueryBoards",         kGlide_grSstQueryBoards,         "grSstQueryBoards" },
	{ "\022grSstQueryHardware",       kGlide_grSstQueryHardware,       "grSstQueryHardware" },
	{ "\013grSstSelect",              kGlide_grSstSelect,              "grSstSelect" },
	{ "\014grSstWinOpen",             kGlide_grSstWinOpen,             "grSstWinOpen" },
	{ "\015grSstWinClose",            kGlide_grSstWinClose,            "grSstWinClose" },
	{ "\014grSstControl",             kGlide_grSstControl,             "grSstControl" },
	{ "\011grSstIdle",                kGlide_grSstIdle,                "grSstIdle" },
	{ "\013grSstIsBusy",              kGlide_grSstIsBusy,              "grSstIsBusy" },
	{ "\013grSstOrigin",              kGlide_grSstOrigin,              "grSstOrigin" },
	{ "\020grSstScreenWidth",         kGlide_grSstScreenWidth,         "grSstScreenWidth" },
	{ "\021grSstScreenHeight",        kGlide_grSstScreenHeight,        "grSstScreenHeight" },
	{ "\013grSstStatus",              kGlide_grSstStatus,              "grSstStatus" },
	{ "\017grSstVRetraceOn",          kGlide_grSstVRetraceOn,          "grSstVRetraceOn" },
	{ "\015grSstVRetrace",            kGlide_grSstVRetraceOn,          "grSstVRetrace" },
	{ "\016grSstVideoLine",           kGlide_grSstVideoLine,           "grSstVideoLine" },
	{ "\015grBufferClear",            kGlide_grBufferClear,            "grBufferClear" },
	{ "\014grBufferSwap",             kGlide_grBufferSwap,             "grBufferSwap" },
	{ "\022grBufferNumPending",       kGlide_grBufferNumPending,       "grBufferNumPending" },
	{ "\020grBuffersPending",         kGlide_grBufferNumPending,       "grBuffersPending" },
	{ "\016grRenderBuffer",           kGlide_grRenderBuffer,           "grRenderBuffer" },
	{ "\013grDrawPoint",              kGlide_grDrawPoint,              "grDrawPoint" },
	{ "\012grDrawLine",               kGlide_grDrawLine,               "grDrawLine" },
	{ "\016grDrawTriangle",           kGlide_grDrawTriangle,           "grDrawTriangle" },
	{ "\020grAADrawTriangle",         kGlide_grAADrawTriangle,         "grAADrawTriangle" },
	{ "\015grDrawPolygon",            kGlide_grDrawPolygon,            "grDrawPolygon" },
	{ "\027grDrawPolygonVertexList",  kGlide_grDrawPolygonVertexList,  "grDrawPolygonVertexList" },
	{ "\023grDrawPlanarPolygon",      kGlide_grDrawPlanarPolygon,      "grDrawPlanarPolygon" },
	{ "\035grDrawPlanarPolygonVertexList", kGlide_grDrawPlanarPolygonVertexList, "grDrawPlanarPolygonVertexList" },
	{ "\024grAlphaBlendFunction",     kGlide_grAlphaBlendFunction,     "grAlphaBlendFunction" },
	{ "\016grAlphaCombine",           kGlide_grAlphaCombine,           "grAlphaCombine" },
	{ "\014grClipWindow",             kGlide_grClipWindow,             "grClipWindow" },
	{ "\016grColorCombine",           kGlide_grColorCombine,           "grColorCombine" },
	{ "\013grColorMask",              kGlide_grColorMask,              "grColorMask" },
	{ "\024grConstantColorValue",     kGlide_grConstantColorValue,     "grConstantColorValue" },
	{ "\012grCullMode",               kGlide_grCullMode,               "grCullMode" },
	{ "\025grDepthBufferFunction",    kGlide_grDepthBufferFunction,    "grDepthBufferFunction" },
	{ "\021grDepthBufferMode",        kGlide_grDepthBufferMode,        "grDepthBufferMode" },
	{ "\013grDepthMask",              kGlide_grDepthMask,              "grDepthMask" },
	{ "\011grFogMode",                kGlide_grFogMode,                "grFogMode" },
	{ "\017grChromakeyMode",          kGlide_grChromakeyMode,          "grChromakeyMode" },
	{ "\020grChromakeyValue",         kGlide_grChromakeyValue,         "grChromakeyValue" },
	{ "\014grDitherMode",             kGlide_grDitherMode,             "grDitherMode" },
	{ "\013grTexSource",              kGlide_grTexSource,              "grTexSource" },
	{ "\016grTexClampMode",           kGlide_grTexClampMode,           "grTexClampMode" },
	{ "\014grTexCombine",             kGlide_grTexCombine,             "grTexCombine" },
	{ "\017grTexFilterMode",          kGlide_grTexFilterMode,          "grTexFilterMode" },
	{ "\017grTexMipMapMode",          kGlide_grTexMipMapMode,          "grTexMipMapMode" },
	{ "\023grTexDownloadMipMap",      kGlide_grTexDownloadMipMap,      "grTexDownloadMipMap" },
	{ "\030grTexDownloadMipMapLevel", kGlide_grTexDownloadMipMapLevel, "grTexDownloadMipMapLevel" },
	{ "\022grTexDownloadTable",       kGlide_grTexDownloadTable,       "grTexDownloadTable" },
	{ "\017grTexMinAddress",          kGlide_grTexMinAddress,          "grTexMinAddress" },
	{ "\017grTexMaxAddress",          kGlide_grTexMaxAddress,          "grTexMaxAddress" },
	{ "\024grTexCalcMemRequired",     kGlide_grTexCalcMemRequired,     "grTexCalcMemRequired" },
#endif
	{ "\005grGet",                    kGlide_grGet,                    "grGet" },
#if 1
	{ "\013grGetString",              kGlide_grGetString,              "grGetString" },
	{ "\020grGetProcAddress",         kGlide_grGetProcAddress,         "grGetProcAddress" },
	{ "\007grReset",                  kGlide_grReset,                  "grReset" },
	{ "\010grEnable",                 kGlide_grEnable,                 "grEnable" },
	{ "\011grDisable",                kGlide_grDisable,                "grDisable" },
	{ "\022grCoordinateSystem",       kGlide_grCoordinateSystem,       "grCoordinateSystem" },
	{ "\021grCoordinateSpace",        kGlide_grCoordinateSystem,       "grCoordinateSpace" },
	{ "\016grVertexLayout",           kGlide_grVertexLayout,           "grVertexLayout" },
	{ "\021grDrawVertexArray",        kGlide_grDrawVertexArray,        "grDrawVertexArray" },
	{ "\033grDrawVertexArrayContiguous", kGlide_grDrawVertexArrayContiguous, "grDrawVertexArrayContiguous" },
	{ "\010grFinish",                 kGlide_grFinish,                 "grFinish" },
	{ "\007grFlush",                  kGlide_grFlush,                  "grFlush" },
	{ "\011grLfbLock",                kGlide_grLfbLock,                "grLfbLock" },
	{ "\013grLfbUnlock",              kGlide_grLfbUnlock,              "grLfbUnlock" },
	{ "\017grLfbReadRegion",          kGlide_grLfbReadRegion,          "grLfbReadRegion" },
	{ "\020grLfbWriteRegion",         kGlide_grLfbWriteRegion,         "grLfbWriteRegion" },
	{ "\024guGammaCorrectionRGB",     kGlide_guGammaCorrectionRGB,     "guGammaCorrectionRGB" },
	{ "\011grSstBusy",                kGlide_grSstIsBusy,              "grSstBusy" },

	{ "\022grQueryResolutions",       kGlide_grQueryResolutions,       "grQueryResolutions" },
	{ "\017grGlideGetState",          kGlide_grGlideGetState,          "grGlideGetState" },
	{ "\014grSstVidMode",             kGlide_grSstVidMode,             "grSstVidMode" },
	{ "\012grViewport",               kGlide_grViewport,               "grViewport" },
	{ "\014grDepthRange",             kGlide_grDepthRange,             "grDepthRange" },
	{ "\026grLfbWriteColorSwizzle",    kGlide_grLfbWriteColorSwizzle,    "grLfbWriteColorSwizzle" },
	{ "\025grLfbWriteColorFormat",     kGlide_grLfbWriteColorFormat,     "grLfbWriteColorFormat" },
	{ "\017grFogColorValue",          kGlide_grFogColorValue,          "grFogColorValue" },
	{ "\020grDepthBiasLevel",         kGlide_grDepthBiasLevel,         "grDepthBiasLevel" },
	{ "\015grChromaRange",            kGlide_grChromaRange,            "grChromaRange" },
	{ "\021grChromaRangeMode",        kGlide_grChromaRangeMode,        "grChromaRangeMode" },
	{ "\034grAlphaControlsITRGBLighting", kGlide_grAlphaControlsITRGBLighting, "grAlphaControlsITRGBLighting" },
	{ "\031grAlphaTestReferenceValue",kGlide_grAlphaTestReferenceValue,"grAlphaTestReferenceValue" },
	{ "\023grAlphaTestFunction",     kGlide_grAlphaTestFunction,     "grAlphaTestFunction" },
	{ "\026grGlideSetVertexLayout",   kGlide_grGlideSetVertexLayout,   "grGlideSetVertexLayout" },
	{ "\026grGlideGetVertexLayout",   kGlide_grGlideGetVertexLayout,   "grGlideGetVertexLayout" },
	{ "\031grTexDownloadTablePartial", kGlide_grTexDownloadTablePartial, "grTexDownloadTablePartial" },
	{ "\027grTexTextureMemRequired",  kGlide_grTexTextureMemRequired,  "grTexTextureMemRequired" },
	{ "\022grTexDetailControl",       kGlide_grTexDetailControl,       "grTexDetailControl" },
	{ "\022grErrorSetCallback",       kGlide_grErrorSetCallback,       "grErrorSetCallback" },
	{ "\007grHints",                  kGlide_grHints,                  "grHints" },
	{ "\026grGammaCorrectionValue",   kGlide_grGammaCorrectionValue,   "grGammaCorrectionValue" },
	{ "\026guColorCombineFunction",   kGlide_guColorCombineFunction,   "guColorCombineFunction" },
	{ "\024guTexCombineFunction",     kGlide_guTexCombineFunction,     "guTexCombineFunction" },
	{ "\024grTexCombineFunction",     kGlide_grTexCombineFunction,     "grTexCombineFunction" },
	{ "\022guTexMemQueryAvail",       kGlide_guTexMemQueryAvail,       "guTexMemQueryAvail" },
	{ "\017grGlideSetState",          kGlide_grGlideSetState,          "grGlideSetState" },
	{ "\012grFogTable",               kGlide_grFogTable,               "grFogTable" },
	{ "\023grDisableAllEffects",      kGlide_grDisableAllEffects,      "grDisableAllEffects" },
	{ "\022grLfbConstantDepth",       kGlide_grLfbConstantDepth,       "grLfbConstantDepth" },
	{ "\022grLfbConstantAlpha",       kGlide_grLfbConstantAlpha,       "grLfbConstantAlpha" },
	{ "\010grSplash",                 kGlide_grSplash,                 "grSplash" },
	{ "\020grLoadGammaTable",         kGlide_grLoadGammaTable,         "grLoadGammaTable" },
	{ "\017grSelectContext",          kGlide_grSelectContext,          "grSelectContext" },
	{ "\020grTexChromaRange",         kGlide_grTexChromaRange,         "grTexChromaRange" },
	{ "\017grTexChromaMode",          kGlide_grTexChromaMode,          "grTexChromaMode" },
	{ "\025grTexMultibaseAddress",    kGlide_grTexMultibaseAddress,    "grTexMultibaseAddress" },
	{ "\016grTexMultibase",           kGlide_grTexMultibase,           "grTexMultibase" },
	{ "\015grTexNCCTable",            kGlide_grTexNCCTable,            "grTexNCCTable" },
	{ "\021grTexLodBiasValue",        kGlide_grTexLodBiasValue,        "grTexLodBiasValue" },
	{ "\037grTexDownloadMipMapLevelPartial", kGlide_grTexDownloadMipMapLevelPartial, "grTexDownloadMipMapLevelPartial" },
	{ "\023guFogGenerateLinear",      kGlide_guFogGenerateLinear,      "guFogGenerateLinear" },
	{ "\021guFogGenerateExp2",        kGlide_guFogGenerateExp2,        "guFogGenerateExp2" },
	{ "\020guFogGenerateExp",         kGlide_guFogGenerateExp,         "guFogGenerateExp" },
	{ "\022guFogTableIndexToW",       kGlide_guFogTableIndexToW,       "guFogTableIndexToW" },
	{ "\015guAlphaSource",            kGlide_guAlphaSource,            "guAlphaSource" },
	/* Do not intercept gu3dfGetInfo/gu3dfLoad, guEncodeRLE16,
	 * guTexCreateColorMipMap, or the guEndianSwap* helpers.  They are pure
	 * guest CPU/file utilities and the stock PEF implementations remain valid;
	 * those implementations call back through the rendering exports above.
	 * Replacing them with an incomplete host dispatcher would only turn valid
	 * Mac file paths and buffers into false failures. */
#endif
};
static const int num_glide_symbols =
	(int)(sizeof(glide_symbols) / sizeof(glide_symbols[0]));

/* Guest CFM fragment name candidates (Pascal). D2 PEF-imports 3DfxGlideLib3.x. */
static const char *const kGlideLibCandidates[] = {
	"\0173DfxGlideLib3.x",   /* 15 - D2 import / real cfrg name */
	"\0203dfx GlideLib3.x",  /* 16 - file name with space */
	"\007Glide3x",
	"\0173DfxGlideLib2.x",
	"\0203dfx GlideLib2.x",
	"\0153DfxGlideLib3",
	"\014Glide Library",
};
static const int kGlideLibCandidateCount =
	(int)(sizeof(kGlideLibCandidates) / sizeof(kGlideLibCandidates[0]));

/*
 * Same mechanics as dsp_install_patch_one:
 *   overwrite first 4 PPC instructions at the export's code with a branch
 *   into our native thunk. CFM callers load code from the TVECT; some paths
 *   also cache/use the code pointer. TVECT-only rewrite produced zero runtime
 *   GlideDispatch (D2 still ran stock entry points) - code smash is required.
 *
 * Also rewrite TVECT[0] so any TVECT that still pointed at the old entry is
 * consistent if something reloads it.
 */
static int glide_install_patch_one(uint32_t orig_tvect, uint32_t hook_tvect, const char *name)
{
	if (hook_tvect == 0) {
		QD3D_INIT_LOG("Glide: hook TVECT for %s not allocated", name);
		return 0;
	}

	uint32_t orig_code = ReadMacInt32(orig_tvect);
	uint32_t hook_code = ReadMacInt32(hook_tvect);
	if (orig_code == 0) {
		QD3D_INIT_LOG("Glide: orig_code for %s is zero (tvect=0x%08x)", name, orig_tvect);
		return 0;
	}

	/* DSp-style: branch at original entry so any direct code call hits us */
	const uint32_t r11 = 11;
	uint32_t hook_hi = (hook_code >> 16) & 0xFFFF;
	uint32_t hook_lo = hook_code & 0xFFFF;

	WriteMacInt32(orig_code + 0,  0x3C000000 | (r11 << 21) | hook_hi);
	WriteMacInt32(orig_code + 4,  0x60000000 | (r11 << 21) | (r11 << 16) | hook_lo);
	WriteMacInt32(orig_code + 8,  0x7C0903A6 | (r11 << 21)); /* mtctr r11 */
	WriteMacInt32(orig_code + 12, 0x4E800420);               /* bctr */

#if EMULATED_PPC
	FlushCodeCache(orig_code, orig_code + 16);
#endif

	QD3D_INIT_LOG("Glide: patched %s: tvect=0x%08x orig_code=0x%08x -> hook=0x%08x",
				  name, orig_tvect, orig_code, hook_code);
	return 1;
}

void GlideInstallHooks(void)
{
	if (!PrefsFindBool("glideaccel")) return;
	if (glide_hooks_installed) return;
	if (glide_hooks_in_progress) {
		QD3D_INIT_LOG("GlideInstallHooks: skipped (re-entrant)");
		return;
	}
	glide_hooks_in_progress = true;

	QD3D_INIT_LOG("GlideInstallHooks: installing FindLibSymbol hooks for Glide");

	/*
	 * ---- Find EVERY installed Glide library, not just the first ----
	 *
	 * Glide 2.x and Glide 3.x ship as two separate CFM fragments
	 * ("3DfxGlideLib2.x" and "3DfxGlideLib3.x") and users routinely have both
	 * extensions installed at once. Different games bind to different ones:
	 * Diablo II PEF-imports Glide 3, while Unreal Tournament PEF-imports
	 * Glide 2.
	 */
	const char *glide_libs[kGlideLibCandidateCount];
	int glide_lib_count = 0;
	uint32 libraryloadoptions = 1;
	if (HasNanoKernel2())
		libraryloadoptions = 2;
	for (int c = 0; c < kGlideLibCandidateCount; c++) {
		const char *candidate = kGlideLibCandidates[c];
		QD3D_INIT_LOG("GlideInstallHooks: trying library \"%s\" (%d chars)",
					  candidate + 1, (int)(unsigned char)candidate[0]);
		uint32_t probe_tvect = FindLibSymbol(candidate, glide_symbols[0].pascal_sym, libraryloadoptions);
		if (probe_tvect == 0)
			continue;

		/*
		 * Candidate list holds several aliases for the same fragment (e.g.
		 * "3DfxGlideLib3.x" and "3dfx GlideLib3.x"). Two names that probe to
		 * the same TVECT are the same library; patching it twice would smash
		 * our own already-installed branch stub and corrupt the hook.
		 */
		bool duplicate = false;
		for (int p = 0; p < glide_lib_count; p++) {
			if (FindLibSymbol(glide_libs[p], glide_symbols[0].pascal_sym) == probe_tvect) {
				duplicate = true;
				break;
			}
		}
		if (duplicate) {
			QD3D_INIT_LOG("GlideInstallHooks: \"%s\" is an alias of an "
						  "already-selected fragment (probe TVECT 0x%08x) - skipping",
						  candidate + 1, probe_tvect);
			continue;
		}

		glide_libs[glide_lib_count++] = candidate;
		QD3D_INIT_LOG("GlideInstallHooks: found library \"%s\" "
					  "(probe TVECT for %s = 0x%08x)",
					  candidate + 1, glide_symbols[0].name, probe_tvect);
	}

	if (glide_lib_count == 0) {
		QD3D_INIT_LOG("GlideInstallHooks: no Glide library resolved yet "
					  "(guest extension present?)");
		glide_hooks_in_progress = false;
		return;
	}
	QD3D_INIT_LOG("GlideInstallHooks: %d Glide fragment(s) will be hooked",
				  glide_lib_count);

	struct CachedTVECT {
		uint32_t tvect;
		int sub_opcode;
		const char *name;
	};
	std::vector<CachedTVECT> cached_tvects;
	int found_count = 0;
	int not_found_count = 0;

	/* ---- Pass 1: resolve all (no WriteMacInt32 yet), across every fragment ---- */
	for (int lib = 0; lib < glide_lib_count; lib++) {
		const char *glide_lib = glide_libs[lib];
		int lib_found = 0, lib_not_found = 0;
		QD3D_INIT_LOG("GlideInstallHooks: unresolved-symbol-diagnostic begin "
					  "(candidate lib = \"%s\")", glide_lib + 1);
		int length_mismatches = 0;
		for (int i = 0; i < num_glide_symbols; i++) {
			const char *psym = glide_symbols[i].pascal_sym;
			int pascal_len = (unsigned char)psym[0];
			int ascii_len = (int)strlen(psym + 1);
			int name_len = (int)strlen(glide_symbols[i].name);
			bool length_match = (pascal_len == ascii_len) && (ascii_len == name_len);
			if (!length_match) length_mismatches++;

			uint32_t tvect = FindLibSymbol(glide_lib, psym);
			/* If table length byte was wrong, rebuild a correct Pascal name from
			 * the C name and retry - wrong length = silent miss + stock HW spin. */
			char fixed_psym[64];
			if (tvect == 0 && name_len > 0 && name_len < 63) {
				fixed_psym[0] = (char)name_len;
				memcpy(fixed_psym + 1, glide_symbols[i].name, (size_t)name_len);
				fixed_psym[1 + name_len] = 0;
				if (!length_match || memcmp(psym + 1, glide_symbols[i].name, (size_t)name_len) != 0)
					tvect = FindLibSymbol(glide_lib, fixed_psym);
			}
			QD3D_INIT_LOG("[diagnostic] %-32s pascal_len=%d strlen(ascii)=%d "
						  "strlen(name)=%d match=%s FindLibSymbol=0x%08x",
						  glide_symbols[i].name, pascal_len, ascii_len, name_len,
						  length_match ? "OK" : "MISMATCH", tvect);

			if (tvect != 0) {
				cached_tvects.push_back({ tvect, glide_symbols[i].sub_opcode,
										  glide_symbols[i].name });
				found_count++;
				lib_found++;
			} else {
				not_found_count++;
				lib_not_found++;
			}
		}
		QD3D_INIT_LOG("GlideInstallHooks: unresolved-symbol-diagnostic end "
					  "lib \"%s\" (%d / %d resolved; "
					  "%d length mismatches; %d not found)",
					  glide_lib + 1,
					  lib_found, num_glide_symbols, length_mismatches,
					  lib_not_found);
	}

	/* ---- Pass 2: patch all resolved symbols ---- */
	int patched_count = 0;
	for (size_t i = 0; i < cached_tvects.size(); i++) {
		uint32_t hook_tvect = glide_method_tvects[cached_tvects[i].sub_opcode];
		patched_count += glide_install_patch_one(cached_tvects[i].tvect, hook_tvect,
												 cached_tvects[i].name);
	}

	QD3D_INIT_LOG("GlideInstallHooks: patched %d functions total "
				  "(resolved = %d, table = %d)",
				  patched_count, found_count, num_glide_symbols);

	glide_hooks_in_progress = false;

	if (patched_count == 0) {
		QD3D_INIT_LOG("GlideInstallHooks: resolved %d fragment(s) but patched 0 "
					  "functions", glide_lib_count);
		return;
	}

	glide_hooks_installed = true;
	QD3D_INIT_LOG("GlideInstallHooks: installed - %d / %d resolved symbols patched "
				  "(%d table rows not in this PEF)",
				  patched_count, found_count, not_found_count);
}

void GlideResetForReboot(void)
{
	QD3D_INIT_LOG("GlideResetForReboot: hooksInstalled=%d", glide_hooks_installed);
	glide_hooks_installed = false;
	glide_hooks_in_progress = false;
}

uint32_t glide_method_tvects[GLIDE_MAX_SUBOPCODE];
uint32_t glide_scratch_addr = 0;

static uint32 AllocateGlideTVECT(int method_id, uint32 glide_opcode)
{
	uint32 scratch_hi = (glide_scratch_addr >> 16) & 0xFFFF;
	uint32 scratch_lo = glide_scratch_addr & 0xFFFF;

	uint32 base = SheepMem::ReserveProc(32);
	uint32 code = base + 8;

	WriteMacInt32(base + 0, code);
	WriteMacInt32(base + 4, 0);

	const uint32 r11 = 11;
	const uint32 r12 = 12;

	/* lis r11, scratch_hi */
	WriteMacInt32(code + 0, 0x3C000000 | (r11 << 21) | (scratch_hi & 0xFFFF));
	/* ori r11, r11, scratch_lo */
	WriteMacInt32(code + 4, 0x60000000 | (r11 << 21) | (r11 << 16) | (scratch_lo & 0xFFFF));
	/* li r12, method_id */
	WriteMacInt32(code + 8, 0x38000000 | (r12 << 21) | (method_id & 0xFFFF));
	/* stw r12, 0(r11) */
	WriteMacInt32(code + 12, 0x90000000 | (r12 << 21) | (r11 << 16));
	/* NATIVE_GLIDE_DISPATCH */
	WriteMacInt32(code + 16, glide_opcode);
	/* blr */
	WriteMacInt32(code + 20, 0x4E800020);

	return base;
}

void GlideThunksInit(void)
{
	QD3D_INIT_LOG("GlideThunksInit: begin");
	glide_scratch_addr = SheepMem::Reserve(32);
	WriteMacInt32(glide_scratch_addr, 0);

#if EMULATED_PPC
	uint32 glide_opcode = NativeOpcode(NATIVE_GLIDE_DISPATCH);
#else
	uint32 glide_opcode = 0;
#endif
	memset(glide_method_tvects, 0, sizeof(glide_method_tvects));

	int tvectcount = 0;
	for (size_t i = 0; i < sizeof(glide_symbols) / sizeof(glide_symbols[0]); i++) {
		int op = glide_symbols[i].sub_opcode;
		uint32 tvect = AllocateGlideTVECT(op, glide_opcode);
		glide_method_tvects[op] = tvect;
		tvectcount++;
	}

	/*
	 * grSurfaceSetTextureSurfaceExt is obtained only through
	 * grGetProcAddress; it is not a normal PEF export.  The stock 3dfx RAVE
	 * driver nevertheless calls it unconditionally from rvTerminate after a
	 * successful board probe, so it still needs a real CFM TVECT.
	 *
	 * Try allocating for all the GetProcAddress funcs
	 */
	for (size_t i = kGlide_FirstGetProcAddress; i <= kGlide_LastGetProcAddress;
		i++) {
		glide_method_tvects[i] =
			AllocateGlideTVECT(i, glide_opcode);
		tvectcount++;
	}

	QD3D_INIT_LOG("GlideThunksInit: allocated %d TVECTs scratch=0x%08x",
	              tvectcount, glide_scratch_addr);
}

