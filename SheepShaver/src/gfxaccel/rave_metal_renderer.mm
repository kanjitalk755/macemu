/*
 *  rave_metal_renderer.mm - Metal rendering infrastructure for RAVE
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Implements:
 *    - Offscreen texture rendering through compositor overlay API
 *    - Per-context Metal resource initialization (device, queue, pipelines, depth states)
 *    - RenderStart/End/Abort/Flush/Sync methods
 *    - Uber-shader pipeline pre-building with function constants
 */

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "rave_engine.h"
#include "rave_metal_renderer.h"
#include "metal_device_shared.h"
#include "metal_compositor.h"
#include "display_mode_controller.h"
#include "rave_blend_policy.h"
#include "rave_compositor_rect.h"
#include "rave_overlay_clear_policy.h"
#include "rave_mipmap_bias_policy.h"
#include "rave_texture_alpha_policy.h"
#include "rave_depth_policy.h"
#include "gfxaccel_resources.h"
#include "gfxaccel_resources_heap.h"

// Forward declare Mac_sysalloc (from macos_util.h) to avoid UIKit header conflicts
extern uint32 Mac_sysalloc(uint32 size);

// RAVE error codes
#define kQANoErr           0
#define kQAError          -2

// Viewport uniform (matches Viewport struct in rave_shaders.metal)
struct RaveViewport {
    float width;
    float height;
    float znear;
    float zfar;
};

// Vertex uniforms (matches VertexUniforms struct in rave_shaders.metal)
struct VertexUniforms {
	float point_width;
};

// Fragment uniforms (must match FragmentUniforms in rave_shaders.metal exactly)
struct FragmentUniforms {
    int32_t  texture_op;
    int32_t  fog_mode;
    float    fog_color_a;
    float    fog_color_r;
    float    fog_color_g;
    float    fog_color_b;
    float    fog_start;
    float    fog_end;
    float    fog_density;
    float    fog_max_depth;
    int32_t  alpha_test_func;
    float    alpha_test_ref;
    uint32_t multi_texture_op;      // 0=Add, 1=Modulate, 2=BlendAlpha, 3=Fixed
    float    multi_texture_factor;  // blend factor for Fixed mode
    float    mipmap_bias;           // LOD bias for texture sampling (BIA-01)
    float    multi_texture_mipmap_bias;  // LOD bias for second texture (tag 42)
    float    env_color_r;           // kQATextureOp_Blend env color (tags 150-153)
    float    env_color_g;
    float    env_color_b;
    float    env_color_a;
};

static int32_t RaveNormalizeATIFogMode(const RaveDrawPrivate *priv)
{
	switch ((int)priv->ati_state[2].i) {
		case 0: return 0;  // kQATIFogDisable -> None
		case 1: return 3;  // kQATIFogExp -> Exponential
		case 2: return 4;  // kQATIFogExp2 -> ExponentialSquared
		case 3: return 1;  // kQATIFogAlpha -> Alpha
		case 4: return 2;  // kQATIFogLinear -> Linear
		default: return 0;
	}
}

static bool RaveLooksLikeQD3DLinearFog(const RaveDrawPrivate *priv)
{
	return priv->state[24].f > 100.0f &&     // QD3D passes yon here in Bugdom
	       priv->state[22].f >= 0.0f &&
	       priv->state[22].f < priv->state[23].f &&
	       priv->state[23].f <= 1.0f;
}

static int32_t RaveNormalizeStandardFogMode(const RaveDrawPrivate *priv)
{
	uint32_t mode = priv->state[17].i;
	if (mode > 4) return 0;

	/*
	 * Bugdom's QD3D path describes plane-based linear fog, but reaches RAVE
	 * as FogMode_ExponentialSquared with normalized start/end and camera yon
	 * in FogDensity. Literal exp2 fog makes whole meshes collapse to fog color.
	 */
	if (mode == 4 && RaveLooksLikeQD3DLinearFog(priv)) return 2;
	return (int32_t)mode;
}

static int32_t RaveEffectiveFogMode(const RaveDrawPrivate *priv)
{
	return priv->ati_fog_active ? RaveNormalizeATIFogMode(priv) : RaveNormalizeStandardFogMode(priv);
}

static bool RaveEffectiveFogEnabled(const RaveDrawPrivate *priv)
{
	return RaveEffectiveFogMode(priv) != 0;
}

static void RaveFillFogUniforms(RaveDrawPrivate *priv, FragmentUniforms *fragUniforms)
{
	if (priv->ati_fog_active) {
		fragUniforms->fog_mode = RaveNormalizeATIFogMode(priv);
		fragUniforms->fog_color_r = priv->ati_state[3].f;   // kATIFogColor_r
		fragUniforms->fog_color_g = priv->ati_state[4].f;   // kATIFogColor_g
		fragUniforms->fog_color_b = priv->ati_state[5].f;   // kATIFogColor_b
		fragUniforms->fog_color_a = priv->ati_state[6].f;   // kATIFogColor_a
		fragUniforms->fog_density = priv->ati_state[7].f;   // kATIFogDensity
		fragUniforms->fog_start = priv->ati_state[8].f;     // kATIFogStart
		fragUniforms->fog_end = priv->ati_state[9].f;       // kATIFogEnd
	} else {
		fragUniforms->fog_mode = RaveNormalizeStandardFogMode(priv);
		fragUniforms->fog_color_a = priv->state[18].f;
		fragUniforms->fog_color_r = priv->state[19].f;
		fragUniforms->fog_color_g = priv->state[20].f;
		fragUniforms->fog_color_b = priv->state[21].f;
		fragUniforms->fog_start = priv->state[22].f;
		fragUniforms->fog_end = priv->state[23].f;
		fragUniforms->fog_density = priv->state[24].f;
	}
	fragUniforms->fog_max_depth = priv->state[25].f != 0.0f ? priv->state[25].f : 1.0f;
}

// kQAContext flag bits
#define kQAContext_NoZBuffer RAVE_CONTEXT_NO_Z_BUFFER

// RAVE.h constants used by draw-buffer CPU access.
#define kQADeviceMemory       0
#define kQAPixel_RGB32        3

static int32_t RestartRenderPassWithLoad(RaveDrawPrivate *priv);
static const uint64_t kRavePipelineKeyInvalid = ~0ULL;

static uint32_t RaveNormalizeChannelMask(uint32_t channelMask)
{
	return channelMask == 0 ? 0xF : (channelMask & 0xF);
}

static uint64_t RavePipelineBindingKey(int pipeIdx, uint32_t channelMask,
                                       bool usesGLPipeline,
                                       uint32_t glBlendSrc, uint32_t glBlendDst,
                                       bool msaa, bool hasDepthAttachment)
{
	uint64_t key = (uint64_t)(pipeIdx & 0x3F);
	key |= ((uint64_t)(channelMask & 0xF) << 6);
	key |= ((uint64_t)(usesGLPipeline ? 1 : 0) << 10);
	key |= ((uint64_t)(msaa ? 1 : 0) << 11);
	key |= ((uint64_t)(hasDepthAttachment ? 1 : 0) << 12);
	if (usesGLPipeline) {
		key |= ((uint64_t)(glBlendSrc & 0xFFFF) << 16);
		key |= ((uint64_t)(glBlendDst & 0xFFFF) << 32);
	}
	return key;
}

struct RaveRTTResourceToken {
	uint32_t handle;
	uint32_t generation;
};

/*
 *  RaveMetalState - Opaque Metal resource container
 *
 *  Holds all ObjC Metal objects. Allocated by RaveInitMetalResources,
 *  freed by RaveReleaseMetalResources. Referenced from RaveDrawPrivate
 *  via an opaque pointer (RaveDrawPrivate.metal).
 */
struct RaveMetalState {
	id<MTLDevice>               device;
	id<MTLCommandQueue>         commandQueue;
	id<MTLRenderPipelineState>  pipelines[48];     // 16 function constant combos x 3 blend modes
	id<MTLDepthStencilState>    depthStates[9];     // 9 ZFunction values (depth write enabled)
	id<MTLDepthStencilState>    depthStatesNoWrite[9]; // 9 ZFunction values (depth write disabled)
	uint64_t                    currentPipelineKey; // full signature of currently bound pipeline
	id<MTLTexture>              depthBuffer;

	id<MTLCommandBuffer>        currentCommandBuffer;
	id<MTLRenderCommandEncoder> currentEncoder;
	id<MTLTexture>              overlayTexture;        // offscreen render target from compositor
	id<MTLCommandBuffer>        lastCommittedBuffer;
	bool                        renderPassActive;

	// Saved for lazy MSAA pipeline creation
	id<MTLLibrary>              shaderLibrary;
	MTLVertexDescriptor        *vertexDescriptor;

	// MSAA 4x resources (allocated lazily on demand)
	id<MTLTexture>              msaaColorTexture;   // 4x MSAA render target (nil until needed)
	id<MTLTexture>              msaaDepthTexture;   // 4x MSAA depth (nil until needed)
	id<MTLRenderPipelineState>  msaaPipelines[48];  // MSAA variants of all 48 pipelines
	bool                        msaaActive;         // currently rendering in MSAA mode
	bool                        msaaResourcesReady; // MSAA textures allocated

	// Pre-built sampler states
	id<MTLSamplerState>         samplers[3];        // [0]=nearest, [1]=bilinear, [2]=trilinear

	// Lazily-built samplers for the GL wrap/filter tag combinations (each of
	// the 4 parameters is a binary ==1 choice, so 16 configs cover the space).
	// Indexed by (wrapU==1) | (wrapV==1)<<1 | (magFilt==1)<<2 | (minFilt==1)<<3.
	id<MTLSamplerState>         glSamplers[16];

	// Diagnostic counters (per-frame, reset at RenderStart)
	uint32_t                    drawCallCount;      // total draw method invocations this frame
	uint32_t                    triangleCount;      // triangles submitted this frame
	// Per-method counters for debugging which draw paths are active
	uint32_t                    triGouraudCount;    // DrawTriGouraud calls
	uint32_t                    triTextureCount;    // DrawTriTexture calls
	uint32_t                    vGouraudCount;      // DrawVGouraud calls
	uint32_t                    vTextureCount;      // DrawVTexture calls
	uint32_t                    meshGouraudCount;   // DrawTriMeshGouraud calls
	uint32_t                    meshTextureCount;   // DrawTriMeshTexture calls
	uint32_t                    bitmapCount;        // DrawBitmap calls
	uint32_t                    pointCount;         // DrawPoint calls
	uint32_t                    lineCount;          // DrawLine calls
	uint32_t                    frameWorldTexDraws;       // textured world (texOp==None) draws this frame
	uint32_t                    frameBlackWorldTexDraws;  // of those, count binding a mostly-black texture
	uint32_t                    frameModulateDraws;       // draws using the multiply blend (DST_COLOR/ZERO)
	uint32_t                    textureDrawCounts[RAVE_MAX_RESOURCES + 1]; // 1-based resource handle draw counts

	// Buffer access staging (AccessDrawBuffer/AccessZBuffer)
	id<MTLTexture>              stagingDrawBuffer;    // CPU-readable color staging (MTLStorageModeShared)
	id<MTLTexture>              stagingZBuffer;       // CPU-readable depth staging (MTLStorageModeShared, Float32)
	uint8_t                    *drawBufferCPU;        // Mac-addressable color buffer (Mac_sysalloc)
	uint32_t                    drawBufferCPUMac;     // Mac address of drawBufferCPU
	uint32_t                    drawBufferCPUSize;    // byte size of draw buffer CPU allocation
	uint8_t                    *zBufferCPU;           // Mac-addressable depth buffer (Mac_sysalloc)
	uint32_t                    zBufferCPUMac;        // Mac address of zBufferCPU
	uint32_t                    zBufferCPUSize;       // byte size of Z buffer CPU allocation
	bool                        drawBufferAccessed;   // currently in AccessDrawBuffer state
	bool                        zBufferAccessed;      // currently in AccessZBuffer state
	uint32_t                    noticeDeviceMac;      // TQADevice for buffer notice callbacks
	uint32_t                    noticeDirtyRectMac;   // TQARect for buffer notice callbacks

	// Depth-only clear pipeline (ClearZBuffer)
	id<MTLDepthStencilState>    depthAlwaysWriteState; // depth=always, write=yes

	// Channel mask pipeline cache (MSK-01): lazy pipeline variants with colorWriteMask
	// Key: (pipelineIndex << 4) | (channelMask & 0xF), bit 32 set for MSAA
	std::unordered_map<uint64_t, id<MTLRenderPipelineState>> maskedPipelines;

	// GL blend pipeline cache (kQABlend_OpenGL = 2)
	// Key includes func consts, sample/depth variants, channel mask, and GL blend factors.
	std::unordered_map<uint64_t, id<MTLRenderPipelineState>> glBlendPipelines;

	// RTT texture handles (TextureNewFromDrawContext / BitmapNewFromDrawContext)
	std::vector<RaveRTTResourceToken> rttTextureHandles;
};

static bool RaveSetVertexData(RaveMetalState *ms, const void *data,
                              size_t dataSize, NSUInteger bufferIndex)
{
	if (!ms || !ms->currentEncoder || !data) return false;
	if (dataSize > 4096) {
		uint32_t ringOffset = 0;
		void *ringBuf = gfxaccel_rave_ring_stage(data, (uint32_t)dataSize, &ringOffset);
		if (!ringBuf) return false;
		[ms->currentEncoder setVertexBuffer:(__bridge id<MTLBuffer>)ringBuf
		                              offset:ringOffset atIndex:bufferIndex];
	} else {
		[ms->currentEncoder setVertexBytes:data length:dataSize atIndex:bufferIndex];
	}
	return true;
}

static void CommitRaveCommandBufferWithRing(id<MTLCommandBuffer> commandBuffer)
{
	if (!commandBuffer) {
		return;
	}
	gfxaccel_rave_ring_frame_end((__bridge void *)commandBuffer);
	[commandBuffer commit];
}


/*
 *  RAVE per-engine overlay state.
 *
 *  Replaces the legacy shared-overlay refcount + deferred-destroy machinery
 *  with per-engine ownership. RAVE owns a two-texture overlay pair, vended by
 *  gfxaccel_resources_vend_overlay_texture_indexed(kGfxEngineRAVE, ...) and
 *  released via gfxaccel_resources_release_overlay_texture. Same resolution =
 *  cache-hit recycle; resolution change = re-vend. RenderEnd submits the
 *  just-rendered texture, then flips the write index so the next frame cannot
 *  redraw the texture cached by MetalCompositorSubmitFrame.
 *
 *  Deferred-destroy is gone because per-engine ownership eliminates the
 *  shared-refcount race that motivated it.
 *
 *  RenderEnd paces on RAVE's own deadline lane; SubmitFrame does not pace
 *  production.
 */
static id<MTLTexture> s_rave_overlay_pair[2] = { nil, nil };
static id<MTLTexture> s_rave_overlay_tex = nil;     /* current render target */
static uint32_t       s_rave_overlay_w   = 0;
static uint32_t       s_rave_overlay_h   = 0;
static uint32_t       s_rave_overlay_write_index = 0;
static bool           s_rave_overlay_binding_preserved = false;
static int32_t        s_rave_dst_left    = 0;
static int32_t        s_rave_dst_top     = 0;
static int32_t        s_rave_dst_width   = 0;
static int32_t        s_rave_dst_height  = 0;

static void rave_clear_overlay_binding_state(void)
{
	s_rave_overlay_w = 0;
	s_rave_overlay_h = 0;
	s_rave_overlay_binding_preserved = false;
	s_rave_dst_left = 0;
	s_rave_dst_top = 0;
	s_rave_dst_width = 0;
	s_rave_dst_height = 0;
}


/*
 *  rave_acquire_overlay_texture - vend (or cache-hit) the per-engine overlay.
 *
 *  Returns the MTLTexture handle on success or nil on failure (vend may
 *  return NULL during early startup or if SharedMetalDevice() is nil).
 */
static void rave_release_overlay_texture(void);

static id<MTLTexture> rave_acquire_overlay_texture(uint32_t width, uint32_t height)
{
	if ((s_rave_overlay_pair[0] != nil || s_rave_overlay_pair[1] != nil) &&
	    (s_rave_overlay_w != width || s_rave_overlay_h != height)) {
		rave_release_overlay_texture();
	}

	if (s_rave_overlay_pair[0] == nil || s_rave_overlay_pair[1] == nil) {
		void *raw0 = gfxaccel_resources_vend_overlay_texture_indexed(
		                kGfxEngineRAVE, 0, width, height,
		                MTLPixelFormatBGRA8Unorm);
		void *raw1 = gfxaccel_resources_vend_overlay_texture_indexed(
		                kGfxEngineRAVE, 1, width, height,
		                MTLPixelFormatBGRA8Unorm);
		if (raw0 == NULL || raw1 == NULL) {
			RAVE_LOG("rave_acquire_overlay_texture: vend pair(%ux%u) returned NULL", width, height);
			if (raw0 != NULL) {
				gfxaccel_resources_release_overlay_texture(kGfxEngineRAVE, raw0);
			}
			if (raw1 != NULL) {
				gfxaccel_resources_release_overlay_texture(kGfxEngineRAVE, raw1);
			}
			s_rave_overlay_pair[0] = nil;
			s_rave_overlay_pair[1] = nil;
			s_rave_overlay_tex = nil;
			s_rave_overlay_write_index = 0;
			rave_clear_overlay_binding_state();
			return nil;
		}
		s_rave_overlay_pair[0] = (__bridge id<MTLTexture>)raw0;
		s_rave_overlay_pair[1] = (__bridge id<MTLTexture>)raw1;
		s_rave_overlay_w = width;
		s_rave_overlay_h = height;
	}

	s_rave_overlay_tex = s_rave_overlay_pair[s_rave_overlay_write_index];
	return s_rave_overlay_tex;
}

static void rave_advance_overlay_texture_after_submit(void)
{
	if (s_rave_overlay_pair[0] == nil || s_rave_overlay_pair[1] == nil) return;
	s_rave_overlay_write_index ^= 1u;
	s_rave_overlay_tex = s_rave_overlay_pair[s_rave_overlay_write_index];
}

/*
 *  rave_release_overlay_texture - release the per-engine overlay back to
 *  the resource manager and clear RAVE's cached handle. Idempotent.
 */
static void rave_release_overlay_texture(void)
{
	if (s_rave_overlay_pair[0] != nil) {
		gfxaccel_resources_release_overlay_texture(
		    kGfxEngineRAVE, (__bridge void *)s_rave_overlay_pair[0]);
	}
	if (s_rave_overlay_pair[1] != nil) {
		gfxaccel_resources_release_overlay_texture(
		    kGfxEngineRAVE, (__bridge void *)s_rave_overlay_pair[1]);
	}
	s_rave_overlay_pair[0] = nil;
	s_rave_overlay_pair[1] = nil;
	s_rave_overlay_tex = nil;
	s_rave_overlay_write_index = 0;
}

static void RaveSetCompositorDestinationRect(
    int32_t left,
    int32_t top,
    int32_t width,
    int32_t height)
{
	const struct DMCModeSnapshot *snap = dmc_current_snapshot();
	const uint32_t mode_width = snap ? snap->width : 0;
	const uint32_t mode_height = snap ? snap->height : 0;
	const struct RaveCompositorRect dst = RaveCompositorRectFromDrawRect(
	    left, top, width, height, mode_width, mode_height);

	s_rave_dst_left   = dst.left;
	s_rave_dst_top    = dst.top;
	s_rave_dst_width  = dst.width;
	s_rave_dst_height = dst.height;
}

/*
 *  rave_has_active_overlay - true iff RAVE currently holds a vended overlay
 *  or still has a logical RAVE overlay binding whose texture can be lazily
 *  re-vended after a DMC mode-exit detach.
 *  Used by the gfxaccel_resources fan-out attach/detach handlers (in
 *  rave_engine.cpp) to decide whether to pre-vend on mode-enter / release
 *  on mode-exit.
 */
extern "C" int rave_has_active_overlay(void)
{
	return (s_rave_overlay_pair[0] != nil ||
	        s_rave_overlay_pair[1] != nil ||
	        s_rave_overlay_tex != nil ||
	        s_rave_overlay_binding_preserved) ? 1 : 0;
}

/*
 *  rave_get_overlay_dims - export current overlay dimensions for the
 *  fan-out attach handler (so it can re-vend at the previously-active
 *  resolution if RAVE was active before the mode switch). Returns 0 if
 *  RAVE has no cached or preserved dimensions.
 */
extern "C" int rave_get_overlay_dims(uint32_t *outW, uint32_t *outH)
{
	if (s_rave_overlay_pair[0] == nil &&
	    s_rave_overlay_pair[1] == nil &&
	    s_rave_overlay_tex == nil &&
	    (s_rave_overlay_w == 0 || s_rave_overlay_h == 0)) return 0;
	if (outW) *outW = s_rave_overlay_w;
	if (outH) *outH = s_rave_overlay_h;
	return 1;
}

/*
 *  rave_release_overlay_for_detach - external entry called from the
 *  gfxaccel_resources fan-out detach handler. Releases the cached Metal
 *  texture but preserves the logical RAVE overlay binding and dimensions so
 *  the mode-enter attach handler can pre-vend and veto if vending fails.
 */
extern "C" void rave_release_overlay_for_detach(void)
{
	const bool preserve_binding = rave_has_active_overlay() &&
	                              s_rave_overlay_w != 0 &&
	                              s_rave_overlay_h != 0;
	rave_release_overlay_texture();
	s_rave_overlay_binding_preserved = preserve_binding;
}

void RaveCreateMetalOverlay(int32_t left, int32_t top, int32_t width, int32_t height)
{
	id<MTLTexture> tex = rave_acquire_overlay_texture((uint32_t)width, (uint32_t)height);
	if (tex == nil) {
		RAVE_LOG("RaveCreateMetalOverlay: vend(%dx%d) FAILED", width, height);
		return;
	}
	s_rave_overlay_binding_preserved = true;
	RaveSetCompositorDestinationRect(left, top, width, height);
	/* Declare RAVE as the active owner so DMC subscribers / snapshot
	 * readers know which engine drives mode semantics. The idempotent
	 * early-return makes this a fast no-op when owner already matches. */
	(void)dmc_set_active_owner(kDMCOwnerRAVE);
	RAVE_LOG("RaveCreateMetalOverlay: vended overlay %dx%d draw=(%d,%d) dst=(%d,%d)",
	         width, height, left, top, s_rave_dst_left, s_rave_dst_top);
}


/*
 *  Metal resource initialization
 */

// ZFunction compare function mapping (RAVE -> Metal)
// kQAZFunction_None(0), LT(1), EQ(2), LE(3), GT(4), NE(5), GE(6), Always(7), Never(8)
static MTLCompareFunction ZFunctionToMTL(int zfunc)
{
	switch (zfunc) {
		case 0:  return MTLCompareFunctionAlways;   // None: always pass, no write
		case 1:  return MTLCompareFunctionLess;
		case 2:  return MTLCompareFunctionEqual;
		case 3:  return MTLCompareFunctionLessEqual;
		case 4:  return MTLCompareFunctionGreater;
		case 5:  return MTLCompareFunctionNotEqual;
		case 6:  return MTLCompareFunctionGreaterEqual;
		case 7:  return MTLCompareFunctionAlways;
		case 8:  return MTLCompareFunctionNever;
		default: return MTLCompareFunctionAlways;
	}
}

static void RaveSetDepthStencilStateForDraw(RaveDrawPrivate *priv,
                                            int blendMode,
                                            uint32_t glBlendSrc,
                                            uint32_t glBlendDst)
{
	RaveMetalState *ms = priv ? priv->metal : nullptr;
	if (!ms || !ms->currentEncoder) return;
	if (!RaveContextUsesMetalDepthAttachment(priv->flags)) {
		/* No depth attachment: leave the encoder on Metal's default
		 * depth-stencil state (always pass, no write). Passing nil to
		 * setDepthStencilState: is a Metal API validation error — it
		 * aborted DII's z-less 800x600 menu context on its first draw. */
		return;
	}
	int zfunc = (int)priv->state[0].i;
	if (zfunc < 0 || zfunc >= 9) return;

	bool depthWriteEnabled = RaveDrawDepthWriteEnabledForBlendFactors(
		priv->state[28].i,
		priv->ati_state[kRaveATIDepthWriteEnableIndex].i,
		blendMode,
		glBlendSrc,
		glBlendDst);
	__strong id<MTLDepthStencilState> *dsArray =
		depthWriteEnabled ? ms->depthStates : ms->depthStatesNoWrite;
	if (dsArray[zfunc]) {
		[ms->currentEncoder setDepthStencilState:dsArray[zfunc]];
	}
}

static void RaveSetDepthStencilStateForCurrentDraw(RaveDrawPrivate *priv)
{
	if (!priv) return;
	RaveSetDepthStencilStateForDraw(priv,
	                                (int)priv->state[9].i,
	                                priv->state[109].i,
	                                priv->state[110].i);
}


/*
 *  RaveChannelMaskToMTL - convert RAVE channel mask bits to Metal colorWriteMask
 *
 *  RAVE: bit0=R, bit1=G, bit2=B, bit3=A
 */
static MTLColorWriteMask RaveChannelMaskToMTL(uint32_t raveMask) {
	MTLColorWriteMask m = MTLColorWriteMaskNone;
	if (raveMask & 1) m |= MTLColorWriteMaskRed;
	if (raveMask & 2) m |= MTLColorWriteMaskGreen;
	if (raveMask & 4) m |= MTLColorWriteMaskBlue;
	if (raveMask & 8) m |= MTLColorWriteMaskAlpha;
	return m;
}


/*
 *  GetMaskedPipeline - get or lazily create a pipeline with channel mask applied
 *
 *  Clones the base pipeline configuration but overrides colorAttachments[0].writeMask.
 *  Results are cached in ms->maskedPipelines keyed by pipeline, mask,
 *  sample count, and whether the active pass has a depth attachment.
 */
