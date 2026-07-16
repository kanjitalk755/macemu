/*
 *  dsp_main_device_redirect_policy.h - DrawSprocket MainDevice PixMap redirect helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef DSP_MAIN_DEVICE_REDIRECT_POLICY_H
#define DSP_MAIN_DEVICE_REDIRECT_POLICY_H

#include <stdint.h>

#include "dsp_display_mode_policy.h"

static inline uint32_t DSpMainDevicePixMapDepth(uint32_t back_buffer_depth,
                                                uint32_t display_depth)
{
	return DSpDisplayModeDepth(back_buffer_depth, display_depth);
}

static inline uint32_t DSpMainDevicePixMapRowBytes(uint32_t width,
	uint32_t back_buffer_depth,
	uint32_t display_depth)
{
	return DSpDisplayModePitch(
		width,
		DSpMainDevicePixMapDepth(back_buffer_depth, display_depth));
}

static inline uint16_t DSpMainDevicePixMapRowBytesField(uint32_t row_bytes)
{
	return (uint16_t)(0x8000u | (row_bytes & 0x3FFFu));
}

static inline uint32_t DSpMainDevicePixMapBoundDimension(uint32_t display_dimension,
                                                         uint32_t back_buffer_dimension)
{
	if (back_buffer_dimension == 0 ||
	    back_buffer_dimension > display_dimension) {
		return display_dimension;
	}
	return back_buffer_dimension;
}

static inline uint16_t DSpMainDevicePixMapVersion(void)
{
	return 0u;
}

static inline uint16_t DSpMainDevicePixMapPackType(void)
{
	return 0u;
}

static inline uint32_t DSpMainDevicePixMapPackSize(void)
{
	return 0u;
}

static inline uint32_t DSpMainDevicePixMapResolution(void)
{
	return 0x00480000u;
}

static inline uint32_t DSpMainDevicePixMapPlaneBytes(void)
{
	return 0u;
}

static inline bool DSpShouldRedirectMainDevicePixMap(uint32_t back_buffer_depth,
                                                     uint32_t display_depth)
{
	return back_buffer_depth != 0 &&
	       display_depth != 0 &&
	       DSpMainDevicePixMapDepth(back_buffer_depth, display_depth) != 0;
}

static inline bool DSpMainDeviceRedirectShouldExposeFrontStaging(
	uint32_t back_buffer_depth,
	uint32_t display_depth)
{
	return DSpShouldRedirectMainDevicePixMap(back_buffer_depth,
	                                         display_depth) &&
	       DSpMainDevicePixMapDepth(back_buffer_depth,
	                                display_depth) != back_buffer_depth;
}

static inline bool DSpShouldCacheMainDevicePixMapOriginal(bool saved_valid,
                                                          uint32_t saved_pixmap_addr,
                                                          uint32_t current_pixmap_addr)
{
	(void)saved_pixmap_addr;
	(void)current_pixmap_addr;
	return !saved_valid;
}

#endif /* DSP_MAIN_DEVICE_REDIRECT_POLICY_H */
