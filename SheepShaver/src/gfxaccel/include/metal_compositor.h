/*
 *  metal_compositor.h - Metal compositor for 2D framebuffer presentation
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  C++ callable interface for the Metal compositor that replaces SDL's
 *  rendering pipeline on iOS. Returns void/int so this header can be
 *  included from plain .cpp files without pulling in ObjC types.
 */

#ifndef METAL_COMPOSITOR_H
#define METAL_COMPOSITOR_H

#include <stdint.h>

#include "display_mode_controller.h"   /* struct DMCModeSnapshot forward */
#include "gfx_frame_pacing_policy.h"

/*
 * SubmitFrame API vocabulary.
 *
 * The compositor exposes a single engine-to-compositor publish entry point:
 * MetalCompositorSubmitFrame(const FrameDescriptor *desc). In production it
 * validates the FrameDescriptor, rejects stale DMC generations, caches the
 * last kLayerSlotOverlay layer in a single overlay mailbox, and returns.
 * MetalCompositorPresent is the only production path that acquires a
 * CAMetalLayer drawable, encodes, and presents.
 *
 * Non-overlay slots are accepted by SubmitFrame for API compatibility but are
 * ignored in production; underlay/framebuffer composition is not implemented.
 * The compositor is engine-blind by construction: it sees only CompositeLayer
 * PODs with opaque source texture handles, never engine identifiers.
 *
 * All types below are POD and safe to include from plain .cpp files.
 * The `source` field is declared as void* so engines can cast from
 * id<MTLTexture> at use site without pulling ObjC into cpp translation
 * units.
 */

/*
 * DMCLayerSlot - strict z-order slots.
 * Arbitrary z-indices are out of scope; named slots make the compositor
 * inspectable (`grep kLayerSlot*` returns the full z-vocabulary).
 */
typedef enum {
	kLayerSlotUnderlay    = 0,
	kLayerSlotFramebuffer = 1,
	kLayerSlotOverlay     = 2,
	kLayerSlotCount       = 3
} DMCLayerSlot;

/*
 * CompositeBlendMode - per-layer blend mode.
 * Covers every composite mode classic 2D and 3D engines can emit today;
 * additional modes stay deferred until an app-level need surfaces.
 */
typedef enum {
	kBlendOpaque        = 0,
	kBlendPremultiplied = 1,
	kBlendStraight      = 2
} CompositeBlendMode;

/*
 * CompositeLayer - POD layer record.
 *
 * `source` is an id<MTLTexture> cast to void* so this header compiles in
 * plain .cpp files. Engines bridge-cast at their use site. The
 * source texture must be BGRA8Unorm for overlay-cache presentation.
 * Production SubmitFrame does not encode framebuffer/underlay layers.
 *
 * Destination rectangle fields use floats so the compositor can express
 * fractional positioning when the drawable size doesn't match the
 * framebuffer at integer multiples.
 */
struct CompositeLayer {
	void              *source;          /* id<MTLTexture> as void* - BGRA8Unorm */
	uint32_t           src_origin_x;
	uint32_t           src_origin_y;
	uint32_t           src_size_w;
	uint32_t           src_size_h;
	float              dst_origin_x;
	float              dst_origin_y;
	float              dst_size_w;
	float              dst_size_h;
	DMCLayerSlot       slot;
	CompositeBlendMode blend;
	float              alpha;           /* 0.0..1.0; consulted only for non-Opaque blend modes */
};

/*
 * FrameDescriptor - POD frame record.
 *
 * Borrowed `layers` array is valid for the SubmitFrame call only;
 * compositor never holds the pointer beyond the call. `generation` must
 * match dmc_current_snapshot()->generation at SubmitFrame entry, else
 * the descriptor is rejected with kGfxAccelErrStaleGeneration and the
 * caller must rebuild (closes the UAF that made this
 * safe under concurrent rollback).
 *
 * `vbl_tick_target_usec` is retained for descriptor ABI shape only. Production
 * presentation is VBL-driven by MetalCompositorPresent, not SubmitFrame.
 */
struct FrameDescriptor {
	const struct CompositeLayer *layers;
	uint32_t                     layer_count;           /* 0..kMaxLayers (internal = 16) */
	uint32_t                     generation;            /* DMC snapshot generation */
	uint64_t                     vbl_tick_target_usec;  /* advisory */
};

/*
 * GfxAccelError - error block. Starts at -4000 to avoid overlap
 * with DMC's -3000..-3099 range.
 */
