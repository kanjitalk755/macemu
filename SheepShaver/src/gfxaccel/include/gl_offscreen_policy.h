/*
 *  gl_offscreen_policy.h - OpenGL offscreen drawable analysis helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef GL_OFFSCREEN_POLICY_H
#define GL_OFFSCREEN_POLICY_H

#include <stdint.h>

typedef struct GLOffscreenBGRAStats {
	uint64_t total_pixels;
	uint64_t rgb_nonzero_pixels;
	uint64_t alpha_nonzero_pixels;
	uint64_t alpha_opaque_pixels;
	uint32_t rgb_min_x;
	uint32_t rgb_min_y;
	uint32_t rgb_max_x;
	uint32_t rgb_max_y;
	uint32_t alpha_min_x;
	uint32_t alpha_min_y;
	uint32_t alpha_max_x;
	uint32_t alpha_max_y;
	bool rgb_bounds_valid;
	bool alpha_bounds_valid;
} GLOffscreenBGRAStats;

typedef struct GLOffscreenCompositeRect {
	bool valid;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
} GLOffscreenCompositeRect;

typedef struct GLOverlayLayerRect {
	float dst_origin_x;
	float dst_origin_y;
	float dst_size_w;
	float dst_size_h;
	bool framebuffer_space;
} GLOverlayLayerRect;

static inline uint32_t GLOffscreenDrawableBytesPerPixel(uint32_t width,
                                                        uint32_t rowbytes)
{
	if (width == 0) return 0;

	const uint64_t min_32bpp_rowbytes = (uint64_t)width * 4u;
	const uint64_t min_16bpp_rowbytes = (uint64_t)width * 2u;
	if ((uint64_t)rowbytes >= min_32bpp_rowbytes) return 4;
	if ((uint64_t)rowbytes >= min_16bpp_rowbytes) return 2;
	return 0;
}

static inline bool GLShouldAcceptOffscreenDrawable(uint32_t width,
                                                   uint32_t height,
                                                   uint32_t rowbytes,
                                                   uint32_t baseaddr)
{
	return width != 0 &&
	       height != 0 &&
	       GLOffscreenDrawableBytesPerPixel(width, rowbytes) != 0 &&
	       baseaddr != 0;
}

static inline bool GLShouldReadbackOffscreenDrawable(bool is_offscreen,
                                                     uint32_t width,
                                                     uint32_t height,
                                                     uint32_t rowbytes,
                                                     uint32_t baseaddr)
{
	return is_offscreen &&
	       GLShouldAcceptOffscreenDrawable(width, height, rowbytes, baseaddr);
}

static inline bool GLShouldPreserveOverlayBindingAfterResourceDetach(
	bool drawable_bound,
	int32_t dst_width,
	int32_t dst_height)
{
	return drawable_bound && dst_width > 0 && dst_height > 0;
}

static inline bool GLShouldReacquireBoundOverlayForPresent(
	bool cached_overlay,
	bool drawable_bound,
	int32_t dst_width,
	int32_t dst_height)
{
	return !cached_overlay &&
	       GLShouldPreserveOverlayBindingAfterResourceDetach(drawable_bound,
	                                                         dst_width,
	                                                         dst_height);
}

static inline bool GLShouldSubmitOverlayFrame(bool cached_overlay,
                                              bool committed_frame)
{
	return cached_overlay && committed_frame;
}

static inline bool GLShouldBeginRenderPassForClear(uint32_t clear_mask)
{
	/* GL_COLOR_BUFFER_BIT. Depth/stencil-only clears are not visible frames. */
	return (clear_mask & 0x00004000u) != 0;
}

static inline bool GLShouldClearColorAttachment(uint32_t clear_mask)
{
	return (clear_mask & 0x00004000u) != 0;
}

static inline bool GLShouldClearDepthAttachment(uint32_t clear_mask)
{
	return (clear_mask & 0x00000100u) != 0;
}

static inline bool GLShouldClearStencilAttachment(uint32_t clear_mask)
{
	return (clear_mask & 0x00000400u) != 0;
}

static inline bool GLShouldApplyAttachmentClear(uint32_t clear_mask)
{
	return GLShouldClearColorAttachment(clear_mask) ||
	       GLShouldClearDepthAttachment(clear_mask) ||
	       GLShouldClearStencilAttachment(clear_mask);
}

