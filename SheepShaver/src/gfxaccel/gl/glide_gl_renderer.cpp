/*
 *  glide_gl_renderer.cpp - 3dfx Glide raster path on desktop OpenGL
 *
 *  Renders into a double-buffered overlay texture (same compositor contract
 *  as RAVE) and publishes via MetalCompositorSubmitFrame each buffer swap.
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
#include "glide_engine.h"
#include "metal_compositor.h"
#include "metal_device_shared.h"
#include "display_mode_controller.h"
#include "gfxaccel_resources.h"
#include "gfxaccel_backend.h"
#include "gfx_frame_pacing_policy.h"
#include "gfx_log.h"

#if USE_SDL3
#include <SDL3/SDL_opengl.h>
#else
#include <SDL_opengl.h>
#endif
#include "gl_ext.h"

#include <cstring>
#include <vector>
#include <cmath>

/* Classic GrVertex layout (Glide 2 / common packed layout). Glide 3 may
 * override field offsets via grVertexLayout; we still accept the classic
 * structure when layout offsets match. */
struct GrVertexClassic {
	float x, y, z;           /* 0 */
	float r, g, b;           /* 12 */
	float ooz;               /* 24 */
	float a;                 /* 28 */
	float s, t, w;           /* 32 - simplified */
};

/* Double-buffered overlay. Glide draws directly into glide_color_tex
 * (glide_overlay_pair[0]); the compositor samples the SEPARATE front texture
 * glide_front_tex (glide_overlay_pair[1]). The front is only refreshed from a
 * COMPLETE frame at grBufferSwap / LFB-present, so the free-run VBL present can
 * never composite a half-drawn or freshly-cleared (black) draw buffer - which
 * was the source of the black-screen flashes. The draw buffer persists between
 * frames so incremental (non-full-clear) rendering still accumulates. */
static GLuint glide_overlay_pair[2] = {0, 0};
static GLuint glide_color_tex = 0;   /* draw target (pair[0]) */
static GLuint glide_front_tex = 0;   /* compositor-sampled front (pair[1]) */
static GLuint glide_fbo = 0;
static GLuint glide_depth_rb = 0;
static uint32_t glide_width = 0, glide_height = 0;
static uint32_t glide_write = 0;
static bool glide_is_ready = false;
static bool glide_is_in_frame = false;
static bool glide_has_context = false;
/* Set once a complete frame has been copied into glide_front_tex. Until then
 * there is nothing safe to present, so the overlay submit is suppressed. */
static bool glide_front_valid = false;

static GLuint glide_gl_texture = 0;
static float glide_texture_s_extent = 256.f;
static float glide_texture_t_extent = 256.f;
static bool glide_is_texture_enabled = false;
static bool glide_alpha_lighting_multitex = false;
static int glide_texture_mip_levels = 1;

struct GlideMetalTextureCacheEntry {
	uint32_t address;
	int width;
	int height;
	int format;
	int small_lod;
	int large_lod;
	int aspect_log2;
	int even_odd;
	uint32_t chain_size;
	int mip_levels;
	int chroma_mode;
	uint32_t chroma_value;
	int color_format;
	GLuint texture;
	bool dirty;
	GLint wrap_s;
	GLint wrap_t;
};

/* Diablo II switches among hundreds of resident TMU addresses per frame.
 * A single host texture forced a CPU decode + glTexSubImage for every
 * grTexSource, even when the guest had not changed that address.
 * FIXME: Awful idea for a global texture cache.
 */
static std::vector<GlideMetalTextureCacheEntry> glide_texture_cache;

static void GlideSubitOverlay(int do_present);
static void GlideUnpackColorRGB(uint32_t color, int color_format,
								uint8_t *out_r, uint8_t *out_g,
								uint8_t *out_b);

static void GlideReleaseTextureCache(void)
{
	if (SharedMetalDevice()) {
		for (size_t ei = 0; ei < glide_texture_cache.size(); ei++) {
			const GlideMetalTextureCacheEntry &entry = glide_texture_cache[ei];
			if (entry.texture)
				glDeleteTextures(1, &entry.texture);
		}
	}
	glide_texture_cache.clear();
	glide_gl_texture = 0;
	glide_texture_s_extent = glide_texture_t_extent = 256.f;
	glide_is_texture_enabled = false;
	glide_texture_mip_levels = 1;
}

/* The SDL compositor and Glide use the same compatibility context.  A VBL
 * Present must not bind framebuffer 0 or replace Glide state between two
 * guest draw calls in the same back-buffer frame.
 */
extern "C" int GlideMetalRenderPassActive(void)
{
	return glide_is_in_frame ? 1 : 0;
}

extern int GlideStateDepthMode(void);
extern int GlideStateDepthMask(void);
extern int GlideStateDepthFunc(void);
extern float GlideStateDepthBias(void);
extern int GlideStateCullMode(void);
extern int GlideStateAlphaBlendSrc(void);
extern int GlideStateAlphaBlendDst(void);
extern int GlideStateColorFormat(void);
extern float GlideStateConstantR(void);
extern float GlideStateConstantG(void);
extern float GlideStateConstantB(void);
extern float GlideStateConstantA(void);
extern int GlideStateColorCombineFunction(void);
extern int GlideStateColorCombineFactor(void);
extern int GlideStateColorCombineLocal(void);
extern int GlideStateColorCombineOther(void);
extern int GlideStateColorCombineInvert(void);
extern int GlideStateAlphaCombineFunction(void);
extern int GlideStateAlphaCombineFactor(void);
extern int GlideStateAlphaCombineLocal(void);
extern int GlideStateAlphaCombineOther(void);
extern int GlideStateAlphaCombineInvert(void);
extern int GlideStateCoordSystem(void);
extern int GlideStateVertexOffset(int param);
extern int GlideStateVertexStride(void);
extern int GlideStateOriginUpperLeft(void);
extern int GlideStateColorMaskR(void);
extern int GlideStateColorMaskG(void);
extern int GlideStateColorMaskB(void);
extern int GlideStateColorMaskA(void);
extern int GlideStateAlphaTestEnabled(void);
extern int GlideStateAlphaTestFunc(void);
extern float GlideStateAlphaTestRef(void);
extern int GlideStateClipMinX(void);
extern int GlideStateClipMinY(void);
extern int GlideStateClipMaxX(void);
extern int GlideStateClipMaxY(void);
extern int GlideStateViewportX(void);
extern int GlideStateViewportY(void);
extern int GlideStateViewportWidth(void);
extern int GlideStateViewportHeight(void);
extern float GlideStateDepthRangeNear(void);
extern float GlideStateDepthRangeFar(void);
extern int GlideStateChromaMode(void);
extern uint32_t GlideStateChromaValue(void);
extern uint32_t GlideStateChromaRangeMin(void);
extern uint32_t GlideStateChromaRangeMax(void);
extern int GlideStateChromaRangeMode(void);
extern uint32_t GlideStateTexChromaRangeMin(void);
extern uint32_t GlideStateTexChromaRangeMax(void);
extern int GlideStateTexChromaMode(void);
extern int GlideStateFogMode(void);
extern uint32_t GlideStateFogColor(void);
extern float GlideStateFogFactor(float oow);
extern int GlideStateAlphaControlsLighting(void);
extern const uint8_t *GlideStateTmuPtr(uint32_t start, uint32_t *out_avail);
extern bool GlideStateTmuWrite(uint32_t start, const void *src, uint32_t nbytes);
extern void GlideTexLodDims(int lod, int aspect_log2, int *out_w, int *out_h);
extern int GlideTexBpp(int format);
extern uint32_t GlideTexLevelSizeBytes(int lod, int aspect_log2, int format);
extern uint32_t GlideTexLevelOffsetBytes(int lod, int large_lod,
										 int aspect_log2, int format);
extern uint32_t GlideTexCalcMemRequired(int small_lod, int large_lod,
										int aspect_log2, int format);
extern int GlideTexDecodeAspectLog2(int aspect);
extern const uint32_t *GlideStateTexPalette(void);
extern int GlideStateTexClampS(void);
extern int GlideStateTexClampT(void);
extern int GlideStateTexFilterMin(void);
extern int GlideStateTexFilterMag(void);
extern float GlideStateTexLodBias(void);
extern int GlideStateTexMipMapMode(void);

static void GlideReleaseOverlay(void)
{
	if (!SharedMetalDevice()) {
		/* The SDL GL context has already gone away, so its object names are
		 * invalid and must not survive into the replacement context.  Return
		 * the logical handles too: the GL resource manager must forget the
		 * same stale names or its next vend would hand them straight back. */
		for (int i = 0; i < 2; ++i) {
			if (glide_overlay_pair[i])
				gfxaccel_resources_release_overlay_texture(
					kGfxEngineGlide,
					(void *)(uintptr_t)glide_overlay_pair[i]);
			glide_overlay_pair[i] = 0;
		}
		glide_color_tex = 0;
		glide_front_tex = 0;
		glide_fbo = 0;
		glide_depth_rb = 0;
		glide_front_valid = false;
		glide_width = glide_height = 0;
		glide_write = 0;
		glide_is_in_frame = false;
		glide_has_context = false;
		return;
	}
	GfxGLExt &ext = gfx_gl_ext();
	if (glide_fbo) {
		if (ext.fbo) ext.BindFramebuffer(GL_FRAMEBUFFER, 0);
		if (ext.DeleteFramebuffers) ext.DeleteFramebuffers(1, &glide_fbo);
		glide_fbo = 0;
	}
	if (glide_depth_rb) {
		if (ext.DeleteRenderbuffers) ext.DeleteRenderbuffers(1, &glide_depth_rb);
		glide_depth_rb = 0;
	}
	for (int i = 0; i < 2; i++) {
		if (glide_overlay_pair[i]) {
			gfxaccel_resources_release_overlay_texture(
				kGfxEngineGlide, (void *)(uintptr_t)glide_overlay_pair[i]);
			glide_overlay_pair[i] = 0;
		}
	}
	glide_color_tex = 0;
	glide_front_tex = 0;
	glide_front_valid = false;
	glide_width = glide_height = 0;
	glide_write = 0;
	glide_is_in_frame = false;
	glide_has_context = false;
	MetalCompositorSubmitFrame_ClearCachedOverlay();
}

