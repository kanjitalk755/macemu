/*
 *  dsp_metal_renderer.mm - Metal back-buffer allocation + blit encoder
 *                           for DSp.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Every DSp back-buffer uses
 *  MTLStorageModeShared on all iOS GPU families, allocated through the
 *  bump sub-allocator on kHeapCompositor. Unified memory on
 *  A-series makes VRAM / system-memory a no-op at the performance layer;
 *  Shared gives CPU-writable semantics (the emulated app writes
 *  back-buffer bytes via the CGrafPtr) and GPU readability for the
 *  SwapBuffers blit — without the Private+Shared dual-buffer copy cost
 *  that would have been necessary on discrete-GPU devices.
 *
 *  Engine-blindness preserved: no kGfxEngineDSp symbol in
 *  metal_compositor.{h,mm} or compositor_shaders.metal. DSp writes a
 *  CompositeLayer POD with slot=kLayerSlotFramebuffer; the compositor
 *  sees only the slot + source texture handle, never the engine.
 *
 *  Pixel-format mapping:
 *    8bpp  → MTLPixelFormatR8Uint       (indexed; CLUT unpack)
 *    16bpp → MTLPixelFormatR16Uint      (xRGB1555; shader unpack)
 *    32bpp → MTLPixelFormatBGRA8Unorm   (direct 32-bit)
 *  Matches metal_compositor.mm:370-400 precedent so the compositor's
 *  existing 2D framebuffer shaders sample DSp-owned textures without any
 *  engine-specific branching.
 *
 *  Threading: all entry points run on the emul thread (PPC dispatch
 *  handler context). No explicit Metal synchronization primitives are
 *  used — implicit encoder ordering on the single shared
 *  MTLCommandQueue covers the release / SwapBuffers race.
 *  The ThreadingContractTests grep gate enforces this.
 */

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include <cstring>
#include <unistd.h>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "thunks.h"              /* SheepMem::Reserve */
#include "dsp_back_buffer_cgraf_policy.h"
#include "dsp_back_buffer_range.h"
#include "dsp_cgraf_port_policy.h"
#include "dsp_display_mode_policy.h"
#include "dsp_engine.h"
#include "dsp_draw_context.h"
#include "dsp_front_staging_color_policy.h"
#include "dsp_guest_address.h"
#include "dsp_pixel_staging_lifetime_policy.h"
#include "dsp_metal_renderer.h"
#include "dsp_context_private.h"
#include "gfxaccel_resources.h"       /* per-buffer owner tag */
#include "gfxaccel_resources_heap.h"
#include "metal_compositor.h"
#include "metal_device_shared.h"     /* SharedMetalDevice() for unpack-PSO lazy init */
#include "display_mode_controller.h" /* dmc_current_snapshot() for fade_active in the unpack twin */

extern uint32 Mac_sysalloc(uint32 size);
extern void Mac_sysfree(uint32 addr);
extern "C" uint64_t GLCompositeLatestOffscreenToGuestSurfaceUsingLatestExtentIfNotSuppressed(
	uint32_t dstBaseaddr,
	uint32_t dstRowbytes,
	uint32_t dstDepthBits);

/* Forward decl of the lowmem PixMap restore
 * helper defined in dsp_draw_context.mm. Called at the START of
 * DSpReleaseBackBufferNow so the redirected PixMap.baseAddr is rewound
 * to the cached original BEFORE the MTLBuffer goes away (Landmine-7:
 * prevents dangling Mac address in lowmem). */
extern "C" void DSpRestoreMainDevicePixMap(DSpContextPrivate *ctx);

/* DSp exposes these blocks as guest PixMap.baseAddr storage. Once exposed,
 * do not DisposePtr them: launch-time CFM/component allocations may reuse
 * the same system-heap range and execute stale frame bytes. */
enum {
	kDSpPixelStagingPoolCapacity = 16
};

/* Uniform allocation floor.  Vending every staging block at >= this size
 * lets later, larger needs (Diablo II: 640x480 splash staging -> 800x600
 * menu staging) REUSE quarantined blocks instead of making fresh multi-MB
 * NewPtrSys allocations mid-game.  Mid-game system-heap allocations of
 * that size are device-proven (2026-06-11) to detonate purge/compaction
 * storms in the guest system zone: purgeable component code gets evicted
 * (sound-component crash class) and live structures move while stale
 * pointers still reference them (the Memory-Manager zone-walk wedge that
 * froze DII's menu).  2 MiB covers every mode DII uses (800x600x4 with
 * aligned rowBytes = 1,996,800 bytes). */
enum : uint32_t {
	kDSpPixelStagingAllocFloor = 0x200000
};

struct DSpPixelStagingPoolBlock {
	uint32_t mac_addr;
	uint32_t size;        /* logical size of the most recent exposure */
	uint32_t alloc_size;  /* TRUE Mac_sysalloc size; NEVER grown — the
	                       * ground truth a write must never exceed. */
	bool     in_use;
	bool     ever_exposed_to_guest;
	bool     allocated_from_mac_system_heap;
};

static DSpPixelStagingPoolBlock
	s_dsp_pixel_staging_pool[kDSpPixelStagingPoolCapacity];

static DSpPixelStagingPoolBlock *DSpFindPixelStagingBlock(uint32_t mac_addr)
{
	for (uint32_t i = 0; i < kDSpPixelStagingPoolCapacity; i++) {
		DSpPixelStagingPoolBlock *block = &s_dsp_pixel_staging_pool[i];
		if (block->mac_addr == mac_addr) return block;
	}
	return nullptr;
}

static DSpPixelStagingPoolBlock *DSpFindEmptyPixelStagingBlock(void)
{
	for (uint32_t i = 0; i < kDSpPixelStagingPoolCapacity; i++) {
		DSpPixelStagingPoolBlock *block = &s_dsp_pixel_staging_pool[i];
		if (block->mac_addr == 0) return block;
	}
	return nullptr;
}

/* Keep one idle floor-sized block parked in the pool.  Fresh allocations
 * come in pairs (back staging at Reserve, then front staging at the first
 * GetFrontBuffer mid-game); allocating the spare TOGETHER with the first
 * fresh block moves the second allocation's heap violence to mode-set
 * time, where the guest expects turbulence, instead of mid-frame. */
static void DSpPrewarmPixelStagingSpare(void)
{
	for (uint32_t i = 0; i < kDSpPixelStagingPoolCapacity; i++) {
		DSpPixelStagingPoolBlock *block = &s_dsp_pixel_staging_pool[i];
		if (block->mac_addr != 0 && !block->in_use &&
		    block->alloc_size >= kDSpPixelStagingAllocFloor)
			return;                    /* an idle spare already exists */
	}
	DSpPixelStagingPoolBlock *slot = DSpFindEmptyPixelStagingBlock();
	if (slot == nullptr)
		return;
	uint32_t mac_addr = Mac_sysalloc(kDSpPixelStagingAllocFloor);
	if (mac_addr == 0)
		return;
	slot->mac_addr = mac_addr;
	slot->size = 0;
	slot->alloc_size = kDSpPixelStagingAllocFloor;
	slot->in_use = false;
	slot->ever_exposed_to_guest = false;
	slot->allocated_from_mac_system_heap = true;
	DSP_LOG("DSpReserveGuestPixelStaging: prewarmed spare 0x%08x alloc=%u",
	        mac_addr, (unsigned)kDSpPixelStagingAllocFloor);
}

extern "C" uint32_t DSpReserveGuestPixelStaging(uint32_t size)
{
	if (size == 0) return 0;

	for (uint32_t i = 0; i < kDSpPixelStagingPoolCapacity; i++) {
		DSpPixelStagingPoolBlock *block = &s_dsp_pixel_staging_pool[i];
		if (block->mac_addr != 0 &&
		    !block->in_use &&
		    DSpPixelStagingCanReuseQuarantinedAllocation(block->alloc_size, size)) {
			block->in_use = true;
			block->size = size;
			DSP_LOG("DSpReserveGuestPixelStaging: reused quarantined "
			        "staging 0x%08x alloc=%u need=%u",
			        block->mac_addr, block->alloc_size, size);
			return block->mac_addr;
		}
	}

	uint32_t alloc_request = size;
	if (alloc_request < kDSpPixelStagingAllocFloor)
		alloc_request = kDSpPixelStagingAllocFloor;
	uint32_t mac_addr = Mac_sysalloc(alloc_request);
	if (mac_addr == 0 && alloc_request != size) {
		/* Degraded: the floored request did not fit; take the exact size. */
		alloc_request = size;
		mac_addr = Mac_sysalloc(size);
	}
	if (mac_addr == 0) return 0;

	DSpPixelStagingPoolBlock *slot = DSpFindEmptyPixelStagingBlock();
	if (slot != nullptr) {
		slot->mac_addr = mac_addr;
		slot->size = size;
		slot->alloc_size = alloc_request; /* ground truth; never grown afterwards */
		slot->in_use = true;
		slot->ever_exposed_to_guest = false;
		slot->allocated_from_mac_system_heap = true;
		DSP_LOG("DSpReserveGuestPixelStaging: fresh Mac_sysalloc 0x%08x alloc=%u need=%u",
		        mac_addr, alloc_request, size);
		DSpPrewarmPixelStagingSpare();
	} else {
		DSP_LOG("DSpReserveGuestPixelStaging: pool full; 0x%08x "
		        "size=%u will be quarantined untracked on release",
		        mac_addr, size);
	}
	return mac_addr;
}

extern "C" uint32_t DSpGuardStagingWrite(uint32_t mac_addr, uint32_t size,
                                         const char *site)
{
	if (mac_addr == 0 || size == 0) return size;

	DSpPixelStagingPoolBlock *block = DSpFindPixelStagingBlock(mac_addr);
	if (block == nullptr) {
		/* Write targets an address that is not a tracked staging block.
		 * We cannot bound it — flag it so the field log shows an untracked
		 * staging write (itself suspicious). */
		DSP_LOG("DSpGuardStagingWrite: UNTRACKED staging target site=%s "
		        "addr=0x%08x size=%u (no pool entry — cannot bound)",
		        site ? site : "?", mac_addr, size);
		return size;
	}

	if (size > block->alloc_size) {
		DSP_LOG("DSpGuardStagingWrite: *** STAGING OVERRUN PREVENTED *** "
		        "site=%s addr=0x%08x requested=%u alloc=%u overrun=%u "
		        "-> CLAMPED to alloc",
		        site ? site : "?", mac_addr, size, block->alloc_size,
		        size - block->alloc_size);
		return block->alloc_size;
	}
	return size;
}