enum GfxAccelError {
	kGfxAccelNoErr                       =     0,
	kGfxAccelErrInvalidDescriptor        = -4001,   /* NULL desc or layer_count > kMaxLayers or layers==NULL */
	kGfxAccelErrStaleGeneration          = -4002,   /* desc->generation != current snapshot gen */
	kGfxAccelErrInvalidSlot              = -4003,   /* slot enum out of range */
	kGfxAccelErrDrawableUnavailable      = -4004,   /* nextDrawable returned nil */
	kGfxAccelErrPipelineUnavailable      = -4005,   /* PSO build failed */
	kGfxAccelErrEngineNotRegistered      = -4006,   /* vend called for unregistered engine */
	kGfxAccelErrTextureFormatUnsupported = -4007    /* non-BGRA8Unorm source texture */
};

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Depth values correspond to VIDEO_DEPTH_* constants from video_blit.h:
 *   VIDEO_DEPTH_1BIT  — 1-bit indexed (2 colors)
 *   VIDEO_DEPTH_2BIT  — 2-bit indexed (4 colors)
 *   VIDEO_DEPTH_4BIT  — 4-bit indexed (16 colors)
 *   VIDEO_DEPTH_8BIT  — 8-bit indexed (256 colors)
 *   VIDEO_DEPTH_16BIT — 16-bit direct (xRGB1555 big-endian)
 *   VIDEO_DEPTH_32BIT — 32-bit direct (BGRA8)
 */

/*
 * Initialize the Metal compositor for 2D framebuffer presentation.
 *
 * Creates a CompositorMetalView (UIView + CAMetalLayer) covering the full
 * iOS window, wraps the framebuffer as a zero-copy shared MTLBuffer with
 * a MTLTexture view, and builds the render pipeline for the specified depth.
 *
 * Parameters:
 *   width       - framebuffer width in pixels
 *   height      - framebuffer height in pixels
 *   depth       - VIDEO_DEPTH_* constant (selects texture format + shader)
 *   row_bytes   - actual data row stride in bytes (VIDEO_MODE_ROW_BYTES)
 *   pitch       - allocation row stride in bytes (>= row_bytes, page-aligned)
 *   buffer      - pointer to the_buffer (page-aligned framebuffer memory)
 *   buffer_size - total allocation size (page-aligned, >= height * pitch)
 *
 * Returns 0 on success, -1 on failure (with diagnostic log messages).
 */
int MetalCompositorInit(int width, int height, int depth, int row_bytes,
                        int pitch, void *buffer, uint32_t buffer_size);

/*
 * Update the 256-entry palette for indexed color depths (1/2/4/8-bit).
 *
 * Expands 3-byte-per-entry RGB input to 4-byte RGBA (A=255) and copies
 * to the shared palette MTLBuffer. No-op with a warning if called when
 * the compositor is not in an indexed color mode.
 *
 * Parameters:
 *   pal        - pointer to palette data (3 bytes per color: R, G, B)
 *   num_colors - number of palette entries to update (max 256)
 */
void MetalCompositorUpdatePalette(const uint8_t *pal, int num_colors);

/*
 * Render one frame of the 2D framebuffer to the CAMetalLayer.
 *
 * Gets the next drawable, encodes a fullscreen triangle draw sampling
 * the shared framebuffer texture, and presents+commits immediately.
 * Safe to call when not initialized (returns silently).
 */
void MetalCompositorPresent(void);

/*
 * Tear down all Metal compositor resources.
 *
 * Removes the CompositorMetalView from the UIWindow and releases
 * all Metal objects. Safe to call when not initialized.
 */
void MetalCompositorShutdown(void);

/*
 * Resize the Metal compositor for a resolution/depth mode switch.
 *
 * Rebuilds only the depth-dependent resources — buffer, texture, pipeline,
 * palette, and sampler — while keeping the view, layer, device, and queue
 * alive. This avoids the visual flash and UIView lifecycle overhead from a
 * full Shutdown→Init cycle. Per-engine overlay textures are managed by
 * gfxaccel_resources and are re-vended by each engine in its own
 * on_mode_enter / on_mode_exit handlers.
 *
 * Threading: called from the PPC emulation thread with interrupts disabled —
 * no MetalCompositorPresent() calls can fire during resize. UIView operations
 * must NOT happen here (which is why the view stays alive).
 *
 * Precondition: compositor must already be initialized (via MetalCompositorInit).
 * If not, returns -1 and logs an error — caller should use Init instead.
 *
 * Parameters: same as MetalCompositorInit.
 *
 * Returns 0 on success, -1 on failure (with diagnostic log messages).
 */
int MetalCompositorResize(int width, int height, int depth, int row_bytes,
                          int pitch, void *buffer, uint32_t buffer_size);