/* Publish the completed draw buffer to the front (compositor-sampled) texture.
 * Copies the whole draw color texture via the draw FBO so the front always
 * holds a COMPLETE frame - never a half-drawn or freshly-cleared draw buffer.
 * Must be called with a valid overlay + FBO; leaves GL bound to framebuffer 0. */
static void GlidePublishFrontFromDraw(void)
{
	if (!glide_fbo || !glide_color_tex || !glide_front_tex ||
		!SharedMetalDevice())
		return;
	GfxGLExt &ext = gfx_gl_ext();
	if (!ext.fbo)
		return;
	/* Read from the draw color texture via the FBO; copy into the front tex. */
	ext.BindFramebuffer(GL_FRAMEBUFFER, glide_fbo);
	ext.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
							 GL_TEXTURE_2D, glide_color_tex, 0);
	glBindTexture(GL_TEXTURE_2D, glide_front_tex);
	/* glCopyTexSubImage2D is core GL 1.1 - copies the bound read framebuffer's
	 * color into the bound texture. Front and draw are the same size. */
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
						(GLsizei)glide_width, (GLsizei)glide_height);
	glBindTexture(GL_TEXTURE_2D, 0);
	ext.BindFramebuffer(GL_FRAMEBUFFER, 0);
	glide_front_valid = true;
}

static bool GlideEnsureOverlay(uint32_t w, uint32_t h)
{
	if (!SharedMetalDevice()) return false;
	if (glide_overlay_pair[0] && glide_overlay_pair[1] && glide_width == w && glide_height == h)
		return true;
	GlideReleaseOverlay();

	void *a = gfxaccel_resources_vend_overlay_texture_indexed(
		kGfxEngineGlide, 0, w, h, MTLPixelFormatBGRA8Unorm);
	void *b = gfxaccel_resources_vend_overlay_texture_indexed(
		kGfxEngineGlide, 1, w, h, MTLPixelFormatBGRA8Unorm);
	if (!a || !b) {
		if (a) gfxaccel_resources_release_overlay_texture(kGfxEngineGlide, a);
		if (b) gfxaccel_resources_release_overlay_texture(kGfxEngineGlide, b);
		QD3D_INIT_LOG("GlideMetal: overlay vend failed %ux%u", w, h);
		return false;
	}
	glide_overlay_pair[0] = (GLuint)(uintptr_t)a;
	glide_overlay_pair[1] = (GLuint)(uintptr_t)b;
	glide_write = 0;
	glide_color_tex = glide_overlay_pair[0];   /* draw target */
	glide_front_tex = glide_overlay_pair[1];   /* compositor-sampled front */
	glide_front_valid = false;
	glide_width = w;
	glide_height = h;

	GfxGLExt &ext = gfx_gl_ext();
	if (!ext.fbo) {
		QD3D_INIT_LOG("GlideMetal: FBO extension missing");
		GlideReleaseOverlay();
		return false;
	}
	ext.GenFramebuffers(1, &glide_fbo);
	ext.GenRenderbuffers(1, &glide_depth_rb);
	ext.BindRenderbuffer(GL_RENDERBUFFER, glide_depth_rb);
	/* Voodoo/Glide exposes a 16-bit Z/W buffer. Matching that precision is
	 * important for equality and depth-bias behavior. */
	ext.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
							(GLsizei)w, (GLsizei)h);
	ext.BindFramebuffer(GL_FRAMEBUFFER, glide_fbo);
	ext.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
							 GL_TEXTURE_2D, glide_color_tex, 0);
	ext.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
								GL_RENDERBUFFER, glide_depth_rb);
	GLenum st = ext.CheckFramebufferStatus(GL_FRAMEBUFFER);
	ext.BindFramebuffer(GL_FRAMEBUFFER, 0);
	if (st != GL_FRAMEBUFFER_COMPLETE) {
		QD3D_INIT_LOG("GlideMetal: FBO incomplete 0x%x", (unsigned)st);
		GlideReleaseOverlay();
		return false;
	}
	return true;
}

static bool GlideBindDrawFBO(void)
{
	if (!glide_fbo || !glide_color_tex || !SharedMetalDevice()) return false;
	GfxGLExt &ext = gfx_gl_ext();
	ext.BindFramebuffer(GL_FRAMEBUFFER, glide_fbo);
	ext.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
							 GL_TEXTURE_2D, glide_color_tex, 0);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	if (GlideStateCoordSystem() == 0) {
		glViewport(0, 0, (GLsizei)glide_width, (GLsizei)glide_height);
		/* Glide window coords: x right, y down if origin upper-left. */
		if (GlideStateOriginUpperLeft())
			glOrtho(0.0, (double)glide_width, (double)glide_height, 0.0, -1.0, 1.0);
		else
			glOrtho(0.0, (double)glide_width, 0.0, (double)glide_height, -1.0, 1.0);
		glDepthRange(0.0, 1.0);
	} else {
		/* Clip coordinates use Glide's explicit viewport.  OpenGL viewports
		 * cannot have negative dimensions, so use their absolute rectangle and
		 * mirror normalized coordinates in the projection when requested. */
		const int vx = GlideStateViewportX();
		const int vy = GlideStateViewportY();
		const int vw = GlideStateViewportWidth();
		const int vh = GlideStateViewportHeight();
		const int x1 = vx + vw;
		const int y1 = vy + vh;
		const int gl_x = vx < x1 ? vx : x1;
		const int gl_w = vw < 0 ? -vw : vw;
		int gl_y;
		if (GlideStateOriginUpperLeft()) {
			const int gy0 = (int)glide_height - vy;
			const int gy1 = (int)glide_height - y1;
			gl_y = gy0 < gy1 ? gy0 : gy1;
		} else {
			gl_y = vy < y1 ? vy : y1;
		}
		const int gl_h = vh < 0 ? -vh : vh;
		glViewport(gl_x, gl_y, gl_w > 0 ? gl_w : 1, gl_h > 0 ? gl_h : 1);
		const float sx = vw < 0 ? -1.0f : 1.0f;
		float sy = vh < 0 ? -1.0f : 1.0f;
		if (GlideStateOriginUpperLeft()) sy = -sy;
		glScalef(sx, sy, 1.0f);
		glDepthRange(GlideStateDepthRangeNear(), GlideStateDepthRangeFar());
	}
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glide_is_in_frame = true;
	return true;
}

static GLenum GlideMapBlendFactor(int gr, bool source_factor)
{
	/* Glide 3 GrAlphaBlendFnc_t values.  Color factors are relative to the
	 * opposite input: SRC_COLOR means destination color when used as the
	 * source factor, and source color when used as the destination factor. */
	switch (gr) {
	case 0x0: return GL_ZERO;
	case 0x1: return GL_SRC_ALPHA;
	case 0x2: return source_factor ? GL_DST_COLOR : GL_SRC_COLOR;
	case 0x3: return GL_DST_ALPHA;
	case 0x4: return GL_ONE;
	case 0x5: return GL_ONE_MINUS_SRC_ALPHA;
	case 0x6: return source_factor ? GL_ONE_MINUS_DST_COLOR
								   : GL_ONE_MINUS_SRC_COLOR;
	case 0x7: return GL_ONE_MINUS_DST_ALPHA;
	case 0xf: return source_factor ? GL_SRC_ALPHA_SATURATE : GL_ONE;
	default:  return GL_ONE;
	}
}

static GLenum GlideMapGLCmpFunc(int gr)
{
	/* GR_CMP_* */
	switch (gr) {
	case 0: return GL_NEVER;
	case 1: return GL_LESS;
	case 2: return GL_EQUAL;
	case 3: return GL_LEQUAL;
	case 4: return GL_GREATER;
	case 5: return GL_NOTEQUAL;
	case 6: return GL_GEQUAL;
	case 7: return GL_ALWAYS;
	default: return GL_LEQUAL;
	}
}

enum {
	kGlideCombineFunctionZero = 0,
	kGlideCombineFunctionLocal = 1,
	kGlideCombineFunctionLocalAlpha = 2,
	kGlideCombineFunctionScaleOther = 3,
	kGlideCombineFunctionScaleOtherAddLocal = 4,
	kGlideCombineFunctionScaleOtherAddLocalAlpha = 5,
	kGlideCombineFunctionScaleOtherMinusLocal = 6,
	kGlideCombineFunctionScaleOtherMinusLocalAddLocal = 7,
	kGlideCombineFunctionScaleOtherMinusLocalAddLocalAlpha = 8,
	kGlideCombineFactorLocal = 1,
	kGlideCombineFactorOne = 8,
	kGlideCombineLocalIterated = 0,
	kGlideCombineLocalConstant = 1,
	kGlideCombineOtherTexture = 1,
	kGlideWindowCoords = 0
};

static bool GlideAnyChromaEnabled(void)
{
	return GlideStateChromaMode() != 0 || GlideStateTexChromaMode() != 0;
}