extern "C" void DSpQuarantineGuestPixelStaging(
	uint32_t mac_addr,
	uint32_t size,
	bool allocated_from_mac_system_heap)
{
	if (mac_addr == 0) return;

	DSpPixelStagingPoolBlock *block = DSpFindPixelStagingBlock(mac_addr);
	if (block != nullptr) {
		if (size > block->size) block->size = size;
		block->in_use = false;
		block->ever_exposed_to_guest = true;
		block->allocated_from_mac_system_heap =
			allocated_from_mac_system_heap;
		DSP_LOG("DSpQuarantineGuestPixelStaging: retained exposed "
		        "staging 0x%08x size=%u",
		        mac_addr, block->size);
		return;
	}

	DSpPixelStagingPoolBlock *slot = DSpFindEmptyPixelStagingBlock();
	if (slot != nullptr) {
		slot->mac_addr = mac_addr;
		slot->size = size;
		slot->alloc_size = size;   /* best-known true size for untracked block */
		slot->in_use = false;
		slot->ever_exposed_to_guest = true;
		slot->allocated_from_mac_system_heap =
			allocated_from_mac_system_heap;
		DSP_LOG("DSpQuarantineGuestPixelStaging: retained untracked "
		        "exposed staging 0x%08x size=%u",
		        mac_addr, size);
		return;
	}

	if (DSpPixelStagingShouldReturnExposedAllocationToMacHeap(
	        allocated_from_mac_system_heap)) {
		Mac_sysfree(mac_addr);
		return;
	}

	DSP_LOG("DSpQuarantineGuestPixelStaging: pool full; leaving exposed "
	        "staging 0x%08x size=%u allocated",
	        mac_addr, size);
}

extern "C" void DSpDiscardUnusedGuestPixelStaging(
	uint32_t mac_addr,
	bool allocated_from_mac_system_heap)
{
	if (mac_addr == 0) return;

	DSpPixelStagingPoolBlock *block = DSpFindPixelStagingBlock(mac_addr);
	if (block != nullptr) {
		if (block->ever_exposed_to_guest) {
			block->in_use = false;
			DSP_LOG("DSpDiscardUnusedGuestPixelStaging: retained "
			        "previously exposed staging 0x%08x size=%u",
			        mac_addr, block->size);
			return;
		}
		if (block->allocated_from_mac_system_heap) {
			Mac_sysfree(mac_addr);
		}
		*block = {};
		return;
	}

	if (allocated_from_mac_system_heap) {
		Mac_sysfree(mac_addr);
	}
}

extern "C" void DSpReleaseBackBufferStaging(DSpContextPrivate *ctx)
{
	if (ctx == nullptr || ctx->staging_mac_addr == 0) return;
	DSpQuarantineGuestPixelStaging(ctx->staging_mac_addr,
	                               ctx->staging_size,
	                               ctx->staging_owned_sysheap);
	ctx->staging_mac_addr = 0;
	ctx->staging_size = 0;
	ctx->staging_owned_sysheap = false;
}

/*
 *  Pixel-format mapping.
 *  Matches metal_compositor.mm:370-400 precedent so the compositor's
 *  existing 2D framebuffer shaders accept DSp-owned textures without
 *  branching on engine.
 */
static inline MTLPixelFormat DSpPixelFormatForDepthBits(uint32_t depth_bits)
{
	switch (depth_bits) {
		/* 1/2/4 bpp use R8Uint + shader-unpack pattern
		 * matching metal_compositor.mm:377-380 and compositor_shaders.metal
		 * compositor_fragment_indexed (bits_per_pixel branch). */
		case 1:  return MTLPixelFormatR8Uint;       /* 1 bpp indexed; 8 px/byte, MSB-first */
		case 2:  return MTLPixelFormatR8Uint;       /* 2 bpp indexed; 4 px/byte */
		case 4:  return MTLPixelFormatR8Uint;       /* 4 bpp indexed; 2 px/byte */
		case 8:  return MTLPixelFormatR8Uint;       /* 8 bpp indexed; 1 px/byte */
		case 16: return MTLPixelFormatR16Uint;      /* xRGB1555; shader unpack */
		case 32: return MTLPixelFormatBGRA8Unorm;   /* direct 32-bit */
		default: return MTLPixelFormatInvalid;
	}
}

/*
 *  Row-stride alignment: 256 bytes matches the bump allocator's
 *  alignment floor and the Metal minimum bytes-per-row for buffer-backed
 *  textures on arm64. The emulated app sees rowBytes = alignedRB, so its
 *  write index advances by alignedRB per scanline (may include up to
 *  255 bytes of padding per row — acceptable; DSp apps iterate by
 *  rowBytes, not by tightly-packed width × bpp/8).
 */
static inline uint32_t DSpAlignedRowBytes(uint32_t w, uint32_t bpp)
{
	return DSpBackBufferAlignedRowBytes(w, bpp);
}

static uint32_t DSpCreateBackBufferRectRegion(uint32_t w, uint32_t h)
{
	uint32_t region_addr = DSpReserveGuestScratch(DSP_RECT_REGION_SIZE);
	uint32_t handle_addr = DSpReserveGuestScratch(4u);
	if (region_addr == 0 || handle_addr == 0) return 0;

	Mac_memset(region_addr, 0, DSP_RECT_REGION_SIZE);
	WriteMacInt16(region_addr + DSP_REGION_OFF_SIZE, DSP_RECT_REGION_SIZE);
	WriteMacInt16(region_addr + DSP_REGION_OFF_BBOX + 0, 0);
	WriteMacInt16(region_addr + DSP_REGION_OFF_BBOX + 2, 0);
	WriteMacInt16(region_addr + DSP_REGION_OFF_BBOX + 4, (uint16_t)h);
	WriteMacInt16(region_addr + DSP_REGION_OFF_BBOX + 6, (uint16_t)w);
	WriteMacInt32(handle_addr, region_addr);
	return handle_addr;
}

/* ---------------------------------------------------------------------- *
 *  DSpAllocateBackBuffer — heap-routed MTLBuffer + MTLTexture view       *
 * ---------------------------------------------------------------------- */

extern "C" bool DSpAllocateBackBuffer(DSpContextPrivate *ctx,
                                       uint32_t w, uint32_t h, uint32_t bpp)
{
	if (ctx == nullptr || w == 0 || h == 0) return false;

	MTLPixelFormat fmt = DSpPixelFormatForDepthBits(bpp);
	if (fmt == MTLPixelFormatInvalid) {
		DSP_LOG("DSpAllocateBackBuffer: invalid pixel format for bpp=%u", bpp);
		return false;
	}

	uint32_t alignedRB   = DSpAlignedRowBytes(w, bpp);
	uint32_t buffer_size = alignedRB * h;

	/* Heap-routed alloc — zero direct [device newBufferWith*] in DSp
	 * code, preserving the engine-blindness invariants. The heap
	 * ceiling check is inside the sub-allocator; NULL return here means
	 * either the heap is exhausted (eviction already ran) or allocation
	 * failed outright.
	 *
	 * Migrated from kHeapCompositor to kHeapEngineDSp.
	 * The DSp back buffer now lives in the 5th per-engine heap, which is exempt
	 * from the DMC on_mode_exit reset rule.
	 * This closes the root cause "DSp back buffer reclaimed by kHeapCompositor
	 * reset" with a single-argument change. */
	void *buf_raw = gfxaccel_resources_heap_alloc_buffer(
	    kHeapEngineDSp,                                          // DSp back buffer now owns its 5th per-engine heap (kHeapDSp excluded from on_mode_exit reset)
	    buffer_size,
	    (uint32_t)MTLResourceStorageModeShared);
	if (buf_raw == NULL) {
		DSP_LOG("DSpAllocateBackBuffer: heap alloc failed (size=%u, %ux%u@%ubpp)",
		        buffer_size, w, h, bpp);
		return false;
	}
	/* The heap API returns a retained object. Transfer it into ARC so the
	 * DSp release paths can actually drop the Metal resource before resetting
	 * the bump offset. */
	id<MTLBuffer> buf = (__bridge_transfer id<MTLBuffer>)buf_raw;
	ctx->back_buffer = buf;

	/* Rule 1 bug fix: at 1/2/4 bpp we use the R8Uint shader-
	 * unpack pattern — the Metal texture is a byte-view over
	 * the packed pixel bytes, so its descriptor.width must be the packed
	 * byte count, not the logical pixel width. `(w * bpp + 7) / 8` yields
	 * 80/160/320/640 bytes-per-row at 1/2/4/8 bpp for w=640 — matches the
	 * compositor's own 2D framebuffer shader path (metal_compositor.mm:
	 * 377-380, s_framebuffer_texture_width is packed-byte width for
	 * indexed depths). At 16/32 bpp the texture is a direct view so
	 * descriptor.width == logical pixel width. The shader unpacks the
	 * byte-view into logical pixels via bit-shifts + CLUT lookup. Without
	 * this fix, newTextureWithDescriptor fails at 1/2/4 bpp because
	 * alignedRB (rounded packed-byte count) < w * bytes-per-texel for
	 * R8Uint (bytesPerRow < descriptor.width * 1). */
	NSUInteger tex_width = (NSUInteger)DSpDisplayModeTextureWidth(w, bpp);

	MTLTextureDescriptor *desc = [MTLTextureDescriptor new];
	desc.textureType = MTLTextureType2D;
	desc.pixelFormat = fmt;
	desc.width       = tex_width;
	desc.height      = h;
	desc.storageMode = MTLStorageModeShared;
	desc.usage       = MTLTextureUsageShaderRead;

	/* Texture is a VIEW over the buffer memory — no separate heap alloc.
	 * Metal requires bytesPerRow to be 16-byte aligned for buffer-backed
	 * textures; 256-byte alignedRB satisfies that trivially. */
	id<MTLTexture> tex = [buf newTextureWithDescriptor:desc
	                                            offset:0
	                                       bytesPerRow:alignedRB];
	if (tex == nil) {
		DSP_LOG("DSpAllocateBackBuffer: newTextureWithDescriptor returned nil "
		        "(bpp=%u, alignedRB=%u)", bpp, alignedRB);
		gfxaccel_resources_heap_note_allocation_released(kHeapEngineDSp);
		ctx->back_buffer = nil;
		return false;
	}
	ctx->back_texture = tex;

	/* Tag the back-buffer with the DSp
	 * engine id so ownership is explicit per-buffer (NOT DMC-implicit).
	 * The NQD conflict gate uses the DMC active-owner
	 * snapshot in the hot path; this tag is authoritative per-buffer and
	 * is consumed by test harnesses (coexistence tests) to
	 * cross-check. The compositor NEVER queries this tag —
	 * compositor-blindness is preserved. */
	gfxaccel_resources_set_buffer_owner(
	    (__bridge void *)ctx->back_buffer, (uint32_t)kGfxEngineDSp);

	DSP_LOG("DSpAllocateBackBuffer: %ux%u@%ubpp alignedRB=%u size=%u",
	        w, h, bpp, alignedRB, buffer_size);
	return true;
}

