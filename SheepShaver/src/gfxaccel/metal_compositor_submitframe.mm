/*
 *  metal_compositor_submitframe.mm - SubmitFrame implementation.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Engine-blind: the compositor sees only CompositeLayer PODs with
 *  opaque source-texture handles. In production SubmitFrame is a
 *  validate-and-cache shim: it rejects malformed/stale descriptors, stores
 *  the last kLayerSlotOverlay layer in a single mailbox, and returns.
 *  MetalCompositorPresent is the sole production drawable owner/presenter.
 *  Underlay/framebuffer layers are accepted but ignored in production.
 *
 *  Split from metal_compositor.mm to keep the SubmitFrame state machine
 *  self-contained. metal_compositor.mm calls
 *  MetalCompositorSubmitFrame_BindPresentationContext from its Init
 *  to hand over the device + queue + CAMetalLayer; this module keeps
 *  its own static state and provides:
 *
 *    - MetalCompositorSubmitFrame(const FrameDescriptor *)     (public)
 *    - MetalCompositorSubmitFrame_BindPresentationContext      (internal)
 *    - MetalCompositorSubmitFrame_UnbindPresentationContext    (internal)
 *
 *  Overlay caching (rave-overlay-flicker-black, 2026-04-16):
 *  -------------------------------------------------------
 *  RAVE and GL engines submit overlay frames via SubmitFrame only when
 *  they actually render (e.g. on RAVE's QARenderEnd).  Nanosaur and
 *  similar full-3D apps destroy and recreate their QADrawContext during
 *  scene transitions, which produces a short interval with no
 *  SubmitFrame calls.  During that interval MetalCompositorPresent (called
 *  every emulated VBL from VideoVBL) presents the 2D framebuffer only;
 *  for a full-3D app the 2D framebuffer is black, so the user sees a
 *  visible black flash.
 *
 *  Fix (Iteration #2): every successful SubmitFrame populates a
 *  last-good overlay cache (source texture + layer POD).
 *  MetalCompositorPresent consults the cache via
 *  MetalCompositorSubmitFrame_AcquireCachedOverlay and composites the
 *  last-known overlay atop the 2D framebuffer in its own render pass.
 *
 *  Fix (Iteration #3, 2026-04-16 — 2D/3D composition regression):
 *  Iteration #2 introduced a second presenter: SubmitFrame was still
 *  acquiring its own drawable via [layer nextDrawable] and calling
 *  presentDrawable:/presentAtTime: alongside the new Present-path
 *  cached-overlay composite.  The two presenters raced for the
 *  CAMetalLayer drawable pool; SubmitFrame (one present per RenderEnd
 *  at ~60 Hz) consistently won the race during active 3D rendering,
 *  so the user saw the overlay-only pass (no 2D framebuffer) and the
 *  Nanosaur menu bar / HUD disappeared.  Even with the race fixed,
 *  submitframe_encode_layer was issuing a fullscreen triangle that
 *  ignored dst_origin / dst_size, so a 640x469 overlay rect (Nanosaur's
 *  11-px menu strip) still covered the full 640x480 drawable.
 *
 *  Iteration #3 makes Present the sole drawable owner in production:
 *    - SubmitFrame ONLY caches the overlay and returns -- no drawable
 *      acquisition, no render pass, no present.  MetalCompositorPresent
 *      is the single owner of nextDrawable / presentDrawable,
 *      eliminating the race.
 *    - submitframe_encode_layer now sets a per-layer MTLViewport to
 *      (dst_origin, dst_size) so the fullscreen triangle is clipped
 *      to the layer's dst rect; 2D underlay shows through outside the
 *      overlay rect.  Existing tests all use dst_size == render target
 *      size so the viewport is the default (no behavior change).
 *
 *  The cache retains the source MTLTexture by strong reference so it
 *  remains valid even if the submitting engine releases its own handle. It
 *  is not a pixel snapshot; the submitting engine must not redraw that
 *  texture until it submits a different overlay texture.
 *
 *  Concurrency: production SubmitFrame does no GPU work and does not consume
 *  s_inflight_semaphore.
 */

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <stdint.h>
#include <stdatomic.h>
#include <os/lock.h>

