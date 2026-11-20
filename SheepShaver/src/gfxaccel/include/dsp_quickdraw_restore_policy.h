/*
 *  dsp_quickdraw_restore_policy.h - DrawSprocket QuickDraw mode restore helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef DSP_QUICKDRAW_RESTORE_POLICY_H
#define DSP_QUICKDRAW_RESTORE_POLICY_H

#include <stdint.h>
#include <stdbool.h>

static inline uint32_t DSpPixMapRowBytesPayload(uint16_t row_bytes_field)
{
	return (uint32_t)(row_bytes_field & 0x3FFFu);
}

static inline bool DSpPixelSizeIsValidForQuickDrawRestore(uint16_t pixel_size)
{
	return pixel_size == 1u || pixel_size == 2u || pixel_size == 4u ||
	       pixel_size == 8u || pixel_size == 16u || pixel_size == 32u;
}

static inline bool DSpSavedQuickDrawModeIsUsable(uint8_t saved_valid,
                                                 uint32_t base_addr,
                                                 uint16_t row_bytes_field,
                                                 int16_t top,
                                                 int16_t left,
                                                 int16_t bottom,
                                                 int16_t right,
                                                 uint16_t pixel_size,
                                                 uint32_t ram_base,
                                                 uint32_t ram_size)
{
	(void)ram_base;
	(void)ram_size;

	if (saved_valid == 0 || base_addr == 0) {
		return false;
	}
	if (!DSpPixelSizeIsValidForQuickDrawRestore(pixel_size)) {
		return false;
	}
	if (right <= left || bottom <= top) {
		return false;
	}

	const uint32_t width = (uint32_t)(right - left);
	const uint32_t height = (uint32_t)(bottom - top);
	const uint32_t row_bytes = DSpPixMapRowBytesPayload(row_bytes_field);
	const uint32_t min_row_bytes = (width * (uint32_t)pixel_size + 7u) / 8u;
	if (row_bytes == 0 || row_bytes < min_row_bytes || height == 0) {
		return false;
	}

	const uint64_t end =
	    (uint64_t)base_addr + (uint64_t)row_bytes * (uint64_t)height;
	return end >= (uint64_t)base_addr && end <= 0x100000000ULL;
}

static inline bool DSpQuickDrawModeRestoreDiffers(uint32_t saved_width,
                                                  uint32_t saved_height,
                                                  uint32_t saved_depth,
                                                  uint32_t snap_width,
                                                  uint32_t snap_height,
                                                  uint32_t snap_depth)
{
	return saved_width != snap_width ||
	       saved_height != snap_height ||
	       saved_depth != snap_depth;
}

#endif /* DSP_QUICKDRAW_RESTORE_POLICY_H */