/*
 * Query whether the compositor has been successfully initialized.
 *
 * Returns 1 if MetalCompositorInit completed successfully and
 * MetalCompositorShutdown has not been called since. Returns 0 otherwise.
 *
 * Used by video_sdl2.cpp to decide between Init (first time) and Resize
 * (subsequent mode switches).
 */
int MetalCompositorIsInitialized(void);

/*
 * MetalCompositorSubmitFrame.
 *
 * Production semantics:
 *   1. Validate the descriptor (null / layer_count bound / slot bound /
 *      source non-null).
 *   2. Reject stale descriptors whose `generation` does not match
 *      dmc_current_snapshot()->generation with kGfxAccelErrStaleGeneration;
 *      engines drop the frame and rebuild.
 *   3. Cache the last kLayerSlotOverlay layer in the descriptor, retaining
 *      its source texture handle.
 *   4. Return kGfxAccelNoErr. No semaphore is consumed, no drawable is
 *      acquired, no render pass is encoded, and no present is issued.
 *
 * kLayerSlotUnderlay and kLayerSlotFramebuffer layers are accepted but ignored
 * in production. MetalCompositorPresent composites the cached overlay over the
 * 2D framebuffer texture every VBL; this is the sole production drawable owner
 * and presenter. Because SubmitFrame keeps a texture handle rather than a
 * snapshot, a submitted overlay texture must remain unmodified by the engine
 * until that engine's next SubmitFrame call.
 *
 * Returns kGfxAccelNoErr or one of the kGfxAccelErr* codes defined in the
 * GfxAccelError enum above.
 */
int32_t MetalCompositorSubmitFrame(const struct FrameDescriptor *desc);

/*
 * MetalCompositorSync3DFramePacingForEngine.
 *
 * Block the calling thread until that engine's next 3D frame deadline.
 * Deadlines are derived from the display-link cadence and fall back to one
 * cadence interval when ticks stop. Returns kGfxAccelNoErr on success,
 * kGfxAccelErrVBLTimeout if mach_wait_until fails, or
 * kGfxAccelErrVBLNotInitialized if VBLSource is not running.
 */
int32_t MetalCompositorSync3DFramePacingForEngine(int32_t engine_id);

/*
 * Retained VBL timestamp setter.
 *
 * Called from the VBL callback for API continuity with the older
 * SubmitFrame-present path. Production SubmitFrame does not present, and the
 * stored value is currently unused.
 */
void MetalCompositorSubmitFrame_SetTargetTimestamp(double target_ts);

/*
 * rave-overlay-flicker-black fix (2026-04-16): overlay cache API.
 *
 * SubmitFrame caches the most recent kLayerSlotOverlay CompositeLayer
 * (and retains its source MTLTexture) so that MetalCompositorPresent can
 * keep composing the last-known overlay between SubmitFrame calls --
 * e.g. during Nanosaur's QADrawContextDelete -> QADrawContextNew interval
 * where RAVE emits no RenderEnd for ~20 log lines.  Without the cache
 * those interval frames present 2D-framebuffer-only (black for full-3D apps)
 * producing a visible flicker to black.
 *
 * MetalCompositorSubmitFrame_AcquireCachedOverlay copies the cached
 * CompositeLayer into *out_layer and returns a retained MTLTexture
 * reference in *out_tex_retained; the caller MUST release via
 * MetalCompositorSubmitFrame_ReleaseCachedOverlay after the frame is
 * committed.  Returns 1 if a cached overlay is available, 0 otherwise.
 *
 * MetalCompositorSubmitFrame_EncodeCachedOverlay encodes the supplied
 * CompositeLayer into an existing MTLRenderCommandEncoder using the
 * SubmitFrame module's display-gamma premultiplied overlay PSO when possible
 * -- used by Present after drawing the 2D framebuffer underlay.
 *
 * MetalCompositorSubmitFrame_ClearCachedOverlay invalidates the cache
 * (called on mode-exit / compositor shutdown).
 */
int  MetalCompositorSubmitFrame_AcquireCachedOverlay(struct CompositeLayer *out_layer,
                                                     void **out_tex_retained);
void MetalCompositorSubmitFrame_ReleaseCachedOverlay(void *tex_retained);
void MetalCompositorSubmitFrame_EncodeCachedOverlay(void *render_encoder,
                                                    const struct CompositeLayer *layer,
                                                    void *display_gamma_lut);
void MetalCompositorSubmitFrame_ClearCachedOverlay(void);