/* ---------------------------------------------------------------------- *
 *  DSpReleaseBackBufferNow — synchronous release, texture-first          *
 * ---------------------------------------------------------------------- */

extern "C" void DSpReleaseBackBufferNow(DSpContextPrivate *ctx)
{
	if (ctx == nullptr) return;
	DSpRestoreMainDevicePixMap(ctx);  /* Landmine-7: drop PixMap redirect BEFORE back_buffer goes away (no dangling Mac address in lowmem). */
	/* Clear the owner-tag BEFORE the buffer
	 * goes away so the owner map does not hold a dangling pointer. */
	if (ctx->back_buffer != nil) {
		gfxaccel_resources_clear_buffer_owner(
		    (__bridge void *)ctx->back_buffer);
		gfxaccel_resources_heap_note_allocation_released(kHeapEngineDSp);
	}
	/* Texture FIRST (drops the view
	 * reference into the buffer memory), buffer SECOND. Some iOS Metal
	 * drivers assert on "Texture references buffer memory that has been
	 * deallocated" when the backing is released before a view. Matches
	 * DSpReleaseNow in dsp_draw_context.mm + the release-FIFO
	 * drain.
	 *
	 * NOTE: dsp_draw_context.mm also has its own release paths
	 * (DSpReleaseNow synchronous, DSpQueueReleaseAtVBL deferred,
	 * DSpQueueReleaseAtVBLPartial for bg survival, and the VBL drain
	 * callback). All must clear the owner tag before nil'ing the buffer. */
	ctx->back_texture = nil;
	ctx->back_buffer  = nil;
	DSpReleaseBackBufferStaging(ctx);
	ctx->cgrafptr_mac_addr = 0;
	if (gfxaccel_resources_heap_live_allocation_count(kHeapEngineDSp) == 0) {
		uint64_t reclaimed = gfxaccel_resources_heap_reset(kHeapEngineDSp);
		if (reclaimed > 0) {
			DSP_LOG("DSp heap reset after DSpReleaseBackBufferNow reclaimed %llu bytes",
			        (unsigned long long)reclaimed);
		}
	}
}

/* ---------------------------------------------------------------------- *
 *  DSpEncodeBackBufferBlit — full-buffer blit                            *
 * ---------------------------------------------------------------------- */

/*
 *  The always-full-blit body is extended with a
 *  three-way branch honoring ctx->dirty_* state:
 *
 *    1. If ctx->dirty_cold_start (set at Reserve, set again
 *       at foreground restore, and reset here after every
 *       successful blit), upload the FULL back-buffer per PDF p.38.
 *    2. If ctx->dirty_empty (zero Inval calls between SwapBuffers), also
 *       upload FULL — PDF p.38 semantics: no Inval = entire buffer dirty.
 *    3. Otherwise, compute dirty bounding rect area and compare against
 *       full-buffer area: if dirty covers > 90% promote to FULL
 *       (sub-rect encoder setup cost does not pay off above ~90%). Else
 *       sub-rect blit at (dirty_left, dirty_top) with size (dw, dh).
 *
 *  After encoding (any branch): reset dirty state to empty + cold_start
 *  = false so the next SwapBuffers starts fresh. Per PDF p.38 a zero-
 *  Inval SwapBuffers returns to full-blit semantics via dirty_empty=true
 *  on the next frame (cold-start logic still fires only after
 *  bg/fg restore).
 */
extern "C" void DSpEncodeBackBufferBlit(DSpContextPrivate *ctx,
                                         void *encoder_raw,
                                         void *framebuffer_texture_raw)
{
	id<MTLBlitCommandEncoder> encoder =
	    (__bridge id<MTLBlitCommandEncoder>)encoder_raw;
	id<MTLTexture> framebuffer_texture =
	    (__bridge id<MTLTexture>)framebuffer_texture_raw;
	if (ctx == nullptr || encoder == nil || framebuffer_texture == nil) return;
	if (ctx->back_texture == nil) return;

	uint32_t full_w = DSpContextBackBufferWidth(ctx);
	uint32_t full_h = DSpContextBackBufferHeight(ctx);

	/* Clamp to the smaller of back-buffer and framebuffer dimensions.
	 * DSp Reserve can request a smaller/different back-buffer drawing
	 * environment than the selected display mode (DSp 1.7 p.25), and the
	 * compositor framebuffer is sized from the active display mode. Don't
	 * blit past either extent; Metal validation asserts on that. */
	NSUInteger dst_w = framebuffer_texture.width;
	NSUInteger dst_h = framebuffer_texture.height;
	NSUInteger back_w = ctx->back_texture.width;
	NSUInteger back_h = ctx->back_texture.height;
	NSUInteger clamp_w = ((NSUInteger)full_w < dst_w) ? (NSUInteger)full_w : dst_w;
	NSUInteger clamp_h = ((NSUInteger)full_h < dst_h) ? (NSUInteger)full_h : dst_h;
	if (clamp_w > back_w) clamp_w = back_w;
	if (clamp_h > back_h) clamp_h = back_h;

	bool full_upload;
	uint32_t dw = 0, dh = 0;
	if (ctx->dirty_cold_start) {
		/* PDF p.38: cold-start = full. */
		full_upload = true;
	} else if (ctx->dirty_empty) {
		/* Zero-Inval between SwapBuffers per PDF p.38 = full. */
		full_upload = true;
	} else {
		dw = (uint32_t)(ctx->dirty_right  - ctx->dirty_left);
		dh = (uint32_t)(ctx->dirty_bottom - ctx->dirty_top);
		/* > 90% coverage -> promote to full blit.
		 * Integer math: (dw*dh) / (full_w*full_h) > 0.9  <->
		 * (dw*dh) * 10 > (full_w*full_h) * 9. */
		uint64_t dirty_area = (uint64_t)dw * (uint64_t)dh;
		uint64_t full_area  = (uint64_t)full_w * (uint64_t)full_h;
		full_upload = (dirty_area * 10ULL > full_area * 9ULL);
	}

	if (full_upload) {
		[encoder copyFromTexture:ctx->back_texture
		             sourceSlice:0
		             sourceLevel:0
		            sourceOrigin:MTLOriginMake(0, 0, 0)
		              sourceSize:MTLSizeMake(clamp_w, clamp_h, 1)
		               toTexture:framebuffer_texture
		        destinationSlice:0
		        destinationLevel:0
		       destinationOrigin:MTLOriginMake(0, 0, 0)];
		DSP_VLOG("EncodeBackBufferBlit: FULL %lux%lu (cold_start=%d empty=%d)",
		         (unsigned long)clamp_w, (unsigned long)clamp_h,
		         ctx->dirty_cold_start, ctx->dirty_empty);
	} else {
		/* Sub-rect extent; clamp so source range stays inside the
		 * back-buffer and destination range stays inside the framebuffer
		 * texture (Metal validation asserts on overflow). */
		NSUInteger origin_x = (NSUInteger)ctx->dirty_left;
		NSUInteger origin_y = (NSUInteger)ctx->dirty_top;
		NSUInteger sub_w    = (NSUInteger)dw;
		NSUInteger sub_h    = (NSUInteger)dh;
		if (origin_x + sub_w > clamp_w) sub_w = (origin_x < clamp_w) ? (clamp_w - origin_x) : 0;
		if (origin_y + sub_h > clamp_h) sub_h = (origin_y < clamp_h) ? (clamp_h - origin_y) : 0;
		if (sub_w > 0 && sub_h > 0) {
			[encoder copyFromTexture:ctx->back_texture
			             sourceSlice:0
			             sourceLevel:0
			            sourceOrigin:MTLOriginMake((NSUInteger)ctx->dirty_left,
			                                        (NSUInteger)ctx->dirty_top, 0)
			              sourceSize:MTLSizeMake(sub_w, sub_h, 1)
			               toTexture:framebuffer_texture
			        destinationSlice:0
			        destinationLevel:0
			       destinationOrigin:MTLOriginMake((NSUInteger)ctx->dirty_left,
			                                        (NSUInteger)ctx->dirty_top, 0)];
		}
		DSP_VLOG("EncodeBackBufferBlit: SUB %lux%lu at (%d,%d)",
		         (unsigned long)sub_w, (unsigned long)sub_h,
		         (int)ctx->dirty_left, (int)ctx->dirty_top);
	}

	/* Reset dirty state — next frame starts fresh. */
	ctx->dirty_empty       = true;
	ctx->dirty_cold_start  = false;
	ctx->dirty_left = ctx->dirty_top = ctx->dirty_right = ctx->dirty_bottom = 0;
}