static inline GLOverlayLayerRect GLMakeOverlayLayerRect(
	int32_t dst_left,
	int32_t dst_top,
	int32_t dst_width,
	int32_t dst_height,
	uint32_t overlay_width,
	uint32_t overlay_height,
	uint32_t framebuffer_width,
	uint32_t framebuffer_height)
{
	GLOverlayLayerRect rect;
	rect.dst_origin_x = (float)dst_left;
	rect.dst_origin_y = (float)dst_top;
	rect.dst_size_w = (float)dst_width;
	rect.dst_size_h = (float)dst_height;
	rect.framebuffer_space = false;

	if (overlay_width != 0 &&
	    overlay_height != 0 &&
	    framebuffer_width != 0 &&
	    framebuffer_height != 0 &&
	    dst_width > 0 &&
	    dst_height > 0 &&
	    overlay_width == framebuffer_width &&
	    overlay_height == framebuffer_height &&
	    ((uint32_t)dst_width != framebuffer_width ||
	     (uint32_t)dst_height != framebuffer_height)) {
		rect.dst_origin_x = 0.0f;
		rect.dst_origin_y = 0.0f;
		rect.dst_size_w = (float)framebuffer_width;
		rect.dst_size_h = (float)framebuffer_height;
		rect.framebuffer_space = true;
	}

	return rect;
}

static inline bool GLShouldCompositeOffscreenIntoGuestSurface(
	bool readback_valid,
	bool alpha_bounds_valid,
	uint32_t readback_baseaddr,
	uint32_t readback_width,
	uint32_t readback_height,
	uint32_t readback_bytes_per_pixel,
	uint32_t dst_baseaddr,
	uint32_t dst_rowbytes,
	uint32_t dst_depth_bits)
{
	return readback_valid &&
	       alpha_bounds_valid &&
	       readback_baseaddr != 0 &&
	       readback_width != 0 &&
	       readback_height != 0 &&
	       readback_bytes_per_pixel == 2 &&
	       dst_baseaddr != 0 &&
	       dst_baseaddr != readback_baseaddr &&
	       dst_depth_bits == 16 &&
	       GLOffscreenDrawableBytesPerPixel(readback_width, dst_rowbytes) == 2;
}

static inline bool GLShouldBridgeOffscreenAfterNQDFrontBlt(
	bool main_device_valid,
	uint32_t main_device_baseaddr,
	int32_t main_device_rowbytes,
	uint32_t src_baseaddr,
	int32_t src_rowbytes,
	uint32_t src_depth_bits,
	uint32_t dst_baseaddr,
	int32_t dst_rowbytes,
	uint32_t dst_depth_bits,
	uint32_t transfer_mode)
{
	return main_device_valid &&
	       main_device_baseaddr != 0 &&
	       main_device_rowbytes > 0 &&
	       dst_baseaddr == main_device_baseaddr &&
	       dst_rowbytes == main_device_rowbytes &&
	       src_baseaddr != 0 &&
	       src_baseaddr != dst_baseaddr &&
	       src_rowbytes >= dst_rowbytes &&
	       transfer_mode == 0 &&
	       src_depth_bits == 16 &&
	       dst_depth_bits == 16;
}

static inline bool GLShouldCompositeOffscreenDirtyRect(
	bool alpha_bounds_valid,
	uint32_t alpha_min_x,
	uint32_t alpha_min_y,
	uint32_t alpha_max_x,
	uint32_t alpha_max_y,
	int32_t dirty_x,
	int32_t dirty_y,
	int32_t dirty_width,
	int32_t dirty_height)
{
	if (!alpha_bounds_valid ||
	    alpha_max_x < alpha_min_x ||
	    alpha_max_y < alpha_min_y ||
	    dirty_width <= 0 ||
	    dirty_height <= 0) {
		return false;
	}

	const int64_t alpha_left = alpha_min_x;
	const int64_t alpha_top = alpha_min_y;
	const int64_t alpha_right = (int64_t)alpha_max_x + 1;
	const int64_t alpha_bottom = (int64_t)alpha_max_y + 1;
	const int64_t dirty_left = dirty_x;
	const int64_t dirty_top = dirty_y;
	const int64_t dirty_right = (int64_t)dirty_x + dirty_width;
	const int64_t dirty_bottom = (int64_t)dirty_y + dirty_height;

	return dirty_left < alpha_right &&
	       dirty_right > alpha_left &&
	       dirty_top < alpha_bottom &&
	       dirty_bottom > alpha_top;
}

