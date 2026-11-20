/*
 *  dsp_default_clut.h - default DrawSprocket indexed CLUT initialization.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef DSP_DEFAULT_CLUT_H
#define DSP_DEFAULT_CLUT_H

#include <stdint.h>
#include <string.h>

static inline void DSpInitDefaultCLUT(uint8_t clut_bytes[768],
                                      uint8_t clut_bytes_latched[768],
                                      uint32_t depth_bits)
{
	if (clut_bytes == 0) return;

	memset(clut_bytes, 0, 768);

	if (depth_bits == 1) {
		clut_bytes[0] = 255;
		clut_bytes[1] = 255;
		clut_bytes[2] = 255;
		clut_bytes[3] = 0;
		clut_bytes[4] = 0;
		clut_bytes[5] = 0;
	} else if (depth_bits == 2 || depth_bits == 4 || depth_bits == 8) {
		const uint32_t count = 1u << depth_bits;
		const uint32_t max_index = count - 1u;
		for (uint32_t i = 0; i < count; i++) {
			uint8_t v = (uint8_t)((i * 255u) / max_index);
			clut_bytes[i * 3u + 0u] = v;
			clut_bytes[i * 3u + 1u] = v;
			clut_bytes[i * 3u + 2u] = v;
		}
	}

	if (clut_bytes_latched != 0) {
		memcpy(clut_bytes_latched, clut_bytes, 768);
	}
}

#endif /* DSP_DEFAULT_CLUT_H */
