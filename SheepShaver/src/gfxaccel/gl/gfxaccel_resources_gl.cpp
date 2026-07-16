/*
 *  gfxaccel_resources_gl.cpp - OpenGL side of resource manager
 *  (implements symbols from gfxaccel_resources.mm)
 */

#include "sysdeps.h"
#include "gfxaccel_resources.h"
#include "gl_device.h"
#include "gl_ext.h"
#include "vbl_source.h"
#include "metal_compositor.h"

#include <SDL_opengl.h>
#include <cstdio>
#include <cstring>

struct OverlaySlot {
	GLuint tex[2];
	uint32_t w, h, format;
};

static OverlaySlot s_overlays[kGfxEngineCount];
static GfxAccelLifecycleHookFn s_bg_hook = nullptr, s_fg_hook = nullptr;
static void *s_bg_ctx = nullptr, *s_fg_ctx = nullptr;
static void *s_fb_host = nullptr;

extern "C" void gfxaccel_resources_mm_init_metal_state(void)
{
	std::memset(s_overlays, 0, sizeof(s_overlays));
	s_fb_host = nullptr;
}

extern "C" void gfxaccel_resources_mm_shutdown_metal_state(void)
{
	if (GfxGLDeviceMakeCurrent()) {
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
	if (!GfxGLDeviceMakeCurrent()) return 0;
	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
	             GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
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
	if (engine_id >= kGfxEngineCount || texture_index > 1) return nullptr;
	OverlaySlot &s = s_overlays[engine_id];
	if (s.w != width || s.h != height || s.format != pixel_format) {
		for (int i = 0; i < 2; i++) {
			if (s.tex[i] && GfxGLDeviceMakeCurrent())
				glDeleteTextures(1, &s.tex[i]);
			s.tex[i] = 0;
		}
		s.w = width; s.h = height; s.format = pixel_format;
	}
	if (!s.tex[texture_index]) {
		s.tex[texture_index] = make_tex(width, height);
		if (!s.tex[texture_index]) return nullptr;
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
			if (GfxGLDeviceMakeCurrent()) glDeleteTextures(1, &s.tex[i]);
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
