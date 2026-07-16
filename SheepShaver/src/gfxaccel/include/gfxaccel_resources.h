/*
 *  gfxaccel_resources.h - Cross-boundary Metal resource lifetime owner.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Foundation module that owns every Metal resource (MTLBuffer / MTLTexture) that
 *  crosses the engine <-> compositor boundary:
 *
 *    - Per-engine overlay texture fleet (two MTLTextures per engine slot,
 *      same-resolution recycle, no global refcount).
 *    - Zero-copy framebuffer MTLBuffer wrapping emulated Mac RAM via
 *      newBufferWithBytesNoCopy + MTLResourceStorageModeShared
 *      + deallocator:nil (page-alignment guard).
 *    - DMC subscriber fan-out registry so engines attach/detach via this
 *      module rather than subscribing to DMC directly.
 *
 *  Registration ordering:
 *    1. metal_compositor registers with DMC first (already in
 *       MetalCompositorInit).
 *    2. gfxaccel_resources_init() runs AFTER MetalCompositorInit and
 *       subscribes SECOND under the name "gfxaccel_resources".
 *    => LIFO on_mode_enter dispatch fires gfxaccel_resources.on_mode_enter
 *       FIRST (which fans out to engines to attach their resources) and
 *       metal_compositor.on_mode_enter LAST (so the compositor always sees
 *       attached engine resources on its first post-switch present).
 *       FIFO on_mode_exit dispatch fires metal_compositor.on_mode_exit
 *       FIRST (drops drawable refs) and gfxaccel_resources.on_mode_exit
 *       LAST (fans out to engines to detach their resources).
 *
 *  Scope caveats:
 *    - Single-threaded from the PPC emul thread. No std::mutex, no atomic.
 *      The DMC carve-out in the Threading section remains the ONLY
 *      acceleration-code module with concurrency primitives.
 *    - Dual-ownership of the framebuffer MTLBuffer is tolerated initially:
 *      metal_compositor.mm retains its own newBufferWithBytesNoCopy call
 *      (unchanged) while this module also creates one wrapping the same
 *      backing. Both use deallocator:nil, so the emul runtime retains
 *      sole ownership of the backing memory. A later revision makes the
 *      compositor consume gfxaccel_resources_get_framebuffer_buffer() and
 *      drops the duplicate.
 *    - Engine migration is deferred; no engine
 *      registers with this module yet. gfxaccel_resources_init() is a
 *      behavioral no-op at its first deployment.
 *
 *  C-callable throughout: the header can be included from .cpp, .mm, or
 *  Swift-via-bridging-header without pulling in Metal types; id<MTLBuffer>
 *  and id<MTLTexture> return values are exposed as void* and bridge-cast
 *  inside the .mm implementation.
 */

#ifndef GFXACCEL_RESOURCES_H
#define GFXACCEL_RESOURCES_H

#include <stdint.h>

#include "display_mode_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GfxEngineId enum.
 *
 * Declared ONLY in this header. MUST NOT appear in metal_compositor.{h,mm}
 * or compositor_shaders.metal. The compositor sees only CompositeLayer
 * entries and must remain engine-blind by construction.
 * `grep -rn "kGfxEngine"
 * SheepShaver/src/gfxaccel/metal_compositor.{h,mm}` MUST return 0.
 */
typedef enum {
	kGfxEngineNQD    = 0,
	kGfxEngineRAVE   = 1,
	kGfxEngineGL     = 2,
	kGfxEngineDSp    = 3,   /* fourth engine (DrawSprocket) */
	kGfxEngineCount  = 4
} GfxEngineId;

/*
 * Fan-out registration types.
 *
 * Engines register a pair of callbacks with gfxaccel_resources (NOT with
 * DMC directly). The resource manager is the single DMC subscriber for
 * engine lifecycle events and dispatches to its registered engines in
 * FIFO order on exit, LIFO (reverse) order on enter - matching the DMC
 * contract.
 *
 * Return kGfxAccelResNoErr (0) from attach/detach to accept the transition.
 * A non-zero attach return propagates back to DMC as a subscriber rejection
 * (kDMCErrSubscriberRejected) and triggers the rollback path, so
 * engines that fail to allocate their overlay texture can veto a mode
 * switch cleanly. Detach returns are advisory.
 */
typedef int32_t (*GfxResAttachFn)(uint32_t engine_id,
                                  const struct DMCModeSnapshot *incoming,
                                  void *ctx);
typedef int32_t (*GfxResDetachFn)(uint32_t engine_id,
                                  const struct DMCModeSnapshot *outgoing,
                                  void *ctx);

struct GfxResEngineHandlers {
	GfxResAttachFn attach;   /* nullable */
	GfxResDetachFn detach;   /* nullable */
	void          *ctx;      /* passed verbatim to attach/detach */
};