/* ---------------------------------------------------------------------- *
 *  DSpGetBackBufferCGrafPtr — CGrafPort emission into guest RAM          *
 * ---------------------------------------------------------------------- */

extern "C" uint32_t DSpGetBackBufferCGrafPtr(DSpContextPrivate *ctx)
{
	if (ctx == nullptr || ctx->back_buffer == nil) return 0;
	/* Stable-pointer contract: subsequent GetBackBuffer calls
	 * for the same context return the same Mac address for the lifetime
	 * of the mode. Cached on first call. */
	if (ctx->cgrafptr_mac_addr != 0) return ctx->cgrafptr_mac_addr;

	uint32_t bpp       = ctx->attr.backBufferBestDepth;
	uint32_t w         = DSpContextBackBufferWidth(ctx);
	uint32_t h         = DSpContextBackBufferHeight(ctx);
	uint32_t alignedRB = DSpAlignedRowBytes(w, bpp);

	/* baseAddr: Mac-address view of the MTLBuffer's CPU-side contents
	 * pointer. The buffer is MTLStorageModeShared so ctx->back_buffer
	 * .contents is a host VA pointer; Host2MacAddr (from
	 * include/cpu_emulation.h) wraps vm_do_get_virtual_address on real-
	 * addressing platforms and an identity cast on direct-addressing.
		 *
		 * On arm64 iOS the bump allocator lives in its own heap —
		 * NOT mapped into the emulated RAM region — so Host2MacAddr may
		 * return 0 or a nonzero address outside guest RAM for the MTLBuffer
		 * contents pointer. Fallback: reserve a Mac system-heap staging region
		 * the same size as the back-buffer and
		 * memcpy staging → back_buffer.contents in SwapBuffers before
		 * encoding the GPU blit. This preserves guest-writable CGrafPtr
		 * semantics.
	 *
	 * Raw (uint32)(uintptr_t) cast of the contents pointer is FORBIDDEN
	 * — it's undefined behaviour on arm64 iOS (64-bit host VA truncated
	 * to 32-bit Mac address). */
	uint32_t buffer_size = alignedRB * h;
	uint8_t *back_contents = (uint8_t *)ctx->back_buffer.contents;
	uint32_t mapped_addr = Host2MacAddr(back_contents);
	uint8_t *round_trip_host = mapped_addr != 0 ? Mac2HostAddr(mapped_addr) : NULL;
	uint32_t baseAddr_mac = DSpUsableDirectGuestBaseOrZero(
		mapped_addr,
		buffer_size,
		(uint32_t)RAMBase,
		(uint32_t)RAMSize,
		(uintptr_t)round_trip_host,
		(uintptr_t)back_contents);
	if (baseAddr_mac == 0) {
		DSP_LOG("DSpGetBackBufferCGrafPtr: direct base rejected "
		        "(mapped=0x%08x roundTrip=%p contents=%p size=%u)",
		        mapped_addr, round_trip_host, back_contents, buffer_size);
		if (ctx->staging_mac_addr != 0) {
			baseAddr_mac = DSpUsableGuestBaseOrZero(
				ctx->staging_mac_addr,
				buffer_size,
				(uint32_t)RAMBase,
				(uint32_t)RAMSize);
			if (baseAddr_mac == 0) {
				DSP_LOG("DSpGetBackBufferCGrafPtr: discarding unusable cached "
				        "staging baseAddr=0x%08x (size=%u)",
				        ctx->staging_mac_addr, buffer_size);
				DSpReleaseBackBufferStaging(ctx);
			}
		}
		if (baseAddr_mac == 0) {
			uint32_t staging_mac = DSpReserveGuestPixelStaging(buffer_size);
			baseAddr_mac = DSpUsableGuestBaseOrZero(
				staging_mac,
				buffer_size,
				(uint32_t)RAMBase,
				(uint32_t)RAMSize);
			if (baseAddr_mac == 0) {
				DSpDiscardUnusedGuestPixelStaging(staging_mac, true);
				DSP_LOG("DSpGetBackBufferCGrafPtr: neither Host2MacAddr nor "
				        "pixel staging allocation (%u) could vend a usable "
				        "guest-RAM baseAddr "
				        "(last=0x%08x)",
				        buffer_size, staging_mac);
				return 0;
			}
			ctx->staging_mac_addr = baseAddr_mac;
			ctx->staging_size = buffer_size;
			ctx->staging_owned_sysheap = true;
			uint32_t seed_n = DSpGuardStagingWrite(baseAddr_mac, buffer_size,
			                                       "GetBackBufferCGrafPtr.seed");
			if (back_contents != NULL) {
				Host2Mac_memcpy(baseAddr_mac, back_contents, seed_n);
			} else {
				Mac_memset(baseAddr_mac, 0, seed_n);
			}
			DSP_LOG("DSpGetBackBufferCGrafPtr: using guest-RAM staging at 0x%08x "
			        "(size=%u); initialized from back_buffer; SwapBuffers will "
			        "memcpy staging→back_buffer",
			        baseAddr_mac, buffer_size);
		}
	}

	int16_t bounds_top    = 0;
	int16_t bounds_left   = 0;
	int16_t bounds_bottom = (int16_t)h;
	int16_t bounds_right  = (int16_t)w;

	/* PixMap field values per Classic Mac PixMap conventions.
	 * The behavior-preserving defaults are used; fixtures will
	 * validate against DrawSprocketLib captures.
	 *
	 * The 1/2/4 bpp indexed cases follow DSp 1.7 PDF
	 * p.36 + Inside Macintosh: Imaging With QuickDraw ch.4 PixMap layout.
	 * pixelType=0 (chunky indexed); pixelSize = bpp; cmpCount = 1
	 * (single index channel); cmpSize = bpp. */
	uint16_t pixelType, pixelSize, cmpCount, cmpSize;
	if (bpp == 1) {
		pixelType = 0;       /* chunky indexed */
		pixelSize = 1;  cmpCount = 1;  cmpSize = 1;
	} else if (bpp == 2) {
		pixelType = 0;
		pixelSize = 2;  cmpCount = 1;  cmpSize = 2;
	} else if (bpp == 4) {
		pixelType = 0;
		pixelSize = 4;  cmpCount = 1;  cmpSize = 4;
	} else if (bpp == 8) {
		pixelType = 0;
		pixelSize = 8;  cmpCount = 1;  cmpSize = 8;
	} else if (bpp == 16) {
		pixelType = 0x10;    /* RGBDirect */
		pixelSize = 16; cmpCount = 3;  cmpSize = 5;   /* xRGB1555 */
	} else { /* bpp == 32 */
		pixelType = 0x10;    /* RGBDirect */
		pixelSize = 32; cmpCount = 3;  cmpSize = 8;   /* ARGB8888 */
	}

	uint32_t pixmap_addr =
	    DSpReserveGuestScratch(DSpBackBufferPixMapRecordSize());
	uint32_t pixmap_handle_addr =
	    DSpReserveGuestScratch(DSpBackBufferPixMapHandleSize());
	uint32_t cgrafptr_addr =
	    DSpReserveGuestScratch(DSpBackBufferCGrafPortSize());
	uint32_t vis_rgn_handle = DSpCreateBackBufferRectRegion(w, h);
	uint32_t clip_rgn_handle = DSpCreateBackBufferRectRegion(w, h);
	if (pixmap_addr == 0 || pixmap_handle_addr == 0 ||
	    cgrafptr_addr == 0 || vis_rgn_handle == 0 ||
	    clip_rgn_handle == 0) {
		DSP_LOG("DSpGetBackBufferCGrafPtr: guest-scratch reserve failed "
		        "(pixmap=0x%08x handle=0x%08x cgraf=0x%08x "
		        "vis=0x%08x clip=0x%08x)",
		        pixmap_addr, pixmap_handle_addr, cgrafptr_addr,
		        vis_rgn_handle, clip_rgn_handle);
		return 0;
	}

	const uint16_t row_bytes_field =
	    DSpBackBufferPixMapRowBytesField(alignedRB);

	Mac_memset(pixmap_addr, 0, DSpBackBufferPixMapRecordSize());
	WriteMacInt32(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_BASEADDR,     baseAddr_mac);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_ROWBYTES,     row_bytes_field);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_TOP,   (uint16_t)bounds_top);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_LEFT,  (uint16_t)bounds_left);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_BOT,   (uint16_t)bounds_bottom);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_BOUNDS_RIGHT, (uint16_t)bounds_right);
	WriteMacInt32(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_HRES,         0x00480000u);
	WriteMacInt32(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_VRES,         0x00480000u);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_PIXELTYPE,    pixelType);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_PIXELSIZE,    pixelSize);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_CMPCOUNT,     cmpCount);
	WriteMacInt16(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_CMPSIZE,      cmpSize);
	WriteMacInt32(pixmap_addr + DSP_MAINDEVICE_PIXMAP_OFF_PLANEBYTES,   0);

	WriteMacInt32(pixmap_handle_addr, pixmap_addr);

	Mac_memset(cgrafptr_addr, 0, DSpBackBufferCGrafPortSize());
	WriteMacInt32(cgrafptr_addr + DSP_CGRAFPORT_OFF_PORT_PIXMAP,
	              pixmap_handle_addr);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_PORT_VERSION, 0xC000);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_PORT_RECT + 0,
	              (uint16_t)bounds_top);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_PORT_RECT + 2,
	              (uint16_t)bounds_left);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_PORT_RECT + 4,
	              (uint16_t)bounds_bottom);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_PORT_RECT + 6,
	              (uint16_t)bounds_right);
	WriteMacInt32(cgrafptr_addr + DSP_CGRAFPORT_OFF_VIS_RGN,
	              vis_rgn_handle);
	WriteMacInt32(cgrafptr_addr + DSP_CGRAFPORT_OFF_CLIP_RGN,
	              clip_rgn_handle);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_RGB_FG_COLOR + 0,
	              0xffff);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_RGB_FG_COLOR + 2,
	              0xffff);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_RGB_FG_COLOR + 4,
	              0xffff);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_RGB_BK_COLOR + 0, 0);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_RGB_BK_COLOR + 2, 0);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_RGB_BK_COLOR + 4, 0);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_PN_SIZE + 0, 1);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_PN_SIZE + 2, 1);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_PN_MODE, 8);
	WriteMacInt16(cgrafptr_addr + DSP_CGRAFPORT_OFF_TX_SIZE, 12);
	WriteMacInt32(cgrafptr_addr + DSP_CGRAFPORT_OFF_FG_COLOR, 0xffffffffu);
	WriteMacInt32(cgrafptr_addr + DSP_CGRAFPORT_OFF_BK_COLOR, 0x00000000u);

	ctx->cgrafptr_mac_addr = cgrafptr_addr;
	DSP_LOG("DSpGetBackBufferCGrafPtr: ctx=%u cgrafptr=0x%08x pixmapH=0x%08x "
	        "pixmap=0x%08x visRgn=0x%08x clipRgn=0x%08x baseAddr=0x%08x "
	        "rbRaw=0x%04x rb=%u bpp=%u",
	        ctx->handle, cgrafptr_addr, pixmap_handle_addr, pixmap_addr,
	        vis_rgn_handle, clip_rgn_handle, baseAddr_mac, row_bytes_field,
	        alignedRB, bpp);
	return cgrafptr_addr;
}

