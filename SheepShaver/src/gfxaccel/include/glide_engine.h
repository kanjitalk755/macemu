/*
 *  glide_engine.h - 3dfx Glide 2.x / 3.x guest API (CFM hooks + dispatch)
 *
 *  Peer to rave_engine.h / gl_engine.h / dsp_engine.h. Guest Glide library
 *  exports are patched to TVECTs that land in NATIVE_GLIDE_DISPATCH.
 *
 *  Diablo II (Mac) is the first acceptance title and uses Glide 3.0.
 *  Glide 2.x shares the same raster path with a separate symbol table.
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

#ifndef GLIDE_ENGINE_H
#define GLIDE_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sub-opcode ranges:
 *   0-199:   shared / Glide 2 core
 *   200-399: Glide 3 combinatorial / query API
 *   400-499: LFB / extension
 */
enum {
	/* Lifecycle / SST */
	kGlide_grGlideInit                 = 0,
	kGlide_grGlideShutdown             = 1,
	kGlide_grGlideGetVersion           = 2,
	kGlide_grSstQueryBoards            = 3,
	kGlide_grSstQueryHardware          = 4,
	kGlide_grSstSelect                 = 5,
	kGlide_grSstWinOpen                = 6,
	kGlide_grSstWinClose               = 7,
	kGlide_grSstControl                = 8,
	kGlide_grSstIdle                   = 9,
	kGlide_grSstIsBusy                 = 10,
	kGlide_grSstOrigin                 = 11,
	kGlide_grSstScreenWidth            = 12,
	kGlide_grSstScreenHeight           = 13,
	kGlide_grSstStatus                 = 14,
	kGlide_grSstVRetraceOn             = 15,
	kGlide_grSstVideoLine              = 16,

	/* Buffering */
	kGlide_grBufferClear               = 20,
	kGlide_grBufferSwap                = 21,
	kGlide_grBufferNumPending          = 22,
	kGlide_grRenderBuffer              = 23,

	/* Drawing */
	kGlide_grDrawPoint                 = 30,
	kGlide_grDrawLine                  = 31,
	kGlide_grDrawTriangle              = 32,
	kGlide_grDrawPlanarPolygon         = 33,
	kGlide_grDrawPlanarPolygonVertexList = 34,
	kGlide_grDrawPolygon               = 35,
	kGlide_grDrawPolygonVertexList     = 36,
	kGlide_grAADrawTriangle            = 37,

	/* State (Glide 2 style + shared) */
	kGlide_grAlphaBlendFunction        = 50,
	kGlide_grAlphaCombine              = 51,
	kGlide_grAlphaControlsITRGBLighting = 52,
	kGlide_grAlphaTestFunction         = 53,
	kGlide_grAlphaTestReferenceValue   = 54,
	kGlide_grChromakeyMode             = 55,
	kGlide_grChromakeyValue            = 56,
	kGlide_grClipWindow                = 57,
	kGlide_grColorCombine              = 58,
	kGlide_grColorMask                 = 59,
	kGlide_grConstantColorValue        = 60,
	kGlide_grConstantColorValue4       = 61,
	kGlide_grCullMode                  = 62,
	kGlide_grDepthBiasLevel            = 63,
	kGlide_grDepthBufferFunction       = 64,
	kGlide_grDepthBufferMode           = 65,
	kGlide_grDepthMask                 = 66,
	kGlide_grDisableAllEffects         = 67,
	kGlide_grDitherMode                = 68,
	kGlide_grFogColorValue             = 69,
	kGlide_grFogMode                   = 70,
	kGlide_grFogTable                  = 71,
	kGlide_grGammaCorrectionValue      = 72,
	kGlide_grHints                     = 73,
	kGlide_grSplash                    = 74,

