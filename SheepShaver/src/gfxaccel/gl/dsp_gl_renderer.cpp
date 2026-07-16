/*
 *  dsp_gl_renderer.cpp - DSp back-buffer via host memory + compositor present
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "dsp_engine.h"
#include "dsp_metal_renderer.h"
#include "dsp_context_private.h"
#include "gfxaccel_resources.h"
#include "gfxaccel_resources_heap.h"
#include "metal_compositor.h"
#include "gl_device.h"
#include "macos_util.h"
#include "video.h"

#include <SDL_opengl.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>

#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

extern "C" void gfxaccel_resources_heap_mm_free_buffer(uint32_t heap_id, void *ptr);

bool DSpAllocateBackBuffer(DSpContextPrivate *ctx, uint32_t w, uint32_t h, uint32_t bpp)
{
	if (!ctx || !w || !h) return false;
	uint32_t bytes_pp = (bpp <= 8) ? 1 : (bpp <= 16) ? 2 : 4;
	uint32_t row = w * bytes_pp;
	uint32_t size = row * h;
	void *buf = gfxaccel_resources_heap_alloc_buffer(kHeapEngineDSp, size, 0);
	if (!buf) return false;

	GLuint tex = 0;
	if (GfxGLDeviceMakeCurrent()) {
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
		             GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
	}

	ctx->back_buffer = buf;
	ctx->back_texture = (void *)(uintptr_t)tex;
	ctx->dirty_empty = true;
	ctx->dirty_cold_start = true;
	ctx->attr.displayWidth = w;
	ctx->attr.displayHeight = h;
	if (!ctx->attr.colorNeeds) ctx->attr.colorNeeds = bpp;
	gfxaccel_resources_set_buffer_owner(buf, kGfxEngineDSp);
	return true;
}

void DSpReleaseBackBufferNow(DSpContextPrivate *ctx)
{
	if (!ctx) return;
	if (ctx->back_texture && GfxGLDeviceMakeCurrent()) {
		GLuint tex = (GLuint)(uintptr_t)ctx->back_texture;
		glDeleteTextures(1, &tex);
	}
	if (ctx->back_buffer) {
		gfxaccel_resources_clear_buffer_owner(ctx->back_buffer);
		gfxaccel_resources_heap_mm_free_buffer(kHeapEngineDSp, ctx->back_buffer);
		ctx->back_buffer = nullptr;
	}
	ctx->back_texture = nullptr;
}

void DSpReleaseBackBufferStaging(DSpContextPrivate *ctx)
{
	if (!ctx) return;
	if (ctx->front_staging_owned_sysheap && ctx->front_staging_mac_addr) {
		/* SheepMem/Mac_sysalloc has no free; leave until mode change */
		ctx->front_staging_mac_addr = 0;
		ctx->front_staging_size = 0;
		ctx->front_staging_owned_sysheap = false;
	}
}

/* Expand back buffer into tightly packed RGBA for GL texture upload. */
static void expand_back_to_rgba(const DSpContextPrivate *ctx, std::vector<uint8_t> &out)
{
	uint32_t w = ctx->attr.displayWidth ? ctx->attr.displayWidth : 640;
	uint32_t h = ctx->attr.displayHeight ? ctx->attr.displayHeight : 480;
	uint32_t bpp = ctx->attr.colorNeeds ? ctx->attr.colorNeeds : 32;
	uint32_t bytes_pp = (bpp <= 8) ? 1 : (bpp <= 16) ? 2 : 4;
	uint32_t row = w * bytes_pp;
	const uint8_t *src = (const uint8_t *)ctx->back_buffer;
	out.resize((size_t)w * h * 4);
	if (!src) {
		std::memset(out.data(), 0, out.size());
		return;
	}
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *srow = src + (size_t)y * row;
		uint8_t *drow = out.data() + (size_t)y * w * 4;
		for (uint32_t x = 0; x < w; x++) {
			if (bytes_pp == 4) {
				/* Guest BE ARGB */
				drow[x * 4 + 0] = srow[x * 4 + 1];
				drow[x * 4 + 1] = srow[x * 4 + 2];
				drow[x * 4 + 2] = srow[x * 4 + 3];
				drow[x * 4 + 3] = 255;
			} else if (bytes_pp == 2) {
				uint16_t be = (uint16_t)((srow[x * 2] << 8) | srow[x * 2 + 1]);
				uint8_t R = (uint8_t)(((be >> 10) & 0x1f) * 255 / 31);
				uint8_t G = (uint8_t)(((be >> 5) & 0x1f) * 255 / 31);
				uint8_t B = (uint8_t)((be & 0x1f) * 255 / 31);
				drow[x * 4 + 0] = R; drow[x * 4 + 1] = G; drow[x * 4 + 2] = B; drow[x * 4 + 3] = 255;
			} else {
				uint8_t i = srow[x];
				/* Use latched CLUT when available (identity fallback) */
				const uint8_t *pal = ctx->clut_bytes_latched;
				drow[x * 4 + 0] = pal[i * 3 + 0];
				drow[x * 4 + 1] = pal[i * 3 + 1];
				drow[x * 4 + 2] = pal[i * 3 + 2];
				drow[x * 4 + 3] = 255;
			}
		}
	}
}