static id<MTLRenderPipelineState> GetMaskedPipeline(RaveMetalState *ms, int pipeIdx,
                                                    uint32_t channelMask, bool msaa,
                                                    bool hasDepthAttachment) {
	uint64_t key = ((uint64_t)pipeIdx << 4) | (channelMask & 0xF);
	if (msaa) key |= (1ULL << 32);
	if (hasDepthAttachment) key |= (1ULL << 33);

	auto it = ms->maskedPipelines.find(key);
	if (it != ms->maskedPipelines.end()) return it->second;

	// Derive function constants from pipeline index
	int blend = pipeIdx / 16;
	int bits = pipeIdx % 16;
	bool tex      = (bits & 1) != 0;
	bool fog      = (bits & 2) != 0;
	bool alpha    = (bits & 4) != 0;
	bool multiTex = (bits & 8) != 0;
	bool premultiplyOutput = RaveBlendModeUsesPremultipliedOutput(blend) != 0;

	MTLFunctionConstantValues *constants = [[MTLFunctionConstantValues alloc] init];
	[constants setConstantValue:&tex      type:MTLDataTypeBool atIndex:0];
	[constants setConstantValue:&fog      type:MTLDataTypeBool atIndex:1];
	[constants setConstantValue:&alpha    type:MTLDataTypeBool atIndex:2];
	[constants setConstantValue:&multiTex type:MTLDataTypeBool atIndex:3];
	[constants setConstantValue:&premultiplyOutput type:MTLDataTypeBool atIndex:4];

	NSError *err = nil;
	id<MTLFunction> vertexFunc = [ms->shaderLibrary newFunctionWithName:@"rave_vertex"
	                                                     constantValues:constants error:&err];
	id<MTLFunction> fragmentFunc = [ms->shaderLibrary newFunctionWithName:@"rave_fragment"
	                                                       constantValues:constants error:&err];
	if (!vertexFunc || !fragmentFunc) {
		RAVE_LOG("GetMaskedPipeline: function creation failed for pipe %d mask 0x%x", pipeIdx, channelMask);
		return nil;
	}

	MTLRenderPipelineDescriptor *pipeDesc = [[MTLRenderPipelineDescriptor alloc] init];
	pipeDesc.vertexFunction = vertexFunc;
	pipeDesc.fragmentFunction = fragmentFunc;
	pipeDesc.vertexDescriptor = ms->vertexDescriptor;
	pipeDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	pipeDesc.depthAttachmentPixelFormat = hasDepthAttachment ? MTLPixelFormatDepth32Float : MTLPixelFormatInvalid;
	pipeDesc.sampleCount = msaa ? 4 : 1;

	// Blend factors (same logic as BuildPipelines)
	pipeDesc.colorAttachments[0].blendingEnabled = YES;
	pipeDesc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
	pipeDesc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
	if (blend == 0) {
		pipeDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
		pipeDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	} else {
		pipeDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
		pipeDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	}
	pipeDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
	pipeDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

	// Apply channel mask (the whole reason for this pipeline variant)
	pipeDesc.colorAttachments[0].writeMask = RaveChannelMaskToMTL(channelMask);

	id<MTLRenderPipelineState> pipeline = [ms->device newRenderPipelineStateWithDescriptor:pipeDesc error:&err];
	if (!pipeline) {
		RAVE_LOG("GetMaskedPipeline: creation failed for pipe %d mask 0x%x: %s",
		         pipeIdx, channelMask, [[err localizedDescription] UTF8String]);
		return nil;
	}

	ms->maskedPipelines[key] = pipeline;
	RAVE_LOG("GetMaskedPipeline: created pipe %d mask 0x%x msaa=%d (cache size=%zu)",
	         pipeIdx, channelMask, msaa, ms->maskedPipelines.size());
	return pipeline;
}


/*
 *  BuildPipelines - create 48 pipeline states for given sample count
 *
 *  Shared between initial resource creation (sampleCount=1) and
 *  lazy MSAA resource creation (sampleCount=4).
 *  16 function constant combos (bits 0-3) x 3 blend modes = 48 pipelines.
 */
static void BuildPipelines(id<MTLDevice> device, id<MTLLibrary> lib,
                           MTLVertexDescriptor *vertDesc,
                           id<MTLRenderPipelineState> __strong *outPipelines,
                           int sampleCount,
                           bool hasDepthAttachment)
{
	for (int blend = 0; blend < 3; blend++) {
		for (int i = 0; i < 16; i++) {
			int pipeIdx = i + blend * 16;
			bool tex      = (i & 1) != 0;
			bool fog      = (i & 2) != 0;
			bool alpha    = (i & 4) != 0;
			bool multiTex = (i & 8) != 0;
			bool premultiplyOutput = RaveBlendModeUsesPremultipliedOutput(blend) != 0;

			MTLFunctionConstantValues *constants = [[MTLFunctionConstantValues alloc] init];
			[constants setConstantValue:&tex      type:MTLDataTypeBool atIndex:0];
			[constants setConstantValue:&fog      type:MTLDataTypeBool atIndex:1];
			[constants setConstantValue:&alpha    type:MTLDataTypeBool atIndex:2];
			[constants setConstantValue:&multiTex type:MTLDataTypeBool atIndex:3];
			[constants setConstantValue:&premultiplyOutput type:MTLDataTypeBool atIndex:4];

			NSError *err = nil;
			id<MTLFunction> vertexFunc = [lib newFunctionWithName:@"rave_vertex"
			                                       constantValues:constants error:&err];
			if (!vertexFunc) {
				RAVE_LOG("Pipeline %d (samples=%d): vertex function error: %s",
				         pipeIdx, sampleCount, [[err localizedDescription] UTF8String]);
				continue;
			}

			id<MTLFunction> fragmentFunc = [lib newFunctionWithName:@"rave_fragment"
			                                         constantValues:constants error:&err];
			if (!fragmentFunc) {
				RAVE_LOG("Pipeline %d (samples=%d): fragment function error: %s",
				         pipeIdx, sampleCount, [[err localizedDescription] UTF8String]);
				continue;
			}

			MTLRenderPipelineDescriptor *pipeDesc = [[MTLRenderPipelineDescriptor alloc] init];
			pipeDesc.vertexFunction = vertexFunc;
			pipeDesc.fragmentFunction = fragmentFunc;
			pipeDesc.vertexDescriptor = vertDesc;
			pipeDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
			pipeDesc.depthAttachmentPixelFormat = hasDepthAttachment ? MTLPixelFormatDepth32Float : MTLPixelFormatInvalid;
			pipeDesc.sampleCount = sampleCount;

			// Blend factors per mode
			pipeDesc.colorAttachments[0].blendingEnabled = YES;
			pipeDesc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
			pipeDesc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;

			if (blend == 0) {
				pipeDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
				pipeDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
			} else {
				pipeDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
				pipeDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
			}
			pipeDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
			pipeDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

			outPipelines[pipeIdx] = [device newRenderPipelineStateWithDescriptor:pipeDesc error:&err];
			if (!outPipelines[pipeIdx]) {
				RAVE_LOG("Pipeline %d (samples=%d): creation error: %s",
				         pipeIdx, sampleCount, [[err localizedDescription] UTF8String]);
			}
		}
	}
}


/*
 *  EnsureMSAAResources - lazy allocation of 4x MSAA textures and pipelines
 *
 *  Called on first frame that requests MSAA. Creates multisample color and
 *  depth textures plus 48 MSAA pipeline variants.
 */
static void EnsureMSAAResources(RaveDrawPrivate *priv)
{
	RaveMetalState *ms = priv->metal;
	if (ms->msaaResourcesReady) return;
	const bool hasDepthAttachment = RaveContextUsesMetalDepthAttachment(priv->flags) != 0;

	// Create 4x MSAA color texture
	MTLTextureDescriptor *colorDesc = [MTLTextureDescriptor
	    texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	                                width:(NSUInteger)priv->width
	                               height:(NSUInteger)priv->height
	                            mipmapped:NO];
	colorDesc.textureType = MTLTextureType2DMultisample;
	colorDesc.sampleCount = 4;
	colorDesc.storageMode = MTLStorageModePrivate;
	colorDesc.usage = MTLTextureUsageRenderTarget;
	ms->msaaColorTexture = [ms->device newTextureWithDescriptor:colorDesc];

	// Create 4x MSAA depth texture only for contexts that actually expose a z-buffer.
	if (hasDepthAttachment) {
		MTLTextureDescriptor *depthDesc = [MTLTextureDescriptor
		    texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
		                                width:(NSUInteger)priv->width
		                               height:(NSUInteger)priv->height
		                            mipmapped:NO];
		depthDesc.textureType = MTLTextureType2DMultisample;
		depthDesc.sampleCount = 4;
		depthDesc.storageMode = MTLStorageModePrivate;
		depthDesc.usage = MTLTextureUsageRenderTarget;
		ms->msaaDepthTexture = [ms->device newTextureWithDescriptor:depthDesc];
	}

	// Build 48 MSAA pipeline variants
	BuildPipelines(ms->device, ms->shaderLibrary, ms->vertexDescriptor,
	               ms->msaaPipelines, 4, hasDepthAttachment);

	ms->msaaResourcesReady = true;
	RAVE_LOG("MSAA 4x resources allocated: %dx%d (~%.1f MB)",
	         priv->width, priv->height,
	         (priv->width * priv->height * 4 * 4 * 2) / (1024.0 * 1024.0));
}


void RaveInitMetalResources(RaveDrawPrivate *priv)
{
	RaveMetalState *ms = new RaveMetalState();
	priv->metal = ms;

	// Get device from shared Metal device (compositor owns the device lifecycle).
	ms->device = (__bridge id<MTLDevice>)SharedMetalDevice();
	if (!ms->device) {
		RAVE_LOG("RaveInitMetalResources: SharedMetalDevice failed");
		return;
	}

	// Initialize RAVE ring buffer for per-draw vertex staging.
	// Replaces 15+ newBufferWithBytes: calls per frame with ring sub-allocation.
	gfxaccel_rave_ring_init();

	// Cache RAVE's per-engine overlay texture (vended via gfxaccel_resources;
	// replaces the legacy compositor-allocated overlay path).
	// May be nil here at first init - NativeRenderStart re-fetches per frame.
	ms->overlayTexture = s_rave_overlay_tex;

	ms->commandQueue = [ms->device newCommandQueue];

	// Load pre-compiled shader library (.metallib)
	id<MTLLibrary> lib = nil;
	lib = [ms->device newDefaultLibrary];
	if (!lib) {
		RAVE_LOG("RaveInitMetalResources: newDefaultLibrary failed (no .metallib?)");
		return;
	}
	ms->shaderLibrary = lib;
	const bool hasDepthAttachment = RaveContextUsesMetalDepthAttachment(priv->flags) != 0;

	// Create vertex descriptor: position, base color, texcoord, diffuse, specular.
	MTLVertexDescriptor *vertDesc = [[MTLVertexDescriptor alloc] init];
	vertDesc.attributes[0].format = MTLVertexFormatFloat4;
	vertDesc.attributes[0].offset = 0;
	vertDesc.attributes[0].bufferIndex = 0;
	vertDesc.attributes[1].format = MTLVertexFormatFloat4;
	vertDesc.attributes[1].offset = 16;
	vertDesc.attributes[1].bufferIndex = 0;
	vertDesc.attributes[2].format = MTLVertexFormatFloat4;
	vertDesc.attributes[2].offset = 32;
	vertDesc.attributes[2].bufferIndex = 0;
	vertDesc.attributes[3].format = MTLVertexFormatFloat4;
	vertDesc.attributes[3].offset = 48;
	vertDesc.attributes[3].bufferIndex = 0;
	vertDesc.attributes[4].format = MTLVertexFormatFloat4;
	vertDesc.attributes[4].offset = 64;
	vertDesc.attributes[4].bufferIndex = 0;
	vertDesc.layouts[0].stride = RAVE_VERTEX_BYTES;
	vertDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
	ms->vertexDescriptor = vertDesc;

	// RAVE blend state configuration.
	// 48 pipelines = 3 blend modes x 16 function constant combos.
	// Blend mode 0: premultiplied alpha (shader premultiplies, src=One, dst=OneMinusSrcAlpha)
	// Blend mode 1: standard alpha (src=SrcAlpha, dst=OneMinusSrcAlpha)
	// Blend mode 2 (kQABlend_OpenGL): handled separately via GetGLBlendPipeline
	// All pipelines have blendingEnabled=YES, which is correct for RAVE
	// (RAVE always blends; it's a compositing rasterizer, not an opaque renderer).
	// Pre-build 48 pipeline states (sampleCount=1)
	BuildPipelines(ms->device, lib, vertDesc, ms->pipelines, 1, hasDepthAttachment);
	RAVE_LOG("48 pipeline states created");

	if (hasDepthAttachment) {
		// RAVE depth state configuration.
		// 9 ZFunction values mapped to Metal compare functions (ZFunctionToMTL).
		// kQAZFunction_None(0) maps to MTLCompareFunctionAlways with depth write disabled,
		// which is correct (no depth buffer = always pass, never write).
		// All other functions (1-8) have depth write enabled by default; a separate
		// depthStatesNoWrite[9] array handles ZBufferMask=0 (depth write disabled).
		for (int i = 0; i < 9; i++) {
			MTLDepthStencilDescriptor *dsDesc = [[MTLDepthStencilDescriptor alloc] init];
			dsDesc.depthCompareFunction = ZFunctionToMTL(i);
			dsDesc.depthWriteEnabled = (i != 0);
			ms->depthStates[i] = [ms->device newDepthStencilStateWithDescriptor:dsDesc];
		}
		for (int i = 0; i < 9; i++) {
			MTLDepthStencilDescriptor *dsDesc = [[MTLDepthStencilDescriptor alloc] init];
			dsDesc.depthCompareFunction = ZFunctionToMTL(i);
			dsDesc.depthWriteEnabled = NO;
			ms->depthStatesNoWrite[i] = [ms->device newDepthStencilStateWithDescriptor:dsDesc];
		}
		RAVE_LOG("9+9 depth stencil states created (write enabled + disabled)");
	} else {
		RAVE_LOG("Depth stencil states skipped (no z-buffer context)");
	}

	// 3 sampler states with MTLSamplerAddressModeRepeat (correct for
	// UV-wrapped 3D geometry). nearest/bilinear/trilinear filter progression matches RAVE
	// kQATextureFilter_Fast(0)/Mid(1)/Best(2). GL tags override sampler in ApplyDirtyState.
	// Create 3 sampler states (nearest, bilinear, trilinear)
	{
		MTLSamplerDescriptor *sampDesc = [[MTLSamplerDescriptor alloc] init];
		sampDesc.sAddressMode = MTLSamplerAddressModeRepeat;
		sampDesc.tAddressMode = MTLSamplerAddressModeRepeat;

		// [0] nearest (kQATextureFilter_Fast = 0)
		sampDesc.minFilter = MTLSamplerMinMagFilterNearest;
		sampDesc.magFilter = MTLSamplerMinMagFilterNearest;
		sampDesc.mipFilter = MTLSamplerMipFilterNotMipmapped;
		ms->samplers[0] = [ms->device newSamplerStateWithDescriptor:sampDesc];

		// [1] bilinear (kQATextureFilter_Mid = 1)
		sampDesc.minFilter = MTLSamplerMinMagFilterLinear;
		sampDesc.magFilter = MTLSamplerMinMagFilterLinear;
		sampDesc.mipFilter = MTLSamplerMipFilterNearest;
		ms->samplers[1] = [ms->device newSamplerStateWithDescriptor:sampDesc];

		// [2] trilinear (kQATextureFilter_Best = 2)
		sampDesc.mipFilter = MTLSamplerMipFilterLinear;
		ms->samplers[2] = [ms->device newSamplerStateWithDescriptor:sampDesc];
	}
	RAVE_LOG("3 sampler states created");

	// Create depth buffer texture only for contexts that actually expose a z-buffer.
	if (hasDepthAttachment && priv->width > 0 && priv->height > 0) {
		MTLTextureDescriptor *depthDesc = [MTLTextureDescriptor
		    texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
		                                width:(NSUInteger)priv->width
		                               height:(NSUInteger)priv->height
		                            mipmapped:NO];
		depthDesc.textureType = MTLTextureType2D;
		depthDesc.storageMode = MTLStorageModePrivate;
		depthDesc.usage = MTLTextureUsageRenderTarget;
		ms->depthBuffer = [ms->device newTextureWithDescriptor:depthDesc];
		RAVE_LOG("Depth buffer created: %dx%d", priv->width, priv->height);
	}

	ms->renderPassActive = false;
	ms->currentPipelineKey = kRavePipelineKeyInvalid;
	ms->msaaActive = false;
	ms->msaaResourcesReady = false;

	// Buffer access staging (lazy allocation)
	ms->stagingDrawBuffer = nil;
	ms->stagingZBuffer = nil;
	ms->drawBufferCPU = nullptr;
	ms->drawBufferCPUMac = 0;
	ms->drawBufferCPUSize = 0;
	ms->zBufferCPU = nullptr;
	ms->zBufferCPUMac = 0;
	ms->zBufferCPUSize = 0;
	ms->drawBufferAccessed = false;
	ms->zBufferAccessed = false;
	ms->noticeDeviceMac = 0;
	ms->noticeDirtyRectMac = 0;

	// Depth-always-write state for ClearZBuffer
	if (hasDepthAttachment) {
		MTLDepthStencilDescriptor *dsDesc = [[MTLDepthStencilDescriptor alloc] init];
		dsDesc.depthCompareFunction = MTLCompareFunctionAlways;
		dsDesc.depthWriteEnabled = YES;
		ms->depthAlwaysWriteState = [ms->device newDepthStencilStateWithDescriptor:dsDesc];
	}

	RAVE_LOG("Metal resources initialized: device=%p queue=%p", ms->device, ms->commandQueue);
}


void RaveReleaseMetalResources(RaveDrawPrivate *priv)
{
	RaveMetalState *ms = priv->metal;
	if (!ms) return;
	const bool releaseGlobalRing = (rave_context_count <= 1);

	// Metal resource cleanup:
	// (1) RTT texture handles: disowned, NOT freed (RAVE 1.6 either-order delete)
	// (2) 48+48 pipeline states: nil'd (ARC releases)
	// (3) 9+9 depth-stencil states: nil'd (ARC releases)
	// (4) 3 samplers: nil'd (ARC releases)
	// (5) Staging buffers: nil'd (ARC releases)
	// (6) Masked/GL-blend pipeline caches: cleared (ARC releases)
	// (7) MSAA textures + pipelines: nil'd (ARC releases)
	// (8) Depth buffer, shader library, vertex descriptor: nil'd (ARC releases)
	// (9) Command queue, device, command buffers: nil'd (ARC releases)
	// (10) RAVE ring buffer: shutdown'd only after the last context releases

	// RTT (render-to-texture) resources created by this draw context stay
	// live in the resource table: RAVE 1.6 (pp.40-41) allows the texture or
	// bitmap and its source context to be deleted in either order, so the
	// guest's handle must survive this context. Each entry owns a standalone
	// MTLTexture (no reference back into this context) and is freed when the
	// guest deletes the resource itself (NativeEngineTextureDelete ->
	// RaveForgetRTTAndFreeResource). The token list is only context-membership
	// bookkeeping for RenderEnd's overlay->RTT blit.
	if (!ms->rttTextureHandles.empty()) {
		RAVE_LOG("RaveReleaseMetalResources: disowned %zu live RTT texture handles",
		         ms->rttTextureHandles.size());
	}
	ms->rttTextureHandles.clear();

	// (0) Ensure no pending GPU work before releasing resources
	if (ms->currentEncoder) {
		[ms->currentEncoder endEncoding];
		ms->currentEncoder = nil;
	}
	if (ms->currentCommandBuffer) {
		CommitRaveCommandBufferWithRing(ms->currentCommandBuffer);
		[ms->currentCommandBuffer waitUntilCompleted];
		ms->currentCommandBuffer = nil;
	}
	if (ms->lastCommittedBuffer) {
		[ms->lastCommittedBuffer waitUntilCompleted];
		ms->lastCommittedBuffer = nil;
	}

	// Nil out all Metal objects (ARC handles release)
	for (int i = 0; i < 48; i++)
		ms->pipelines[i] = nil;
	for (int i = 0; i < 48; i++)
		ms->msaaPipelines[i] = nil;
	for (int i = 0; i < 9; i++)
		ms->depthStates[i] = nil;
	for (int i = 0; i < 9; i++)
		ms->depthStatesNoWrite[i] = nil;

	for (int i = 0; i < 3; i++)
		ms->samplers[i] = nil;
	for (int i = 0; i < 16; i++)
		ms->glSamplers[i] = nil;

	ms->stagingDrawBuffer = nil;
	ms->stagingZBuffer = nil;
	ms->depthAlwaysWriteState = nil;
	// Note: drawBufferCPU/zBufferCPU/notice structs are Mac_sysalloc'd -- no explicit free needed
	// (Mac heap is reclaimed on emulator shutdown)

	ms->maskedPipelines.clear();
	ms->glBlendPipelines.clear();

	ms->msaaColorTexture = nil;
	ms->msaaDepthTexture = nil;
	ms->shaderLibrary = nil;
	ms->vertexDescriptor = nil;
	ms->depthBuffer = nil;
	ms->currentCommandBuffer = nil;
	ms->currentEncoder = nil;
	ms->overlayTexture = nil;
	ms->lastCommittedBuffer = nil;
	ms->commandQueue = nil;
	ms->device = nil;

	delete ms;
	priv->metal = nullptr;

	// RAVE's vertex ring is process-global, not per-context. NativeDrawPrivateDelete
	// decrements rave_context_count after this call, so a pre-delete count of one
	// means the context being released is the last live owner.
	if (releaseGlobalRing) {
		gfxaccel_rave_ring_shutdown();
	}

	RAVE_LOG("Metal resources released");
}


/*
 *  Render method helper
 */

static RaveDrawPrivate *GetContextFromDrawAddr(uint32 drawContextAddr)
{
	uint32 handle = ReadMacInt32(drawContextAddr + 0);
	return RaveGetContext(handle);
}

void RaveForgetRTTResourceHandle(uint32_t handle, uint32_t generation)
{
	if (handle == 0 || generation == 0) return;

	for (uint32_t ctxHandle = 1; ctxHandle <= RAVE_MAX_CONTEXTS; ctxHandle++) {
		RaveDrawPrivate *ctx = RaveGetContext(ctxHandle);
		if (!ctx || !ctx->metal) continue;

		RaveMetalState *ms = ctx->metal;
		auto &handles = ms->rttTextureHandles;
		size_t oldSize = handles.size();
		handles.erase(std::remove_if(handles.begin(), handles.end(),
			[handle, generation](const RaveRTTResourceToken &token) {
				return token.handle == handle && token.generation == generation;
			}), handles.end());
		if (handles.size() != oldSize) {
			RAVE_LOG("RaveForgetRTTResourceHandle: removed handle=%u gen=%u from ctx=%u",
			         handle, generation, ctxHandle);
		}
	}
}


/*
 *  GLBlendToMTL - map OpenGL blend factor constants to Metal blend factors
 */
static MTLBlendFactor GLBlendToMTL(uint32_t glFactor) {
	switch (glFactor) {
		case 0x0000: return MTLBlendFactorZero;
		case 0x0001: return MTLBlendFactorOne;
		case 0x0300: return MTLBlendFactorSourceColor;
		case 0x0301: return MTLBlendFactorOneMinusSourceColor;
		case 0x0302: return MTLBlendFactorSourceAlpha;
		case 0x0303: return MTLBlendFactorOneMinusSourceAlpha;
		case 0x0304: return MTLBlendFactorDestinationAlpha;
		case 0x0305: return MTLBlendFactorOneMinusDestinationAlpha;
		case 0x0306: return MTLBlendFactorDestinationColor;
		case 0x0307: return MTLBlendFactorOneMinusDestinationColor;
		case 0x0308: return MTLBlendFactorSourceAlphaSaturated;
		default:     return MTLBlendFactorOne;
	}
}


/*
 *  GetGLBlendPipeline - get or lazily create a pipeline with GL blend factors
 *
 *  Used when kQABlend_OpenGL (blend mode 2) is active. Caches pipelines
 *  keyed by function constants, channel mask, GL blend factors, sample count,
 *  and whether the active pass has a depth attachment.
 */
static id<MTLRenderPipelineState> GetGLBlendPipeline(RaveMetalState *ms, int funcConstBits,
                                                     uint32_t glBlendSrc, uint32_t glBlendDst,
                                                     uint32_t channelMask,
                                                     bool msaa, bool hasDepthAttachment) {
	uint64_t key = ((uint64_t)(funcConstBits & 0xF) << 48) | ((uint64_t)(msaa ? 1 : 0) << 32)
	             | ((uint64_t)(glBlendSrc & 0xFFFF) << 16) | (uint64_t)(glBlendDst & 0xFFFF);
	if (hasDepthAttachment) key |= (1ULL << 33);
	key |= ((uint64_t)(channelMask & 0xF) << 34);

	auto it = ms->glBlendPipelines.find(key);
	if (it != ms->glBlendPipelines.end()) return it->second;

	// Derive function constants from bits 0-3
	bool tex      = (funcConstBits & 1) != 0;
	bool fog      = (funcConstBits & 2) != 0;
	bool alpha    = (funcConstBits & 4) != 0;
	bool multiTex = (funcConstBits & 8) != 0;
	bool premultiplyOutput = false;

	MTLFunctionConstantValues *constants = [[MTLFunctionConstantValues alloc] init];
	[constants setConstantValue:&tex      type:MTLDataTypeBool atIndex:0];
	[constants setConstantValue:&fog      type:MTLDataTypeBool atIndex:1];
	[constants setConstantValue:&alpha    type:MTLDataTypeBool atIndex:2];
	[constants setConstantValue:&multiTex type:MTLDataTypeBool atIndex:3];
	[constants setConstantValue:&premultiplyOutput type:MTLDataTypeBool atIndex:4];

	NSError *err = nil;
	id<MTLFunction> vertexFunc = [ms->shaderLibrary newFunctionWithName:@"rave_vertex"
	                                                     constantValues:constants error:&err];
	id<MTLFunction> fragmentFunc = [ms->shaderLibrary newFunctionWithName:@"rave_fragment"
	                                                       constantValues:constants error:&err];
	if (!vertexFunc || !fragmentFunc) {
		RAVE_LOG("GetGLBlendPipeline: function creation failed for bits %d src 0x%x dst 0x%x",
		         funcConstBits, glBlendSrc, glBlendDst);
		return nil;
	}

	MTLRenderPipelineDescriptor *pipeDesc = [[MTLRenderPipelineDescriptor alloc] init];
	pipeDesc.vertexFunction = vertexFunc;
	pipeDesc.fragmentFunction = fragmentFunc;
	pipeDesc.vertexDescriptor = ms->vertexDescriptor;
	pipeDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	pipeDesc.depthAttachmentPixelFormat = hasDepthAttachment ? MTLPixelFormatDepth32Float : MTLPixelFormatInvalid;
	pipeDesc.sampleCount = msaa ? 4 : 1;

	// GL blend factors
	pipeDesc.colorAttachments[0].blendingEnabled = YES;
	pipeDesc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
	pipeDesc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
	pipeDesc.colorAttachments[0].sourceRGBBlendFactor = GLBlendToMTL(glBlendSrc);
	pipeDesc.colorAttachments[0].destinationRGBBlendFactor = GLBlendToMTL(glBlendDst);
	pipeDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
	pipeDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	pipeDesc.colorAttachments[0].writeMask = RaveChannelMaskToMTL(channelMask);

	id<MTLRenderPipelineState> pipeline = [ms->device newRenderPipelineStateWithDescriptor:pipeDesc error:&err];
	if (!pipeline) {
		RAVE_LOG("GetGLBlendPipeline: creation failed for bits %d src 0x%x dst 0x%x: %s",
		         funcConstBits, glBlendSrc, glBlendDst, [[err localizedDescription] UTF8String]);
		return nil;
	}

	ms->glBlendPipelines[key] = pipeline;
	RAVE_LOG("GetGLBlendPipeline: created bits %d src 0x%x dst 0x%x msaa=%d (cache size=%zu)",
	         funcConstBits, glBlendSrc, glBlendDst, msaa, ms->glBlendPipelines.size());
	return pipeline;
}


