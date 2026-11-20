/*
 *  dsp_gl_renderer.cpp - DSp back-buffer via host memory + compositor present
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
#include "cpu_emulation.h"
#include "dsp_engine.h"
#include "dsp_metal_renderer.h"
#include "dsp_context_private.h"
#include "gfxaccel_resources.h"
#include "gfxaccel_resources_heap.h"
#include "metal_device_shared.h"
#include "macos_util.h"
#include "video.h"
#include "dsp_back_buffer_range.h"
#include "dsp_back_buffer_cgraf_policy.h"
#include "dsp_cgraf_port_policy.h"
#include "dsp_pixmap_offsets.h"
#include "dsp_draw_context.h"
#include "dsp_alt_buffer.h"
#include "gfx_log.h"
#include "thunks.h"

#if USE_SDL3
#include <SDL3/SDL_opengl.h>
#else
#include <SDL_opengl.h>
#endif
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>

#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

extern "C" void gfxaccel_resources_heap_mm_free_buffer(uint32_t heap_id, void *ptr);
static bool DSpGLIsValidDepth(uint32_t depth)
{
	return depth == 1 || depth == 2 || depth == 4 || depth == 8 ||
		   depth == 16 || depth == 32;
}

static bool DSpGLIsValidBackBufferLayout(uint32_t w, uint32_t h, uint32_t depth,
								   uint32_t *out_row_bytes,
								   uint32_t *out_size)
{
	if (!w || !h || !DSpGLIsValidDepth(depth)) return false;
	const uint32_t row_bytes = DSpBackBufferAlignedRowBytes(w, depth);
	const uint64_t size = (uint64_t)row_bytes * (uint64_t)h;
	if (!row_bytes || size > std::numeric_limits<uint32_t>::max()) return false;
	if (out_row_bytes) *out_row_bytes = row_bytes;
	if (out_size) *out_size = (uint32_t)size;
	return true;
}

static void DSpGLGetPixmapFormat(uint32_t depth, uint16_t *pixel_type,
							  uint16_t *pixel_size, uint16_t *component_count,
							  uint16_t *component_size)
{
	*pixel_type = depth <= 8 ? 0 : 0x10;
	*pixel_size = (uint16_t)depth;
	*component_count = depth <= 8 ? 1 : 3;
	*component_size = depth <= 8 ? (uint16_t)depth :
					  depth == 16 ? 5 : 8;
}

static uint32_t DSpGLAllocRectRegion(uint32_t w, uint32_t h)
{
	/* Region handles embedded in a guest-visible CGrafPort must outlive any
	 * Toolbox drawing that retains the port.  Mac_sysalloc storage is movable
	 * and can be reclaimed/poisoned by the guest Memory Manager; SheepMem is
	 * the permanent descriptor arena used by the Metal backend. */
	const uint32_t region = SheepMem::Reserve(DSP_RECT_REGION_SIZE);
	const uint32_t handle = SheepMem::Reserve(4);
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

bool DSpDoAllocateBackBuffer(uint32_t w, uint32_t h, uint32_t bpp,
	void** pbuf, void** ptex)
{
	uint32_t row = 0;
	uint32_t size = 0;
	if (!DSpGLIsValidBackBufferLayout(w, h, bpp, &row, &size)) {
		QD3D_RESOURCE_LOG("DSpAllocateBackBuffer(GL): invalid layout %ux%u@%u",
						  w, h, bpp);
		return false;
	}
	void *buf = gfxaccel_resources_heap_alloc_buffer(kHeapEngineDSp, size, 0);
	if (!buf) return false;
	std::memset(buf, 0, size);

	GLuint tex = 0;
	if (SharedMetalDevice()) {
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
					 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	}
	if (!tex) {
		gfxaccel_resources_heap_mm_free_buffer(kHeapEngineDSp, buf);
		QD3D_RESOURCE_LOG("DSpAllocateBackBuffer(GL): texture allocation "
				"failed for %ux%u@%u",
			w, h, bpp);
		return false;
	}

	*pbuf = buf;
	*ptex = (void *)(uintptr_t)tex;
	gfxaccel_resources_set_buffer_owner(buf, kGfxEngineDSp);
	QD3D_RESOURCE_LOG("DSpAllocateBackBuffer(GL): "
			"back=%ux%u@%u row=%u size=%u texture=%u",
		w, h, bpp, row, size, (unsigned)tex);
	return true;
}
bool DSpAllocateBackBuffer(struct DSpContextPrivate *ctx,
								   uint32_t w, uint32_t h, uint32_t bpp)
{
	if(!DSpDoAllocateBackBuffer(w, h, bpp,
			&ctx->back_buffer, &ctx->back_texture))
		return false;
	ctx->dirty_empty = true;
	ctx->dirty_cold_start = true;
	return true;
}

