/*
 *  glide_dispatch.cpp - NATIVE_GLIDE_DISPATCH multiplex
 *
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
#include "vec_data.h"
#include "cpu_emulation.h"
#include "thunks.h"        /* SheepMem */
#include "macos_util.h"
#include "glide_engine.h"
#include "display_mode_controller.h"
#include "gfx_log.h"
#include "gfxaccel_backend.h"

#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <cmath>          /* pow (guFogTableIndexToW) */
#include <vector>

#if !defined(GFXACCEL_USE_OPENGL)
/* Metal Glide raster path is not implemented yet; keep dispatch linkable. */
int  GlideMetalInit(void) { return -1; }
void GlideMetalShutdown(void) {}
int  GlideMetalWinOpen(int, int, int) { return -1; }
void GlideMetalWinClose(void) {}
void GlideMetalBufferClear(uint32_t, uint32_t, uint32_t) {}
void GlideMetalBufferSwap(int) {}
void GlideMetalDrawPoint(const void *) {}
void GlideMetalDrawLine(const void *, const void *) {}
void GlideMetalDrawTriangle(const void *, const void *, const void *) {}
void GlideMetalDrawPolygon(int, const void *const *) {}
void GlideMetalDrawPolygonIndexed(int, const void *, const void *, uint32_t) {}
void GlideMetalDrawPolygonContiguous(int, const void *, uint32_t) {}
void GlideMetalDrawVertexArray(uint32_t, uint32_t, const void *const *) {}
void GlideMetalDrawVertexArrayContiguous(uint32_t, uint32_t, const void *, uint32_t) {}
void GlideMetalApplyState(void) {}
void GlideMetalSetClipWindow(int, int, int, int) {}
void GlideMetalSetColorMask(int, int, int, int) {}
void GlideMetalSetAlphaTest(int, int, float) {}
void GlideMetalSetChromakey(void) {}
void GlideMetalSetFog(int, uint32_t) {}
void GlideMetalSetDepthBias(float) {}
void GlideMetalMarkContent(void) {}
void GlideMetalPublishOverlay(int) {}
void GlideMetalSplash(void) {}
void GlideMetalFlush(void) {}
void GlideMetalFinish(void) {}
void GlideMetalUploadLfbAndPresent(const uint8_t *, int, int, int, int) {}
void GlideMetalTexDownloadLevel(uint32_t, int, int, int, int, const void *, uint32_t) {}
void GlideMetalTexSource(uint32_t, int, int, int, int, int) {}
void GlideMetalTexDownloadTable(int, const void *) {}
#endif

/* State accessors (glide_state.cpp) */
extern void GlideStateResetDefaults(void);
extern bool GlideStateIsInited(void);
extern bool GlideStateWindowOpen(void);
extern int  GlideStateWidth(void);
extern int  GlideStateHeight(void);
extern int  GlideStateOriginUpperLeft(void);
extern void GlideStateSetInited(bool v);
extern void GlideStateSetWindowOpen(bool v);
extern void GlideStateSetWin(int w, int h, int origin_ul, int cfmt, int nbuf, int naux);
extern bool GlideStateBeginPresentation(int width, int height);
extern void GlideStateEndPresentation(void);
extern void GlideStateSetClip(int minx, int miny, int maxx, int maxy);
extern void GlideStateSetConstantColor(uint32_t c);
extern void GlideStateSetConstantColor4(float r, float g, float b, float a);
extern uint32_t GlideStateConstantColor(void);
extern void GlideStateSetDepth(int mode, int func, int mask);
extern void GlideStateSetDepthBias(float b);
extern void GlideStateSetCull(int mode);
extern void GlideStateSetAlphaBlend(int s, int d, int sa, int da);
extern void GlideStateSetAlphaTest(int func, float ref);
extern int GlideStateAlphaTestEnabled(void);
extern int GlideStateAlphaTestFunc(void);
extern float GlideStateAlphaTestRef(void);
extern void GlideStateSetColorMask(int r, int g, int b, int a);
extern void GlideStateSetChromakey(int mode, uint32_t value);
extern void GlideStateSetChromaRange(uint32_t color0, uint32_t color1, int match);
extern void GlideStateSetChromaRangeMode(int mode);
extern void GlideStateSetTexChromaRange(uint32_t color0, uint32_t color1, int match);
extern void GlideStateSetTexChromaMode(int mode);
extern int GlideStateChromaMode(void);
extern uint32_t GlideStateChromaValue(void);
extern void GlideStateSetFog(int mode, uint32_t color);
extern void GlideStateSetFogTable(const uint8_t *table);
extern int GlideStateFogMode(void);
extern uint32_t GlideStateFogColor(void);
extern void GlideStateSetAlphaControlsLighting(int enable);
extern void GlideStateSetDither(int mode);
extern void GlideStateSetRenderBuffer(int buf);
extern void GlideStateSetColorCombine(int func, int factor, int local,
									 int other, int invert);
extern void GlideStateSetAlphaCombine(int func, int factor, int local,
									 int other, int invert);
extern void GlideStateSetTexCombine(int rgb_func, int rgb_factor,
								   int alpha_func, int alpha_factor,
								   int rgb_invert, int alpha_invert);
extern void GlideStateSetTexLodBias(float b);
extern void GlideStateSetTexDetail(int n, int d, int clamp);
extern void GlideStateSetTexMipMapMode(int mode);
extern void GlideStateSetLfbConstAlpha(uint32_t a);
extern void GlideStateSetLfbConstDepth(uint32_t d);
extern void GlideStateSetLfbWriteColorFormat(int f);
extern void GlideStateSetLfbWriteColorSwizzle(int s);
extern void GlideStateSetCoordSystem(int c);
extern void GlideStateSetViewport(int x, int y, int width, int height);
extern void GlideStateSetDepthRange(float near_value, float far_value);
extern int GlideStateViewportX(void);
extern int GlideStateViewportY(void);
extern int GlideStateViewportWidth(void);
extern int GlideStateViewportHeight(void);
extern void GlideStateDisableAllEffects(void);
extern void GlideStateSetVertexLayout(int param, int offset, int mode);
extern void GlideStateSetGlide2VertexLayout(void);
extern void GlideStateSetTexSource(uint32_t startAddress, int evenOdd, int format);
extern void GlideStateSetTexSourceEx(uint32_t startAddress, int evenOdd,
									 int small_lod, int large_lod, int aspect_log2, int format);
extern void GlideStateSetTexClamp(int s, int t);
extern void GlideStateSetTexFilter(int minf, int magf);
extern bool GlideStateLfbWriteRegion(int dst_buffer, int dst_x, int dst_y,
									 int src_format, int src_w, int src_h,
									 int src_stride, const void *src_data);
extern bool GlideStateLfbReadRegion(int src_buffer, int src_x, int src_y,
									int src_w, int src_h, int dst_stride, void *dst_data);
extern uint32_t GlideStateTexMinAddress(int tmu);
extern uint32_t GlideStateTexMaxAddress(int tmu);
extern uint32_t GlideStateFbMem(void);
extern uint32_t GlideStateTmuMem(void);
extern uint32_t GlideStatePresentationOwnerBefore(void);
extern uint32_t GlideTexCalcMemRequired(int small_lod, int large_lod, int aspect_log2, int format);
extern uint32_t GlideTexLevelSizeBytes(int lod, int aspect_log2, int format);
extern void GlideTexObserveLodRange(int small_lod, int large_lod);
extern void GlideStateResolveResolution(int res_enum, int *out_w, int *out_h);
extern void GlideStateLfbRelease(void);
extern bool GlideStateLfbLock(int type, int buffer, int write_mode, int origin,
							  uint32_t *out_ptr, uint32_t *out_stride, int *out_write_mode);
extern bool GlideStateLfbUnlock(int buffer);
extern bool GlideStateLfbIsLocked(void);
extern uint32_t GlideStateLfbGuestPtr(void);
extern uint32_t GlideStateLfbStride(void);
extern int GlideStateLfbWriteMode(void);
extern int GlideStateLfbType(void);
extern int GlideStateLfbBuffer(void);
extern int GlideStateLfbBpp(void);
extern const uint8_t *GlideStateLfbConvertToBGRA(int *out_w, int *out_h, int *out_pitch);
extern void GlideStateLfbClear(uint16_t color565);
extern void GlideStateApplyGammaRGB(float red, float green, float blue);
extern void GlideStateApplyGammaTable(const uint8_t *lut);

static void GlideLog(const char *fmt, ...)
{
#if ACCEL_LOGGING_ENABLED
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	QD3D_INIT_LOG("%s", buf);
	/* Hang diagnosis: force line out immediately so the last line is real. */
	fflush(stderr);
	fflush(stdout);
#else
	(void)fmt;
#endif
}

/*
 * Guest GrHwConfiguration: report one SST-1 (Voodoo) class board.
 *
 * Layout must match the caller's struct EXACTLY - it is normally a stack
 * local, so writing past its end corrupts the caller's frame.
 *
 *   struct GrTMUConfig_t   { int tmuRev; int tmuRam; }                    =  8
 *   struct GrVoodooConfig_t{ int fbRam, fbiRev, nTexelfx; FxBool sliDetect;
 *                            GrTMUConfig_t tmuConfig[2]; }                = 32
 *   struct GrHwConfiguration {
 *     int num_sst;                                             // +0
 *     struct { GrSstType type;            // +0 within entry
 *              union SstBoard_u sstBoard; // +4 within entry, 32 bytes
 *     } SSTs[4];                                               // +4, 36 each
 *   };                                                         // = 148 bytes
 */
#define GLIDE_HWCONFIG_NUM_SST      4   /* MAX_NUM_SST */
#define GLIDE_HWCONFIG_SST_STRIDE   36  /* type(4) + union sstBoard(32), GLIDE_NUM_TMU=2 */
#define GLIDE_HWCONFIG_SIZE \
	(4 + GLIDE_HWCONFIG_NUM_SST * GLIDE_HWCONFIG_SST_STRIDE) /* 148 */