/*
 *  RaveFlushRingWindowIfNearExhaustion - commit the ring submission window
 *  before it can self-deadlock.
 *
 *  Ring slot credits consumed by the current (uncommitted) submission are
 *  only signal-registered at commit (gfxaccel_rave_ring_frame_end), so a
 *  window that staged into every slot would wait forever acquiring the next
 *  one. Mirror GL's mid-frame flush (GLMetalFlushAndResetRingBuffer): commit
 *  and restart with LoadActionLoad. Must run at a draw-sequence boundary,
 *  BEFORE any per-draw encoder state is applied — a mid-draw restart would
 *  orphan bindings already encoded for the draw in progress, whereas here
 *  the caller applies everything on the new encoder.
 */
static void RaveFlushRingWindowIfNearExhaustion(RaveDrawPrivate *priv)
{
	if (!gfxaccel_rave_ring_submission_near_exhaustion()) return;

	RaveMetalState *ms = priv->metal;
	if (!ms || !ms->renderPassActive || !ms->currentEncoder || !ms->currentCommandBuffer) return;

	[ms->currentEncoder endEncoding];
	ms->currentEncoder = nil;
	CommitRaveCommandBufferWithRing(ms->currentCommandBuffer);
	ms->lastCommittedBuffer = ms->currentCommandBuffer;
	ms->currentCommandBuffer = nil;
	RestartRenderPassWithLoad(priv);
	RAVE_VLOG("RaveFlushRingWindowIfNearExhaustion: ring window committed mid-frame");
}


/*
 *  ApplyDirtyState - set pipeline and depth state on encoder per dirty flags
 *
 *  textured: when true, selects has_texture=1 pipeline variant and binds
 *            the current texture, sampler, and fragment uniforms (TextureOp).
 */
static void ApplyDirtyState(RaveDrawPrivate *priv, bool forceAll, bool textured = false)
{
	RaveMetalState *ms = priv->metal;
	if (!ms->currentEncoder) return;

	// Ring-exhaustion guard: every per-draw bind below lands on the (possibly
	// fresh) encoder this leaves behind.
	RaveFlushRingWindowIfNearExhaustion(priv);
	if (!ms->currentEncoder) return;

	// Texture realize / live pixmap refresh can break and restart the render
	// pass (fenced staging upload), so run them BEFORE any per-draw encoder
	// state below: a restart's fresh encoder silently drops bindings already
	// issued for this draw (pipeline/depth/scissor/texture), leaving the draw
	// that triggered the break to render with missing state.
	//
	// Deferred texture creation: if metal_texture is nil but we have a pixmap
	// address, the texture was created with placeholder data at QATextureNew
	// time. The game has since written the real pixels into Mac memory. Read
	// them now and create the Metal texture.
	//
	// Live pixmap re-upload: the original RAVE software renderer reads from
	// the pixmap on every draw call (no GPU upload step). QD3D's Interactive
	// Renderer writes directly to the pixmap between frames. We must re-read
	// and re-upload every frame to match this behavior.
	RaveResourceEntry *tex_entry = nullptr;
	RaveResourceEntry *tex2_entry = nullptr;
	if (textured) {
		uint32_t tex_mac_addr = priv->state[13].i;  // kQATag_Texture
		if (tex_mac_addr != 0) {
			tex_entry = RaveResourceGet(RaveResourceFindByAddr(tex_mac_addr));
			if (tex_entry) {
				if (!tex_entry->metal_texture && tex_entry->pixmap_mac_addr != 0) {
					RaveRealizeDeferredTexture(tex_entry);
				}
				else if (RaveTextureNeedsLivePixmapRefresh(tex_entry)) {
					RaveRefreshTextureFromPixmap(tex_entry);
				}
			}
		}
		if (priv->multiTextureActive && priv->multiTextureHandle != 0) {
			tex2_entry = RaveResourceGet(RaveResourceFindByAddr(priv->multiTextureHandle));
			if (tex2_entry) {
				if (!tex2_entry->metal_texture && tex2_entry->pixmap_mac_addr != 0) {
					RaveRealizeDeferredTexture(tex2_entry);
				}
				else if (RaveTextureNeedsLivePixmapRefresh(tex2_entry)) {
					RaveRefreshTextureFromPixmap(tex2_entry);
				}
			}
		}
		// A failed restart leaves no encoder to bind on.
		if (!ms->currentEncoder) return;
	}

	// Determine current pipeline index
	// bit 0 = has_texture, bit 1 = has_fog, bit 2 = has_alpha_test, bit 3 = has_multi_texture
	int blend_mode = (int)priv->state[9].i;  // kQATag_Blend
	int func_const_bits = 0;
	if (textured) func_const_bits |= 1;  // bit 0 = has_texture
	if (RaveEffectiveFogEnabled(priv)) func_const_bits |= 2;  // bit 1: has_fog
	if (priv->state[31].i != 0 && priv->state[31].i != 7) func_const_bits |= 4;  // bit 2: has_alpha_test
	if (priv->multiTextureActive) func_const_bits |= 8;  // bit 3: has_multi_texture

	const bool hasDepthAttachment = RaveContextUsesMetalDepthAttachment(priv->flags) != 0;
	uint32_t channelMask = RaveNormalizeChannelMask(priv->state[27].i);  // kQATag_ChannelMask

	int pipe_idx = (blend_mode == 2 ? 0 : (blend_mode < 0 || blend_mode > 1 ? 0 : blend_mode)) * 16 + func_const_bits;
	const uint32_t glSrc = blend_mode == 2 ? priv->state[109].i : 0;  // kQATagGL_BlendSrc
	const uint32_t glDst = blend_mode == 2 ? priv->state[110].i : 0;  // kQATagGL_BlendDst
	uint64_t pipelineKey = RavePipelineBindingKey(pipe_idx, channelMask, blend_mode == 2,
	                                             glSrc, glDst, ms->msaaActive,
	                                             hasDepthAttachment);

	if (forceAll || pipelineKey != ms->currentPipelineKey) {
		// kQABlend_OpenGL pipeline lookup only when the binding actually
		// changes — the hash find is redundant while the key matches the
		// encoder's current pipeline (the steady state for UE1 titles).
		id<MTLRenderPipelineState> selectedPipeline = nil;
		if (blend_mode == 2) {
			selectedPipeline = GetGLBlendPipeline(ms, func_const_bits, glSrc, glDst,
			                                      channelMask, ms->msaaActive,
			                                      hasDepthAttachment);
		}
		if (selectedPipeline) {
			// GL blend pipeline (kQABlend_OpenGL)
			[ms->currentEncoder setRenderPipelineState:selectedPipeline];
			ms->currentPipelineKey = pipelineKey;
		} else {
			// Channel mask applied uniformly in all draw paths.
			// Test: RAVERenderingStateTests.testChannelMask_RedOnly
			// Paths covered: ApplyDirtyState (all Draw* methods), FlushZSortBuffer,
			// NativeDrawBitmap, NativeClearDrawBuffer.
			if (channelMask != 0xF) {
				id<MTLRenderPipelineState> maskedPipe = GetMaskedPipeline(
					ms, pipe_idx, channelMask, ms->msaaActive, hasDepthAttachment);
				if (maskedPipe) {
					[ms->currentEncoder setRenderPipelineState:maskedPipe];
					ms->currentPipelineKey = pipelineKey;
				}
			} else {
				id<MTLRenderPipelineState> __strong *pipeArray = ms->msaaActive ? ms->msaaPipelines : ms->pipelines;
				if (pipe_idx >= 0 && pipe_idx < 48 && pipeArray[pipe_idx]) {
					[ms->currentEncoder setRenderPipelineState:pipeArray[pipe_idx]];
					ms->currentPipelineKey = pipelineKey;
				}
			}
		}
	}

	// Depth writes depend on both z-mask tags and the active blend factors.
	RaveSetDepthStencilStateForCurrentDraw(priv);

	// Apply GL scissor rect if GL tags are set (tags 105-108)
	if (priv->state[105].i != 0 || priv->state[106].i != 0 ||
	    priv->state[107].i != 0 || priv->state[108].i != 0) {
		MTLScissorRect scissor;
		scissor.x = (NSUInteger)priv->state[105].i;       // kQATagGL_ScissorXMin
		scissor.y = (NSUInteger)priv->state[106].i;       // kQATagGL_ScissorYMin
		scissor.width = (NSUInteger)(priv->state[107].i - priv->state[105].i);   // XMax - XMin
		scissor.height = (NSUInteger)(priv->state[108].i - priv->state[106].i);  // YMax - YMin
		// Clamp to drawable dimensions to avoid Metal validation errors
		if (scissor.x + scissor.width > (NSUInteger)priv->width)
			scissor.width = priv->width - scissor.x;
		if (scissor.y + scissor.height > (NSUInteger)priv->height)
			scissor.height = priv->height - scissor.y;
		if (scissor.width > 0 && scissor.height > 0) {
			[ms->currentEncoder setScissorRect:scissor];
		}
	}

	// Bind texture and sampler when rendering textured geometry
	bool boundTexOpaque = false;  // reused by the opaque-texture guard below (avoids a 2nd lookup)
	if (textured) {
		// Bind texture from kQATag_Texture (state[13]); realized/refreshed above,
		// before any encoder state was issued for this draw.
		if (tex_entry) {
			boundTexOpaque =
				RaveTextureDiagUsesOpaqueAlphaGuard(tex_entry->diag_alpha_zero);
			if (tex_entry->metal_texture) {
				id<MTLTexture> mtlTex = (__bridge id<MTLTexture>)tex_entry->metal_texture;
				[ms->currentEncoder setFragmentTexture:mtlTex atIndex:0];
			}
		}

		// Bind sampler: check GL wrap/filter tags (101-104) for custom sampler
		uint32_t glWrapU  = priv->state[101].i;  // kQATagGL_TextureWrapU
		uint32_t glWrapV  = priv->state[102].i;  // kQATagGL_TextureWrapV
		uint32_t glMagFilt = priv->state[103].i;  // kQATagGL_TextureMagFilter
		uint32_t glMinFilt = priv->state[104].i;  // kQATagGL_TextureMinFilter
		if (glWrapU != 0 || glWrapV != 0 || glMagFilt != 0 || glMinFilt != 0) {
			// GL tags set: use the lazily-built sampler for this combination.
			// These tags are steady state for UE1 titles, so building a fresh
			// descriptor + newSamplerStateWithDescriptor per textured draw was
			// pure per-draw ObjC overhead for one of 16 immutable objects.
			int glIdx = ((glWrapU == 1) ? 1 : 0) |
			            ((glWrapV == 1) ? 2 : 0) |
			            ((glMagFilt == 1) ? 4 : 0) |
			            ((glMinFilt == 1) ? 8 : 0);
			if (!ms->glSamplers[glIdx]) {
				MTLSamplerDescriptor *sampDesc = [[MTLSamplerDescriptor alloc] init];
				sampDesc.sAddressMode = (glWrapU == 1) ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
				sampDesc.tAddressMode = (glWrapV == 1) ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
				sampDesc.magFilter = (glMagFilt == 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
				sampDesc.minFilter = (glMinFilt == 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
				ms->glSamplers[glIdx] = [ms->device newSamplerStateWithDescriptor:sampDesc];
			}
			[ms->currentEncoder setFragmentSamplerState:ms->glSamplers[glIdx] atIndex:0];
		} else {
			// Use standard sampler from kQATag_TextureFilter (state[11])
			int filter = (int)priv->state[11].i;
			if (filter < 0 || filter > 2) filter = 0;
			[ms->currentEncoder setFragmentSamplerState:ms->samplers[filter] atIndex:0];
		}

		// Multi-texture: bind second texture and sampler at indices 1
		if (priv->multiTextureActive && priv->multiTextureHandle != 0) {
			if (tex2_entry && tex2_entry->metal_texture) {
				id<MTLTexture> mtlTex2 = (__bridge id<MTLTexture>)tex2_entry->metal_texture;
				[ms->currentEncoder setFragmentTexture:mtlTex2 atIndex:1];
			}
			// Use same sampler filter for second texture
			int filter = (int)priv->state[11].i;
			if (filter < 0 || filter > 2) filter = 0;
			[ms->currentEncoder setFragmentSamplerState:ms->samplers[filter] atIndex:1];
		}
	}

	// Bind vertex uniforms (point width from kQATag_Width) at buffer(2)
	VertexUniforms vertUniforms;
	vertUniforms.point_width = priv->state[5].f;  // kQATag_Width
	if (vertUniforms.point_width < 1.0f) vertUniforms.point_width = 1.0f;
	[ms->currentEncoder setVertexBytes:&vertUniforms length:sizeof(vertUniforms) atIndex:2];

	// Always bind fragment uniforms (needed for fog/alpha_test even without texture)
	FragmentUniforms fragUniforms = {};
	if (textured) {
		fragUniforms.texture_op = (int32_t)priv->state[12].i;
		// Opaque-texture guard (bit30): a texture with no transparent texels at level 0
		// (diag_alpha_zero==0) is opaque-intent and must never sample alpha<1. Flag it so
		// the shader forces texPix.a=1, defeating any residual mip/filter alpha artifact;
		// vertex alpha and the blend mode still control real translucency, and masked
		// textures (alpha0>0) are untouched. boundTexOpaque was captured during the
		// texture bind above, so this reuses that lookup rather than repeating it.
		if (boundTexOpaque) {
			fragUniforms.texture_op |= RAVE_TEXTURE_OP_FORCE_OPAQUE_ALPHA;
		}
	}
	RaveFillFogUniforms(priv, &fragUniforms);
	fragUniforms.alpha_test_func = (int32_t)priv->state[31].i;
	fragUniforms.alpha_test_ref = priv->state[46].f;
	fragUniforms.multi_texture_op = priv->state[35].i;      // kQATag_MultiTextureOp
	fragUniforms.multi_texture_factor = priv->state[51].f;  // kQATag_MultiTextureFactor
	fragUniforms.mipmap_bias = RaveMetalSamplerMipBias(priv->state[41].f);  // kQATag_MipmapBias
	fragUniforms.multi_texture_mipmap_bias = RaveMetalSamplerMipBias(priv->state[42].f);  // kQATag_MultiTextureMipmapBias
	// TextureOp Blend env color from GL texture env color tags (150-153)
	fragUniforms.env_color_r = priv->state[151].f;  // kQATagGL_TextureEnvColor_r
	fragUniforms.env_color_g = priv->state[152].f;  // kQATagGL_TextureEnvColor_g
	fragUniforms.env_color_b = priv->state[153].f;  // kQATagGL_TextureEnvColor_b
	fragUniforms.env_color_a = priv->state[150].f;  // kQATagGL_TextureEnvColor_a
	[ms->currentEncoder setFragmentBytes:&fragUniforms length:sizeof(fragUniforms) atIndex:0];

	priv->dirty_flags = 0;
}


/*
 *  Vertex conversion helpers
 */

// RaveVertex matches the Metal vertex layout.
struct RaveVertex {
	float pos[4];    // x, y, z, w          (16 bytes)
	float color[4];  // r, g, b, a          (16 bytes)
	float uv[4];     // uOverW, vOverW, invW, 0  (16 bytes)
	float diffuse[4];   // kd_r, kd_g, kd_b, unused (16 bytes)
	float specular[4];  // ks_r, ks_g, ks_b, unused (16 bytes)
};

static_assert(sizeof(RaveVertex) == RAVE_VERTEX_BYTES, "RaveVertex layout must match shared staging constants");

static inline void SetTextureFactors(RaveVertex *dst, float kdR, float kdG, float kdB,
                                     float ksR, float ksG, float ksB)
{
	dst->diffuse[0] = kdR;
	dst->diffuse[1] = kdG;
	dst->diffuse[2] = kdB;
	dst->diffuse[3] = 0.0f;
	dst->specular[0] = ksR;
	dst->specular[1] = ksG;
	dst->specular[2] = ksB;
	dst->specular[3] = 0.0f;
}

// Read a float from Mac address space.
// ReadMacInt32 already returns host-endian (bswap_32 applied internally).
// We just reinterpret the uint32 bits as float.
static inline float ReadMacFloat(uint32 addr) {
	uint32 bits = ReadMacInt32(addr);
	float f;
	memcpy(&f, &bits, sizeof(float));
	return f;
}

static uint32_t RaveVertexPrimitiveCount(uint32_t nVertices, uint32_t vertexMode)
{
	switch (vertexMode) {
	case 0: return nVertices;                 // points
	case 1: return nVertices / 2;             // independent lines
	case 2: return nVertices > 1 ? nVertices - 1 : 0;  // line strip
	case 3: return nVertices / 3;             // triangle list
	case 4:
	case 5: return nVertices > 2 ? nVertices - 2 : 0;  // triangle strip/fan
	default: return 0;
	}
}

static void RaveTrackTextureDraw(RaveDrawPrivate *priv)
{
	if (!priv || !priv->metal) return;
	uint32_t texAddr = priv->state[13].i;
	uint32_t texHandle = RaveResourceFindByAddr(texAddr);
	if (texHandle > 0 && texHandle <= RAVE_MAX_RESOURCES) {
		priv->metal->textureDrawCounts[texHandle]++;
	}
}


static void ConvertGouraudVertex(uint32 srcAddr, RaveVertex *dst, bool perspZ) {
	dst->pos[0] = ReadMacFloat(srcAddr + 0);   // x (pixel)
	dst->pos[1] = ReadMacFloat(srcAddr + 4);   // y (pixel)
	float z    = ReadMacFloat(srcAddr + 8);     // z (0..1)
	float invW = ReadMacFloat(srcAddr + 12);    // 1/w
	// Clamp RAVE's submitted depth into Metal's clip range. Tomb Raider Gold's
	// QD3D menu text can submit large negative z while still expecting rasterization.
	// Metal clips those vertices before depth testing unless we normalize here.
	// The perspZ flag (kQATag_PerspectiveZ)
	// tells the hardware about z-value distribution (linear vs 1/z) for
	// precision, but z is already in [0,1] range for the depth buffer.
	// Using 1/invW (= w) produces values like 95.0 for Tomb Raider,
	// which are clipped by Metal's [0,1] depth range.
	dst->pos[2] = RaveClampMetalDepth(z);
	dst->pos[3] = 1.0f;

	dst->color[0] = ReadMacFloat(srcAddr + 16); // r
	dst->color[1] = ReadMacFloat(srcAddr + 20); // g
	dst->color[2] = ReadMacFloat(srcAddr + 24); // b
	dst->color[3] = ReadMacFloat(srcAddr + 28); // a

	dst->uv[0] = 0.0f;
	dst->uv[1] = 0.0f;
	dst->uv[2] = invW;  // pass invW for fog depth computation
	dst->uv[3] = 0.0f;
	SetTextureFactors(dst, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f);
}


/*
 *  Per-draw scratch arenas
 *
 *  Draw methods convert guest vertices into a native array that is fully
 *  consumed (setVertexBytes copy or staging-ring copy) before the method
 *  returns, so a grow-only arena on the context replaces a heap alloc/free
 *  pair per draw call. Contents are per-draw; growth never preserves them.
 */
static inline void *RaveDrawScratchEnsure(uint8_t **buf, uint32_t *cap, size_t bytes)
{
	// Never returns null (mirrors new[]'s non-null result even for a
	// zero-element request); +64 keeps degenerate counts off the allocator.
	if (*buf == nullptr || (size_t)*cap < bytes) {
		delete[] *buf;
		size_t grown = bytes + bytes / 2 + 64;
		*buf = new uint8_t[grown];
		*cap = (uint32_t)grown;
	}
	return *buf;
}

/*
 *  Z-sorted transparency helpers
 *
 *  When kQATag_ZSortedHint (tag 29) is set, transparent triangles are buffered
 *  instead of drawn immediately. At RenderEnd, they are sorted back-to-front
 *  (higher Z = farther = drawn first) and flushed with per-triangle state.
 */
static void BufferZSortTriangle(RaveDrawPrivate *priv, const RaveVertex *v0, const RaveVertex *v1,
                                 const RaveVertex *v2, bool textured)
{
	if (priv->zsortCount >= RAVE_ZSORT_MAX_TRIANGLES) {
		RAVE_VLOG("ZSort: buffer full (%d triangles), dropping triangle", RAVE_ZSORT_MAX_TRIANGLES);
		return;
	}

	ZSortTriangle *tri = &priv->zsortBuffer[priv->zsortCount++];
	memcpy(&tri->verts[0], v0, sizeof(RaveVertex));
	memcpy(&tri->verts[1], v1, sizeof(RaveVertex));
	memcpy(&tri->verts[2], v2, sizeof(RaveVertex));
	tri->sortKey = (v0->pos[2] + v1->pos[2] + v2->pos[2]) / 3.0f;
	tri->textured = textured;
	tri->textureMacAddr = priv->state[13].i;  // kQATag_Texture
	tri->textureOp = (int32_t)priv->state[12].i;
	tri->blendMode = (int32_t)priv->state[9].i;
	tri->glBlendSrc = (uint32_t)priv->state[109].i;
	tri->glBlendDst = (uint32_t)priv->state[110].i;
	tri->filterMode = (int32_t)priv->state[11].i;
}

static void FlushZSortBuffer(RaveDrawPrivate *priv)
{
	if (priv->zsortCount == 0) return;

	RaveMetalState *ms = priv->metal;
	if (!ms || !ms->currentEncoder) return;

	// Ring-exhaustion guard: all batch state below is applied after this, so
	// a restart here cannot orphan encoder bindings.
	RaveFlushRingWindowIfNearExhaustion(priv);
	if (!ms->currentEncoder) return;

	// Sort back-to-front (higher Z = farther = draw first)
	std::sort(priv->zsortBuffer, priv->zsortBuffer + priv->zsortCount,
	          [](const ZSortTriangle &a, const ZSortTriangle &b) {
	              return a.sortKey > b.sortKey;
	          });

	// Build fragment uniforms once (fog/alpha state is per-context, not per-triangle)
	FragmentUniforms fragUniforms = {};
	RaveFillFogUniforms(priv, &fragUniforms);
	fragUniforms.alpha_test_func = (int32_t)priv->state[31].i;
	fragUniforms.alpha_test_ref = priv->state[46].f;
	fragUniforms.mipmap_bias = RaveMetalSamplerMipBias(priv->state[41].f);
	fragUniforms.multi_texture_mipmap_bias = RaveMetalSamplerMipBias(priv->state[42].f);
	fragUniforms.env_color_r = priv->state[151].f;
	fragUniforms.env_color_g = priv->state[152].f;
	fragUniforms.env_color_b = priv->state[153].f;
	fragUniforms.env_color_a = priv->state[150].f;

	VertexUniforms vertUniforms;
	vertUniforms.point_width = priv->state[5].f;
	if (vertUniforms.point_width < 1.0f) vertUniforms.point_width = 1.0f;
	[ms->currentEncoder setVertexBytes:&vertUniforms length:sizeof(vertUniforms) atIndex:2];

	bool hasFog = RaveEffectiveFogEnabled(priv);
	bool hasAlphaTest = (priv->state[31].i != 0 && priv->state[31].i != 7);

	// Batch consecutive triangles that share the same render state into single
	// draw calls.  The z-sort order is preserved within each batch since we
	// scan forward.  State keys: textured, textureMacAddr, blendMode,
	// filterMode, textureOp.  Triangles with different keys break the batch.
	//
	// Temporary vertex buffer: sized for the entire zsort buffer.
	// Most frames reuse a single batch for all triangles (same texture/blend).
	RaveVertex *batchVerts = (RaveVertex *)RaveDrawScratchEnsure(&priv->drawScratchA, &priv->drawScratchACap, (priv->zsortCount * 3) * sizeof(RaveVertex));
	uint32_t batchCount = 0;
	uint32_t drawCalls = 0;

	// State of the current batch (from the first triangle)
	int      curPipeIdx = -1;
	bool     curUsesGLPipeline = false;
	uint32_t curGlBlendSrc = 0;
	uint32_t curGlBlendDst = 0;
	uint32_t curTexAddr = 0;
	int32_t  curTexOp   = 0;
	int      curFilter  = 0;
	bool     curTextured = false;
	bool     curTexOpaque = false;

	auto flushBatch = [&]() {
		if (batchCount == 0) return;
		uint32_t vertCount = batchCount * 3;
		size_t dataSize = vertCount * sizeof(RaveVertex);

		// Set fragment uniforms with the current batch's textureOp. Z-sorted
		// texture batches use the same opaque-texture guard as the immediate path.
		fragUniforms.texture_op = curTextured ? curTexOp : 0;
		if (curTextured && curTexOpaque) {
			fragUniforms.texture_op |= RAVE_TEXTURE_OP_FORCE_OPAQUE_ALPHA;
		}
		[ms->currentEncoder setFragmentBytes:&fragUniforms length:sizeof(fragUniforms) atIndex:0];

		if (dataSize > 4096) {
			uint32_t ringOffset = 0;
			void *ringBuf = gfxaccel_rave_ring_stage(batchVerts, (uint32_t)dataSize, &ringOffset);
			if (ringBuf) {
				[ms->currentEncoder setVertexBuffer:(__bridge id<MTLBuffer>)ringBuf offset:ringOffset atIndex:0];
			} else {
				fprintf(stderr, "[RAVE Metal] ring buffer not available; skipping draw\n");
				batchCount = 0;
				return;
			}
		} else {
			[ms->currentEncoder setVertexBytes:batchVerts length:dataSize atIndex:0];
		}
		[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:vertCount];
		drawCalls++;
		batchCount = 0;
	};

	for (uint32_t i = 0; i < priv->zsortCount; i++) {
		ZSortTriangle *tri = &priv->zsortBuffer[i];

		int blend_mode = tri->blendMode;
		if (blend_mode < 0 || blend_mode > 2) blend_mode = 0;
		int func_const_bits = 0;
		if (tri->textured) func_const_bits |= 1;
		if (hasFog) func_const_bits |= 2;
		if (hasAlphaTest) func_const_bits |= 4;
		bool usesGLPipeline = RaveBlendModeUsesGLPipeline(blend_mode) != 0;
		int pipe_idx = (usesGLPipeline ? 0 : blend_mode) * 16 + func_const_bits;

		// Check if this triangle can join the current batch
		bool stateChanged = (pipe_idx != curPipeIdx ||
		                     usesGLPipeline != curUsesGLPipeline ||
		                     (usesGLPipeline &&
		                      (tri->glBlendSrc != curGlBlendSrc || tri->glBlendDst != curGlBlendDst)) ||
		                     tri->textured != curTextured ||
		                     tri->textureMacAddr != curTexAddr ||
		                     tri->textureOp != curTexOp ||
		                     tri->filterMode != curFilter);

		if (stateChanged) {
			// Flush previous batch
			flushBatch();

			// Set new GPU state
			curPipeIdx = pipe_idx;
			curUsesGLPipeline = usesGLPipeline;
			curGlBlendSrc = tri->glBlendSrc;
			curGlBlendDst = tri->glBlendDst;
			curTextured = tri->textured;
			curTexAddr = tri->textureMacAddr;
			curTexOp = tri->textureOp;
			curFilter = tri->filterMode;
			curTexOpaque = false;

			// Texture realize / live pixmap refresh can break and restart the
			// render pass (fenced staging upload) — run it after the previous
			// batch's flush but before the pipeline/depth/texture binds below,
			// so a restart cannot orphan state already issued for this batch
			// (same hazard as ApplyDirtyState's texture section).
			RaveResourceEntry *tex_entry = nullptr;
			if (tri->textured && tri->textureMacAddr != 0) {
				tex_entry = RaveResourceGet(RaveResourceFindByAddr(tri->textureMacAddr));
				if (tex_entry) {
					if (!tex_entry->metal_texture && tex_entry->pixmap_mac_addr != 0) {
						RaveRealizeDeferredTexture(tex_entry);
					}
					else if (RaveTextureNeedsLivePixmapRefresh(tex_entry)) {
						RaveRefreshTextureFromPixmap(tex_entry);
					}
				}
			}

			const bool hasDepthAttachment = RaveContextUsesMetalDepthAttachment(priv->flags) != 0;

			uint32_t channelMask = RaveNormalizeChannelMask(priv->state[27].i);  // kQATag_ChannelMask
			if (usesGLPipeline) {
				id<MTLRenderPipelineState> glPipe = GetGLBlendPipeline(
					ms, func_const_bits, tri->glBlendSrc, tri->glBlendDst,
					channelMask, ms->msaaActive, hasDepthAttachment);
				if (glPipe)
					[ms->currentEncoder setRenderPipelineState:glPipe];
			} else {
				// Apply channel mask in z-sort flush path (same as ApplyDirtyState)
				if (channelMask != 0xF) {
					id<MTLRenderPipelineState> maskedPipe = GetMaskedPipeline(
						ms, pipe_idx, channelMask, ms->msaaActive, hasDepthAttachment);
					if (maskedPipe)
						[ms->currentEncoder setRenderPipelineState:maskedPipe];
				} else {
					id<MTLRenderPipelineState> __strong *pipeArray = ms->msaaActive ? ms->msaaPipelines : ms->pipelines;
					if (pipe_idx >= 0 && pipe_idx < 48 && pipeArray[pipe_idx])
						[ms->currentEncoder setRenderPipelineState:pipeArray[pipe_idx]];
				}
			}
			RaveSetDepthStencilStateForDraw(priv, blend_mode, tri->glBlendSrc, tri->glBlendDst);

			if (tri->textured && tri->textureMacAddr != 0) {
				if (tex_entry) {
					curTexOpaque =
						RaveTextureDiagUsesOpaqueAlphaGuard(tex_entry->diag_alpha_zero);
					if (tex_entry->metal_texture) {
						id<MTLTexture> mtlTex = (__bridge id<MTLTexture>)tex_entry->metal_texture;
						[ms->currentEncoder setFragmentTexture:mtlTex atIndex:0];
					}
				}
				int filter = tri->filterMode;
				if (filter < 0 || filter > 2) filter = 0;
				[ms->currentEncoder setFragmentSamplerState:ms->samplers[filter] atIndex:0];
			}
		}

		// Append triangle vertices to batch
		memcpy(&batchVerts[batchCount * 3], tri->verts, sizeof(RaveVertex) * 3);
		batchCount++;
	}

	// Flush final batch
	flushBatch();
	ms->currentPipelineKey = kRavePipelineKeyInvalid;

	RAVE_LOG("ZSort: flushed %d triangles in %d draw calls", priv->zsortCount, drawCalls);
	priv->zsortCount = 0;
}


/*
 *  NativeDrawTriGouraud
 *
 *  DrawTriGouraud(drawContext, v0, v1, v2, flags)
 *  PPC args: r3=drawContextAddr, r4=v0Addr, r5=v1Addr, r6=v2Addr, r7=flags
 *
 *  flags: kQATriFlags_Backfacing (bit 0) = triangle is back-facing (hint only)
 *  Per RAVE spec p.1566, this flag is a hint to help resolve Z-buffer ambiguity,
 *  NOT a cull instruction. The QD3D IR handles actual backface culling before
 *  sending triangles to RAVE. We ignore this flag.
 */
int32 NativeDrawTriGouraud(uint32 drawContextAddr, uint32 v0Addr, uint32 v1Addr,
                            uint32 v2Addr, uint32 flags)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal || !priv->metal->renderPassActive) return kQANoErr;

	// kQATriFlags_Backfacing (bit 0) is a hint for Z-buffer disambiguation only.
	// The QD3D IR culls backfacing triangles itself before sending to RAVE.
	(void)flags;

	RaveMetalState *ms = priv->metal;
	bool perspZ = (priv->state[10].i != 0);  // kQATag_PerspectiveZ

	// Convert 3 vertices
	RaveVertex verts[3];
	ConvertGouraudVertex(v0Addr, &verts[0], perspZ);
	ConvertGouraudVertex(v1Addr, &verts[1], perspZ);
	ConvertGouraudVertex(v2Addr, &verts[2], perspZ);

	// Z-sorted transparency: buffer instead of drawing immediately
	if (priv->state[29].i == 1) {  // kQATag_ZSortedHint
		BufferZSortTriangle(priv, &verts[0], &verts[1], &verts[2], false);
		return kQANoErr;
	}

	// Apply pending state (pipeline, depth)
	ApplyDirtyState(priv, false);

	// Submit triangle
	[ms->currentEncoder setVertexBytes:verts length:sizeof(verts) atIndex:0];
	[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	ms->drawCallCount++;
	ms->triangleCount += 1;
	ms->triGouraudCount++;

	return kQANoErr;
}


/*
 *  ConvertTextureVertex - convert TQAVTexture (64 bytes) to RaveVertex
 *
 *  TQAVTexture layout (16 floats, 64 bytes):
 *    0: x, 4: y, 8: z, 12: invW
 *   16: r, 20: g, 24: b, 28: a
 *   32: uOverW, 36: vOverW
 *   40: kd_r, 44: kd_g, 48: kd_b
 *   52: ks_r, 56: ks_g, 60: ks_b
 *
 *  TextureOp is a mask, so base color, diffuse, and specular are carried
 *  independently for the fragment shader to combine in spec order.
 */
static void ConvertTextureVertex(uint32 srcAddr, RaveVertex *dst, bool perspZ) {
	dst->pos[0] = ReadMacFloat(srcAddr + 0);   // x
	dst->pos[1] = ReadMacFloat(srcAddr + 4);   // y
	float z     = ReadMacFloat(srcAddr + 8);    // z
	float invW  = ReadMacFloat(srcAddr + 12);   // invW
	dst->pos[2] = RaveClampMetalDepth(z);
	dst->pos[3] = 1.0f;

	dst->color[0] = ReadMacFloat(srcAddr + 16);  // r
	dst->color[1] = ReadMacFloat(srcAddr + 20);  // g
	dst->color[2] = ReadMacFloat(srcAddr + 24);  // b
	dst->color[3] = ReadMacFloat(srcAddr + 28);  // alpha (always needed)
	SetTextureFactors(dst,
	                  ReadMacFloat(srcAddr + 40), ReadMacFloat(srcAddr + 44), ReadMacFloat(srcAddr + 48),
	                  ReadMacFloat(srcAddr + 52), ReadMacFloat(srcAddr + 56), ReadMacFloat(srcAddr + 60));

	// Perspective-correct UV data
	// RAVE/QD3D V=0 maps to the first row of the TQAImage pixel buffer.
	// Metal textures also store row 0 at the top.  However, the QD3D
	// Interactive Renderer was originally backed by ATI's OpenGL-based
	// RAVE engine, where textures are stored with row 0 at the *bottom*
	// (OpenGL convention).  QD3D's IR therefore emits V coordinates in
	// OpenGL convention: V=0 = bottom of texture, V=1 = top.
	// Since our Metal textures have row 0 at the top, we must flip V:
	//   v_metal = 1 - v_gl
	// In perspective-divided form: vOverW_flipped = invW - vOverW
	float uOverW = ReadMacFloat(srcAddr + 32);
	float vOverW = ReadMacFloat(srcAddr + 36);
	dst->uv[0] = uOverW;
	dst->uv[1] = invW - vOverW;  // flip V for Metal's top-left origin
	dst->uv[2] = invW;                         // for fragment shader division
	dst->uv[3] = 0.0f;
}


/*
 *  NativeDrawTriTexture
 *
 *  DrawTriTexture(drawContext, v0, v1, v2, flags)
 *  PPC args: r3=drawContextAddr, r4=v0Addr, r5=v1Addr, r6=v2Addr, r7=flags
 *
 *  Renders a single textured triangle with perspective-correct UVs.
 *  TQAVTexture vertices are 64 bytes each.
 */
int32 NativeDrawTriTexture(uint32 drawContextAddr, uint32 v0Addr, uint32 v1Addr,
                            uint32 v2Addr, uint32 flags)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal || !priv->metal->renderPassActive) return kQANoErr;

	// kQATriFlags_Backfacing (bit 0) is a hint for Z-buffer disambiguation only.
	// The QD3D IR culls backfacing triangles itself before sending to RAVE.
	(void)flags;

	RaveMetalState *ms = priv->metal;
	bool perspZ = (priv->state[10].i != 0);

	RaveVertex verts[3];
	ConvertTextureVertex(v0Addr, &verts[0], perspZ);
	ConvertTextureVertex(v1Addr, &verts[1], perspZ);
	ConvertTextureVertex(v2Addr, &verts[2], perspZ);
	RaveTrackTextureDraw(priv);

	// Z-sorted transparency: buffer instead of drawing immediately
	if (priv->state[29].i == 1) {  // kQATag_ZSortedHint
		BufferZSortTriangle(priv, &verts[0], &verts[1], &verts[2], true);
		return kQANoErr;
	}

	ApplyDirtyState(priv, false, true);  // textured=true

	[ms->currentEncoder setVertexBytes:verts length:sizeof(verts) atIndex:0];

	// Bind UV2 for multi-texture (DrawTriTexture draws 3 vertices per call,
	// multi-texture UV2 only applies if staging count matches)
	if (priv->multiTextureActive && priv->multiTexStagingCount >= 3) {
		size_t multiTexSize = 3 * 16;  // 4 floats per vertex
		if (!RaveSetVertexData(ms, priv->multiTexStagingBuffer, multiTexSize, 3))
			return kQANoErr;
	}

	[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	ms->drawCallCount++;
	ms->triangleCount += 1;
	ms->triTextureCount++;

	return kQANoErr;
}


