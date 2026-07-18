/*
 *  gfxaccel_backend.h - Graphics acceleration GPU backend selection
 *
 *  (C) 2026 Sierra Burkhart (sierra760) / OpenGL+SDL port
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Selects Metal (Apple) vs OpenGL+SDL (desktop / non-Apple).
 *
 *  CMake defines exactly one of:
 *    GFXACCEL_USE_METAL
 *    GFXACCEL_USE_OPENGL
 *
 *  If neither is defined, defaults are:
 *    Apple platforms -> Metal
 *    everything else -> OpenGL
 */

#ifndef GFXACCEL_BACKEND_H
#define GFXACCEL_BACKEND_H

#if defined(GFXACCEL_USE_METAL) && defined(GFXACCEL_USE_OPENGL)
#error "Define only one of GFXACCEL_USE_METAL or GFXACCEL_USE_OPENGL"
#endif

#if !defined(GFXACCEL_USE_METAL) && !defined(GFXACCEL_USE_OPENGL)
#  if defined(__APPLE__)
#    define GFXACCEL_USE_METAL 1
#  else
#    define GFXACCEL_USE_OPENGL 1
#  endif
#endif

#if defined(GFXACCEL_USE_METAL)
#  define GFXACCEL_BACKEND_NAME "Metal"
#elif defined(GFXACCEL_USE_OPENGL)
#  define GFXACCEL_BACKEND_NAME "OpenGL+SDL"
#endif

/*
 *  Shared opaque GPU resource handle vocabulary.
 *  Metal backends store id<MTLTexture>/id<MTLBuffer> as void*.
 *  OpenGL backends store GLuint (or host-pointer wrappers) as void*.
 */
typedef void *GfxGpuTexture;
typedef void *GfxGpuBuffer;
typedef void *GfxGpuDevice;
typedef void *GfxGpuCommandQueue;

/*
 *  Pixel formats used across engines/compositor (independent of Metal enums).
 *  OpenGL maps these to GL_* formats; Metal maps to MTLPixelFormat*.
 */
typedef enum {
	kGfxPixelFormatBGRA8Unorm = 0,
	kGfxPixelFormatR8Uint     = 1,
	kGfxPixelFormatR16Uint    = 2,
	kGfxPixelFormatDepth32F   = 3,
	kGfxPixelFormatRGBA8Unorm = 4
} GfxPixelFormat;

/* Historical Metal format values used at some call sites (BGRA8Unorm = 80). */
#ifndef MTLPixelFormatBGRA8Unorm
#define MTLPixelFormatBGRA8Unorm 80u
#endif

#endif /* GFXACCEL_BACKEND_H */