/* ====================================================================== *
 *  Pixel-format-aware present-to-framebuffer                             *
 *                                                                        *
 *  A latent bug surfaced when a                                          *
 *  DSp client transitions to a sub-32-bpp mode (Sims jumps to            *
 *  1024x768@16bpp after the initial 640x480@32 mode): the existing       *
 *  DSpEncodeBackBufferBlit helper issues a raw                           *
 *  [blit copyFromTexture:back_texture toTexture:framebuffer_texture]     *
 *  even though back_texture is R16Uint (xRGB1555) and                    *
 *  framebuffer_texture is BGRA8Unorm. Metal's debug blit-validation      *
 *  rejects pixel-size-mismatched format pairs and the app dies on the    *
 *  assertion. (At 32 bpp the helper works because both sides are         *
 *  BGRA8Unorm; the test harness's MakeSolidTexture is BGRA8Unorm for     *
 *  every depth, so this didn't surface in the 16-bpp DSpContextTests.)   *
 *                                                                        *
 *  Fix: route through a DSp-owned render pass for non-32bpp depths. The  *
 *  unpack fragment shader is compiled lazily from inline source at first *
 *  call (no .metal file additions, no Xcode project pbxproj edits, no    *
 *  new linkage). Every new symbol lives on the DSp side; no new          *
 *  threading primitives.                                                 *
 * ====================================================================== */

static const char *kDSpUnpackShaderSource =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct DSpUnpackVertexOut {\n"
    "    float4 position [[position]];\n"
    "    float2 texCoord;\n"
    "};\n"
    "\n"
    "/* Fullscreen-triangle vertex shader (3 vertices). UV mapped with Y\n"
    " * unflipped because back_texture pixel (0,0) is top-left in Mac\n"
    " * raster order and the framebuffer texture is also top-left origin.\n"
    " * Matches the unflipped sense of [blit copyFromTexture: ... ] which\n"
    " * preserves Mac raster orientation. */\n"
    "vertex DSpUnpackVertexOut dsp_unpack_vertex(uint vid [[vertex_id]])\n"
    "{\n"
    "    DSpUnpackVertexOut out;\n"
    "    float2 pos;\n"
    "    pos.x = (vid == 1) ? 3.0 : -1.0;\n"
    "    pos.y = (vid == 2) ? 3.0 : -1.0;\n"
    "    out.position = float4(pos, 0.0, 1.0);\n"
    "    /* Mac raster: top-left origin. Metal clip-space Y points up, so\n"
    "     * Y=+1 in clip maps to V=0 (top). Unflipped Y mapping: */\n"
    "    out.texCoord.x = (pos.x + 1.0) * 0.5;\n"
    "    out.texCoord.y = 1.0 - (pos.y + 1.0) * 0.5;\n"
    "    return out;\n"
    "}\n"
    "\n"
    "/* R8Uint indexed -> BGRA8Unorm. Mirrors compositor_fragment_indexed\n"
    " * while reading DSp's per-context planar RGB CLUT directly. */\n"
    "fragment float4 dsp_unpack_fragment_indexed(\n"
    "    DSpUnpackVertexOut in [[stage_in]],\n"
    "    texture2d<uint, access::read> tex [[texture(0)]],\n"
    "    constant uchar *gamma_lut [[buffer(0)]],\n"
    "    constant uint &fade_active [[buffer(1)]],\n"
    "    constant uchar *clut_rgb [[buffer(2)]],\n"
    "    constant uint &bits_per_pixel [[buffer(3)]],\n"
    "    constant uint &pixel_width [[buffer(4)]])\n"
    "{\n"
    "    (void)fade_active;\n"
    "    uint px = uint(in.texCoord.x * float(pixel_width));\n"
    "    uint py = uint(in.texCoord.y * float(tex.get_height()));\n"
    "    px = min(px, pixel_width - 1);\n"
    "    py = min(py, tex.get_height() - 1);\n"
    "\n"
    "    uint index = 0;\n"
    "    if (bits_per_pixel == 8u) {\n"
    "        index = tex.read(uint2(px, py)).r;\n"
    "    } else if (bits_per_pixel == 4u) {\n"
    "        uint byte_val = tex.read(uint2(px / 2u, py)).r;\n"
    "        index = ((px & 1u) == 0u) ? (byte_val >> 4u) : (byte_val & 0xFu);\n"
    "    } else if (bits_per_pixel == 2u) {\n"
    "        uint byte_val = tex.read(uint2(px / 4u, py)).r;\n"
    "        uint shift = (3u - (px & 3u)) * 2u;\n"
    "        index = (byte_val >> shift) & 0x3u;\n"
    "    } else if (bits_per_pixel == 1u) {\n"
    "        uint byte_val = tex.read(uint2(px / 8u, py)).r;\n"
    "        index = (byte_val >> (7u - (px & 7u))) & 0x1u;\n"
    "    }\n"
    "\n"
    "    uint clut_offset = index * 3u;\n"
    "    float r = float(gamma_lut[clut_rgb[clut_offset + 0u]])        / 255.0;\n"
    "    float g = float(gamma_lut[256u + clut_rgb[clut_offset + 1u]]) / 255.0;\n"
    "    float b = float(gamma_lut[512u + clut_rgb[clut_offset + 2u]]) / 255.0;\n"
    "    return float4(r, g, b, 1.0);\n"
    "}\n"
    "\n"
    "fragment float4 dsp_unpack_fragment_indexed_compositor32(\n"
    "    DSpUnpackVertexOut in [[stage_in]],\n"
    "    texture2d<uint, access::read> tex [[texture(0)]],\n"
    "    constant uchar *gamma_lut [[buffer(0)]],\n"
    "    constant uint &fade_active [[buffer(1)]],\n"
    "    constant uchar *clut_rgb [[buffer(2)]],\n"
    "    constant uint &bits_per_pixel [[buffer(3)]],\n"
    "    constant uint &pixel_width [[buffer(4)]])\n"
    "{\n"
    "    (void)fade_active;\n"
    "    uint px = uint(in.texCoord.x * float(pixel_width));\n"
    "    uint py = uint(in.texCoord.y * float(tex.get_height()));\n"
    "    px = min(px, pixel_width - 1);\n"
    "    py = min(py, tex.get_height() - 1);\n"
    "\n"
    "    uint index = 0;\n"
    "    if (bits_per_pixel == 8u) {\n"
    "        index = tex.read(uint2(px, py)).r;\n"
    "    } else if (bits_per_pixel == 4u) {\n"
    "        uint byte_val = tex.read(uint2(px / 2u, py)).r;\n"
    "        index = ((px & 1u) == 0u) ? (byte_val >> 4u) : (byte_val & 0xFu);\n"
    "    } else if (bits_per_pixel == 2u) {\n"
    "        uint byte_val = tex.read(uint2(px / 4u, py)).r;\n"
    "        uint shift = (3u - (px & 3u)) * 2u;\n"
    "        index = (byte_val >> shift) & 0x3u;\n"
    "    } else if (bits_per_pixel == 1u) {\n"
    "        uint byte_val = tex.read(uint2(px / 8u, py)).r;\n"
    "        index = (byte_val >> (7u - (px & 7u))) & 0x1u;\n"
    "    }\n"
    "\n"
    "    uint clut_offset = index * 3u;\n"
    "    float r = float(gamma_lut[clut_rgb[clut_offset + 0u]])        / 255.0;\n"
    "    float g = float(gamma_lut[256u + clut_rgb[clut_offset + 1u]]) / 255.0;\n"
    "    float b = float(gamma_lut[512u + clut_rgb[clut_offset + 2u]]) / 255.0;\n"
    "    return float4(g, r, 1.0, b);\n"
    "}\n"
    "\n"
    "static inline float4 dsp_unpack_rgb555_to_rgba(\n"
    "    uint packed,\n"
    "    constant uchar *gamma_lut,\n"
    "    uint fade_active)\n"
    "{\n"
    "    (void)fade_active;\n"
    "    uint R = (packed >> 10) & 0x1F;\n"
    "    uint G = (packed >>  5) & 0x1F;\n"
    "    uint B =  packed        & 0x1F;\n"
    "    uint idx_r = (R * 255u) / 31u;\n"
    "    uint idx_g = (G * 255u) / 31u;\n"
    "    uint idx_b = (B * 255u) / 31u;\n"
    "    float r = float(gamma_lut[idx_r])        / 255.0;\n"
    "    float g = float(gamma_lut[256u + idx_g]) / 255.0;\n"
    "    float b = float(gamma_lut[512u + idx_b]) / 255.0;\n"
    "    return float4(r, g, b, 1.0);\n"
    "}\n"
    "\n"
    "static inline float4 dsp_store_for_compositor_32bpp(float4 rgba)\n"
    "{\n"
    "    /* MetalCompositorPresent's 32-bit shader treats the compositor\n"
    "     * texture as classic big-endian ARGB memory and reconstructs RGB\n"
    "     * as (s.g, s.r, s.a). Store front-staging pixels in that layout. */\n"
    "    return float4(rgba.g, rgba.r, 1.0, rgba.b);\n"
    "}\n"
    "\n"
    "/* R16Uint xRGB1555 -> BGRA8Unorm. Mirrors compositor_fragment_16bpp\n"
    " * (compositor_shaders.metal:143). The Mac stores 16-bit pixels as\n"
    " * big-endian xRGB1555, but ARM64 reads them as little-endian. */\n"
    "/* Non-visible-path twin: DSp force-resize to 32bpp routes the\n"
    " * visible pixels through compositor_fragment_32bpp via the blit fast-path;\n"
    " * this unpack pass is hit only for the rare R16Uint-back / non-BGRA\n"
    " * framebuffer case. We sample the same display-ready planar gamma_lut the\n"
    " * compositor present shaders use, for four-shader consistency. */\n"
    "fragment float4 dsp_unpack_fragment_16bpp(\n"
    "    DSpUnpackVertexOut in [[stage_in]],\n"
    "    texture2d<uint, access::read> tex [[texture(0)]],\n"
    "    constant uchar *gamma_lut [[buffer(0)]],\n"
    "    constant uint &fade_active [[buffer(1)]])\n"
    "{\n"
    "    uint w = tex.get_width();\n"
    "    uint h = tex.get_height();\n"
    "    uint px = uint(in.texCoord.x * float(w));\n"
    "    uint py = uint(in.texCoord.y * float(h));\n"
    "    px = min(px, w - 1);\n"
    "    py = min(py, h - 1);\n"
    "    uint packed = tex.read(uint2(px, py)).r;\n"
    "    /* Byte-swap big-endian -> little-endian (matches compositor\n"
    "     * shader; Mac authored xRGB1555 big-endian). */\n"
    "    packed = ((packed & 0xFF) << 8) | ((packed >> 8) & 0xFF);\n"
    "    return dsp_unpack_rgb555_to_rgba(packed, gamma_lut, fade_active);\n"
    "}\n"
    "\n"
    "/* R16Uint xRGB1555 -> BGRA8Unorm for native-order staging inputs.\n"
    " * Most Mac-authored 16-bit DSp pixels use dsp_unpack_fragment_16bpp,\n"
    " * which swaps big-endian xRGB1555 before decoding. */\n"
    "fragment float4 dsp_unpack_fragment_16bpp_native(\n"
    "    DSpUnpackVertexOut in [[stage_in]],\n"
    "    texture2d<uint, access::read> tex [[texture(0)]],\n"
    "    constant uchar *gamma_lut [[buffer(0)]],\n"
    "    constant uint &fade_active [[buffer(1)]])\n"
    "{\n"
    "    uint w = tex.get_width();\n"
    "    uint h = tex.get_height();\n"
    "    uint px = uint(in.texCoord.x * float(w));\n"
    "    uint py = uint(in.texCoord.y * float(h));\n"
    "    px = min(px, w - 1);\n"
    "    py = min(py, h - 1);\n"
    "    uint packed = tex.read(uint2(px, py)).r;\n"
    "    return dsp_unpack_rgb555_to_rgba(packed, gamma_lut, fade_active);\n"
    "}\n"
    "\n"
    "fragment float4 dsp_unpack_fragment_16bpp_compositor32(\n"
    "    DSpUnpackVertexOut in [[stage_in]],\n"
    "    texture2d<uint, access::read> tex [[texture(0)]],\n"
    "    constant uchar *gamma_lut [[buffer(0)]],\n"
    "    constant uint &fade_active [[buffer(1)]])\n"
    "{\n"
    "    uint w = tex.get_width();\n"
    "    uint h = tex.get_height();\n"
    "    uint px = uint(in.texCoord.x * float(w));\n"
    "    uint py = uint(in.texCoord.y * float(h));\n"
    "    px = min(px, w - 1);\n"
    "    py = min(py, h - 1);\n"
    "    uint packed = tex.read(uint2(px, py)).r;\n"
    "    packed = ((packed & 0xFF) << 8) | ((packed >> 8) & 0xFF);\n"
    "    return dsp_store_for_compositor_32bpp(\n"
    "        dsp_unpack_rgb555_to_rgba(packed, gamma_lut, fade_active));\n"
    "}\n"
    "\n"
    "fragment float4 dsp_unpack_fragment_16bpp_native_compositor32(\n"
    "    DSpUnpackVertexOut in [[stage_in]],\n"
    "    texture2d<uint, access::read> tex [[texture(0)]],\n"
    "    constant uchar *gamma_lut [[buffer(0)]],\n"
    "    constant uint &fade_active [[buffer(1)]])\n"
    "{\n"
    "    uint w = tex.get_width();\n"
    "    uint h = tex.get_height();\n"
    "    uint px = uint(in.texCoord.x * float(w));\n"
    "    uint py = uint(in.texCoord.y * float(h));\n"
    "    px = min(px, w - 1);\n"
    "    py = min(py, h - 1);\n"
    "    uint packed = tex.read(uint2(px, py)).r;\n"
    "    return dsp_store_for_compositor_32bpp(\n"
    "        dsp_unpack_rgb555_to_rgba(packed, gamma_lut, fade_active));\n"
    "}\n";

/* Lazy-initialised PSOs for the indexed and 16 bpp unpack passes. Built once per
 * process; reused across every VBL present. The destination format is
 * pinned to BGRA8Unorm to match the compositor framebuffer texture
 * (metal_compositor.h:69). dispatch_once gives us thread-safe lazy init
 * without introducing a new mutex / atomic. */
static id<MTLRenderPipelineState> s_dsp_unpack_pso_indexed = nil;
static id<MTLRenderPipelineState> s_dsp_unpack_pso_indexed_compositor32 = nil;
static id<MTLRenderPipelineState> s_dsp_unpack_pso_16bpp = nil;
static id<MTLRenderPipelineState> s_dsp_unpack_pso_16bpp_native = nil;
static id<MTLRenderPipelineState> s_dsp_unpack_pso_16bpp_compositor32 = nil;
static id<MTLRenderPipelineState> s_dsp_unpack_pso_16bpp_native_compositor32 = nil;
static dispatch_once_t            s_dsp_unpack_pso_once = 0;
static bool                       s_dsp_unpack_pso_build_failed = false;

static id<MTLRenderPipelineState> DSpBuildUnpackPSO(id<MTLDevice> device,
                                                     id<MTLFunction> vfn,
                                                     id<MTLFunction> ffn,
                                                     const char *name)
{
	if (vfn == nil || ffn == nil) {
		DSP_LOG("DSpBuildUnpackPSOs: missing function for %s", name);
		return nil;
	}

	MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
	desc.vertexFunction   = vfn;
	desc.fragmentFunction = ffn;
	desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

	NSError *psoErr = nil;
	id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc
	                                                                        error:&psoErr];
	if (pso == nil) {
		DSP_LOG("DSpBuildUnpackPSOs: %s PSO creation failed: %s",
		        name, psoErr ? [[psoErr localizedDescription] UTF8String] : "(no error)");
	}
	return pso;
}