/*
 *  NativeDrawVGouraud
 *
 *  DrawVGouraud(drawContext, nVertices, vertexMode, vertices[], flags[])
 *  PPC args: r3=drawContextAddr, r4=nVertices, r5=vertexMode, r6=verticesAddr, r7=flagsAddr
 *
 *  verticesAddr: Mac address of TQAVGouraud[] array (each 32 bytes)
 *  flagsAddr: Mac address of uint32[] per-triangle flags array
 *             May be NULL for points/lines. For triangles, one flags entry per triangle.
 */
int32 NativeDrawVGouraud(uint32 drawContextAddr, uint32 nVertices, uint32 vertexMode,
                          uint32 verticesAddr, uint32 flagsAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal || !priv->metal->renderPassActive) return kQANoErr;
	if (nVertices == 0) return kQANoErr;

	RaveMetalState *ms = priv->metal;
	ms->drawCallCount++;
	ms->vGouraudCount++;
	uint32 primitiveCount = RaveVertexPrimitiveCount(nVertices, vertexMode);
	switch (vertexMode) {
	case 0: ms->pointCount += primitiveCount; break;
	case 1:
	case 2: ms->lineCount += primitiveCount; break;
	case 3:
	case 4:
	case 5: ms->triangleCount += primitiveCount; break;
	default: break;
	}
	bool perspZ = (priv->state[10].i != 0);

	ApplyDirtyState(priv, false);

	switch (vertexMode) {
	case 0: // kQAVertexMode_Point
	{
		RaveVertex *verts = (RaveVertex *)RaveDrawScratchEnsure(&priv->drawScratchA, &priv->drawScratchACap, (nVertices) * sizeof(RaveVertex));
		for (uint32 i = 0; i < nVertices; i++) {
			ConvertGouraudVertex(verticesAddr + i * 32, &verts[i], perspZ);
		}
		size_t dataSize = nVertices * sizeof(RaveVertex);
		if (dataSize > 4096) {
			uint32_t ringOffset = 0;
			void *ringBuf = gfxaccel_rave_ring_stage(verts, (uint32_t)dataSize, &ringOffset);
			if (ringBuf) {
				[ms->currentEncoder setVertexBuffer:(__bridge id<MTLBuffer>)ringBuf offset:ringOffset atIndex:0];
			} else {
				break;
			}
		} else {
			[ms->currentEncoder setVertexBytes:verts length:dataSize atIndex:0];
		}
		[ms->currentEncoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:nVertices];
		break;
	}

	case 1: // kQAVertexMode_Line
	{
		uint32 nLines = nVertices / 2;
		if (nLines == 0) break;
		uint32 count = nLines * 2;
		RaveVertex *verts = (RaveVertex *)RaveDrawScratchEnsure(&priv->drawScratchA, &priv->drawScratchACap, (count) * sizeof(RaveVertex));
		for (uint32 i = 0; i < count; i++) {
			ConvertGouraudVertex(verticesAddr + i * 32, &verts[i], perspZ);
		}
		size_t dataSize = count * sizeof(RaveVertex);
		if (dataSize > 4096) {
			uint32_t ringOffset = 0;
			void *ringBuf = gfxaccel_rave_ring_stage(verts, (uint32_t)dataSize, &ringOffset);
			if (ringBuf) {
				[ms->currentEncoder setVertexBuffer:(__bridge id<MTLBuffer>)ringBuf offset:ringOffset atIndex:0];
			} else {
				break;
			}
		} else {
			[ms->currentEncoder setVertexBytes:verts length:dataSize atIndex:0];
		}
		[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:count];
		break;
	}

	case 2: // kQAVertexMode_Polyline
	{
		if (nVertices < 2) break;
		RaveVertex *verts = (RaveVertex *)RaveDrawScratchEnsure(&priv->drawScratchA, &priv->drawScratchACap, (nVertices) * sizeof(RaveVertex));
		for (uint32 i = 0; i < nVertices; i++) {
			ConvertGouraudVertex(verticesAddr + i * 32, &verts[i], perspZ);
		}
		size_t dataSize = nVertices * sizeof(RaveVertex);
		if (dataSize > 4096) {
			uint32_t ringOffset = 0;
			void *ringBuf = gfxaccel_rave_ring_stage(verts, (uint32_t)dataSize, &ringOffset);
			if (ringBuf) {
				[ms->currentEncoder setVertexBuffer:(__bridge id<MTLBuffer>)ringBuf offset:ringOffset atIndex:0];
			} else {
				break;
			}
		} else {
			[ms->currentEncoder setVertexBytes:verts length:dataSize atIndex:0];
		}
		[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeLineStrip vertexStart:0 vertexCount:nVertices];
		break;
	}

	case 3: // kQAVertexMode_Tri (triangle list)
	{
		uint32 nTris = nVertices / 3;
		if (nTris == 0) break;

		// kQATriFlags_Backfacing is a hint only (RAVE spec p.1566) -- do not cull.
		// The QD3D IR handles backface culling before sending triangles to RAVE.
		uint32 totalVerts = nTris * 3;
		RaveVertex *allVerts = (RaveVertex *)RaveDrawScratchEnsure(&priv->drawScratchA, &priv->drawScratchACap, (totalVerts) * sizeof(RaveVertex));
		for (uint32 i = 0; i < totalVerts; i++) {
			ConvertGouraudVertex(verticesAddr + i * 32, &allVerts[i], perspZ);
		}

		size_t dataSize = totalVerts * sizeof(RaveVertex);
		if (dataSize > 4096) {
			uint32_t ringOffset = 0;
			void *ringBuf = gfxaccel_rave_ring_stage(allVerts, (uint32_t)dataSize, &ringOffset);
			if (ringBuf) {
				[ms->currentEncoder setVertexBuffer:(__bridge id<MTLBuffer>)ringBuf offset:ringOffset atIndex:0];
			} else {
				break;
			}
		} else {
			[ms->currentEncoder setVertexBytes:allVerts length:dataSize atIndex:0];
		}
		[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:totalVerts];
		break;
	}

	case 4: // kQAVertexMode_Strip
	{
		if (nVertices < 3) break;

		// kQATriFlags_Backfacing is a hint only -- do not cull.
		RaveVertex *allVerts = (RaveVertex *)RaveDrawScratchEnsure(&priv->drawScratchA, &priv->drawScratchACap, (nVertices) * sizeof(RaveVertex));
		for (uint32 i = 0; i < nVertices; i++) {
			ConvertGouraudVertex(verticesAddr + i * 32, &allVerts[i], perspZ);
		}

		size_t dataSize = nVertices * sizeof(RaveVertex);
		if (dataSize > 4096) {
			uint32_t ringOffset = 0;
			void *ringBuf = gfxaccel_rave_ring_stage(allVerts, (uint32_t)dataSize, &ringOffset);
			if (ringBuf) {
				[ms->currentEncoder setVertexBuffer:(__bridge id<MTLBuffer>)ringBuf offset:ringOffset atIndex:0];
			} else {
				break;
			}
		} else {
			[ms->currentEncoder setVertexBytes:allVerts length:dataSize atIndex:0];
		}
		[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:nVertices];
		break;
	}

	case 5: // kQAVertexMode_Fan
	{
		if (nVertices < 3) break;
		uint32 nTris = nVertices - 2;

		// kQATriFlags_Backfacing is a hint only -- do not cull.
		// Metal has no triangle fan primitive, so expand to triangle list.
		RaveVertex *allVerts = (RaveVertex *)RaveDrawScratchEnsure(&priv->drawScratchA, &priv->drawScratchACap, (nVertices) * sizeof(RaveVertex));
		for (uint32 i = 0; i < nVertices; i++) {
			ConvertGouraudVertex(verticesAddr + i * 32, &allVerts[i], perspZ);
		}

		RaveVertex *expanded = (RaveVertex *)RaveDrawScratchEnsure(&priv->drawScratchB, &priv->drawScratchBCap, (nTris * 3) * sizeof(RaveVertex));
		uint32 outIdx = 0;
		for (uint32 t = 0; t < nTris; t++) {
			expanded[outIdx++] = allVerts[0];
			expanded[outIdx++] = allVerts[t + 1];
			expanded[outIdx++] = allVerts[t + 2];
		}
		size_t dataSize = outIdx * sizeof(RaveVertex);
		if (dataSize > 4096) {
			uint32_t ringOffset = 0;
			void *ringBuf = gfxaccel_rave_ring_stage(expanded, (uint32_t)dataSize, &ringOffset);
			if (ringBuf) {
				[ms->currentEncoder setVertexBuffer:(__bridge id<MTLBuffer>)ringBuf offset:ringOffset atIndex:0];
			} else {
				break;
			}
		} else {
			[ms->currentEncoder setVertexBytes:expanded length:dataSize atIndex:0];
		}
		[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:outIdx];
		break;
	}

	default:
		RAVE_VLOG("DrawVGouraud: unknown vertexMode %d", vertexMode);
		break;
	}

	return kQANoErr;
}


/*
 *  NativeSubmitVerticesGouraud
 *
 *  SubmitVerticesGouraud(drawContext, nVertices, vertices[])
 *  PPC args: r3=drawContextAddr, r4=nVertices, r5=verticesAddr
 *  Copies and byte-swaps vertices into per-context staging buffer.
 *  Replaces buffer contents (spec behavior).
 */
int32 NativeSubmitVerticesGouraud(uint32 drawContextAddr, uint32 nVertices, uint32 verticesAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv) return kQANoErr;

	if (nVertices > priv->vertexStagingCapacity) {
		RAVE_VLOG("SubmitVerticesGouraud: %d vertices exceeds capacity %d, clamping",
		         nVertices, priv->vertexStagingCapacity);
		nVertices = priv->vertexStagingCapacity;
	}

	bool perspZ = (priv->state[10].i != 0);
	RaveVertex *dst = (RaveVertex *)priv->vertexStagingBuffer;

	for (uint32 i = 0; i < nVertices; i++) {
		ConvertGouraudVertex(verticesAddr + i * 32, &dst[i], perspZ);
	}
	priv->vertexStagingCount = nVertices;

	// Diagnostic: dump vertex bounds for first 10 frames of each context
	if (priv->frameCount <= 10 && nVertices > 0) {
		float minX = dst[0].pos[0], maxX = dst[0].pos[0];
		float minY = dst[0].pos[1], maxY = dst[0].pos[1];
		float minZ = dst[0].pos[2], maxZ = dst[0].pos[2];
		float minA = dst[0].color[3], maxA = dst[0].color[3];
		float minR = dst[0].color[0], maxR = dst[0].color[0];
		float minG = dst[0].color[1], maxG = dst[0].color[1];
		float minB = dst[0].color[2], maxB = dst[0].color[2];
		for (uint32 i = 1; i < nVertices; i++) {
			if (dst[i].pos[0] < minX) minX = dst[i].pos[0];
			if (dst[i].pos[0] > maxX) maxX = dst[i].pos[0];
			if (dst[i].pos[1] < minY) minY = dst[i].pos[1];
			if (dst[i].pos[1] > maxY) maxY = dst[i].pos[1];
			if (dst[i].pos[2] < minZ) minZ = dst[i].pos[2];
			if (dst[i].pos[2] > maxZ) maxZ = dst[i].pos[2];
			if (dst[i].color[3] < minA) minA = dst[i].color[3];
			if (dst[i].color[3] > maxA) maxA = dst[i].color[3];
			if (dst[i].color[0] < minR) minR = dst[i].color[0];
			if (dst[i].color[0] > maxR) maxR = dst[i].color[0];
			if (dst[i].color[1] < minG) minG = dst[i].color[1];
			if (dst[i].color[1] > maxG) maxG = dst[i].color[1];
			if (dst[i].color[2] < minB) minB = dst[i].color[2];
			if (dst[i].color[2] > maxB) maxB = dst[i].color[2];
		}
		RAVE_VLOG("SubmitVGour: f=%u n=%u x=[%.1f,%.1f] y=[%.1f,%.1f] z=[%.3f,%.3f] a=[%.2f,%.2f] rgb=[%.2f-%.2f,%.2f-%.2f,%.2f-%.2f] perspZ=%d",
		         priv->frameCount, nVertices,
		         minX, maxX, minY, maxY, minZ, maxZ, minA, maxA,
		         minR, maxR, minG, maxG, minB, maxB,
		         perspZ ? 1 : 0);
	}

	return kQANoErr;
}


/*
 *  NativeDrawVTexture
 *
 *  DrawVTexture(drawContext, nVertices, vertexMode, vertices[], flags[])
 *  PPC args: r3=drawContextAddr, r4=nVertices, r5=vertexMode, r6=verticesAddr, r7=flagsAddr
 *
 *  verticesAddr: Mac address of TQAVTexture[] array (each 64 bytes)
 *  flagsAddr: Mac address of uint32[] per-triangle flags array
 *             May be NULL for points/lines. For triangles, one flags entry per triangle.
 */