/*
 * Error codes. This set is kept small; the named codes
 * (kGfxAccelErrStaleGeneration, kGfxAccelErrInvalidDescriptor, etc.) are
 * added as the SubmitFrame API lands. Codes stay in the -4000 block to avoid
 * overlap with DMC's -3000..-3099 range.
 */
enum GfxAccelResourcesError {
	kGfxAccelResNoErr           = 0,
	kGfxAccelResErrGeneric      = -4001,
	kGfxAccelResErrNotInit      = -4002, /* called before gfxaccel_resources_init() */
	kGfxAccelResErrAlreadyInit  = -4003, /* defensive only; init is idempotent */
	kGfxAccelResErrInvalidEngine = -4004, /* engine_id >= kGfxEngineCount */
	kGfxAccelResErrMisaligned   = -4005, /* framebuffer not page-aligned */
	kGfxAccelResErrAllocFailed  = -4006  /* Metal newBuffer/newTexture returned nil */
};

/* --- Lifecycle --- */

/*
 * Initialize the resource manager. Idempotent - a second call returns
 * kGfxAccelResNoErr without side effects.
 *
 * Must be called AFTER SharedMetalDevice() is available and AFTER
 * MetalCompositorInit() has subscribed "compositor" to DMC. See
 * the ordering note in the file header.
 *
 * Returns kGfxAccelResNoErr on success;
 *         kGfxAccelResErrGeneric if DMC subscription fails
 *           (dmc_subscribe return is non-zero other than
 *            kDMCErrSubscriberAlreadyRegistered, which is tolerated).
 */
int32_t gfxaccel_resources_init(void);

/*
 * Tear down the resource manager. Idempotent - safe to call in Quiescent.
 *
 * Releases: the overlay-texture fleet, the framebuffer MTLBuffer, and the
 * shared DepthStencil texture. Drains the engine fan-out registry (any
 * engines that forgot to unregister are silently dropped at shutdown).
 * Unsubscribes from DMC.
 *
 * Must be called BEFORE MetalCompositorShutdown() to preserve the
 * tear-down ordering (compositor's on_mode_exit fires first during final
 * DMC exit; resources' detach fan-out fires last).
 */
void gfxaccel_resources_shutdown(void);

/* --- Engine fan-out registry --- */

/*
 * Register an engine's attach/detach handlers with the fan-out registry.
 *
 * Called by each engine (NQD / RAVE / GL) during its own init AFTER
 * gfxaccel_resources_init() has run. A subsequent registration for the
 * same engine_id REPLACES the prior entry (engines own their own state).
 * Handlers may be NULL individually.
 *
 * Engines do NOT call this yet. The registry is populated
 * as engines migrate.
 */
void gfxaccel_resources_register_engine(uint32_t engine_id,
                                        const struct GfxResEngineHandlers *h);

/*
 * Remove an engine's handlers from the fan-out registry.
 *
 * Called during engine shutdown. Silent no-op if engine_id is not
 * currently registered.
 */
void gfxaccel_resources_unregister_engine(uint32_t engine_id);

/* --- Framebuffer MTLBuffer --- */

/*
 * Vend the shared framebuffer MTLBuffer that wraps emulated Mac RAM.
 *
 * Created via [device newBufferWithBytesNoCopy:host_base length:length
 *                                       options:MTLResourceStorageModeShared
 *                                   deallocator:nil] - byte-identical to
 * metal_compositor.mm:279-282 so the zero-copy path is preserved by
 * construction (RES-05).
 *
 * Cached: repeated calls with the same (host_base, length) return the
 * same MTLBuffer handle. A call with different args releases the prior
 * buffer and creates a new one wrapping the new backing.
 *
 * Page-alignment guard: host_base MUST be page-aligned
 * (vm_page_size-aligned), matching the vm_allocate allocator used for
 * `the_buffer`. If misaligned, the function returns NULL and logs an
 * error (iOS 17 16K-page devices fail the Metal
 * validation assertion otherwise).
 *
 * Returns id<MTLBuffer> as void* on success; NULL if
 *   - gfxaccel_resources_init() has not been called yet, or
 *   - host_base is not page-aligned, or
 *   - SharedMetalDevice() returned nil, or
 *   - newBufferWithBytesNoCopy returned nil.
 *
 * Caller must NOT release the returned handle - it is owned by this
 * module. Lifetime ends at gfxaccel_resources_shutdown() or the next
 * call that invalidates the cache.
 */
void *gfxaccel_resources_get_framebuffer_buffer(void *host_base, uint32_t length);

/* --- Per-engine overlay texture fleet --- */

