/*
 *  gl_compositor.cpp - OpenGL+SDL compositor (implements metal_compositor.h)
 *
 *  Desktop counterpart of metal_compositor.mm. The current guest framebuffer
 *  is the one authoritative screen used by QuickDraw and accelerated engines.
 *  OpenGL owns only a work/presentation copy of those pixels.
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
#include "cpu_emulation.h"   /* ReadMacInt32 (guest tick at 0x016a) */
#include "video.h"
#include "video_blit.h"
#include "metal_device_shared.h"
#include "metal_compositor.h"
#include "gfxaccelhost.h"
#include "display_mode_controller.h"
#include "gfxaccel_resources.h"
#include "vbl_source.h"
#include "gfx_color_policy.h"
#include "gfx_log.h"
/* Windows GL 1.1 has no GLSL compile entry points; present path uses FFP. */

#include "my_sdl.h"
#if USE_SDL3
#include <SDL3/SDL_opengl.h>
#else
#include <SDL_opengl.h>
#endif
#include "gl_ext.h" /* GL_RGBA8 / GL_BGRA / FBO enums for Windows GL 1.1 */

#include "atomic.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include <cmath>

#define COMPOSITOR_LOG(...) GFX_DEBUG_EMIT("[compositor] ", __VA_ARGS__)
#define COMPOSITOR_ERR(...) GFX_DEBUG_EMIT("[compositor ERROR] ", __VA_ARGS__)

#if defined(USE_SDL1)
#include "glcontext.h"
extern void *video_get_native_window(void);
extern void video_get_host_window_size(int *width, int *height);
#define GL_HOST_WINDOW video_get_native_window()
static GLCONTEXT s_gl_ctx;
static bool s_gl_ctx_created = false;
#else
#if SDL_MAJOR_VERSION >= 3
	#undef SDL_GL_DeleteContext
	#undef SDL_GL_GetDrawableSize
	#define SDL_GL_DeleteContext SDL_GL_DestroyContext
	#define SDL_GL_GetDrawableSize SDL_GetWindowSizeInPixels
	#define SDL_GL_MakeCurrentFAILURE false
#else
	#define SDL_GL_MakeCurrentFAILURE -1
#endif
extern SDL_Window *sdl_window;
#define GL_HOST_WINDOW ((void *)sdl_window)
static SDL_GLContext s_gl_ctx = NULL;
#endif
static void *s_gl_device_window = NULL;
static bool s_ready = false;
/* Opaque sentinels so code that null-checks SharedMetalDevice() still works. */
static char s_device_sentinel = 1;
static char s_queue_sentinel = 1;
/* Set when Present is skipped because a guest 3D pass (or nested Present)
 * owns the shared GL context. Cleared when a Present completes or is
 * cancelled. */
static bool s_present_deferred = false;
static bool s_init = false;
static bool s_present_in_progress = false;
static int compositor_pixel_width = 0, compositor_pixel_height = 0, compositor_depth = 0;
static int compositor_row_bytes = 0, compositor_pitch = 0;
static int compositor_bits_per_pixel = 0;
static void *compositor_buffer = NULL;
static uint32_t compositor_buffer_size = 0;
static GLuint s_fb_tex = 0;
static GLuint s_screen_write_fbo = 0;
static int s_fb_tex_width = 0;
static int s_fb_tex_height = 0;
static uint8_t s_palette[256 * 4];
static uint8_t s_palette_5bit_lut[32 * 32 * 32];
static bool s_palette_5bit_lut_dirty = true;
static int s_palette_5bit_lut_bpp = 0;
/* True until a real CLUT has been pushed via MetalCompositorUpdatePalette.
 * Init only seeds the grayscale placeholder while this is set; once the guest
 * (DSp SetCLUT or the classic video_set_palette SetEntries) has delivered a
 * real palette, a later re-init (window recreate on a mode switch, e.g.
 * returning to the QuickDraw desktop when a game exits) preserves it instead
 * of wiping the desktop to a grayscale ramp - the "black and white on exit". */
static bool s_palette_is_placeholder = true;
static uint8_t s_gamma_lut[768];
#if !defined(GFXACCEL_USE_SDLGPU)
static GLuint s_gamma_texture = 0;
static GLuint s_gamma_program = 0;
#endif
static uint32_t s_last_gamma_gen = UINT32_MAX;
static bool s_palette_dirty = true;
static atomic_uint64 s_present_origin = 0;
static atomic_uint64 s_present_size = 0;
#if QD3D_GRAPHICS_LOGGING_ENABLED
static uint64_t s_screen_submit_count = 0;
static uint64_t s_present_count = 0;
#endif

#if defined(USE_SDL1)
static bool HostGLHasContext(void)
{
	return s_gl_ctx_created;
}

static bool HostGLCreateContext(void *window)
{
	GLPIXELATTRIBUTES pixelattributes;
	GLCONTEXTATTRIBUTES contextattributes;
	GLPixelAttributes_Create(&pixelattributes);
	GLPixelAttributes_PlatformDefaults(&pixelattributes);
	GLPixelAttributes_RGBA(&pixelattributes);
	GLPixelAttributes_DoubleBuffer(&pixelattributes);
	GLPixelAttributes_Depth(&pixelattributes, 24);
	GLPixelAttributes_Stencil(&pixelattributes, 8);
	GLPixelAttributes_EndList(&pixelattributes);
	GLContextAttributes_Create(&contextattributes);
	GLContextAttributes_PlatformDefaults(&contextattributes);
	GLContextAttributes_MajorVersion(&contextattributes, 2);
	GLContextAttributes_MinorVersion(&contextattributes, 1);
	GLContextAttributes_CompatibilityProfile(&contextattributes);
	GLContextAttributes_EndList(&contextattributes);
	memset(&s_gl_ctx, 0, sizeof(s_gl_ctx));
	s_gl_ctx_created = GLContext_CreateWithWindow(&s_gl_ctx,
		(GLWINDOWHANDLE)window, &contextattributes, &pixelattributes) != 0;
	return s_gl_ctx_created;
}

static bool HostGLMakeCurrent(void)
{
	return GLContext_SetCurrent(&s_gl_ctx) != 0;
}

static bool HostGLIsCurrent(void)
{
	return GLContext_GetCurrent(&s_gl_ctx) == s_gl_ctx.mhContext;
}

static void HostGLDestroyContext(void)
{
	GLContext_SetCurrentHandle(&s_gl_ctx, NULL);
	GLContext_Destroy(&s_gl_ctx);
	s_gl_ctx_created = false;
}

static void HostGLSetSwapInterval(int interval)
{
	GLContext_SetSwapInterval(&s_gl_ctx, interval);
}

static void HostGLSwap(void)
{
	GLContext_SwapBuffers(&s_gl_ctx);
}

static void HostGLDrawableSize(int *width, int *height)
{
	video_get_host_window_size(width, height);
}

static void HostGLWindowSize(int *width, int *height)
{
	video_get_host_window_size(width, height);
}

static const char *HostGLError(void)
{
	return "glcontext";
}

void *GfxGLGetProcAddress(const char *name)
{
	if (!s_gl_ctx_created)
		return NULL;
	return (void *)GLContext_GetProc(&s_gl_ctx, name);
}
#else
static bool HostGLHasContext(void)
{
	return s_gl_ctx != NULL;
}

static bool HostGLCreateContext(void *window)
{
	/* Prefer a compatibility profile so Mac GL 1.2 FFP maps cleanly. */
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
#if defined(SDL_GL_CONTEXT_PROFILE_MASK)
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
#endif
	s_gl_ctx = SDL_GL_CreateContext((SDL_Window *)window);
	return s_gl_ctx != NULL;
}

static bool HostGLMakeCurrent(void)
{
	return SDL_GL_MakeCurrent((SDL_Window *)s_gl_device_window, s_gl_ctx)
		!= SDL_GL_MakeCurrentFAILURE;
}

static bool HostGLIsCurrent(void)
{
	return SDL_GL_GetCurrentContext() == s_gl_ctx;
}

static void HostGLDestroyContext(void)
{
	if (s_gl_device_window)
		SDL_GL_MakeCurrent((SDL_Window *)s_gl_device_window, NULL);
	SDL_GL_DeleteContext(s_gl_ctx);
	s_gl_ctx = NULL;
}

static void HostGLSetSwapInterval(int interval)
{
	SDL_GL_SetSwapInterval(interval);
}

static void HostGLSwap(void)
{
	SDL_GL_SwapWindow((SDL_Window *)s_gl_device_window);
}

static void HostGLDrawableSize(int *width, int *height)
{
	*width = 0;
	*height = 0;
	if (sdl_window)
		SDL_GL_GetDrawableSize(sdl_window, width, height);
}

static void HostGLWindowSize(int *width, int *height)
{
	*width = 0;
	*height = 0;
	if (sdl_window)
		SDL_GetWindowSize(sdl_window, width, height);
}

static const char *HostGLError(void)
{
	return SDL_GetError();
}

void *GfxGLGetProcAddress(const char *name)
{
	return (void *)SDL_GL_GetProcAddress(name);
}
#endif

