/*
 *  gl_metal_coordinates.h - Pure coordinate helpers for GL-on-Metal rendering
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  These helpers keep GL's bottom-left window coordinate semantics separate
 *  from Metal's top-left viewport/scissor coordinate system.
 */

#ifndef GL_METAL_COORDINATES_H
#define GL_METAL_COORDINATES_H

#include <algorithm>
#include <cmath>
#include <cstdint>

struct GLMetalViewportRect {
	double origin_x;
	double origin_y;
	double width;
	double height;
	double znear;
	double zfar;
};

struct GLMetalScissorRect {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	bool valid;
};

struct GLMetalPixelQuadVertex {
	float x;
	float y;
	float u;
	float v;
};

static inline GLMetalViewportRect GLMetalMakeViewportRect(
	double gl_x, double gl_y, double gl_w, double gl_h,
	double target_h, double znear, double zfar)
{
	GLMetalViewportRect rect;
	rect.origin_x = gl_x;
	rect.origin_y = target_h - (gl_y + gl_h);
	rect.width = gl_w;
	rect.height = gl_h;
	rect.znear = znear;
	rect.zfar = zfar;
	return rect;
}

static inline GLMetalScissorRect GLMetalMakeScissorRect(
	int32_t gl_x, int32_t gl_y, int32_t gl_w, int32_t gl_h,
	uint32_t target_w, uint32_t target_h)
{
	GLMetalScissorRect rect = {0, 0, 0, 0, false};
	if (gl_w <= 0 || gl_h <= 0 || target_w == 0 || target_h == 0) return rect;

	int64_t x0 = gl_x;
	int64_t x1 = (int64_t)gl_x + gl_w;
	int64_t y0 = (int64_t)target_h - ((int64_t)gl_y + gl_h);
	int64_t y1 = y0 + gl_h;

	x0 = std::max<int64_t>(0, std::min<int64_t>(x0, target_w));
	x1 = std::max<int64_t>(0, std::min<int64_t>(x1, target_w));
	y0 = std::max<int64_t>(0, std::min<int64_t>(y0, target_h));
	y1 = std::max<int64_t>(0, std::min<int64_t>(y1, target_h));
	if (x1 <= x0 || y1 <= y0) return rect;

	rect.x = (uint32_t)x0;
	rect.y = (uint32_t)y0;
	rect.width = (uint32_t)(x1 - x0);
	rect.height = (uint32_t)(y1 - y0);
	rect.valid = true;
	return rect;
}

static inline uint32_t GLMetalTextureYForGLRead(
	int32_t gl_y, int32_t read_h, uint32_t target_h)
{
	if (read_h <= 0 || target_h == 0) return 0;

	const int64_t max_y = (target_h > (uint32_t)read_h)
		? (int64_t)target_h - read_h
		: 0;
	int64_t metal_y = (int64_t)target_h - ((int64_t)gl_y + read_h);
	metal_y = std::max<int64_t>(0, std::min<int64_t>(metal_y, max_y));
	return (uint32_t)metal_y;
}

static inline uint16_t GLMetalDepthFloatToUnsignedShort(float depth)
{
	if (!std::isfinite(depth)) return 0xffffu;
	if (depth <= 0.0f) return 0x0000u;
	if (depth >= 1.0f) return 0xffffu;
	return (uint16_t)(depth * 65535.0f + 0.5f);
}

static inline void GLMetalBuildPixelQuadVertices(
	GLMetalPixelQuadVertex out[4],
	float ndc_x0, float ndc_y0, float ndc_x1, float ndc_y1)
{
	const float v_at_origin = (ndc_y1 < ndc_y0) ? 0.0f : 1.0f;
	const float v_at_extent = 1.0f - v_at_origin;

	out[0] = {ndc_x0, ndc_y0, 0.0f, v_at_origin};
	out[1] = {ndc_x1, ndc_y0, 1.0f, v_at_origin};
	out[2] = {ndc_x0, ndc_y1, 0.0f, v_at_extent};
	out[3] = {ndc_x1, ndc_y1, 1.0f, v_at_extent};
}

#endif /* GL_METAL_COORDINATES_H */
