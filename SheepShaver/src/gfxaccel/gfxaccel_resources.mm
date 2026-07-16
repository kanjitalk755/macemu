/*
 *  gfxaccel_resources.mm - Metal allocation implementation.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Metal-side half of gfxaccel_resources. Owns:
 *    - per-engine overlay texture fleet (two MTLTextures per engine slot,
 *      same-resolution recycle);
 *    - the framebuffer MTLBuffer wrapping emulated Mac RAM via the
 *      zero-copy newBufferWithBytesNoCopy path (byte-identical to
 *      metal_compositor.mm:279-282);
 *    - the shared DepthStencil texture (placeholder; consumed by the
 *      SubmitFrame render pass).
 *
 *  Page-alignment guard on the framebuffer buffer:
 *  iOS 17+ 16K-page devices fail the Metal validation assertion
 *  "newBufferWithBytesNoCopy:pointer is not %d byte aligned" if host_base
 *  is not vm_page_size-aligned. vm_allocate (used by the_buffer allocator)
 *  already produces page-aligned pointers; the guard is cheap insurance.
 *
 *  ARC ownership model: s_overlay_fleet + s_framebuffer_buffer +
 *  s_depth_stencil are strong references under ARC (assigning nil
 *  releases). newBufferWithBytesNoCopy uses deallocator:nil - Metal does
 *  NOT own the backing memory; the emul runtime does. Both properties
 *  are required by the zero-copy contract.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdint>
#include <cstdio>
#include <unistd.h>         // sysconf(_SC_PAGESIZE)

#include "gfxaccel_resources.h"
#include "metal_device_shared.h"
#include "metal_compositor.h"
#include "display_mode_controller.h"
#include "vbl_source.h"

// ---------------------------------------------------------------------------
// File-scope Metal state
// ---------------------------------------------------------------------------

typedef struct {
	id<MTLTexture> tex[2];
	uint32_t       w;
	uint32_t       h;
	uint32_t       fmt;         // stored as uint32_t so the bridging header
	                            // can pass numeric MTLPixelFormat values
} GfxResOverlaySlot;

static GfxResOverlaySlot s_overlay_fleet[kGfxEngineCount] = {};

static id<MTLBuffer>  s_framebuffer_buffer    = nil;
static void          *s_framebuffer_host_base = NULL;
static uint32_t       s_framebuffer_length    = 0;

// Depth/stencil texture is a placeholder. The SubmitFrame render pass
// will consume it; the slot is kept wired so subsequent work can add
// texture creation without module churn.
static id<MTLTexture> s_depth_stencil         = nil;

// Static-duration page size snapshot captured on first call. sysconf
// result is invariant for the process lifetime.
static uintptr_t gfxres_page_size(void)
{
	long ps = sysconf(_SC_PAGESIZE);
	return (ps > 0) ? (uintptr_t)ps : (uintptr_t)4096;
}

// Forward declaration: owner-map clear helper used by shutdown.
// Implementation lives near the map definition below.
static void gfxres_clear_owner_map_on_shutdown(void);

// ---------------------------------------------------------------------------
// Init / shutdown helpers (called from gfxaccel_resources.cpp)
// ---------------------------------------------------------------------------

extern "C" void gfxaccel_resources_mm_init_metal_state(void)
{
	// Zero the overlay fleet. No MTLTextures are allocated until an
	// engine calls gfxaccel_resources_vend_overlay_texture_indexed().
	for (uint32_t i = 0; i < (uint32_t)kGfxEngineCount; ++i) {
		s_overlay_fleet[i].tex[0] = nil;
		s_overlay_fleet[i].tex[1] = nil;
		s_overlay_fleet[i].w   = 0;
		s_overlay_fleet[i].h   = 0;
		s_overlay_fleet[i].fmt = 0;
	}

	// The framebuffer MTLBuffer is lazily created on first vend; do not
	// force-create one here (we don't know the buffer / size yet).
	s_framebuffer_buffer    = nil;
	s_framebuffer_host_base = NULL;
	s_framebuffer_length    = 0;

	s_depth_stencil = nil;
}

extern "C" void gfxaccel_resources_mm_shutdown_metal_state(void)
{
	for (uint32_t i = 0; i < (uint32_t)kGfxEngineCount; ++i) {
		s_overlay_fleet[i].tex[0] = nil;  // ARC release
		s_overlay_fleet[i].tex[1] = nil;
		s_overlay_fleet[i].w   = 0;
		s_overlay_fleet[i].h   = 0;
		s_overlay_fleet[i].fmt = 0;
	}

	s_framebuffer_buffer    = nil;  // ARC release; deallocator:nil means
	                                // the emul RAM backing is NOT freed
	                                // - it belongs to the emul runtime.
	s_framebuffer_host_base = NULL;
	s_framebuffer_length    = 0;

	s_depth_stencil = nil;

	// Clear owner-tag map on shutdown so stale pointers do not survive
	// shutdown/restart cycles. The forward-declared helper is implemented
	// alongside the map storage below.
	gfxres_clear_owner_map_on_shutdown();
}

// ---------------------------------------------------------------------------
// Framebuffer MTLBuffer
// ---------------------------------------------------------------------------

extern "C" void *gfxaccel_resources_get_framebuffer_buffer(void *host_base, uint32_t length)
{
	if (host_base == NULL || length == 0) {
		fprintf(stderr, "[gfxaccel_resources] get_framebuffer_buffer: invalid "
		                "host_base=%p length=%u\n", host_base, (unsigned)length);
		return NULL;
	}

	// Page-alignment guard.
	const uintptr_t page = gfxres_page_size();
	if (((uintptr_t)host_base & (page - 1)) != 0) {
		fprintf(stderr, "[gfxaccel_resources] get_framebuffer_buffer: "
		                "host_base=%p is NOT page-aligned (page=%lu) - "
		                "newBufferWithBytesNoCopy would assert on 16K-page "
		                "devices. Returning NULL.\n",
		        host_base, (unsigned long)page);
		return NULL;
	}

	// Cache hit: same backing + length.
	if (s_framebuffer_buffer != nil &&
	    s_framebuffer_host_base == host_base &&
	    s_framebuffer_length == length) {
		return (__bridge void *)s_framebuffer_buffer;
	}

	// Cache miss: release prior buffer (ARC) and allocate a fresh one.
	s_framebuffer_buffer    = nil;
	s_framebuffer_host_base = NULL;
	s_framebuffer_length    = 0;

	id<MTLDevice> device = (__bridge id<MTLDevice>)SharedMetalDevice();
	if (device == nil) {
		fprintf(stderr, "[gfxaccel_resources] get_framebuffer_buffer: "
		                "SharedMetalDevice() returned nil\n");
		return NULL;
	}

	// Byte-identical to metal_compositor.mm:279-282 zero-copy creation.
	id<MTLBuffer> buf = [device newBufferWithBytesNoCopy:host_base
	                                              length:length
	                                             options:MTLResourceStorageModeShared
	                                         deallocator:nil];
	if (buf == nil) {
		fprintf(stderr, "[gfxaccel_resources] get_framebuffer_buffer: "
		                "newBufferWithBytesNoCopy returned nil "
		                "(host_base=%p length=%u)\n",
		        host_base, (unsigned)length);
		return NULL;
	}

	s_framebuffer_buffer    = buf;
	s_framebuffer_host_base = host_base;
	s_framebuffer_length    = length;

	return (__bridge void *)s_framebuffer_buffer;
}

// ---------------------------------------------------------------------------
// Per-engine overlay texture fleet
// ---------------------------------------------------------------------------

extern "C" void *gfxaccel_resources_vend_overlay_texture_indexed(uint32_t engine_id,
                                                                 uint32_t texture_index,
                                                                 uint32_t width,
                                                                 uint32_t height,
                                                                 uint32_t pixel_format);

extern "C" void *gfxaccel_resources_vend_overlay_texture(uint32_t engine_id,
                                                         uint32_t width,
                                                         uint32_t height,
                                                         uint32_t pixel_format)
{
	return gfxaccel_resources_vend_overlay_texture_indexed(
	    engine_id, 0, width, height, pixel_format);
}

extern "C" void *gfxaccel_resources_vend_overlay_texture_indexed(uint32_t engine_id,
                                                                 uint32_t texture_index,
                                                                 uint32_t width,
                                                                 uint32_t height,
                                                                 uint32_t pixel_format)
{
	if (engine_id >= (uint32_t)kGfxEngineCount) {
		fprintf(stderr, "[gfxaccel_resources] vend_overlay_texture_indexed: engine_id=%u "
		                "out of range (max=%u)\n",
		        (unsigned)engine_id, (unsigned)kGfxEngineCount);
		return NULL;
	}
	if (texture_index >= 2) {
		fprintf(stderr, "[gfxaccel_resources] vend_overlay_texture_indexed(engine_id=%u): "
		                "texture_index=%u out of range\n",
		        (unsigned)engine_id, (unsigned)texture_index);
		return NULL;
	}
	if (width == 0 || height == 0) {
		fprintf(stderr, "[gfxaccel_resources] vend_overlay_texture_indexed(engine_id=%u): "
		                "invalid dimensions %ux%u\n",
		        (unsigned)engine_id, (unsigned)width, (unsigned)height);
		return NULL;
	}

	GfxResOverlaySlot *slot = &s_overlay_fleet[engine_id];

	// Cache hit: same engine, same pair index, same dimensions + format.
	if (slot->tex[texture_index] != nil &&
	    slot->w == width &&
	    slot->h == height &&
	    slot->fmt == pixel_format) {
		return (__bridge void *)slot->tex[texture_index];
	}

	// Size/format change invalidates the pair as a unit.
	if ((slot->tex[0] != nil || slot->tex[1] != nil) &&
	    (slot->w != width || slot->h != height || slot->fmt != pixel_format)) {
		slot->tex[0] = nil;  // ARC release
		slot->tex[1] = nil;
		slot->w = 0;
		slot->h = 0;
		slot->fmt = 0;
	}

	id<MTLDevice> device = (__bridge id<MTLDevice>)SharedMetalDevice();
	if (device == nil) {
		fprintf(stderr, "[gfxaccel_resources] vend_overlay_texture_indexed(engine_id=%u): "
		                "SharedMetalDevice() returned nil\n",
		        (unsigned)engine_id);
		return NULL;
	}

	MTLTextureDescriptor *desc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:(MTLPixelFormat)pixel_format
		                             width:(NSUInteger)width
		                            height:(NSUInteger)height
		                         mipmapped:NO];
	desc.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModePrivate;

	id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
	if (tex == nil) {
		fprintf(stderr, "[gfxaccel_resources] vend_overlay_texture_indexed(engine_id=%u): "
		                "newTextureWithDescriptor returned nil "
		                "(idx=%u %ux%u fmt=%u)\n",
		        (unsigned)engine_id, (unsigned)texture_index,
		        (unsigned)width, (unsigned)height, (unsigned)pixel_format);
		return NULL;
	}

	slot->tex[texture_index] = tex;
	slot->w   = width;
	slot->h   = height;
	slot->fmt = pixel_format;

	return (__bridge void *)slot->tex[texture_index];
}

extern "C" void gfxaccel_resources_release_overlay_texture(uint32_t engine_id,
                                                           void *texture)
{
	if (texture == NULL) {
		return;
	}
	if (engine_id >= (uint32_t)kGfxEngineCount) {
		fprintf(stderr, "[gfxaccel_resources] release_overlay_texture: "
		                "engine_id=%u out of range\n", (unsigned)engine_id);
		return;
	}

	GfxResOverlaySlot *slot = &s_overlay_fleet[engine_id];

	id<MTLTexture> passed = (__bridge id<MTLTexture>)texture;
	uint32_t matched_index = 2;
	if (slot->tex[0] == passed) matched_index = 0;
	if (slot->tex[1] == passed) matched_index = 1;
	if (matched_index >= 2) {
		// Unknown/stale handle. Log and return - we don't own this
		// texture so we can't release it safely, and we don't know
		// which engine actually does.
		fprintf(stderr, "[gfxaccel_resources] release_overlay_texture(engine_id=%u): "
		                "texture=%p does not match cached pair (%p, %p); ignoring\n",
		        (unsigned)engine_id, texture,
		        (__bridge void *)slot->tex[0],
		        (__bridge void *)slot->tex[1]);
		return;
	}

	slot->tex[matched_index] = nil;  // ARC release
	if (slot->tex[0] == nil && slot->tex[1] == nil) {
		slot->w   = 0;
		slot->h   = 0;
		slot->fmt = 0;
	}
}

// ---------------------------------------------------------------------------
// Per-buffer owner-tag map
//
// Explicit per-buffer engine-id tagging. The "explicit tag" model is used
// (the alternative "DMC-implicit fallback" was rejected): every engine
// that allocates a buffer at a slot where arbitration may fire tags the
// buffer's owner here. The NQD conflict gate queries
// dmc_current_snapshot()->active_owner rather than this map in the hot
// path, but the map is the authoritative per-buffer record and is consumed
// by test harnesses (coexistence tests) to cross-check tag integrity.
//
// Bounded 32-slot array, linear scan on set/get/clear. No dynamic
// allocation. No mutex / atomic — DSp Reserve + engine allocations fire
// at mode-enter (not per-frame) on the emul thread, so O(n) with n<=32
// is well within budget. Single-writer invariant matches the rest of
// the resource manager.
//
// The compositor never reads this map. The CompositeLayer POD the
// compositor sees carries only slot + source; ownership lives here.
// ---------------------------------------------------------------------------

#define GFXRES_OWNER_MAP_CAP 32

typedef struct {
	void    *buffer;        // non-NULL == slot occupied
	uint32_t engine_id;     // kGfxEngineNQD/RAVE/GL/DSp
} GfxResOwnerEntry;

static GfxResOwnerEntry s_owner_map[GFXRES_OWNER_MAP_CAP];

extern "C" void gfxaccel_resources_set_buffer_owner(void *buffer,
                                                    uint32_t engine_id)
{
	if (buffer == NULL) return;
	if (engine_id >= (uint32_t)kGfxEngineCount) {
		fprintf(stderr, "[gfxaccel_resources] set_buffer_owner: engine_id=%u "
		                "out of range (max=%u)\n",
		        (unsigned)engine_id, (unsigned)kGfxEngineCount);
		return;
	}
	// Replace-in-place: if the buffer is already tagged, overwrite.
	for (uint32_t i = 0; i < GFXRES_OWNER_MAP_CAP; ++i) {
		if (s_owner_map[i].buffer == buffer) {
			s_owner_map[i].engine_id = engine_id;
			return;
		}
	}
	// Insert into first free slot.
	for (uint32_t i = 0; i < GFXRES_OWNER_MAP_CAP; ++i) {
		if (s_owner_map[i].buffer == NULL) {
			s_owner_map[i].buffer    = buffer;
			s_owner_map[i].engine_id = engine_id;
			return;
		}
	}
	fprintf(stderr, "[gfxaccel_resources] set_buffer_owner: owner-map "
	                "overflow (cap=%u) - increase GFXRES_OWNER_MAP_CAP. "
	                "buffer=%p engine_id=%u dropped.\n",
	        (unsigned)GFXRES_OWNER_MAP_CAP, buffer, (unsigned)engine_id);
}

extern "C" void gfxaccel_resources_clear_buffer_owner(void *buffer)
{
	if (buffer == NULL) return;
	for (uint32_t i = 0; i < GFXRES_OWNER_MAP_CAP; ++i) {
		if (s_owner_map[i].buffer == buffer) {
			s_owner_map[i].buffer    = NULL;
			s_owner_map[i].engine_id = 0;
			return;
		}
	}
}


// Owner-map clear helper invoked by the shutdown path. File-local (static)
// so production builds reset the map on shutdown/restart cycles.
static void gfxres_clear_owner_map_on_shutdown(void)
{
	for (uint32_t i = 0; i < GFXRES_OWNER_MAP_CAP; ++i) {
		s_owner_map[i].buffer    = NULL;
		s_owner_map[i].engine_id = 0;
	}
}

// ---------------------------------------------------------------------------
// Background/Foreground lifecycle
// ---------------------------------------------------------------------------

// DSp M2 hook state. Both NULL initially. M2's DrawSprocket will register
// its own enter/exit handlers here.
static GfxAccelLifecycleHookFn s_dsp_background_hook     = NULL;
static void                   *s_dsp_background_hook_ctx = NULL;
static GfxAccelLifecycleHookFn s_dsp_foreground_hook     = NULL;
static void                   *s_dsp_foreground_hook_ctx = NULL;

extern "C" void gfxaccel_handle_background_enter(void)
{
	// Step 1: Pause VBLSource (stop display link callbacks).
	vbl_source_set_paused(1);

	// Step 2: Drain in-flight GPU work. Submit an empty command buffer
	// and wait for GPU completion. This ensures any previously committed
	// work has finished before we release drawables.
	id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)SharedMetalCommandQueue();
	if (queue != nil) {
		id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
		if (cmdBuf != nil) {
			[cmdBuf commit];
			[cmdBuf waitUntilCompleted];
		}
	}

	// Step 3: Release drawables by setting drawableSize = CGSizeZero.
	// Apple-recommended pattern: setting drawableSize to zero releases
	// the drawable backing store without destroying the layer.
	void *layerPtr = MetalCompositorGetLayer();
	if (layerPtr != NULL) {
		CAMetalLayer *layer = (__bridge CAMetalLayer *)layerPtr;
		layer.drawableSize = CGSizeZero;
	}

	// Step 4: Invoke DSp M2 background hook if registered (NULL initially).
	if (s_dsp_background_hook != NULL) {
		s_dsp_background_hook(s_dsp_background_hook_ctx);
	}
}

extern "C" void gfxaccel_handle_foreground_enter(void)
{
	// Step 1: Restore drawableSize to current DMC snapshot dimensions.
	void *layerPtr = MetalCompositorGetLayer();
	if (layerPtr != NULL) {
		const struct DMCModeSnapshot *snap = dmc_current_snapshot();
		if (snap != NULL && snap->width > 0 && snap->height > 0) {
			CAMetalLayer *layer = (__bridge CAMetalLayer *)layerPtr;
			layer.drawableSize = CGSizeMake((CGFloat)snap->width,
			                                (CGFloat)snap->height);
		}
	}

	// Step 2: Resume VBLSource.
	vbl_source_set_paused(0);

	// Step 3: Invoke DSp M2 foreground hook if registered (NULL initially).
	if (s_dsp_foreground_hook != NULL) {
		s_dsp_foreground_hook(s_dsp_foreground_hook_ctx);
	}
}

extern "C" void gfxaccel_set_dsp_background_hook(GfxAccelLifecycleHookFn fn, void *ctx)
{
	s_dsp_background_hook     = fn;
	s_dsp_background_hook_ctx = ctx;
}

extern "C" void gfxaccel_set_dsp_foreground_hook(GfxAccelLifecycleHookFn fn, void *ctx)
{
	s_dsp_foreground_hook     = fn;
	s_dsp_foreground_hook_ctx = ctx;
}