static inline bool GLDirtyRectCoversSurface(int32_t dirty_x,
                                            int32_t dirty_y,
                                            int32_t dirty_width,
                                            int32_t dirty_height,
                                            uint32_t surface_width,
                                            uint32_t surface_height)
{
	if (dirty_width <= 0 ||
	    dirty_height <= 0 ||
	    surface_width == 0 ||
	    surface_height == 0) {
		return false;
	}

	const int64_t dirty_left = dirty_x;
	const int64_t dirty_top = dirty_y;
	const int64_t dirty_right = (int64_t)dirty_x + dirty_width;
	const int64_t dirty_bottom = (int64_t)dirty_y + dirty_height;

	return dirty_left <= 0 &&
	       dirty_top <= 0 &&
	       dirty_right >= (int64_t)surface_width &&
	       dirty_bottom >= (int64_t)surface_height;
}

static inline uint64_t GLDirtyRectAlphaIntersectionArea(
	uint32_t alpha_min_x,
	uint32_t alpha_min_y,
	uint32_t alpha_max_x,
	uint32_t alpha_max_y,
	int32_t dirty_x,
	int32_t dirty_y,
	int32_t dirty_width,
	int32_t dirty_height)
{
	if (alpha_max_x < alpha_min_x ||
	    alpha_max_y < alpha_min_y ||
	    dirty_width <= 0 ||
	    dirty_height <= 0) {
		return 0;
	}

	const int64_t alpha_left = alpha_min_x;
	const int64_t alpha_top = alpha_min_y;
	const int64_t alpha_right = (int64_t)alpha_max_x + 1;
	const int64_t alpha_bottom = (int64_t)alpha_max_y + 1;
	const int64_t dirty_left = dirty_x;
	const int64_t dirty_top = dirty_y;
	const int64_t dirty_right = (int64_t)dirty_x + dirty_width;
	const int64_t dirty_bottom = (int64_t)dirty_y + dirty_height;
	const int64_t clipped_left =
		dirty_left > alpha_left ? dirty_left : alpha_left;
	const int64_t clipped_top =
		dirty_top > alpha_top ? dirty_top : alpha_top;
	const int64_t clipped_right =
		dirty_right < alpha_right ? dirty_right : alpha_right;
	const int64_t clipped_bottom =
		dirty_bottom < alpha_bottom ? dirty_bottom : alpha_bottom;

	if (clipped_left >= clipped_right || clipped_top >= clipped_bottom) {
		return 0;
	}
	return (uint64_t)(clipped_right - clipped_left) *
	       (uint64_t)(clipped_bottom - clipped_top);
}

static inline uint64_t GLOffscreenAlphaBoundsArea(uint32_t alpha_min_x,
                                                  uint32_t alpha_min_y,
                                                  uint32_t alpha_max_x,
                                                  uint32_t alpha_max_y)
{
	if (alpha_max_x < alpha_min_x || alpha_max_y < alpha_min_y) {
		return 0;
	}
	return (uint64_t)((uint64_t)alpha_max_x - alpha_min_x + 1u) *
	       (uint64_t)((uint64_t)alpha_max_y - alpha_min_y + 1u);
}

static inline bool GLDirtyRectFullyCoversAlphaBounds(
	uint32_t alpha_min_x,
	uint32_t alpha_min_y,
	uint32_t alpha_max_x,
	uint32_t alpha_max_y,
	int32_t dirty_x,
	int32_t dirty_y,
	int32_t dirty_width,
	int32_t dirty_height)
{
	if (alpha_max_x < alpha_min_x ||
	    alpha_max_y < alpha_min_y ||
	    dirty_width <= 0 ||
	    dirty_height <= 0) {
		return false;
	}

	const int64_t dirty_left = dirty_x;
	const int64_t dirty_top = dirty_y;
	const int64_t dirty_right = (int64_t)dirty_x + dirty_width;
	const int64_t dirty_bottom = (int64_t)dirty_y + dirty_height;
	const int64_t alpha_left = alpha_min_x;
	const int64_t alpha_top = alpha_min_y;
	const int64_t alpha_right = (int64_t)alpha_max_x + 1;
	const int64_t alpha_bottom = (int64_t)alpha_max_y + 1;

	return dirty_left <= alpha_left &&
	       dirty_top <= alpha_top &&
	       dirty_right >= alpha_right &&
	       dirty_bottom >= alpha_bottom;
}

