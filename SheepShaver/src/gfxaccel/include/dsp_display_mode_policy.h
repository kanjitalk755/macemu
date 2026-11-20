/*
 *  dsp_display_mode_policy.h - DrawSprocket display mode selection helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef DSP_DISPLAY_MODE_POLICY_H
#define DSP_DISPLAY_MODE_POLICY_H

#include <stdint.h>

static inline uint32_t DSpDisplayModeDepth(uint32_t back_buffer_depth,
                                           uint32_t display_depth)
{
	switch (display_depth) {
	case 1:
	case 2:
	case 4:
	case 8:
	case 16:
	case 32:
		return display_depth;
	default:
		break;
	}
	return back_buffer_depth;
}

static inline uint32_t DSpDisplayModeRowBytes(uint32_t width,
                                              uint32_t display_depth)
{
	return (width * display_depth + 7u) / 8u;
}

static inline uint32_t DSpDisplayModePitch(uint32_t width,
                                           uint32_t display_depth)
{
	const uint32_t row_bytes = DSpDisplayModeRowBytes(width, display_depth);
	return (row_bytes + 255u) & ~255u;
}

static inline uint32_t DSpDisplayModeTextureWidth(uint32_t width,
                                                  uint32_t display_depth)
{
	switch (display_depth) {
	case 1:
	case 2:
	case 4:
		return DSpDisplayModeRowBytes(width, display_depth);
	default:
		return width;
	}
}

static inline uint32_t DSpReserveActualDisplayDimension(uint32_t selected_dimension,
                                                        uint32_t desired_dimension)
{
	return selected_dimension != 0 ? selected_dimension : desired_dimension;
}

static inline uint32_t DSpReserveBackBufferDimension(uint32_t selected_dimension,
                                                     uint32_t desired_dimension)
{
	(void)selected_dimension;
	return desired_dimension;
}

static inline uint32_t DSpReserveActualDisplayDepth(uint32_t selected_display_depth,
                                                    uint32_t desired_display_depth,
                                                    uint32_t desired_back_buffer_depth)
{
	if (selected_display_depth == 1 || selected_display_depth == 2 ||
	    selected_display_depth == 4 || selected_display_depth == 8 ||
	    selected_display_depth == 16 || selected_display_depth == 32) {
		return selected_display_depth;
	}
	return DSpDisplayModeDepth(desired_back_buffer_depth,
	                           desired_display_depth);
}

#endif /* DSP_DISPLAY_MODE_POLICY_H */