int32 NativeDrawVTexture(uint32 drawContextAddr, uint32 nVertices, uint32 vertexMode,
                          uint32 verticesAddr, uint32 flagsAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal || !priv->metal->renderPassActive) return kQANoErr;
	if (nVertices == 0) return kQANoErr;

	RaveMetalState *ms = priv->metal;
	ms->drawCallCount++;
	ms->vTextureCount++;
	RaveTrackTextureDraw(priv);

	uint32 primitiveCount = RaveVertexPrimitiveCount(nVertices, vertexMode);
	switch (vertexMode) {
	case 0: ms->pointCount += primitiveCount; break;
	case 1:
	case 2: ms->lineCount += primitiveCount; break;
	case 3:
	case 4:
	case 5: ms->triangleCount += primitiveCount; break;
	default: break;
	}
	bool perspZ = (priv->state[10].i != 0);

	ApplyDirtyState(priv, false, true);  // textured=true

	// Bind UV2 staging buffer for multi-texture at buffer index 3
	if (priv->multiTextureActive && priv->multiTexStagingCount > 0) {
		size_t multiTexSize = priv->multiTexStagingCount * 16;  // 4 floats per vertex
		if (!RaveSetVertexData(ms, priv->multiTexStagingBuffer, multiTexSize, 3))
			return kQANoErr;
	}

	switch (vertexMode) {
	case 0: // kQAVertexMode_Point
	{
		RaveVertex *verts = (RaveVertex *)RaveDrawScratchEnsure(&priv->drawScratchA, &priv->drawScratchACap, (nVertices) * sizeof(RaveVertex));
		for (uint32 i = 0; i < nVertices; i++) {
			ConvertTextureVertex(verticesAddr + i * 64, &verts[i], perspZ);
		}
		size_t dataSize = nVertices * sizeof(RaveVertex);
		if (dataSize > 4096) {
			uint32_t ringOffset = 0;
			void *ringBuf = gfxaccel_rave_ring_stage(verts, (uint32_t)dataSize, &ringOffset);
			if (ringBuf) {
				[ms->currentEncoder setVertexBuffer:(__bridge id<MTLBuffer>)ringBuf offset:ringOffset atIndex:0];
			} else {
				break;
			}
		} else {
			[ms->currentEncoder setVertexBytes:verts length:dataSize atIndex:0];
		}
		[ms->currentEncoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:nVertices];
		break;
	}

	case 1: // kQAVertexMode_Line
	{
		uint32 nLines = nVertices / 2;
		if (nLines == 0) break;
		uint32 count = nLines * 2;
		RaveVertex *verts = (RaveVertex *)RaveDrawScratchEnsure(&priv->drawScratchA, &priv->drawScratchACap, (count) * sizeof(RaveVertex));
		for (uint32 i = 0; i < count; i++) {
			ConvertTextureVertex(verticesAddr + i * 64, &verts[i], perspZ);
		}
		size_t dataSize = count * sizeof(RaveVertex);
		if (dataSize > 4096) {
			uint32_t ringOffset = 0;
			void *ringBuf = gfxaccel_rave_ring_stage(verts, (uint32_t)dataSize, &ringOffset);
			if (ringBuf) {
				[ms->currentEncoder setVertexBuffer:(__bridge id<MTLBuffer>)ringBuf offset:ringOffset atIndex:0];
			} else {
				break;
			}
		} else {
			[ms->currentEncoder setVertexBytes:verts length:dataSize atIndex:0];
		}
		[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:count];
		break;
	}

	case 2: // kQAVertexMode_Polyline
	{
		if (nVertices < 2) break;
		RaveVertex *verts = (RaveVertex *)RaveDrawScratchEnsure(&priv->drawScratchA, &priv->drawScratchACap, (nVertices) * sizeof(RaveVertex));
		for (uint32 i = 0; i < nVertices; i++) {
			ConvertTextureVertex(verticesAddr + i * 64, &verts[i], perspZ);
		}
		size_t dataSize = nVertices * sizeof(RaveVertex);
		if (dataSize > 4096) {
			uint32_t ringOffset = 0;
			void *ringBuf = gfxaccel_rave_ring_stage(verts, (uint32_t)dataSize, &ringOffset);
			if (ringBuf) {
				[ms->currentEncoder setVertexBuffer:(__bridge id<MTLBuffer>)ringBuf offset:ringOffset atIndex:0];
			} else {
				break;
			}
		} else {
			[ms->currentEncoder setVertexBytes:verts length:dataSize atIndex:0];
		}
		[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeLineStrip vertexStart:0 vertexCount:nVertices];
		break;
	}

	case 3: // kQAVertexMode_Tri (triangle list)
	{
		uint32 nTris = nVertices / 3;
		if (nTris == 0) break;

		// kQATriFlags_Backfacing is a hint only (RAVE spec p.1566) -- do not cull.
		uint32 totalVerts = nTris * 3;
		RaveVertex *allVerts = (RaveVertex *)RaveDrawScratchEnsure(&priv->drawScratchA, &priv->drawScratchACap, (totalVerts) * sizeof(RaveVertex));
		for (uint32 i = 0; i < totalVerts; i++) {
			ConvertTextureVertex(verticesAddr + i * 64, &allVerts[i], perspZ);
		}

		size_t dataSize = totalVerts * sizeof(RaveVertex);
		if (dataSize > 4096) {
			uint32_t ringOffset = 0;
			void *ringBuf = gfxaccel_rave_ring_stage(allVerts, (uint32_t)dataSize, &ringOffset);
			if (ringBuf) {
				[ms->currentEncoder setVertexBuffer:(__bridge id<MTLBuffer>)ringBuf offset:ringOffset atIndex:0];
			} else {
				break;
			}
		} else {
			[ms->currentEncoder setVertexBytes:allVerts length:dataSize atIndex:0];
		}
		[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:totalVerts];
		break;
	}

	case 4: // kQAVertexMode_Strip
	{
		if (nVertices < 3) break;

		// kQATriFlags_Backfacing is a hint only -- do not cull.
		RaveVertex *allVerts = (RaveVertex *)RaveDrawScratchEnsure(&priv->drawScratchA, &priv->drawScratchACap, (nVertices) * sizeof(RaveVertex));
		for (uint32 i = 0; i < nVertices; i++) {
			ConvertTextureVertex(verticesAddr + i * 64, &allVerts[i], perspZ);
		}

		size_t dataSize = nVertices * sizeof(RaveVertex);
		if (dataSize > 4096) {
			uint32_t ringOffset = 0;
			void *ringBuf = gfxaccel_rave_ring_stage(allVerts, (uint32_t)dataSize, &ringOffset);
			if (ringBuf) {
				[ms->currentEncoder setVertexBuffer:(__bridge id<MTLBuffer>)ringBuf offset:ringOffset atIndex:0];
			} else {
				break;
			}
		} else {
			[ms->currentEncoder setVertexBytes:allVerts length:dataSize atIndex:0];
		}
		[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:nVertices];
		break;
	}

	case 5: // kQAVertexMode_Fan
	{
		if (nVertices < 3) break;
		uint32 nTris = nVertices - 2;

		// kQATriFlags_Backfacing is a hint only -- do not cull.
		// Metal has no triangle fan primitive, so expand to triangle list.
		RaveVertex *allVerts = (RaveVertex *)RaveDrawScratchEnsure(&priv->drawScratchA, &priv->drawScratchACap, (nVertices) * sizeof(RaveVertex));
		for (uint32 i = 0; i < nVertices; i++) {
			ConvertTextureVertex(verticesAddr + i * 64, &allVerts[i], perspZ);
		}

		RaveVertex *expanded = (RaveVertex *)RaveDrawScratchEnsure(&priv->drawScratchB, &priv->drawScratchBCap, (nTris * 3) * sizeof(RaveVertex));
		uint32 outIdx = 0;
		for (uint32 t = 0; t < nTris; t++) {
			expanded[outIdx++] = allVerts[0];
			expanded[outIdx++] = allVerts[t + 1];
			expanded[outIdx++] = allVerts[t + 2];
		}
		size_t dataSize = outIdx * sizeof(RaveVertex);
		if (dataSize > 4096) {
			uint32_t ringOffset = 0;
			void *ringBuf = gfxaccel_rave_ring_stage(expanded, (uint32_t)dataSize, &ringOffset);
			if (ringBuf) {
				[ms->currentEncoder setVertexBuffer:(__bridge id<MTLBuffer>)ringBuf offset:ringOffset atIndex:0];
			} else {
				break;
			}
		} else {
			[ms->currentEncoder setVertexBytes:expanded length:dataSize atIndex:0];
		}
		[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:outIdx];
		break;
	}

	default:
		RAVE_VLOG("DrawVTexture: unknown vertexMode %d", vertexMode);
		break;
	}

	return kQANoErr;
}


/*
 *  NativeSubmitVerticesTexture
 *
 *  SubmitVerticesTexture(drawContext, nVertices, vertices[])
 *  PPC args: r3=drawContextAddr, r4=nVertices, r5=verticesAddr
 *  Copies and byte-swaps textured vertices into per-context staging buffer.
 *  TQAVTexture is 64 bytes (16 floats) per vertex.
 */
int32 NativeSubmitVerticesTexture(uint32 drawContextAddr, uint32 nVertices, uint32 verticesAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv) return kQANoErr;

	if (nVertices > priv->vertexStagingCapacity) {
		RAVE_VLOG("SubmitVerticesTexture: %d vertices exceeds capacity %d, clamping",
		         nVertices, priv->vertexStagingCapacity);
		nVertices = priv->vertexStagingCapacity;
	}

	bool perspZ = (priv->state[10].i != 0);
	RaveVertex *dst = (RaveVertex *)priv->vertexStagingBuffer;

	for (uint32 i = 0; i < nVertices; i++) {
		ConvertTextureVertex(verticesAddr + i * 64, &dst[i], perspZ);
	}
	priv->vertexStagingCount = nVertices;

	return kQANoErr;
}


/*
 *  NativeSubmitMultiTextureParams
 *
 *  SubmitMultiTextureVertex(drawContext, nVertices, multiTexParams[])
 *  PPC args: r3=drawContextAddr, r4=nVertices, r5=multiTexParamsAddr
 *
 *  RAVE 1.6 multi-texture extension. Receives per-vertex UV data for a
 *  second texture layer, stored in a parallel staging buffer alongside the
 *  primary vertex staging buffer. The struct layout is speculative based on
 *  the RAVE 1.6 DDK specification:
 *
 *  TQAVMultiTexture (12 bytes per vertex, per RAVE 1.6 spec):
 *    0: invW    (float) - inverse W for perspective correction
 *    4: uOverW  (float) - perspective-correct U for second texture
 *    8: vOverW  (float) - perspective-correct V for second texture
 *
 *  The texture handle for the second layer is set via kQATag_MultiTexture
 *  while kQATag_MultiTextureCurrent selects which layer the state applies to.
 */
int32 NativeSubmitMultiTextureParams(uint32 drawContextAddr, uint32 nVertices, uint32 multiTexParamsAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv) return kQANoErr;

	if (nVertices == 0 || multiTexParamsAddr == 0) return kQANoErr;

	uint32_t currentLayer = priv->state[34].i;  // kQATag_MultiTextureCurrent
	uint32_t enabledLayers = priv->state[33].i; // kQATag_MultiTextureEnable
	if (enabledLayers < 2 || currentLayer != 1) {
		priv->multiTexStagingCount = 0;
		priv->multiTextureActive = false;
		priv->multiTextureHandle = 0;
		RAVE_VLOG("SubmitMultiTex: ignored layer=%u enabled=%u (only secondary layer 1 is supported)",
		         currentLayer, enabledLayers);
		return kQANoErr;
	}

	// TQAVMultiTexture: 12 bytes per vertex (invW, uOverW, vOverW) per RAVE 1.6 spec
	static const uint32 kMultiTexStride = 12;

	// Cap to staging buffer capacity
	uint32 maxVerts = priv->vertexStagingCapacity;
	if (nVertices > maxVerts) nVertices = maxVerts;

	// Extract per-vertex UV2 data into multi-texture staging buffer
	// Staging buffer layout: 16 bytes per vertex (uOverW2, vOverW2, invW2, pad)
	float *dst = (float *)priv->multiTexStagingBuffer;

	for (uint32 i = 0; i < nVertices; i++) {
		uint32 srcAddr = multiTexParamsAddr + i * kMultiTexStride;
		float invW    = ReadMacFloat(srcAddr + 0);   // invW is FIRST in RAVE 1.6 spec
		float uOverW  = ReadMacFloat(srcAddr + 4);   // uOverW second
		float vOverW  = ReadMacFloat(srcAddr + 8);   // vOverW third

		// Store as (uOverW2, vOverW2, invW2, 0) for shader consumption
		// Flip V for Metal's top-left texture origin (same as primary UV path)
		dst[i * 4 + 0] = uOverW;
		dst[i * 4 + 1] = invW - vOverW;  // flip V
		dst[i * 4 + 2] = invW;
		dst[i * 4 + 3] = 0.0f;
	}

	priv->multiTexStagingCount = nVertices;
	priv->multiTextureHandle = priv->state[26].i;     // kQATag_MultiTexture
	priv->multiTextureOp = priv->state[35].i;         // kQATag_MultiTextureOp
	priv->multiTextureFactor = priv->state[51].f;     // kQATag_MultiTextureFactor
	priv->multiTextureActive = (priv->multiTextureHandle != 0);

	RAVE_VLOG("SubmitMultiTex: stored %u verts, layer=%u enabled=%u texHandle=0x%08x",
	         nVertices, currentLayer, enabledLayers, priv->multiTextureHandle);

	return kQANoErr;
}


/*
 *  NativeDrawBitmap
 *
 *  DrawBitmap(drawContext, vertex, bitmap)
 *  PPC args: r3=drawContextAddr, r4=vertexAddr, r5=bitmapMacAddr
 *
 *  Renders a screen-aligned textured quad for 2D sprites and HUD elements.
 *  The single TQAVGouraud vertex (32 bytes) provides screen position (x,y) and alpha.
 *  The bitmap Mac address identifies the resource whose Metal texture is used.
 *  Depth test is disabled (z=0, ZFunction_Always) so HUD renders on top.
 */
int32 NativeDrawBitmap(uint32 drawContextAddr, uint32 vertexAddr, uint32 bitmapMacAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal || !priv->metal->renderPassActive) return kQANoErr;

	RaveMetalState *ms = priv->metal;

	// Look up bitmap resource by Mac address
	uint32_t bmpHandle = RaveResourceFindByAddr(bitmapMacAddr);
	if (bmpHandle == 0) {
		RAVE_LOG("DrawBitmap: bitmap not found at 0x%08x", bitmapMacAddr);
		return kQANoErr;
	}
	RaveResourceEntry *bmpEntry = RaveResourceGet(bmpHandle);
	if (!bmpEntry) {
		RAVE_LOG("DrawBitmap: bitmap not found (handle=%d)", bmpHandle);
		return kQANoErr;
	}
	if (!bmpEntry->metal_texture && bmpEntry->pixmap_mac_addr != 0) {
		RaveRealizeDeferredTexture(bmpEntry);
	}
	if (!bmpEntry->metal_texture) {
		RAVE_LOG("DrawBitmap: bitmap has no Metal texture (handle=%d)", bmpHandle);
		return kQANoErr;
	}

	// Read position and alpha from TQAVGouraud vertex (32 bytes)
	float x = ReadMacFloat(vertexAddr + 0);
	float y = ReadMacFloat(vertexAddr + 4);
	float alpha = ReadMacFloat(vertexAddr + 28);  // vertex alpha

	float w = (float)bmpEntry->width;
	float h = (float)bmpEntry->height;

	// Apply bitmap scale factors (tags 52 and 53, default 1.0)
	float scaleX = priv->state[52].f;
	float scaleY = priv->state[53].f;
	if (scaleX <= 0.0f) scaleX = 1.0f;  // guard against zero/negative
	if (scaleY <= 0.0f) scaleY = 1.0f;
	w *= scaleX;
	h *= scaleY;

	// Generate 6 vertices for screen-aligned quad (2 triangles)
	// Depth test disabled: z=0 (front), w=1
	// UV is simple 0-1 mapping, no perspective correction needed (invW=1)
	RaveVertex verts[6];

	// Triangle 1: top-left, top-right, bottom-left
	// v0: top-left (x, y)
	verts[0].pos[0] = x;     verts[0].pos[1] = y;
	verts[0].pos[2] = 0.0f;  verts[0].pos[3] = 1.0f;
	verts[0].color[0] = 1.0f; verts[0].color[1] = 1.0f;
	verts[0].color[2] = 1.0f; verts[0].color[3] = alpha;
	verts[0].uv[0] = 0.0f;  verts[0].uv[1] = 0.0f;
	verts[0].uv[2] = 1.0f;  verts[0].uv[3] = 0.0f;  // invW=1 for no perspective

	// v1: top-right (x+w, y)
	verts[1].pos[0] = x + w; verts[1].pos[1] = y;
	verts[1].pos[2] = 0.0f;  verts[1].pos[3] = 1.0f;
	verts[1].color[0] = 1.0f; verts[1].color[1] = 1.0f;
	verts[1].color[2] = 1.0f; verts[1].color[3] = alpha;
	verts[1].uv[0] = 1.0f;  verts[1].uv[1] = 0.0f;
	verts[1].uv[2] = 1.0f;  verts[1].uv[3] = 0.0f;

	// v2: bottom-left (x, y+h)
	verts[2].pos[0] = x;     verts[2].pos[1] = y + h;
	verts[2].pos[2] = 0.0f;  verts[2].pos[3] = 1.0f;
	verts[2].color[0] = 1.0f; verts[2].color[1] = 1.0f;
	verts[2].color[2] = 1.0f; verts[2].color[3] = alpha;
	verts[2].uv[0] = 0.0f;  verts[2].uv[1] = 1.0f;
	verts[2].uv[2] = 1.0f;  verts[2].uv[3] = 0.0f;

	// Triangle 2: top-right, bottom-right, bottom-left
	verts[3] = verts[1];  // top-right

	// v4: bottom-right (x+w, y+h)
	verts[4].pos[0] = x + w; verts[4].pos[1] = y + h;
	verts[4].pos[2] = 0.0f;  verts[4].pos[3] = 1.0f;
	verts[4].color[0] = 1.0f; verts[4].color[1] = 1.0f;
	verts[4].color[2] = 1.0f; verts[4].color[3] = alpha;
	verts[4].uv[0] = 1.0f;  verts[4].uv[1] = 1.0f;
	verts[4].uv[2] = 1.0f;  verts[4].uv[3] = 0.0f;

	verts[5] = verts[2];  // bottom-left
	for (int i = 0; i < 6; i++) {
		SetTextureFactors(&verts[i], 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f);
	}

	// Select textured pipeline with current blend mode (default to Interpolate for bitmaps)
	int blend_mode = (int)priv->state[9].i;
	if (blend_mode < 0 || blend_mode > 2) blend_mode = 1;  // default Interpolate for bitmaps
	int pipe_idx = 1 + blend_mode * 16;  // has_texture=1

	// Apply channel mask in bitmap draw path (same as ApplyDirtyState)
	uint32_t channelMask = RaveNormalizeChannelMask(priv->state[27].i);  // kQATag_ChannelMask
	const bool hasDepthAttachment = RaveContextUsesMetalDepthAttachment(priv->flags) != 0;
	if (channelMask != 0xF) {
		id<MTLRenderPipelineState> maskedPipe = GetMaskedPipeline(
			ms, pipe_idx, channelMask, ms->msaaActive, hasDepthAttachment);
		if (maskedPipe)
			[ms->currentEncoder setRenderPipelineState:maskedPipe];
	} else {
		id<MTLRenderPipelineState> __strong *pipeArray = ms->msaaActive ? ms->msaaPipelines : ms->pipelines;
		if (pipe_idx >= 0 && pipe_idx < 48 && pipeArray[pipe_idx]) {
			[ms->currentEncoder setRenderPipelineState:pipeArray[pipe_idx]];
		}
	}

	// Disable depth test for bitmap (ZFunction_Always = 7, no depth writes)
	if (hasDepthAttachment && ms->depthStatesNoWrite[7]) {
		[ms->currentEncoder setDepthStencilState:ms->depthStatesNoWrite[7]];
	}

	// Bind bitmap texture directly (not from state[13])
	id<MTLTexture> bmpTex = (__bridge id<MTLTexture>)bmpEntry->metal_texture;
	[ms->currentEncoder setFragmentTexture:bmpTex atIndex:0];

	// Select sampler based on BitmapFilter (tag 54): 0=nearest, 1=bilinear, 2=trilinear
	int bmpFilter = (int)priv->state[54].i;
	if (bmpFilter < 0 || bmpFilter > 2) bmpFilter = 0;
	[ms->currentEncoder setFragmentSamplerState:ms->samplers[bmpFilter] atIndex:0];

	// Set fragment uniforms for bitmap -- TextureOp=None, no fog/alpha_test
	FragmentUniforms fragUniforms = {};
	fragUniforms.texture_op = 0;
	fragUniforms.fog_max_depth = 1.0f;
	[ms->currentEncoder setFragmentBytes:&fragUniforms length:sizeof(fragUniforms) atIndex:0];

	[ms->currentEncoder setVertexBytes:verts length:sizeof(verts) atIndex:0];
	[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	ms->drawCallCount++;
	ms->bitmapCount++;
	ms->triangleCount += 2;

	// Force pipeline re-selection on next draw (we changed pipeline/depth)
	ms->currentPipelineKey = kRavePipelineKeyInvalid;
	priv->dirty_flags |= 1;  // force depth state re-application

	return kQANoErr;
}


/*
 *  NativeDrawTriMeshGouraud
 *
 *  DrawTriMeshGouraud(drawContext, numTriangles, triangles[])
 *  PPC args: r3=drawContextAddr, r4=numTriangles, r5=trianglesAddr
 *
 *  Indexed triangle mesh drawing using previously submitted Gouraud vertices.
 *  trianglesAddr points to an array of TQAIndexedTriangle structs (16 bytes each):
 *    offset 0: triangleFlags (uint32) - bit 0 = backfacing
 *    offset 4: vertices[3] (3 x uint32) - indices into staging buffer
 */
int32 NativeDrawTriMeshGouraud(uint32 drawContextAddr, uint32 numTriangles, uint32 trianglesAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal || !priv->metal->renderPassActive) return kQANoErr;
	if (numTriangles == 0 || priv->vertexStagingCount == 0) return kQANoErr;

	RaveMetalState *ms = priv->metal;
	RaveVertex *staged = (RaveVertex *)priv->vertexStagingBuffer;
	uint32 maxIdx = priv->vertexStagingCount;

	ApplyDirtyState(priv, false);

	// Build triangle list from indexed triangles
	// kQATriFlags_Backfacing (bit 0) is a hint only (RAVE spec p.1566) -- do not cull.
	// The QD3D IR handles backface culling before sending triangles to RAVE.
	RaveVertex *verts = (RaveVertex *)RaveDrawScratchEnsure(&priv->drawScratchA, &priv->drawScratchACap, (numTriangles * 3) * sizeof(RaveVertex));
	uint32 outIdx = 0;
	uint32 oobCount = 0;

	for (uint32 t = 0; t < numTriangles; t++) {
		uint32 triAddr = trianglesAddr + t * 16;
		// flags at offset 0 contain kQATriFlags_Backfacing hint -- ignored

		uint32 i0 = ReadMacInt32(triAddr + 4);
		uint32 i1 = ReadMacInt32(triAddr + 8);
		uint32 i2 = ReadMacInt32(triAddr + 12);

		// Bounds check indices
		if (i0 >= maxIdx || i1 >= maxIdx || i2 >= maxIdx) { oobCount++; continue; }

		verts[outIdx++] = staged[i0];
		verts[outIdx++] = staged[i1];
		verts[outIdx++] = staged[i2];
	}

	// Diagnostic: log draw state for first 5 frames of each context
	if (priv->frameCount <= 10) {
		// Dump first triangle's raw data for struct layout verification
		uint32 raw0 = 0, raw1 = 0, raw2 = 0, raw3 = 0;
		if (numTriangles > 0) {
			raw0 = ReadMacInt32(trianglesAddr + 0);
			raw1 = ReadMacInt32(trianglesAddr + 4);
			raw2 = ReadMacInt32(trianglesAddr + 8);
			raw3 = ReadMacInt32(trianglesAddr + 12);
		}
		RAVE_VLOG("MeshGour: f=%u tris=%u/%u(oob=%u) staged=%u zfunc=%d blend=%d raw0=[%u,%u,%u,%u]",
		         priv->frameCount, outIdx / 3, numTriangles, oobCount,
		         priv->vertexStagingCount,
		         (int)priv->state[0].i, (int)priv->state[9].i,
		         raw0, raw1, raw2, raw3);
	}

	if (outIdx > 0) {
		size_t dataSize = outIdx * sizeof(RaveVertex);
		if (dataSize > 4096) {
			uint32_t ringOffset = 0;
			void *ringBuf = gfxaccel_rave_ring_stage(verts, (uint32_t)dataSize, &ringOffset);
			if (ringBuf) {
				[ms->currentEncoder setVertexBuffer:(__bridge id<MTLBuffer>)ringBuf offset:ringOffset atIndex:0];
			} else {
				return kQANoErr;
			}
		} else {
			[ms->currentEncoder setVertexBytes:verts length:dataSize atIndex:0];
		}
		[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:outIdx];
		ms->drawCallCount++;
		ms->triangleCount += outIdx / 3;
		ms->meshGouraudCount++;
	}

	return kQANoErr;
}


/*
 *  NativeDrawTriMeshTexture
 *
 *  DrawTriMeshTexture(drawContext, numTriangles, triangles[])
 *  PPC args: r3=drawContextAddr, r4=numTriangles, r5=trianglesAddr
 *
 *  Indexed triangle mesh drawing using previously submitted textured vertices.
 *  Same TQAIndexedTriangle struct layout as Gouraud version.
 */
int32 NativeDrawTriMeshTexture(uint32 drawContextAddr, uint32 numTriangles, uint32 trianglesAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal || !priv->metal->renderPassActive) return kQANoErr;
	if (numTriangles == 0 || priv->vertexStagingCount == 0) return kQANoErr;

	RaveMetalState *ms = priv->metal;
	RaveVertex *staged = (RaveVertex *)priv->vertexStagingBuffer;
	uint32 maxIdx = priv->vertexStagingCount;

	ApplyDirtyState(priv, false, true);  // textured=true

	// Build triangle list from indexed triangles
	// kQATriFlags_Backfacing (bit 0) is a hint only (RAVE spec p.1566) -- do not cull.
	// The QD3D IR handles backface culling before sending triangles to RAVE.
	RaveVertex *verts = (RaveVertex *)RaveDrawScratchEnsure(&priv->drawScratchA, &priv->drawScratchACap, (numTriangles * 3) * sizeof(RaveVertex));
	uint32 outIdx = 0;
	uint32 oobCount = 0;

	for (uint32 t = 0; t < numTriangles; t++) {
		uint32 triAddr = trianglesAddr + t * 16;
		// flags at offset 0 contain kQATriFlags_Backfacing hint -- ignored

		uint32 i0 = ReadMacInt32(triAddr + 4);
		uint32 i1 = ReadMacInt32(triAddr + 8);
		uint32 i2 = ReadMacInt32(triAddr + 12);

		// Bounds check indices
		if (i0 >= maxIdx || i1 >= maxIdx || i2 >= maxIdx) { oobCount++; continue; }

		verts[outIdx++] = staged[i0];
		verts[outIdx++] = staged[i1];
		verts[outIdx++] = staged[i2];
	}

	// Diagnostic: log draw state for first 5 frames of each context
	if (priv->frameCount <= 10) {
		uint32 raw0 = 0, raw1 = 0, raw2 = 0, raw3 = 0;
		if (numTriangles > 0) {
			raw0 = ReadMacInt32(trianglesAddr + 0);
			raw1 = ReadMacInt32(trianglesAddr + 4);
			raw2 = ReadMacInt32(trianglesAddr + 8);
			raw3 = ReadMacInt32(trianglesAddr + 12);
		}
		RAVE_VLOG("MeshTex: f=%u tris=%u/%u(oob=%u) staged=%u zfunc=%d zmask=%d blend=%d tex=0x%08x texOp=%d alpha=%d/%.3f fog=%d->%d atiFog=%d fogRGBA=[%.3f,%.3f,%.3f,%.3f] fogRange=[%.3f,%.3f] fogDensity=%.6f fogMax=%.3f filter=%d wrap=%u/%u raw0=[%u,%u,%u,%u]",
		         priv->frameCount, outIdx / 3, numTriangles, oobCount,
		         priv->vertexStagingCount,
		         (int)priv->state[0].i, (int)priv->state[28].i,
		         (int)priv->state[9].i,
		         priv->state[13].i, (int)priv->state[12].i,
		         (int)priv->state[31].i, priv->state[46].f,
		         (int)priv->state[17].i, (int)RaveEffectiveFogMode(priv),
		         priv->ati_fog_active ? 1 : 0,
		         priv->state[18].f, priv->state[19].f, priv->state[20].f, priv->state[21].f,
		         priv->state[22].f, priv->state[23].f, priv->state[24].f, priv->state[25].f,
		         (int)priv->state[11].i,
		         priv->state[101].i, priv->state[102].i,
		         raw0, raw1, raw2, raw3);
	}

	if (outIdx > 0) {
		size_t dataSize = outIdx * sizeof(RaveVertex);
		if (dataSize > 4096) {
			uint32_t ringOffset = 0;
			void *ringBuf = gfxaccel_rave_ring_stage(verts, (uint32_t)dataSize, &ringOffset);
			if (ringBuf) {
				[ms->currentEncoder setVertexBuffer:(__bridge id<MTLBuffer>)ringBuf offset:ringOffset atIndex:0];
			} else {
				return kQANoErr;
			}
		} else {
			[ms->currentEncoder setVertexBytes:verts length:dataSize atIndex:0];
		}

		// Bind UV2 for multi-texture: build indexed UV2 from staging buffer
		if (priv->multiTextureActive && priv->multiTexStagingCount > 0) {
			float *uv2Staged = (float *)priv->multiTexStagingBuffer;
			float *uv2Indexed = (float *)RaveDrawScratchEnsure(&priv->drawScratchF, &priv->drawScratchFCap, (outIdx * 4) * sizeof(float));
			uint32 uv2Out = 0;
			for (uint32 t = 0; t < numTriangles; t++) {
				uint32 triAddr = trianglesAddr + t * 16;
				uint32 i0 = ReadMacInt32(triAddr + 4);
				uint32 i1 = ReadMacInt32(triAddr + 8);
				uint32 i2 = ReadMacInt32(triAddr + 12);
				if (i0 >= maxIdx || i1 >= maxIdx || i2 >= maxIdx) continue;
				for (int c = 0; c < 4; c++) uv2Indexed[uv2Out * 4 + c] = (i0 < priv->multiTexStagingCount) ? uv2Staged[i0 * 4 + c] : 0.0f;
				uv2Out++;
				for (int c = 0; c < 4; c++) uv2Indexed[uv2Out * 4 + c] = (i1 < priv->multiTexStagingCount) ? uv2Staged[i1 * 4 + c] : 0.0f;
				uv2Out++;
				for (int c = 0; c < 4; c++) uv2Indexed[uv2Out * 4 + c] = (i2 < priv->multiTexStagingCount) ? uv2Staged[i2 * 4 + c] : 0.0f;
				uv2Out++;
			}
			size_t uv2Size = uv2Out * 16;
			if (!RaveSetVertexData(ms, uv2Indexed, uv2Size, 3)) {
				return kQANoErr;
			}
		}

		[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:outIdx];
		ms->drawCallCount++;
		ms->triangleCount += outIdx / 3;
		ms->meshTextureCount++;
	}

	return kQANoErr;
}


