/*
 *  dsp_front_staging_seed_policy.h - mixed-depth front-buffer seed helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef DSP_FRONT_STAGING_SEED_POLICY_H
#define DSP_FRONT_STAGING_SEED_POLICY_H

#include <stdint.h>

typedef struct DSpFrontStagingSeedRGB {
	uint8_t r;
	uint8_t g;
	uint8_t b;
} DSpFrontStagingSeedRGB;

static inline DSpFrontStagingSeedRGB
DSpFrontStagingSeedRGBForBackPixelZero(uint32_t back_depth,
                                       const uint8_t clut_bytes[768])
{
	DSpFrontStagingSeedRGB seed = {0, 0, 0};
	if ((back_depth == 1 || back_depth == 2 ||
	     back_depth == 4 || back_depth == 8) &&
	    clut_bytes != 0) {
		seed.r = clut_bytes[0];
		seed.g = clut_bytes[1];
		seed.b = clut_bytes[2];
	}
	return seed;
}

static inline uint16_t DSpPackRGB555BigEndian(uint8_t r,
                                              uint8_t g,
                                              uint8_t b)
{
	return (uint16_t)((((uint16_t)r >> 3) << 10) |
	                  (((uint16_t)g >> 3) <<  5) |
	                   ((uint16_t)b >> 3));
}

static inline void DSpStoreRGB555FrontStagingBytes(uint16_t pixel,
                                                   uint8_t out_bytes[2])
{
	/*
	 * Front staging is guest-visible Mac framebuffer memory. Keep 16-bit
	 * pixels in Mac big-endian xRGB1555 byte order, then let the same Metal
	 * unpack path used by the compositor byte-swap before extracting RGB.
	 */
	out_bytes[0] = (uint8_t)(pixel >> 8);
	out_bytes[1] = (uint8_t)(pixel & 0xffu);
}

#endif /* DSP_FRONT_STAGING_SEED_POLICY_H */