	/* Textures */
	kGlide_grTexCalcMemRequired        = 90,
	kGlide_grTexTextureMemRequired     = 91,
	kGlide_grTexMinAddress             = 92,
	kGlide_grTexMaxAddress             = 93,
	kGlide_grTexNCCTable               = 94,
	kGlide_grTexSource                 = 95,
	kGlide_grTexClampMode              = 96,
	kGlide_grTexCombine                = 97,
	kGlide_grTexDetailControl          = 98,
	kGlide_grTexFilterMode             = 99,
	kGlide_grTexLodBiasValue           = 100,
	kGlide_grTexLodTable               = 101,
	kGlide_grTexMipMapMode             = 102,
	kGlide_grTexDownloadMipMap         = 103,
	kGlide_grTexDownloadMipMapLevel    = 104,
	kGlide_grTexDownloadMipMapLevelPartial = 105,
	kGlide_grTexDownloadTable          = 106,
	kGlide_grTexDownloadTablePartial   = 107,
	kGlide_grTexMultibase              = 108,
	kGlide_grTexMultibaseAddress       = 109,

	/* Glide 3 */
	kGlide_grGet                       = 200,
	kGlide_grGetString                 = 201,
	kGlide_grReset                     = 202,
	kGlide_grEnable                    = 203,
	kGlide_grDisable                   = 204,
	kGlide_grCoordinateSystem          = 205,
	kGlide_grVertexLayout              = 206,
	kGlide_grDrawVertexArray           = 207,
	kGlide_grDrawVertexArrayContiguous = 208,
	kGlide_grGlideGetState             = 209,
	kGlide_grGlideSetState             = 210,
	kGlide_grGlideGetVertexLayout      = 211,
	kGlide_grGlideSetVertexLayout      = 212,
	kGlide_grFinish                    = 213,
	kGlide_grFlush                     = 214,
	kGlide_grLoadIdentity              = 215,
	kGlide_grLoadMatrix                = 216,
	kGlide_grMultMatrix                = 217,
	kGlide_grRotatef                   = 218,
	kGlide_grScalef                    = 219,
	kGlide_grTranslatef                = 220,
	kGlide_grViewport                  = 221,
	kGlide_grDepthRange                = 222,
	kGlide_grTexDownloadMipMapPartial  = 223,
	kGlide_grTexNCCTableG3             = 224,
	kGlide_grSelectContext             = 225,
	kGlide_grSstWinOpenG3              = 226, /* alias if export differs */
	kGlide_guEncodeRLE16			= 227,
	kGlide_guTexCreateColorMipMap	= 228,
	kGlide_guFogGenerateLinear		= 229,
	kGlide_guFogGenerateExp2		= 230,
	kGlide_guFogGenerateExp		= 231,
	kGlide_guFogTableIndexToW	= 232,
	kGlide_guEndianSwapBytes	= 233,
	kGlide_guEndianSwapWords	= 234,
	kGlide_guAlphaSource		= 235,
	kGlide_grTexChromaRange		= 236,
	kGlide_grTexChromaMode		= 237,
	kGlide_grLoadGammaTable		= 238,
	kGlide_grChromaRange		= 239,
	kGlide_grChromaRangeMode	= 240,
	kGlide_grSstVidMode			= 241,
	kGlide_grQueryResolutions	= 242,
	kGlide_gu3dfLoad	= 243,
	kGlide_gu3dfGetInfo	= 244,
	kGlide_grErrorSetCallback	= 245,
	kGlide_grGetProcAddress            = 246, /* Glide 3 extension lookup */
	kGlide_guGammaCorrectionRGB        = 247,
	/* Glide 2 "gu" convenience wrappers: single-enum shorthands that the stock
	 * library expands into a grColorCombine / grTexCombine call. Unreal
	 * Tournament sets up its combine state exclusively through these. */
	kGlide_guColorCombineFunction      = 248,
	/* GetProcAddress */
	kGlide_FirstGetProcAddress = 249,
	kGlide_grSurfaceCreateContextExt = 249,
	kGlide_grSurfaceReleaseContextExt = 250,
	kGlide_grSurfaceSetRenderingSurfaceExt = 251,
	kGlide_grSurfaceCalcTextureWHDExt = 252,
	kGlide_grSurfaceSetAuxSurfaceExt = 253,
	kGlide_grSurfaceSetTextureSurfaceExt = 254, /* 3dfx RAVE teardown */
	kGlide_grDeviceQueryExt = 255,
	kGlide_grSurfaceCreateExt = 256,
	kGlide_grSurfaceReleaseExt = 257,
	kGlide_grSurfaceGetDescExt = 258,
	kGlide_LastGetProcAddress = 258,