/*
 *  NativeDrawPoint
 *
 *  DrawPoint(drawContext, v0)
 *  PPC args: r3=drawContextAddr, r4=v0Addr
 */
int32 NativeDrawPoint(uint32 drawContextAddr, uint32 v0Addr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal || !priv->metal->renderPassActive) return kQANoErr;

	RaveMetalState *ms = priv->metal;
	bool perspZ = (priv->state[10].i != 0);

	RaveVertex vert;
	ConvertGouraudVertex(v0Addr, &vert, perspZ);

	ApplyDirtyState(priv, false);
	[ms->currentEncoder setVertexBytes:&vert length:sizeof(vert) atIndex:0];
	[ms->currentEncoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:1];
	ms->drawCallCount++;
	ms->pointCount++;

	return kQANoErr;
}


/*
 *  NativeDrawLine
 *
 *  DrawLine(drawContext, v0, v1)
 *  PPC args: r3=drawContextAddr, r4=v0Addr, r5=v1Addr
 *
 *  NOTE: Metal on iOS does not support line width > 1px.
 *  kQATag_Width is ignored for lines. No known QD3D game relies on wide lines.
 */
int32 NativeDrawLine(uint32 drawContextAddr, uint32 v0Addr, uint32 v1Addr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal || !priv->metal->renderPassActive) return kQANoErr;

	RaveMetalState *ms = priv->metal;
	bool perspZ = (priv->state[10].i != 0);

	RaveVertex verts[2];
	ConvertGouraudVertex(v0Addr, &verts[0], perspZ);
	ConvertGouraudVertex(v1Addr, &verts[1], perspZ);

	ApplyDirtyState(priv, false);
	[ms->currentEncoder setVertexBytes:verts length:sizeof(verts) atIndex:0];
	[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:2];
	ms->drawCallCount++;
	ms->lineCount++;

	return kQANoErr;
}


/*
 *  Notice method storage, retrieval, and invocation
 *
 *  Notice methods are PPC callbacks registered by the application for
 *  compositing events. Selectors:
 *    0 = kQAMethod_RenderCompletion      -- after present
 *    1 = kQAMethod_DisplayModeChanged    -- not relevant on iOS
 *    2 = kQAMethod_ReloadTextures        -- not relevant
 *    3 = kQAMethod_ImageBufferInitialize -- at RenderStart
 *    4 = kQAMethod_ImageBuffer2DComposite -- during RenderEnd, before present
 */

static void EndAndCommitCurrentRenderPass(RaveMetalState *ms)
{
	if (ms->currentEncoder) {
		[ms->currentEncoder endEncoding];
		ms->currentEncoder = nil;
	}
	if (ms->currentCommandBuffer) {
		CommitRaveCommandBufferWithRing(ms->currentCommandBuffer);
		[ms->currentCommandBuffer waitUntilCompleted];
		ms->currentCommandBuffer = nil;
	}
}

static bool EnsureDrawBufferCPU(RaveDrawPrivate *priv)
{
	RaveMetalState *ms = priv->metal;
	id<MTLTexture> drawableTexture = ms->overlayTexture;
	if (!drawableTexture || !ms->device || !ms->commandQueue) return false;

	NSUInteger w = drawableTexture.width;
	NSUInteger h = drawableTexture.height;
	if (!ms->stagingDrawBuffer ||
	    ms->stagingDrawBuffer.width != w || ms->stagingDrawBuffer.height != h) {
		MTLTextureDescriptor *desc = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
			                            width:w height:h mipmapped:NO];
		desc.storageMode = MTLStorageModeShared;
		desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
		ms->stagingDrawBuffer = [ms->device newTextureWithDescriptor:desc];
	}

	uint32_t rowBytes = (uint32_t)(w * 4);
	uint32_t bufSize = rowBytes * (uint32_t)h;
	if (!ms->drawBufferCPU || ms->drawBufferCPUSize != bufSize) {
		uint32_t macAddr = Mac_sysalloc(bufSize);
		if (macAddr == 0) return false;
		ms->drawBufferCPU = Mac2HostAddr(macAddr);
		ms->drawBufferCPUMac = macAddr;
		ms->drawBufferCPUSize = bufSize;
	}

	return ms->stagingDrawBuffer != nil && ms->drawBufferCPU != nullptr;
}

static void ConvertBGRA8ToMacRGB32(uint8_t *pixels, NSUInteger w, NSUInteger h, uint32_t rowBytes)
{
	for (NSUInteger y = 0; y < h; y++) {
		uint8_t *row = pixels + y * rowBytes;
		for (NSUInteger x = 0; x < w; x++) {
			uint8_t *p = row + x * 4;
			uint8_t b = p[0];
			uint8_t g = p[1];
			uint8_t r = p[2];
			p[0] = 0;
			p[1] = r;
			p[2] = g;
			p[3] = b;
		}
	}
}

static void ConvertMacRGB32ToBGRA8(const uint8_t *src, uint8_t *dst,
                                    NSUInteger w, NSUInteger h, uint32_t srcRowBytes)
{
	for (NSUInteger y = 0; y < h; y++) {
		const uint8_t *srcRow = src + y * srcRowBytes;
		uint8_t *dstRow = dst + y * w * 4;
		for (NSUInteger x = 0; x < w; x++) {
			const uint8_t *s = srcRow + x * 4;
			uint8_t *d = dstRow + x * 4;
			d[0] = s[3];
			d[1] = s[2];
			d[2] = s[1];
			d[3] = 255;
		}
	}
}

static bool CopyOverlayToDrawBufferCPU(RaveDrawPrivate *priv)
{
	RaveMetalState *ms = priv->metal;
	if (!EnsureDrawBufferCPU(priv)) return false;

	id<MTLTexture> drawableTexture = ms->overlayTexture;
	NSUInteger w = drawableTexture.width;
	NSUInteger h = drawableTexture.height;
	uint32_t rowBytes = (uint32_t)(w * 4);

	id<MTLCommandBuffer> blitCmdBuf = [ms->commandQueue commandBuffer];
	id<MTLBlitCommandEncoder> blit = [blitCmdBuf blitCommandEncoder];
	[blit copyFromTexture:drawableTexture sourceSlice:0 sourceLevel:0
	         sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(w,h,1)
	            toTexture:ms->stagingDrawBuffer destinationSlice:0
	     destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
	[blit endEncoding];
	[blitCmdBuf commit];
	[blitCmdBuf waitUntilCompleted];

	[ms->stagingDrawBuffer getBytes:ms->drawBufferCPU bytesPerRow:rowBytes
	                     fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
	ConvertBGRA8ToMacRGB32(ms->drawBufferCPU, w, h, rowBytes);
	return true;
}

static void GetTQARectUploadRegion(uint32_t rectAddr, NSUInteger texW, NSUInteger texH,
                                   NSUInteger *uploadX, NSUInteger *uploadY,
                                   NSUInteger *uploadW, NSUInteger *uploadH)
{
	*uploadX = 0;
	*uploadY = 0;
	*uploadW = texW;
	*uploadH = texH;

	if (rectAddr == 0) return;

	int32_t dLeft   = (int32_t)ReadMacInt32(rectAddr + 0);
	int32_t dRight  = (int32_t)ReadMacInt32(rectAddr + 4);
	int32_t dTop    = (int32_t)ReadMacInt32(rectAddr + 8);
	int32_t dBottom = (int32_t)ReadMacInt32(rectAddr + 12);

	if (dLeft < 0) dLeft = 0;
	if (dTop < 0) dTop = 0;
	if ((NSUInteger)dRight > texW) dRight = (int32_t)texW;
	if ((NSUInteger)dBottom > texH) dBottom = (int32_t)texH;
	if (dRight > dLeft && dBottom > dTop) {
		*uploadX = (NSUInteger)dLeft;
		*uploadY = (NSUInteger)dTop;
		*uploadW = (NSUInteger)(dRight - dLeft);
		*uploadH = (NSUInteger)(dBottom - dTop);
	}
}

static bool UploadDrawBufferCPUToOverlay(RaveDrawPrivate *priv, uint32_t dirtyRectAddr)
{
	RaveMetalState *ms = priv->metal;
	if (!EnsureDrawBufferCPU(priv)) return false;

	id<MTLTexture> drawableTexture = ms->overlayTexture;
	NSUInteger w = drawableTexture.width;
	NSUInteger h = drawableTexture.height;
	uint32_t rowBytes = (uint32_t)(w * 4);

	NSUInteger uploadX, uploadY, uploadW, uploadH;
	GetTQARectUploadRegion(dirtyRectAddr, w, h, &uploadX, &uploadY, &uploadW, &uploadH);

	const uint8_t *srcBytes = (const uint8_t *)ms->drawBufferCPU + uploadY * rowBytes + uploadX * 4;
	std::vector<uint8_t> bgra(uploadW * uploadH * 4);
	ConvertMacRGB32ToBGRA8(srcBytes, bgra.data(), uploadW, uploadH, rowBytes);

	[ms->stagingDrawBuffer replaceRegion:MTLRegionMake2D(uploadX, uploadY, uploadW, uploadH)
	                         mipmapLevel:0 withBytes:bgra.data()
	                         bytesPerRow:(NSUInteger)(uploadW * 4)];

	id<MTLCommandBuffer> blitCmdBuf = [ms->commandQueue commandBuffer];
	id<MTLBlitCommandEncoder> blit = [blitCmdBuf blitCommandEncoder];
	[blit copyFromTexture:ms->stagingDrawBuffer sourceSlice:0 sourceLevel:0
	         sourceOrigin:MTLOriginMake(uploadX, uploadY, 0) sourceSize:MTLSizeMake(uploadW, uploadH, 1)
	            toTexture:drawableTexture destinationSlice:0
	     destinationLevel:0 destinationOrigin:MTLOriginMake(uploadX, uploadY, 0)];
	[blit endEncoding];
	[blitCmdBuf commit];
	[blitCmdBuf waitUntilCompleted];

	return true;
}

static void FireBufferNoticeMethod(RaveDrawPrivate *priv, int selector,
                                   uint32_t callback, uint32_t refCon)
{
	RaveMetalState *ms = priv->metal;
	if (!ms || !ms->overlayTexture) return;

	bool restartRenderPass = ms->renderPassActive && selector != 4;
	EndAndCommitCurrentRenderPass(ms);

	if (!CopyOverlayToDrawBufferCPU(priv)) {
		RAVE_LOG("FireNoticeMethod: selector=%d failed to copy draw buffer", selector);
		if (restartRenderPass) RestartRenderPassWithLoad(priv);
		return;
	}

	if (ms->noticeDeviceMac == 0) {
		ms->noticeDeviceMac = Mac_sysalloc(24);
	}
	if (ms->noticeDirtyRectMac == 0) {
		ms->noticeDirtyRectMac = Mac_sysalloc(16);
	}
	if (ms->noticeDeviceMac == 0 || ms->noticeDirtyRectMac == 0) {
		if (restartRenderPass) RestartRenderPassWithLoad(priv);
		return;
	}

	NSUInteger w = ms->overlayTexture.width;
	NSUInteger h = ms->overlayTexture.height;
	uint32_t rowBytes = (uint32_t)(w * 4);

	WriteMacInt32(ms->noticeDeviceMac + 0,  kQADeviceMemory);
	WriteMacInt32(ms->noticeDeviceMac + 4,  rowBytes);
	WriteMacInt32(ms->noticeDeviceMac + 8,  kQAPixel_RGB32);
	WriteMacInt32(ms->noticeDeviceMac + 12, (uint32_t)w);
	WriteMacInt32(ms->noticeDeviceMac + 16, (uint32_t)h);
	WriteMacInt32(ms->noticeDeviceMac + 20, ms->drawBufferCPUMac);

	WriteMacInt32(ms->noticeDirtyRectMac + 0,  0);
	WriteMacInt32(ms->noticeDirtyRectMac + 4,  (uint32_t)w);
	WriteMacInt32(ms->noticeDirtyRectMac + 8,  0);
	WriteMacInt32(ms->noticeDirtyRectMac + 12, (uint32_t)h);

	call_macos4(callback, priv->drawContextAddr, ms->noticeDeviceMac,
	            ms->noticeDirtyRectMac, refCon);

	if (!UploadDrawBufferCPUToOverlay(priv, ms->noticeDirtyRectMac)) {
		RAVE_LOG("FireNoticeMethod: selector=%d failed to upload draw buffer", selector);
		if (restartRenderPass) RestartRenderPassWithLoad(priv);
		return;
	}

	if (restartRenderPass) {
		if (ms->msaaActive) {
			ms->msaaActive = false;
			RAVE_LOG("FireNoticeMethod: selector=%d continuing frame without MSAA after CPU buffer access", selector);
		}
		RestartRenderPassWithLoad(priv);
	}
}

int32_t NativeSetNoticeMethod(uint32_t drawContextAddr, uint32_t method,
                               uint32_t callback, uint32_t refCon)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv) return kQAError;
	if (method >= RAVE_NUM_NOTICE_METHODS) return kQAError;

	priv->noticeMethods[method].callback = callback;
	priv->noticeMethods[method].refCon = refCon;

	RAVE_LOG("SetNoticeMethod: selector=%d callback=0x%08x refCon=0x%08x",
	         method, callback, refCon);
	return kQANoErr;
}

int32_t NativeGetNoticeMethod(uint32_t drawContextAddr, uint32_t method,
                               uint32_t callbackOutPtr, uint32_t refConOutPtr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv) return kQAError;
	if (method >= RAVE_NUM_NOTICE_METHODS) return kQAError;

	if (callbackOutPtr != 0)
		WriteMacInt32(callbackOutPtr, priv->noticeMethods[method].callback);
	if (refConOutPtr != 0)
		WriteMacInt32(refConOutPtr, priv->noticeMethods[method].refCon);

	return kQANoErr;
}

static void FireNoticeMethod(RaveDrawPrivate *priv, int selector)
{
	if (selector >= RAVE_NUM_NOTICE_METHODS) return;
	uint32_t callback = priv->noticeMethods[selector].callback;
	if (callback == 0) return;

	uint32_t refCon = priv->noticeMethods[selector].refCon;

	if (selector == 3 || selector == 4) {
		// Buffer notice: TQABufferNoticeMethod(drawContext, buffer, dirtyRect, refCon)
		FireBufferNoticeMethod(priv, selector, callback, refCon);
	} else {
		// Standard notice: TQAStandardNoticeMethod(drawContext, refCon)
		call_macos2(callback, priv->drawContextAddr, refCon);
	}

	RAVE_LOG("FireNoticeMethod: selector=%d callback=0x%08x", selector, callback);
}


/*
 *  NativeRenderStart
 *
 *  Creates a Metal render pass with clear color from state tags.
 *  PPC args: r3=drawContext, r4=dirtyRect, r5=initialContext
 */
int32 NativeRenderStart(uint32 drawContextAddr, uint32 dirtyRectAddr, uint32 initialContextAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) {
		RAVE_LOG("RenderStart: invalid context 0x%08x", drawContextAddr);
		return kQAError;
	}

	RaveMetalState *ms = priv->metal;

	if (ms->renderPassActive) {
		RAVE_LOG("RenderStart: render pass already active, returning error");
		return kQAError;
	}

	// Get RAVE's per-engine overlay texture (vended via
	// gfxaccel_resources rather than allocated by the compositor).
	// If the cached texture's dims don't match the context viewport,
	// re-vend at the current viewport size so RAVE always renders into
	// a correctly-sized overlay.
	id<MTLTexture> overlay = rave_acquire_overlay_texture(
	                              (uint32_t)priv->width, (uint32_t)priv->height);
	if (overlay == nil) {
		RAVE_LOG("NativeRenderStart: failed to acquire per-engine overlay texture");
		return kQAError;
	}
	ms->overlayTexture = overlay;
	/* Track the destination rect for SubmitFrame emission in NativeRenderEnd. */
	RaveSetCompositorDestinationRect(
	    priv->left, priv->top, priv->width, priv->height);

	// Check if MSAA is requested
	// kQAAntialias_Fast (1) = default, no MSAA
	// kQAAntialias_Best (2) = 4x MSAA
	bool wantMSAA = (priv->state[8].i >= 2);

	if (wantMSAA) {
		EnsureMSAAResources(priv);
		ms->msaaActive = true;
	} else {
		ms->msaaActive = false;
	}

	// Build render pass descriptor
	MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];

	// Color attachment: use the shared overlay clear policy. Zero-alpha clears
	// are promoted to opaque so the guest RGB remains a scene backdrop; nonzero
	// guest alpha is preserved and RGB is premultiplied by that effective alpha.
	const struct DMCModeSnapshot *clearSnap = dmc_current_snapshot();
	const uint32_t clearModeWidth = clearSnap ? clearSnap->width : 0;
	const uint32_t clearModeHeight = clearSnap ? clearSnap->height : 0;
	const float clearAlpha = RaveOverlayEffectiveClearAlpha(
	    priv->state[2].f, priv->state[3].f, priv->state[4].f, priv->state[1].f,
	    (uint32_t)priv->width, (uint32_t)priv->height,
	    clearModeWidth, clearModeHeight);
	rpd.colorAttachments[0].clearColor = MTLClearColorMake(
		RaveOverlayPremultipliedClearComponent(priv->state[2].f, clearAlpha),
		RaveOverlayPremultipliedClearComponent(priv->state[3].f, clearAlpha),
		RaveOverlayPremultipliedClearComponent(priv->state[4].f, clearAlpha),
		clearAlpha
	);
	rpd.colorAttachments[0].loadAction = MTLLoadActionClear;

	if (ms->msaaActive) {
		// MSAA: render to multisample texture, resolve to offscreen overlay
		rpd.colorAttachments[0].texture = ms->msaaColorTexture;
		rpd.colorAttachments[0].resolveTexture = ms->overlayTexture;
		rpd.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
	} else {
		rpd.colorAttachments[0].texture = ms->overlayTexture;
		rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
	}

	// Depth attachment (if depth buffer exists)
	if (!(priv->flags & kQAContext_NoZBuffer)) {
		id<MTLTexture> depthTex = (ms->msaaActive && ms->msaaDepthTexture) ? ms->msaaDepthTexture : ms->depthBuffer;
		if (depthTex) {
			rpd.depthAttachment.texture = depthTex;
			rpd.depthAttachment.loadAction = MTLLoadActionClear;
			rpd.depthAttachment.storeAction = MTLStoreActionStore;
			rpd.depthAttachment.clearDepth = 1.0;
		}
	}

	// Create command buffer and encoder
	ms->currentCommandBuffer = [ms->commandQueue commandBuffer];
	ms->currentEncoder = [ms->currentCommandBuffer renderCommandEncoderWithDescriptor:rpd];

	if (!ms->currentEncoder) {
		RAVE_LOG("RenderStart: failed to create render command encoder");
		ms->currentCommandBuffer = nil;
		return kQAError;
	}

	// Set scissor rect from context dimensions
	MTLScissorRect scissor;
	scissor.x = 0;
	scissor.y = 0;
	scissor.width = (NSUInteger)priv->width;
	scissor.height = (NSUInteger)priv->height;
	[ms->currentEncoder setScissorRect:scissor];

	// Set viewport to match context dimensions
	MTLViewport viewport;
	viewport.originX = 0.0;
	viewport.originY = 0.0;
	viewport.width = (double)priv->width;
	viewport.height = (double)priv->height;
	viewport.znear = 0.0;
	viewport.zfar = 1.0;
	[ms->currentEncoder setViewport:viewport];

	// Set viewport uniform for vertex shader (buffer index 1)
	RaveViewport vp = { (float)priv->width, (float)priv->height, 0.0f, 1.0f };
	[ms->currentEncoder setVertexBytes:&vp length:sizeof(vp) atIndex:1];

	// Set vertex uniforms (point width from kQATag_Width) at buffer(2)
	VertexUniforms vertUniforms;
	vertUniforms.point_width = priv->state[5].f;  // kQATag_Width
	if (vertUniforms.point_width < 1.0f) vertUniforms.point_width = 1.0f;
	[ms->currentEncoder setVertexBytes:&vertUniforms length:sizeof(vertUniforms) atIndex:2];

	// Set default depth stencil state for the current draw state.
	RaveSetDepthStencilStateForCurrentDraw(priv);

	ms->renderPassActive = true;
	ms->currentPipelineKey = kRavePipelineKeyInvalid;  // Force pipeline selection on first draw

	// Reset per-frame diagnostic counters
	ms->drawCallCount = 0;
	ms->triangleCount = 0;
	ms->triGouraudCount = 0;
	ms->triTextureCount = 0;
	ms->vGouraudCount = 0;
	ms->vTextureCount = 0;
	ms->meshGouraudCount = 0;
	ms->meshTextureCount = 0;
	ms->bitmapCount = 0;
	ms->pointCount = 0;
	ms->lineCount = 0;
	ms->frameWorldTexDraws = 0;
	ms->frameBlackWorldTexDraws = 0;
	ms->frameModulateDraws = 0;
	memset(ms->textureDrawCounts, 0, sizeof(ms->textureDrawCounts));

	// Reset Z-sort buffer for this frame
	priv->zsortCount = 0;

	// Reset multi-texture state for this frame
	priv->multiTextureActive = false;
	priv->multiTexStagingCount = 0;

	// Increment frame counter for diagnostics
	priv->frameCount++;

	// Fire kQAMethod_ImageBufferInitialize (selector 3) callback
	FireNoticeMethod(priv, 3);

	// SetOverlayRect is gone — destination rect now travels with
	// each SubmitFrame's CompositeLayer (encoded in NativeRenderEnd from
	// s_rave_dst_*). Update local cached dst rect in case the viewport
	// changed after context creation.
	RaveSetCompositorDestinationRect(
	    priv->left, priv->top, priv->width, priv->height);

	RAVE_LOG("RenderStart: ctx=0x%08x dirty=0x%08x initial=0x%08x clear=(%.2f,%.2f,%.2f,%.2f) effectiveClearAlpha=%.2f size=%dx%d",
	         drawContextAddr,
	         dirtyRectAddr, initialContextAddr,
	         priv->state[2].f, priv->state[3].f, priv->state[4].f, priv->state[1].f,
	         clearAlpha, priv->width, priv->height);

	return kQANoErr;
}


/*
 *  NativeRenderEnd
 *
 *  Presents the Metal drawable showing the clear color.
 *  PPC args: r3=drawContext, r4=modifiedRect
 */
