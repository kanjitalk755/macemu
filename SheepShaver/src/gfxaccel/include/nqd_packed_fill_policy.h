/*
 *  nqd_packed_fill_policy.h - CPU packed-pixel FillRect mode helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef NQD_PACKED_FILL_POLICY_H
#define NQD_PACKED_FILL_POLICY_H

#include <stdint.h>

static inline uint32_t NQDPackedFillRasterOp(uint32_t transfer_mode,
                                             uint32_t pen_mode)
{
	if (pen_mode >= 8u && pen_mode <= 15u) {
		return pen_mode;
	}
	return transfer_mode;
}

static inline uint32_t NQDPackedFillNativeColor(uint32_t fore_pen_native,
                                                uint32_t back_pen_native,
                                                uint32_t pen_mode)
{
	(void)back_pen_native;
	if (pen_mode >= 8u && pen_mode <= 15u) {
		return fore_pen_native;
	}
	return fore_pen_native;
}

static inline uint32_t NQDPacked1BppIndexFromNativeColor(uint32_t native_color)
{
	const uint32_t rgb = native_color & 0x00ffffffu;
	const uint32_t r = (rgb >> 16) & 0xffu;
	const uint32_t g = (rgb >> 8) & 0xffu;
	const uint32_t b = rgb & 0xffu;
	const uint32_t luma = r * 30u + g * 59u + b * 11u;
	return luma < (128u * 100u) ? 1u : 0u;
}

static inline uint32_t NQDPackedGet1BppPixel(uint8_t byte_value, uint32_t x)
{
	const uint32_t shift = 7u - (x & 7u);
	return (byte_value >> shift) & 1u;
}

static inline uint8_t NQDPackedSet1BppPixel(uint8_t byte_value,
                                            uint32_t x,
                                            uint32_t pixel)
{
	const uint32_t shift = 7u - (x & 7u);
	const uint8_t mask = (uint8_t)(1u << shift);
	if ((pixel & 1u) != 0) {
		return (uint8_t)(byte_value | mask);
	}
	return (uint8_t)(byte_value & (uint8_t)~mask);
}

#endif /* NQD_PACKED_FILL_POLICY_H */
