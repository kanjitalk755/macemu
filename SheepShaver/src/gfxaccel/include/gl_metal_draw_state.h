/*
 *  gl_metal_draw_state.h - OpenGL fixed-function draw state helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef GL_METAL_DRAW_STATE_H
#define GL_METAL_DRAW_STATE_H

#include <cmath>
#include <cstdint>

#include "overlay_clear_policy.h"

/*
 * Fixed-function draw state helpers shared by the renderer and focused tests.
 *
 * glActiveTextureARB selects the texture unit modified by subsequent texture
 * state calls. It does not move legacy GL_TEXTURE_2D sampling for texcoord0
 * away from unit 0. Unit 1 remains a separate multitexture input.
 */
static inline int GLMetalPrimaryTextureUnitForDraw(int active_texture)
{
	(void)active_texture;
	return 0;
}

static inline unsigned GLMetalOverlayColorWriteMask(bool red, bool green,
                                                    bool blue, bool alpha)
{
	return (red ? 1u : 0u) | (green ? 2u : 0u) |
	       (blue ? 4u : 0u) | (alpha ? 8u : 0u);
}

static inline unsigned GLMetalGuestColorWriteMask(bool red, bool green,
                                                  bool blue, bool alpha)
{
	return (red ? 1u : 0u) | (green ? 2u : 0u) |
	       (blue ? 4u : 0u) | (alpha ? 8u : 0u);
}

static inline unsigned GLMetalDrawableColorWriteMask(bool is_offscreen,
                                                     bool red, bool green,
                                                     bool blue, bool alpha)
{
	if (is_offscreen)
		return GLMetalGuestColorWriteMask(red, green, blue, alpha);
	return GLMetalOverlayColorWriteMask(red, green, blue, alpha);
}

static inline float GLMetalOverlayClearAlpha(bool is_offscreen,
                                             float guest_clear_alpha)
{
	return OverlayClearEffectiveAlpha(is_offscreen, guest_clear_alpha);
}

static inline float GLMetalOverlayClearColorComponent(
	bool is_offscreen, float guest_clear_component, float effective_clear_alpha)
{
	if (is_offscreen)
		return guest_clear_component;
	return OverlayClearPremultipliedComponent(guest_clear_component,
	                                          effective_clear_alpha);
}

static inline bool GLMetalForceOpaqueOverlayOutput(bool is_offscreen,
                                                   bool blend_enabled)
{
	return !is_offscreen && !blend_enabled;
}

struct GLMetalRenderTargetSize {
	uint32_t width;
	uint32_t height;
};

static inline GLMetalRenderTargetSize GLMetalChooseRenderTargetSize(
	bool is_offscreen, uint32_t drawable_w, uint32_t drawable_h,
	uint32_t viewport_w, uint32_t viewport_h)
{
	if (is_offscreen && drawable_w != 0 && drawable_h != 0)
		return {drawable_w, drawable_h};
	return {viewport_w, viewport_h};
}

static inline bool GLMetalFrontFacingWindingIsCounterClockwise(unsigned gl_front_face)
{
	return gl_front_face == 0x0901u; /* GL_CCW */
}

static inline bool GLMetalDepthAttachmentShouldStoreForReadback()
{
	return true;
}

static inline bool GLMetalPreparedDrawStateSurvivesDisable(
	uint32_t last_enable_draw_serial, uint32_t completed_draw_serial,
	bool had_resource)
{
	return had_resource && last_enable_draw_serial == completed_draw_serial;
}

static inline bool GLMetalLatchedTexture2DSurvivesDisable(
	uint32_t last_enable_draw_serial, uint32_t completed_draw_serial,
	bool had_resource)
{
	if (!had_resource)
		return false;
	/* Latch ONLY the trailing-draw pattern: enable -> specify -> disable with
	 * NO draw in between (the pending draw arrives after the teardown). If a
	 * draw already consumed the enable (serial advanced), the bracket was
	 * well-formed and the disable is authoritative — latching past it leaks
	 * the unit onto every later draw (Quake 3: world lightmap unit 1 glued
	 * onto weapon/HUD/menu with a stale texcoord pointer, per-frame flicker). */
	return completed_draw_serial == last_enable_draw_serial;
}

static inline bool GLMetalTexCoordArrayAvailableForArrayDraw(
	bool current_enabled, bool prepared_for_draw,
	bool latched_texture_for_array_draw, bool retained_pointer)
{
	return current_enabled ||
	       prepared_for_draw ||
	       (latched_texture_for_array_draw && retained_pointer);
}