int32 NativeRenderEnd(uint32 drawContextAddr, uint32 modifiedRectAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQANoErr;

	RaveMetalState *ms = priv->metal;

	if (!ms->renderPassActive) return kQANoErr;

	// Flush Z-sorted transparent triangles (back-to-front)
	FlushZSortBuffer(priv);

	// Fire kQAMethod_ImageBuffer2DComposite (selector 4) before ending the render pass.
	// This fires while the encoder is still active so the app can submit 2D overlay
	// draw commands (menus, HUD) if it uses Metal.
	FireNoticeMethod(priv, 4);

	if (ms->currentEncoder) {
		[ms->currentEncoder endEncoding];
		ms->currentEncoder = nil;
	}
	if (!ms->currentCommandBuffer) {
		ms->currentCommandBuffer = [ms->commandQueue commandBuffer];
	}

	// Offscreen overlay blitted to all RTT texture handles via MTLBlitCommandEncoder.
	// Test: RAVESurfaceTests.testOffscreenOverlay_RTT_blit
	if (!ms->rttTextureHandles.empty() && ms->overlayTexture) {
		id<MTLBlitCommandEncoder> blit = [ms->currentCommandBuffer blitCommandEncoder];
		id<MTLTexture> srcTex = ms->overlayTexture;
		NSUInteger w = srcTex.width;
		NSUInteger h = srcTex.height;
		for (const RaveRTTResourceToken &token : ms->rttTextureHandles) {
			RaveResourceEntry *entry = RaveResourceGet(token.handle);
			if (entry && entry->generation != token.generation)
				entry = nullptr;
			if (entry && entry->metal_texture) {
				id<MTLTexture> rttTex = (__bridge id<MTLTexture>)entry->metal_texture;
				[blit copyFromTexture:srcTex
				          sourceSlice:0 sourceLevel:0
				         sourceOrigin:MTLOriginMake(0, 0, 0)
				           sourceSize:MTLSizeMake(w, h, 1)
				            toTexture:rttTex
				     destinationSlice:0 destinationLevel:0
				    destinationOrigin:MTLOriginMake(0, 0, 0)];
			}
		}
		[blit endEncoding];
	}

	// DELIBERATE: DontSwap is an intentional no-op beyond logging.
	// With offscreen texture, the rendered content persists regardless and the
	// command buffer commits unconditionally; DontSwap just controls whether we
	// log "committed (DontSwap)" vs "frame committed to offscreen". There is no
	// presentDrawable / held back-buffer to suppress — the compositor reads the
	// offscreen overlay on its next VBL, so this is semantically equivalent to a
	// true single-overlay DontSwap. Test: RAVEABITests.testDontSwap_intentionalNoOp
	int32_t dont_swap = (int32_t)priv->state[32].i;

	if (ms->currentCommandBuffer) {
		CommitRaveCommandBufferWithRing(ms->currentCommandBuffer);
	}

	ms->lastCommittedBuffer = ms->currentCommandBuffer;
	ms->currentEncoder = nil;
	ms->currentCommandBuffer = nil;
	ms->renderPassActive = false;

	// modifiedRect is a const input from the guest, not an output parameter.
	(void)modifiedRectAddr;

	// Fire kQAMethod_RenderCompletion (selector 0) after present/commit
	FireNoticeMethod(priv, 0);

	if (rave_logging_enabled) {
		RAVE_LOG("RenderEnd: ctx=0x%08x draws=%u tris=%u [tG=%u tT=%u vG=%u vT=%u mG=%u mT=%u bm=%u pt=%u ln=%u] blackWorldTex=%u/%u mulDraws=%u %s",
		         drawContextAddr, ms->drawCallCount, ms->triangleCount,
		         ms->triGouraudCount, ms->triTextureCount,
		         ms->vGouraudCount, ms->vTextureCount,
		         ms->meshGouraudCount, ms->meshTextureCount,
		         ms->bitmapCount, ms->pointCount, ms->lineCount,
		         ms->frameBlackWorldTexDraws, ms->frameWorldTexDraws, ms->frameModulateDraws,
		         dont_swap ? "committed (DontSwap)" : "frame committed to offscreen");
	}

	// Emit a CompositeLayer for the just-rendered overlay via
	// MetalCompositorSubmitFrame. The
	// compositor sees this as a single kLayerSlotOverlay layer with
	// premultiplied-alpha blend (RAVE always blends; see BuildPipelines)
	// over whatever the framebuffer currently
	// shows. dmc_set_active_owner already declared RAVE in
	// RaveCreateMetalOverlay; the idempotent fast-path handles the
	// per-frame re-declaration.
	id<MTLTexture> submittedOverlay = ms->overlayTexture ?: s_rave_overlay_tex;
	if (submittedOverlay != nil) {
		struct CompositeLayer layer;
		layer.source       = (__bridge void *)submittedOverlay;
		layer.src_origin_x = 0;
		layer.src_origin_y = 0;
		layer.src_size_w   = s_rave_overlay_w;
		layer.src_size_h   = s_rave_overlay_h;
		layer.dst_origin_x = (float)s_rave_dst_left;
		layer.dst_origin_y = (float)s_rave_dst_top;
		layer.dst_size_w   = (float)s_rave_dst_width;
		layer.dst_size_h   = (float)s_rave_dst_height;
		layer.slot         = kLayerSlotOverlay;
		layer.blend        = kBlendPremultiplied;
		layer.alpha        = 1.0f;

		const struct DMCModeSnapshot *snap = dmc_current_snapshot();
		struct FrameDescriptor desc;
		desc.layers               = &layer;
		desc.layer_count          = 1;
		desc.generation           = snap ? snap->generation : 0;
		desc.vbl_tick_target_usec = 0;

		int32_t err = MetalCompositorSubmitFrame(&desc);
		if (err == kGfxAccelErrStaleGeneration) {
			RAVE_LOG("RenderEnd: SubmitFrame stale generation; dropping frame");
		} else if (err != kGfxAccelNoErr) {
			RAVE_LOG("RenderEnd: SubmitFrame returned %d", err);
		} else {
			rave_advance_overlay_texture_after_submit();
		}
		/* Re-declare RAVE owner — fast no-op when already RAVE. */
		(void)dmc_set_active_owner(kDMCOwnerRAVE);
	}

	// SubmitFrame is cache-only in production; pace RAVE render ends explicitly.
	MetalCompositorSync3DFramePacingForEngine(kGfxFramePacingEngineRAVE);

	return kQANoErr;
}


/*
 *  NativeSwapBuffers
 *
 *  Explicitly presents the held drawable from a DontSwap RenderEnd.
 *  If no drawable is held (DontSwap was not set), this is a no-op.
 *  DELIBERATE: Metal CAMetalLayer.presentDrawable always presents the full
 *  drawable. Partial present is not supported by the Metal API. QD3D 1.6 spec §Present allows
 *  implementation-defined behavior. Test: RAVESurfaceTests.testDirtyRect_present_deliberate
 *  PPC args: r3=drawContextAddr, r4=dirtyRectAddr
 */
int32 NativeSwapBuffers(uint32 drawContextAddr, uint32 dirtyRectAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQANoErr;
	(void)dirtyRectAddr;

	// With offscreen texture rendering, the rendered content persists and
	// the compositor reads it on every VBL. SwapBuffers is effectively a no-op.
	RAVE_LOG("SwapBuffers: ctx=0x%08x no-op (offscreen texture persists)", drawContextAddr);
	return kQANoErr;
}


/*
 *  NativeBusy
 *
 *  Returns whether the engine has uncommitted/in-flight command buffers.
 *  Returns 1 (busy) if the last committed buffer has not completed, 0 (idle) otherwise.
 *  PPC args: r3=drawContextAddr
 */
int32 NativeBusy(uint32 drawContextAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return 0;

	RaveMetalState *ms = priv->metal;

	if (!ms->lastCommittedBuffer) return 0;

	MTLCommandBufferStatus status = [ms->lastCommittedBuffer status];
	return (status != MTLCommandBufferStatusCompleted &&
	        status != MTLCommandBufferStatusError) ? 1 : 0;
}


/*
 *  NativeTextureNewFromDrawContext
 *
 *  Creates an offscreen render target (MTLTexture) at the draw context's
 *  dimensions, usable as a texture resource for render-to-texture passes.
 *  PPC args: r3=drawContextAddr, r4=flags, r5=newTexturePtr
 *  Returns: TQAError; writes the TQATexture* through newTexturePtr.
 *
 *  DELIBERATE: Lock and Mipmap flags intentionally ignored for RTT textures.
 *  Metal manages GPU memory directly; lock semantics are a software rendering artifact.
 *  QD3D 1.6 spec allows engine-specific interpretation of these hints.
 *  Test: RAVESurfaceTests.testLockMipmap_documented
 */
int32 NativeTextureNewFromDrawContext(uint32 drawContextAddr, uint32 flags, uint32 newTexturePtr)
{
	if (newTexturePtr != 0) WriteMacInt32(newTexturePtr, 0);

	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal || newTexturePtr == 0) return kQAError;
	(void)flags;

	RaveMetalState *ms = priv->metal;
	id<MTLDevice> device = ms->device;
	if (!device) return kQAError;

	uint32_t w = (uint32_t)priv->width;
	uint32_t h = (uint32_t)priv->height;

	// Allocate resource table entry
	uint32_t handle = RaveResourceAlloc(kRaveResourceTexture);
	if (handle == 0) return kQAError;
	RaveResourceEntry *entry = RaveResourceGet(handle);

	// Create MTLTexture for render-to-texture with shader read capability
	MTLTextureDescriptor *desc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		                            width:w
		                           height:h
		                        mipmapped:NO];
	desc.storageMode = MTLStorageModePrivate;
	desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;

	id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
	if (!texture) {
		RaveResourceFree(handle);
		RAVE_LOG("TextureNewFromDrawContext: failed to create %dx%d texture", w, h);
		return kQAError;
	}

	// Create matching depth texture for RTT passes
	MTLTextureDescriptor *depthDesc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
		                            width:w
		                           height:h
		                        mipmapped:NO];
	depthDesc.storageMode = MTLStorageModePrivate;
	depthDesc.usage = MTLTextureUsageRenderTarget;
	// Depth texture stored separately -- not needed in resource entry

	entry->metal_texture = (__bridge_retained void *)texture;
	entry->pixel_type = 4;  // kQAPixel_ARGB32
	entry->width = w;
	entry->height = h;
	entry->mip_levels = 1;
	entry->row_bytes = w * 4;

	// Allocate CPU pixel buffer for AccessTexture support
	uint32_t cpuBufSize = w * h * 4;
	uint32_t cpuMacAddr = Mac_sysalloc(cpuBufSize);
	if (cpuMacAddr != 0) {
		entry->cpu_pixel_mac_addr = cpuMacAddr;
		entry->cpu_pixel_data = Mac2HostAddr(cpuMacAddr);
		entry->cpu_pixel_data_size = cpuBufSize;
	}

	priv->metal->rttTextureHandles.push_back({ handle, entry->generation });

	RAVE_LOG("TextureNewFromDrawContext: %dx%d handle=%d mac=0x%08x",
	         w, h, handle, entry->mac_addr);
	WriteMacInt32(newTexturePtr, entry->mac_addr);
	return kQANoErr;
}


/*
 *  NativeBitmapNewFromDrawContext
 *
 *  Creates an offscreen render target usable as a bitmap resource.
 *  Same as TextureNewFromDrawContext but single mip level and bitmap type.
 *  PPC args: r3=drawContextAddr, r4=flags, r5=newBitmapPtr
 *  Returns: TQAError; writes the TQABitmap* through newBitmapPtr.
 */
int32 NativeBitmapNewFromDrawContext(uint32 drawContextAddr, uint32 flags, uint32 newBitmapPtr)
{
	if (newBitmapPtr != 0) WriteMacInt32(newBitmapPtr, 0);

	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal || newBitmapPtr == 0) return kQAError;
	(void)flags;

	RaveMetalState *ms = priv->metal;
	id<MTLDevice> device = ms->device;
	if (!device) return kQAError;

	uint32_t w = (uint32_t)priv->width;
	uint32_t h = (uint32_t)priv->height;

	// Allocate resource table entry as bitmap
	uint32_t handle = RaveResourceAlloc(kRaveResourceBitmap);
	if (handle == 0) return kQAError;
	RaveResourceEntry *entry = RaveResourceGet(handle);

	// Create MTLTexture -- no depth needed for bitmaps
	MTLTextureDescriptor *desc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		                            width:w
		                           height:h
		                        mipmapped:NO];
	desc.storageMode = MTLStorageModePrivate;
	desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;

	id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
	if (!texture) {
		RaveResourceFree(handle);
		RAVE_LOG("BitmapNewFromDrawContext: failed to create %dx%d texture", w, h);
		return kQAError;
	}

	entry->metal_texture = (__bridge_retained void *)texture;
	entry->pixel_type = 4;  // kQAPixel_ARGB32
	entry->width = w;
	entry->height = h;
	entry->mip_levels = 1;
	entry->row_bytes = w * 4;

	// Allocate CPU pixel buffer for AccessBitmap support
	uint32_t cpuBufSize = w * h * 4;
	uint32_t cpuMacAddr = Mac_sysalloc(cpuBufSize);
	if (cpuMacAddr != 0) {
		entry->cpu_pixel_mac_addr = cpuMacAddr;
		entry->cpu_pixel_data = Mac2HostAddr(cpuMacAddr);
		entry->cpu_pixel_data_size = cpuBufSize;
	}

	priv->metal->rttTextureHandles.push_back({ handle, entry->generation });

	RAVE_LOG("BitmapNewFromDrawContext: %dx%d handle=%d mac=0x%08x",
	         w, h, handle, entry->mac_addr);
	WriteMacInt32(newBitmapPtr, entry->mac_addr);
	return kQANoErr;
}


/*
 *  NativeRenderAbort
 *
 *  Discards the in-progress command buffer and logs a warning.
 *  PPC args: r3=drawContext
 */
int32 NativeRenderAbort(uint32 drawContextAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQANoErr;

	RaveMetalState *ms = priv->metal;

	if (!ms->renderPassActive) return kQANoErr;

	[ms->currentEncoder endEncoding];
	// Must commit even on abort per Metal rules
	CommitRaveCommandBufferWithRing(ms->currentCommandBuffer);

	ms->currentEncoder = nil;
	ms->currentCommandBuffer = nil;
	ms->renderPassActive = false;

	RAVE_LOG("RenderAbort: ctx=0x%08x render pass aborted", drawContextAddr);

	return kQANoErr;
}


/*
 *  NativeFlush
 *
 *  Mid-frame commit: ends the current encoder, commits the command buffer
 *  (non-blocking), then starts a new command buffer and encoder so the CPU
 *  can continue encoding while the GPU processes earlier work.
 *
 *  The drawable is NOT presented here -- presentation happens only in RenderEnd.
 *  The new encoder uses MTLLoadActionLoad to preserve existing framebuffer content.
 *
 *  PPC args: r3=drawContext
 */
int32 NativeFlush(uint32 drawContextAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQANoErr;

	RaveMetalState *ms = priv->metal;

	if (!ms->renderPassActive || !ms->currentEncoder) {
		// Nothing to flush if no render pass is active
		return kQANoErr;
	}

	// End current encoder and commit (non-blocking, no present)
	[ms->currentEncoder endEncoding];
	CommitRaveCommandBufferWithRing(ms->currentCommandBuffer);
	ms->lastCommittedBuffer = ms->currentCommandBuffer;

	// Create new command buffer to continue encoding
	ms->currentCommandBuffer = [ms->commandQueue commandBuffer];

	// Build render pass descriptor with LoadActionLoad to preserve content
	MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];

	if (ms->msaaActive && ms->msaaColorTexture) {
		rpd.colorAttachments[0].texture = ms->msaaColorTexture;
		rpd.colorAttachments[0].resolveTexture = ms->overlayTexture;
		rpd.colorAttachments[0].loadAction = MTLLoadActionLoad;
		rpd.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
	} else {
		rpd.colorAttachments[0].texture = ms->overlayTexture;
		rpd.colorAttachments[0].loadAction = MTLLoadActionLoad;
		rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
	}

	if (!(priv->flags & kQAContext_NoZBuffer)) {
		id<MTLTexture> depthTex = (ms->msaaActive && ms->msaaDepthTexture) ? ms->msaaDepthTexture : ms->depthBuffer;
		if (depthTex) {
			rpd.depthAttachment.texture = depthTex;
			rpd.depthAttachment.loadAction = MTLLoadActionLoad;
			rpd.depthAttachment.storeAction = MTLStoreActionStore;
		}
	}

	ms->currentEncoder = [ms->currentCommandBuffer renderCommandEncoderWithDescriptor:rpd];

	if (!ms->currentEncoder) {
		RAVE_LOG("Flush: failed to create continuation encoder");
		ms->currentCommandBuffer = nil;
		ms->renderPassActive = false;
		return kQAError;
	}

	// Restore viewport and scissor on the new encoder
	MTLScissorRect scissor;
	scissor.x = 0;
	scissor.y = 0;
	scissor.width = (NSUInteger)priv->width;
	scissor.height = (NSUInteger)priv->height;
	[ms->currentEncoder setScissorRect:scissor];

	MTLViewport viewport;
	viewport.originX = 0.0;
	viewport.originY = 0.0;
	viewport.width = (double)priv->width;
	viewport.height = (double)priv->height;
	viewport.znear = 0.0;
	viewport.zfar = 1.0;
	[ms->currentEncoder setViewport:viewport];

	// Restore viewport uniform for vertex shader (buffer index 1)
	RaveViewport vp = { (float)priv->width, (float)priv->height, 0.0f, 1.0f };
	[ms->currentEncoder setVertexBytes:&vp length:sizeof(vp) atIndex:1];

	// Restore vertex uniforms (point width) at buffer(2)
	VertexUniforms vertUniforms;
	vertUniforms.point_width = priv->state[5].f;  // kQATag_Width
	if (vertUniforms.point_width < 1.0f) vertUniforms.point_width = 1.0f;
	[ms->currentEncoder setVertexBytes:&vertUniforms length:sizeof(vertUniforms) atIndex:2];

	// Force pipeline re-bind on next draw after flush
	ms->currentPipelineKey = kRavePipelineKeyInvalid;

	// Restore depth stencil state for the current draw state.
	RaveSetDepthStencilStateForCurrentDraw(priv);

	RAVE_VLOG("Flush: ctx=0x%08x mid-frame commit, new encoder started", drawContextAddr);

	return kQANoErr;
}


/*
 *  NativeSync
 *
 *  Blocks until all GPU work is complete.
 *  PPC args: r3=drawContext
 *
 *  Handles both mid-frame and post-frame states:
 *  - Mid-frame (renderPassActive): flush current work via NativeFlush (which
 *    commits the buffer and starts a new encoder), then wait on the committed buffer.
 *  - Post-frame (!renderPassActive): wait on lastCommittedBuffer if available.
 */
int32 NativeSync(uint32 drawContextAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQANoErr;

	RaveMetalState *ms = priv->metal;

	if (ms->renderPassActive) {
		// Mid-frame sync: flush to commit current work, then wait on it
		// NativeFlush saves the committed buffer in lastCommittedBuffer
		int32 flushResult = NativeFlush(drawContextAddr);
		if (flushResult != kQANoErr) {
			RAVE_LOG("Sync: flush failed during mid-frame sync");
			return flushResult;
		}
		// Now wait on the buffer that was just committed by Flush
		if (ms->lastCommittedBuffer) {
			[ms->lastCommittedBuffer waitUntilCompleted];
			RAVE_LOG("Sync: ctx=0x%08x mid-frame wait completed", drawContextAddr);
		}
	} else {
		// Post-frame sync: wait on the most recently committed buffer
		if (ms->lastCommittedBuffer) {
			[ms->lastCommittedBuffer waitUntilCompleted];
			RAVE_LOG("Sync: ctx=0x%08x post-frame wait completed", drawContextAddr);
		}
	}

	return kQANoErr;
}


/*
 *  Metal texture creation/upload functions
 *
 *  Called from rave_engine.cpp (C++ code) to create ObjC Metal objects.
 *  Use void* to cross the ObjC++/C++ language boundary.
 */

/*
 *  Helper: restart render pass with LoadActionLoad (preserves existing content).
 *  Used after AccessDrawBufferEnd and AccessZBufferEnd to continue rendering.
 */
static int32_t RestartRenderPassWithLoad(RaveDrawPrivate *priv)
{
	RaveMetalState *ms = priv->metal;
	ms->currentCommandBuffer = [ms->commandQueue commandBuffer];
	MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
	if (ms->msaaActive && ms->msaaColorTexture) {
		rpd.colorAttachments[0].texture = ms->msaaColorTexture;
		rpd.colorAttachments[0].resolveTexture = ms->overlayTexture;
		rpd.colorAttachments[0].loadAction = MTLLoadActionLoad;
		rpd.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
	} else {
		rpd.colorAttachments[0].texture = ms->overlayTexture;
		rpd.colorAttachments[0].loadAction = MTLLoadActionLoad;
		rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
	}
	if (!(priv->flags & kQAContext_NoZBuffer)) {
		id<MTLTexture> depthTex = (ms->msaaActive && ms->msaaDepthTexture) ? ms->msaaDepthTexture : ms->depthBuffer;
		if (depthTex) {
			rpd.depthAttachment.texture = depthTex;
			rpd.depthAttachment.loadAction = MTLLoadActionLoad;
			rpd.depthAttachment.storeAction = MTLStoreActionStore;
		}
	}
	ms->currentEncoder = [ms->currentCommandBuffer renderCommandEncoderWithDescriptor:rpd];
	if (!ms->currentEncoder) {
		RAVE_LOG("RestartRenderPassWithLoad: failed to create encoder");
		ms->currentCommandBuffer = nil;
		ms->renderPassActive = false;
		return kQAError;
	}
	MTLScissorRect scissor;
	scissor.x = 0; scissor.y = 0;
	scissor.width = (NSUInteger)priv->width;
	scissor.height = (NSUInteger)priv->height;
	[ms->currentEncoder setScissorRect:scissor];
	MTLViewport viewport;
	viewport.originX = 0.0; viewport.originY = 0.0;
	viewport.width = (double)priv->width; viewport.height = (double)priv->height;
	viewport.znear = 0.0; viewport.zfar = 1.0;
	[ms->currentEncoder setViewport:viewport];
	RaveViewport vp = { (float)priv->width, (float)priv->height, 0.0f, 1.0f };
	[ms->currentEncoder setVertexBytes:&vp length:sizeof(vp) atIndex:1];
	VertexUniforms vertUniforms;
	vertUniforms.point_width = priv->state[5].f;
	if (vertUniforms.point_width < 1.0f) vertUniforms.point_width = 1.0f;
	[ms->currentEncoder setVertexBytes:&vertUniforms length:sizeof(vertUniforms) atIndex:2];
	ms->currentPipelineKey = kRavePipelineKeyInvalid;
	RaveSetDepthStencilStateForCurrentDraw(priv);
	return kQANoErr;
}


/*
 *  SDK: TQAError AccessDrawBuffer(const TQADrawContext*, TQAPixelBuffer*)
 *  TQAPixelBuffer = TQADeviceMemory = {rowBytes(+0), pixelType(+4), width(+8), height(+12), baseAddr(+16)}
 *  Populates the TQAPixelBuffer struct at bufferStructAddr with draw buffer info.
 */
int32_t NativeAccessDrawBuffer(uint32_t drawContextAddr, uint32_t bufferStructAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQAError;
	RaveMetalState *ms = priv->metal;
	if (!ms->renderPassActive || !ms->overlayTexture) return kQAError;

	EndAndCommitCurrentRenderPass(ms);
	if (!CopyOverlayToDrawBufferCPU(priv)) return kQAError;

	NSUInteger w = ms->overlayTexture.width;
	NSUInteger h = ms->overlayTexture.height;
	uint32_t rowBytes = (uint32_t)(w * 4);

	// Populate TQAPixelBuffer (TQADeviceMemory) struct
	WriteMacInt32(bufferStructAddr + 0,  rowBytes);
	WriteMacInt32(bufferStructAddr + 4,  kQAPixel_RGB32);
	WriteMacInt32(bufferStructAddr + 8,  (uint32_t)w);
	WriteMacInt32(bufferStructAddr + 12, (uint32_t)h);
	WriteMacInt32(bufferStructAddr + 16, ms->drawBufferCPUMac);
	ms->drawBufferAccessed = true;
	RAVE_VLOG("AccessDrawBuffer: ctx=0x%08x %lux%lu buf=0x%08x rowBytes=%d",
	         drawContextAddr, (unsigned long)w, (unsigned long)h, ms->drawBufferCPUMac, rowBytes);
	return kQANoErr;
}


int32_t NativeAccessDrawBufferEnd(uint32_t drawContextAddr, uint32_t dirtyRectAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQAError;
	RaveMetalState *ms = priv->metal;
	if (!ms->drawBufferAccessed) return kQANoErr;

	NSUInteger uploadX, uploadY, uploadW, uploadH;
	GetTQARectUploadRegion(dirtyRectAddr, ms->overlayTexture.width, ms->overlayTexture.height,
	                       &uploadX, &uploadY, &uploadW, &uploadH);
	if (!UploadDrawBufferCPUToOverlay(priv, dirtyRectAddr)) return kQAError;
	ms->drawBufferAccessed = false;
	if (ms->msaaActive) {
		ms->msaaActive = false;
		RAVE_VLOG("AccessDrawBufferEnd: continuing frame without MSAA after CPU buffer access");
	}
	int32_t result = RestartRenderPassWithLoad(priv);
	RAVE_VLOG("AccessDrawBufferEnd: ctx=0x%08x uploaded region (%lu,%lu %lux%lu), render pass restarted",
	         drawContextAddr, (unsigned long)uploadX, (unsigned long)uploadY,
	         (unsigned long)uploadW, (unsigned long)uploadH);
	return result;
}


/*
 *  SDK: TQAError AccessZBuffer(const TQADrawContext*, TQAZBuffer*)
 *  TQAZBuffer = {width(+0), height(+4), rowBytes(+8), zbuffer(+12), zDepth(+16), isBigEndian(+20)}
 *  Populates the TQAZBuffer struct at bufferStructAddr with Z buffer info.
 */