#if !defined(GFXACCEL_USE_SDLGPU)
void GfxGLExtLoad(GfxGLExt &e)
{
	e.GenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)GfxGLGetProcAddress("glGenFramebuffers");
	if (!e.GenFramebuffers)
		e.GenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)GfxGLGetProcAddress("glGenFramebuffersEXT");
	e.DeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)GfxGLGetProcAddress("glDeleteFramebuffers");
	if (!e.DeleteFramebuffers)
		e.DeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)GfxGLGetProcAddress("glDeleteFramebuffersEXT");
	e.BindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)GfxGLGetProcAddress("glBindFramebuffer");
	if (!e.BindFramebuffer)
		e.BindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)GfxGLGetProcAddress("glBindFramebufferEXT");
	e.FramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)GfxGLGetProcAddress("glFramebufferTexture2D");
	if (!e.FramebufferTexture2D)
		e.FramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)GfxGLGetProcAddress("glFramebufferTexture2DEXT");
	e.GenRenderbuffers = (PFNGLGENRENDERBUFFERSPROC)GfxGLGetProcAddress("glGenRenderbuffers");
	if (!e.GenRenderbuffers)
		e.GenRenderbuffers = (PFNGLGENRENDERBUFFERSPROC)GfxGLGetProcAddress("glGenRenderbuffersEXT");
	e.DeleteRenderbuffers = (PFNGLDELETERENDERBUFFERSPROC)GfxGLGetProcAddress("glDeleteRenderbuffers");
	if (!e.DeleteRenderbuffers)
		e.DeleteRenderbuffers = (PFNGLDELETERENDERBUFFERSPROC)GfxGLGetProcAddress("glDeleteRenderbuffersEXT");
	e.BindRenderbuffer = (PFNGLBINDRENDERBUFFERPROC)GfxGLGetProcAddress("glBindRenderbuffer");
	if (!e.BindRenderbuffer)
		e.BindRenderbuffer = (PFNGLBINDRENDERBUFFERPROC)GfxGLGetProcAddress("glBindRenderbufferEXT");
	e.RenderbufferStorage = (PFNGLRENDERBUFFERSTORAGEPROC)GfxGLGetProcAddress("glRenderbufferStorage");
	if (!e.RenderbufferStorage)
		e.RenderbufferStorage = (PFNGLRENDERBUFFERSTORAGEPROC)GfxGLGetProcAddress("glRenderbufferStorageEXT");
	e.FramebufferRenderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)GfxGLGetProcAddress("glFramebufferRenderbuffer");
	if (!e.FramebufferRenderbuffer)
		e.FramebufferRenderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)GfxGLGetProcAddress("glFramebufferRenderbufferEXT");
	e.CheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)GfxGLGetProcAddress("glCheckFramebufferStatus");
	if (!e.CheckFramebufferStatus)
		e.CheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)GfxGLGetProcAddress("glCheckFramebufferStatusEXT");
	e.GenerateMipmap = (PFNGLGENERATEMIPMAPPROC)GfxGLGetProcAddress("glGenerateMipmap");
	if (!e.GenerateMipmap)
		e.GenerateMipmap = (PFNGLGENERATEMIPMAPPROC)GfxGLGetProcAddress("glGenerateMipmapEXT");
	e.ActiveTexture = (PFNGLACTIVETEXTUREPROC)GfxGLGetProcAddress("glActiveTexture");
	if (!e.ActiveTexture)
		e.ActiveTexture = (PFNGLACTIVETEXTUREPROC)GfxGLGetProcAddress("glActiveTextureARB");
	e.ClientActiveTexture = (PFNGLCLIENTACTIVETEXTUREPROC)GfxGLGetProcAddress("glClientActiveTexture");
	if (!e.ClientActiveTexture)
		e.ClientActiveTexture = (PFNGLCLIENTACTIVETEXTUREPROC)GfxGLGetProcAddress("glClientActiveTextureARB");
	e.MultiTexCoord2f = (PFNGLMULTITEXCOORD2FPROC)GfxGLGetProcAddress("glMultiTexCoord2f");
	if (!e.MultiTexCoord2f)
		e.MultiTexCoord2f = (PFNGLMULTITEXCOORD2FPROC)GfxGLGetProcAddress("glMultiTexCoord2fARB");
	e.MultiTexCoord4f = (PFNGLMULTITEXCOORD4FPROC)GfxGLGetProcAddress("glMultiTexCoord4f");
	if (!e.MultiTexCoord4f)
		e.MultiTexCoord4f = (PFNGLMULTITEXCOORD4FPROC)GfxGLGetProcAddress("glMultiTexCoord4fARB");
	e.SecondaryColor3f = (PFNGLSECONDARYCOLOR3FPROC)GfxGLGetProcAddress("glSecondaryColor3f");
	if (!e.SecondaryColor3f)
		e.SecondaryColor3f = (PFNGLSECONDARYCOLOR3FPROC)GfxGLGetProcAddress("glSecondaryColor3fEXT");
	e.BlendFuncSeparate = (PFNGLBLENDFUNCSEPARATEPROC)GfxGLGetProcAddress("glBlendFuncSeparate");
	if (!e.BlendFuncSeparate)
		e.BlendFuncSeparate = (PFNGLBLENDFUNCSEPARATEPROC)GfxGLGetProcAddress("glBlendFuncSeparateEXT");
	e.BlendColor = (GFXPFNGLBLENDCOLORPROC)GfxGLGetProcAddress("glBlendColor");
	if (!e.BlendColor)
		e.BlendColor = (GFXPFNGLBLENDCOLORPROC)GfxGLGetProcAddress("glBlendColorEXT");
	e.BlendEquation = (GFXPFNGLBLENDEQUATIONPROC)GfxGLGetProcAddress("glBlendEquation");
	if (!e.BlendEquation)
		e.BlendEquation = (GFXPFNGLBLENDEQUATIONPROC)GfxGLGetProcAddress("glBlendEquationEXT");
	e.FogCoordf = (PFNGLFOGCOORDFPROC)GfxGLGetProcAddress("glFogCoordf");
	if (!e.FogCoordf)
		e.FogCoordf = (PFNGLFOGCOORDFPROC)GfxGLGetProcAddress("glFogCoordfEXT");
	e.BlitFramebuffer = (GFXPFNGLBLITFRAMEBUFFERPROC)GfxGLGetProcAddress("glBlitFramebuffer");
	if (!e.BlitFramebuffer)
		e.BlitFramebuffer = (GFXPFNGLBLITFRAMEBUFFERPROC)GfxGLGetProcAddress("glBlitFramebufferEXT");
	e.RenderbufferStorageMultisample = (GFXPFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC)GfxGLGetProcAddress("glRenderbufferStorageMultisample");
	if (!e.RenderbufferStorageMultisample)
		e.RenderbufferStorageMultisample = (GFXPFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC)GfxGLGetProcAddress("glRenderbufferStorageMultisampleEXT");
	e.fbo = e.GenFramebuffers && e.BindFramebuffer && e.FramebufferTexture2D &&
	        e.GenRenderbuffers && e.BindRenderbuffer && e.RenderbufferStorage &&
	        e.FramebufferRenderbuffer && e.CheckFramebufferStatus && e.DeleteFramebuffers;
	e.GenPrograms = (GFXPFNGLGENPROGRAMSPROC)GfxGLGetProcAddress("glGenProgramsARB");
	e.BindProgram = (GFXPFNGLBINDPROGRAMPROC)GfxGLGetProcAddress("glBindProgramARB");
	e.ProgramString = (GFXPFNGLPROGRAMSTRINGPROC)GfxGLGetProcAddress("glProgramStringARB");
	e.multitex = e.ActiveTexture != NULL &&
	             (e.MultiTexCoord2f != NULL || e.MultiTexCoord4f != NULL);
}
#endif

bool GLCompositorDeviceInit(void)
{ /* Should ONLY be called by MetalCompositorInit or otherwise
	the backing variables will not be recreated correctly and it will
	white screen */
	void *window = GL_HOST_WINDOW;
	QD3D_INIT_LOG("GLCompositorDeviceInit: ready=%d context=%d window=%p",
				  s_ready, HostGLHasContext(), window);

	if (!window) {
		QD3D_INIT_LOG("GLCompositorDeviceInit: FAILED because host window is null");
		fprintf(stderr, "[gfxaccel-gl] GLCompositorDeviceInit: host window is NULL\n");
		return false;
	}

	if (HostGLHasContext())
		HostGLDestroyContext();

	s_gl_device_window = window;
	if (!HostGLCreateContext(window)) {
		QD3D_INIT_LOG("GLCompositorDeviceInit: context creation FAILED: %s", HostGLError());
		fprintf(stderr, "[gfxaccel-gl] GL context creation failed: %s\n", HostGLError());
		s_gl_device_window = NULL;
		s_ready = false;
		return false;
	}

	if (!HostGLMakeCurrent()) {
		QD3D_INIT_LOG("GLCompositorDeviceInit: MakeCurrent FAILED: %s", HostGLError());
		fprintf(stderr, "[gfxaccel-gl] MakeCurrent failed: %s\n", HostGLError());
		HostGLDestroyContext();
		s_gl_device_window = NULL;
		s_ready = false;
		return false;
	}

	HostGLSetSwapInterval(0);

	const char *vendor = (const char *)glGetString(GL_VENDOR);
	const char *renderer = (const char *)glGetString(GL_RENDERER);
	const char *version = (const char *)glGetString(GL_VERSION);
	fprintf(stderr, "[gfxaccel-gl] OpenGL ready: %s / %s / %s\n",
			vendor ? vendor : "?",
			renderer ? renderer : "?",
			version ? version : "?");

	s_ready = true;
	QD3D_INIT_LOG("GLCompositorDeviceInit: SUCCESS vendor='%s' renderer='%s' version='%s'",
				  vendor ? vendor : "?", renderer ? renderer : "?",
				  version ? version : "?");
	return true;
}