static inline bool GLMetalTexture2DEnabledForArrayDraw(
	bool current_enabled, bool prepared_for_draw, bool latched_for_array_draw,
	bool bound_texture, bool texcoord_available)
{
	if (current_enabled)
		return true;
	if (prepared_for_draw)
		return texcoord_available;
	return latched_for_array_draw && bound_texture && texcoord_available;
}

static inline bool GLMetalLatchedTexture2DSurvivesTextureRebind(
	bool latched_for_array_draw, bool binding_nonzero_texture)
{
	return latched_for_array_draw && binding_nonzero_texture;
}

static inline void GLMetalCopyClipPlaneToUniform(float out[4],
                                                 const double plane[4])
{
	if (!out || !plane)
		return;
	for (int i = 0; i < 4; i++)
		out[i] = (float)plane[i];
}

static inline bool GLMetalFloatNearlyEqual(float a, float b, float epsilon)
{
	const float delta = a - b;
	return delta <= epsilon && delta >= -epsilon;
}

static inline bool GLMetalProjectPointToNDC(const float m[16], float x, float y,
                                            float z, float w, float out_ndc[3])
{
	if (m == 0)
		return false;
	const float cx = m[0] * x + m[4] * y + m[8] * z + m[12] * w;
	const float cy = m[1] * x + m[5] * y + m[9] * z + m[13] * w;
	const float cz = m[2] * x + m[6] * y + m[10] * z + m[14] * w;
	const float cw = m[3] * x + m[7] * y + m[11] * z + m[15] * w;
	if (!std::isfinite(cx) || !std::isfinite(cy) ||
	    !std::isfinite(cz) || !std::isfinite(cw) ||
	    std::fabs(cw) < 1.0e-20f) {
		return false;
	}
	out_ndc[0] = cx / cw;
	out_ndc[1] = cy / cw;
	out_ndc[2] = cz / cw;
	return std::isfinite(out_ndc[0]) && std::isfinite(out_ndc[1]) &&
	       std::isfinite(out_ndc[2]);
}

static inline bool GLMetalMatrixActsLikeIdentityForPixelBounds(
	const float mvp[16], float min_x, float max_x, float min_y, float max_y,
	float min_z, float max_z)
{
	const float z = (min_z + max_z) * 0.5f;
	const float epsilon = 0.01f;
	const float corners[4][2] = {
		{min_x, min_y},
		{max_x, min_y},
		{min_x, max_y},
		{max_x, max_y},
	};
	for (int i = 0; i < 4; i++) {
		float ndc[3];
		if (!GLMetalProjectPointToNDC(
			    mvp, corners[i][0], corners[i][1], z, 1.0f, ndc)) {
			return false;
		}
		if (!GLMetalFloatNearlyEqual(ndc[0], corners[i][0], epsilon) ||
		    !GLMetalFloatNearlyEqual(ndc[1], corners[i][1], epsilon) ||
		    !GLMetalFloatNearlyEqual(ndc[2], z, epsilon)) {
			return false;
		}
	}
	return true;
}

static inline bool GLMetalIdentityPixelProjectionApplies(
	const float mvp[16], bool has_texture, bool depth_test, bool lighting_enabled,
	uint32_t viewport_w, uint32_t viewport_h, float min_x, float max_x,
	float min_y, float max_y, float min_z, float max_z)
{
	if (!has_texture || depth_test || lighting_enabled ||
	    viewport_w == 0 || viewport_h == 0) {
		return false;
	}

	if (min_z < -0.0001f || max_z > 0.0001f)
		return false;

	const bool already_clip_like =
		min_x >= -1.0f && max_x <= 1.0f && min_y >= -1.0f && max_y <= 1.0f;
	if (already_clip_like)
		return false;

	const float margin = 1.0f;
	if (min_x < -margin || min_y < -margin ||
	    max_x > (float)viewport_w + margin ||
	    max_y > (float)viewport_h + margin) {
		return false;
	}

	return GLMetalMatrixActsLikeIdentityForPixelBounds(
		mvp, min_x, max_x, min_y, max_y, min_z, max_z);
}

static inline float GLMetalPixelToClipX(float x, uint32_t viewport_w)
{
	if (viewport_w == 0)
		return x;
	return (x / (float)viewport_w) * 2.0f - 1.0f;
}

static inline float GLMetalPixelToClipY(float y, uint32_t viewport_h)
{
	if (viewport_h == 0)
		return y;
	return (y / (float)viewport_h) * 2.0f - 1.0f;
}

#endif /* GL_METAL_DRAW_STATE_H */