static void write_query_hardware(uint32_t hwConfigPtr)
{
	if (!hwConfigPtr) return;

	/* Clear exactly the struct, never a byte more. */
	for (uint32_t off = 0; off < GLIDE_HWCONFIG_SIZE; off += 4)
		WriteMacInt32(hwConfigPtr + off, 0);

	WriteMacInt32(hwConfigPtr + 0, 1); /* num_sst: one board */

	/* SSTs[0]: type = GR_SSTTYPE_VOODOO (0, already zeroed), then the
	 * VoodooConfig union member at +4 within the entry. */
	const uint32_t sst0 = hwConfigPtr + 4;
	const uint32_t voodoo = sst0 + 4;
	WriteMacInt32(voodoo + 0,  (int32_t)(GlideStateFbMem() / (1024 * 1024))); /* fbRam MB */
	WriteMacInt32(voodoo + 4,  2);                        /* fbiRev */
	WriteMacInt32(voodoo + 8,  1);                        /* nTexelfx */
	WriteMacInt32(voodoo + 12, 0);                        /* sliDetect = FXFALSE */
	/* tmuConfig[0]: { tmuRev, tmuRam } at +16; tmuConfig[1] stays zeroed. */
	WriteMacInt32(voodoo + 16, 1);                        /* tmuRev */
	WriteMacInt32(voodoo + 20,
				  (int32_t)(GlideStateTmuMem() / (1024 * 1024))); /* tmuRam MB */
}
/*
 * Glide 3: FxU32 grGet(FxU32 pname, FxU32 plength, void *params).
 * The result is the number of bytes written, not an FxBool.  Bad tokens and
 * short buffers return zero without modifying the caller's buffer.
 */
static uint32_t handle_grGet(uint32_t pname, uint32_t plength, uint32_t paramsPtr)
{
	if (!paramsPtr) {
		GlideLog("grGet(pname=%u plength=%u params=NULL) -> 0 bytes", pname, plength);
		return 0;
	}

	uint32_t v0 = 0, v1 = 0, v2 = 0, v3 = 0;
	uint32_t required = 4;
	switch (pname) {
	case GR_BITS_DEPTH:                v0 = 16; break;
	case GR_BITS_RGBA:
		v0 = 5; v1 = 6; v2 = 5; v3 = 0; required = 16; break;
	case GR_FIFO_FULLNESS:
		/* Minimum and maximum FIFO fullness are a two-value Glide 3 query. */
		v0 = 0; v1 = 0; required = 8; break;
	case GR_FOG_TABLE_ENTRIES:         v0 = 64; break;
	case GR_GAMMA_TABLE_ENTRIES:       v0 = 256; break;
	case GR_GLIDE_STATE_SIZE:          v0 = 4096; break;
	case GR_GLIDE_VERTEXLAYOUT_SIZE:   v0 = 256; break;
	case GR_IS_BUSY:                   v0 = 0; break;
	case GR_LFB_PIXEL_PIPE:            v0 = 0; break;
	case GR_MAX_TEXTURE_SIZE:          v0 = 256; break;
	case GR_MAX_TEXTURE_ASPECT_RATIO:  v0 = 8; break;
	case GR_MEMORY_FB:                 v0 = GlideStateFbMem(); break;
	case GR_MEMORY_TMU:                v0 = GlideStateTmuMem(); break;
	case GR_MEMORY_UMA:                v0 = 0; break;
	case GR_NUM_BOARDS:                v0 = 1; break;
	case GR_NON_POWER_OF_TWO_TEXTURES: v0 = 0; break;
	case GR_NUM_FB:                    v0 = 1; break;
	case GR_NUM_SWAP_HISTORY_BUFFER:   v0 = 8; break;
	case GR_NUM_TMU:                   v0 = 1; break;
	case GR_PENDING_BUFFERSWAPS:
		v0 = 0; break; /* swaps complete synchronously */
	case GR_REVISION_FB:               v0 = 2; break;
	case GR_REVISION_TMU:              v0 = 1; break;
	case GR_STATS_LINES:
	case GR_STATS_PIXELS_AFUNC_FAIL:
	case GR_STATS_PIXELS_CHROMA_FAIL:
	case GR_STATS_PIXELS_DEPTHFUNC_FAIL:
	case GR_STATS_PIXELS_IN:
	case GR_STATS_PIXELS_OUT:
	case GR_STATS_PIXELS:
	case GR_STATS_POINTS:
	case GR_STATS_TRIANGLES_IN:
	case GR_STATS_TRIANGLES_OUT:
	case GR_STATS_TRIANGLES:
	case GR_SWAP_HISTORY:              v0 = 0; break;
	case GR_SUPPORTS_PASSTHRU:         v0 = 0; break;
	case GR_TEXTURE_ALIGN:             v0 = 8; break;
	case GR_VIDEO_POSITION:
		v0 = 0; v1 = 0; required = 8; break;
	case GR_VIEWPORT:
		v0 = (uint32_t)GlideStateViewportX();
		v1 = (uint32_t)GlideStateViewportY();
		v2 = (uint32_t)GlideStateViewportWidth();
		v3 = (uint32_t)GlideStateViewportHeight();
		required = 16;
		break;
	case GR_WDEPTH_MIN_MAX:
	case GR_ZDEPTH_MIN_MAX:
		v0 = 0; v1 = 65535; required = 8; break;
	case GR_VERTEX_PARAMETER:          v0 = 0; break;
	case GR_BITS_GAMMA:                v0 = 8; break;
	default:
		GlideLog("grGet(pname=0x%x plength=%u) unknown -> 0 bytes", pname, plength);
		return 0;
	}

	if (plength < required) {
		GlideLog("grGet(pname=0x%x plength=%u) needs %u -> 0 bytes",
				  pname, plength, required);
		return 0;
	}
	WriteMacInt32(paramsPtr + 0, (int32_t)v0);
	if (required >= 8)  WriteMacInt32(paramsPtr + 4, (int32_t)v1);
	if (required >= 12) WriteMacInt32(paramsPtr + 8, (int32_t)v2);
	if (required >= 16) WriteMacInt32(paramsPtr + 12, (int32_t)v3);
	GlideLog("grGet(pname=0x%x plength=%u) -> v0=%u v1=%u bytes=%u",
			  pname, plength, v0, v1, required);
	return required;
}

/* Copy a static C string into SheepMem once, then reuse the reservation. */
static uint32_t ensure(uint32_t &slot, const char *text)
{
	if (slot) return slot;
	size_t n = strlen(text) + 1;
	slot = SheepMem::Reserve((uint32)((n + 3) & ~3u));
	uint8 *p = Mac2HostAddr(slot);
	if (p) memcpy(p, text, n);
	return slot;
}

static uint32_t handle_grGetString(uint32_t pname)
{
	/* Return a Mac pointer to a static C string in SheepMem once allocated. */
	static uint32_t s_vendor = 0, s_renderer = 0, s_version = 0, s_ext = 0;
	/* Official grGetString tokens are 0xa0-0xa4. */
	switch (pname) {
	case GR_EXTENSION:
		/*
		 * The Mac 3dfx RAVE driver searches for the space-delimited
		 * " SURFACE " token. Its RvTerminate then calls
		 * grSurfaceSetTextureSurfaceExt for TMUs 0 and 1 without checking
		 * the ProcPtr. Advertise the group so it resolves our safe unbind
		 * hook; the remaining SURFACE functions stay unresolved, keeping
		 * the driver's full surface capability disabled.
		 *
		 * I.E. Both 3dfx Rave (through RvInitialize/RvTerminate) _AND_
		 * the game (for example Diablo II) call our glide functions.
		 * Without these implemented Diablo II crashes on exit.
		 */
		return ensure(s_ext, " SURFACE DEVICE CHROMARANGE TEXCHROMA ");
	case GR_HARDWARE:
		return ensure(s_renderer, "Voodoo 3");
	case GR_RENDERER:
		return ensure(s_renderer, "Glide");
	case GR_VENDOR:
		return ensure(s_vendor, "3Dfx Interactive");
	case GR_VERSION:
		return ensure(s_version, "3.10");
	default:
		GlideLog("grGetString(pname=0x%x) unknown", pname);
		return ensure(s_version, "3.10");
	}
}

static float GlideFogTableIndexToWValue(int index)
{
	return std::ldexp(1.0f, 3 + (index >> 2)) /
		   (float)(8 - (index & 3));
}

enum GlideFogGenerator {
	kGlideFogLinear,
	kGlideFogExp,
	kGlideFogExp2
};

static void GlideGenerateFogTable(uint32_t table_mac, GlideFogGenerator kind,
								  float parameter0, float parameter1)
{
	uint8_t *table = table_mac ? Mac2HostAddr(table_mac) : NULL;
	if (!table)
		return;

	float normalization = 1.0f;
	if (kind != kGlideFogLinear) {
		const float last_w = GlideFogTableIndexToWValue(63);
		const float x = parameter0 * last_w;
		normalization = kind == kGlideFogExp
			? 1.0f - std::exp(-x)
			: 1.0f - std::exp(-(x * x));
		if (!std::isfinite(normalization) || normalization <= 0.0f)
			normalization = 1.0f;
	}

	for (int i = 0; i < 64; i++) {
		const float w = GlideFogTableIndexToWValue(i);
		float fog;
		if (kind == kGlideFogLinear) {
			const float range = parameter1 - parameter0;
			fog = std::fabs(range) > 1.0e-20f
				? (w - parameter0) / range
				: (w >= parameter1 ? 1.0f : 0.0f);
		} else {
			const float x = parameter0 * w;
			fog = (kind == kGlideFogExp
				? 1.0f - std::exp(-x)
				: 1.0f - std::exp(-(x * x))) / normalization;
		}
		if (!std::isfinite(fog) || fog < 0.0f) fog = 0.0f;
		if (fog > 1.0f) fog = 1.0f;
		table[i] = (uint8_t)(fog * 255.0f + 0.5f);
	}
}

static uint8_t GlideReadGammaEntry(uint32_t table_mac, uint32_t index)
{
	uint32_t value = ReadMacInt32(table_mac + index * 4);
	if (value > 255)
		value = 255;
	return (uint8_t)value;
}