void GLCompositorDeviceShutdown(void)
{
	if (HostGLHasContext())
		HostGLDestroyContext();
	s_gl_device_window = NULL;
	s_ready = false;
}

bool GLCompositorDeviceIsReady(void)
{
	return s_ready && HostGLHasContext();
}

void GLCompositorDeviceSwap(void)
{
	assert(s_ready);
	assert(HostGLHasContext());
	assert(s_gl_device_window != NULL);
	HostGLSwap();
}

void GLCompositorDeviceGetDrawableSize(int *out_w, int *out_h)
{
	assert(s_ready);
	assert(HostGLHasContext());
	assert(s_gl_device_window != NULL);
	assert(out_w != NULL);
	assert(out_h != NULL);
	HostGLDrawableSize(out_w, out_h);
}
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
extern bool objc_getIsLinearGammaEnabled(void);
static inline bool GLCompositorIsLinearGamma(void) {
	return objc_getIsLinearGammaEnabled();
}
#else
static inline bool GLCompositorIsLinearGamma(void)
{ return false; }
#endif


void MetalValidation_InstallErrorHandler(void * /*cmdBufPtr*/)
{
	/* no-op on OpenGL */
}

extern "C" void vbl_source_sdl_tick(double target_ts);
extern "C" int RaveGLRenderPassActive(void);
extern "C" int GLFFPRenderPassActive(void);
extern "C" int GlideMetalRenderPassActive(void);

/* All guest 3D backends borrow the compositor's compatibility context.  Keep
 * the ownership test in one place so Present and the deferred flush cannot
 * drift apart as backends are added or removed. */
static bool MetalIs3DRenderPassActive(void)
{
	return RaveGLRenderPassActive() || GLFFPRenderPassActive() ||
		   GlideMetalRenderPassActive();
}




class ScopedCompositorPresent
{
public:
	ScopedCompositorPresent()
	{
		assert(!s_present_in_progress);
		s_present_in_progress = true;
	}

	~ScopedCompositorPresent()
	{
		assert(s_present_in_progress);
		s_present_in_progress = false;
	}

private:
	/* Declared and never defined: copying a scope guard would run the
	   destructor twice. */
	ScopedCompositorPresent(const ScopedCompositorPresent &);
	ScopedCompositorPresent &operator=(const ScopedCompositorPresent &);
};

/* Screen resolve and final presentation borrow the same compatibility context
 * as RAVE, AGL and Glide. Keep that borrowing transactional: neither path may
 * leak its one-texture transfer matrices or fixed-function state into the
 * producer's next frame. */
class ScopedGLScreenTransfer
{
public:
	ScopedGLScreenTransfer()
		: saved_matrix_mode(GL_MODELVIEW),
		  saved_active_texture(GL_TEXTURE0),
		  has_multitexture(false)
	{
		glGetIntegerv(GL_MATRIX_MODE, &saved_matrix_mode);
		GfxGLExt &ext = gfx_gl_ext();
		has_multitexture = ext.multitex && ext.ActiveTexture;
		if (has_multitexture)
			glGetIntegerv(GL_ACTIVE_TEXTURE, &saved_active_texture);

		glPushAttrib(GL_ALL_ATTRIB_BITS);
		if (has_multitexture)
			ext.ActiveTexture(GL_TEXTURE0);
		glDisable(GL_TEXTURE_GEN_S);
		glDisable(GL_TEXTURE_GEN_T);
		glDisable(GL_TEXTURE_GEN_R);
		glDisable(GL_TEXTURE_GEN_Q);
		glMatrixMode(GL_TEXTURE);
		glPushMatrix();
		glLoadIdentity();
		glMatrixMode(GL_PROJECTION);
		glPushMatrix();
		glLoadIdentity();
		glMatrixMode(GL_MODELVIEW);
		glPushMatrix();
		glLoadIdentity();
	}

	~ScopedGLScreenTransfer()
	{
		glMatrixMode(GL_MODELVIEW);
		glPopMatrix();
		glMatrixMode(GL_PROJECTION);
		glPopMatrix();
		glMatrixMode(GL_TEXTURE);
		glPopMatrix();
		glPopAttrib();

		GfxGLExt &ext = gfx_gl_ext();
		if (has_multitexture)
			ext.ActiveTexture((GLenum)saved_active_texture);
		glMatrixMode((GLenum)saved_matrix_mode);
	}

private:
	/* Declared and never defined: copying a scope guard would restore the
	   borrowed state twice. */
	ScopedGLScreenTransfer(const ScopedGLScreenTransfer &);
	ScopedGLScreenTransfer &operator=(const ScopedGLScreenTransfer &);

	GLint saved_matrix_mode;
	GLint saved_active_texture;
	bool has_multitexture;
};


// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/* Round half away from zero, the rule std::lround follows. */
static long round_to_long(double v)
{
	return (long)(v >= 0.0 ? v + 0.5 : v - 0.5);
}
#if QD3D_GRAPHICS_LOGGING_ENABLED
static bool MetalCompositorShouldTracePresent(uint64_t count)
{
	return count <= 8 || (count != 0 && (count & (count - 1)) == 0) ||
		   (count % 120) == 0;
}
#endif
static int MetalCompositorDepthToBPPBits(int depth_mode)
{
	switch (depth_mode) {
	case VIDEO_DEPTH_1BIT: return 1;
	case VIDEO_DEPTH_2BIT: return 2;
	case VIDEO_DEPTH_4BIT: return 4;
	case VIDEO_DEPTH_8BIT: return 8;
	case VIDEO_DEPTH_16BIT: return 16;
	case VIDEO_DEPTH_32BIT: return 32;
	default: return 32;
	}
}

static void MetalCompositorEnsureIdentityGamma(void)
{
	for (int i = 0; i < 256; i++) {
		s_gamma_lut[i] = (uint8_t)i;
		s_gamma_lut[256 + i] = (uint8_t)i;
		s_gamma_lut[512 + i] = (uint8_t)i;
	}
	/* Init recreates presentation resources but DMC deliberately carries the
	 * guest gamma generation across mode switches. Force the next VBL latch
	 * to rebuild this freshly-reset working LUT even when the generation
	 * number itself did not change. */
	s_last_gamma_gen = UINT32_MAX;
}

static void MetalCompositorDestroyTextures(void)
{
	GfxGLExt &ext = gfx_gl_ext();
	if (s_screen_write_fbo && ext.fbo) {
		ext.DeleteFramebuffers(1, &s_screen_write_fbo);
		s_screen_write_fbo = 0;
	}
	if (s_fb_tex) { glDeleteTextures(1, &s_fb_tex); s_fb_tex = 0; }
	s_fb_tex_width = 0;
	s_fb_tex_height = 0;
}