static inline bool GLShouldCompositeCurrentOffscreenDirtyRect(
	bool using_dirty_rect,
	uint32_t alpha_min_x,
	uint32_t alpha_min_y,
	uint32_t alpha_max_x,
	uint32_t alpha_max_y,
	int32_t dirty_x,
	int32_t dirty_y,
	int32_t dirty_width,
	int32_t dirty_height,
	uint32_t surface_width,
	uint32_t surface_height)
{
	if (alpha_max_x < alpha_min_x || alpha_max_y < alpha_min_y) {
		return false;
	}
	if (!using_dirty_rect) return true;
	if (GLDirtyRectCoversSurface(dirty_x,
	                             dirty_y,
	                             dirty_width,
	                             dirty_height,
	                             surface_width,
	                             surface_height)) {
		return true;
	}
	if (!GLShouldCompositeOffscreenDirtyRect(true,
	                                        alpha_min_x,
	                                        alpha_min_y,
	                                        alpha_max_x,
	                                        alpha_max_y,
	                                        dirty_x,
	                                        dirty_y,
	                                        dirty_width,
	                                        dirty_height)) {
		return false;
	}
	const uint64_t alpha_area =
		GLOffscreenAlphaBoundsArea(alpha_min_x,
		                           alpha_min_y,
		                           alpha_max_x,
		                           alpha_max_y);
	const uint64_t dirty_area =
		(uint64_t)(uint32_t)dirty_width * (uint32_t)dirty_height;
	const uint64_t intersection_area =
		GLDirtyRectAlphaIntersectionArea(alpha_min_x,
		                                  alpha_min_y,
		                                  alpha_max_x,
		                                  alpha_max_y,
		                                  dirty_x,
		                                  dirty_y,
		                                  dirty_width,
		                                  dirty_height);
	if (alpha_area == 0 || dirty_area == 0 || intersection_area == 0) {
		return false;
	}
	if (GLDirtyRectFullyCoversAlphaBounds(alpha_min_x,
	                                      alpha_min_y,
	                                      alpha_max_x,
	                                      alpha_max_y,
	                                      dirty_x,
	                                      dirty_y,
	                                      dirty_width,
	                                      dirty_height) &&
	    dirty_area > alpha_area * 2u) {
		return false;
	}

	return intersection_area * 5u >= alpha_area * 4u ||
	       intersection_area * 3u >= dirty_area * 2u;
}

static inline bool GLDirtyRectFullyCoversOffscreenComposite(
	int32_t dirty_x,
	int32_t dirty_y,
	int32_t dirty_width,
	int32_t dirty_height,
	uint32_t composite_x,
	uint32_t composite_y,
	uint32_t composite_width,
	uint32_t composite_height)
{
	if (dirty_width <= 0 ||
	    dirty_height <= 0 ||
	    composite_width == 0 ||
	    composite_height == 0) {
		return false;
	}

	const int64_t dirty_left = dirty_x;
	const int64_t dirty_top = dirty_y;
	const int64_t dirty_right = (int64_t)dirty_x + dirty_width;
	const int64_t dirty_bottom = (int64_t)dirty_y + dirty_height;
	const int64_t composite_left = composite_x;
	const int64_t composite_top = composite_y;
	const int64_t composite_right =
		(int64_t)composite_x + composite_width;
	const int64_t composite_bottom =
		(int64_t)composite_y + composite_height;

	return dirty_left <= composite_left &&
	       dirty_top <= composite_top &&
	       dirty_right >= composite_right &&
	       dirty_bottom >= composite_bottom;
}