static bool GlideColorCombineUsesTexture(void)
{
	if (GlideStateColorCombineOther() != kGlideCombineOtherTexture)
		return false;
	switch (GlideStateColorCombineFunction()) {
	case kGlideCombineFunctionScaleOther:
	case kGlideCombineFunctionScaleOtherAddLocal:
	case kGlideCombineFunctionScaleOtherAddLocalAlpha:
	case kGlideCombineFunctionScaleOtherMinusLocal:
	case kGlideCombineFunctionScaleOtherMinusLocalAddLocal:
	case kGlideCombineFunctionScaleOtherMinusLocalAddLocalAlpha:
		return true;
	default:
		return false;
	}
}

static bool GlideAlphaCombineUsesTexture(void)
{
	if (GlideStateAlphaCombineOther() != kGlideCombineOtherTexture)
		return false;
	switch (GlideStateAlphaCombineFunction()) {
	case kGlideCombineFunctionScaleOther:
	case kGlideCombineFunctionScaleOtherAddLocal:
	case kGlideCombineFunctionScaleOtherAddLocalAlpha:
	case kGlideCombineFunctionScaleOtherMinusLocal:
	case kGlideCombineFunctionScaleOtherMinusLocalAddLocal:
	case kGlideCombineFunctionScaleOtherMinusLocalAddLocalAlpha:
		return true;
	default:
		return false;
	}
}

static void GlideConfigureTextureEnvironment(bool color_uses_texture,
											 bool alpha_uses_texture)
{
	/*
	 * Glide has independent RGB and alpha combine units. GL_MODULATE couples
	 * them and is wrong for combinations such as texture RGB times constant
	 * RGB with texture alpha, or textured RGB with iterated alpha.
	 */
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB,
			 color_uses_texture ? GL_MODULATE : GL_REPLACE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB,
			 color_uses_texture ? GL_TEXTURE : GL_PRIMARY_COLOR);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
	if (color_uses_texture) {
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
	}

	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA,
			 alpha_uses_texture ? GL_MODULATE : GL_REPLACE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA,
			 alpha_uses_texture ? GL_TEXTURE : GL_PRIMARY_COLOR);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
	if (alpha_uses_texture) {
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
	}
}

static bool GlideConfigureAlphaControlledLighting(bool alpha_uses_texture)
{
	GfxGLExt &ext = gfx_gl_ext();
	if (!ext.ActiveTexture || !ext.MultiTexCoord4f)
		return false;

	/* Unit 0 selects local RGB from iterated or constant color using texture
	 * alpha. ARGB1555, the intended format for this Glide feature, makes the
	 * interpolation an exact high-bit selection. */
	ext.ActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, glide_gl_texture);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_CONSTANT);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB, GL_TEXTURE);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA);
	const GLfloat constant_color[4] = {
		GlideStateConstantR(), GlideStateConstantG(),
		GlideStateConstantB(), GlideStateConstantA()
	};
	glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, constant_color);
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PRIMARY_COLOR);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);

	/* Unit 1 applies the texture color to the selected local lighting. */
	ext.ActiveTexture(GL_TEXTURE1);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, glide_gl_texture);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PREVIOUS);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA,
			 alpha_uses_texture ? GL_MODULATE : GL_REPLACE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA,
			 alpha_uses_texture ? GL_TEXTURE : GL_PREVIOUS);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
	if (alpha_uses_texture) {
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_PREVIOUS);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
	}
	ext.ActiveTexture(GL_TEXTURE0);
	return true;
}

void GlideMetalApplyState(void)
{
	if (!SharedMetalDevice()) return;
	GfxGLExt &ext = gfx_gl_ext();
	glide_alpha_lighting_multitex = false;
	if (ext.ActiveTexture) {
		ext.ActiveTexture(GL_TEXTURE1);
		glDisable(GL_TEXTURE_2D);
		ext.ActiveTexture(GL_TEXTURE0);
	}
	const int cull = GlideStateCullMode();
	if (cull == 0) {
		glDisable(GL_CULL_FACE);
	} else {
		glEnable(GL_CULL_FACE);
		glCullFace(cull == 1 ? GL_FRONT : GL_BACK);
	}
	if (GlideStateDepthMode() != 0) {
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GlideMapGLCmpFunc(GlideStateDepthFunc() ? GlideStateDepthFunc() : 3));
		glDepthMask(GlideStateDepthMask() ? GL_TRUE : GL_FALSE);
		const float bias = GlideStateDepthBias();
		if (bias != 0.f) {
			glEnable(GL_POLYGON_OFFSET_FILL);
			/* Glide adds a signed integer bias in 16-bit depth-buffer units.
			 * The FBO is also 16-bit, and has no slope-scaled component. */
			glPolygonOffset(0.f, bias);
		} else {
			glDisable(GL_POLYGON_OFFSET_FILL);
		}
	} else {
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_POLYGON_OFFSET_FILL);
	}
	glEnable(GL_BLEND);
	glBlendFunc(GlideMapBlendFactor(GlideStateAlphaBlendSrc(), true),
				GlideMapBlendFactor(GlideStateAlphaBlendDst(), false));
	const bool color_uses_texture = GlideColorCombineUsesTexture();
	const bool alpha_uses_texture =
		GlideAlphaCombineUsesTexture() || GlideAnyChromaEnabled();
	if (glide_is_texture_enabled && glide_gl_texture &&
		(color_uses_texture || alpha_uses_texture)) {
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, glide_gl_texture);
		/* Glide keeps minification and magnification filters independent.  Only
		 * select an OpenGL mip filter when a complete, multi-level host texture
		 * is resident; selecting it for the old base-only upload made the texture
		 * incomplete and therefore black on conforming GL implementations. */
		const GLint min_filter = GlideStateTexFilterMin() ? GL_LINEAR : GL_NEAREST;
		const GLint mag_filter = GlideStateTexFilterMag() ? GL_LINEAR : GL_NEAREST;
		if (GlideStateTexMipMapMode() != 0 && glide_texture_mip_levels > 1)
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
				GlideStateTexFilterMin() ? GL_LINEAR_MIPMAP_NEAREST :
					GL_NEAREST_MIPMAP_NEAREST);
		else
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
#ifdef GL_TEXTURE_LOD_BIAS
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS,
						GlideStateTexLodBias());
#endif
		if (GlideStateAlphaControlsLighting() &&
			GlideStateColorCombineFunction() == kGlideCombineFunctionScaleOther)
			glide_alpha_lighting_multitex =
				GlideConfigureAlphaControlledLighting(alpha_uses_texture);
		if (!glide_alpha_lighting_multitex)
			GlideConfigureTextureEnvironment(color_uses_texture,
										 alpha_uses_texture);
	} else {
		glDisable(GL_TEXTURE_2D);
	}
	if (GlideAnyChromaEnabled()) {
		/* Chroma-keyed texels are decoded with alpha zero; reject them before
		 * blending/depth writes, matching the Glide chroma test. */
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.f);
	} else if (GlideStateAlphaTestEnabled()) {
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GlideMapGLCmpFunc(GlideStateAlphaTestFunc()),
					GlideStateAlphaTestRef());
	} else {
		glDisable(GL_ALPHA_TEST);
	}
	glColorMask(GlideStateColorMaskR() ? GL_TRUE : GL_FALSE,
				GlideStateColorMaskG() ? GL_TRUE : GL_FALSE,
				GlideStateColorMaskB() ? GL_TRUE : GL_FALSE,
				GlideStateColorMaskA() ? GL_TRUE : GL_FALSE);
	/* Clip window -> scissor in window coords (origin upper-left). */
	{
		const int minx = GlideStateClipMinX();
		const int miny = GlideStateClipMinY();
		const int maxx = GlideStateClipMaxX();
		const int maxy = GlideStateClipMaxY();
		const int w = maxx - minx;
		const int h = maxy - miny;
		if (w > 0 && h > 0 && (minx > 0 || miny > 0 ||
			maxx < (int)glide_width || maxy < (int)glide_height)) {
			glEnable(GL_SCISSOR_TEST);
			/* GL scissor origin is lower-left if we use raw GL without flip;
			 * our FBO ortho is upper-left y-down, but scissor is still bottom-left
			 * in window coordinates of the FBO. Convert. */
			const int gl_y = (int)glide_height - miny - h;
			glScissor(minx, gl_y > 0 ? gl_y : 0, w, h);
		} else {
			glDisable(GL_SCISSOR_TEST);
		}
	}
	if (GlideStateFogMode() != 0) {
		glEnable(GL_FOG);
		glFogi(GL_FOG_MODE, GL_LINEAR);
		if (gfx_gl_ext().FogCoordf) {
			/* Feed the table-derived Glide fog amount as an explicit coordinate.
			 * OpenGL linear fog then uses 1-coordinate as the incoming-color
			 * weight, matching Glide's f*fog + (1-f)*incoming equation. */
			glFogi(GL_FOG_COORDINATE_SOURCE, GL_FOG_COORDINATE);
			glFogf(GL_FOG_START, 0.0f);
			glFogf(GL_FOG_END, 1.0f);
		}
		const uint32_t fc = GlideStateFogColor();
		const float col[4] = {
			((fc >> 16) & 0xff) / 255.f,
			((fc >> 8) & 0xff) / 255.f,
			(fc & 0xff) / 255.f,
			1.f
		};
		glFogfv(GL_FOG_COLOR, col);
	} else {
		glDisable(GL_FOG);
	}
	glDisable(GL_LIGHTING);
}

void GlideMetalSetClipWindow(int minx, int miny, int maxx, int maxy)
{
	/* State already stored; re-apply if in frame. */
	(void)minx; (void)miny; (void)maxx; (void)maxy;
	if (glide_is_in_frame) GlideMetalApplyState();
}

