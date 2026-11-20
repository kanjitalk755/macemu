/*
 *  gfxaccel_resources_gl.cpp - OpenGL side of resource manager
 *  (implements symbols from gfxaccel_resources.mm)
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
#include "gfxaccel_resources.h"
#include "gl_ext.h"
#include "vbl_source.h"
#include "metal_device_shared.h"

#if USE_SDL3
#include <SDL3/SDL_opengl.h>
#else
#include <SDL_opengl.h>
#endif
#include <cstdio>
#include <cstring>

struct OverlaySlot {
	GLuint tex[2];
	uint32_t w, h, format;
};

static OverlaySlot s_overlays[kGfxEngineCount];
static GfxAccelLifecycleHookFn s_bg_hook = NULL, s_fg_hook = NULL;
static void *s_bg_ctx = NULL, *s_fg_ctx = NULL;
static void *s_fb_host = NULL;

extern "C" void gfxaccel_resources_mm_init_metal_state(void)
{
	std::memset(s_overlays, 0, sizeof(s_overlays));
	s_fb_host = NULL;
}

extern "C" void gfxaccel_resources_mm_shutdown_metal_state(void)
{
	if (SharedMetalDevice()) {
		for (int e = 0; e < kGfxEngineCount; e++) {
			for (int i = 0; i < 2; i++) {
				if (s_overlays[e].tex[i]) {
					glDeleteTextures(1, &s_overlays[e].tex[i]);
					s_overlays[e].tex[i] = 0;
				}
			}
		}
	}
	std::memset(s_overlays, 0, sizeof(s_overlays));
}

void *gfxaccel_resources_get_framebuffer_buffer(void *host_base, uint32_t /*length*/)
{
	s_fb_host = host_base;
	return host_base;
}

static GLuint make_tex(uint32_t w, uint32_t h)
{
	if (!SharedMetalDevice()) return 0;
	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
				 GL_BGRA, GL_UNSIGNED_BYTE, NULL);
	return tex;
}

void *gfxaccel_resources_vend_overlay_texture(uint32_t engine_id,
											  uint32_t width, uint32_t height,
											  uint32_t pixel_format)
{
	return gfxaccel_resources_vend_overlay_texture_indexed(engine_id, 0, width, height, pixel_format);
}

void *gfxaccel_resources_vend_overlay_texture_indexed(uint32_t engine_id,
													  uint32_t texture_index,
													  uint32_t width, uint32_t height,
													  uint32_t pixel_format)
{
	if (engine_id >= kGfxEngineCount || texture_index > 1) return NULL;
	OverlaySlot &s = s_overlays[engine_id];
	if (s.w != width || s.h != height || s.format != pixel_format) {
		for (int i = 0; i < 2; i++) {
			if (s.tex[i] && SharedMetalDevice())
				glDeleteTextures(1, &s.tex[i]);
			s.tex[i] = 0;
		}
		s.w = width; s.h = height; s.format = pixel_format;
	}
	if (!s.tex[texture_index]) {
		s.tex[texture_index] = make_tex(width, height);
		if (!s.tex[texture_index]) return NULL;
	}
	return (void *)(uintptr_t)s.tex[texture_index];
}

void gfxaccel_resources_release_overlay_texture(uint32_t engine_id, void *texture)
{
	if (engine_id >= kGfxEngineCount || !texture) return;
	GLuint tex = (GLuint)(uintptr_t)texture;
	OverlaySlot &s = s_overlays[engine_id];
	for (int i = 0; i < 2; i++) {
		if (s.tex[i] == tex) {
			if (SharedMetalDevice()) glDeleteTextures(1, &s.tex[i]);
			s.tex[i] = 0;
		}
	}
}

void gfxaccel_resources_set_buffer_owner(void *, uint32_t) {}
void gfxaccel_resources_clear_buffer_owner(void *) {}

void gfxaccel_handle_background_enter(void)
{
	vbl_source_set_paused(1);
	if (s_bg_hook) s_bg_hook(s_bg_ctx);
}
void gfxaccel_handle_foreground_enter(void)
{
	vbl_source_set_paused(0);
	if (s_fg_hook) s_fg_hook(s_fg_ctx);
}
void gfxaccel_set_dsp_background_hook(GfxAccelLifecycleHookFn fn, void *ctx)
{ s_bg_hook = fn; s_bg_ctx = ctx; }
void gfxaccel_set_dsp_foreground_hook(GfxAccelLifecycleHookFn fn, void *ctx)
{ s_fg_hook = fn; s_fg_ctx = ctx; }