/*
 * MetalCompositorGetLayer.
 *
 * Returns the compositor's CAMetalLayer as void* (bridge-cast in the
 * caller). Used by gfxaccel_handle_background_enter to set drawableSize
 * to CGSizeZero and by gfxaccel_handle_foreground_enter to restore it.
 * Returns NULL if the compositor is not initialized.
 */
void *MetalCompositorGetLayer(void);

/*
 * MetalCompositorGetFramebufferTexture.
 *
 * Returns the compositor-owned 2D framebuffer texture as void*
 * (id<MTLTexture>; caller bridge-casts). This is the MTLTexture view over
 * the_buffer that MetalCompositorPresent samples as fragment input — DSp
 * SwapBuffers blits its private back_texture into this texture directly; its
 * kLayerSlotFramebuffer SubmitFrame descriptor is accepted but ignored in
 * production.
 * Returns NULL if the compositor has not been
 * successfully initialized. The caller must NOT retain/release the
 * returned handle — it is owned by the compositor for the lifetime of
 * the current video mode.
 *
 * The compositor-blindness invariant is unchanged: this accessor
 * does not reference DSp — it is the same framebuffer texture handle
 * the compositor already uses internally, exposed for zero-copy blit
 * encoding from outside the .mm. NQD already writes into the backing
 * MTLBuffer via the shared-memory path; DSp instead writes into the
 * texture directly via MTLBlitCommandEncoder.copyFromTexture: so the
 * path is symmetric.
 */
void *MetalCompositorGetFramebufferTexture(void);

/*
 * MetalCompositorPaletteLatch.
 *
 * VBL-latched dirty swap: atomically promotes the back palette buffer to
 * front only when MetalCompositorUpdatePalette has published new data. Must
 * be called from the VBL callback BEFORE any command encoding for the frame
 * begins. The compositor reads s_palette_front_idx once at encode start and
 * binds that buffer.
 */
void MetalCompositorPaletteLatch(void);

/*
 * MetalCompositorUpdateGammaLUT.
 *
 * Upload 768 bytes of Mac-side gamma LUT data (planar: 256 R + 256 G + 256 B)
 * into the compositor's display-ready gamma_lut_buffer MTLBuffer, composing
 * the shared non-fade display policy. No-op if gamma_lut_buffer is nil or
 * lut is NULL.
 */
void MetalCompositorUpdateGammaLUT(const uint8_t *lut);

/*
 * MetalCompositorGetGammaLUTBuffer.
 *
 * Return the compositor's gamma_lut_buffer as void* (id<MTLBuffer>), or NULL
 * if uninitialized. Production accessor used by the
 * DSp 16bpp unpack render pass (dsp_metal_renderer.mm) to bind the same
 * display-ready gamma LUT the compositor present path samples.
 * The unpack path is the rare non-visible twin (DSp force-resize to
 * 32bpp routes visible pixels through compositor_fragment_32bpp).
 */
void *MetalCompositorGetGammaLUTBuffer(void);

/*
 * MetalCompositorGetGammaIdentityBuffer.
 *
 * Return the compositor's permanently-allocated identity-LUT fallback buffer
 * as void* (id<MTLBuffer>), or NULL if the compositor is uninitialized. The
 * DSp 16bpp unpack twin binds this when MetalCompositorGetGammaLUTBuffer() is
 * nil so its gamma-sampling shader never reads an unbound buffer index (UB).
 */
void *MetalCompositorGetGammaIdentityBuffer(void);

/*
 * Present-rect cache for the absolute-cursor letterbox map.
 *
 * The cursor map in video_sdl2.cpp must map the finger/pointer into the same
 * rectangle the compositor draws into — compositor_view's bounds, which on Mac
 * Catalyst are pinned to the superview's safe area (NOT the full window) and
 * shift when the menu bar shows/hides on a refocus. Reading uiWindow.bounds or
 * SDL_GetWindowSize instead makes the cursor and image disagree after a
 * refocus, so the cursor can't reach the full emulated desktop.
 *
 * MetalCompositorRefreshPresentRect reads the view rect (in window coordinates,
 * the space SDL mouse events use — origin folds in the safe-area inset) and
 * caches it; it is a no-op unless called on the main thread (pumped from
 * on_sdl_event_generated). MetalCompositorGetPresentRect returns the last cached
 * rect from any thread (size 0 before the first refresh — caller should fall
 * back to an identity map).
 */
void MetalCompositorRefreshPresentRect(void);
void MetalCompositorGetPresentRect(int *out_x, int *out_y, int *out_w, int *out_h);


#ifdef __cplusplus
}
#endif

#endif /* METAL_COMPOSITOR_H */