#include "metal_compositor.h"
#include "display_mode_controller.h"
#include "gfxaccel_resources_heap.h"
#include "vbl_source.h"

// ---------------------------------------------------------------------------
// Logging (stand-alone; does not share metal_compositor.mm's macros so this
// module compiles without that file's includes).
// ---------------------------------------------------------------------------

#define COMPSF_LOG(fmt, ...) \
    do { printf("[MetalCompositor/SubmitFrame] " fmt "\n", ##__VA_ARGS__); } while (0)

#define COMPSF_ERR(fmt, ...) \
    do { printf("[MetalCompositor/SubmitFrame ERROR] " fmt "\n", ##__VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------
// Module-local state
// ---------------------------------------------------------------------------

static id<MTLDevice>                  s_device             = nil;
static id<MTLCommandQueue>            s_queue              = nil;
static id<MTLLibrary>                 s_library            = nil;
static CAMetalLayer                  *s_layer              = nil;    // nil in headless/testing mode
static dispatch_semaphore_t           s_inflight_semaphore = NULL;
static id<MTLRenderPipelineState>     s_pipe_for_blend[3]  = { nil, nil, nil };
static id<MTLRenderPipelineState>     s_pipe_display_premultiplied = nil;

/*
 * Framebuffer-texture publication.
 *
 * metal_compositor.mm calls MetalCompositorSubmitFrame_SetFramebufferTexture
 * from Init / Resize with its compositor_texture handle, and again from
 * Shutdown with NULL. DSpContext_SwapBuffersHandler retrieves the handle via
 * MetalCompositorGetFramebufferTexture (also defined below) and blits its
 * private back_texture into it before submitting a kLayerSlotFramebuffer
 * CompositeLayer. The setter / getter live in this module (rather than in
 * metal_compositor.mm) so the test target — which does not compile
 * metal_compositor.mm due to SDL2 deps — can still resolve both symbols.
 *
 * Threading: written from emul thread during MetalCompositorInit / Resize /
 * Shutdown (same thread that calls SwapBuffers); read from emul thread
 * during SwapBuffers. No cross-thread access, no lock needed. A single
 * plain id<MTLTexture> retains correctly under ARC.
 */
static id<MTLTexture>                 s_framebuffer_texture = nil;

/*
 * Target presentation timestamp set by the VBL callback via
 * MetalCompositorSubmitFrame_SetTargetTimestamp().
 *
 * C11 _Atomic double -- written from VBL callback (main thread), read
 * from emul thread.  Same minimal-primitive rationale as vbl_source.mm's
 * _Atomic uint64_t counters.
 *
 * Iteration #3 (rave-overlay-flicker-black): in production SubmitFrame
 * no longer calls presentAtTime: (Present owns presentation), so this
 * field is effectively unused.  Retained for the
 * MetalCompositorSubmitFrame_SetTargetTimestamp API shape which external
 * callers (vbl_source) still invoke.
 */
static _Atomic double s_target_presentation_ts = 0;

static const uint32_t kMaxLayers = 16;

// ---------------------------------------------------------------------------
// Overlay cache (rave-overlay-flicker-black fix).
//
// Single kLayerSlotOverlay layer kept around between SubmitFrame calls so
// that MetalCompositorPresent can keep composing the last-known overlay
// while the submitting engine is between frames (QADrawContextDelete ->
// QADrawContextNew interval, etc.).  The lock is os_unfair_lock (same primitive
// family as the project's minimal-primitive concurrency pattern; writes
// happen on the emul thread from SubmitFrame and the clear helpers, reads
// happen on the VBL / emul thread from Present).
//
// The cache retains the source MTLTexture so that if the submitting
// engine releases its own reference the texture survives until the cache
// is cleared or replaced.
// ---------------------------------------------------------------------------

static os_unfair_lock           s_overlay_cache_lock  = OS_UNFAIR_LOCK_INIT;
static bool                     s_overlay_cache_valid = false;
static struct CompositeLayer    s_overlay_cache_layer;
static id<MTLTexture>           s_overlay_cache_tex   = nil;


// ---------------------------------------------------------------------------
// submitframe_build_pipelines - lazy one-time PSO cache
// ---------------------------------------------------------------------------
//
// Builds three MTLRenderPipelineState objects sharing submitframe_vertex +
// submitframe_fragment but differing in their color-attachment blend state.
// BGRA8Unorm color attachments match the drawable pixel format.
//
//   kBlendOpaque        - blending disabled; source color replaces dest
//   kBlendPremultiplied - src=One,      dst=OneMinusSrcAlpha
//   kBlendStraight      - src=SrcAlpha, dst=OneMinusSrcAlpha

static id<MTLLibrary> submitframe_resolve_library(id<MTLDevice> dev);

static int submitframe_build_pipelines(void)
{
    if (!s_device) {
        COMPSF_ERR("submitframe_build_pipelines: device nil");
        return -1;
    }
    if (!s_library) {
        s_library = submitframe_resolve_library(s_device);
    }
    id<MTLLibrary> lib = s_library;
    if (!lib) {
        COMPSF_ERR("submitframe_build_pipelines: could not resolve Metal library");
        return -1;
    }

    id<MTLFunction> vfunc = [lib newFunctionWithName:@"submitframe_vertex"];
    id<MTLFunction> ffunc = [lib newFunctionWithName:@"submitframe_fragment"];
    id<MTLFunction> display_ffunc =
        [lib newFunctionWithName:@"submitframe_fragment_display_premultiplied"];
    if (!vfunc || !ffunc || !display_ffunc) {
        COMPSF_ERR("submitframe_build_pipelines: shader function lookup "
                   "(vertex=%p fragment=%p display=%p)",
                   vfunc, ffunc, display_ffunc);
        return -1;
    }

    const CompositeBlendMode modes[3] = { kBlendOpaque, kBlendPremultiplied, kBlendStraight };
    for (int i = 0; i < 3; ++i) {
        CompositeBlendMode mode = modes[i];
        MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction   = vfunc;
        pd.fragmentFunction = ffunc;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

        switch (mode) {
            case kBlendOpaque:
                pd.colorAttachments[0].blendingEnabled = NO;
                break;
            case kBlendPremultiplied:
                pd.colorAttachments[0].blendingEnabled             = YES;
                pd.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorOne;
                pd.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
                pd.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
                pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                pd.colorAttachments[0].rgbBlendOperation           = MTLBlendOperationAdd;
                pd.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationAdd;
                break;
            case kBlendStraight:
                pd.colorAttachments[0].blendingEnabled             = YES;
                pd.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
                pd.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
                pd.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
                pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                pd.colorAttachments[0].rgbBlendOperation           = MTLBlendOperationAdd;
                pd.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationAdd;
                break;
        }

        NSError *err = nil;
        id<MTLRenderPipelineState> pso =
            [s_device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!pso) {
            COMPSF_ERR("submitframe_build_pipelines: blend=%d PSO failed: %s",
                       (int)mode, [[err localizedDescription] UTF8String]);
            return -1;
        }
        s_pipe_for_blend[(int)mode] = pso;
    }

    {
        MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction   = vfunc;
        pd.fragmentFunction = display_ffunc;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pd.colorAttachments[0].blendingEnabled             = YES;
        pd.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorOne;
        pd.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
        pd.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
        pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        pd.colorAttachments[0].rgbBlendOperation           = MTLBlendOperationAdd;
        pd.colorAttachments[0].alphaBlendOperation         = MTLBlendOperationAdd;

        NSError *err = nil;
        s_pipe_display_premultiplied =
            [s_device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!s_pipe_display_premultiplied) {
            COMPSF_ERR("submitframe_build_pipelines: display premult PSO failed: %s",
                       [[err localizedDescription] UTF8String]);
            return -1;
        }
    }

    COMPSF_LOG("submitframe_build_pipelines: built blend-mode + display PSOs");
    return 0;
}

static id<MTLRenderPipelineState> submitframe_pipeline_for_blend(CompositeBlendMode b)
{
    if ((int)b < 0 || (int)b >= 3) return nil;
    return s_pipe_for_blend[(int)b];
}

// ---------------------------------------------------------------------------
// submitframe_encode_layer - single-layer encode (engine-blind)
// ---------------------------------------------------------------------------
//
// Sets the layer's source texture, selects the appropriate blend-mode PSO,
// uploads a single-struct LayerUniform via setFragmentBytes (small-buffer
// path - no MTLBuffer allocation per frame), sets an MTLViewport to the
// layer's dst_origin / dst_size so the fullscreen-triangle output is
// clipped to the layer's destination rect, and issues the drawPrimitives.
//
// Viewport clipping (Iteration #3, rave-overlay-flicker-black): previously
// the fullscreen triangle covered the entire render target regardless of
// dst rect, so e.g. a 640x469 Nanosaur overlay on a 640x480 drawable
// overwrote the 11-pixel 2D menu strip.  Setting viewport = dst rect
// confines rasterization to that rect; pixels outside the rect are not
// touched (2D underlay shows through).  All existing tests use
// dst_size == render target size so the viewport == default full-target
// viewport and there is no test behavior change.

static void submitframe_encode_layer(id<MTLRenderCommandEncoder> enc,
                                     const struct CompositeLayer *layer)
{
    if (!enc || !layer) return;

    id<MTLRenderPipelineState> pso = submitframe_pipeline_for_blend(layer->blend);
    if (!pso) {
        COMPSF_ERR("submitframe_encode_layer: no PSO for blend=%d - skipping layer",
                   (int)layer->blend);
        return;
    }
    [enc setRenderPipelineState:pso];

    /* Per-rect viewport clipping (Iteration #3).  dst_origin / dst_size are
     * in pixel coords (same space Metal viewports expect).  MTLViewport
     * takes doubles: origin_x, origin_y, width, height, znear, zfar.  Zero-
     * sized rects are silently skipped by Metal; guard explicitly to avoid
     * a validation warning under Metal debug. */
    if (layer->dst_size_w > 0.0f && layer->dst_size_h > 0.0f) {
        MTLViewport vp;
        vp.originX = (double)layer->dst_origin_x;
        vp.originY = (double)layer->dst_origin_y;
        vp.width   = (double)layer->dst_size_w;
        vp.height  = (double)layer->dst_size_h;
        vp.znear   = 0.0;
        vp.zfar    = 1.0;
        [enc setViewport:vp];
    }

    id<MTLTexture> src = (__bridge id<MTLTexture>)layer->source;
    [enc setFragmentTexture:src atIndex:0];

    typedef struct {
        float src_origin[2];
        float src_size[2];
        float dst_origin[2];
        float dst_size[2];
        float alpha;
        float _pad[3];
    } LayerUniform;

    LayerUniform u;
    u.src_origin[0] = (float)layer->src_origin_x;
    u.src_origin[1] = (float)layer->src_origin_y;
    u.src_size[0]   = (float)layer->src_size_w;
    u.src_size[1]   = (float)layer->src_size_h;
    u.dst_origin[0] = layer->dst_origin_x;
    u.dst_origin[1] = layer->dst_origin_y;
    u.dst_size[0]   = layer->dst_size_w;
    u.dst_size[1]   = layer->dst_size_h;
    u.alpha         = layer->alpha;
    u._pad[0] = u._pad[1] = u._pad[2] = 0.0f;

    [enc setVertexBytes:&u length:sizeof(u) atIndex:0];
    [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];

    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

static void submitframe_encode_layer_display_gamma(id<MTLRenderCommandEncoder> enc,
                                                   const struct CompositeLayer *layer,
                                                   id<MTLBuffer> display_gamma_lut)
{
    if (!enc || !layer) return;
    if (layer->blend != kBlendPremultiplied ||
        display_gamma_lut == nil ||
        s_pipe_display_premultiplied == nil) {
        submitframe_encode_layer(enc, layer);
        return;
    }

    [enc setRenderPipelineState:s_pipe_display_premultiplied];

    if (layer->dst_size_w > 0.0f && layer->dst_size_h > 0.0f) {
        MTLViewport vp;
        vp.originX = (double)layer->dst_origin_x;
        vp.originY = (double)layer->dst_origin_y;
        vp.width   = (double)layer->dst_size_w;
        vp.height  = (double)layer->dst_size_h;
        vp.znear   = 0.0;
        vp.zfar    = 1.0;
        [enc setViewport:vp];
    }

    id<MTLTexture> src = (__bridge id<MTLTexture>)layer->source;
    [enc setFragmentTexture:src atIndex:0];

    typedef struct {
        float src_origin[2];
        float src_size[2];
        float dst_origin[2];
        float dst_size[2];
        float alpha;
        float _pad[3];
    } LayerUniform;

    LayerUniform u;
    u.src_origin[0] = (float)layer->src_origin_x;
    u.src_origin[1] = (float)layer->src_origin_y;
    u.src_size[0]   = (float)layer->src_size_w;
    u.src_size[1]   = (float)layer->src_size_h;
    u.dst_origin[0] = layer->dst_origin_x;
    u.dst_origin[1] = layer->dst_origin_y;
    u.dst_size[0]   = layer->dst_size_w;
    u.dst_size[1]   = layer->dst_size_h;
    u.alpha         = layer->alpha;
    u._pad[0] = u._pad[1] = u._pad[2] = 0.0f;

    [enc setVertexBytes:&u length:sizeof(u) atIndex:0];
    [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
    [enc setFragmentBuffer:display_gamma_lut offset:0 atIndex:1];

    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

// ---------------------------------------------------------------------------
// Overlay cache helpers (rave-overlay-flicker-black fix).
// ---------------------------------------------------------------------------
//
// Cache is updated at the END of SubmitFrame once we know the submitted
// overlay layers were encoded without error.  Present reads via
// MetalCompositorSubmitFrame_AcquireCachedOverlay and encodes the overlay
// with submitframe_encode_layer in its own render pass.
//
// Invalidation surfaces:
//   - MetalCompositorSubmitFrame_ClearCachedOverlay: called explicitly on
//     mode-exit / compositor shutdown (MTLTexture tied to a drawable size
//     must not be reused across mode changes).
//   - Unbind path drops the cache so that a fresh Bind doesn't present
//     texture from a torn-down device.

static void submitframe_cache_store(const struct CompositeLayer *layer)
{
    if (!layer) return;
    id<MTLTexture> new_tex = (__bridge id<MTLTexture>)layer->source;
    os_unfair_lock_lock(&s_overlay_cache_lock);
    s_overlay_cache_layer = *layer;
    s_overlay_cache_tex   = new_tex;       /* strong retain under ARC */
    s_overlay_cache_valid = (new_tex != nil);
    os_unfair_lock_unlock(&s_overlay_cache_lock);
}

extern "C" int MetalCompositorSubmitFrame_AcquireCachedOverlay(
    struct CompositeLayer *out_layer, void **out_tex_retained)
{
    if (!out_layer || !out_tex_retained) return 0;
    *out_tex_retained = NULL;
    id<MTLTexture> tex = nil;
    int have = 0;
    os_unfair_lock_lock(&s_overlay_cache_lock);
    if (s_overlay_cache_valid && s_overlay_cache_tex != nil) {
        *out_layer = s_overlay_cache_layer;
        tex        = s_overlay_cache_tex;  /* retained by __bridge_retained below */
        have       = 1;
    }
    os_unfair_lock_unlock(&s_overlay_cache_lock);
    if (have && tex != nil) {
        *out_tex_retained = (__bridge_retained void *)tex;
        /* Rebind the source field to the retained handle: the caller's
         * layer copy now owns +1 retain count on the texture which the
         * caller must release via MetalCompositorSubmitFrame_ReleaseCachedOverlay. */
        out_layer->source = *out_tex_retained;
    }
    return have;
}

extern "C" void MetalCompositorSubmitFrame_ReleaseCachedOverlay(void *tex_retained)
{
    if (tex_retained == NULL) return;
    /* Balance the __bridge_retained in AcquireCachedOverlay. */
    (void)(__bridge_transfer id<MTLTexture>)tex_retained;
}

extern "C" void MetalCompositorSubmitFrame_ClearCachedOverlay(void)
{
    os_unfair_lock_lock(&s_overlay_cache_lock);
    s_overlay_cache_tex   = nil;
    s_overlay_cache_valid = false;
    os_unfair_lock_unlock(&s_overlay_cache_lock);
}

// ---------------------------------------------------------------------------
// VBL-paced present + 3D pacing API
// ---------------------------------------------------------------------------

/*
 * Called from the VBL callback to preserve the older SubmitFrame-present API
 * shape. Production SubmitFrame no longer presents, so the stored value is
 * currently unused. Declared in metal_compositor.h under extern "C".
 */
extern "C"
void MetalCompositorSubmitFrame_SetTargetTimestamp(double ts)
{
	atomic_store_explicit(&s_target_presentation_ts, ts, memory_order_release);
}

/*
 * MetalCompositorSync3DFramePacingForEngine.
 *
 * Thin wrapper that routes to VBLSource's per-engine deadline pacing.
 * RAVE/GL engines include only metal_compositor.h, not vbl_source.h.
 */
extern "C"
int32_t MetalCompositorSync3DFramePacingForEngine(int32_t engine_id)
{
	return vbl_source_sync_3d_pacing_for_engine(engine_id);
}

// ---------------------------------------------------------------------------
// Present-path overlay encode helper (rave-overlay-flicker-black fix).
// ---------------------------------------------------------------------------
//
// Encodes `layer` onto an existing MTLRenderCommandEncoder using the same
// blend-mode PSO cache that SubmitFrame uses.  Callers (specifically
// MetalCompositorPresent) must have already drawn the 2D framebuffer
// underlay with their own pipeline; this routine composites the overlay
// on top with kBlendPremultiplied or whatever blend mode is in the layer.
//
// Iteration #3: the encode path now sets an MTLViewport to the layer's
// dst rect so the overlay fullscreen triangle is clipped to that rect --
// 2D pixels outside the rect (e.g. Nanosaur's 11-pixel menu strip)
// remain visible.

extern "C" void MetalCompositorSubmitFrame_EncodeCachedOverlay(
    void *render_encoder, const struct CompositeLayer *layer, void *display_gamma_lut)
{
    id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)render_encoder;
    id<MTLBuffer> gamma = (__bridge id<MTLBuffer>)display_gamma_lut;
    submitframe_encode_layer_display_gamma(enc, layer, gamma);
}

// ---------------------------------------------------------------------------
// Presentation-context bind / unbind (called from metal_compositor.mm)
// ---------------------------------------------------------------------------

/*
 * Resolve the Metal shader library that contains submitframe_vertex /
 * submitframe_fragment. In production the compositor lives in the main
 * bundle (default.metallib on disk next to the executable). To be robust
 * to the shaders living in a non-default library, we try newDefaultLibrary
 * first, then fall back to every loaded bundle's default.metallib until we
 * find the submitframe_vertex function.
 */
static id<MTLLibrary> submitframe_resolve_library(id<MTLDevice> dev)
{
    id<MTLLibrary> lib = [dev newDefaultLibrary];
    if (lib != nil) {
        id<MTLFunction> probe = [lib newFunctionWithName:@"submitframe_vertex"];
        if (probe != nil) return lib;
    }
    for (NSBundle *b in [NSBundle allBundles]) {
        NSURL *url = [b URLForResource:@"default" withExtension:@"metallib"];
        if (url == nil) continue;
        NSError *err = nil;
        id<MTLLibrary> candidate = [dev newLibraryWithURL:url error:&err];
        if (candidate == nil) continue;
        id<MTLFunction> probe = [candidate newFunctionWithName:@"submitframe_vertex"];
        if (probe != nil) return candidate;
    }
    return lib;   /* return the best-we-got (may still be nil) */
}

extern "C" int MetalCompositorSubmitFrame_BindPresentationContext(
    void *device, void *queue, void *cametal_layer)
{
    if (device == NULL || queue == NULL) {
        COMPSF_ERR("Bind: NULL device/queue");
        return -1;
    }
    s_device = (__bridge id<MTLDevice>)device;
    s_queue  = (__bridge id<MTLCommandQueue>)queue;
    s_layer  = (__bridge CAMetalLayer *)cametal_layer;  // may be nil in some test modes

    /* Resolve shader library (see submitframe_resolve_library). */
    s_library = submitframe_resolve_library(s_device);
    if (!s_library) {
        COMPSF_ERR("Bind: no Metal library with submitframe shaders found");
        s_device = nil;
        s_queue  = nil;
        s_layer  = nil;
        return -1;
    }

    /* Release any prior PSOs. */
    for (int i = 0; i < 3; ++i) s_pipe_for_blend[i] = nil;
    s_pipe_display_premultiplied = nil;

    if (submitframe_build_pipelines() != 0) {
        for (int i = 0; i < 3; ++i) s_pipe_for_blend[i] = nil;
        s_pipe_display_premultiplied = nil;
        s_device  = nil;
        s_queue   = nil;
        s_layer   = nil;
        s_library = nil;
        return -1;
    }

    if (s_inflight_semaphore == NULL) {
        s_inflight_semaphore = dispatch_semaphore_create(3);
        if (s_inflight_semaphore == NULL) {
            COMPSF_ERR("Bind: dispatch_semaphore_create(3) failed");
            for (int i = 0; i < 3; ++i) s_pipe_for_blend[i] = nil;
            s_pipe_display_premultiplied = nil;
            s_device  = nil;
            s_queue   = nil;
            s_layer   = nil;
            s_library = nil;
            return -1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Framebuffer-texture publication API.
// Companion to MetalCompositorSubmitFrame_BindPresentationContext, kept
// on a separate setter so iterative Resize rebuilds can retarget the
// published handle without re-running the full bind flow. The public
// getter is MetalCompositorGetFramebufferTexture (declared in
// metal_compositor.h); DSp calls it from SwapBuffers (Case B pre-blit).
// ---------------------------------------------------------------------------

extern "C" void MetalCompositorSubmitFrame_SetFramebufferTexture(void *texture)
{
    if (texture == NULL) {
        s_framebuffer_texture = nil;
    } else {
        s_framebuffer_texture = (__bridge id<MTLTexture>)texture;
    }
}

extern "C" void *MetalCompositorGetFramebufferTexture(void)
{
    return (__bridge void *)s_framebuffer_texture;
}

extern "C" void MetalCompositorSubmitFrame_UnbindPresentationContext(void)
{
    /* Drain the semaphore (3 waits) to ensure any in-flight GPU completion
     * handlers have fired before we release the semaphore object - then
     * signal 3 times to restore the semaphore's count to its initial value
     * (3). Releasing a dispatch_semaphore_t whose count is less than its
     * initial value triggers libdispatch abort_cause_3 under ARC dealloc.
     * Per Apple docs: "Calling dispatch_release on a semaphore with a
     * count lower than the value passed to dispatch_semaphore_create is
     * a bug." */
    if (s_inflight_semaphore != NULL) {
        dispatch_semaphore_wait(s_inflight_semaphore, DISPATCH_TIME_FOREVER);
        dispatch_semaphore_wait(s_inflight_semaphore, DISPATCH_TIME_FOREVER);
        dispatch_semaphore_wait(s_inflight_semaphore, DISPATCH_TIME_FOREVER);
        dispatch_semaphore_signal(s_inflight_semaphore);
        dispatch_semaphore_signal(s_inflight_semaphore);
        dispatch_semaphore_signal(s_inflight_semaphore);
        s_inflight_semaphore = nil;
    }
    for (int i = 0; i < 3; ++i) s_pipe_for_blend[i] = nil;
    s_pipe_display_premultiplied = nil;
    s_device  = nil;
    s_queue   = nil;
    s_layer   = nil;
    s_library = nil;
    /* Drop the framebuffer-texture publication — avoids dangling-texture
     * reads from a stray late SwapBuffers on a torn-down compositor. */
    s_framebuffer_texture = nil;
    /* Drop any cached overlay whose texture is tied to a now-torn-down
     * device / drawable size (rave-overlay-flicker-black fix). */
    os_unfair_lock_lock(&s_overlay_cache_lock);
    s_overlay_cache_tex   = nil;
    s_overlay_cache_valid = false;
    os_unfair_lock_unlock(&s_overlay_cache_lock);
}


// ---------------------------------------------------------------------------
// MetalCompositorSubmitFrame - public entry point
// ---------------------------------------------------------------------------
//
// Iteration #3 (rave-overlay-flicker-black): in production SubmitFrame
// no longer acquires a CAMetalLayer drawable or calls presentDrawable.
// It caches the overlay layer and returns -- MetalCompositorPresent is
// now the sole drawable owner, eliminating the race that hid the 2D
// underlay in multi-engine apps.

extern "C" int32_t MetalCompositorSubmitFrame(const struct FrameDescriptor *desc)
{
    // 1. Cheap validation first - no semaphore consumed.
    if (desc == NULL) {
        return kGfxAccelErrInvalidDescriptor;
    }
    if (desc->layer_count > kMaxLayers) {
        return kGfxAccelErrInvalidDescriptor;
    }
    if (desc->layer_count > 0 && desc->layers == NULL) {
        return kGfxAccelErrInvalidDescriptor;
    }
    for (uint32_t i = 0; i < desc->layer_count; ++i) {
        const struct CompositeLayer *layer = &desc->layers[i];
        if ((int)layer->slot < 0 || (int)layer->slot >= kLayerSlotCount) {
            return kGfxAccelErrInvalidSlot;
        }
        if (layer->source == NULL) {
            return kGfxAccelErrInvalidDescriptor;
        }
    }


    // 2. Stale-generation rejection (cheap atomic load; closes the UAF
    //    under subscriber-reject rollback).
    const DMCModeSnapshot *cur = dmc_current_snapshot();
    if (cur != NULL && desc->generation != cur->generation) {
        return kGfxAccelErrStaleGeneration;
    }

    int32_t eviction_err = gfxaccel_heap_wait_for_eviction(0);
    if (eviction_err != kGfxAccelResNoErr) {
        return eviction_err;
    }

    // 3. Guard against un-bound compositor.
    if (s_inflight_semaphore == NULL) {
        COMPSF_ERR("SubmitFrame: inflight semaphore NULL - bind not called");
        return kGfxAccelErrPipelineUnavailable;
    }
    if (desc->layer_count > 0 && s_pipe_for_blend[kBlendOpaque] == nil) {
        COMPSF_ERR("SubmitFrame: blend PSOs not built");
        return kGfxAccelErrPipelineUnavailable;
    }

    // 4. Populate the overlay cache first.  In production (no test render
    //    target) this is the only work SubmitFrame does -- Present owns
    //    the drawable and composites the cached overlay atop the 2D
    //    framebuffer.  We cache the LAST overlay-slot layer in the
    //    descriptor; a typical RAVE/GL frame submits a single overlay
    //    layer, so "last" == "only" in practice.  Framebuffer / underlay
    //    layers are NOT cached and are ignored in production.
    for (int32_t i = (int32_t)desc->layer_count - 1; i >= 0; --i) {
        if (desc->layers[i].slot == kLayerSlotOverlay) {
            submitframe_cache_store(&desc->layers[i]);
            break;
        }
    }

    // 5. Production: Present is the sole drawable owner.  Cache is populated,
    //    nothing more to do.  No semaphore consumed -- we do no GPU work.
    return kGfxAccelNoErr;

}