static inline bool GLDirtyRectIntersectsOffscreenComposite(
	int32_t dirty_x,
	int32_t dirty_y,
	int32_t dirty_width,
	int32_t dirty_height,
	uint32_t composite_x,
	uint32_t composite_y,
	uint32_t composite_width,
	uint32_t composite_height)
{
	if (composite_width == 0 || composite_height == 0) return false;
	return GLShouldCompositeOffscreenDirtyRect(
		true,
		composite_x,
		composite_y,
		composite_x + composite_width - 1u,
		composite_y + composite_height - 1u,
		dirty_x,
		dirty_y,
		dirty_width,
		dirty_height);
}

static inline GLOffscreenCompositeRect GLOffscreenCompositeRectForDirty(
	bool using_dirty_rect,
	uint32_t alpha_min_x,
	uint32_t alpha_min_y,
	uint32_t alpha_max_x,
	uint32_t alpha_max_y,
	int32_t dirty_x,
	int32_t dirty_y,
	int32_t dirty_width,
	int32_t dirty_height)
{
	GLOffscreenCompositeRect rect = {false, 0, 0, 0, 0};
	if (alpha_max_x < alpha_min_x || alpha_max_y < alpha_min_y) {
		return rect;
	}

	const int64_t alpha_left = alpha_min_x;
	const int64_t alpha_top = alpha_min_y;
	const int64_t alpha_right = (int64_t)alpha_max_x + 1;
	const int64_t alpha_bottom = (int64_t)alpha_max_y + 1;

	if (!using_dirty_rect) {
		rect.valid = true;
		rect.x = alpha_min_x;
		rect.y = alpha_min_y;
		rect.width = (uint32_t)(alpha_right - alpha_left);
		rect.height = (uint32_t)(alpha_bottom - alpha_top);
		return rect;
	}

	if (dirty_width <= 0 || dirty_height <= 0) return rect;

	const int64_t dirty_left = dirty_x;
	const int64_t dirty_top = dirty_y;
	const int64_t dirty_right = (int64_t)dirty_x + dirty_width;
	const int64_t dirty_bottom = (int64_t)dirty_y + dirty_height;
	const int64_t clipped_left =
		dirty_left > alpha_left ? dirty_left : alpha_left;
	const int64_t clipped_top =
		dirty_top > alpha_top ? dirty_top : alpha_top;
	const int64_t clipped_right =
		dirty_right < alpha_right ? dirty_right : alpha_right;
	const int64_t clipped_bottom =
		dirty_bottom < alpha_bottom ? dirty_bottom : alpha_bottom;
	if (clipped_left >= clipped_right || clipped_top >= clipped_bottom) {
		return rect;
	}

	rect.valid = true;
	rect.x = (uint32_t)clipped_left;
	rect.y = (uint32_t)clipped_top;
	rect.width = (uint32_t)(clipped_right - clipped_left);
	rect.height = (uint32_t)(clipped_bottom - clipped_top);
	return rect;
}

static inline GLOffscreenCompositeRect GLOffscreenCurrentCompositeRectForDirty(
	bool using_dirty_rect,
	uint32_t alpha_min_x,
	uint32_t alpha_min_y,
	uint32_t alpha_max_x,
	uint32_t alpha_max_y,
	int32_t dirty_x,
	int32_t dirty_y,
	int32_t dirty_width,
	int32_t dirty_height,
	uint32_t surface_width,
	uint32_t surface_height)
{
	GLOffscreenCompositeRect rect = {false, 0, 0, 0, 0};
	if (!GLShouldCompositeCurrentOffscreenDirtyRect(using_dirty_rect,
	                                                alpha_min_x,
	                                                alpha_min_y,
	                                                alpha_max_x,
	                                                alpha_max_y,
	                                                dirty_x,
	                                                dirty_y,
	                                                dirty_width,
	                                                dirty_height,
	                                                surface_width,
	                                                surface_height)) {
		return rect;
	}

	return GLOffscreenCompositeRectForDirty(false,
	                                       alpha_min_x,
	                                       alpha_min_y,
	                                       alpha_max_x,
	                                       alpha_max_y,
	                                       0,
	                                       0,
	                                       0,
	                                       0);
}

static inline bool GLShouldInvalidateOffscreenReadbackAfterGLFlush(
	bool committed_command_buffer,
	bool readback_succeeded)
{
	return committed_command_buffer && !readback_succeeded;
}