void DSpEncodeBackBufferBlit(DSpContextPrivate *ctx, void * /*encoder*/, void * /*framebuffer_texture*/)
{
	if (!ctx || !ctx->back_buffer) return;

	/* Upload back buffer into GL texture and cache as compositor overlay */
	if (ctx->back_texture && GfxGLDeviceMakeCurrent()) {
		std::vector<uint8_t> rgba;
		expand_back_to_rgba(ctx, rgba);
		uint32_t w = ctx->attr.displayWidth ? ctx->attr.displayWidth : 640;
		uint32_t h = ctx->attr.displayHeight ? ctx->attr.displayHeight : 480;
		GLuint tex = (GLuint)(uintptr_t)ctx->back_texture;
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

		/* Present as full-screen overlay so games using DSp page flip show up */
		CompositeLayer layer = {};
		layer.source = (void *)(uintptr_t)tex;
		layer.src_size_w = w;
		layer.src_size_h = h;
		layer.dst_origin_x = 0;
		layer.dst_origin_y = 0;
		layer.dst_size_w = (float)w;
		layer.dst_size_h = (float)h;
		layer.slot = kLayerSlotOverlay;
		layer.blend = kBlendOpaque;
		layer.alpha = 1.f;
		FrameDescriptor desc = {};
		desc.layers = &layer;
		desc.layer_count = 1;
		const DMCModeSnapshot *snap = dmc_current_snapshot();
		desc.generation = snap ? snap->generation : 0;
		MetalCompositorSubmitFrame(&desc);
	}

	/* Also mirror into Mac main framebuffer when base matches screen */
	if (ctx->front_staging_mac_addr && ctx->front_staging_size && ctx->back_buffer) {
		/* staging already copied to back_buffer at SwapBuffers; if screen_base
		 * points at the same region, compositor will show it on next present. */
	}

	ctx->dirty_empty = true;
	ctx->dirty_cold_start = false;
	ctx->dirty_left = ctx->dirty_top = ctx->dirty_right = ctx->dirty_bottom = 0;
}

void DSpEncodePresentToFramebuffer(DSpContextPrivate *ctx, void *command_buffer, void *framebuffer_texture)
{
	DSpEncodeBackBufferBlit(ctx, command_buffer, framebuffer_texture);
}

bool DSpEncodeFrontBufferStagingToFramebuffer(DSpContextPrivate *ctx, void *, void *)
{
	if (!ctx) return false;
	DSpEncodeBackBufferBlit(ctx, nullptr, nullptr);
	return true;
}

uint32_t DSpGetBackBufferCGrafPtr(DSpContextPrivate *ctx)
{
	if (!ctx) return 0;
	if (ctx->cgrafptr_mac_addr) return ctx->cgrafptr_mac_addr;
	return 0;
}

uint32_t DSpGuardStagingWrite(uint32_t /*mac_addr*/, uint32_t size, const char * /*site*/)
{
	return size;
}