static void MetalCompositorEnsureFrameBufferTexture(void)
{
	if (!s_fb_tex) {
		glGenTextures(1, &s_fb_tex);
		glBindTexture(GL_TEXTURE_2D, s_fb_tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	if (s_fb_tex_width != compositor_pixel_width || s_fb_tex_height != compositor_pixel_height) {
		glBindTexture(GL_TEXTURE_2D, s_fb_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, compositor_pixel_width, compositor_pixel_height, 0,
					 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		s_fb_tex_width = compositor_pixel_width;
		s_fb_tex_height = compositor_pixel_height;
	}
}

struct GuestScreenRect {
	int x;
	int y;
	int width;
	int height;
};

/*
 * Expand one guest framebuffer rectangle into tightly packed RGBA8.
 *
 * Pre-rewrite FFP present uploaded raw guest pixels (gamma shaders were never
 * enabled). The rewrite's upload_guest_screen_to_texture(true) baked s_gamma_lut
 * into this texture and washed the image. Keep expand raw so present matches
 * the pre-changeset look; the shared texture is also the RAVE/Glide resolve
 * target and must not carry a display-policy bake.
 */
static void expand_framebuffer_rect_rgba(const GuestScreenRect &rect,
										 std::vector<uint8_t> &out)
{
	out.resize((size_t)rect.width * (size_t)rect.height * 4);
	const uint8_t *src = (const uint8_t *)compositor_buffer;
	if (!src) {
		std::memset(vec_data(out), 0, out.size());
		return;
	}

	const int rb = compositor_row_bytes;
	const int bpp = compositor_bits_per_pixel;

	if (bpp == 32) {
		for (int y = 0; y < rect.height; y++) {
			const uint8_t *row =
				src + (size_t)(rect.y + y) * rb + (size_t)rect.x * 4;
			uint8_t *dst = vec_data(out) + (size_t)y * rect.width * 4;
			for (int x = 0; x < rect.width; x++) {
				dst[x * 4 + 0] = row[x * 4 + 1];
				dst[x * 4 + 1] = row[x * 4 + 2];
				dst[x * 4 + 2] = row[x * 4 + 3];
				dst[x * 4 + 3] = 255;
			}
		}
		return;
	}

	if (bpp == 16) {
		for (int y = 0; y < rect.height; y++) {
			const uint8_t *row =
				src + (size_t)(rect.y + y) * rb + (size_t)rect.x * 2;
			uint8_t *dst = vec_data(out) + (size_t)y * rect.width * 4;
			for (int x = 0; x < rect.width; x++) {
				const uint16_t be = (uint16_t)((row[x * 2] << 8) | row[x * 2 + 1]);
				dst[x * 4 + 0] = (uint8_t)(((be >> 10) & 0x1f) * 255 / 31);
				dst[x * 4 + 1] = (uint8_t)(((be >> 5) & 0x1f) * 255 / 31);
				dst[x * 4 + 2] = (uint8_t)((be & 0x1f) * 255 / 31);
				dst[x * 4 + 3] = 255;
			}
		}
		return;
	}

	/* Indexed 1/2/4/8 - resolve CLUT only, no display gamma. */
	for (int y = 0; y < rect.height; y++) {
		const uint8_t *row = src + (size_t)(rect.y + y) * (size_t)rb;
		uint8_t *dst = vec_data(out) + (size_t)y * rect.width * 4;
		for (int dx = 0; dx < rect.width; dx++) {
			const int x = rect.x + dx;
			uint8_t index = 0;
			if (bpp == 8) {
				index = row[x];
			} else if (bpp == 4) {
				const uint8_t b = row[x / 2];
				index = (x & 1) ? (b & 0x0f) : (b >> 4);
			} else if (bpp == 2) {
				const uint8_t b = row[x / 4];
				const int shift = (3 - (x % 4)) * 2;
				index = (b >> shift) & 0x3;
			} else {
				const uint8_t b = row[x / 8];
				index = (b >> (7 - (x % 8))) & 0x1;
			}
			dst[dx * 4 + 0] = s_palette[index * 4 + 0];
			dst[dx * 4 + 1] = s_palette[index * 4 + 1];
			dst[dx * 4 + 2] = s_palette[index * 4 + 2];
			dst[dx * 4 + 3] = 255;
		}
	}
}

#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8
#define GL_UNSIGNED_INT_8_8_8_8 0x8035
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#endif
#ifdef WORDS_BIGENDIAN
#define COMPOSITORGUESTPIXELTYPE GL_UNSIGNED_INT_8_8_8_8_REV
#else
#define COMPOSITORGUESTPIXELTYPE GL_UNSIGNED_INT_8_8_8_8
#endif
#define COMPOSITORGUESTPIXELBYTES 4

/* Refresh the presentation texture from guest VRAM. No dirty journal: every
 * writer (QD, WM, engines after resolve) shares screen_base. */
static bool upload_guest_screen_to_texture(void)
{
	if (!compositor_buffer || compositor_pixel_width <= 0 ||
		compositor_pixel_height <= 0)
		return false;

	MetalCompositorEnsureFrameBufferTexture();
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, s_fb_tex);
#if !defined(GFXACCEL_USE_SDLGPU)
	if (compositor_bits_per_pixel == 32) {
		glPixelStorei(GL_UNPACK_ROW_LENGTH,
					  compositor_row_bytes / COMPOSITORGUESTPIXELBYTES);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
						compositor_pixel_width, compositor_pixel_height,
						GL_BGRA, COMPOSITORGUESTPIXELTYPE, compositor_buffer);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		return true;
	}
#endif
	static std::vector<uint8_t> rgba;
	const GuestScreenRect screen = {
		0, 0, compositor_pixel_width, compositor_pixel_height
	};
	expand_framebuffer_rect_rgba(screen, rgba);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
					compositor_pixel_width, compositor_pixel_height,
					GL_RGBA, GL_UNSIGNED_BYTE, vec_data(rgba));
	return true;
}

static void draw_textured_quad(void)
{
	glBegin(GL_QUADS);
	glTexCoord2f(0.f, 1.f); glVertex2f(-1.f, -1.f);
	glTexCoord2f(1.f, 1.f); glVertex2f( 1.f, -1.f);
	glTexCoord2f(1.f, 0.f); glVertex2f( 1.f,  1.f);
	glTexCoord2f(0.f, 0.f); glVertex2f(-1.f,  1.f);
	glEnd();
}

/*
 * RAVE and the compositor intentionally share one compatibility-profile GL
 * context. State set while an engine FBO is bound therefore survives after
 * the FBO is unbound.  In particular, a 960x720 RAVE scissor clips a
 * 1920x1440 window to its lower-left quarter, and a channel write mask such as
 * red-only also applies to the window back buffer.  Establish a complete
 * presentation boundary before clearing or drawing the screen.
 */
static void prepare_present_state(void)
{
	GfxGLExt &ext = gfx_gl_ext();
	if (ext.fbo)
		ext.BindFramebuffer(GL_FRAMEBUFFER, 0);

	/* A multi-textured RAVE draw may leave texture unit 1 active/enabled. */
	if (ext.multitex && ext.ActiveTexture) {
		ext.ActiveTexture(GL_TEXTURE1);
		glDisable(GL_TEXTURE_2D);
		ext.ActiveTexture(GL_TEXTURE0);
	}

	glDrawBuffer(GL_BACK);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_FOG);
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	glDisable(GL_COLOR_SUM);
	glDisable(GL_BLEND);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);
	glStencilMask(~0u);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

static bool bind_screen_write_target(void)
{
	if (!s_fb_tex || !SharedMetalDevice())
		return false;
	GfxGLExt &ext = gfx_gl_ext();
	if (!ext.fbo)
		return false;
	if (!s_screen_write_fbo)
		ext.GenFramebuffers(1, &s_screen_write_fbo);
	ext.BindFramebuffer(GL_FRAMEBUFFER, s_screen_write_fbo);
	ext.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
							 GL_TEXTURE_2D, s_fb_tex, 0);
	/* Draw/read-buffer selection is shared context state on the EXT_fbo path.
	 * Present leaves GL_BACK selected; using that selection while an FBO is
	 * bound makes the accelerated resolve either a no-op or
	 * GL_INVALID_OPERATION. Select the attachment explicitly at this
	 * screen-synchronization boundary. */
	glDrawBuffer(GL_COLOR_ATTACHMENT0);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	const bool complete =
		ext.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	if (!complete) {
		ext.BindFramebuffer(GL_FRAMEBUFFER, 0);
		glDrawBuffer(GL_BACK);
		glReadBuffer(GL_BACK);
	}
	return complete;
}

static void unbind_screen_write_target(void)
{
	GfxGLExt &ext = gfx_gl_ext();
	ext.BindFramebuffer(GL_FRAMEBUFFER, 0);
	glDrawBuffer(GL_BACK);
	glReadBuffer(GL_BACK);
}

static void rebuild_palette_5bit_lut(void)
{
	const int palette_count =
		1 << std::max(1, std::min(8, compositor_bits_per_pixel));
	for (int r5 = 0; r5 < 32; r5++) {
		const int r = (r5 * 255 + 15) / 31;
		for (int g5 = 0; g5 < 32; g5++) {
			const int g = (g5 * 255 + 15) / 31;
			for (int b5 = 0; b5 < 32; b5++) {
				const int b = (b5 * 255 + 15) / 31;
				int best_index = 0;
				uint32_t best_distance = UINT32_MAX;
				for (int i = 0; i < palette_count; i++) {
					const int dr = r - s_palette[i * 4 + 0];
					const int dg = g - s_palette[i * 4 + 1];
					const int db = b - s_palette[i * 4 + 2];
					const uint32_t distance =
						(uint32_t)(dr * dr + dg * dg + db * db);
					if (distance < best_distance) {
						best_distance = distance;
						best_index = i;
						if (distance == 0)
							break;
					}
				}
				s_palette_5bit_lut[(r5 << 10) | (g5 << 5) | b5] =
					(uint8_t)best_index;
			}
		}
	}
	s_palette_5bit_lut_dirty = false;
	s_palette_5bit_lut_bpp = compositor_bits_per_pixel;
}