static void GlideApplyGuestGammaTable(uint32_t count, uint32_t red_mac,
									 uint32_t green_mac, uint32_t blue_mac)
{
	if (count == 0 || !red_mac || !green_mac || !blue_mac)
		return;
	if (count > 256)
		count = 256;

	const uint32_t tables[3] = { red_mac, green_mac, blue_mac };
	uint8_t lut[768];
	for (int component = 0; component < 3; component++) {
		for (uint32_t i = 0; i < 256; i++) {
			if (count == 1) {
				lut[component * 256 + i] = GlideReadGammaEntry(tables[component], 0);
				continue;
			}
			const uint32_t scaled = i * (count - 1);
			const uint32_t index = scaled / 255;
			const uint32_t fraction = scaled % 255;
			const uint32_t next = index + 1 < count ? index + 1 : index;
			const uint32_t a = GlideReadGammaEntry(tables[component], index);
			const uint32_t b = GlideReadGammaEntry(tables[component], next);
			lut[component * 256 + i] =
				(uint8_t)((a * (255 - fraction) + b * fraction + 127) / 255);
		}
	}
	GlideStateApplyGammaTable(lut);
}

static uint32_t GlideQueryResolutions(uint32_t template_mac, uint32_t output_mac)
{
	/* GrResolution is four consecutive FxI32 values.  This software board
	 * exposes the canonical resolutions accepted by GlideStateResolveResolution,
	 * a 60 Hz refresh, double buffering, and either zero or one aux buffer. */
	static const int resolutions[] = {
		GR_RESOLUTION_320x200, GR_RESOLUTION_320x240,
		GR_RESOLUTION_400x256, GR_RESOLUTION_512x384,
		GR_RESOLUTION_640x200, GR_RESOLUTION_640x350,
		GR_RESOLUTION_640x400, GR_RESOLUTION_640x480,
		GR_RESOLUTION_800x600, GR_RESOLUTION_960x720,
		GR_RESOLUTION_856x480, GR_RESOLUTION_512x256,
		GR_RESOLUTION_1024x768, GR_RESOLUTION_1280x1024,
		GR_RESOLUTION_1600x1200, GR_RESOLUTION_400x300
	};
	int want_resolution = -1;
	int want_refresh = -1;
	int want_color_buffers = -1;
	int want_aux_buffers = -1;
	if (template_mac) {
		want_resolution = (int)ReadMacInt32(template_mac + 0);
		want_refresh = (int)ReadMacInt32(template_mac + 4);
		want_color_buffers = (int)ReadMacInt32(template_mac + 8);
		want_aux_buffers = (int)ReadMacInt32(template_mac + 12);
	}

	uint32_t count = 0;
	for (size_t i = 0; i < sizeof(resolutions) / sizeof(resolutions[0]); i++) {
		for (int aux = 0; aux <= 1; aux++) {
			if (want_resolution != -1 && want_resolution != resolutions[i]) continue;
			if (want_refresh != -1 && want_refresh != GR_REFRESH_60Hz) continue;
			if (want_color_buffers != -1 && want_color_buffers != 2) continue;
			if (want_aux_buffers != -1 && want_aux_buffers != aux) continue;
			if (output_mac) {
				const uint32_t record = output_mac + count * 16;
				WriteMacInt32(record + 0, resolutions[i]);
				WriteMacInt32(record + 4, GR_REFRESH_60Hz);
				WriteMacInt32(record + 8, 2);
				WriteMacInt32(record + 12, aux);
			}
			count++;
		}
	}
	return count * 16;
}

/* Ring of recent Glide calls for hang dumps (host-side, no guest pointers kept live). */
struct GlideCallRec {
	uint64_t n;
	uint32_t subop;
	uint32_t r3, r4, r5, r6, r7, r8, r9, r10, stack9;
};
static const int kGlideHist = 48;
static GlideCallRec s_glide_hist[kGlideHist];
static int s_glide_hist_i = 0;
static uint64_t s_glide_hist_total = 0;
static uint32_t s_last_sub = 0;
static uint64_t s_call_n_global = 0;


void GlideHangDumpState(void)
{
	GlideLog("======== GLIDE HANG STATE ========");
	GlideLog("glide: call_n=%llu last_sub=%u hist_total=%llu",
			  (unsigned long long)s_call_n_global, s_last_sub,
			  (unsigned long long)s_glide_hist_total);
	GlideLog("glide: inited=%d win_open=%d size=%dx%d originUL=%d chroma_mode=%d chroma_val=%08x",
			  GlideStateIsInited() ? 1 : 0,
			  GlideStateWindowOpen() ? 1 : 0,
			  GlideStateWidth(), GlideStateHeight(),
			  GlideStateOriginUpperLeft(),
			  GlideStateChromaMode(),
			  (unsigned)GlideStateChromaValue());
	GlideLog("glide: swap=sync lfb_locked=%d lfb_ptr=%08x stride=%u type=%d buf=%d mode=%d",
			  GlideStateLfbIsLocked() ? 1 : 0,
			  (unsigned)GlideStateLfbGuestPtr(),
			  (unsigned)GlideStateLfbStride(),
			  GlideStateLfbType(), GlideStateLfbBuffer(), GlideStateLfbWriteMode());
	GlideLog("glide: scratch=%08x (subop slot)", (unsigned)glide_scratch_addr);
	if (glide_scratch_addr)
		GlideLog("glide: scratch_sub_now=%u", (unsigned)ReadMacInt32(glide_scratch_addr));
	/* Dump hist oldest..newest */
	const int n = (s_glide_hist_total < (uint64_t)kGlideHist)
				  ? (int)s_glide_hist_total : kGlideHist;
	GlideLog("glide: last %d calls (oldest first):", n);
	for (int k = 0; k < n; k++) {
		const int idx = (s_glide_hist_i - n + k + kGlideHist * 2) % kGlideHist;
		const GlideCallRec &c = s_glide_hist[idx];
		GlideLog("  hist[%d] #%llu subop=%u r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x r8=%08x r9=%08x r10=%08x sp9=%08x",
				  k, (unsigned long long)c.n, c.subop,
				  c.r3, c.r4, c.r5, c.r6, c.r7, c.r8, c.r9, c.r10, c.stack9);
	}
	GlideLog("======== GLIDE HANG STATE END ========");
}

static bool  s_glide_float_result_pending = false;
static float s_glide_float_result = 0.f;
static uint32_t GlideReturnFloat(float v)
{
	s_glide_float_result = v;
	s_glide_float_result_pending = true;
	return 0;
}

bool GlideDispatchTakeFloatResult(float *out)
{
	if (!s_glide_float_result_pending)
		return false;
	s_glide_float_result_pending = false;
	if (out) *out = s_glide_float_result;
	return true;
}