void GlideMetalSetColorMask(int r, int g, int b, int a)
{
	if (!SharedMetalDevice()) return;
	glColorMask(r ? GL_TRUE : GL_FALSE, g ? GL_TRUE : GL_FALSE,
				b ? GL_TRUE : GL_FALSE, a ? GL_TRUE : GL_FALSE);
}

void GlideMetalSetAlphaTest(int enabled, int func, float ref)
{
	if (!SharedMetalDevice()) return;
	if (GlideAnyChromaEnabled()) {
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.f);
	} else if (enabled) {
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GlideMapGLCmpFunc(func), ref);
	} else {
		glDisable(GL_ALPHA_TEST);
	}
}

void GlideMetalSetFog(int mode, uint32_t color)
{
	(void)mode; (void)color;
	if (glide_is_in_frame) GlideMetalApplyState();
}

void GlideMetalSetDepthBias(float bias)
{
	(void)bias;
	if (glide_is_in_frame) GlideMetalApplyState();
}

void GlideMetalSplash(void)
{
	/* Classic Glide splash - solid color flash so titles that call it
	 * get a visible frame rather than a silent no-op. */
	if (!GlideBindDrawFBO()) return;
	glClearColor(0.1f, 0.15f, 0.35f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glide_has_context = true;
	/* Splash is a one-shot visible frame - Present is intentional. */
	GlideSubitOverlay(/*do_present=*/1);
}

void GlideMetalFlush(void)
{
	if (SharedMetalDevice())
		glFlush();
}

void GlideMetalFinish(void)
{
	if (SharedMetalDevice())
		glFinish();
}

int GlideMetalInit(void)
{
	glide_is_ready = SharedMetalDevice() != NULL;
	QD3D_INIT_LOG("GlideMetalInit: ready=%d", glide_is_ready ? 1 : 0);
	return glide_is_ready ? 0 : -1;
}

void GlideMetalShutdown(void)
{
	if (SharedMetalDevice())
		GlideReleaseTextureCache();
	else
		glide_texture_cache.clear();
	/* Always discard the cached FBO/renderbuffer names.  The host context can
	 * be destroyed before the guest closes its Glide window. */
	GlideReleaseOverlay();
	glide_is_ready = false;
	glide_is_in_frame = false;
}

int GlideMetalWinOpen(int width, int height, int origin_upper_left)
{
	(void)origin_upper_left;
	if (width <= 0 || height <= 0) return -1;
	if (!SharedMetalDevice()) {
		if (GlideMetalInit() != 0) return -1;
	}
	if (!GlideEnsureOverlay((uint32_t)width, (uint32_t)height))
		return -1;
	if (!GlideBindDrawFBO())
		return -1;
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	GfxGLExt &ext = gfx_gl_ext();
	ext.BindFramebuffer(GL_FRAMEBUFFER, 0);
	glide_is_in_frame = false;
	glide_has_context = false;

	/* Glide allocates only a private producer target. The presentation session
	 * has already selected the canonical 16-bit QuickDraw surface; publication
	 * below resolves this completed target into that driver-owned screen. */
	glide_has_context = true;
	GlideSubitOverlay(/*do_present=*/1);

	QD3D_INIT_LOG("GlideMetalWinOpen: %dx%d overlay=%u (clear present; free-run VBL)",
				  width, height, (unsigned)glide_color_tex);
	return 0;
}

void GlideMetalWinClose(void)
{
	GlideReleaseOverlay();
	glide_is_in_frame = false;
	glide_has_context = false;
}

/* Host-endian float load from guest big-endian PPC memory. */
static float GlideLoadF32(const uint8_t *p)
{
	union { uint32_t u; float f; } v;
	v.u = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		  ((uint32_t)p[2] << 8) | (uint32_t)p[3];
	return v.f;
}

static uint32_t GlideLoadU32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		   ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/*
 * Glide's snapped window coordinates carry a large bias which the hardware
 * removes before rasterization. Unreal Tournament submits the biased values
 * directly (about 786432 plus the actual screen coordinate); treating them as
 * literal OpenGL coordinates puts every primitive outside the viewport.
 */
static float GlideUnsnapCoord(float value)
{
	const float snap_bias = 786432.0f; /* 3 << 18 */
	return value >= snap_bias * 0.5f ? value - snap_bias : value;
}

static int GlideLayoutOff(int standard_param, int d2_alias)
{
	int o = GlideStateVertexOffset(standard_param);
	if (o >= 0) return o;
	if (d2_alias >= 0)
		return GlideStateVertexOffset(d2_alias);
	return -1;
}

/*
 * Convert Glide's 16-bit window-depth convention to the object Z expected by
 * the orthographic projection installed in GlideBindDrawFBO.
 *
 * Real Glide does not rasterize the dummy GrVertex.z member. Z buffering uses
 * ooz directly in the 0..65535 depth range, while W buffering uses
 * 65535 * (1 - oow). The latter is the path used by Unreal Tournament. With
 * glOrtho(..., -1, 1), object Z maps to window depth as (1 - z) / 2.
 */
static float GlideVertexObjectZ(const uint8_t *base)
{
	const int mode = GlideStateDepthMode();
	float depth = 0.f;

	if (mode == 2 || mode == 4) {
		/* GR_PARAM_Q is the FBI reciprocal-W. Glide 2 has no layout API, so
		 * its fixed vertex-level oow is also exposed through Q0 by our seed. */
		const int q_off = GlideLayoutOff(0x04, 0x50);
		float oow = q_off >= 0 ? GlideLoadF32(base + q_off) : 1.f;
		if (!std::isfinite(oow))
			oow = 1.f;
		depth = 1.f - oow;
	} else if (mode == 1 || mode == 3) {
		/* For the fixed Glide 2 layout GR_PARAM_Z is seeded to ooz, not to
		 * the documented-but-ignored GrVertex.z member. */
		const int z_off = GlideLayoutOff(0x02, -1);
		const float ooz = z_off >= 0 ? GlideLoadF32(base + z_off) : 0.f;
		depth = std::isfinite(ooz) ? ooz * (1.f / 65535.f) : 0.f;
	}

	if (depth < 0.f) depth = 0.f;
	else if (depth > 1.f) depth = 1.f;
	return 1.f - 2.f * depth;
}

static void GlideSelectLocalRGB(int local,
								float iterated_r,
								float iterated_g,
								float iterated_b,
								float *r,
								float *g,
								float *b)
{
	if (local == kGlideCombineLocalConstant) {
		*r = GlideStateConstantR();
		*g = GlideStateConstantG();
		*b = GlideStateConstantB();
	} else if (local == kGlideCombineLocalIterated) {
		*r = iterated_r;
		*g = iterated_g;
		*b = iterated_b;
	} else {
		*r = *g = *b = 0.f;
	}
}

static float GlideSelectLocalAlpha(int local, float iterated_a)
{
	return local == kGlideCombineLocalConstant
		? GlideStateConstantA() : iterated_a;
}

static float GlideTextureAlphaFactor(float iterated_a)
{
	const float local_a =
		GlideSelectLocalAlpha(GlideStateAlphaCombineLocal(), iterated_a);
	switch (GlideStateAlphaCombineFactor()) {
	case 0: /* GR_COMBINE_FACTOR_ZERO */
		return 0.f;
	case 1: /* GR_COMBINE_FACTOR_LOCAL */
	case 3: /* GR_COMBINE_FACTOR_LOCAL_ALPHA */
		return local_a;
	case 8: /* GR_COMBINE_FACTOR_ONE */
		return 1.f;
	case 9: /* GR_COMBINE_FACTOR_ONE_MINUS_LOCAL */
	case 11: /* GR_COMBINE_FACTOR_ONE_MINUS_LOCAL_ALPHA */
		return 1.f - local_a;
	default:
		/*
		 * Texture-derived factors need a richer expression than primary
		 * alpha modulation. Preserve the texture instead of suppressing it
		 * with an unrelated vertex alpha.
		 */
		return 1.f;
	}
}

static void GlideEmitVertex(const uint8_t *base)
{
	/* Defaults: classic GrVertex. */
	float x = GlideLoadF32(base + 0);
	float y = GlideLoadF32(base + 4);
	float r = 1.f, g = 1.f, b = 1.f, a = 1.f;

	/* Official GR_PARAM_XY=1. */
	int xy_off = GlideLayoutOff(0x01, -1);
	if (xy_off >= 0) {
		x = GlideLoadF32(base + xy_off);
		y = GlideLoadF32(base + xy_off + 4);
	}
	const bool window_coords = GlideStateCoordSystem() == kGlideWindowCoords;
	float clip_w = 1.0f;
	if (window_coords) {
		x = GlideUnsnapCoord(x);
		y = GlideUnsnapCoord(y);
	} else {
		const int w_off = GlideLayoutOff(0x03, -1); /* GR_PARAM_W */
		if (w_off >= 0)
			clip_w = GlideLoadF32(base + w_off);
		if (!std::isfinite(clip_w) || std::fabs(clip_w) < 1.0e-20f)
			clip_w = 1.0f;
	}

	/* Glide 3 layout tokens are grouped by attribute: RGB=0x20,
	 * PARGB=0x30.  Keep the small Glide-2-compatible IDs as fallbacks.
	 * In particular, 0x50 is Q0 -- treating it as PARGB converts the bits of
	 * reciprocal-W into an almost-black vertex color. */
	int rgb_off = GlideLayoutOff(0x20, 0x07);
	int pargb_off = GlideLayoutOff(0x30, 0x08);
	bool rgb_is_float = false;
	bool alpha_is_float = false;
	if (rgb_off >= 0) {
		r = GlideLoadF32(base + rgb_off);
		g = GlideLoadF32(base + rgb_off + 4);
		b = GlideLoadF32(base + rgb_off + 8);
		rgb_is_float = true;
	} else if (pargb_off >= 0) {
		const uint32_t c = GlideLoadU32(base + pargb_off);
		a = ((c >> 24) & 0xff) / 255.f;
		r = ((c >> 16) & 0xff) / 255.f;
		g = ((c >> 8) & 0xff) / 255.f;
		b = (c & 0xff) / 255.f;
	}

	int a_off = GlideLayoutOff(0x10, 0x06);
	if (a_off >= 0 && pargb_off < 0) {
		a = GlideLoadF32(base + a_off);
		alpha_is_float = true;
	}

	/* Window-coordinate float iterators use Glide's 0..255 native range;
	 * clip-coordinate colors are already normalized to 0..1.  Packed PARGB
	 * was normalized while unpacking in either coordinate system. */
	if (rgb_is_float && window_coords) {
		r *= 1.f / 255.f;
		g *= 1.f / 255.f;
		b *= 1.f / 255.f;
	}
	if (alpha_is_float && window_coords)
		a *= 1.f / 255.f;
	if (r < 0.f) r = 0.f; else if (r > 1.f) r = 1.f;
	if (g < 0.f) g = 0.f; else if (g > 1.f) g = 1.f;
	if (b < 0.f) b = 0.f; else if (b > 1.f) b = 1.f;
	if (a < 0.f) a = 0.f; else if (a > 1.f) a = 1.f;

	const float iterated_r = r;
	const float iterated_g = g;
	const float iterated_b = b;
	const float iterated_a = a;

	/* Implement the two color-combine paths used by Diablo II:
	 *   LOCAL                         -> solid iterated/constant color
	 *   SCALE_OTHER(texture, LOCAL)  -> texture modulated by local color
	 * SCALE_OTHER(texture, ONE) uses a white modulation color. */
	const int color_func = GlideStateColorCombineFunction();
	if (color_func == kGlideCombineFunctionZero) {
		r = g = b = 0.f;
	} else if (color_func == kGlideCombineFunctionLocal) {
		GlideSelectLocalRGB(GlideStateColorCombineLocal(),
						   iterated_r, iterated_g, iterated_b, &r, &g, &b);
	} else if (color_func == kGlideCombineFunctionLocalAlpha) {
		const float local_a = GlideStateColorCombineLocal() ==
			kGlideCombineLocalConstant ? GlideStateConstantA() : iterated_a;
		r = g = b = local_a;
	} else if (GlideColorCombineUsesTexture()) {
		if (GlideStateAlphaControlsLighting()) {
			r = iterated_r;
			g = iterated_g;
			b = iterated_b;
		} else if (GlideStateColorCombineFactor() == kGlideCombineFactorOne) {
			r = g = b = 1.f;
		} else {
			GlideSelectLocalRGB(GlideStateColorCombineLocal(),
							   iterated_r, iterated_g, iterated_b, &r, &g, &b);
		}
	}

	if (GlideStateColorCombineInvert() && !GlideColorCombineUsesTexture()) {
		r = 1.f - r;
		g = 1.f - g;
		b = 1.f - b;
	}

	/* Preserve texture alpha as a chroma discard mask. Chroma-keying happens
	 * before Glide's alpha combine stage, so a ZERO alpha combine must not
	 * erase that mask in the fixed-function emulation. */
	const int alpha_func = GlideStateAlphaCombineFunction();
	if (alpha_func == kGlideCombineFunctionZero) {
		a = GlideAnyChromaEnabled() && GlideColorCombineUsesTexture() ? 1.f : 0.f;
	} else if (alpha_func == kGlideCombineFunctionLocal) {
		a = GlideSelectLocalAlpha(GlideStateAlphaCombineLocal(), iterated_a);
	} else if (GlideAlphaCombineUsesTexture()) {
		/*
		 * The texture environment supplies the "other" alpha. The primary
		 * alpha contains only Glide's scale factor. In particular,
		 * guAlphaSource(TEXTURE_ALPHA) uses factor ONE; multiplying by the
		 * unused GrVertex.a field made Unreal Tournament reject every
		 * fragment in its alpha test.
		 */
		a = GlideTextureAlphaFactor(iterated_a);
	} else {
		a = iterated_a;
	}
	if (GlideStateAlphaCombineInvert() &&
		!(GlideAnyChromaEnabled() && GlideColorCombineUsesTexture()))
		a = 1.f - a;

	/* ST0 contains s/q,t/q in Glide texel units and Q0 contains q.  OpenGL's
	 * projective coordinate division happens after interpolation. Window
	 * coordinates use a virtual 256-unit long axis regardless of mip size;
	 * clip coordinates are already normalized to [0,1]. */
	const int q_off = GlideLayoutOff(0x50, 0x04);
	float q = q_off >= 0 ? GlideLoadF32(base + q_off) : 1.f;
	if (!std::isfinite(q) || std::fabs(q) < 1.0e-20f)
		q = 1.f;

	int st_off = GlideLayoutOff(0x40, 0x09);
	if (st_off >= 0) {
		const float s = GlideLoadF32(base + st_off);
		const float t = GlideLoadF32(base + st_off + 4);
		float gl_s = s;
		float gl_t = t;
		if (GlideStateCoordSystem() == kGlideWindowCoords) {
			gl_s /= glide_texture_s_extent;
			gl_t /= glide_texture_t_extent;
		}
		GfxGLExt &ext = gfx_gl_ext();
		if (glide_alpha_lighting_multitex && ext.MultiTexCoord4f) {
			ext.MultiTexCoord4f(GL_TEXTURE0, gl_s, gl_t, 0.f, q);
			ext.MultiTexCoord4f(GL_TEXTURE1, gl_s, gl_t, 0.f, q);
		} else {
			glTexCoord4f(gl_s, gl_t, 0.f, q);
		}
	}
	if (GlideStateFogMode() != 0 && gfx_gl_ext().FogCoordf)
		gfx_gl_ext().FogCoordf(GlideStateFogFactor(
			window_coords ? q : q / clip_w));

	glColor4f(r, g, b, a);
	if (window_coords) {
		glVertex3f(x, y, GlideVertexObjectZ(base));
	} else {
		const int z_off = GlideLayoutOff(0x02, -1); /* GR_PARAM_Z */
		float z = z_off >= 0 ? GlideLoadF32(base + z_off) : 0.0f;
		if (!std::isfinite(z)) z = 0.0f;
		glVertex4f(x, y, z, clip_w);
	}
}

static GLenum GlideMapGlideToGLPrim(uint32_t mode)
{
	/* GrPrimitive_t */
	switch (mode) {
	case 0: return GL_POINTS;
	case 1: return GL_LINE_STRIP;
	case 2: return GL_LINES;
	case 3: return GL_POLYGON;
	case 4: return GL_TRIANGLE_STRIP;
	case 5: return GL_TRIANGLE_FAN;
	case 6: return GL_TRIANGLES;
	case 7: return GL_TRIANGLE_STRIP; /* CONTINUE */
	case 8: return GL_TRIANGLE_FAN;
	default: return GL_TRIANGLES;
	}
}

void GlideMetalDrawPoint(const void *a)
{
	if (!a) return;
	if (!GlideBindDrawFBO()) return;
	GlideMetalApplyState();
	glBegin(GL_POINTS);
	GlideEmitVertex((const uint8_t *)a);
	glEnd();
	glide_has_context = true;
}

void GlideMetalDrawLine(const void *a, const void *b)
{
	if (!a || !b) return;
	if (!GlideBindDrawFBO()) return;
	GlideMetalApplyState();
	glBegin(GL_LINES);
	GlideEmitVertex((const uint8_t *)a);
	GlideEmitVertex((const uint8_t *)b);
	glEnd();
	glide_has_context = true;
}

void GlideMetalDrawTriangle(const void *a, const void *b, const void *c)
{
	if (!a || !b || !c) return;
	if (!GlideBindDrawFBO()) return;
	GlideMetalApplyState();
	glBegin(GL_TRIANGLES);
	GlideEmitVertex((const uint8_t *)a);
	GlideEmitVertex((const uint8_t *)b);
	GlideEmitVertex((const uint8_t *)c);
	glEnd();
	glide_has_context = true;
}

void GlideMetalDrawPolygon(int nverts, const void *const *ptrs)
{
	if (!ptrs || nverts < 3) return;
	if (!GlideBindDrawFBO()) return;
	GlideMetalApplyState();
	glBegin(GL_TRIANGLE_FAN);
	for (int i = 0; i < nverts; i++) {
		if (ptrs[i])
			GlideEmitVertex((const uint8_t *)ptrs[i]);
	}
	glEnd();
	glide_has_context = true;
}

void GlideMetalDrawPolygonIndexed(int nverts, const void *indices,
								  const void *verts, uint32_t stride)
{
	if (!indices || !verts || nverts < 3) return;
	if (!GlideBindDrawFBO()) return;
	if (stride == 0) stride = (uint32_t)GlideStateVertexStride();
	if (stride < 8) stride = 8;
	GlideMetalApplyState();
	const uint8_t *ilist = (const uint8_t *)indices;
	const uint8_t *base = (const uint8_t *)verts;
	glBegin(GL_TRIANGLE_FAN);
	for (int i = 0; i < nverts; i++) {
		const uint32_t index = GlideLoadU32(ilist + (size_t)i * 4);
		/* A corrupt index must not turn a guest draw into an unbounded host
		 * pointer walk.  Glide 2 meshes use 16-bit-scale vertex pools. */
		if (index <= 0xffffu)
			GlideEmitVertex(base + (size_t)index * stride);
	}
	glEnd();
	glide_has_context = true;
}

void GlideMetalDrawPolygonContiguous(int nverts, const void *verts, uint32_t stride)
{
	if (!verts || nverts < 3) return;
	if (!GlideBindDrawFBO()) return;
	if (stride == 0) stride = (uint32_t)GlideStateVertexStride();
	if (stride < 8) stride = 8;
	GlideMetalApplyState();
	const uint8_t *base = (const uint8_t *)verts;
	glBegin(GL_TRIANGLE_FAN);
	for (int i = 0; i < nverts; i++)
		GlideEmitVertex(base + (size_t)i * stride);
	glEnd();
	glide_has_context = true;
}

void GlideMetalDrawVertexArray(uint32_t mode, uint32_t count, const void *const *ptrs)
{
	if (!ptrs || count == 0) return;
	if (!GlideBindDrawFBO()) return;
	GlideMetalApplyState();
	glBegin(GlideMapGlideToGLPrim(mode));
	for (uint32_t i = 0; i < count; i++) {
		if (ptrs[i])
			GlideEmitVertex((const uint8_t *)ptrs[i]);
	}
	glEnd();
	glide_has_context = true;
}

void GlideMetalDrawVertexArrayContiguous(uint32_t mode, uint32_t count,
									  const void *vertices, uint32_t stride)
{
	if (!vertices || count == 0) return;
	if (!GlideBindDrawFBO()) return;
	if (stride == 0)
		stride = (uint32_t)GlideStateVertexStride();
	if (stride < 8) stride = 8;
	GlideMetalApplyState();
	const uint8_t *base = (const uint8_t *)vertices;
	glBegin(GlideMapGlideToGLPrim(mode));
	for (uint32_t i = 0; i < count; i++)
		GlideEmitVertex(base + (size_t)i * stride);
	glEnd();
	glide_has_context = true;
}

/*
 * Publish the Glide color buffer into the compositor overlay mailbox.
 * do_present=0: SubmitFrame only - VideoVBL/free-run owns Present.
 * do_present=1: also present immediately (WinOpen first black frame, LFB
 * unlock, swap), but without running the guest VBL callback chain inside
 * NATIVE_GLIDE_DISPATCH.
 */
static void GlideSubitOverlay(int do_present)
{
	if (!glide_color_tex || !SharedMetalDevice()) return;
	/* Never push an empty/clear-only frame as opaque overlay. */
	if (!glide_has_context) {
		MetalCompositorSubmitFrame_ClearCachedOverlay();
		if (do_present)
			MetalCompositorPresentWithoutVBL();
		return;
	}

	/* Every GlideSubitOverlay call sits at a frame boundary (WinOpen clear,
	 * grBufferSwap, LFB unlock/present, splash), so the draw buffer now holds
	 * a COMPLETE frame. Copy it to the front texture and present from THAT, so
	 * the free-run VBL present never samples a half-drawn or freshly-cleared
	 * (black) draw buffer between the clear and the draw calls. */
	GlidePublishFrontFromDraw();
	if (!glide_front_valid) {
		/* Copy could not run (missing FBO); fall back to the draw texture. */
		glide_front_tex = glide_color_tex;
		glide_front_valid = true;
	}

	(void)dmc_set_active_owner(kDMCOwnerGlide);
	const DMCModeSnapshot *snap = dmc_current_snapshot();
	float dst_w = (float)glide_width;
	float dst_h = (float)glide_height;
	if (snap && snap->width > 0 && snap->height > 0 &&
		(snap->width != glide_width || snap->height != glide_height)) {
		dst_w = (float)snap->width;
		dst_h = (float)snap->height;
		static uint32_t s_mismatch_log = 0;
		if (s_mismatch_log++ < 8)
			QD3D_INIT_LOG("Glide submit: DMC %ux%u != glide %ux%u - fill-scale fallback",
						  snap->width, snap->height, glide_width, glide_height);
	}

	CompositeLayer layer = {};
	layer.source = (void *)(uintptr_t)glide_front_tex;
	layer.src_origin_x = 0;
	layer.src_origin_y = 0;
	layer.src_size_w = glide_width;
	layer.src_size_h = glide_height;
	layer.dst_origin_x = 0.f;
	layer.dst_origin_y = 0.f;
	layer.dst_size_w = dst_w;
	layer.dst_size_h = dst_h;
	layer.slot = kLayerSlotOverlay;
	layer.blend = kBlendOpaque;
	layer.alpha = 1.f;

	FrameDescriptor desc = {};
	desc.layers = &layer;
	desc.layer_count = 1;
	desc.generation = snap ? snap->generation : 0;
	desc.vbl_tick_target_usec = 0;
	(void)MetalCompositorSubmitFrame(&desc);
	if (do_present)
		MetalCompositorPresentWithoutVBL();
}

void GlideMetalPublishOverlay(int do_present)
{
	GlideSubitOverlay(do_present ? 1 : 0);
}

void GlideMetalBufferSwap(int swap_interval)
{
	if (!glide_color_tex || !SharedMetalDevice()) return;

	GfxGLExt &ext = gfx_gl_ext();
	glFlush();
	if (ext.fbo)
		ext.BindFramebuffer(GL_FRAMEBUFFER, 0);
	glide_is_in_frame = false;

	/* grBufferSwap(1) blocks for the next retrace on real Glide hardware.
	 * Pending-count emulation cannot provide that contract on a single-threaded
	 * guest; pace the completed frame on the host deadline lane, then report the
	 * synchronous swap complete. Diablo II uses interval 1. */
	int intervals = swap_interval > 0 ? swap_interval : 0;
	if (intervals > 4) intervals = 4;
	for (int i = 0; i < intervals; i++) {
		(void)MetalCompositorSync3DFramePacingForEngine(
			kGfxFramePacingEngineGlide);
	}

	/*
	 * Real grBufferSwap makes the back buffer visible. Match the proven
	 * movie LFB path: SubmitFrame + Present here. free-run VIA still keeps
	 * Mac VBL timebase alive between swaps; it is not a substitute for the
	 * swap Present itself.
	 */
	GlideSubitOverlay(/*do_present=*/1);
}

void GlideMetalBufferClear(uint32_t color, uint32_t alpha, uint32_t depth)
{
	if (!GlideBindDrawFBO()) return;
	/* grSstWinOpen's GrColorFormat controls every GrColor_t argument,
	 * including BufferClear. Diablo II opens RGBA, not the ARGB default. */
	uint8_t clear_r = 0, clear_g = 0, clear_b = 0;
	GlideUnpackColorRGB(color, GlideStateColorFormat(),
						&clear_r, &clear_g, &clear_b);
	const float r = clear_r / 255.f;
	const float g = clear_g / 255.f;
	const float b = clear_b / 255.f;
	const float a = (alpha & 0xffu) / 255.f;
	glClearColor(r, g, b, a);
	/* grBufferClear's depth is the exact unsigned 16-bit buffer value for
	 * both Z and W modes. Do not replace it with a fixed far clear. */
	glClearDepth((GLclampd)(depth & 0xffffu) * (1.0 / 65535.0));
	/* OpenGL masks depth clears when depth writes are disabled; Glide's
	 * explicit buffer clear is independent of the current grDepthMask. */
	glDepthMask(GL_TRUE);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDepthMask(GlideStateDepthMask() ? GL_TRUE : GL_FALSE);
	/*
	 * Real Glide only clears the back buffer. Display is grBufferSwap /
	 * LFB unlock. Mark content so the next swap is allowed to present.
	 */
	glide_has_context = true;
	{
		GfxGLExt &ext = gfx_gl_ext();
		if (ext.fbo)
			ext.BindFramebuffer(GL_FRAMEBUFFER, 0);
		glide_is_in_frame = false;
	}
}

void GlideMetalMarkContent(void)
{
	glide_has_context = true;
}

void GlideMetalUploadLfbAndPresent(const uint8_t *bgra, int w, int h, int pitch,
								int present)
{
	if (!bgra || w <= 0 || h <= 0 || !glide_color_tex || !SharedMetalDevice())
		return;
	if ((uint32_t)w > glide_width) w = (int)glide_width;
	if ((uint32_t)h > glide_height) h = (int)glide_height;

	/* Upload into the current write color texture (BGRA8). Unbind FBO so
	 * the texture is not both attachment and upload target. */
	GfxGLExt &ext = gfx_gl_ext();
	if (ext.fbo)
		ext.BindFramebuffer(GL_FRAMEBUFFER, 0);
	glide_is_in_frame = false;

	glBindTexture(GL_TEXTURE_2D, glide_color_tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	if (pitch == w * 4) {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
						GL_BGRA, GL_UNSIGNED_BYTE, bgra);
	} else {
		/* Row-by-row if pitch differs (should not for our converter). */
		for (int y = 0; y < h; y++) {
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, w, 1,
							GL_BGRA, GL_UNSIGNED_BYTE,
							bgra + (size_t)y * (size_t)pitch);
		}
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	glFlush();

	glide_has_context = true;
	/* LFB unlock path: Present so movies keep working without waiting VBL. */
	if (present)
		GlideSubitOverlay(/*do_present=*/1);
	else
		GlideSubitOverlay(/*do_present=*/0);
}

/* ---- Textures ---------------------------------------------------------- */

static void GlideUnpackColorRGB(uint32_t color, int color_format,
								uint8_t *out_r, uint8_t *out_g,
								uint8_t *out_b)
{
	uint8_t r = 0, g = 0, b = 0;
	switch (color_format) {
	case GR_COLORFORMAT_ABGR:
		r = (uint8_t)(color & 0xffu);
		g = (uint8_t)((color >> 8) & 0xffu);
		b = (uint8_t)((color >> 16) & 0xffu);
		break;
	case GR_COLORFORMAT_RGBA:
		r = (uint8_t)((color >> 24) & 0xffu);
		g = (uint8_t)((color >> 16) & 0xffu);
		b = (uint8_t)((color >> 8) & 0xffu);
		break;
	case GR_COLORFORMAT_BGRA:
		b = (uint8_t)((color >> 24) & 0xffu);
		g = (uint8_t)((color >> 16) & 0xffu);
		r = (uint8_t)((color >> 8) & 0xffu);
		break;
	case GR_COLORFORMAT_ARGB:
	default:
		r = (uint8_t)((color >> 16) & 0xffu);
		g = (uint8_t)((color >> 8) & 0xffu);
		b = (uint8_t)(color & 0xffu);
		break;
	}
	*out_r = r;
	*out_g = g;
	*out_b = b;
}

static void GlideDecodeTextureLevel(const uint8_t *src, int w, int h, int format,
								   int chroma_mode, uint32_t chroma_value,
								   int color_format,
								   std::vector<uint8_t> &rgba)
{
	rgba.resize((size_t)w * (size_t)h * 4u);
	const uint32_t *pal = GlideStateTexPalette();
	const int bpp = GlideTexBpp(format);
	uint8_t chroma_r = 0, chroma_g = 0, chroma_b = 0;
	GlideUnpackColorRGB(chroma_value, color_format,
						&chroma_r, &chroma_g, &chroma_b);
	uint8_t range0_r = 0, range0_g = 0, range0_b = 0;
	uint8_t range1_r = 0, range1_g = 0, range1_b = 0;
	uint8_t tex0_r = 0, tex0_g = 0, tex0_b = 0;
	uint8_t tex1_r = 0, tex1_g = 0, tex1_b = 0;
	const bool global_range = chroma_mode && GlideStateChromaRangeMode();
	const bool texture_range = GlideStateTexChromaMode() != 0;
	if (global_range) {
		GlideUnpackColorRGB(GlideStateChromaRangeMin(), color_format,
			&range0_r, &range0_g, &range0_b);
		GlideUnpackColorRGB(GlideStateChromaRangeMax(), color_format,
			&range1_r, &range1_g, &range1_b);
	}
	if (texture_range) {
		GlideUnpackColorRGB(GlideStateTexChromaRangeMin(), color_format,
			&tex0_r, &tex0_g, &tex0_b);
		GlideUnpackColorRGB(GlideStateTexChromaRangeMax(), color_format,
			&tex1_r, &tex1_g, &tex1_b);
	}
	const uint8_t range_lo_r = range0_r < range1_r ? range0_r : range1_r;
	const uint8_t range_hi_r = range0_r > range1_r ? range0_r : range1_r;
	const uint8_t range_lo_g = range0_g < range1_g ? range0_g : range1_g;
	const uint8_t range_hi_g = range0_g > range1_g ? range0_g : range1_g;
	const uint8_t range_lo_b = range0_b < range1_b ? range0_b : range1_b;
	const uint8_t range_hi_b = range0_b > range1_b ? range0_b : range1_b;
	const uint8_t tex_lo_r = tex0_r < tex1_r ? tex0_r : tex1_r;
	const uint8_t tex_hi_r = tex0_r > tex1_r ? tex0_r : tex1_r;
	const uint8_t tex_lo_g = tex0_g < tex1_g ? tex0_g : tex1_g;
	const uint8_t tex_hi_g = tex0_g > tex1_g ? tex0_g : tex1_g;
	const uint8_t tex_lo_b = tex0_b < tex1_b ? tex0_b : tex1_b;
	const uint8_t tex_hi_b = tex0_b > tex1_b ? tex0_b : tex1_b;
	uint8_t *d = vec_data(rgba);
	const uint8_t *s = src;
	for (int i = 0; i < w * h; i++) {
		uint8_t R = 255, G = 255, B = 255, A = 255;
		if (bpp == 1) {
			const uint8_t p = *s++;
			if (format == 0x05) {
				/* P_8 expands a palette RGB value with opaque alpha. The
				 * standard palette word's high byte is not texture alpha. */
				const uint32_t c = pal ? pal[p] : 0xffffffffu;
				A = 255;
				R = (uint8_t)((c >> 16) & 0xff);
				G = (uint8_t)((c >> 8) & 0xff);
				B = (uint8_t)(c & 0xff);
			} else {
				/* intensity / alpha-ish */
				R = G = B = p;
				A = 255;
			}
		} else if (bpp == 2) {
			/* Guest big-endian 16-bit */
			const uint16_t p = (uint16_t)((s[0] << 8) | s[1]);
			s += 2;
			if (format == 0x08) {
				/* ARGB_8332 */
				A = (uint8_t)(p >> 8);
				const int r = (p >> 5) & 0x7;
				const int g = (p >> 2) & 0x7;
				const int b = p & 0x3;
				R = (uint8_t)((r * 255) / 7);
				G = (uint8_t)((g * 255) / 7);
				B = (uint8_t)((b * 255) / 3);
			} else if (format == 0x0a) {
				/* RGB_565 */
				const int r = (p >> 11) & 0x1f;
				const int g = (p >> 5) & 0x3f;
				const int b = p & 0x1f;
				R = (uint8_t)((r << 3) | (r >> 2));
				G = (uint8_t)((g << 2) | (g >> 4));
				B = (uint8_t)((b << 3) | (b >> 2));
			} else if (format == 0x0c) {
				/* ARGB_4444 */
				A = (uint8_t)(((p >> 12) & 0xf) * 17);
				R = (uint8_t)(((p >> 8) & 0xf) * 17);
				G = (uint8_t)(((p >> 4) & 0xf) * 17);
				B = (uint8_t)((p & 0xf) * 17);
			} else {
				/* ARGB_1555 default for 16bpp */
				A = (p & 0x8000) ? 255 : 0;
				const int r = (p >> 10) & 0x1f;
				const int g = (p >> 5) & 0x1f;
				const int b = p & 0x1f;
				R = (uint8_t)((r << 3) | (r >> 2));
				G = (uint8_t)((g << 3) | (g >> 2));
				B = (uint8_t)((b << 3) | (b >> 2));
			}
		} else {
			/* ARGB_8888 guest BE */
			A = s[0]; R = s[1]; G = s[2]; B = s[3];
			s += 4;
		}
		/* Glide chroma tests reject the fragment before blending/depth writes.
		 * The range endpoints are order-independent for each RGB component. */
		bool reject = chroma_mode && !global_range &&
			R == chroma_r && G == chroma_g && B == chroma_b;
		if (global_range)
			reject = R >= range_lo_r && R <= range_hi_r &&
				G >= range_lo_g && G <= range_hi_g &&
				B >= range_lo_b && B <= range_hi_b;
		if (texture_range)
			reject = reject || (R >= tex_lo_r && R <= tex_hi_r &&
				G >= tex_lo_g && G <= tex_hi_g &&
				B >= tex_lo_b && B <= tex_hi_b);
		if (reject)
			A = 0;
		d[0] = R; d[1] = G; d[2] = B; d[3] = A;
		d += 4;
	}
}

static void GlideUploadCachedTexture(GlideMetalTextureCacheEntry &entry,
										const uint8_t *src,
										uint32_t available,
										int chroma_mode,
										uint32_t chroma_value,
										int color_format)
{
	std::vector<uint8_t> rgba;
	glBindTexture(GL_TEXTURE_2D, entry.texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

	int uploaded = 0;
	const int step = entry.small_lod >= entry.large_lod ? 1 : -1;
	for (int lod = entry.large_lod;; lod += step) {
		const uint32_t offset = GlideTexLevelOffsetBytes(
			lod, entry.large_lod, entry.aspect_log2, entry.format);
		const uint32_t size = GlideTexLevelSizeBytes(
			lod, entry.aspect_log2, entry.format);
		if (offset == 0xffffffffu || offset > available ||
			size > available - offset)
			break;

		int width = 1, height = 1;
		GlideTexLodDims(lod, entry.aspect_log2, &width, &height);
		GlideDecodeTextureLevel(src + offset, width, height, entry.format,
							   chroma_mode, chroma_value, color_format, rgba);
		glTexImage2D(GL_TEXTURE_2D, uploaded, GL_RGBA, width, height, 0,
					 GL_RGBA, GL_UNSIGNED_BYTE, vec_data(rgba));
		uploaded++;
		if (lod == entry.small_lod) break;
	}

	/* Glide chains may intentionally stop before 1x1.  Limit the OpenGL
	 * texture to the levels actually supplied so it remains complete. */
	if (uploaded < 1) uploaded = 1;
#ifdef GL_TEXTURE_MAX_LEVEL
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, uploaded - 1);
#endif
	entry.mip_levels = uploaded;
	entry.chroma_mode = chroma_mode;
	entry.chroma_value = chroma_value;
	entry.color_format = color_format;
	entry.dirty = false;
}

void GlideMetalSetChromakey(void)
{
	const int chroma_mode = GlideStateChromaMode();
	const uint32_t chroma_value = GlideStateChromaValue();
	const int color_format = GlideStateColorFormat();
	GlideMetalTextureCacheEntry *bound = NULL;
	for (size_t ei = 0; ei < glide_texture_cache.size(); ei++) {
		GlideMetalTextureCacheEntry &entry = glide_texture_cache[ei];
		/* This entry point is also used for range/mode changes, whose values
		 * are intentionally not duplicated in each cache key. */
		entry.dirty = true;
		if (glide_gl_texture != 0 && entry.texture == glide_gl_texture)
			bound = &entry;
	}

	/* A key can change after grTexSource. Re-decode the resident texture now
	 * so the very next draw sees the new key even without another source call. */
	if (bound && bound->dirty && SharedMetalDevice()) {
		uint32_t avail = 0;
		const uint8_t *src = GlideStateTmuPtr(bound->address, &avail);
		if (src && avail >= bound->chain_size)
			GlideUploadCachedTexture(*bound, src, avail, chroma_mode,
										chroma_value, color_format);
		glide_texture_mip_levels = bound->mip_levels;
	}
	if (glide_is_in_frame)
		GlideMetalApplyState();
	else if (SharedMetalDevice())
		glBindTexture(GL_TEXTURE_2D, 0);
}

void GlideMetalTexDownloadLevel(uint32_t start_addr, int lod, int large_lod,
							 int aspect_log2, int format, const void *data,
							 uint32_t nbytes)
{
	if (!data) return;
	const uint32_t level_offset =
		GlideTexLevelOffsetBytes(lod, large_lod, aspect_log2, format);
	int w = 1, h = 1;
	GlideTexLodDims(lod, aspect_log2, &w, &h);
	uint32_t need = GlideTexLevelSizeBytes(lod, aspect_log2, format);
	if (nbytes && nbytes < need)
		need = nbytes;
	if (level_offset > 0xffffffffu - start_addr) {
		QD3D_INIT_LOG("GlideMetalTexDownloadLevel FAIL base=%08x offset=%u",
					  start_addr, level_offset);
		return;
	}
	const uint32_t level_addr = start_addr + level_offset;
	uint32_t old_avail = 0;
	const uint8_t *old_data = GlideStateTmuPtr(level_addr, &old_avail);
	const bool changed = old_data == NULL || old_avail < need ||
		std::memcmp(old_data, data, need) != 0;
	if (changed && !GlideStateTmuWrite(level_addr, data, need)) {
		QD3D_INIT_LOG("GlideMetalTexDownloadLevel FAIL addr=%08x need=%u",
					  level_addr, need);
		return;
	}
	/* Invalidate every resident base whose sampled level overlaps a real write. */
	const uint64_t write_begin = level_addr;
	const uint64_t write_end = write_begin + need;
	if (changed) {
		for (size_t ei = 0; ei < glide_texture_cache.size(); ei++) {
			GlideMetalTextureCacheEntry &entry = glide_texture_cache[ei];
			const uint64_t entry_begin = entry.address;
			const uint64_t entry_end = entry_begin + entry.chain_size;
			if (write_begin < entry_end && write_end > entry_begin)
				entry.dirty = true;
		}
	}
	static uint32_t s_dl_n = 0;
	if (++s_dl_n <= 12 || (s_dl_n & (s_dl_n - 1)) == 0)
		QD3D_INIT_LOG("GlideMetalTexDownloadLevel #%u base=%08x addr=%08x lod=%d %dx%d fmt=%d n=%u",
					  (unsigned)s_dl_n, start_addr, level_addr,
					  lod, w, h, format, need);
}

void GlideMetalTexSource(uint32_t start_addr, int even_odd, int small_lod,
					  int large_lod, int aspect_log2, int format)
{
	if (!SharedMetalDevice()) return;
	/* Use large LOD as base resolution for sampling. */
	int w = 1, h = 1;
	GlideTexLodDims(large_lod, aspect_log2, &w, &h);
	uint32_t avail = 0;
	const uint8_t *src = GlideStateTmuPtr(start_addr, &avail);
	const uint32_t need = GlideTexCalcMemRequired(
		small_lod, large_lod, aspect_log2, format);
	if (!src || avail < need || need == 0) {
		glide_is_texture_enabled = false;
		QD3D_INIT_LOG("GlideMetalTexSource FAIL addr=%08x need=%u avail=%u",
					  start_addr, need, avail);
		return;
	}

	const int chroma_mode = GlideStateChromaMode();
	const uint32_t chroma_value = GlideStateChromaValue();
	const int color_format = GlideStateColorFormat();
	GlideMetalTextureCacheEntry *cached = NULL;
	for (size_t ei = 0; ei < glide_texture_cache.size(); ei++) {
		GlideMetalTextureCacheEntry &entry = glide_texture_cache[ei];
		if (entry.address == start_addr && entry.width == w &&
			entry.height == h && entry.format == format &&
			entry.small_lod == small_lod && entry.large_lod == large_lod &&
			entry.aspect_log2 == aspect_log2 && entry.even_odd == even_odd) {
			cached = &entry;
			break;
		}
	}
	if (!cached) {
		GlideMetalTextureCacheEntry entry = {};
		entry.address = start_addr;
		entry.width = w;
		entry.height = h;
		entry.format = format;
		entry.small_lod = small_lod;
		entry.large_lod = large_lod;
		entry.aspect_log2 = aspect_log2;
		entry.even_odd = even_odd;
		entry.chain_size = need;
		entry.mip_levels = 1;
		entry.chroma_mode = chroma_mode;
		entry.chroma_value = chroma_value;
		entry.color_format = color_format;
		entry.dirty = true;
		entry.wrap_s = -1;
		entry.wrap_t = -1;
		glGenTextures(1, &entry.texture);
		glide_texture_cache.push_back(entry);
		cached = &glide_texture_cache.back();
	}

	const bool upload = cached->dirty ||
		cached->chroma_mode != chroma_mode ||
		cached->chroma_value != chroma_value ||
		cached->color_format != color_format;
	glide_gl_texture = cached->texture;
	glide_texture_mip_levels = cached->mip_levels;
	glBindTexture(GL_TEXTURE_2D, cached->texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	const GLint wrap_s = GlideStateTexClampS() ? GL_CLAMP_TO_EDGE : GL_REPEAT;
	const GLint wrap_t = GlideStateTexClampT() ? GL_CLAMP_TO_EDGE : GL_REPEAT;
	if (cached->wrap_s != wrap_s) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_s);
		cached->wrap_s = wrap_s;
	}
	if (cached->wrap_t != wrap_t) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_t);
		cached->wrap_t = wrap_t;
	}
	if (upload) {
		GlideUploadCachedTexture(*cached, src, avail, chroma_mode,
									chroma_value, color_format);
		glide_texture_mip_levels = cached->mip_levels;
	}
	glBindTexture(GL_TEXTURE_2D, 0);

	/* Window-coordinate texture coordinates describe one repeat with a
	 * 256-unit long axis. The shorter axis is reduced only by aspect ratio,
	 * never by the selected LOD's physical pixel dimensions. */
	glide_texture_s_extent = 256.f;
	glide_texture_t_extent = 256.f;
	const int decoded_aspect = GlideTexDecodeAspectLog2(aspect_log2);
	if (decoded_aspect > 0) {
		const int shift = decoded_aspect;
		glide_texture_t_extent /= (float)(1 << shift);
	} else if (decoded_aspect < 0) {
		const int shift = -decoded_aspect;
		glide_texture_s_extent /= (float)(1 << shift);
	}
	glide_is_texture_enabled = true;

	static uint32_t s_src_n = 0;
	if (++s_src_n <= 12 || (s_src_n & (s_src_n - 1)) == 0)
		QD3D_INIT_LOG("GlideMetalTexSource #%u addr=%08x %dx%d fmt=%d levels=%d",
					  (unsigned)s_src_n, start_addr, w, h, format,
					  cached->mip_levels);
}