static int synchronize_screen_rect_to_guest(int left, int top,
											int width, int height)
{
	if (!compositor_buffer || compositor_bits_per_pixel <= 0 ||
		!bind_screen_write_target()) {
		return -1;
	}

	const int64_t requested_right = (int64_t)left + width;
	const int64_t requested_bottom = (int64_t)top + height;
	const int right = (int)std::max<int64_t>(
		0, std::min<int64_t>(compositor_pixel_width, requested_right));
	const int bottom = (int)std::max<int64_t>(
		0, std::min<int64_t>(compositor_pixel_height, requested_bottom));
	left = std::max(0, std::min(compositor_pixel_width, left));
	top = std::max(0, std::min(compositor_pixel_height, top));
	width = right - left;
	height = bottom - top;
	if (width <= 0 || height <= 0) {
		unbind_screen_write_target();
		return 0;
	}

	static std::vector<uint8_t> rgba;
	rgba.resize((size_t)width * (size_t)height * 4u);
	GLint old_pack = 4;
	glGetIntegerv(GL_PACK_ALIGNMENT, &old_pack);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	while (glGetError() != GL_NO_ERROR) {}
	glReadPixels(left, top, width, height,
				 GL_RGBA, GL_UNSIGNED_BYTE, vec_data(rgba));
	glPixelStorei(GL_PACK_ALIGNMENT, old_pack);
	const GLenum read_err = glGetError();
	const bool ok = read_err == GL_NO_ERROR;
	unbind_screen_write_target();
	if (!ok)
		return -1;

	uint8_t *guest = static_cast<uint8_t *>(compositor_buffer);
	for (int y = 0; y < height; y++) {
		const uint8_t *src = vec_data(rgba) + (size_t)y * width * 4u;
		uint8_t *dst = guest + (size_t)(top + y) * compositor_row_bytes +
			(size_t)left * (size_t)(compositor_bits_per_pixel / 8);
		if (compositor_bits_per_pixel == 16) {
			for (int x = 0; x < width; x++) {
				const uint16_t pixel =
					(uint16_t)(((src[0] >> 3) << 10) |
							   ((src[1] >> 3) << 5) |
							   (src[2] >> 3));
				dst[0] = (uint8_t)(pixel >> 8);
				dst[1] = (uint8_t)pixel;
				src += 4;
				dst += 2;
			}
		} else if (compositor_bits_per_pixel == 32) {
			for (int x = 0; x < width; x++) {
				dst[0] = 0xff;
				dst[1] = src[0];
				dst[2] = src[1];
				dst[3] = src[2];
				src += 4;
				dst += 4;
			}
		} else {
			if (s_palette_5bit_lut_dirty ||
				s_palette_5bit_lut_bpp != compositor_bits_per_pixel) {
				rebuild_palette_5bit_lut();
			}
			dst = guest + (size_t)(top + y) * compositor_row_bytes;
			const int bpp = compositor_bits_per_pixel;
			const uint8_t pixel_mask = (uint8_t)((1u << bpp) - 1u);
			for (int x = 0; x < width; x++) {
				const uint8_t index = s_palette_5bit_lut[
					((uint32_t)(src[0] >> 3) << 10) |
					((uint32_t)(src[1] >> 3) << 5) |
					(uint32_t)(src[2] >> 3)];
				const int pixel_x = left + x;
				if (bpp == 8) {
					dst[pixel_x] = index;
				} else {
					const int pixels_per_byte = 8 / bpp;
					const int shift =
						(pixels_per_byte - 1 - pixel_x % pixels_per_byte) * bpp;
					uint8_t &packed = dst[pixel_x / pixels_per_byte];
					packed = (uint8_t)((packed & ~(pixel_mask << shift)) |
									  ((index & pixel_mask) << shift));
				}
				src += 4;
			}
		}
	}
	return 0;
}

static int32_t publish_layer_to_guest_screen(const CompositeLayer *layer)
{
	if (!layer || !layer->source)
		return kGfxAccelErrInvalidDescriptor;
	if ((uint32_t)layer->blend > (uint32_t)kBlendStraight ||
		!std::isfinite(layer->alpha) ||
		!std::isfinite(layer->dst_origin_x) ||
		!std::isfinite(layer->dst_origin_y) ||
		!std::isfinite(layer->dst_size_w) ||
		!std::isfinite(layer->dst_size_h)) {
		return kGfxAccelErrInvalidDescriptor;
	}
	if (!s_init || !SharedMetalDevice())
		return kGfxAccelErrDrawableUnavailable;

	const GLuint source = (GLuint)(uintptr_t)layer->source;
	ScopedGLScreenTransfer screen_transfer;
	MetalCompositorEnsureFrameBufferTexture();

	/* Load the current guest screen before a partial or translucent engine
	 * write. The completed result is copied back below, preserving real
	 * framebuffer last-writer ordering per pixel. */
	(void)upload_guest_screen_to_texture();
	if (!bind_screen_write_target())
		return kGfxAccelErrPipelineUnavailable;

	GfxGLExt &ext = gfx_gl_ext();
	if (ext.multitex && ext.ActiveTexture) {
		/* A RAVE/AGL frame may finish with its second texture unit enabled.
		 * The screen resolve is a one-texture transfer, not another guest
		 * rendering stage, so no producer texture environment may participate. */
		ext.ActiveTexture(GL_TEXTURE1);
		glDisable(GL_TEXTURE_2D);
		ext.ActiveTexture(GL_TEXTURE0);
	}
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, source);
	GLint source_width = 0;
	GLint source_height = 0;
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,
							&source_width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,
							&source_height);
	if (source_width <= 0 || source_height <= 0) {
		unbind_screen_write_target();
		return kGfxAccelErrInvalidDescriptor;
	}
	if (layer->src_origin_x >= (uint32_t)source_width ||
		layer->src_origin_y >= (uint32_t)source_height) {
		unbind_screen_write_target();
		return kGfxAccelErrInvalidDescriptor;
	}
	const uint32_t source_rect_width =
		layer->src_size_w ? layer->src_size_w :
		(uint32_t)source_width - layer->src_origin_x;
	const uint32_t source_rect_height =
		layer->src_size_h ? layer->src_size_h :
		(uint32_t)source_height - layer->src_origin_y;
	if ((uint64_t)layer->src_origin_x + source_rect_width >
			(uint64_t)source_width ||
		(uint64_t)layer->src_origin_y + source_rect_height >
			(uint64_t)source_height) {
		unbind_screen_write_target();
		return kGfxAccelErrInvalidDescriptor;
	}

	const float dst_x = layer->dst_origin_x;
	const float dst_y = layer->dst_origin_y;
	const float dst_w = layer->dst_size_w > 0.f
		? layer->dst_size_w : (float)source_rect_width;
	const float dst_h = layer->dst_size_h > 0.f
		? layer->dst_size_h : (float)source_rect_height;
	const double dst_right = (double)dst_x + dst_w;
	const double dst_bottom = (double)dst_y + dst_h;
	if (dst_w <= 0.f || dst_h <= 0.f ||
		!std::isfinite(dst_right) || !std::isfinite(dst_bottom)) {
		unbind_screen_write_target();
		return kGfxAccelErrInvalidDescriptor;
	}

	glViewport(0, 0, compositor_pixel_width, compositor_pixel_height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_FOG);
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	if (layer->blend == kBlendOpaque) {
		glDisable(GL_BLEND);
	} else {
		glEnable(GL_BLEND);
		glBlendFunc(layer->blend == kBlendPremultiplied
						? GL_ONE : GL_SRC_ALPHA,
					GL_ONE_MINUS_SRC_ALPHA);
	}
	const float alpha = std::max(0.f, std::min(1.f, layer->alpha));
	glColor4f(1.f, 1.f, 1.f, alpha);

	const float x0 = dst_x / compositor_pixel_width * 2.f - 1.f;
	const float x1 = (dst_x + dst_w) / compositor_pixel_width * 2.f - 1.f;
	/* t=0 is the logical top of the canonical texture. Render it at the
	 * physical bottom of the FBO so the final presentation's vertical mapping
	 * remains identical to a CPU upload. */
	const float y0 = dst_y / compositor_pixel_height * 2.f - 1.f;
	const float y1 = (dst_y + dst_h) / compositor_pixel_height * 2.f - 1.f;
	const float u0 = (float)layer->src_origin_x / source_width;
	const float u1 =
		(float)(layer->src_origin_x + source_rect_width) / source_width;
	const float t0 =
		1.f - (float)layer->src_origin_y / source_height;
	const float t1 =
		1.f - (float)(layer->src_origin_y + source_rect_height) /
			source_height;

	while (glGetError() != GL_NO_ERROR) {}
	glBegin(GL_QUADS);
	glTexCoord2f(u0, t0); glVertex2f(x0, y0);
	glTexCoord2f(u1, t0); glVertex2f(x1, y0);
	glTexCoord2f(u1, t1); glVertex2f(x1, y1);
	glTexCoord2f(u0, t1); glVertex2f(x0, y1);
	glEnd();

	unbind_screen_write_target();
	if (glGetError() != GL_NO_ERROR)
		return kGfxAccelErrPipelineUnavailable;

	const int sync_left = (int)std::max(
		0.0, std::min((double)compositor_pixel_width,
					  std::floor((double)dst_x)));
	const int sync_top = (int)std::max(
		0.0, std::min((double)compositor_pixel_height,
					  std::floor((double)dst_y)));
	const int sync_right = (int)std::max(
		0.0, std::min((double)compositor_pixel_width,
					  std::ceil(dst_right)));
	const int sync_bottom = (int)std::max(
		0.0, std::min((double)compositor_pixel_height,
					  std::ceil(dst_bottom)));
	if (sync_left >= sync_right || sync_top >= sync_bottom)
		return kGfxAccelNoErr;

	/* Guest QuickDraw can read and modify the screen without a hook. Complete
	 * the accelerated destination into screen_base before returning so later
	 * QuickDraw, dialogs and Cursor Manager operations naturally see it. */
	return synchronize_screen_rect_to_guest(
		sync_left, sync_top, sync_right - sync_left, sync_bottom - sync_top) == 0
		? kGfxAccelNoErr : kGfxAccelErrPipelineUnavailable;
}