static void DSpBuildUnpackPSOs(void)
{
	id<MTLDevice> device = (__bridge id<MTLDevice>)SharedMetalDevice();
	if (device == nil) {
		s_dsp_unpack_pso_build_failed = true;
		DSP_LOG("DSpBuildUnpackPSOs: SharedMetalDevice returned nil");
		return;
	}

	NSError *libErr = nil;
	NSString *src = [NSString stringWithUTF8String:kDSpUnpackShaderSource];
	id<MTLLibrary> lib = [device newLibraryWithSource:src
	                                          options:nil
	                                            error:&libErr];
	if (lib == nil) {
		s_dsp_unpack_pso_build_failed = true;
		DSP_LOG("DSpBuildUnpackPSOs: newLibraryWithSource failed: %s",
		        libErr ? [[libErr localizedDescription] UTF8String] : "(no error)");
		return;
	}

	id<MTLFunction> vfn = [lib newFunctionWithName:@"dsp_unpack_vertex"];
	id<MTLFunction> indexed_ffn = [lib newFunctionWithName:@"dsp_unpack_fragment_indexed"];
	id<MTLFunction> indexed_compositor32_ffn =
	    [lib newFunctionWithName:@"dsp_unpack_fragment_indexed_compositor32"];
	id<MTLFunction> ffn_16bpp = [lib newFunctionWithName:@"dsp_unpack_fragment_16bpp"];
	id<MTLFunction> ffn_16bpp_native = [lib newFunctionWithName:@"dsp_unpack_fragment_16bpp_native"];
	id<MTLFunction> ffn_16bpp_compositor32 =
	    [lib newFunctionWithName:@"dsp_unpack_fragment_16bpp_compositor32"];
	id<MTLFunction> ffn_16bpp_native_compositor32 =
	    [lib newFunctionWithName:@"dsp_unpack_fragment_16bpp_native_compositor32"];
	if (vfn == nil) {
		s_dsp_unpack_pso_build_failed = true;
		DSP_LOG("DSpBuildUnpackPSOs: missing dsp_unpack_vertex");
		return;
	}

	s_dsp_unpack_pso_indexed = DSpBuildUnpackPSO(device, vfn, indexed_ffn, "indexed");
	s_dsp_unpack_pso_indexed_compositor32 =
	    DSpBuildUnpackPSO(device, vfn, indexed_compositor32_ffn,
	                      "indexed-compositor32");
	s_dsp_unpack_pso_16bpp   = DSpBuildUnpackPSO(device, vfn, ffn_16bpp, "16bpp");
	s_dsp_unpack_pso_16bpp_native =
	    DSpBuildUnpackPSO(device, vfn, ffn_16bpp_native, "16bpp-native");
	s_dsp_unpack_pso_16bpp_compositor32 =
	    DSpBuildUnpackPSO(device, vfn, ffn_16bpp_compositor32,
	                      "16bpp-compositor32");
	s_dsp_unpack_pso_16bpp_native_compositor32 =
	    DSpBuildUnpackPSO(device, vfn, ffn_16bpp_native_compositor32,
	                      "16bpp-native-compositor32");

	if (s_dsp_unpack_pso_indexed != nil)
		DSP_LOG("DSpBuildUnpackPSOs: indexed R8Uint->BGRA8Unorm PSO ready");
	if (s_dsp_unpack_pso_indexed_compositor32 != nil)
		DSP_LOG("DSpBuildUnpackPSOs: indexed compositor32 R8Uint->BGRA8Unorm PSO ready");
	if (s_dsp_unpack_pso_16bpp != nil)
		DSP_LOG("DSpBuildUnpackPSOs: 16bpp R16Uint->BGRA8Unorm PSO ready");
	if (s_dsp_unpack_pso_16bpp_native != nil)
		DSP_LOG("DSpBuildUnpackPSOs: 16bpp native R16Uint->BGRA8Unorm PSO ready");
	if (s_dsp_unpack_pso_16bpp_compositor32 != nil)
		DSP_LOG("DSpBuildUnpackPSOs: 16bpp compositor32 R16Uint->BGRA8Unorm PSO ready");
	if (s_dsp_unpack_pso_16bpp_native_compositor32 != nil)
		DSP_LOG("DSpBuildUnpackPSOs: 16bpp native compositor32 R16Uint->BGRA8Unorm PSO ready");
}