int32_t NativeAccessZBuffer(uint32_t drawContextAddr, uint32_t bufferStructAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQAError;
	RaveMetalState *ms = priv->metal;
	if (!ms->renderPassActive || !ms->depthBuffer) return kQAError;

	EndAndCommitCurrentRenderPass(ms);

	NSUInteger w = ms->depthBuffer.width;
	NSUInteger h = ms->depthBuffer.height;
	if (!ms->stagingZBuffer ||
	    ms->stagingZBuffer.width != w || ms->stagingZBuffer.height != h) {
		MTLTextureDescriptor *desc = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
			                            width:w height:h mipmapped:NO];
		desc.storageMode = MTLStorageModeShared;
		desc.usage = MTLTextureUsageShaderRead;
		ms->stagingZBuffer = [ms->device newTextureWithDescriptor:desc];
	}
	uint32_t rowBytes = (uint32_t)(w * 4);
	uint32_t bufSize = rowBytes * (uint32_t)h;
	if (!ms->zBufferCPU || ms->zBufferCPUSize != bufSize) {
		uint32_t macAddr = Mac_sysalloc(bufSize);
		if (macAddr == 0) return kQAError;
		ms->zBufferCPU = Mac2HostAddr(macAddr);
		ms->zBufferCPUMac = macAddr;
		ms->zBufferCPUSize = bufSize;
	}
	id<MTLCommandBuffer> blitCmdBuf = [ms->commandQueue commandBuffer];
	if (ms->msaaActive && ms->msaaDepthTexture) {
		// The MSAA pass renders depth into msaaDepthTexture, so ms->depthBuffer
		// is stale here. Blits cannot copy across sample counts; resolve
		// (sample 0) into depthBuffer with an empty load/resolve pass so the
		// readback below sees the depth the pass actually produced.
		MTLRenderPassDescriptor *resolveDesc = [MTLRenderPassDescriptor renderPassDescriptor];
		resolveDesc.depthAttachment.texture = ms->msaaDepthTexture;
		resolveDesc.depthAttachment.resolveTexture = ms->depthBuffer;
		resolveDesc.depthAttachment.loadAction = MTLLoadActionLoad;
		resolveDesc.depthAttachment.storeAction = MTLStoreActionMultisampleResolve;
		resolveDesc.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
		id<MTLRenderCommandEncoder> resolveEnc =
			[blitCmdBuf renderCommandEncoderWithDescriptor:resolveDesc];
		[resolveEnc endEncoding];
	}
	id<MTLBlitCommandEncoder> blit = [blitCmdBuf blitCommandEncoder];
	[blit copyFromTexture:ms->depthBuffer sourceSlice:0 sourceLevel:0
	         sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(w,h,1)
	            toTexture:ms->stagingZBuffer destinationSlice:0
	     destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
	[blit endEncoding];
	[blitCmdBuf commit];
	[blitCmdBuf waitUntilCompleted];

	const NSUInteger pixelCount = w * h;
	std::vector<uint32_t> hostDepth(pixelCount);
	[ms->stagingZBuffer getBytes:hostDepth.data() bytesPerRow:rowBytes
	                  fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
	for (NSUInteger i = 0; i < pixelCount; i++) {
		WriteMacInt32(ms->zBufferCPUMac + (uint32_t)(i * sizeof(uint32_t)), hostDepth[i]);
	}

	// Populate TQAZBuffer struct
	WriteMacInt32(bufferStructAddr + 0,  (uint32_t)w);       // width
	WriteMacInt32(bufferStructAddr + 4,  (uint32_t)h);       // height
	WriteMacInt32(bufferStructAddr + 8,  rowBytes);           // rowBytes
	WriteMacInt32(bufferStructAddr + 12, ms->zBufferCPUMac);  // zbuffer pointer
	WriteMacInt32(bufferStructAddr + 16, 32);                  // zDepth (32-bit float)
	WriteMacInt32(bufferStructAddr + 20, 1);                   // isBigEndian (PPC = big-endian)
	ms->zBufferAccessed = true;
	RAVE_VLOG("AccessZBuffer: ctx=0x%08x %lux%lu buf=0x%08x rowBytes=%d",
	         drawContextAddr, (unsigned long)w, (unsigned long)h, ms->zBufferCPUMac, rowBytes);
	return kQANoErr;
}


int32_t NativeAccessZBufferEnd(uint32_t drawContextAddr, uint32_t dirtyRectAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQAError;
	RaveMetalState *ms = priv->metal;
	if (!ms->zBufferAccessed) return kQANoErr;
	NSUInteger w = ms->depthBuffer.width;
	NSUInteger h = ms->depthBuffer.height;
	uint32_t rowBytes = (uint32_t)(w * 4);
	const NSUInteger pixelCount = w * h;
	std::vector<uint32_t> hostDepth(pixelCount);
	for (NSUInteger i = 0; i < pixelCount; i++) {
		hostDepth[i] = ReadMacInt32(ms->zBufferCPUMac + (uint32_t)(i * sizeof(uint32_t)));
	}
	[ms->stagingZBuffer replaceRegion:MTLRegionMake2D(0,0,w,h)
	                      mipmapLevel:0 withBytes:hostDepth.data()
	                      bytesPerRow:rowBytes];
	id<MTLCommandBuffer> blitCmdBuf = [ms->commandQueue commandBuffer];
	id<MTLBlitCommandEncoder> blit = [blitCmdBuf blitCommandEncoder];
	[blit copyFromTexture:ms->stagingZBuffer sourceSlice:0 sourceLevel:0
	         sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(w,h,1)
	            toTexture:ms->depthBuffer destinationSlice:0
	     destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
	[blit endEncoding];
	[blitCmdBuf commit];
	[blitCmdBuf waitUntilCompleted];
	ms->zBufferAccessed = false;
	if (ms->msaaActive) {
		// Mirror AccessDrawBufferEnd: the upload above wrote depthBuffer, so
		// resume single-sampled — restarting with msaaDepthTexture attached
		// would drop the guest's depth edits.
		ms->msaaActive = false;
		RAVE_VLOG("AccessZBufferEnd: continuing frame without MSAA after CPU depth access");
	}
	int32_t result = RestartRenderPassWithLoad(priv);
	RAVE_VLOG("AccessZBufferEnd: ctx=0x%08x depth uploaded, render pass restarted", drawContextAddr);
	return result;
}


int32_t NativeClearDrawBuffer(uint32_t drawContextAddr, uint32_t rectAddr, uint32_t initialContextAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQAError;
	RaveMetalState *ms = priv->metal;
	if (!ms->renderPassActive || !ms->currentEncoder) return kQAError;
	int32_t left   = (int32_t)ReadMacInt32(rectAddr + 0);
	int32_t right  = (int32_t)ReadMacInt32(rectAddr + 4);
	int32_t top    = (int32_t)ReadMacInt32(rectAddr + 8);
	int32_t bottom = (int32_t)ReadMacInt32(rectAddr + 12);
	if (left < 0) left = 0;
	if (top < 0) top = 0;
	if (right > priv->width) right = priv->width;
	if (bottom > priv->height) bottom = priv->height;
	if (right <= left || bottom <= top) return kQANoErr;
	// Read BG color: from initialContext if non-NULL per RAVE.h, else from current context
	float r, g, b, a;
	if (initialContextAddr != 0) {
		// Per RAVE.h: initialContext is a TQADrawContext* — read its drawPrivate handle
		// from offset 0, then look up the native context to read its BG color state
		uint32_t initHandle = ReadMacInt32(initialContextAddr);
		RaveDrawPrivate *initCtx = RaveGetContext(initHandle);
		if (initCtx) {
			a = 1.0f;                    // Force opaque for overlay compositing
			r = initCtx->state[2].f;     // kQATag_ColorBG_r
			g = initCtx->state[3].f;     // kQATag_ColorBG_g
			b = initCtx->state[4].f;     // kQATag_ColorBG_b
			RAVE_LOG("ClearDrawBuffer: using initialContext 0x%08x BG=(%.2f,%.2f,%.2f)",
			         initialContextAddr, r, g, b);
		} else {
			// initialContext handle invalid — fall back to current context
			a = 1.0f;
			r = priv->state[2].f;
			g = priv->state[3].f;
			b = priv->state[4].f;
			RAVE_LOG("ClearDrawBuffer: initialContext 0x%08x invalid handle %d, using current BG",
			         initialContextAddr, initHandle);
		}
	} else {
		// NULL initialContext — use current context's BG color
		a = 1.0f;              // Force opaque for overlay compositing
		r = priv->state[2].f;  // kQATag_ColorBG_r
		g = priv->state[3].f;  // kQATag_ColorBG_g
		b = priv->state[4].f;  // kQATag_ColorBG_b
	}
	MTLScissorRect clearScissor;
	clearScissor.x = (NSUInteger)left;
	clearScissor.y = (NSUInteger)top;
	clearScissor.width = (NSUInteger)(right - left);
	clearScissor.height = (NSUInteger)(bottom - top);
	[ms->currentEncoder setScissorRect:clearScissor];
	int pipe_idx = 0;
	// Apply channel mask from state[27] (kQATag_ChannelMask, default 0xF = all)
	uint32_t channelMask = RaveNormalizeChannelMask(priv->state[27].i);
	const bool hasDepthAttachment = RaveContextUsesMetalDepthAttachment(priv->flags) != 0;
	if (channelMask != 0xF) {
		// Use masked pipeline variant to respect per-channel write control
		id<MTLRenderPipelineState> maskedPipe = GetMaskedPipeline(
			ms, pipe_idx, channelMask, ms->msaaActive, hasDepthAttachment);
		if (maskedPipe)
			[ms->currentEncoder setRenderPipelineState:maskedPipe];
	} else {
		id<MTLRenderPipelineState> __strong *pipeArray = ms->msaaActive ? ms->msaaPipelines : ms->pipelines;
		if (pipeArray[pipe_idx])
			[ms->currentEncoder setRenderPipelineState:pipeArray[pipe_idx]];
	}
	// ClearDrawBuffer is color-only. Use an always-pass, no-write depth state so
	// the clear quad ignores existing Z without clobbering the depth attachment.
	if (hasDepthAttachment && ms->depthStatesNoWrite[7])
		[ms->currentEncoder setDepthStencilState:ms->depthStatesNoWrite[7]];
	float x0 = 0.0f, y0 = 0.0f;
	float x1 = (float)priv->width, y1 = (float)priv->height;
	float z = 0.0f;
	RaveVertex quad[6];
	quad[0] = { {x0,y0,z,1.0f}, {r,g,b,a}, {0,0,0,0}, {1,1,1,0}, {0,0,0,0} };
	quad[1] = { {x1,y0,z,1.0f}, {r,g,b,a}, {0,0,0,0}, {1,1,1,0}, {0,0,0,0} };
	quad[2] = { {x0,y1,z,1.0f}, {r,g,b,a}, {0,0,0,0}, {1,1,1,0}, {0,0,0,0} };
	quad[3] = { {x1,y0,z,1.0f}, {r,g,b,a}, {0,0,0,0}, {1,1,1,0}, {0,0,0,0} };
	quad[4] = { {x1,y1,z,1.0f}, {r,g,b,a}, {0,0,0,0}, {1,1,1,0}, {0,0,0,0} };
	quad[5] = { {x0,y1,z,1.0f}, {r,g,b,a}, {0,0,0,0}, {1,1,1,0}, {0,0,0,0} };
	FragmentUniforms fragUniforms = {};
	fragUniforms.fog_max_depth = 1.0f;
	[ms->currentEncoder setFragmentBytes:&fragUniforms length:sizeof(fragUniforms) atIndex:0];
	[ms->currentEncoder setVertexBytes:quad length:sizeof(quad) atIndex:0];
	[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	MTLScissorRect fullScissor;
	fullScissor.x = 0; fullScissor.y = 0;
	fullScissor.width = (NSUInteger)priv->width;
	fullScissor.height = (NSUInteger)priv->height;
	[ms->currentEncoder setScissorRect:fullScissor];
	ms->currentPipelineKey = kRavePipelineKeyInvalid;
	RaveSetDepthStencilStateForCurrentDraw(priv);
	RAVE_LOG("ClearDrawBuffer: ctx=0x%08x rect=(%d,%d,%d,%d) initialCtx=0x%08x bg=(%.2f,%.2f,%.2f,%.2f) mask=0x%x",
	         drawContextAddr, left, top, right, bottom, initialContextAddr, r, g, b, a, channelMask);
	return kQANoErr;
}


int32_t NativeClearZBuffer(uint32_t drawContextAddr, uint32_t rectAddr, uint32_t initialContextAddr)
{
	RaveDrawPrivate *priv = GetContextFromDrawAddr(drawContextAddr);
	if (!priv || !priv->metal) return kQAError;
	RaveMetalState *ms = priv->metal;
	if (!ms->renderPassActive || !ms->currentEncoder) return kQAError;
	if (!RaveContextUsesMetalDepthAttachment(priv->flags)) {
		RAVE_LOG("ClearZBuffer: skipped (no z-buffer context)");
		return kQANoErr;
	}

	// Disabled depth writes skip depth clear per kQAZBufferMask_Disable and ATI tag 1022.
	// Test: RAVERenderingStateTests.testZBufferMask_skipClear
	if (!RaveEffectiveDepthWriteEnabled(priv->state[28].i, priv->ati_state[kRaveATIDepthWriteEnableIndex].i)) {
		RAVE_LOG("ClearZBuffer: skipped (depth writes disabled)");
		return kQANoErr;
	}
	int32_t left   = (int32_t)ReadMacInt32(rectAddr + 0);
	int32_t right  = (int32_t)ReadMacInt32(rectAddr + 4);
	int32_t top    = (int32_t)ReadMacInt32(rectAddr + 8);
	int32_t bottom = (int32_t)ReadMacInt32(rectAddr + 12);
	if (left < 0) left = 0;
	if (top < 0) top = 0;
	if (right > priv->width) right = priv->width;
	if (bottom > priv->height) bottom = priv->height;
	if (right <= left || bottom <= top) return kQANoErr;
	// Determine clear depth: from initialContext if non-NULL per RAVE.h, else default 1.0
	float clearDepth;
	if (initialContextAddr != 0) {
		// Per RAVE.h: initialContext is a TQADrawContext* — read its drawPrivate handle
		// from offset 0, then look up its depth BG state
		uint32_t initHandle = ReadMacInt32(initialContextAddr);
		RaveDrawPrivate *initCtx = RaveGetContext(initHandle);
		if (initCtx) {
			// kQATagGL_DepthBG = 112 — if set, use it; otherwise default 1.0
			float depthBG = initCtx->state[112].f;
			clearDepth = (depthBG != 0.0f) ? depthBG : 1.0f;
			RAVE_LOG("ClearZBuffer: using initialContext 0x%08x depth=%.4f",
			         initialContextAddr, clearDepth);
		} else {
			clearDepth = 1.0f;
			RAVE_LOG("ClearZBuffer: initialContext 0x%08x invalid handle %d, using depth 1.0",
			         initialContextAddr, initHandle);
		}
	} else {
		// NULL initialContext — use far plane default
		clearDepth = 1.0f;
	}
	MTLScissorRect clearScissor;
	clearScissor.x = (NSUInteger)left;
	clearScissor.y = (NSUInteger)top;
	clearScissor.width = (NSUInteger)(right - left);
	clearScissor.height = (NSUInteger)(bottom - top);
	[ms->currentEncoder setScissorRect:clearScissor];
	int pipe_idx = 0;
	id<MTLRenderPipelineState> __strong *pipeArray = ms->msaaActive ? ms->msaaPipelines : ms->pipelines;
	if (pipeArray[pipe_idx])
		[ms->currentEncoder setRenderPipelineState:pipeArray[pipe_idx]];
	if (ms->depthAlwaysWriteState)
		[ms->currentEncoder setDepthStencilState:ms->depthAlwaysWriteState];
	float x0 = 0.0f, y0 = 0.0f;
	float x1 = (float)priv->width, y1 = (float)priv->height;
	RaveVertex quad[6];
	quad[0] = { {x0,y0,clearDepth,1.0f}, {0,0,0,0}, {0,0,0,0}, {1,1,1,0}, {0,0,0,0} };
	quad[1] = { {x1,y0,clearDepth,1.0f}, {0,0,0,0}, {0,0,0,0}, {1,1,1,0}, {0,0,0,0} };
	quad[2] = { {x0,y1,clearDepth,1.0f}, {0,0,0,0}, {0,0,0,0}, {1,1,1,0}, {0,0,0,0} };
	quad[3] = { {x1,y0,clearDepth,1.0f}, {0,0,0,0}, {0,0,0,0}, {1,1,1,0}, {0,0,0,0} };
	quad[4] = { {x1,y1,clearDepth,1.0f}, {0,0,0,0}, {0,0,0,0}, {1,1,1,0}, {0,0,0,0} };
	quad[5] = { {x0,y1,clearDepth,1.0f}, {0,0,0,0}, {0,0,0,0}, {1,1,1,0}, {0,0,0,0} };
	FragmentUniforms fragUniforms = {};
	fragUniforms.fog_max_depth = 1.0f;
	[ms->currentEncoder setFragmentBytes:&fragUniforms length:sizeof(fragUniforms) atIndex:0];
	[ms->currentEncoder setVertexBytes:quad length:sizeof(quad) atIndex:0];
	[ms->currentEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	MTLScissorRect fullScissor;
	fullScissor.x = 0; fullScissor.y = 0;
	fullScissor.width = (NSUInteger)priv->width;
	fullScissor.height = (NSUInteger)priv->height;
	[ms->currentEncoder setScissorRect:fullScissor];
	ms->currentPipelineKey = kRavePipelineKeyInvalid;
	RaveSetDepthStencilStateForCurrentDraw(priv);
	RAVE_LOG("ClearZBuffer: ctx=0x%08x rect=(%d,%d,%d,%d) initialCtx=0x%08x depth=%.4f",
	         drawContextAddr, left, top, right, bottom, initialContextAddr, clearDepth);
	return kQANoErr;
}


// MTLPixelFormatBGRA8Unorm matches converter output. bytesPerRow = width*4
// for BGRA8. replaceRegion origin (0,0) and size (width,height) correct. mipLevels > 1 sets
// mipmapLevelCount. Usage includes ShaderWrite when mipmapped (for generateMipmapsForTexture:).
void *RaveCreateMetalTexture(uint32_t width, uint32_t height, uint32_t mipLevels,
                              const uint8_t *pixelData, uint32_t bytesPerRow)
{
	id<MTLDevice> device = (__bridge id<MTLDevice>)SharedMetalDevice();
	if (!device) {
		RAVE_LOG("RaveCreateMetalTexture: no device available");
		return nullptr;
	}

	MTLTextureDescriptor *desc = [MTLTextureDescriptor
		texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		                            width:width
		                           height:height
		                        mipmapped:(mipLevels > 1)];
	desc.storageMode = MTLStorageModeShared;
	// ShaderWrite is required for RaveGenerateMipmaps (blit encoder generateMipmapsForTexture:)
	desc.usage = MTLTextureUsageShaderRead | ((mipLevels > 1) ? MTLTextureUsageShaderWrite : 0);
	if (mipLevels > 1) desc.mipmapLevelCount = mipLevels;

	id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
	if (!texture) {
		RAVE_LOG("RaveCreateMetalTexture: newTextureWithDescriptor failed");
		return nullptr;
	}

	// Upload level 0
	[texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
	           mipmapLevel:0
	             withBytes:pixelData
	           bytesPerRow:bytesPerRow];

	return (__bridge_retained void *)texture;
}


static RaveDrawPrivate *RaveCurrentDrawContext(void)
{
	if (rave_current_draw_context_addr == 0) {
		return nullptr;
	}

	uint32_t handle = ReadMacInt32(rave_current_draw_context_addr + 0);
	return RaveGetContext(handle);
}

static bool RaveEndActiveRenderPassForTextureUpload(RaveDrawPrivate *priv)
{
	if (!priv || !priv->metal) {
		return false;
	}

	RaveMetalState *ms = priv->metal;
	if (!ms->renderPassActive || !ms->currentEncoder || !ms->currentCommandBuffer) {
		return false;
	}

	[ms->currentEncoder endEncoding];
	ms->currentEncoder = nil;
	CommitRaveCommandBufferWithRing(ms->currentCommandBuffer);
	ms->lastCommittedBuffer = ms->currentCommandBuffer;
	ms->currentCommandBuffer = nil;
	return true;
}

static id<MTLCommandQueue> RaveTextureUploadCommandQueue(RaveDrawPrivate *priv)
{
	if (priv && priv->metal && priv->metal->commandQueue) {
		return priv->metal->commandQueue;
	}
	return (__bridge id<MTLCommandQueue>)SharedMetalCommandQueue();
}

/*
 *  Texture-upload batch scope: a multi-mip upload (level 0 + generated chain)
 *  brackets the whole chain in one Begin/End so the active render pass is
 *  broken/committed/restarted once, not once per mip level. Depth-counted so
 *  a call-site bracket nesting around RaveUploadGeneratedMips' internal
 *  bracket stays balanced. Emul-thread only (gfxaccel threading policy).
 */
static uint32_t s_rave_upload_batch_depth = 0;
static bool s_rave_upload_batch_restart = false;
static RaveDrawPrivate *s_rave_upload_batch_ctx = nullptr;

void RaveTextureUploadBatchBegin(void)
{
	if (s_rave_upload_batch_depth++ != 0) return;
	s_rave_upload_batch_ctx = RaveCurrentDrawContext();
	s_rave_upload_batch_restart =
		RaveEndActiveRenderPassForTextureUpload(s_rave_upload_batch_ctx);
}

void RaveTextureUploadBatchEnd(void)
{
	if (s_rave_upload_batch_depth == 0) return;
	if (--s_rave_upload_batch_depth != 0) return;
	if (s_rave_upload_batch_restart) {
		RestartRenderPassWithLoad(s_rave_upload_batch_ctx);
	}
	s_rave_upload_batch_restart = false;
	s_rave_upload_batch_ctx = nullptr;
}

// Upload through a staging buffer + blit so CPU writes do not race in-flight
// sampling of the shared texture. bytesPerRow must be width*4 for BGRA8 at the
// specified mip level (caller responsible).
void RaveUploadMipLevel(void *metalTexture, uint32_t level, uint32_t width, uint32_t height,
                         const uint8_t *pixelData, uint32_t bytesPerRow)
{
	if (!metalTexture || !pixelData || width == 0 || height == 0 || bytesPerRow == 0) return;

	id<MTLTexture> tex = (__bridge id<MTLTexture>)metalTexture;
	RaveDrawPrivate *currentCtx = RaveCurrentDrawContext();
	// Inside a Begin/End batch the pass was already broken once for the whole
	// mip chain — do not re-break it per level.
	bool restartRenderPass = (s_rave_upload_batch_depth == 0) &&
		RaveEndActiveRenderPassForTextureUpload(currentCtx);
	auto restartIfNeeded = [&]() {
		if (restartRenderPass) {
			RestartRenderPassWithLoad(currentCtx);
		}
	};

	id<MTLCommandQueue> queue = RaveTextureUploadCommandQueue(currentCtx);
	if (!queue) {
		RAVE_LOG("RaveUploadMipLevel: no command queue for texture upload");
		restartIfNeeded();
		return;
	}

	id<MTLDevice> device = tex.device;
	if (!device) {
		RAVE_LOG("RaveUploadMipLevel: texture has no device");
		restartIfNeeded();
		return;
	}

	NSUInteger bytesPerImage = (NSUInteger)bytesPerRow * (NSUInteger)height;
	id<MTLBuffer> staging = [device newBufferWithLength:bytesPerImage
	                                            options:MTLResourceStorageModeShared];
	if (!staging) {
		RAVE_LOG("RaveUploadMipLevel: failed to allocate %lu byte staging buffer",
		         (unsigned long)bytesPerImage);
		restartIfNeeded();
		return;
	}
	memcpy([staging contents], pixelData, bytesPerImage);

	id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
	id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];
	if (!cmdBuf || !blit) {
		RAVE_LOG("RaveUploadMipLevel: failed to create upload command buffer");
		restartIfNeeded();
		return;
	}

	[blit copyFromBuffer:staging
	        sourceOffset:0
	   sourceBytesPerRow:(NSUInteger)bytesPerRow
	 sourceBytesPerImage:bytesPerImage
	          sourceSize:MTLSizeMake((NSUInteger)width, (NSUInteger)height, 1)
	           toTexture:tex
	    destinationSlice:0
	    destinationLevel:(NSUInteger)level
	   destinationOrigin:MTLOriginMake(0, 0, 0)];
	[blit endEncoding];

	id<MTLBuffer> keepAliveStaging = staging;
	id<MTLTexture> keepAliveTexture = tex;
	[cmdBuf addCompletedHandler:^(id<MTLCommandBuffer> completedCommandBuffer) {
		(void)completedCommandBuffer;
		(void)keepAliveStaging;
		(void)keepAliveTexture;
	}];
	[cmdBuf commit];
	restartIfNeeded();
}


// generateMipmapsForTexture: generates full mip chain from level 0.
// Requires MTLTextureUsageShaderWrite on the texture (fixed in RaveCreateMetalTexture).
void RaveGenerateMipmaps(void *metalTexture)
{
	if (!metalTexture) return;
	id<MTLTexture> tex = (__bridge id<MTLTexture>)metalTexture;
	id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)SharedMetalCommandQueue();
	if (!queue) return;

	@autoreleasepool {
		id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cmdBuf blitCommandEncoder];
		[blit generateMipmapsForTexture:tex];
		[blit endEncoding];
		[cmdBuf commit];
		[cmdBuf waitUntilCompleted];
	}
}


void RaveReleaseTexture(void *metalTexture)
{
	if (metalTexture) {
		id<MTLTexture> tex = (__bridge_transfer id<MTLTexture>)metalTexture;
		tex = nil;  // ARC releases
	}
}


/*
 *  RaveRefreshTextureFromPixmap - re-read pixmap and re-upload to Metal texture
 *
 *  Called for deferred direct-format textures that were empty when first
 *  realized. Standard QATextureNew has copy semantics unless kQATexture_NoCopy
 *  is explicitly used; since Bugdom's calls have flags=0, stop polling once
 *  non-empty data is captured so later heap reuse cannot corrupt the texture.
 */
void RaveRefreshTextureFromPixmap(RaveResourceEntry *entry)
{
	if (!entry || !entry->metal_texture || entry->pixmap_mac_addr == 0) return;

	uint32_t w = entry->width;
	uint32_t h = entry->height;
	uint32_t pixelType = entry->pixel_type;
	uint32_t rowBytes = entry->row_bytes;

	// Prefer cpu_pixel_data when a
	// Q3Pixmap_Set_Image intercept has populated it. Same logic as
	// RaveRealizeDeferredTexture — the pixmap_mac_addr path reads
	// potentially-stale heap data for titles (e.g. Bugdom) that
	// free the source buffer after QATextureNew.
	uint32_t pixmap = entry->pixmap_mac_addr;
	if (entry->cpu_pixel_data_is_authoritative &&
	    entry->cpu_pixel_mac_addr != 0 &&
	    entry->cpu_pixel_data_size > 0) {
		pixmap = entry->cpu_pixel_mac_addr;
	}

	// Re-read until non-empty data appears.
	uint8_t *expanded = new uint8_t[w * h * 4];
	ConvertPixels(pixelType, pixmap, expanded, w, h, rowBytes);
	const bool whitenedAlphaMask =
		(pixelType == 4) && RaveBGRAWhitenAlphaOnlyMask(expanded, w * h);  // kQAPixel_ARGB32

	// Check the whole converted image. ARGB16 sprites can have transparent
	// leading scanlines, so a top-left sample is not enough to decide whether
	// the texture has received real data.
	RaveBGRAImageStats sourceStats = RaveBGRAImageAnalyze(expanded, w * h);
	bool hasData = (sourceStats.nonzero != 0);
	entry->diag_alpha_zero = (w * h) - sourceStats.alpha;
	entry->diag_index_zero = 0;
	entry->diag_rgb_nonzero = sourceStats.rgb;

	// Replace Metal texture contents through the fenced staging+blit path
	// (RAVE-05): a raw replaceRegion CPU-writes a shared texture that prior
	// committed in-flight frames may still be sampling. While the source is
	// still empty the texture already holds the zeros uploaded when it was
	// realized, so skip the byte-identical re-upload — it would otherwise
	// break the render pass on every draw that polls this texture.
	if (hasData) {
		RaveUploadMipLevel(entry->metal_texture, 0, w, h, expanded, w * 4);
	}

	delete[] expanded;

	if (hasData && !entry->pixels_copied) {
		// First non-empty data observed. Under R2 authoritative read path
		// `pixmap == cpu_pixel_mac_addr`, so this copy is a no-op
		// (self-copy). Under the classic path it mirrors the pixmap data
		// into cpu_pixel_data for AccessTexture consumers. No further refresh
		// is needed for normal copy-semantics textures.
		if (entry->cpu_pixel_data && entry->cpu_pixel_mac_addr) {
			Host2Mac_memcpy(entry->cpu_pixel_mac_addr, Mac2HostAddr(pixmap), entry->cpu_pixel_data_size);
		}
		entry->pixels_copied = true;
		RAVE_LOG("TextureRefresh: pixelType=%d %dx%d pixmap=0x%08x -> first non-empty data observed nz=%u a=%u rgb=%u white=%u first[nz/a/rgb]=%u/%u/%u alphaMaskWhite=%d",
		       pixelType, w, h, pixmap,
		       sourceStats.nonzero, sourceStats.alpha, sourceStats.rgb, sourceStats.white,
		       sourceStats.first_nonzero, sourceStats.first_alpha, sourceStats.first_rgb,
		       whitenedAlphaMask);
	}
}


/*
 *  ATI RaveExtFuncs: clearDrawBuffer / clearZBuffer
 *
 *  These are the ATI extension versions of the standard RAVE clear methods.
 *  They take only 2 args (drawContext, rect) instead of 3 (no initialContextAddr).
 *  Delegate to the existing 3-arg implementations with initialContextAddr=0,
 *  which causes them to use the draw context's own state tags for clear values.
 */

int32_t NativeATIClearDrawBuffer(uint32_t drawContextAddr, uint32_t rectAddr)
{
	return NativeClearDrawBuffer(drawContextAddr, rectAddr, 0);
}

int32_t NativeATIClearZBuffer(uint32_t drawContextAddr, uint32_t rectAddr)
{
	return NativeClearZBuffer(drawContextAddr, rectAddr, 0);
}