static inline bool GLShouldInvalidateOffscreenReadbackAfterNoCommandBufferFlush(
	bool cached_readback_valid,
	uint64_t composite_count)
{
	return cached_readback_valid && composite_count > 0;
}

static inline uint16_t GLOffscreenPackRGB555BigEndian(uint8_t r,
                                                      uint8_t g,
                                                      uint8_t b)
{
	return (uint16_t)((((uint16_t)r >> 3) << 10) |
	                  (((uint16_t)g >> 3) <<  5) |
	                   ((uint16_t)b >> 3));
}

static inline uint16_t GLOffscreenPackARGB1555BigEndian(uint8_t r,
                                                        uint8_t g,
                                                        uint8_t b,
                                                        uint8_t a)
{
	const uint16_t alpha = (a >= 128) ? 0x8000u : 0u;
	return (uint16_t)(alpha | GLOffscreenPackRGB555BigEndian(r, g, b));
}

static inline void GLOffscreenStoreRGB555Bytes(uint16_t pixel,
                                               uint8_t out_bytes[2])
{
	out_bytes[0] = (uint8_t)(pixel >> 8);
	out_bytes[1] = (uint8_t)(pixel & 0xffu);
}

static inline uint16_t GLOffscreenLoadRGB555Bytes(const uint8_t bytes[2])
{
	return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static inline bool GLOffscreenARGB1555PixelHasAlpha(uint16_t pixel)
{
	return (pixel & 0x8000u) != 0;
}

static inline uint16_t GLOffscreenRGB555WithoutAlpha(uint16_t pixel)
{
	return (uint16_t)(pixel & 0x7fffu);
}

static inline void GLOffscreenStoreARGB8888Bytes(uint8_t r,
                                                 uint8_t g,
                                                 uint8_t b,
                                                 uint8_t a,
                                                 uint8_t out_bytes[4])
{
	out_bytes[0] = a;
	out_bytes[1] = r;
	out_bytes[2] = g;
	out_bytes[3] = b;
}

static inline GLOffscreenBGRAStats GLOffscreenAnalyzeBGRA8Pixels(
	const uint8_t *bgra,
	uint32_t width,
	uint32_t height,
	uint32_t rowbytes)
{
	GLOffscreenBGRAStats stats =
		{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, false, false};
	if (bgra == 0) return stats;

	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *row = bgra + (uint64_t)y * (uint64_t)rowbytes;
		for (uint32_t x = 0; x < width; x++) {
			const uint8_t *pixel = row + (uint64_t)x * 4u;
			const uint8_t b = pixel[0];
			const uint8_t g = pixel[1];
			const uint8_t r = pixel[2];
			const uint8_t a = pixel[3];
			stats.total_pixels++;
			if (r != 0 || g != 0 || b != 0) {
				stats.rgb_nonzero_pixels++;
				if (!stats.rgb_bounds_valid) {
					stats.rgb_min_x = stats.rgb_max_x = x;
					stats.rgb_min_y = stats.rgb_max_y = y;
					stats.rgb_bounds_valid = true;
				} else {
					if (x < stats.rgb_min_x) stats.rgb_min_x = x;
					if (y < stats.rgb_min_y) stats.rgb_min_y = y;
					if (x > stats.rgb_max_x) stats.rgb_max_x = x;
					if (y > stats.rgb_max_y) stats.rgb_max_y = y;
				}
			}
			if (a != 0) stats.alpha_nonzero_pixels++;
			if (a >= 128) {
				stats.alpha_opaque_pixels++;
				if (!stats.alpha_bounds_valid) {
					stats.alpha_min_x = stats.alpha_max_x = x;
					stats.alpha_min_y = stats.alpha_max_y = y;
					stats.alpha_bounds_valid = true;
				} else {
					if (x < stats.alpha_min_x) stats.alpha_min_x = x;
					if (y < stats.alpha_min_y) stats.alpha_min_y = y;
					if (x > stats.alpha_max_x) stats.alpha_max_x = x;
					if (y > stats.alpha_max_y) stats.alpha_max_y = y;
				}
			}
		}
	}
	return stats;
}

#endif /* GL_OFFSCREEN_POLICY_H */
