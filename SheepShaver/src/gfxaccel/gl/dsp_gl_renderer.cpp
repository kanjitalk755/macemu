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
#include "dsp_back_buffer_range.h"
#include "dsp_back_buffer_cgraf_policy.h"
#include "dsp_cgraf_port_policy.h"
#include "dsp_pixmap_offsets.h"
#include "gfx_log.h"

#include <SDL_opengl.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>
#include <vector>

#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

extern "C" void gfxaccel_resources_heap_mm_free_buffer(uint32_t heap_id, void *ptr);

static bool dsp_valid_depth(uint32_t depth)
{
	return depth == 1 || depth == 2 || depth == 4 || depth == 8 ||
	       depth == 16 || depth == 32;
}

static uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b)
{
	/* The desktop OpenGL backends are little-endian; this integer layout is
	 * byte-for-byte RGBA when uploaded with GL_UNSIGNED_BYTE. */
	return (uint32_t)r | ((uint32_t)g << 8) |
	       ((uint32_t)b << 16) | 0xff000000u;
}

static const uint32_t *rgb555_gamma_lut(const uint8_t *gamma)
{
	static std::array<uint32_t, 32768> lut;
	static std::array<uint8_t, 768> gamma_snapshot;
	static bool valid = false;
	bool changed = !valid;
	if (!changed) {
		for (uint32_t i = 0; i < gamma_snapshot.size(); i++) {
			const uint8_t value = gamma ? gamma[i] : (uint8_t)(i & 255u);
			if (gamma_snapshot[i] != value) {
				changed = true;
				break;
			}
		}
	}
	if (changed) {
		for (uint32_t i = 0; i < gamma_snapshot.size(); i++)
			gamma_snapshot[i] = gamma ? gamma[i] : (uint8_t)(i & 255u);
		for (uint32_t value = 0; value < lut.size(); value++) {
			const uint8_t r = (uint8_t)(((value >> 10) & 31u) * 255u / 31u);
			const uint8_t g = (uint8_t)(((value >> 5) & 31u) * 255u / 31u);
			const uint8_t b = (uint8_t)((value & 31u) * 255u / 31u);
			lut[value] = pack_rgba(gamma_snapshot[r],
			                       gamma_snapshot[256u + g],
			                       gamma_snapshot[512u + b]);
		}
		valid = true;
	}
	return lut.data();
}

static bool dsp_back_buffer_layout(uint32_t w, uint32_t h, uint32_t depth,
	                               uint32_t *out_row_bytes,
	                               uint32_t *out_size)
{
	if (!w || !h || !dsp_valid_depth(depth)) return false;
	const uint32_t row_bytes = DSpBackBufferAlignedRowBytes(w, depth);
	const uint64_t size = (uint64_t)row_bytes * (uint64_t)h;
	if (!row_bytes || size > std::numeric_limits<uint32_t>::max()) return false;
	if (out_row_bytes) *out_row_bytes = row_bytes;
	if (out_size) *out_size = (uint32_t)size;
	return true;
}

static void dsp_pixmap_format(uint32_t depth, uint16_t *pixel_type,
	                          uint16_t *pixel_size, uint16_t *component_count,
	                          uint16_t *component_size)
{
	*pixel_type = depth <= 8 ? 0 : 0x10;
	*pixel_size = (uint16_t)depth;
	*component_count = depth <= 8 ? 1 : 3;
	*component_size = depth <= 8 ? (uint16_t)depth :
	                  depth == 16 ? 5 : 8;
}