uint32_t GlideDispatch(uint32_t r3, uint32_t r4, uint32_t r5,
					   uint32_t r6, uint32_t r7, uint32_t r8,
					   uint32_t r9, uint32_t r10, uint32_t sp,
					   double f1, double f2, double f3, double f4)
{
	/* Stale flag from a previous call must never leak into this one. */
	s_glide_float_result_pending = false;
	if (!glide_scratch_addr)
		return 0;
	const uint32_t subop = ReadMacInt32(glide_scratch_addr);
	/* 9th stack arg (PPC Mac): home area + 8 register slots = SP+56. */
	const uint32_t stack9 = sp ? ReadMacInt32(sp + 56) : 0;
	static uint64_t s_call_n = 0;
	static uint32_t s_post_open_n = 0;
	static uint32_t s_hot_n = 0; /* VRetrace / VideoLine / IsBusy spam */
	++s_call_n;
	s_call_n_global = s_call_n;
	s_last_sub = subop;
	/* Record history (including hot paths - hang diagnosis needs them). */
	{
		GlideCallRec &c = s_glide_hist[s_glide_hist_i];
		c.n = s_call_n; c.subop = subop;
		c.r3 = r3; c.r4 = r4; c.r5 = r5; c.r6 = r6;
		c.r7 = r7; c.r8 = r8; c.r9 = r9; c.r10 = r10; c.stack9 = stack9;
		s_glide_hist_i = (s_glide_hist_i + 1) % kGlideHist;
		s_glide_hist_total++;
	}
	if (subop == kGlide_grSstWinOpen)
		s_post_open_n = 0;
	else if (GlideStateWindowOpen())
		++s_post_open_n;

	const bool hot =
		subop == kGlide_grSstVRetraceOn || subop == kGlide_grSstVideoLine ||
		subop == kGlide_grSstIsBusy || subop == kGlide_grSstIdle ||
		subop == kGlide_grSstStatus || subop == kGlide_grBufferNumPending;
	const bool always_log =
		!hot &&
		(s_post_open_n <= 400 || s_call_n <= 120 ||
		 subop == kGlide_grBufferClear || subop == kGlide_grBufferSwap ||
		 subop == kGlide_grLfbLock || subop == kGlide_grLfbUnlock ||
		 subop == kGlide_grTexDownloadMipMap ||
		 subop == kGlide_grTexDownloadMipMapLevel ||
		 subop == kGlide_grTexSource || subop == kGlide_grTexDownloadTable ||
		 subop == kGlide_grDrawTriangle || subop == kGlide_grDrawVertexArray ||
		 subop == kGlide_grDrawVertexArrayContiguous ||
		 subop == kGlide_grSstWinOpen || subop == kGlide_grSstWinClose ||
		 (subop >= 200 && subop < 230));

	const bool log_this = true;/*hot ? (++s_hot_n <= 16 || (s_hot_n % 10000u) == 0)
							  : always_log;*/
	if (hot && log_this)
		GlideLog("GlideENTER #%llu subop=%u HOT#%u r3=%08x (win=%d)",
				  (unsigned long long)s_call_n, subop, s_hot_n, r3,
				  GlideStateWindowOpen() ? 1 : 0);
	else if (!hot && log_this)
		GlideLog("GlideENTER #%llu subop=%u r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x r8=%08x r9=%08x r10=%08x",
				  (unsigned long long)s_call_n, subop, r3, r4, r5, r6, r7, r8, r9, r10);

	/* RAII exit log - if the log freezes mid-call, ENTER was last without EXIT. */
	struct GlideExitLog {
		uint64_t n;
		uint32_t subop;
		bool on;
		~GlideExitLog()
		{
			if (on)
				GlideLog("GlideEXIT  #%llu subop=%u",
						  (unsigned long long)n, subop);
		}
	} exit_log{s_call_n, subop, log_this && !hot};

	switch (subop) {
	case kGlide_grGlideInit:
		/* A second init closes the old producer before resetting Glide state. */
		if (GlideStateWindowOpen()) {
			GlideMetalWinClose();
			GlideStateSetWindowOpen(false);
		}
		GlideStateEndPresentation();
		GlideStateResetDefaults();
		/* Start from the fixed Glide 2 GrVertex layout. Glide 2 has no
		 * grVertexLayout, so a Glide 2 client would otherwise leave the whole
		 * table disabled and draw with a wrong (default 32-byte) stride.
		 * Glide 3 clients call grVertexLayout and overwrite these. */
		GlideStateSetGlide2VertexLayout();
		GlideStateSetInited(true);
		GlideMetalInit();
		GlideLog("grGlideInit");
		return 0;

	case kGlide_grGlideShutdown:
		if (GlideStateWindowOpen()) {
			GlideMetalWinClose();
			GlideStateSetWindowOpen(false);
		}
		GlideStateEndPresentation();
		GlideStateLfbRelease();
		GlideMetalShutdown();
		GlideStateSetInited(false);
		GlideLog("grGlideShutdown");
		return 0;

	case kGlide_grGlideGetVersion: {
		/* void grGlideGetVersion(char version[80]); */
		if (r3) {
			const char *v = "3.10";
			uint8 *p = Mac2HostAddr(r3);
			if (p) {
				size_t n = strlen(v);
				if (n > 79) n = 79;
				memcpy(p, v, n);
				p[n] = 0;
			}
		}
		return 0;
	}

	case kGlide_grSstQueryBoards: {
		/* FxBool grSstQueryBoards(GrHwConfiguration *hw);
		 * write_query_hardware fills the whole struct including num_sst. */
		if (r3) write_query_hardware(r3);
		return FXTRUE;
	}

	case kGlide_grSstQueryHardware: {
		if (r3) write_query_hardware(r3);
		return FXTRUE;
	}
	case kGlide_grQueryResolutions:
		return GlideQueryResolutions(r3, r4);

	case kGlide_grSstSelect: {
		/* void grSstSelect(int whichsst); */
		const int whichsst = (int)r3;
		return 0;
	}
	case kGlide_grSelectContext:
		/* This backend exposes one stable context handle, returned as 1 by the
		 * Glide 3 WinOpen path.  Its state is never stolen behind the guest. */
		return GlideStateWindowOpen() && r3 != 0 ? FXTRUE : FXFALSE;
	case kGlide_grSstVidMode:
		/* The Mac driver used this private export for the physical video-mode
		 * handoff.  WinOpen already performs that handoff through the DMC. */
		return FXTRUE;
	case kGlide_grSstWinOpen: {
		/* FxBool grSstWinOpen(FxU32 hWnd, GrScreenResolution_t res,
		 *   GrScreenRefresh_t ref, GrColorFormat_t cformat,
		 *   GrOriginLocation_t org, int nColBuffers, int nAuxBuffers) */
		const int res = (int)r4;
		const int cfmt = (int)r6;
		const int org = (int)r7;
		const int nCol = (int)r8;
		/* This seven-argument call fits through r9 on the PPC ABI. */
		const int nAux = (int)r9;
		int w = 640, h = 480;
		GlideStateResolveResolution(res, &w, &h);
		const int origin_ul = (org == GR_ORIGIN_UPPER_LEFT) ? 1 : 0;
		/* Drop any stale lock from a prior mode (movies -> menu). */
		if (GlideStateLfbIsLocked())
			(void)GlideStateLfbUnlock(0);
		/* Close the old private target before the shared display transaction;
		 * the presentation state retains the original outer mode across a
		 * direct WinOpen-to-WinOpen resolution change. */
		if (GlideStateWindowOpen()) {
			GlideMetalWinClose();
			GlideStateSetWindowOpen(false);
		}
		if (!GlideStateBeginPresentation(w, h)) {
			GlideStateEndPresentation();
			GlideLog("grSstWinOpen FAILED canonical 16-bit mode %dx%d", w, h);
			return FXFALSE;
		}
		if (GlideMetalWinOpen(w, h, origin_ul) != 0) {
			GlideStateEndPresentation();
			GlideLog("grSstWinOpen FAILED %dx%d", w, h);
			return FXFALSE;
		}
		GlideStateSetWin(w, h, origin_ul, cfmt, nCol ? nCol : 2, nAux);
		GlideLog("grSstWinOpen %dx%d orgUL=%d cfmt=%d nCol=%d",
				  w, h, origin_ul, cfmt, nCol);
		return FXTRUE;
	}

	case kGlide_grSstWinClose: {
		/* D2 spams WinClose on teardown - ignore when already closed. */
		static uint32_t s_close_n = 0;
		if (!GlideStateWindowOpen())
			return 0;
		++s_close_n;
		if (GlideStateLfbIsLocked())
			(void)GlideStateLfbUnlock(0);
		GlideMetalWinClose();
		GlideStateSetWindowOpen(false);
		GlideStateEndPresentation();
		/* Keep LFB alloc across close/reopen (movies -> menu); free on
		 * shutdown. EndPresentation restored the outer guest mode above. */
		if (s_close_n <= 4 || (s_close_n & (s_close_n - 1)) == 0)
			GlideLog("grSstWinClose (#%u)", (unsigned)s_close_n);
		return 0;
	}

	case kGlide_grSstControl:
		/* Glide 2's display-mux operation maps directly onto compositor
		 * ownership.  ACTIVATE reveals the last complete Glide front buffer;
		 * DEACTIVATE returns to the owner that was visible before WinOpen.
		 * RESIZE/MOVE require no work for the texture-backed overlay. */
		switch ((int)r3) {
		case 0: /* GR_CONTROL_ACTIVATE */
			(void)dmc_set_active_owner(kDMCOwnerGlide);
			return FXTRUE;
		case 1: /* GR_CONTROL_DEACTIVATE */
			(void)dmc_set_active_owner(GlideStatePresentationOwnerBefore());
			return FXTRUE;
		case 2: /* GR_CONTROL_RESIZE */
		case 3: /* GR_CONTROL_MOVE */
			return FXTRUE;
		default:
			return FXFALSE;
		}
	case kGlide_grSstIdle:
		/* The API promises that all submitted rendering has completed when this
		 * returns.  A host glFinish is the direct software-renderer equivalent. */
		GlideMetalFinish();
		return 0;
	case kGlide_grSstIsBusy:
		/* Never sticky-busy (that freezes while(grSstIsBusy()). */
		return FXFALSE;
	case kGlide_grSstOrigin:
		return 0;
	case kGlide_grSstScreenWidth:
		return (uint32_t)GlideStateWidth();
	case kGlide_grSstScreenHeight:
		return (uint32_t)GlideStateHeight();
	case kGlide_grSstStatus:
		/* SST status: 0 = idle / not busy / FIFO empty. */
		return 0;
	case kGlide_grSstVRetraceOn: {
		/*
		 * D2 PEF may not even export this (FindLibSymbol=0) - still implement
		 * for GetProcAddress / future PEFs. Toggle every call AND time-based
		 * so while(grSstVRetraceOn()) / while(!...) cannot stick forever.
		 */
		static uint32_t s_vr_calls = 0;
		static uint64_t s_vr_epoch_ms = 0;
		++s_vr_calls;
		const uint64_t now_ms = GetTicks_usec() / 1000u;
		if (s_vr_epoch_ms == 0)
			s_vr_epoch_ms = now_ms;
		const uint64_t phase = (now_ms - s_vr_epoch_ms) % 16ull;
		/* Report the host-time retrace phase. The prior call-count toggle and
		 * deferred-Present kick fabricated hardware progress and could disturb
		 * a frame already being rendered. */
		uint32_t in_retrace = (phase < 3ull) ? FXTRUE : FXFALSE;
		if (s_vr_calls <= 12 || (s_vr_calls % 50000u) == 0) {
			GlideLog("grSstVRetraceOn #%u -> %u phase=%llu",
					  s_vr_calls, in_retrace, (unsigned long long)phase);
		}
		return in_retrace;
	}
	case kGlide_grSstVideoLine: {
		static uint32_t s_vl_calls = 0;
		static uint64_t s_vl_epoch_ms = 0;
		++s_vl_calls;
		const uint64_t now_ms = GetTicks_usec() / 1000u;
		if (s_vl_epoch_ms == 0)
			s_vl_epoch_ms = now_ms;
		const int h = GlideStateHeight() > 0 ? GlideStateHeight() : 480;
		const uint64_t phase = (now_ms - s_vl_epoch_ms) % 16ull;
		/* Also advance by call count so zero-time polls move. */
		const uint32_t line = (uint32_t)(((phase * (uint64_t)h) / 16ull + s_vl_calls) % (uint32_t)h);
		return line;
	}

	case kGlide_grBufferClear: {
		/* void grBufferClear(GrColor_t color, GrAlpha_t alpha, FxU32 depth) */
		GlideLog("grBufferClear begin color=%08x", r3);
		/* Clear back buffer only - real Glide does not display on clear. */
		GlideMetalBufferClear(r3, r4, r5);
		(void)dmc_set_active_owner(kDMCOwnerGlide);
		GlideLog("grBufferClear done");
		return 0;
	}
	case kGlide_grBufferSwap: {
		/* void grBufferSwap(int swap_interval) */
		const int interval = (int)r3;
		GlideLog("grBufferSwap begin interval=%d", interval);
		/* Make the back buffer visible (SubmitFrame + Present) */
		GlideMetalBufferSwap(interval);
		(void)dmc_set_active_owner(kDMCOwnerGlide);
		GlideLog("grBufferSwap done (pending=0, presented)");
		return 0;
	}
	case kGlide_grBufferNumPending: {
		/* Read-only count; the software swap above completes synchronously. */
		return 0;
	}
	case kGlide_grRenderBuffer:
		GlideStateSetRenderBuffer((int)r3);
		return 0;

	case kGlide_grDrawPoint: {
		const void *a = r3 ? Mac2HostAddr(r3) : NULL;
		if (a) { GlideMetalDrawPoint(a); GlideMetalMarkContent(); }
		return 0;
	}
	case kGlide_grDrawLine: {
		const void *a = r3 ? Mac2HostAddr(r3) : NULL;
		const void *b = r4 ? Mac2HostAddr(r4) : NULL;
		if (a && b) { GlideMetalDrawLine(a, b); GlideMetalMarkContent(); }
		return 0;
	}
	case kGlide_grDrawTriangle: {
		const void *a = r3 ? Mac2HostAddr(r3) : NULL;
		const void *b = r4 ? Mac2HostAddr(r4) : NULL;
		const void *c = r5 ? Mac2HostAddr(r5) : NULL;
		if (a && b && c) {
			GlideMetalDrawTriangle(a, b, c);
			GlideMetalMarkContent();
		}
		return 0;
	}
	case kGlide_grAADrawTriangle: {
		const void *a = r3 ? Mac2HostAddr(r3) : NULL;
		const void *b = r4 ? Mac2HostAddr(r4) : NULL;
		const void *c = r5 ? Mac2HostAddr(r5) : NULL;
		if (a && b && c) {
			GlideMetalDrawTriangle(a, b, c);
			GlideMetalMarkContent();
		}
		return 0;
	}
	case kGlide_grDrawPlanarPolygon:
	case kGlide_grDrawPolygon: {
		/* Glide 2: ilist contains big-endian integer indices into one contiguous
		 * GrVertex array.  It is not an array of guest vertex pointers. */
		const int n = (int)r3;
		const void *indices = r4 ? Mac2HostAddr(r4) : NULL;
		const void *vertices = r5 ? Mac2HostAddr(r5) : NULL;
		if (indices && vertices && n >= 3 && n <= 65536) {
			GlideMetalDrawPolygonIndexed(n, indices, vertices, 0);
			GlideMetalMarkContent();
		}
		return 0;
	}
	case kGlide_grDrawPlanarPolygonVertexList:
	case kGlide_grDrawPolygonVertexList: {
		/* void grDrawPolygonVertexList(int nverts, const GrVertex vlist[]); */
		const int n = (int)r3;
		const void *v = r4 ? Mac2HostAddr(r4) : NULL;
		if (v && n >= 3)
			GlideMetalDrawPolygonContiguous(n, v, 0);
		GlideMetalMarkContent();
		return 0;
	}

	case kGlide_grAlphaBlendFunction:
		GlideStateSetAlphaBlend((int)r3, (int)r4, (int)r5, (int)r6);
		return 0;
	case kGlide_grAlphaCombine:
		GlideStateSetAlphaCombine((int)r3, (int)r4, (int)r5,
								 (int)r6, (int)r7);
		return 0;
	case kGlide_grAlphaControlsITRGBLighting:
		GlideStateSetAlphaControlsLighting(r3 ? 1 : 0);
		return 0;
	case kGlide_grAlphaTestFunction:
		GlideStateSetAlphaTest((int)r3, GlideStateAlphaTestRef());
		GlideMetalSetAlphaTest(1, (int)r3, GlideStateAlphaTestRef());
		return 0;
	case kGlide_grAlphaTestReferenceValue: {
		/* GrAlpha_t often 0..255 */
		const float ref = (r3 > 255) ? 1.f : (r3 / 255.f);
		GlideStateSetAlphaTest(GlideStateAlphaTestFunc(), ref);
		GlideMetalSetAlphaTest(GlideStateAlphaTestEnabled(),
							GlideStateAlphaTestFunc(), ref);
		return 0;
	}
	case kGlide_grChromakeyMode:
		GlideStateSetChromakey((int)r3, GlideStateChromaValue());
		GlideMetalSetChromakey();
		return 0;
	case kGlide_grChromakeyValue:
		GlideStateSetChromakey(GlideStateChromaMode(), r3);
		GlideMetalSetChromakey();
		return 0;
	case kGlide_grChromaRange:
		/* void grChromaRangeExt(color0, color1, matchMode) */
		GlideStateSetChromaRange(r3, r4, (int)r5);
		GlideMetalSetChromakey();
		return 0;
	case kGlide_grChromaRangeMode:
		GlideStateSetChromaRangeMode((int)r3);
		GlideMetalSetChromakey();
		return 0;
	case kGlide_grTexChromaRange:
		/* void grTexChromaRangeExt(tmu, color0, color1, matchMode) */
		GlideStateSetTexChromaRange(r4, r5, (int)r6);
		GlideMetalSetChromakey();
		return 0;
	case kGlide_grTexChromaMode:
		GlideStateSetTexChromaMode((int)r4);
		GlideMetalSetChromakey();
		return 0;
	case kGlide_grClipWindow:
		GlideStateSetClip((int)r3, (int)r4, (int)r5, (int)r6);
		GlideMetalSetClipWindow((int)r3, (int)r4, (int)r5, (int)r6);
		return 0;
	case kGlide_grColorCombine:
		GlideStateSetColorCombine((int)r3, (int)r4, (int)r5,
								 (int)r6, (int)r7);
		return 0;
	case kGlide_guColorCombineFunction: {
		/* void guColorCombineFunction(GrColorCombineFnc_t fnc)
		 *
		 * Glide 2 shorthand that the stock library expands into one
		 * grColorCombine(func, factor, local, other, invert).
		 * Unreal Tournament configures
		 * all of its colour combining through this entry point, so leaving it
		 * unhooked left the combine state at whatever the last explicit
		 * grColorCombine set.
		 *
		 * The ITRGB_DELTA0 variants additionally toggle an internal delta0 mode
		 * that only affects Voodoo iterator setup; the resulting combine is the
		 * same as the non-DELTA0 form, which is all this backend models.
		 */
		int func, factor, local, other, invert = 0;
		switch ((int)r3) {
		case 0x0: /* ZERO */
			func = 0x0; factor = 0x0; local = 0x1; other = 0x2; break;
		case 0x1: /* CCRGB */
			func = 0x1; factor = 0x0; local = 0x1; other = 0x2; break;
		case 0x2: /* ITRGB */
		case 0x3: /* ITRGB_DELTA0 */
			func = 0x1; factor = 0x0; local = 0x0; other = 0x2; break;
		case 0x4: /* DECAL_TEXTURE */
			func = 0x3; factor = 0x8; local = 0x1; other = 0x1; break;
		case 0x5: /* TEXTURE_TIMES_CCRGB */
			func = 0x3; factor = 0x1; local = 0x1; other = 0x1; break;
		case 0x6: /* TEXTURE_TIMES_ITRGB */
		case 0x7: /* TEXTURE_TIMES_ITRGB_DELTA0 */
			func = 0x3; factor = 0x1; local = 0x0; other = 0x1; break;
		case 0x8: /* TEXTURE_TIMES_ITRGB_ADD_ALPHA */
			func = 0x5; factor = 0x1; local = 0x0; other = 0x1; break;
		case 0x9: /* TEXTURE_TIMES_ALPHA */
			func = 0x3; factor = 0x3; local = 0x1; other = 0x1; break;
		case 0xa: /* TEXTURE_TIMES_ALPHA_ADD_ITRGB */
			func = 0x4; factor = 0x3; local = 0x0; other = 0x1; break;
		case 0xb: /* TEXTURE_ADD_ITRGB */
			func = 0x4; factor = 0x8; local = 0x0; other = 0x1; break;
		case 0xc: /* TEXTURE_SUB_ITRGB */
			func = 0x6; factor = 0x8; local = 0x0; other = 0x1; break;
		case 0xd: /* CCRGB_BLEND_ITRGB_ON_TEXALPHA */
			func = 0x7; factor = 0x4; local = 0x1; other = 0x0; break;
		case 0xe: /* DIFF_SPEC_A */
			func = 0x4; factor = 0x3; local = 0x0; other = 0x1; break;
		case 0xf: /* DIFF_SPEC_B */
			func = 0x5; factor = 0x1; local = 0x0; other = 0x1; break;
		case 0x10: /* ONE */
			func = 0x0; factor = 0x0; local = 0x1; other = 0x2; invert = 1; break;
		default:
			GlideLog("guColorCombineFunction: unsupported function %u", r3);
			return 0;
		}
		GlideStateSetColorCombine(func, factor, local, other, invert);
		return 0;
	}
	case kGlide_guAlphaSource: {
		/* void guAlphaSource(GrAlphaSource_t mode)
		 *
		 * Glide 2 shorthand expanding into one grAlphaCombine.
		 * Unreal Tournament selects its
		 * alpha source exclusively through this (67980 calls in one capture),
		 * so while it was unhandled the alpha combine never changed. */
		int func, factor, local, other;
		switch ((int)r3) {
		case 0x0: /* CC_ALPHA: LOCAL / NONE / CONSTANT / NONE */
			func = 0x1; factor = 0x0; local = 0x1; other = 0x2; break;
		case 0x1: /* ITERATED_ALPHA: LOCAL / NONE / ITERATED / NONE */
			func = 0x1; factor = 0x0; local = 0x0; other = 0x2; break;
		case 0x2: /* TEXTURE_ALPHA: SCALE_OTHER / ONE / NONE / TEXTURE */
			func = 0x3; factor = 0x8; local = 0x1; other = 0x1; break;
		case 0x3: /* TEXTURE_ALPHA_TIMES_ITERATED_ALPHA:
				   * SCALE_OTHER / LOCAL / ITERATED / TEXTURE */
			func = 0x3; factor = 0x1; local = 0x0; other = 0x1; break;
		default:
			GlideLog("guAlphaSource: unknown alpha source mode %u", r3);
			return 0;
		}
		GlideStateSetAlphaCombine(func, factor, local, other, 0);
		return 0;
	}
	case kGlide_guFogGenerateLinear:
		GlideGenerateFogTable(r3, kGlideFogLinear, (float)f1, (float)f2);
		return 0;
	case kGlide_guFogGenerateExp:
		GlideGenerateFogTable(r3, kGlideFogExp, (float)f1, 0.0f);
		return 0;
	case kGlide_guFogGenerateExp2:
		GlideGenerateFogTable(r3, kGlideFogExp2, (float)f1, 0.0f);
		return 0;
	case kGlide_guFogTableIndexToW: {
		/* float guFogTableIndexToW(int i)
		 *   return pow(2.0, 3.0 + (i>>2)) / (8 - (i&3));
		 * Returns in FPR1 via the float side channel. */
		const int i = (int)r3;
		const float w = (float)(pow(2.0, 3.0 + (double)(i >> 2)) /
								(double)(8 - (i & 3)));
		return GlideReturnFloat(w);
	}
	case kGlide_grErrorSetCallback:
		/* void grErrorSetCallback(GrErrorCallback_t fnc)
		 * Records a guest callback invoked on fatal Glide errors. This backend
		 * never raises one, so accepting and ignoring it is correct - and it
		 * must not fall through to the "unhandled" default, which would log
		 * once per call for a routine startup call. */
		return 0;
	case kGlide_grColorMask:
		/* void grColorMask(FxBool rgb, FxBool a) */
		GlideStateSetColorMask(r3 ? 1 : 0, r3 ? 1 : 0, r3 ? 1 : 0, r4 ? 1 : 0);
		GlideMetalSetColorMask(r3 ? 1 : 0, r3 ? 1 : 0, r3 ? 1 : 0, r4 ? 1 : 0);
		return 0;
	case kGlide_grConstantColorValue:
		GlideStateSetConstantColor(r3);
		return 0;
	case kGlide_grConstantColorValue4: {
		/* void grConstantColorValue4(float a, float r, float g, float b) */
		GlideStateSetConstantColor4((float)f2, (float)f3,
								(float)f4, (float)f1);
		return 0;
	}
	case kGlide_grCullMode:
		GlideStateSetCull((int)r3);
		return 0;
	case kGlide_grDepthBiasLevel: {
		/* Glide 2 declares FxI16 and Glide 3 FxI32; neither ABI passes a
		 * floating-point value. Mac Glide 2 may zero-extend the 16-bit
		 * argument, so restore its sign when the upper half is clear. */
		int32_t level = (int32_t)r3;
		if ((r3 & 0xffff0000u) == 0 && (r3 & 0x00008000u) != 0)
			level = (int32_t)(int16_t)(r3 & 0xffffu);
		const float bias = (float)level;
		GlideStateSetDepthBias(bias);
		GlideMetalSetDepthBias(bias);
		return 0;
	}
	case kGlide_grDepthBufferFunction:
		GlideStateSetDepth(-1, (int)r3, -1);
		return 0;
	case kGlide_grDepthBufferMode:
		GlideStateSetDepth((int)r3, -1, -1);
		return 0;
	case kGlide_grDepthMask:
		GlideStateSetDepth(-1, -1, r3 ? 1 : 0);
		return 0;
	case kGlide_grDisableAllEffects:
		GlideStateDisableAllEffects();
		GlideMetalSetChromakey();
		return 0;
	case kGlide_grDitherMode:
		GlideStateSetDither((int)r3);
		return 0;
	case kGlide_grFogColorValue:
		GlideStateSetFog(GlideStateFogMode(), r3);
		GlideMetalSetFog(GlideStateFogMode(), r3);
		return 0;
	case kGlide_grFogMode:
		GlideStateSetFog((int)r3, GlideStateFogColor());
		GlideMetalSetFog((int)r3, GlideStateFogColor());
		return 0;
	case kGlide_grFogTable:
		/* Glide 2 fog tables contain 64 one-byte fog factors. */
		if (r3)
			GlideStateSetFogTable(Mac2HostAddr(r3));
		return 0;
	case kGlide_grGammaCorrectionValue: {
		/* Glide 2 applies one gamma exponent to all three components. */
		GlideStateApplyGammaRGB((float)f1, (float)f1, (float)f1);
		return 0;
	}
	case kGlide_grLoadGammaTable:
		GlideApplyGuestGammaTable(r3, r4, r5, r6);
		return 0;
	case kGlide_grHints:
		return 0;
	case kGlide_grSplash:
		GlideMetalSplash();
		return 0;

	case kGlide_grTexCalcMemRequired: {
		/* FxU32 grTexCalcMemRequired(GrLOD_t small, GrLOD_t large,
		 *   GrAspectRatio_t aspect, GrTextureFormat_t format) */
		return GlideTexCalcMemRequired((int)r3, (int)r4, (int)r5, (int)r6);
	}
	case kGlide_grTexTextureMemRequired: {
		/* FxU32 grTexTextureMemRequired(FxU32 evenOdd, GrTexInfo *info) */
		const uint32_t info = r4;
		if (!info)
			return 256 * 256 * 2;
		const int small_lod = (int)ReadMacInt32(info + 0);
		const int large_lod = (int)ReadMacInt32(info + 4);
		const int aspect = (int)ReadMacInt32(info + 8);
		const int format = (int)ReadMacInt32(info + 12);
		return GlideTexCalcMemRequired(small_lod, large_lod, aspect, format);
	}
	case kGlide_grTexMinAddress:
		return GlideStateTexMinAddress((int)r3);
	case kGlide_grTexMaxAddress:
		return GlideStateTexMaxAddress((int)r3);
	case kGlide_guTexMemQueryAvail:
		return GlideStateTexMaxAddress((int)r3) -
			   GlideStateTexMinAddress((int)r3) + 1;
	case kGlide_grTexSource: {
		/* void grTexSource(GrChipID_t tmu, FxU32 startAddress, FxU32 evenOdd,
		 *                  GrTexInfo *info) */
		const uint32_t start = r4;
		const int evenOdd = (int)r5;
		const uint32_t info = r6;
		int small_lod = 0, large_lod = 8, aspect = 0, format = 0x0a;
		if (info) {
			small_lod = (int)ReadMacInt32(info + 0);
			large_lod = (int)ReadMacInt32(info + 4);
			aspect = (int)ReadMacInt32(info + 8);
			format = (int)ReadMacInt32(info + 12);
		}
		GlideStateSetTexSourceEx(start, evenOdd, small_lod, large_lod, aspect, format);
		GlideMetalTexSource(start, evenOdd, small_lod, large_lod, aspect, format);
		return 0;
	}
	case kGlide_grTexClampMode:
		GlideStateSetTexClamp((int)r4, (int)r5); /* tmu=r3, s=r4, t=r5 */
		return 0;
	case kGlide_grTexFilterMode:
		GlideStateSetTexFilter((int)r4, (int)r5);
		return 0;
	case kGlide_grTexCombine:
		/* void grTexCombine(tmu, rgb_func, rgb_factor, alpha_func, alpha_factor,
		 *                   rgb_invert, alpha_invert) */
		GlideStateSetTexCombine((int)r4, (int)r5, (int)r6, (int)r7,
							  (int)r8, (int)r9);
		return 0;
	case kGlide_grTexCombineFunction:
	case kGlide_guTexCombineFunction: {
		/* void guTexCombineFunction(GrChipID_t tmu, GrTextureCombineFnc_t tc)
		 *
		 * Glide 2 shorthand expanding into one grTexCombine with the same
		 * function/factor applied to both the RGB and alpha halves.
		 *
		 * tmu arrives in r3 and tc in r4. Like the kGlide_grTexCombine case
		 * above, the tmu is dropped: GlideStateSetTexCombine models a single
		 * TMU and takes no chip id.
		 */
		int func, factor, invert = 0;
		switch ((int)r4) {
		case 0x0: /* ZERO */
			func = 0x0; factor = 0x0; break;
		case 0x1: /* DECAL */
			func = 0x1; factor = 0x0; break;
		case 0x2: /* OTHER (passthru) */
			func = 0x3; factor = 0x8; break;
		case 0x3: /* ADD */
			func = 0x4; factor = 0x8; break;
		case 0x4: /* MULTIPLY */
			func = 0x3; factor = 0x1; break;
		case 0x5: /* SUBTRACT */
			func = 0x6; factor = 0x8; break;
		case 0x6: /* DETAIL */
			func = 0x7; factor = 0xc; break;
		case 0x7: /* DETAIL_OTHER */
			func = 0x7; factor = 0x4; break;
		case 0x8: /* TRILINEAR_ODD */
			func = 0x7; factor = 0xd; break;
		case 0x9: /* TRILINEAR_EVEN */
			func = 0x7; factor = 0x5; break;
		case 0xa: /* ONE */
			func = 0x0; factor = 0x0; invert = 1; break;
		default:
			GlideLog("guTexCombineFunction: unsupported function %u", r4);
			return 0;
		}
		GlideStateSetTexCombine(func, factor, func, factor, invert, invert);
		return 0;
	}
	case kGlide_grTexDetailControl:
		GlideStateSetTexDetail((int)r4, (int)r5, (int)r6);
		return 0;
	case kGlide_grTexLodBiasValue: {
		GlideStateSetTexLodBias((float)f1);
		return 0;
	}
	case kGlide_grTexLodTable:
		return 0;
	case kGlide_grTexMipMapMode:
		GlideStateSetTexMipMapMode((int)r4);
		return 0;
	case kGlide_grTexDownloadMipMap: {
		/* void grTexDownloadMipMap(GrChipID_t tmu, FxU32 startAddress,
		 *   FxU32 evenOdd, GrTexInfo *info) */
		const uint32_t start = r4;
		const uint32_t info = r6;
		if (!info) return 0;
		const int small_lod = (int)ReadMacInt32(info + 0);
		const int large_lod = (int)ReadMacInt32(info + 4);
		const int aspect = (int)ReadMacInt32(info + 8);
		const int format = (int)ReadMacInt32(info + 12);
		if (small_lod < 0 || small_lod > 11 ||
			large_lod < 0 || large_lod > 11) {
			GlideLog("grTexDownloadMipMap invalid LOD range %d..%d",
					  large_lod, small_lod);
			return 0;
		}
		const uint32_t data_mac = ReadMacInt32(info + 16);
		const uint8_t *data = data_mac ? Mac2HostAddr(data_mac) : NULL;
		if (!data) return 0;
		/* Download each LOD from large->small; data packs levels sequentially. */
		GlideTexObserveLodRange(small_lod, large_lod);
		const uint8_t *p = data;
		const int step = small_lod >= large_lod ? 1 : -1;
		for (int lod = large_lod;; lod += step) {
			const uint32_t n = GlideTexLevelSizeBytes(lod, aspect, format);
			/* startAddress is the base of the complete mip chain for every
			 * level, not the address of this individual level. */
			GlideMetalTexDownloadLevel(start, lod, large_lod,
									  aspect, format, p, n);
			p += n;
			if (lod == small_lod) break;
		}
		return 0;
	}
	case kGlide_grTexDownloadMipMapLevel:
	case kGlide_grTexDownloadMipMapLevelPartial: {
		/* void grTexDownloadMipMapLevel(GrChipID_t tmu, FxU32 startAddress,
		 *   GrLOD_t thisLod, GrLOD_t largeLod, GrAspectRatio_t aspect,
		 *   GrTextureFormat_t format, FxU32 evenOdd, void *data)
		 * PPC: r3..r10 - evenOdd=r9, data=r10. Partial has more stack args;
		 * we treat as full level download. */
		const uint32_t start = r4;
		const int this_lod = (int)r5;
		const int large_lod = (int)r6;
		const int aspect = (int)r7;
		const int format = (int)r8;
		if (this_lod < 0 || this_lod > 11 ||
			large_lod < 0 || large_lod > 11) {
			GlideLog("grTexDownloadMipMapLevel invalid LOD %d large=%d",
					  this_lod, large_lod);
			return 0;
		}
		const uint32_t data_mac = r10 ? r10 : r9; /* tolerate swapped */
		const uint8_t *data = data_mac ? Mac2HostAddr(data_mac) : NULL;
		if (!data) {
			GlideLog("grTexDownloadMipMapLevel NO DATA start=%08x r9=%08x r10=%08x",
					  start, r9, r10);
			return 0;
		}
		GlideTexObserveLodRange(this_lod, large_lod);
		const uint32_t n = GlideTexLevelSizeBytes(this_lod, aspect, format);
		/* For partial, still use full level size from start of guest buffer. */
		GlideMetalTexDownloadLevel(start, this_lod, large_lod, aspect, format, data, n);
		return 0;
	}
	case kGlide_grTexDownloadTable:
	case kGlide_grTexDownloadTablePartial: {
		/* Glide2: grTexDownloadTable(type, data)
		 * Glide3: grTexDownloadTable(tmu, type, data)
		 * Log shows r3=2 (palette type) with data in r4 - Glide2 style. */
		int type = (int)r3;
		uint32_t data_mac = r4;
		if (r3 <= 1 && r4 <= 0x10) {
			/* Likely Glide3: tmu=r3 type=r4 data=r5 */
			type = (int)r4;
			data_mac = r5;
		}
		const void *data = data_mac ? Mac2HostAddr(data_mac) : NULL;
		GlideMetalTexDownloadTable(type, data);
		return 0;
	}
	case kGlide_grTexMultibase:
	case kGlide_grTexMultibaseAddress:
	case kGlide_grTexNCCTable:
		return 0;

	case kGlide_grGet:
		/* FxU32 grGet(FxU32 pname, FxU32 plength, FxI32 *params) */
		return handle_grGet(r3, r4, r5);
	case kGlide_grGetString:
		return handle_grGetString(r3);
	case kGlide_grGetProcAddress: {
		/* void *grGetProcAddress(char *procName); return TVECT or NULL */
		char name[128] = "";
		if (r3) {
			const uint8_t *p = Mac2HostAddr(r3);
			if (p) {
				size_t i = 0;
				while (i + 1 < sizeof(name) && p[i]) {
					name[i] = (char)p[i];
					i++;
				}
				name[i] = 0;
			}
		}
		struct Pair { const char *n; int subop; };
		static const Pair kExt[] = {
			{ "grChromaRangeExt", kGlide_grChromaRange },
			{ "grChromaRangeModeExt", kGlide_grChromaRangeMode },
			{ "grTexChromaRangeExt", kGlide_grTexChromaRange },
			{ "grTexChromaModeExt", kGlide_grTexChromaMode },
			/* D2 board-detect extension (must not return NULL). */
			{ "grSurfaceCreateContextExt", kGlide_grSurfaceCreateContextExt },
			{ "grSurfaceReleaseContextExt", kGlide_grSurfaceReleaseContextExt },
			{ "grSurfaceSetRenderingSurfaceExt", kGlide_grSurfaceSetRenderingSurfaceExt },
			{ "grSurfaceCalcTextureWHDExt", kGlide_grSurfaceCalcTextureWHDExt },
			{ "grSurfaceSetAuxSurfaceExt", kGlide_grSurfaceSetAuxSurfaceExt },
			/*
			 * The stock 3dfx RAVE rvTerminate calls this unconditionally
			 * for TMUs 0 and 1.  Other SURFACE lookups deliberately remain
			 * NULL until that extension is implemented in full.
			 */
			{ "grSurfaceSetTextureSurfaceExt", kGlide_grSurfaceSetTextureSurfaceExt },
			{ "grDeviceQueryExt", kGlide_grDeviceQueryExt },
			{ "grSurfaceCreateExt", kGlide_grSurfaceCreateExt },
			{ "grSurfaceReleaseExt", kGlide_grSurfaceReleaseExt },
			{ "grSurfaceGetDescExt", kGlide_grSurfaceGetDescExt },
		};
		for (size_t i = 0; i < sizeof(kExt) / sizeof(kExt[0]); i++) {
			if (std::strcmp(name, kExt[i].n) == 0) {
				/*
				 * Return the TVECT (CFM ProcPtr), not raw code. Mac callers
				 * do lwz r0,0(r3); mtctr; bctrl. Returning code would break
				 * that. Raw-code callers that bctr to the TVECT itself hit
				 * illegal opcodes - execute_illegal now recovers via LR.
				 */
				uint32_t tv = glide_method_tvects[kExt[i].subop];
				GlideLog("grGetProcAddress('%s') -> tvect=0x%08x code=0x%08x",
						  name, tv, tv ? ReadMacInt32(tv) : 0);
				return tv;
			}
		}
		GlideLog("grGetProcAddress('%s') -> NULL", name);
		return 0;
	}
	case kGlide_guGammaCorrectionRGB:
		GlideStateApplyGammaRGB((float)f1, (float)f2, (float)f3);
		return 0;
	case kGlide_grReset:
		return 0;
	case kGlide_grEnable:
	case kGlide_grDisable:
		/* Combinatorial enable bits - state recorded via specific setters. */
		return 0;
	case kGlide_grCoordinateSystem:
		GlideStateSetCoordSystem((int)r3);
		return 0;
	case kGlide_grViewport:
		/* void grViewport(FxI32 x, FxI32 y, FxI32 width, FxI32 height) */
		GlideStateSetViewport((int)r3, (int)r4, (int)r5, (int)r6);
		return 0;
	case kGlide_grDepthRange:
		/* Floats use PPC FPR1/FPR2, not r3/r4. */
		GlideStateSetDepthRange((float)f1, (float)f2);
		return 0;
	case kGlide_grVertexLayout:
		/* void grVertexLayout(FxU32 param, FxI32 offset, FxU32 mode) */
		GlideStateSetVertexLayout((int)r3, (int)r4, (int)r5);
		GlideLog("grVertexLayout param=0x%x off=%d mode=%u",
				  (unsigned)r3, (int)r4, r5);
		return 0;
	case kGlide_grDrawVertexArray: {
		/* void grDrawVertexArray(FxU32 mode, FxU32 count, void *pointers) */
		const uint32_t mode = r3;
		const uint32_t count = r4;
		const uint32_t ptrs_mac = r5;
		if (!count || !ptrs_mac || count > 65536u)
			return 0;
		static std::vector<const void *> host_ptrs;
		host_ptrs.resize(count);
		for (uint32_t i = 0; i < count; i++) {
			const uint32_t mac = ReadMacInt32(ptrs_mac + i * 4);
			host_ptrs[i] = mac ? Mac2HostAddr(mac) : NULL;
		}
		GlideMetalDrawVertexArray(mode, count, vec_data(host_ptrs));
		GlideMetalMarkContent();
		return 0;
	}
	case kGlide_grDrawVertexArrayContiguous: {
		/* void grDrawVertexArrayContiguous(FxU32 mode, FxU32 count,
		 *                                  void *vertices, FxU32 stride) */
		const uint32_t mode = r3;
		const uint32_t count = r4;
		const void *verts = r5 ? Mac2HostAddr(r5) : NULL;
		const uint32_t stride = r6;
		if (verts && count && count <= 65536u) {
			GlideMetalDrawVertexArrayContiguous(mode, count, verts, stride);
			GlideMetalMarkContent();
		}
		return 0;
	}
	case kGlide_grGlideGetState:
	case kGlide_grGlideSetState:
	case kGlide_grGlideGetVertexLayout:
	case kGlide_grGlideSetVertexLayout:
		/* Opaque state blobs - accept without faulting. */
		return 0;
	case kGlide_grFinish:
		GlideMetalFinish();
		/* GPU idle: no outstanding swaps. */
		return 0;
	case kGlide_grFlush:
		GlideMetalFlush();
		return 0;

	case kGlide_grLfbLock: {
		/* FxBool grLfbLock(GrLock_t type, GrBuffer_t buffer,
		 *   GrLfbWriteMode_t writeMode, GrOriginLocation_t origin,
		 *   FxBool pixelPipeline, GrLfbInfo_t *info);
		 * PPC: r3..r8. D2 log: type=1 (WRITE), buffer=0 (FRONT), writeMode=0 (565). */
		const int type = (int)r3;
		const int buffer = (int)r4;
		const int writeMode = (int)r5;
		const int origin = (int)r6;
		const uint32_t info = r8; /* 6th arg */
		(void)r7; /* pixelPipeline - ignore for software LFB */

		uint32_t lfbPtr = 0, stride = 0;
		int resolvedMode = writeMode;
		if (!info) {
			GlideLog("grLfbLock FAIL info=NULL type=%d buf=%d mode=%d",
					  type, buffer, writeMode);
			return FXFALSE;
		}
		if (!GlideStateLfbLock(type, buffer, writeMode, origin,
							   &lfbPtr, &stride, &resolvedMode)) {
			static uint32_t s_lfb_fail = 0;
			if (++s_lfb_fail <= 8)
				GlideLog("grLfbLock FAIL type=%d buf=%d mode=%d locked=%d win=%d",
						  type, buffer, writeMode,
						  GlideStateLfbIsLocked() ? 1 : 0,
						  GlideStateWindowOpen() ? 1 : 0);
			return FXFALSE;
		}

		/* GrLfbInfo_t { int size; void *lfbPtr; FxU32 stride; mode; origin } */
		WriteMacInt32(info + 4, lfbPtr);
		WriteMacInt32(info + 8, stride);
		WriteMacInt32(info + 12, (uint32_t)resolvedMode);
		WriteMacInt32(info + 16, (uint32_t)origin);
		/* Leave size (info+0) as caller set it. */

		(void)dmc_set_active_owner(kDMCOwnerGlide);

		static uint32_t s_lfb_ok = 0;
		if (++s_lfb_ok <= 8 || (s_lfb_ok & (s_lfb_ok - 1)) == 0)
			GlideLog("grLfbLock OK #%u type=%d buf=%d mode=%d ptr=%08x stride=%u",
					  (unsigned)s_lfb_ok, type, buffer, resolvedMode,
					  lfbPtr, stride);
		return FXTRUE;
	}
	case kGlide_grLfbUnlock: {
		/* FxBool grLfbUnlock(GrLock_t type, GrBuffer_t buffer) */
		const int type = (int)r3;
		const int buffer = (int)r4;
		const int was_write = (GlideStateLfbType() != 0) || (type != 0);
		if (!GlideStateLfbUnlock(buffer))
			return FXFALSE;

		/* On write unlock: convert 565/etc -> BGRA, upload overlay, present.
		 * Front-buffer locks present immediately; back-buffer also present
		 * so D2 soft-blit paths that skip BufferSwap still show pixels. */
		if (was_write) {
			int uw = 0, uh = 0, upitch = 0;
			const uint8_t *bgra = GlideStateLfbConvertToBGRA(&uw, &uh, &upitch);
			if (bgra)
				GlideMetalUploadLfbAndPresent(bgra, uw, uh, upitch, /*present=*/1);
			(void)dmc_set_active_owner(kDMCOwnerGlide);
		}
		return FXTRUE;
	}
	case kGlide_grLfbReadRegion: {
		/* FxBool grLfbReadRegion(GrBuffer_t src_buffer, FxU32 src_x, src_y,
		 *   FxU32 src_width, src_height, FxU32 dst_stride, void *dst_data)
		 * r3=buf r4=x r5=y r6=w r7=h r8=stride r9=data (or stack9). */
		const int buf = (int)r3;
		const int x = (int)r4, y = (int)r5;
		const int w = (int)r6, h = (int)r7;
		const int stride = (int)r8;
		const uint32_t data_mac = r9 ? r9 : stack9;
		void *dst = data_mac ? Mac2HostAddr(data_mac) : NULL;
		const bool ok = GlideStateLfbReadRegion(buf, x, y, w, h, stride, dst);
		GlideLog("grLfbReadRegion %dx%d @%d,%d -> %s", w, h, x, y, ok ? "OK" : "FAIL");
		return ok ? FXTRUE : FXFALSE;
	}
	case kGlide_grLfbWriteRegion: {
		/* FxBool grLfbWriteRegion(dst_buffer, dst_x, dst_y, src_format,
		 *   src_width, src_height, pixelPipeline, src_stride, src_data)
		 * r3..r10 + stack9 = src_data when 9 args. */
		const int buf = (int)r3;
		const int x = (int)r4, y = (int)r5;
		const int fmt = (int)r6;
		const int w = (int)r7, h = (int)r8;
		/* r9 = pixelPipeline or stride; r10 = stride or data */
		int stride = (int)r10;
		uint32_t data_mac = stack9;
		if (!data_mac && r10 > 0x1000) {
			/* 8-arg form: r9=stride r10=data */
			stride = (int)r9;
			data_mac = r10;
		}
		const void *src = data_mac ? Mac2HostAddr(data_mac) : NULL;
		const bool ok = GlideStateLfbWriteRegion(buf, x, y, fmt, w, h, stride, src);
		if (ok) {
			int uw = 0, uh = 0, upitch = 0;
			const uint8_t *bgra = GlideStateLfbConvertToBGRA(&uw, &uh, &upitch);
			if (bgra)
				GlideMetalUploadLfbAndPresent(bgra, uw, uh, upitch, 1);
			GlideMetalMarkContent();
		}
		GlideLog("grLfbWriteRegion %dx%d @%d,%d fmt=%d -> %s",
				  w, h, x, y, fmt, ok ? "OK" : "FAIL");
		return ok ? FXTRUE : FXFALSE;
	}
	case kGlide_grLfbConstantAlpha:
		GlideStateSetLfbConstAlpha(r3);
		return FXTRUE;
	case kGlide_grLfbConstantDepth:
		GlideStateSetLfbConstDepth(r3);
		return FXTRUE;
	case kGlide_grLfbWriteColorFormat:
		GlideStateSetLfbWriteColorFormat((int)r3);
		return FXTRUE;
	case kGlide_grLfbWriteColorSwizzle:
		GlideStateSetLfbWriteColorSwizzle((int)r3);
		return FXTRUE;
	/*
		GetProcAddress Ext

		typedef void* GrSurface_t;
		typedef uint32_t GrSurfaceTexType_t;
		#define GR_SURFACETEXTYPE_FB            0
		#define GR_SURFACETEXTYPE_AGP           1
		typedef uint32_t GrSurfaceContextType_t;
		#define GR_SURFACECONTEXT_WINDOWED      0
		#define GR_SURFACECONTEXT_FULLSCREEN    1
		#define GR_SURFACE_EXTENSION            0x1000
		#define GR_SURFACE_SIZE                 (GR_SURFACE_EXTENSION | 0x1)
		#define GR_SURFACE_TEXTURE              (GR_SURFACE_EXTENSION | 0x2)
	*/
	case kGlide_grSurfaceCreateContextExt: {
		/*
		* GrContext_t grSurfaceCreateContextExt(GrSurfaceContextType_t type)
		*/
		GlideLog("grSurfaceCreateContextExt type=%u -> 1 (no-op)",
			(unsigned)r3);
		return 1;
	}
	case kGlide_grSurfaceReleaseContextExt: {
		/*
		* void grSurfaceReleaseContextExt(GrContext_t ctx)
		*/
		GlideLog("grSurfaceReleaseContextExt sfc=%08x (no-op)",
			(unsigned)r3);
		return 0;
	}
	case kGlide_grSurfaceSetRenderingSurfaceExt: {
		/*
		* void grSurfaceSetRenderingSurfaceExt(
		*	GrSurface_t sfc, FxBool textureP)
		*/
		GlideLog("grSurfaceSetRenderingSurfaceExt sfc=%08x "
				" textureP=%d (no-op)",
			(unsigned)r3, (int)r4);
		return 0;
	}
	case kGlide_grSurfaceCalcTextureWHDExt: {
		/*
		*  FxBool grSurfaceCalcTextureWHDExt(GrTexInfo *tInfo,
		*		FxU32 *w, FxU32 *h, FxU32 *d)
		*/
		GlideLog("grSurfaceCalcTextureWHDExt tInfo=%08x "
				" w=%u h=%u d=%u -> FXTRUE (no-op)",
			(unsigned)r3, (unsigned)r4, (unsigned)r5, (unsigned)r6);
		return FXTRUE;
	}
	case kGlide_grSurfaceSetAuxSurfaceExt: {
		/*
		* void grSurfaceSetAuxSurfaceExt(GrSurface_t sfc)
		*/
		GlideLog("grSurfaceSetAuxSurfaceExt surface=%08x (no-op)",
				  (unsigned)r3);
		return 0;
	}
	case kGlide_grSurfaceSetTextureSurfaceExt:
		/*
		 * typedef int32_t GrChipID_t;
		 * void grSurfaceSetTextureSurfaceExt(GrChipID_t tmu,
		 *                                      GrSurface_t surface)
		 * A NULL surface is the RAVE driver's teardown-time unbind.  There
		 * is no host surface object to release in the current renderer, so
		 * accepting it is the correct minimal behavior.  This hook is not
		 * enough to claim the complete SURFACE rendering extension.
		 */
		GlideLog("grSurfaceSetTextureSurfaceExt tmu=%d surface=%08x (no-op)",
				  (int)r3, (unsigned)r4);
		return 0;
	case kGlide_grDeviceQueryExt: {
		uint32_t ret;
		/*
		 * FxU32 grDeviceQueryExt(GrDeviceInfo_t devList[], FxU32 listCount)
		 *
		 * typedef struct {
		 *	  FxU32 glideDeviceId; void* systemDeviceId; FxU32 reserved;
		 *	} GrDeviceInfo_t;
		 *
		 * D2: grGetProcAddress("grDeviceQueryExt") then call with
		 * (void *buf, FxU32 size). Log shows r4=0x10 (16-byte query block).
		 * Fill a Voodoo-like summary so detection does not reject zeros.
		 */
		uint32_t buf = r3;
		uint32_t size = r4 ? r4 : 16;
		if (buf && size >= 1) {
			/* Word0: board type GR_SSTTYPE_VOODOO=0 or Voodoo2=3 */
			WriteMacInt32(buf + 0, 3); /* Voodoo2-class */
			/*if (size >= 8)*/
				WriteMacInt32(buf + 4, (int32_t)(GlideStateFbMem() / (1024 * 1024)));
			/*if (size >= 12)
				WriteMacInt32(buf + 8, 1);*/ /* nTMU */
			/*if (size >= 16)
				WriteMacInt32(buf + 12, (int32_t)(GlideStateTmuMem() / (1024 * 1024)));
			*/
			ret = 1;
		} else {
			ret = 0;
		}
		GlideLog("grDeviceQueryExt buf=%08x size=%u -> TRUE", buf, size);
		return ret;
	}
	/*
		typedef struct {
		  FxU32 surface;
		  FxU32 width;
		  FxU32 height;
		  FxU32 pitch;
		  FxU32 bytesPerPixel;
		  FxU32 pixelFormat;
		  void  (*notifyCallback)(GrSurface_t sfc, void *userData, FxU32 code);
		  void  *userData;
		  void  *systemPortId;
		  void  *systemDeviceId;
		  FxU32 reserved;
		} GrSurfaceDesc_t;
	*/
	case kGlide_grSurfaceCreateExt: {
		/*
		* GrSurface_t grSurfaceCreateExt(GrSurfaceDesc_t *sfcDesc)
		*/
		GlideLog("grSurfaceGetDescExt sfcDesc=%08x -> 1 (no-op)",
			(unsigned)r3);
		return 1;
	}
	case kGlide_grSurfaceReleaseExt: {
		/*
		* void grSurfaceReleaseExt(GrSurface_t sfc)
		*/
		GlideLog("grSurfaceGetDescExt sfc=%08x (no-op)",
			(unsigned)r3);
		return 0;
	}
	case kGlide_grSurfaceGetDescExt: {
		/*
		* void grSurfaceGetDescExt(GrSurface_t sfc, GrSurfaceDesc_t *sfcDesc)
		*/
		GlideLog("grSurfaceGetDescExt sfc=%08x "
				" sfcDesc=%08x (no-op)",
			(unsigned)r3, (int)r4);
		return 0;
	}
	default:
		GlideLog("GlideDispatch: unhandled subop=%u", subop);
		return 0;
	}
}