// ---------------------------------------------------------------------------
// DMC subscriber callbacks (Metal copies)
// ---------------------------------------------------------------------------
static int32_t MetalCompositor_OnModeExit(const struct DMCModeSnapshot *outgoing, void *ctx)
{
	(void)ctx;
	if (outgoing) {
		COMPOSITOR_LOG("DMC on_mode_exit: outgoing gen=%u %ux%u depth=%u",
					   outgoing->generation, outgoing->width, outgoing->height, outgoing->depth);
	}
	/* Producer ownership changes do not replace the screen. A real geometry
	 * change is handled by MetalCompositorResize when the new surface binds. */
	s_present_deferred = false;
	return 0;
}

static int32_t MetalCompositor_OnModeEnter(const struct DMCModeSnapshot *incoming, void *ctx)
{
	(void)ctx;
	if (incoming) {
		COMPOSITOR_LOG("DMC on_mode_enter: incoming gen=%u %ux%u depth=%u vbl_usec=%llu",
					   incoming->generation, incoming->width, incoming->height, incoming->depth,
					   (unsigned long long)incoming->vbl_usec);
	} else {
		return 0;
	}

	if (incoming->screen_base_host != NULL) {
		int new_w = (int)incoming->width;
		int new_h = (int)incoming->height;
		int new_pixel_depth = (int)incoming->depth;
		int new_depth = DepthModeForPixelDepth(new_pixel_depth);
		int new_row_bytes = (int)incoming->row_bytes;
		int new_pitch = (int)incoming->pitch;
		int cur_w = compositor_pixel_width;
		int cur_h = compositor_pixel_height;
		if (incoming->screen_base_host != compositor_buffer ||
			new_w != cur_w ||
			new_h != cur_h ||
			compositor_depth != new_depth ||
			compositor_row_bytes != new_row_bytes ||
			compositor_pitch != new_pitch) {
			uint64_t buffer_size64 = (uint64_t)incoming->pitch *
									 (uint64_t)incoming->height;
			uint32_t buffer_size =
				buffer_size64 > UINT32_MAX ? UINT32_MAX : (uint32_t)buffer_size64;
			int rc = MetalCompositorResize(
				new_w, new_h,
				new_depth,
				new_row_bytes,
				new_pitch,
				incoming->screen_base_host,
				buffer_size);
			if (rc != 0) {
				COMPOSITOR_ERR("DMC on_mode_enter: MetalCompositorResize failed "
							   "(rc=%d) for canonical screen %dx%d@%d rb=%d host=%p",
							   rc, new_w, new_h, new_pixel_depth, new_row_bytes,
							   incoming->screen_base_host);
			}
		}
	}

	return 0;
}
/* ---------------------------------------------------------------------------
	Public API
 --------------------------------------------------------------------------- */

void *SharedMetalDevice(void)
{
	/* The public interface promises NULL while the backend is unavailable.
	 * Teardown callbacks can legitimately arrive after the SDL GL context was
	 * deleted, so asserting here made it impossible for callers to discard
	 * stale object names safely in debug builds. */
	if (!s_ready || !HostGLHasContext() || !s_gl_device_window ||
		s_gl_device_window != GL_HOST_WINDOW)
		return NULL;
	if (!HostGLIsCurrent() && !HostGLMakeCurrent()) {
		QD3D_INIT_LOG("MakeCurrent failed: %s", HostGLError());
		return NULL;
	}
	return (void *)&s_device_sentinel;
}

void *SharedMetalCommandQueue(void)
{
	return SharedMetalDevice();
}

int MetalCompositorInit(int width, int height, int depth, int row_bytes,
						int pitch, void *buffer, uint32_t buffer_size)
{
	if (!GLCompositorDeviceInit()) {
		COMPOSITOR_ERR("GLCompositorDeviceInit failed");
		return -1;
	}

	compositor_pixel_width = width;
	compositor_pixel_height = height;
	compositor_depth = depth;
	compositor_row_bytes = row_bytes;
	compositor_pitch = pitch;
	compositor_buffer = buffer;
	compositor_buffer_size = buffer_size;
	compositor_bits_per_pixel = MetalCompositorDepthToBPPBits(depth);
	s_palette_5bit_lut_dirty = true;

	MetalCompositorEnsureIdentityGamma();
	/* Seed a grayscale placeholder CLUT ONLY before any real palette has been
	 * delivered. Init runs on every full (re)init, including the window
	 * recreate that returns to the QuickDraw desktop when a game exits; wiping
	 * a real, previously-latched palette back to a grayscale ramp there is what
	 * turned the desktop black-and-white until the next full SetEntries landed.
	 * Once a real CLUT exists, preserve it across re-init and let the guest's
	 * next SetEntries update it normally.
	 *
	 * Grayscale (index 0 black, NOT white): zeroed 8bpp staging expands to
		 * solid white if palette[0] is white, which is what D2's post-movie
		 * screen looked like. */
	if (s_palette_is_placeholder) {
		std::memset(s_palette, 0, sizeof(s_palette));
		for (int i = 0; i < 256; i++) {
			s_palette[i * 4 + 0] = (uint8_t)i;
			s_palette[i * 4 + 1] = (uint8_t)i;
			s_palette[i * 4 + 2] = (uint8_t)i;
			s_palette[i * 4 + 3] = 255;
		}
	}

	MetalCompositorDestroyTextures();
	MetalCompositorEnsureFrameBufferTexture();

	/* Subscribe to DMC first (compositor is presentation layer). */
	struct DMCSubscriber sub = {};
	sub.name = "compositor";
	sub.on_mode_exit = MetalCompositor_OnModeExit;
	sub.on_mode_enter = MetalCompositor_OnModeEnter;
	sub.ctx = NULL;
	dmc_subscribe(&sub);

	/* VBL source - SDL-driven from Present. */
	vbl_source_init(NULL, NULL, NULL);

	s_init = true;
	COMPOSITOR_LOG("Init tick=%u %dx%d depth=%d rb=%d pitch=%d bpp=%d",
				   GuestTickCount(),
				   width, height, depth, row_bytes, pitch, compositor_bits_per_pixel);
	return 0;
}

void MetalCompositorUpdatePalette(const uint8_t *pal, int num_colors)
{
	/* ALWAYS store the CLUT for CPU expansion of indexed guest screens.
	 * Compositor FB is often VIDEO_DEPTH_32BIT (BGRA normalized) even when
	 * the DSp context is 8bpp - ignoring the CLUT then leaves palette[0]
	 * wrong and zeroed staging draws as solid white/black. We still skip
	 * only the GPU indexed-texture path when bpp>8; storage is always kept. */
	if (!pal || num_colors <= 0) return;
	if (num_colors > 256) num_colors = 256;
	/* Reject all-zero CLUTs from non-indexed DSp contexts so we don't wipe
	 * a good 8bpp palette with a 16/32-bit context's empty seed. */
	bool any_nonzero = false;
	for (int i = 0; i < num_colors * 3; i++) {
		if (pal[i]) { any_nonzero = true; break; }
	}
	if (!any_nonzero && num_colors >= 16) {
		COMPOSITOR_LOG("MetalCompositorUpdatePalette: skip all-zero %d colors "
					   "(depth=%d bpp=%d)",
					   num_colors, compositor_depth, compositor_bits_per_pixel);
		return;
	}
	for (int i = 0; i < num_colors; i++) {
		s_palette[i * 4 + 0] = pal[i * 3 + 0];
		s_palette[i * 4 + 1] = pal[i * 3 + 1];
		s_palette[i * 4 + 2] = pal[i * 3 + 2];
		s_palette[i * 4 + 3] = 255;
	}
	s_palette_dirty = true;
	s_palette_5bit_lut_dirty = true;
	/* A substantial CLUT (a real indexed-mode palette, not the 2-colour B/W
	 * startup seed video_sdl2 pushes on every mode init) means the guest has
	 * delivered real colours - stop reseeding the grayscale placeholder on
	 * future re-inits so returning to the desktop keeps these colours. */
	if (num_colors >= 16 && any_nonzero)
		s_palette_is_placeholder = false;
	COMPOSITOR_LOG("MetalCompositorUpdatePalette: stored %d colors "
				   "(compositor depth=%d bpp=%d)",
				   num_colors, compositor_depth, compositor_bits_per_pixel);
}