static uint32_t dsp_create_rect_region(uint32_t w, uint32_t h)
{
	const uint32_t region = Mac_sysalloc(DSP_RECT_REGION_SIZE);
	const uint32_t handle = Mac_sysalloc(4);
	if (!region || !handle) return 0;
	Mac_memset(region, 0, DSP_RECT_REGION_SIZE);
	WriteMacInt16(region + DSP_REGION_OFF_SIZE, DSP_RECT_REGION_SIZE);
	WriteMacInt16(region + DSP_REGION_OFF_BBOX + 0, 0);
	WriteMacInt16(region + DSP_REGION_OFF_BBOX + 2, 0);
	WriteMacInt16(region + DSP_REGION_OFF_BBOX + 4, (uint16_t)h);
	WriteMacInt16(region + DSP_REGION_OFF_BBOX + 6, (uint16_t)w);
	WriteMacInt32(handle, region);
	return handle;
}

bool DSpAllocateBackBuffer(DSpContextPrivate *ctx, uint32_t w, uint32_t h, uint32_t bpp)
{
	uint32_t row = 0;
	uint32_t size = 0;
	if (!ctx || !dsp_back_buffer_layout(w, h, bpp, &row, &size)) {
		QD3D_RESOURCE_LOG("DSpAllocateBackBuffer(GL): invalid layout %ux%u@%u",
		                  w, h, bpp);
		return false;
	}
	void *buf = gfxaccel_resources_heap_alloc_buffer(kHeapEngineDSp, size, 0);
	if (!buf) return false;
	std::memset(buf, 0, size);

	GLuint tex = 0;
	if (GfxGLDeviceMakeCurrent()) {
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	}
	if (!tex) {
		gfxaccel_resources_heap_mm_free_buffer(kHeapEngineDSp, buf);
		QD3D_RESOURCE_LOG("DSpAllocateBackBuffer(GL): texture allocation failed for %ux%u@%u",
		                  w, h, bpp);
		return false;
	}

	ctx->back_buffer = buf;
	ctx->back_texture = (void *)(uintptr_t)tex;
	ctx->dirty_empty = true;
	ctx->dirty_cold_start = true;
	gfxaccel_resources_set_buffer_owner(buf, kGfxEngineDSp);
	QD3D_RESOURCE_LOG("DSpAllocateBackBuffer(GL): ctx=%u back=%ux%u@%u row=%u size=%u texture=%u colorNeeds=%u",
	                  ctx->handle, w, h, bpp, row, size, (unsigned)tex,
	                  ctx->attr.colorNeeds);
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
	DSpReleaseBackBufferStaging(ctx);
	ctx->cgrafptr_mac_addr = 0;
	ctx->front_cgrafptr_mac_addr = 0;
	ctx->front_pixmap_mac_addr = 0;
	ctx->front_pixmap_handle_mac_addr = 0;
	ctx->front_staging_mac_addr = 0;
	ctx->front_staging_size = 0;
	ctx->front_staging_owned_sysheap = false;
	ctx->front_staging_row_bytes = 0;
	ctx->front_staging_height = 0;
}

void DSpReleaseBackBufferStaging(DSpContextPrivate *ctx)
{
	if (!ctx) return;
	/* Exposed Mac_sysalloc storage cannot be safely recycled while guest code
	 * may retain the PixMap. Detach it, matching the Metal quarantine policy. */
	ctx->staging_mac_addr = 0;
	ctx->staging_size = 0;
	ctx->staging_owned_sysheap = false;
}