/*
 *  DSpEncodeUnpackRenderPass — full-screen render pass that samples
 *  ctx->back_texture (R8Uint or R16Uint) and writes BGRA8Unorm to
 *  framebuffer_texture. Called only when pixel formats differ; same-
 *  format presents take the blit fast path in DSpEncodePresentToFramebuffer.
 *
 *  Returns true on success, false if the PSO is unavailable or the
 *  format isn't currently supported.
 */
static bool DSpEncodeUnpackTextureRenderPass(DSpContextPrivate *ctx,
                                             id<MTLTexture> source_texture,
                                             uint32_t source_bpp,
                                             uint32_t source_pixel_width,
                                             const char *source_label,
                                             bool native_r16_order,
                                             bool use_display_gamma,
                                             bool write_compositor32_layout,
                                             id<MTLCommandBuffer> cb,
                                             id<MTLTexture> framebuffer_texture)
{
	if (ctx == nullptr || cb == nil || framebuffer_texture == nil) return false;
	if (source_texture == nil) return false;

	dispatch_once(&s_dsp_unpack_pso_once, ^{ DSpBuildUnpackPSOs(); });
	if (s_dsp_unpack_pso_build_failed) return false;

	MTLPixelFormat src_fmt = source_texture.pixelFormat;
	MTLPixelFormat dst_fmt = framebuffer_texture.pixelFormat;
	const uint32_t bpp = source_bpp;
	const bool indexed =
	    (src_fmt == MTLPixelFormatR8Uint && dst_fmt == MTLPixelFormatBGRA8Unorm &&
	     (bpp == 1 || bpp == 2 || bpp == 4 || bpp == 8));

	id<MTLRenderPipelineState> pso = nil;
	if (indexed) {
		pso = write_compositor32_layout ? s_dsp_unpack_pso_indexed_compositor32
		                                 : s_dsp_unpack_pso_indexed;
	} else if (src_fmt == MTLPixelFormatR16Uint && dst_fmt == MTLPixelFormatBGRA8Unorm) {
		if (write_compositor32_layout) {
			pso = native_r16_order ? s_dsp_unpack_pso_16bpp_native_compositor32
			                       : s_dsp_unpack_pso_16bpp_compositor32;
		} else {
			pso = native_r16_order ? s_dsp_unpack_pso_16bpp_native
			                       : s_dsp_unpack_pso_16bpp;
		}
	}
	if (pso == nil) {
		DSP_LOG("DSpEncodeUnpackRenderPass: unsupported format pair "
		        "(%s src=%lu dst=%lu bpp=%u) — present skipped to avoid "
		        "Metal-incompatible blit",
		        source_label ? source_label : "source",
		        (unsigned long)src_fmt, (unsigned long)dst_fmt, bpp);
		return false;
	}

	/* Build a one-shot render pass targeting framebuffer_texture. Load
	 * action = DontCare because we will overwrite every pixel in a
	 * fullscreen-triangle draw; store action = Store so the result
	 * persists for the compositor's subsequent sample pass. */
	MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
	rpd.colorAttachments[0].texture     = framebuffer_texture;
	rpd.colorAttachments[0].loadAction  = MTLLoadActionDontCare;
	rpd.colorAttachments[0].storeAction = MTLStoreActionStore;

	id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rpd];
	if (re == nil) {
		DSP_LOG("DSpEncodeUnpackRenderPass: renderCommandEncoder returned nil");
		return false;
	}

	/* Clamp viewport to the smaller of back-texture and framebuffer
	 * dimensions (matches DSpEncodeBackBufferBlit's clamp logic — a
	 * lagging compositor framebuffer during a mode-switch in flight must
	 * not be over-rasterised). */
	NSUInteger back_w = indexed ? (NSUInteger)source_pixel_width
	                            : source_texture.width;
	NSUInteger back_h = source_texture.height;
	NSUInteger fb_w   = framebuffer_texture.width;
	NSUInteger fb_h   = framebuffer_texture.height;
	NSUInteger vp_w   = (back_w < fb_w) ? back_w : fb_w;
	NSUInteger vp_h   = (back_h < fb_h) ? back_h : fb_h;

	MTLViewport vp = (MTLViewport){
		.originX = 0.0, .originY = 0.0,
		.width   = (double)vp_w, .height = (double)vp_h,
		.znear   = 0.0, .zfar   = 1.0,
	};
	[re setViewport:vp];

	[re setRenderPipelineState:pso];
	[re setFragmentTexture:source_texture atIndex:0];

	/* Bind gamma LUT (buffer 0) + compatibility fade_active (buffer 1).
	 * Normal DSp back-buffer presents use the same display-ready gamma LUT
	 * as the compositor. RAVE/DSp front-staging overlays use identity gamma
	 * so their 2D pixels are color-matched with the already-BGRA RAVE layer
	 * in the same frame.
	 *
	 * dsp_unpack_fragment_16bpp reads gamma_lut[idx] UNCONDITIONALLY, so
	 * buffer index 0 MUST always be bound. The inline shader still accepts
	 * fade_active at buffer 1 to keep the binding layout stable, but the LUT
	 * is already composed for display/fade policy before it reaches Metal. */
	id<MTLBuffer> gamma_buf = nil;
	if (use_display_gamma) {
		gamma_buf = (__bridge id<MTLBuffer>)MetalCompositorGetGammaLUTBuffer();
	}
	if (gamma_buf == nil) {
		gamma_buf = (__bridge id<MTLBuffer>)MetalCompositorGetGammaIdentityBuffer();
	}
	if (gamma_buf == nil) {
		DSP_LOG("DSpEncodeUnpackRenderPass: no gamma LUT buffer (compositor "
		        "uninitialized) — aborting unpack pass to avoid unbound-buffer "
		        "read; caller skips present");
		[re endEncoding];
		return false;
	}
	[re setFragmentBuffer:gamma_buf offset:0 atIndex:0];
	uint32_t fade_active = 0u;
	if (use_display_gamma) {
		const DMCModeSnapshot *snap = dmc_current_snapshot();
		fade_active = (snap && snap->fade_active) ? 1u : 0u;
	}
	fade_active = DSpUnpackShaderFadeActiveForGammaPolicy(
	    use_display_gamma,
	    fade_active != 0u);
	[re setFragmentBytes:&fade_active length:sizeof(fade_active) atIndex:1];
	if (indexed) {
		[re setFragmentBytes:ctx->clut_bytes length:768 atIndex:2];
		[re setFragmentBytes:&bpp length:sizeof(bpp) atIndex:3];
		uint32_t pixel_width = source_pixel_width;
		[re setFragmentBytes:&pixel_width length:sizeof(pixel_width) atIndex:4];
	}

	[re drawPrimitives:MTLPrimitiveTypeTriangle
	       vertexStart:0
	       vertexCount:3];
	[re endEncoding];

	if (indexed) {
		DSP_VLOG("DSpEncodeUnpackRenderPass: %s %lux%lu R8Uint indexed -> "
		         "BGRA8Unorm (bpp=%u output=%s gamma=%s shaderFade=%u "
		         "cold_start=%d empty=%d "
		         "clut0=%u,%u,%u clut1=%u,%u,%u)",
		         source_label ? source_label : "source",
		         (unsigned long)vp_w, (unsigned long)vp_h,
		         bpp, write_compositor32_layout ? "compositor32" : "normalized",
		         use_display_gamma ? "display" : "identity",
		         fade_active, ctx->dirty_cold_start, ctx->dirty_empty,
		         ctx->clut_bytes[0], ctx->clut_bytes[1], ctx->clut_bytes[2],
		         ctx->clut_bytes[3], ctx->clut_bytes[4], ctx->clut_bytes[5]);
	} else {
		DSP_VLOG("DSpEncodeUnpackRenderPass: %s %lux%lu R16Uint -> BGRA8Unorm "
		         "(bpp=%u order=%s output=%s gamma=%s shaderFade=%u "
		         "cold_start=%d empty=%d)",
		         source_label ? source_label : "source",
		         (unsigned long)vp_w, (unsigned long)vp_h,
		         bpp, native_r16_order ? "native" : "mac-be",
		         write_compositor32_layout ? "compositor32" : "normalized",
		         use_display_gamma ? "display" : "identity", fade_active,
		         ctx->dirty_cold_start, ctx->dirty_empty);
	}
	return true;
}