void GlideMetalTexDownloadTable(int type, const void *data)
{
	/* type 2 = palette (Glide2 GR_TEXTABLE_PALETTE). 256 RGB words. */
	if (!data) return;
	if (type == 2 || type == 0x2) {
		const uint8_t *p = (const uint8_t *)data;
		uint32_t pal[256];
		for (int i = 0; i < 256; i++) {
			/* Guest big-endian palette words; P_8 ignores the high byte. */
			pal[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
					 ((uint32_t)p[2] << 8) | (uint32_t)p[3];
			p += 4;
		}
		/* Store on state via re-export - write through palette setter. */
		const uint32_t *old_palette = GlideStateTexPalette();
		const bool changed = old_palette == NULL ||
			std::memcmp(old_palette, pal, sizeof(pal)) != 0;
		extern void GlideStateTexSetPalette(const uint32_t *argb256);
		GlideStateTexSetPalette(pal);
		if (changed) {
			/* Palette changes affect every resident paletted texture. */
			GlideMetalTextureCacheEntry *bound = NULL;
			for (size_t ei = 0; ei < glide_texture_cache.size(); ei++) {
				GlideMetalTextureCacheEntry &entry = glide_texture_cache[ei];
				if (entry.format == 0x05) {
					entry.dirty = true;
					if (entry.texture == glide_gl_texture)
						bound = &entry;
				}
			}

			/*
			 * Unreal Tournament orders each UI tile as grTexSource(P8),
			 * grTexDownloadTable(palette), draw. TexSource therefore decoded
			 * the selected texture with the preceding tile's palette. Merely
			 * marking the cache entry dirty is too late because ApplyState
			 * only binds the already-decoded GL texture. Refresh the selected
			 * P8 texture now; unbound entries remain dirty until selected.
			 */
			if (bound && SharedMetalDevice()) {
				uint32_t avail = 0;
				const uint8_t *src =
					GlideStateTmuPtr(bound->address, &avail);
				if (src && avail >= bound->chain_size) {
					GlideUploadCachedTexture(
						*bound, src, avail, GlideStateChromaMode(),
						GlideStateChromaValue(),
						GlideStateColorFormat());
					glide_texture_mip_levels = bound->mip_levels;
					glBindTexture(GL_TEXTURE_2D, 0);
				}
			}
		}
		static uint32_t s_pal_n = 0;
		if (++s_pal_n <= 8)
			QD3D_INIT_LOG("GlideMetalTexDownloadTable palette #%u", (unsigned)s_pal_n);
	}
}