void MetalCompositorPresent_(bool presentvbltick = true, bool upload = true)
{
	if (!s_init) return;
	/* Guest callbacks may pump a nested VideoVBL on the same emulation thread.
	 * A second clear/swap while the outer present is incomplete can replay the
	 * first movie frames and expose a partially composed back buffer. */
	if (s_present_in_progress) {
		s_present_deferred = true;
#if QD3D_GRAPHICS_LOGGING_ENABLED
		static uint64_t s_nested_present_count = 0;
		++s_nested_present_count;
		if (MetalCompositorShouldTracePresent(s_nested_present_count)) {
			QD3D_RENDER_LOG("CompositorPresent deferred: nested present count=%llu",
							(unsigned long long)s_nested_present_count);
		}
#endif
		return;
	}

	/* Guest notice callbacks can run VideoVBL before a 3D frame ends. Do not
	 * let presentation rebind framebuffer 0 while an engine is building its
	 * screen update in the shared compatibility context. */
	if (MetalIs3DRenderPassActive()) {
		s_present_deferred = true;
#if QD3D_GRAPHICS_LOGGING_ENABLED
		static uint64_t s_deferred_present_count = 0;
		++s_deferred_present_count;
		if (MetalCompositorShouldTracePresent(s_deferred_present_count)) {
			QD3D_RENDER_LOG("CompositorPresent deferred: 3D render pass active count=%llu",
							(unsigned long long)s_deferred_present_count);
		}
#endif
		return;
	}
	ScopedCompositorPresent present_scope;

	/* Ordinary VBL presents drive DSp drains/callbacks. Synchronous guest 3D
	 * presents suppress this tick to avoid nested PPC execution inside the
	 * native dispatch which submitted the frame. */
	if (presentvbltick)
		vbl_source_sdl_tick(0.0);
	MetalCompositorPaletteLatch();

	if (!SharedMetalDevice()) {
		s_present_deferred = true;
		return;
	}
	s_present_deferred = false;

	ScopedGLScreenTransfer screen_transfer;
	prepare_present_state();

	int dw = 0, dh = 0;
	GLCompositorDeviceGetDrawableSize(&dw, &dh);
	if (dw <= 0 || dh <= 0)
		HostGLWindowSize(&dw, &dh);
	if (dw <= 0) dw = compositor_pixel_width;
	if (dh <= 0) dh = compositor_pixel_height;

	glViewport(0, 0, dw, dh);
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT);

	int present_x = 0, present_y = 0;
	int present_w = dw, present_h = dh;
	if (!video_get_framebuffer_drawable_rect(
			&present_x, &present_y, &present_w, &present_h)) {
		present_x = present_y = 0;
		present_w = dw;
		present_h = dh;
	}
	/* Aspect-fit guest -> drawable (mag_rate). GL y is bottom-left. */
	glViewport(present_x, dh - present_y - present_h,
			   present_w, present_h);

	MetalCompositorEnsureFrameBufferTexture();
	if (upload)
		(void)upload_guest_screen_to_texture();
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, s_fb_tex);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glDisable(GL_BLEND);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	glColor4f(1.f, 1.f, 1.f, 1.f);
#if !defined(GFXACCEL_USE_SDLGPU)
	GfxGLExt &gamma_ext = gfx_gl_ext();
	if (s_gamma_texture) {
		gamma_ext.ActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_1D, s_gamma_texture);
		gamma_ext.ActiveTexture(GL_TEXTURE0);
		glEnable(GL_FRAGMENT_PROGRAM_ARB);
		gamma_ext.BindProgram(GL_FRAGMENT_PROGRAM_ARB, s_gamma_program);
	}
	draw_textured_quad();
	if (s_gamma_texture) {
		gamma_ext.BindProgram(GL_FRAGMENT_PROGRAM_ARB, 0);
		glDisable(GL_FRAGMENT_PROGRAM_ARB);
	}
#else
	draw_textured_quad();
#endif
	GLCompositorDeviceSwap();

	/* Cache present rect in window coords for host cursor / input helpers. */
	int window_w = 0, window_h = 0;
	HostGLWindowSize(&window_w, &window_h);
	if (window_w <= 0) window_w = compositor_pixel_width;
	if (window_h <= 0) window_h = compositor_pixel_height;
	const int window_x = dw > 0
		? (int)round_to_long((double)present_x * window_w / dw) : 0;
	const int window_y = dh > 0
		? (int)round_to_long((double)present_y * window_h / dh) : 0;
	const int window_present_w = dw > 0
		? (int)round_to_long((double)present_w * window_w / dw) : window_w;
	const int window_present_h = dh > 0
		? (int)round_to_long((double)present_h * window_h / dh) : window_h;
	atomic_store_explicit(&s_present_origin,
		((uint64_t)(uint32_t)window_x << 32) | (uint32_t)window_y,
		memory_order_relaxed);
	atomic_store_explicit(&s_present_size,
		((uint64_t)(uint32_t)window_present_w << 32) |
			(uint32_t)window_present_h,
		memory_order_relaxed);
}

void MetalCompositorShutdown(void)
{
	if (s_init) {
		if (SharedMetalDevice()) {
			MetalCompositorSubmitFrame_ClearCachedOverlay();
			MetalCompositorSubmitFrame_ClearCachedFramebuffer();
			MetalCompositorDestroyTextures();
		}
		vbl_source_shutdown();
		dmc_unsubscribe("compositor");
		s_init = false;
		compositor_buffer = NULL;
		COMPOSITOR_LOG("Shutdown tick=%u", GuestTickCount());
	}

	/* The shared SDL_GLContext is tied to the current SDL window.  It must be
	 * deleted while that window is still alive, including partial-init paths. */
	GLCompositorDeviceShutdown();
}

int MetalCompositorResize(int width, int height, int depth, int row_bytes,
						  int pitch, void *buffer, uint32_t buffer_size)
{ /* host window change == w/h/flags changed; recreate everything */
	if (!s_init || s_gl_device_window != GL_HOST_WINDOW)
		return MetalCompositorInit(width, height, depth,
			row_bytes, pitch, buffer, buffer_size);
	if (!SharedMetalDevice())
	{
		COMPOSITOR_LOG("couldn't SharedMetalDevice()!");
		return -1;
	}

	MetalCompositorSubmitFrame_ClearCachedOverlay();
	MetalCompositorSubmitFrame_ClearCachedFramebuffer();
	compositor_pixel_width = width;
	compositor_pixel_height = height;
	compositor_depth = depth;
	compositor_row_bytes = row_bytes;
	compositor_pitch = pitch;
	compositor_buffer = buffer;
	compositor_buffer_size = buffer_size;
	compositor_bits_per_pixel = MetalCompositorDepthToBPPBits(depth);
	s_palette_5bit_lut_dirty = true;
	MetalCompositorEnsureFrameBufferTexture();
	COMPOSITOR_LOG("Resize %dx%d depth=%d", width, height, depth);
	return 0;
}

int MetalCompositorIsInitialized(void)
{
	return s_init ? 1 : 0;
}

/* Guest address and size of the current visible screen surface. Consumed by
 * the NQD CPU blit path (nqd_gl_renderer.cpp) to validate screen-destined
 * surface ranges; the screen lives outside guest RAM. Emul-thread only, like
 * the NQD hooks and mode switches that update compositor_buffer. */
int MetalCompositorGetGuestSurface(uint32_t *out_mac_base, uint32_t *out_byte_size)
{
	if (!s_init || compositor_buffer == NULL || compositor_buffer_size == 0)
		return -1;
	*out_mac_base = Host2MacAddr((uint8 *)compositor_buffer);
	*out_byte_size = compositor_buffer_size;
	return 0;
}

int MetalCompositorCurrentMode(int *out_width, int *out_height, int *out_depth)
{
	if (!s_init)
		return 0;
	if (out_width)  *out_width  = compositor_pixel_width;
	if (out_height) *out_height = compositor_pixel_height;
	if (out_depth)  *out_depth  = compositor_depth;
	return 1;
}

int32_t MetalCompositorSubmitFrame(const struct FrameDescriptor *desc)
{
	if (!desc || desc->layer_count > kGfxAccelMaxLayers ||
		(desc->layer_count != 0 && !desc->layers)) {
		return kGfxAccelErrInvalidDescriptor;
	}

	const DMCModeSnapshot *snap = dmc_current_snapshot();
	if (snap && desc->generation != snap->generation)
		return kGfxAccelErrStaleGeneration;

	/*
	 * Screen publication boundary only. RAVE/GL/Glide resolve into guest
	 * VRAM here; private engine surfaces never enter this path. One
	 * HideCursor/ShowCursor nest so soft-cursor underbits survive the
	 * opaque replace (Bugdom RAVE + later QuickDraw). hardcursor: no-op.
	 */
	const bool publish = desc->layer_count > 0 &&
		compositor_pixel_width > 0 && compositor_pixel_height > 0;
	if (publish) {
		video_screen_publish_begin(0, 0, compositor_pixel_width,
								   compositor_pixel_height);
	}

	int32_t first_err = kGfxAccelNoErr;
	for (uint32_t i = 0; i < desc->layer_count; i++) {
		const CompositeLayer *L = &desc->layers[i];
		if ((uint32_t)L->slot >= (uint32_t)kLayerSlotCount) {
			first_err = kGfxAccelErrInvalidSlot;
			break;
		}
		const int32_t rc = publish_layer_to_guest_screen(L);
		if (rc != kGfxAccelNoErr) {
			first_err = rc;
			break;
		}
	}

	if (publish)
		video_screen_publish_end();

	if (first_err != kGfxAccelNoErr)
		return first_err;
#if QD3D_GRAPHICS_LOGGING_ENABLED
	s_screen_submit_count++;
	if (MetalCompositorShouldTracePresent(s_screen_submit_count)) {
		QD3D_RENDER_LOG("CompositorSubmit count=%llu layers=%u generation=%llu",
						(unsigned long long)s_screen_submit_count,
						desc->layer_count, (unsigned long long)desc->generation);
	}
#endif
	return kGfxAccelNoErr;
}

void MetalCompositorPresent(void)
{
	MetalCompositorPresent_();
}
void MetalCompositorPresentWithoutVBL(void)
{
	MetalCompositorPresent_(false);
}

void MetalCompositorPresentAdopted(void)
{
	MetalCompositorPresent_(false, false);
}