/*
 * Vend an overlay MTLTexture for the named engine.
 *
 * Per-engine: each engine may own two overlay textures for ping-pong
 * presentation. No global refcount. Same-resolution call returns the cached
 * texture at index 0 (recycle path); different-resolution call releases the
 * prior pair and allocates a new index-0 texture. Different engines get
 * different textures even at the same dimensions (no implicit sharing; the
 * refcount model goes away by construction).
 *
 * Texture is created with usage = RenderTarget | ShaderRead and
 * storageMode = Private (compute/render target; no CPU readback path).
 *
 * Returns id<MTLTexture> as void* on success; NULL if
 *   - engine_id >= kGfxEngineCount, or
 *   - gfxaccel_resources_init() has not been called yet, or
 *   - newTextureWithDescriptor returned nil.
 */
void *gfxaccel_resources_vend_overlay_texture(uint32_t engine_id,
                                              uint32_t width,
                                              uint32_t height,
                                              uint32_t pixel_format);

/*
 * Vend one texture from an engine's two-texture overlay pair.
 *
 * texture_index must be 0 or 1. Both indices for an engine share dimensions
 * and pixel format; a different size/format releases the existing pair.
 * Existing single-texture callers should keep using
 * gfxaccel_resources_vend_overlay_texture(), which is equivalent to index 0.
 */
void *gfxaccel_resources_vend_overlay_texture_indexed(uint32_t engine_id,
                                                      uint32_t texture_index,
                                                      uint32_t width,
                                                      uint32_t height,
                                                      uint32_t pixel_format);

/*
 * Release the overlay texture for the named engine.
 *
 * Called when an engine tears down (e.g. on DMC on_mode_exit or engine
 * shutdown). Idempotent: calling with an unknown/stale handle logs a
 * warning and returns without crashing. Passing NULL is a silent no-op.
 */
void gfxaccel_resources_release_overlay_texture(uint32_t engine_id, void *texture);

/* --- Per-buffer engine-id ownership tagging --- */

/*
 * Tag a resource record with the owning engine id. Used by DSp (and any
 * future engine) to record "this buffer belongs to engine X" so that
 * later arbitration paths (the NQD conflict gate, test harnesses) can
 * query the owner without branching on engine-specific types.
 *
 * buffer: id<MTLBuffer> cast to void* - the same handle returned by
 *   gfxaccel_resources_heap_alloc_buffer.
 * engine_id: one of kGfxEngineNQD/RAVE/GL/DSp (must be < kGfxEngineCount).
 *
 * Silently no-ops if buffer is NULL or engine_id >= kGfxEngineCount.
 * Storage: bounded in-memory map (32 slots; no dynamic alloc after init).
 * Single-threaded (emul-thread-only per the threading /
 * Locking Model). A subsequent call for the same buffer REPLACES the
 * prior tag.
 *
 * NOTE: the compositor NEVER queries this API. Ownership is a property
 * of the resource manager, not of the CompositeLayer POD - the
 * compositor-blindness invariant requires the compositor to see only
 * slot + source handle, never an engine identity.
 */
void gfxaccel_resources_set_buffer_owner(void *buffer, uint32_t engine_id);

/*
 * Remove a buffer's owner tag. Silent no-op if buffer is NULL or absent
 * from the map. Called from engine release paths before the buffer is
 * dropped so the map does not hold a dangling pointer.
 */
void gfxaccel_resources_clear_buffer_owner(void *buffer);

/* --- Background/Foreground lifecycle --- */

/*
 * Background enter handler. Called from Swift BackgroundLifecycleObserver
 * on main thread when the app enters background.
 *
 * Sequence:
 *   1. Pause VBLSource (stop display link callbacks).
 *   2. Drain in-flight GPU work (wait for command buffers to complete).
 *   3. Release drawables by setting drawableSize = CGSizeZero.
 *   4. Invoke DSp M2 background hook if registered (NULL for now).
 *
 * Budget: must return within 50 ms on main thread.
 */
void gfxaccel_handle_background_enter(void);

/*
 * Foreground enter handler. Called from Swift BackgroundLifecycleObserver
 * on main thread when the app returns to foreground.
 *
 * Sequence:
 *   1. Restore drawableSize to current DMC snapshot dimensions.
 *   2. Resume VBLSource.
 *   3. Invoke DSp M2 foreground hook if registered (NULL for now).
 *
 * Rendering resumes on the next VBL tick.
 */
void gfxaccel_handle_foreground_enter(void);

/* DSp M2 hook registration. Both NULL for now. */
typedef void (*GfxAccelLifecycleHookFn)(void *ctx);
void gfxaccel_set_dsp_background_hook(GfxAccelLifecycleHookFn fn, void *ctx);
void gfxaccel_set_dsp_foreground_hook(GfxAccelLifecycleHookFn fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* GFXACCEL_RESOURCES_H */