	/* See kGlide_guColorCombineFunction. Kept outside the GetProcAddress
	 * range above, which glide_engine.cpp walks to allocate extension TVECTs. */
	kGlide_guTexCombineFunction        = 259,
	/* Glide 2-only exports used by Summoner.  The Mac library exports both
	 * grTexCombineFunction and guTexCombineFunction as distinct TVECTs. */
	kGlide_guTexMemQueryAvail          = 260,
	kGlide_grTexCombineFunction        = 261,

	/* LFB */
	kGlide_grLfbLock                   = 400,
	kGlide_grLfbUnlock                 = 401,
	kGlide_grLfbReadRegion             = 402,
	kGlide_grLfbWriteRegion            = 403,
	kGlide_grLfbConstantAlpha          = 404,
	kGlide_grLfbConstantDepth          = 405,
	kGlide_grLfbWriteColorFormat       = 406,
	kGlide_grLfbWriteColorSwizzle      = 407,

	kGlide_SUBOPCODE_MAX               = 510
};

#define GLIDE_MAX_SUBOPCODE 512

/* Error / bool style used by Glide (FxBool, FxI32). */
#ifndef FXTRUE
#define FXTRUE  1
#define FXFALSE 0
#endif

/* Common GrScreenResolution_t values */
enum {
	GR_RESOLUTION_320x200   = 0x0,
	GR_RESOLUTION_320x240   = 0x1,
	GR_RESOLUTION_400x256   = 0x2,
	GR_RESOLUTION_512x384   = 0x3,
	GR_RESOLUTION_640x200   = 0x4,
	GR_RESOLUTION_640x350   = 0x5,
	GR_RESOLUTION_640x400   = 0x6,
	GR_RESOLUTION_640x480   = 0x7,
	GR_RESOLUTION_800x600   = 0x8,
	GR_RESOLUTION_960x720   = 0x9,
	GR_RESOLUTION_856x480   = 0xa,
	GR_RESOLUTION_512x256   = 0xb,
	GR_RESOLUTION_1024x768  = 0xc,
	GR_RESOLUTION_1280x1024 = 0xd,
	GR_RESOLUTION_1600x1200 = 0xe,
	GR_RESOLUTION_400x300   = 0xf,
	GR_RESOLUTION_NONE      = 0xff
};

enum {
	GR_REFRESH_60Hz = 0x0,
	GR_REFRESH_70Hz = 0x1,
	GR_REFRESH_72Hz = 0x2,
	GR_REFRESH_75Hz = 0x3,
	GR_REFRESH_80Hz = 0x4,
	GR_REFRESH_90Hz = 0x5,
	GR_REFRESH_100Hz = 0x6,
	GR_REFRESH_85Hz = 0x7,
	GR_REFRESH_120Hz = 0x8,
	GR_REFRESH_NONE = 0xff
};

enum {
	GR_COLORFORMAT_ARGB = 0x0,
	GR_COLORFORMAT_ABGR = 0x1,
	GR_COLORFORMAT_RGBA = 0x2,
	GR_COLORFORMAT_BGRA = 0x3
};

enum {
	GR_ORIGIN_UPPER_LEFT = 0x0,
	GR_ORIGIN_LOWER_LEFT = 0x1
};

/*
 * grGet / grGetString tokens - official Glide 3 (3Dfx glide.h GLIDE3 block).
 * Earlier shifted values were wrong and broke D2 setup (notably 0x24 is
 * GR_TEXTURE_ALIGN); 0xa0 is the first grGetString token.
 */