/* Expand back buffer into tightly packed RGBA for GL texture upload. */
static void expand_back_to_rgba(const DSpContextPrivate *ctx, std::vector<uint8_t> &out)
{
	const uint32_t w = DSpContextBackBufferWidth(ctx);
	const uint32_t h = DSpContextBackBufferHeight(ctx);
	const uint32_t bpp = ctx->attr.backBufferBestDepth;
	uint32_t row = 0;
	uint32_t buffer_size = 0;
	const uint8_t *src = (const uint8_t *)ctx->back_buffer;
	out.resize((size_t)w * h * 4);
	if (!src || !dsp_back_buffer_layout(w, h, bpp, &row, &buffer_size)) {
		std::memset(out.data(), 0, out.size());
		return;
	}
	const uint8_t *gamma = (const uint8_t *)MetalCompositorGetGammaLUTBuffer();
	auto apply_gamma = [gamma](uint8_t c, uint32_t plane) -> uint8_t {
		return gamma ? gamma[plane * 256u + c] : c;
	};
	const uint32_t *rgb555_lut = bpp == 16 ? rgb555_gamma_lut(gamma) : nullptr;
	std::array<uint32_t, 256> palette_rgba = {};
	if (bpp <= 8) {
		const uint8_t *pal = ctx->clut_bytes_latched;
		for (uint32_t i = 0; i < palette_rgba.size(); i++) {
			palette_rgba[i] = pack_rgba(
				apply_gamma(pal[i * 3u + 0u], 0),
				apply_gamma(pal[i * 3u + 1u], 1),
				apply_gamma(pal[i * 3u + 2u], 2));
		}
	}
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *srow = src + (size_t)y * row;
		uint32_t *drow = reinterpret_cast<uint32_t *>(
			out.data() + (size_t)y * w * 4u);
		for (uint32_t x = 0; x < w; x++) {
			if (bpp == 32) {
				/* Guest BE ARGB */
				drow[x] = pack_rgba(
					apply_gamma(srow[x * 4u + 1u], 0),
					apply_gamma(srow[x * 4u + 2u], 1),
					apply_gamma(srow[x * 4u + 3u], 2));
			} else if (bpp == 16) {
				const uint16_t be = (uint16_t)(
					((uint16_t)srow[x * 2u] << 8) | srow[x * 2u + 1u]);
				drow[x] = rgb555_lut[be & 0x7fffu];
			} else {
				uint8_t i = 0;
				if (bpp == 8) {
					i = srow[x];
				} else if (bpp == 4) {
					const uint8_t packed = srow[x >> 1];
					i = (x & 1u) ? (packed & 0x0fu) : (packed >> 4);
				} else if (bpp == 2) {
					const uint8_t packed = srow[x >> 2];
					i = (packed >> ((3u - (x & 3u)) * 2u)) & 0x03u;
				} else {
					const uint8_t packed = srow[x >> 3];
					i = (packed >> (7u - (x & 7u))) & 0x01u;
				}
				drow[x] = palette_rgba[i];
			}
		}
	}
}