void DSpReleaseBackBufferNow(DSpContextPrivate *ctx)
{
	if (!ctx) return;
	if (ctx->back_texture && SharedMetalDevice()) {
		GLuint tex = (GLuint)(uintptr_t)ctx->back_texture;
		glDeleteTextures(1, &tex);
	}
	if (ctx->back_buffer) {
		gfxaccel_resources_clear_buffer_owner(ctx->back_buffer);
		gfxaccel_resources_heap_mm_free_buffer(kHeapEngineDSp, ctx->back_buffer);
		ctx->back_buffer = NULL;
	}
	ctx->back_texture = NULL;
	DSpReleaseBackBufferStaging(ctx);
	ctx->cgrafptr_mac_addr = 0;
	ctx->front_cgrafptr_mac_addr = 0;
	ctx->front_pixmap_mac_addr = 0;
	ctx->front_pixmap_handle_mac_addr = 0;
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
	if (!DSpGLIsValidBackBufferLayout(w, h, bpp, &row_bytes, &buffer_size)) return 0;

	if (!ctx->staging_mac_addr || ctx->staging_size < buffer_size) {
		const uint32_t pixels = Mac_sysalloc(buffer_size);
		if (!pixels || !Mac2HostAddr(pixels)) return 0;
		ctx->staging_mac_addr = pixels;
		ctx->staging_size = buffer_size;
		ctx->staging_owned_sysheap = true;
		std::memcpy(Mac2HostAddr(pixels), ctx->back_buffer, buffer_size);
	}

	const uint32_t pixmap = SheepMem::Reserve(DSpBackBufferPixMapRecordSize());
	const uint32_t pixmap_handle = SheepMem::Reserve(DSpBackBufferPixMapHandleSize());
	const uint32_t port = SheepMem::Reserve(DSpBackBufferCGrafPortSize());
	const uint32_t vis_region = DSpGLAllocRectRegion(w, h);
	const uint32_t clip_region = DSpGLAllocRectRegion(w, h);
	if (!pixmap || !pixmap_handle || !port || !vis_region || !clip_region)
		return 0;

	uint16_t pixel_type = 0, pixel_size = 0, component_count = 0,
			 component_size = 0;
	DSpGLGetPixmapFormat(bpp, &pixel_type, &pixel_size, &component_count,
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

int32_t DSpContext_SwapBuffersHandler(uint32_t ctxRef,
	uint32_t /*doneProc*/, uint32_t /*refCon*/)
{
	DSpContextPrivate *ctx = DSpGetContext(ctxRef);
	if (!ctx || !ctx->back_buffer) return kDSpInternalErr;

	ctx->dirty_cold_start = false;
	ctx->dirty_empty = true;

	/* Copy guest staging -> host back buffer. */
	if (ctx->staging_mac_addr && ctx->staging_size) {
		const uint32_t w = DSpContextBackBufferWidth(ctx);
		const uint32_t h = DSpContextBackBufferHeight(ctx);
		const uint32_t row = DSpBackBufferAlignedRowBytes(
			w, ctx->attr.backBufferBestDepth);
		const uint64_t expected64 = (uint64_t)row * h;
		const uint32_t expected = expected64 <= UINT32_MAX
			? (uint32_t)expected64 : 0;
		uint8 *src = Mac2HostAddr(ctx->staging_mac_addr);
		const uint32_t copy_size = expected
			? std::min(expected, ctx->staging_size) : 0;
		if (src && copy_size) std::memcpy(ctx->back_buffer, src, copy_size);
		if (copy_size != expected) {
			QD3D_RENDER_LOG("DSpSwap(GL): bounded staging copy ctx=%u have=%u expected=%u copied=%u",
							ctxRef, ctx->staging_size, expected, copy_size);
		}
	}
	if (!DSpCopyBackBufferToCanonicalScreen(ctx)) {
		QD3D_RENDER_LOG("DSpSwap(GL): canonical screen publish failed "
						"ctx=%u back=%ux%u@%u cur_mode=%d",
						ctxRef,
						DSpContextBackBufferWidth(ctx),
						DSpContextBackBufferHeight(ctx),
						ctx->attr.backBufferBestDepth,
						cur_mode);
		return kDSpInternalErr;
	}
	return kDSpNoErr;
}

void* DSpGetBackingContents(void* backing)
{
	return backing;
}

/* Host bridge - desktop: track Active fullscreen without iOS idle-timer */
static bool s_dsp_active_fullscreen = false;
extern "C" void DSpHostBridge_SetActiveFullscreen(bool active)
{
	s_dsp_active_fullscreen = active;
}
extern "C" bool DSpHostBridge_GetActiveFullscreen(void)
{
	return s_dsp_active_fullscreen;
}
extern "C" void DSpHostBridgeInit(void)
{
	s_dsp_active_fullscreen = false;
}
extern "C" void DSpHostBridgeShutdown(void)
{
	s_dsp_active_fullscreen = false;
}

bool DSpAllocAltBufferBacking(DSpAltBufferRecord *rec,
	uint32_t w, uint32_t h) {
	bool res;
	if (rec == NULL || w == 0 || h == 0) return false;
	if (w > DSP_ALT_MAX_DIM || h > DSP_ALT_MAX_DIM) {
		fprintf(stderr, "DSpAllocAltBufferBacking: "
			"dims %ux%u exceed DSP_ALT_MAX_DIM\n", w, h);
		return false;
	}

	res = DSpDoAllocateBackBuffer(w, h, rec->depth,
		&rec->backing, &rec->texture);
	if (res == false) {
		DSP_LOG("DSpAllocAltBufferBacking: DSpAllocateBackBuffer failed "
				"(%ux%u@%u)", w, h, rec->depth);
		gfxaccel_resources_heap_note_allocation_released(kHeapEngineDSp);
		return false;
	}
	rec->width = w;
	rec->height = h;
	return true;
}
void DSpContextPrivateReleaseBackBufferVariables(void** texture,void** buffer)
{
}
void DSpContextPrivateReleaseBackBuffer(DSpContextPrivate* ctx)
{
}
