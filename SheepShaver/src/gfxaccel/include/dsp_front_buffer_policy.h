/*
 *  dsp_front_buffer_policy.h - DrawSprocket front-buffer metadata helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef DSP_FRONT_BUFFER_POLICY_H
#define DSP_FRONT_BUFFER_POLICY_H

#include <stdint.h>

#include "dsp_display_mode_policy.h"
#include "dsp_pixmap_offsets.h"

static inline uint32_t DSpFrontBufferDepth(uint32_t back_buffer_depth,
                                           uint32_t display_depth)
{
	return DSpDisplayModeDepth(back_buffer_depth, display_depth);
}

static inline uint32_t DSpFrontBufferRowBytes(uint32_t width,
                                              uint32_t back_buffer_depth,
	uint32_t display_depth)
{
	return DSpDisplayModePitch(
		width,
		DSpFrontBufferDepth(back_buffer_depth, display_depth));
}

static inline uint32_t DSpFrontBufferPixMapRecordSize(void)
{
	return (uint32_t)DSP_MAINDEVICE_PIXMAP_SIZE;
}

static inline uint16_t DSpFrontBufferPixMapRowBytesField(uint32_t row_bytes)
{
	return (uint16_t)(0x8000u | (row_bytes & 0x3FFFu));
}

#endif /* DSP_FRONT_BUFFER_POLICY_H */