void DSpEncodeBackBufferBlit(DSpContextPrivate *ctx, void * /*encoder*/, void * /*framebuffer_texture*/)
{
	if (!ctx || !ctx->back_buffer) return;

	/* Upload back buffer into GL texture and cache as compositor overlay */
	if (ctx->back_texture && GfxGLDeviceMakeCurrent()) {
		/* DSp swaps every movie frame. Reuse conversion storage and update the
		 * texture allocated at Reserve time instead of reallocating driver
		 * storage with glTexImage2D on every swap. */
		static std::vector<uint8_t> rgba;
		expand_back_to_rgba(ctx, rgba);
		const uint32_t w = DSpContextBackBufferWidth(ctx);
		const uint32_t h = DSpContextBackBufferHeight(ctx);
		GLuint tex = (GLuint)(uintptr_t)ctx->back_texture;
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h,
		                GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

		/* DSp is the 2D framebuffer beneath RAVE. Publishing it as an overlay
		 * races and replaces the one-slot RAVE mailbox, hiding all 3D. */
		CompositeLayer layer = {};
		layer.source = (void *)(uintptr_t)tex;
		layer.src_size_w = w;
		layer.src_size_h = h;
		layer.dst_origin_x = 0;
		layer.dst_origin_y = 0;
		layer.dst_size_w = (float)w;
		layer.dst_size_h = (float)h;
		layer.slot = kLayerSlotFramebuffer;
		layer.blend = kBlendOpaque;
		layer.alpha = 1.f;
		const bool submitted =
			ctx->state == (uint32_t)kDSpContextState_Active;
#if QD3D_GRAPHICS_LOGGING_ENABLED
		int32_t submit_result = kGfxAccelNoErr;
#endif
		if (submitted) {
			FrameDescriptor desc = {};
			desc.layers = &layer;
			desc.layer_count = 1;
			const DMCModeSnapshot *snap = dmc_current_snapshot();
			desc.generation = snap ? snap->generation : 0;
		#if QD3D_GRAPHICS_LOGGING_ENABLED
			submit_result = MetalCompositorSubmitFrame(&desc);
		#else
			MetalCompositorSubmitFrame(&desc);
		#endif
		}
#if QD3D_GRAPHICS_LOGGING_ENABLED
		static uint64_t present_count = 0;
		present_count++;
		if (present_count <= 8 || (present_count & (present_count - 1)) == 0 ||
		    (present_count % 120) == 0 || submit_result != 0) {
			uint64_t r_sum = 0, g_sum = 0, b_sum = 0, nonblack = 0;
			for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
				r_sum += rgba[i + 0]; g_sum += rgba[i + 1]; b_sum += rgba[i + 2];
				if (rgba[i + 0] || rgba[i + 1] || rgba[i + 2]) nonblack++;
			}
			QD3D_RENDER_LOG("DSpPresent frame=%llu ctx=%u state=%u submitted=%d display=%ux%u@%u back=%ux%u@%u row=%u bytes=%u nonblack=%llu sums=%llu/%llu/%llu slot=framebuffer submit=%d texture=%u",
			                (unsigned long long)present_count, ctx->handle,
			                ctx->state, submitted ? 1 : 0,
			                ctx->attr.displayWidth, ctx->attr.displayHeight,
			                ctx->attr.displayBestDepth, w, h,
			                ctx->attr.backBufferBestDepth,
			                DSpBackBufferAlignedRowBytes(w, ctx->attr.backBufferBestDepth),
			                ctx->staging_size, (unsigned long long)nonblack,
			                (unsigned long long)r_sum, (unsigned long long)g_sum,
			                (unsigned long long)b_sum, submit_result, (unsigned)tex);
		}
#endif
	}

	/* Also mirror into Mac main framebuffer when base matches screen */
	if (ctx->staging_mac_addr && ctx->staging_size && ctx->back_buffer) {
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
	if (!ctx || !ctx->back_buffer) return 0;
	if (ctx->cgrafptr_mac_addr) return ctx->cgrafptr_mac_addr;

	const uint32_t bpp = ctx->attr.backBufferBestDepth;
	const uint32_t w = DSpContextBackBufferWidth(ctx);
	const uint32_t h = DSpContextBackBufferHeight(ctx);
	uint32_t row_bytes = 0;
	uint32_t buffer_size = 0;
	if (!dsp_back_buffer_layout(w, h, bpp, &row_bytes, &buffer_size)) return 0;

	if (!ctx->staging_mac_addr || ctx->staging_size < buffer_size) {
		const uint32_t pixels = Mac_sysalloc(buffer_size);
		if (!pixels || !Mac2HostAddr(pixels)) return 0;
		ctx->staging_mac_addr = pixels;
		ctx->staging_size = buffer_size;
		ctx->staging_owned_sysheap = true;
		std::memcpy(Mac2HostAddr(pixels), ctx->back_buffer, buffer_size);
	}

	const uint32_t pixmap = Mac_sysalloc(DSpBackBufferPixMapRecordSize());
	const uint32_t pixmap_handle = Mac_sysalloc(DSpBackBufferPixMapHandleSize());
	const uint32_t port = Mac_sysalloc(DSpBackBufferCGrafPortSize());
	const uint32_t vis_region = dsp_create_rect_region(w, h);
	const uint32_t clip_region = dsp_create_rect_region(w, h);
	if (!pixmap || !pixmap_handle || !port || !vis_region || !clip_region)
		return 0;

	uint16_t pixel_type = 0, pixel_size = 0, component_count = 0,
	         component_size = 0;
	dsp_pixmap_format(bpp, &pixel_type, &pixel_size, &component_count,
	                  &component_size);
	Mac_memset(pixmap, 0, DSpBackBufferPixMapRecordSize());
	WriteMacInt32(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_BASEADDR, ctx->staging_mac_addr);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_ROWBYTES,
	              DSpBackBufferPixMapRowBytesField(row_bytes));
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_TOP, 0);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_LEFT, 0);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_BOT, (uint16_t)h);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_RIGHT, (uint16_t)w);
	WriteMacInt32(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_HRES, 0x00480000u);
	WriteMacInt32(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_VRES, 0x00480000u);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_PIXELTYPE, pixel_type);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_PIXELSIZE, pixel_size);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_CMPCOUNT, component_count);
	WriteMacInt16(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_CMPSIZE, component_size);
	WriteMacInt32(pixmap + DSP_MAINDEVICE_PIXMAP_OFF_PLANEBYTES, 0);
	WriteMacInt32(pixmap_handle, pixmap);

	Mac_memset(port, 0, DSpBackBufferCGrafPortSize());
	WriteMacInt32(port + DSP_CGRAFPORT_OFF_PORT_PIXMAP, pixmap_handle);
	WriteMacInt16(port + DSP_CGRAFPORT_OFF_PORT_VERSION, 0xC000);
	WriteMacInt16(port + DSP_CGRAFPORT_OFF_PORT_RECT + 0, 0);
	WriteMacInt16(port + DSP_CGRAFPORT_OFF_PORT_RECT + 2, 0);
	WriteMacInt16(port + DSP_CGRAFPORT_OFF_PORT_RECT + 4, (uint16_t)h);
	WriteMacInt16(port + DSP_CGRAFPORT_OFF_PORT_RECT + 6, (uint16_t)w);
	WriteMacInt32(port + DSP_CGRAFPORT_OFF_VIS_RGN, vis_region);
	WriteMacInt32(port + DSP_CGRAFPORT_OFF_CLIP_RGN, clip_region);
	WriteMacInt16(port + DSP_CGRAFPORT_OFF_RGB_FG_COLOR + 0, 0xffff);
	WriteMacInt16(port + DSP_CGRAFPORT_OFF_RGB_FG_COLOR + 2, 0xffff);
	WriteMacInt16(port + DSP_CGRAFPORT_OFF_RGB_FG_COLOR + 4, 0xffff);
	WriteMacInt16(port + DSP_CGRAFPORT_OFF_RGB_BK_COLOR + 0, 0);
	WriteMacInt16(port + DSP_CGRAFPORT_OFF_RGB_BK_COLOR + 2, 0);
	WriteMacInt16(port + DSP_CGRAFPORT_OFF_RGB_BK_COLOR + 4, 0);
	WriteMacInt16(port + DSP_CGRAFPORT_OFF_PN_SIZE + 0, 1);
	WriteMacInt16(port + DSP_CGRAFPORT_OFF_PN_SIZE + 2, 1);
	WriteMacInt16(port + DSP_CGRAFPORT_OFF_PN_MODE, 8);
	WriteMacInt16(port + DSP_CGRAFPORT_OFF_TX_SIZE, 12);
	WriteMacInt32(port + DSP_CGRAFPORT_OFF_FG_COLOR, 0xffffffffu);
	WriteMacInt32(port + DSP_CGRAFPORT_OFF_BK_COLOR, 0x00000000u);

	ctx->cgrafptr_mac_addr = port;
	QD3D_RESOURCE_LOG("DSpGetBackBuffer(GL): ctx=%u port=0x%08x pixmap=0x%08x base=0x%08x back=%ux%u@%u row=%u size=%u",
	                  ctx->handle, port, pixmap, ctx->staging_mac_addr, w, h,
	                  bpp, row_bytes, buffer_size);
	return port;
}

uint32_t DSpGuardStagingWrite(uint32_t /*mac_addr*/, uint32_t size, const char * /*site*/)
{
	return size;
}
