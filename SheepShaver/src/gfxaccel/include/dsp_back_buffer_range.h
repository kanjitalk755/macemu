/*
 *  dsp_back_buffer_range.h - DrawSprocket back-buffer byte span helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef DSP_BACK_BUFFER_RANGE_H
#define DSP_BACK_BUFFER_RANGE_H

#include <stdint.h>

static inline uint32_t DSpBackBufferAlignedRowBytes(uint32_t width,
                                                    uint32_t depth_bits)
{
	uint32_t row_bytes = (width * depth_bits + 7u) / 8u;
	return (row_bytes + 255u) & ~255u;
}

#endif /* DSP_BACK_BUFFER_RANGE_H */