enum {
	GR_BITS_DEPTH                   = 0x01,
	GR_BITS_RGBA                    = 0x02,
	GR_FIFO_FULLNESS                = 0x03,
	GR_FOG_TABLE_ENTRIES            = 0x04,
	GR_GAMMA_TABLE_ENTRIES          = 0x05,
	GR_GLIDE_STATE_SIZE             = 0x06,
	GR_GLIDE_VERTEXLAYOUT_SIZE      = 0x07,
	GR_IS_BUSY                      = 0x08,
	GR_LFB_PIXEL_PIPE               = 0x09,
	GR_MAX_TEXTURE_SIZE             = 0x0a,
	GR_MAX_TEXTURE_ASPECT_RATIO     = 0x0b,
	GR_MEMORY_FB                    = 0x0c,
	GR_MEMORY_TMU                   = 0x0d,
	GR_MEMORY_UMA                   = 0x0e,
	GR_NUM_BOARDS                   = 0x0f,
	GR_NON_POWER_OF_TWO_TEXTURES    = 0x10,
	GR_NUM_FB                       = 0x11,
	GR_NUM_SWAP_HISTORY_BUFFER      = 0x12,
	GR_NUM_TMU                      = 0x13,
	GR_PENDING_BUFFERSWAPS          = 0x14,
	GR_REVISION_FB                  = 0x15,
	GR_REVISION_TMU                 = 0x16,
	GR_STATS_LINES                  = 0x17,
	GR_STATS_PIXELS_AFUNC_FAIL      = 0x18,
	GR_STATS_PIXELS_CHROMA_FAIL     = 0x19,
	GR_STATS_PIXELS_DEPTHFUNC_FAIL  = 0x1a,
	GR_STATS_PIXELS_IN              = 0x1b,
	GR_STATS_PIXELS_OUT             = 0x1c,
	GR_STATS_PIXELS                 = 0x1d,
	GR_STATS_POINTS                 = 0x1e,
	GR_STATS_TRIANGLES_IN           = 0x1f,
	GR_STATS_TRIANGLES_OUT          = 0x20,
	GR_STATS_TRIANGLES              = 0x21,
	GR_SWAP_HISTORY                 = 0x22,
	GR_SUPPORTS_PASSTHRU            = 0x23,
	GR_TEXTURE_ALIGN                = 0x24,
	GR_VIDEO_POSITION               = 0x25,
	GR_VIEWPORT                     = 0x26,
	GR_WDEPTH_MIN_MAX               = 0x27,
	GR_ZDEPTH_MIN_MAX               = 0x28,
	GR_VERTEX_PARAMETER             = 0x29,
	GR_BITS_GAMMA                   = 0x2a,

	/* grGetString */
	GR_EXTENSION                    = 0xa0,
	GR_HARDWARE                     = 0xa1,
	GR_RENDERER                     = 0xa2,
	GR_VENDOR                       = 0xa3,
	GR_VERSION                      = 0xa4
};

void GlideThunksInit(void);
void GlideInstallHooks(void);
void GlideResetForReboot(void);
void GlideForceReinstallHooks(void);
/* No-op: guest already has the extension (DSp model). */
bool GlideRegisterCfmLibraries(void);

/* Dispatch: called from sheepshaver_glue NATIVE_GLIDE_DISPATCH.
 * r3-r10 = first 8 integer args; sp = guest r1 for 9th+ stack args.
 * f1-f4 are the first four PPC floating-point argument registers. */
uint32_t GlideDispatch(uint32_t r3, uint32_t r4, uint32_t r5,
                       uint32_t r6, uint32_t r7, uint32_t r8,
                       uint32_t r9, uint32_t r10, uint32_t sp,
                       double f1, double f2, double f3, double f4);

/* Float-return side channel.
 *
 * Almost every Glide entry point returns void or an integer in r3, so the
 * dispatch returns a single uint32. The few that return a float (PPC returns
 * those in FPR1, not r3) set this flag plus the value; the glue copies it into
 * fpr(1) and clears the flag. Checked immediately after every GlideDispatch
 * call, so there is no cross-call lifetime to worry about. */