int MetalCompositorAdoptFrame(unsigned int readframebuffer, int width, int height)
{
	GfxGLExt &ext = gfx_gl_ext();
	if (!s_init || !ext.BlitFramebuffer || width != compositor_pixel_width || height != compositor_pixel_height)
		return 0;
	ScopedGLScreenTransfer screen_transfer;
	MetalCompositorEnsureFrameBufferTexture();
	if (!bind_screen_write_target())
		return 0;
	ext.BindFramebuffer(GL_READ_FRAMEBUFFER, readframebuffer);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glDisable(GL_SCISSOR_TEST);
	ext.BlitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	unbind_screen_write_target();
	return 1;
}

#if !defined(GFXACCEL_USE_SDLGPU)
int MetalCompositorSetGamma(const uint8_t *table)
{
	static const char program[] =
		"!!ARBfp1.0\n"
		"PARAM scale = { 0.99609375, 0.001953125, 0, 0 };\n"
		"TEMP color, lookup, red, green, blue;\n"
		"TEX color, fragment.texcoord[0], texture[0], 2D;\n"
		"MAD lookup, color, scale.xxxx, scale.yyyy;\n"
		"TEX red, lookup.xxxx, texture[1], 1D;\n"
		"TEX green, lookup.yyyy, texture[1], 1D;\n"
		"TEX blue, lookup.zzzz, texture[1], 1D;\n"
		"MOV result.color.x, red.x;\n"
		"MOV result.color.y, green.y;\n"
		"MOV result.color.z, blue.z;\n"
		"MOV result.color.w, color.w;\n"
		"END\n";
	uint8_t texels[256 * 4];
	GLint errorposition;
	int index;
	if (!table) {
		if (s_gamma_texture) {
			glDeleteTextures(1, &s_gamma_texture);
			s_gamma_texture = 0;
		}
		return 1;
	}
	GfxGLExt &ext = gfx_gl_ext();
	if (!s_init || !ext.GenPrograms || !ext.BindProgram || !ext.ProgramString || !ext.ActiveTexture)
		return 0;
	ScopedGLScreenTransfer screen_transfer;
	if (!s_gamma_program) {
		ext.GenPrograms(1, &s_gamma_program);
		ext.BindProgram(GL_FRAGMENT_PROGRAM_ARB, s_gamma_program);
		ext.ProgramString(GL_FRAGMENT_PROGRAM_ARB, GL_PROGRAM_FORMAT_ASCII_ARB, (GLsizei)(sizeof(program) - 1), program);
		glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB, &errorposition);
		ext.BindProgram(GL_FRAGMENT_PROGRAM_ARB, 0);
		if (errorposition != -1) {
			s_gamma_program = 0;
			return 0;
		}
	}
	for (index = 0; index < 256; index++) {
		texels[index * 4] = table[index];
		texels[index * 4 + 1] = table[256 + index];
		texels[index * 4 + 2] = table[512 + index];
		texels[index * 4 + 3] = 255;
	}
	if (!s_gamma_texture) {
		glGenTextures(1, &s_gamma_texture);
		glBindTexture(GL_TEXTURE_1D, s_gamma_texture);
		glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	}
	glBindTexture(GL_TEXTURE_1D, s_gamma_texture);
	glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA8, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	return 1;
}

int MetalCompositorReadFrame(uint8_t *pixels, int rowbytes)
{
	if (!s_init || !s_fb_tex || !pixels)
		return 0;
	ScopedGLScreenTransfer screen_transfer;
	glBindTexture(GL_TEXTURE_2D, s_fb_tex);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glPixelStorei(GL_PACK_ROW_LENGTH, rowbytes / COMPOSITORGUESTPIXELBYTES);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, COMPOSITORGUESTPIXELTYPE, pixels);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	return 1;
}
#else
int MetalCompositorSetGamma(const uint8_t *table)
{
	return table == NULL;
}

int MetalCompositorReadFrame(uint8_t *pixels, int rowbytes)
{
	(void)pixels;
	(void)rowbytes;
	return 0;
}
#endif

int32_t MetalCompositorSync3DFramePacingForEngine(int32_t engine_id)
{
	return vbl_source_sync_3d_pacing_for_engine(engine_id);
}

void MetalCompositorSubmitFrame_SetTargetTimestamp(double /*ts*/)
{
}

int MetalCompositorSubmitFrame_AcquireCachedOverlay(struct CompositeLayer *out_layer,
													void **out_tex_retained)
{
	if (out_layer) std::memset(out_layer, 0, sizeof(*out_layer));
	if (out_tex_retained) *out_tex_retained = NULL;
	return 0;
}

void MetalCompositorSubmitFrame_ReleaseCachedOverlay(void * /*tex_retained*/)
{
	/* OpenGL textures are not refcounted here. */
}

void MetalCompositorSubmitFrame_EncodeCachedOverlay(void * /*render_encoder*/,
													const struct CompositeLayer *layer,
													void * /*display_gamma_lut*/)
{
	(void)layer;
}

void MetalCompositorSubmitFrame_ClearCachedOverlay(void)
{
}

void MetalCompositorSubmitFrame_ClearCachedFramebuffer(void)
{
}

/* GL-only: flush a Present that was deferred while the shared context was
 * owned by a guest 3D render pass (or a nested present). Metal has no
 * equivalent because it encodes without borrowing the 3D engine's command
 * stream. */
extern "C" void MetalCompositorFlushDeferredPresent(void)
{
	if (!s_init || !s_present_deferred)
		return;
	if (s_present_in_progress || MetalIs3DRenderPassActive())
		return;
	MetalCompositorPresent();
}

void *MetalCompositorGetLayer(void)
{
	return s_init ? (void *)(uintptr_t)1 : NULL;
}

int MetalCompositorGetScreenSurface(GfxAccelScreenSurface *out_surface)
{
	if (!out_surface || !s_init || !compositor_buffer)
		return 0;
	out_surface->mac_base = Host2MacAddr((uint8 *)compositor_buffer);
	out_surface->host_base = compositor_buffer;
	out_surface->byte_size = compositor_buffer_size;
	out_surface->width = compositor_pixel_width;
	out_surface->height = compositor_pixel_height;
	out_surface->depth = compositor_depth;
	out_surface->row_bytes = compositor_row_bytes;
	return out_surface->mac_base != 0 ? 1 : 0;
}

void MetalCompositorPaletteLatch(void)
{
	/* Palette storage is immediate on desktop. Gamma follows Metal's VBL
	 * snapshot latch so a fade's LUT and fade_active flag become visible as
	 * one display-policy update without modifying canonical screen bytes. */
	s_palette_dirty = false;
	const DMCModeSnapshot *snap = dmc_current_snapshot();
	if (snap != NULL && snap->gamma_gen != s_last_gamma_gen) {
		GfxColorBuildDisplayGammaLUT(
			snap->gamma_lut,
			snap->fade_active != 0,
			!GLCompositorIsLinearGamma(),
			s_gamma_lut);
		s_last_gamma_gen = snap->gamma_gen;
	}
}

void MetalCompositorUpdateGammaLUT(const uint8_t *lut)
{
	if (!lut) return;
	GfxColorBuildDisplayGammaLUT(lut, false,
								 !GLCompositorIsLinearGamma(),
								 s_gamma_lut);
}

#if !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
void MetalCompositorRefreshPresentRect(void)
{
	int window_w = 0, window_h = 0;
	HostGLWindowSize(&window_w, &window_h);
	int drawable_w = 0, drawable_h = 0;
	HostGLDrawableSize(&drawable_w, &drawable_h);
	int x = 0, y = 0, w = drawable_w, h = drawable_h;
	if (!video_get_framebuffer_drawable_rect(&x, &y, &w, &h)) {
		x = y = 0;
		w = drawable_w;
		h = drawable_h;
	}
	if (drawable_w > 0 && drawable_h > 0) {
		x = (int)round_to_long((double)x * window_w / drawable_w);
		y = (int)round_to_long((double)y * window_h / drawable_h);
		w = (int)round_to_long((double)w * window_w / drawable_w);
		h = (int)round_to_long((double)h * window_h / drawable_h);
	}
	atomic_store_explicit(&s_present_origin,
		((uint64_t)(uint32_t)x << 32) | (uint32_t)y,
		memory_order_relaxed);
	atomic_store_explicit(&s_present_size,
		((uint64_t)(uint32_t)w << 32) | (uint32_t)h, memory_order_relaxed);
}

void MetalCompositorGetPresentRect(int *out_x, int *out_y, int *out_w, int *out_h)
{
	uint64_t o = atomic_load_explicit(&s_present_origin, memory_order_relaxed);
	uint64_t s = atomic_load_explicit(&s_present_size, memory_order_relaxed);
	if (out_x) *out_x = (int)(uint32_t)(o >> 32);
	if (out_y) *out_y = (int)(uint32_t)(o & 0xffffffffu);
	if (out_w) *out_w = (int)(uint32_t)(s >> 32);
	if (out_h) *out_h = (int)(uint32_t)(s & 0xffffffffu);
}
#endif

/* Internal helpers used by Metal submitframe module - provide stubs. */
extern "C" int MetalCompositorSubmitFrame_BindPresentationContext(
	void *, void *, void *) { return 0; }
extern "C" void MetalCompositorSubmitFrame_UnbindPresentationContext(void) {}
