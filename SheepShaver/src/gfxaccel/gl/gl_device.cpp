/*
 *  gl_device.cpp - Shared OpenGL + SDL context for gfxaccel
 */

#include "sysdeps.h"
#include "gl_device.h"
#include "gfx_log.h"

#include <SDL.h>
#include <SDL_opengl.h>

#include <cassert>
#include <cstdio>
#include <cstring>

extern SDL_Window *sdl_window;

static SDL_GLContext s_gl_ctx = nullptr;
static bool s_ready = false;

/* Opaque sentinels so code that null-checks SharedMetalDevice() still works. */
static char s_device_sentinel = 1;
static char s_queue_sentinel = 1;

bool GfxGLDeviceInit(void)
{
	QD3D_INIT_LOG("GfxGLDeviceInit: ready=%d context=%p window=%p",
	              s_ready, s_gl_ctx, (void *)sdl_window);
	if (s_ready && s_gl_ctx)
	{
		QD3D_INIT_LOG("GfxGLDeviceInit: reusing existing context");
		return true;
	}

	if (!sdl_window) {
		QD3D_INIT_LOG("GfxGLDeviceInit: FAILED because SDL window is null");
		fprintf(stderr, "[gfxaccel-gl] GfxGLDeviceInit: sdl_window is NULL\n");
		return false;
	}

	/* Prefer a compatibility profile so Mac GL 1.2 FFP maps cleanly. */
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
#if defined(SDL_GL_CONTEXT_PROFILE_MASK)
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
#endif

	if (s_gl_ctx) {
		SDL_GL_DeleteContext(s_gl_ctx);
		s_gl_ctx = nullptr;
	}

	s_gl_ctx = SDL_GL_CreateContext(sdl_window);
	if (!s_gl_ctx) {
		QD3D_INIT_LOG("GfxGLDeviceInit: SDL_GL_CreateContext FAILED: %s", SDL_GetError());
		fprintf(stderr, "[gfxaccel-gl] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
		s_ready = false;
		return false;
	}

	if (SDL_GL_MakeCurrent(sdl_window, s_gl_ctx) != 0) {
		QD3D_INIT_LOG("GfxGLDeviceInit: SDL_GL_MakeCurrent FAILED: %s", SDL_GetError());
		fprintf(stderr, "[gfxaccel-gl] SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
		SDL_GL_DeleteContext(s_gl_ctx);
		s_gl_ctx = nullptr;
		s_ready = false;
		return false;
	}

	/* Never block the emulator thread on vsync. Present is invoked from
	 * VideoVBL on the emul thread; SwapInterval(1) + full-frame uploads
	 * at 2560x1440 starves PPC execution and looks like a hard lockup. */
	SDL_GL_SetSwapInterval(0);

	const char *vendor = (const char *)glGetString(GL_VENDOR);
	const char *renderer = (const char *)glGetString(GL_RENDERER);
	const char *version = (const char *)glGetString(GL_VERSION);
	fprintf(stderr, "[gfxaccel-gl] OpenGL ready: %s / %s / %s\n",
	        vendor ? vendor : "?",
	        renderer ? renderer : "?",
	        version ? version : "?");

	s_ready = true;
	QD3D_INIT_LOG("GfxGLDeviceInit: SUCCESS context=%p vendor='%s' renderer='%s' version='%s'",
	              s_gl_ctx, vendor ? vendor : "?", renderer ? renderer : "?",
	              version ? version : "?");
	return true;
}

bool GfxGLDeviceMakeCurrent(void)
{
	assert(s_ready);
	assert(s_gl_ctx != nullptr);
	assert(sdl_window != nullptr);
	/* RAVE calls this at public API boundaries and resource uploads, often
	 * hundreds of times per frame. SDL_GL_MakeCurrent enters the driver even
	 * when nothing changed; avoid that round trip on the single render thread. */
	if (SDL_GL_GetCurrentContext() == s_gl_ctx &&
	    SDL_GL_GetCurrentWindow() == sdl_window)
		return true;
	return SDL_GL_MakeCurrent(sdl_window, s_gl_ctx) == 0;
}

void GfxGLDeviceReleaseCurrent(void)
{
	/* Unbind the GL context from the calling thread so another thread can make
	 * it current. Used around an in-place mode switch: the redraw thread (which
	 * normally owns the context) releases it at its park point so the emul
	 * thread can reformat compositor resources, then re-binds on resume. */
	if (s_gl_ctx && sdl_window)
		SDL_GL_MakeCurrent(sdl_window, nullptr);
}

void GfxGLDeviceShutdown(void)
{
	if (s_gl_ctx) {
		if (sdl_window)
			SDL_GL_MakeCurrent(sdl_window, nullptr);
		SDL_GL_DeleteContext(s_gl_ctx);
		s_gl_ctx = nullptr;
	}
	s_ready = false;
}

bool GfxGLDeviceIsReady(void)
{
	return s_ready && s_gl_ctx != nullptr;
}

void GfxGLDeviceSwap(void)
{
	assert(s_ready);
	assert(s_gl_ctx != nullptr);
	assert(sdl_window != nullptr);
	SDL_GL_SwapWindow(sdl_window);
}

void GfxGLDeviceGetDrawableSize(int *out_w, int *out_h)
{
	assert(s_ready);
	assert(s_gl_ctx != nullptr);
	assert(sdl_window != nullptr);
	assert(out_w != nullptr);
	assert(out_h != nullptr);
	SDL_GL_GetDrawableSize(sdl_window, out_w, out_h);
}

void *SharedMetalDevice(void)
{
	if (!s_ready)
		GfxGLDeviceInit();
	return s_ready ? (void *)&s_device_sentinel : nullptr;
}

void *SharedMetalCommandQueue(void)
{
	if (!s_ready)
		GfxGLDeviceInit();
	return s_ready ? (void *)&s_queue_sentinel : nullptr;
}

void MetalValidation_InstallErrorHandler(void * /*cmdBufPtr*/)
{
	/* no-op on OpenGL */
}