bool GlideDispatchTakeFloatResult(float *out);

/* Comprehensive hang dump of Glide host state + last N guest calls. */
void GlideHangDumpState(void);

/* CFM gate handlers + host-side FindLibSymbol synthetic resolve. */
uint32_t NativeGlideHookGetSharedLibrary(uint32_t libName, uint32_t arch,
                                         uint32_t flags, uint32_t connIDPtr,
                                         uint32_t mainAddrPtr, uint32_t errMsgPtr);
uint32_t NativeGlideHookFindSymbol(uint32_t conn, uint32_t symName,
                                   uint32_t symAddrPtr, uint32_t symClassPtr);
uint32_t NativeGlideHookCloseConnection(uint32_t connIDPtr);
uint32_t NativeGlideHookCountSymbols(uint32_t conn, uint32_t countPtr);
uint32_t NativeGlideHookGetIndSymbol(uint32_t conn, uint32_t index,
                                     uint32_t namePtr, uint32_t addrPtr,
                                     uint32_t classPtr);
uint32_t GlideResolveSyntheticSymbol(const char *lib_pascal,
                                     const char *sym_pascal);

/* Scratch / TVECT table (defined in glide_thunks.cpp). */
extern uint32_t glide_scratch_addr;
extern uint32_t glide_method_tvects[GLIDE_MAX_SUBOPCODE];

/* GL renderer entry points (gl/glide_gl_renderer.cpp). */
int  GlideMetalInit(void);
void GlideMetalShutdown(void);
int  GlideMetalWinOpen(int width, int height, int origin_upper_left);
void GlideMetalWinClose(void);
void GlideMetalBufferClear(uint32_t color, uint32_t alpha, uint32_t depth);
void GlideMetalBufferSwap(int swap_interval);
/* Submit overlay mailbox; do_present=0 defers Present to VideoVBL. */
void GlideMetalPublishOverlay(int do_present);
void GlideMetalDrawPoint(const void *a);
void GlideMetalDrawLine(const void *a, const void *b);
void GlideMetalDrawTriangle(const void *a, const void *b, const void *c);
void GlideMetalDrawPolygon(int nverts, const void *const *ptrs);
void GlideMetalDrawPolygonIndexed(int nverts, const void *indices,
                                  const void *verts, uint32_t stride);
void GlideMetalDrawPolygonContiguous(int nverts, const void *verts, uint32_t stride);
void GlideMetalDrawVertexArray(uint32_t mode, uint32_t count, const void *const *ptrs);
void GlideMetalDrawVertexArrayContiguous(uint32_t mode, uint32_t count,
                                      const void *vertices, uint32_t stride);
void GlideMetalApplyState(void);
void GlideMetalSetClipWindow(int minx, int miny, int maxx, int maxy);
void GlideMetalSetColorMask(int r, int g, int b, int a);
void GlideMetalSetAlphaTest(int enabled, int func, float ref);
void GlideMetalSetChromakey(void);
void GlideMetalSetFog(int mode, uint32_t color);
void GlideMetalSetDepthBias(float bias);
/* Mark that the overlay has real pixels (LFB / draws) - enables present. */
void GlideMetalMarkContent(void);
void GlideMetalSplash(void);
void GlideMetalFlush(void);
void GlideMetalFinish(void);
/* Upload guest LFB (already converted to BGRA8) into overlay and present. */
void GlideMetalUploadLfbAndPresent(const uint8_t *bgra, int w, int h, int pitch,
                                int present);
/* Texture path: download into TMU sim, bind for draw. */
void GlideMetalTexDownloadLevel(uint32_t start_addr, int lod, int large_lod,
                             int aspect_log2, int format, const void *data, uint32_t nbytes);
void GlideMetalTexSource(uint32_t start_addr, int even_odd, int small_lod,
                      int large_lod, int aspect_log2, int format);
void GlideMetalTexDownloadTable(int type, const void *data);

#ifdef __cplusplus
}
#endif

#endif /* GLIDE_ENGINE_H */