static bool DSpEncodeUnpackRenderPass(DSpContextPrivate *ctx,
                                       id<MTLCommandBuffer> cb,
                                       id<MTLTexture> framebuffer_texture)
{
	if (ctx == nullptr) return false;
	const bool write_compositor32_layout =
	    DSpBackBufferWritesCompositor32Layout();
	return DSpEncodeUnpackTextureRenderPass(ctx,
	                                        ctx->back_texture,
	                                        ctx->attr.backBufferBestDepth,
	                                        DSpContextBackBufferWidth(ctx),
	                                        "backBuffer",
	                                        false,
	                                        DSpBackBufferUnpackUsesDisplayGamma(
	                                            write_compositor32_layout),
	                                        write_compositor32_layout,
	                                        cb,
	                                        framebuffer_texture);
}

extern "C" void DSpEncodePresentToFramebuffer(DSpContextPrivate *ctx,
                                                void *command_buffer_raw,
                                                void *framebuffer_texture_raw)
{
	id<MTLCommandBuffer> cb =
	    (__bridge id<MTLCommandBuffer>)command_buffer_raw;
	id<MTLTexture> framebuffer_texture =
	    (__bridge id<MTLTexture>)framebuffer_texture_raw;
	if (ctx == nullptr || cb == nil || framebuffer_texture == nil) return;
	if (ctx->back_texture == nil) return;

	MTLPixelFormat src_fmt = ctx->back_texture.pixelFormat;
	MTLPixelFormat dst_fmt = framebuffer_texture.pixelFormat;

	if (src_fmt == dst_fmt) {
		/* Fast path: matched formats (e.g. both BGRA8Unorm at 32 bpp).
		 * Open a blit encoder and reuse the proven dirty-state-aware
		 * helper. */
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		if (blit == nil) {
			DSP_LOG("DSpEncodePresentToFramebuffer: blitCommandEncoder "
			        "returned nil (fast path)");
			return;
		}
		DSpEncodeBackBufferBlit(ctx, (__bridge void *)blit,
		                         (__bridge void *)framebuffer_texture);
		[blit endEncoding];
		return;
	}

	/* Mismatched-format path: run a DSp-owned render pass that unpacks
	 * the source pixel format and writes BGRA8Unorm to the framebuffer
	 * texture. */
	bool ok = DSpEncodeUnpackRenderPass(ctx, cb, framebuffer_texture);
	if (!ok) {
		DSP_LOG("DSpEncodePresentToFramebuffer: no compatible present path "
		        "(src=%lu dst=%lu bpp=%u)",
		        (unsigned long)src_fmt, (unsigned long)dst_fmt,
		        ctx->attr.backBufferBestDepth);
		return;
	}

	/* Render pass succeeded — apply the same dirty-state reset that the
	 * blit fast path does inside DSpEncodeBackBufferBlit so callers see
	 * uniform post-condition semantics. */
	ctx->dirty_empty       = true;
	ctx->dirty_cold_start  = false;
	ctx->dirty_left = ctx->dirty_top = ctx->dirty_right = ctx->dirty_bottom = 0;
}

extern "C" bool DSpEncodeFrontBufferStagingToFramebuffer(DSpContextPrivate *ctx,
                                                          void *command_buffer_raw,
                                                          void *framebuffer_texture_raw)
{
	id<MTLCommandBuffer> cb =
	    (__bridge id<MTLCommandBuffer>)command_buffer_raw;
	id<MTLTexture> framebuffer_texture =
	    (__bridge id<MTLTexture>)framebuffer_texture_raw;
	if (ctx == nullptr || cb == nil || framebuffer_texture == nil) return false;
	if (ctx->front_staging_mac_addr == 0 || ctx->front_staging_size == 0) return false;

	const uint32_t front_depth =
	    DSpDisplayModeDepth(ctx->attr.backBufferBestDepth,
	                        ctx->attr.displayBestDepth);
	MTLPixelFormat fmt = DSpPixelFormatForDepthBits(front_depth);
	if (fmt == MTLPixelFormatInvalid) {
		DSP_LOG("DSpEncodeFrontBufferStagingToFramebuffer: unsupported "
		        "front depth %u", front_depth);
		return false;
	}

	const uint32_t w = ctx->attr.displayWidth;
	const uint32_t h = ctx->attr.displayHeight;
	const uint32_t row_bytes =
	    DSpDisplayModePitch(w, front_depth);
	const uint32_t buffer_size = row_bytes * h;
	uint32_t baseAddr_mac = DSpUsableGuestBaseOrZero(
		ctx->front_staging_mac_addr,
		buffer_size,
		(uint32_t)RAMBase,
		(uint32_t)RAMSize);
	if (baseAddr_mac == 0 || buffer_size > ctx->front_staging_size) {
		DSP_LOG("DSpEncodeFrontBufferStagingToFramebuffer: unusable front "
		        "staging addr=0x%08x size=%u need=%u depth=%u",
		        ctx->front_staging_mac_addr, ctx->front_staging_size,
		        buffer_size, front_depth);
		return false;
	}

	uint8_t *front_host = Mac2HostAddr(baseAddr_mac);
	if (front_host == NULL) {
		DSP_LOG("DSpEncodeFrontBufferStagingToFramebuffer: Mac2HostAddr "
		        "failed for 0x%08x", baseAddr_mac);
		return false;
	}

	GLCompositeLatestOffscreenToGuestSurfaceUsingLatestExtentIfNotSuppressed(
		baseAddr_mac,
		row_bytes,
		front_depth);

	/* Re-encode is keyed on CONTENT hash only — never on gamma_gen /
	 * fade_active. The encoded bytes are gamma-independent (the front path
	 * has DSpFrontStagingUsesDisplayGamma() == false and the compositor
	 * applies gamma at present time), so a gamma-keyed re-encode would
	 * re-blit this stale front-staging snapshot over freshly swapped
	 * frames on EVERY fade tick (gamma_gen bumps per VBL during fades). */
	const uint32_t current_hash =
	    DSpFrontStagingHashBytes(front_host, buffer_size);
	if (!DSpFrontStagingShouldEncodeHash(
	        &ctx->front_staging_present_state,
	        current_hash,
	        buffer_size)) {
		const uint32_t skips =
		    ctx->front_staging_present_state.unchanged_skips;
		if (skips <= 3 || (skips % 120u) == 0) {
			DSP_VLOG("DSpEncodeFrontBufferStagingToFramebuffer: skipped "
			         "unchanged front staging addr=0x%08x rowBytes=%u "
			         "%ux%u@%u hash=0x%08x skips=%u",
			         baseAddr_mac, row_bytes, w, h, front_depth,
			         current_hash, skips);
		}
		return true;
	}

	id<MTLDevice> device = (__bridge id<MTLDevice>)SharedMetalDevice();
	if (device == nil) {
		DSP_LOG("DSpEncodeFrontBufferStagingToFramebuffer: shared Metal "
		        "device is nil");
		return false;
	}

	id<MTLBuffer> staging_copy =
	    [device newBufferWithBytes:front_host
	                        length:buffer_size
	                       options:MTLResourceStorageModeShared];
	if (staging_copy == nil) {
		DSP_LOG("DSpEncodeFrontBufferStagingToFramebuffer: newBufferWithBytes "
		        "failed (size=%u depth=%u)", buffer_size, front_depth);
		return false;
	}

	MTLTextureDescriptor *td =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
	                                                       width:DSpDisplayModeTextureWidth(w, front_depth)
	                                                      height:h
	                                                   mipmapped:NO];
	td.usage       = MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModeShared;

	id<MTLTexture> front_texture =
	    [staging_copy newTextureWithDescriptor:td
	                                    offset:0
	                               bytesPerRow:row_bytes];
	if (front_texture == nil) {
		DSP_LOG("DSpEncodeFrontBufferStagingToFramebuffer: front texture "
		        "creation failed (rowBytes=%u depth=%u)",
		        row_bytes, front_depth);
		return false;
	}

	bool ok = false;
	if (front_texture.pixelFormat == framebuffer_texture.pixelFormat) {
		NSUInteger copy_w = (NSUInteger)w;
		NSUInteger copy_h = (NSUInteger)h;
		if (copy_w > front_texture.width) copy_w = front_texture.width;
		if (copy_h > front_texture.height) copy_h = front_texture.height;
		if (copy_w > framebuffer_texture.width) copy_w = framebuffer_texture.width;
		if (copy_h > framebuffer_texture.height) copy_h = framebuffer_texture.height;

		if (copy_w > 0 && copy_h > 0) {
			id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
			if (blit != nil) {
				[blit copyFromTexture:front_texture
				           sourceSlice:0
				           sourceLevel:0
				          sourceOrigin:MTLOriginMake(0, 0, 0)
				            sourceSize:MTLSizeMake(copy_w, copy_h, 1)
				             toTexture:framebuffer_texture
				      destinationSlice:0
				      destinationLevel:0
				     destinationOrigin:MTLOriginMake(0, 0, 0)];
				[blit endEncoding];
				ok = true;
				DSP_VLOG("DSpEncodeFrontBufferStagingToFramebuffer: direct "
				         "front staging blit %lux%lu",
				         (unsigned long)copy_w, (unsigned long)copy_h);
			} else {
				DSP_VLOG("DSpEncodeFrontBufferStagingToFramebuffer: "
				         "blitCommandEncoder returned nil for direct front "
				         "staging path");
			}
		}
	} else {
		ok = DSpEncodeUnpackTextureRenderPass(ctx,
		                                      front_texture,
		                                      front_depth,
		                                      w,
		                                      "frontStaging",
		                                      DSpFrontStagingUsesNativeR16OrderInMetal(),
		                                      DSpFrontStagingUsesDisplayGamma(),
		                                      DSpFrontStagingWritesCompositor32Layout(),
		                                      cb,
		                                      framebuffer_texture);
	}
	if (ok) {
		DSpFrontStagingRememberHash(
		    &ctx->front_staging_present_state,
		    current_hash,
		    buffer_size);
		DSP_VLOG("DSpEncodeFrontBufferStagingToFramebuffer: presented "
		         "front staging addr=0x%08x rowBytes=%u %ux%u@%u "
		         "hash=0x%08x",
		         baseAddr_mac, row_bytes, w, h, front_depth, current_hash);
	}
	return ok;
}
