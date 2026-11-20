/*
 *  gl_pixel_unpack_policy.h - OpenGL pixel-unpack format helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef GL_PIXEL_UNPACK_POLICY_H
#define GL_PIXEL_UNPACK_POLICY_H

#include <stdint.h>

enum {
	GLPixelFormatColorIndex = 0x1900u,
	GLPixelFormatDepthComponent = 0x1902u,
	GLPixelFormatRed = 0x1903u,
	GLPixelFormatGreen = 0x1904u,
	GLPixelFormatBlue = 0x1905u,
	GLPixelFormatAlpha = 0x1906u,
	GLPixelFormatRGB = 0x1907u,
	GLPixelFormatRGBA = 0x1908u,
	GLPixelFormatLuminance = 0x1909u,
	GLPixelFormatLuminanceAlpha = 0x190au,
	GLPixelFormatABGR = 0x8000u,
	GLPixelFormatBGR = 0x80e0u,
	GLPixelFormatBGRA = 0x80e1u,
	GLPixelFormatIntensity = 0x8049u,

	GLPixelTypeByte = 0x1400u,
	GLPixelTypeUnsignedByte = 0x1401u,
	GLPixelTypeShort = 0x1402u,
	GLPixelTypeUnsignedShort = 0x1403u,
	GLPixelTypeInt = 0x1404u,
	GLPixelTypeUnsignedInt = 0x1405u,
	GLPixelTypeFloat = 0x1406u,
	GLPixelTypeUnsignedByte332 = 0x8032u,
	GLPixelTypeUnsignedShort4444 = 0x8033u,
	GLPixelTypeUnsignedShort5551 = 0x8034u,
	GLPixelTypeUnsignedInt8888 = 0x8035u,
	GLPixelTypeUnsignedInt1010102 = 0x8036u,
	GLPixelTypeUnsignedByte233Rev = 0x8362u,
	GLPixelTypeUnsignedShort565 = 0x8363u,
	GLPixelTypeUnsignedShort565Rev = 0x8364u,
	GLPixelTypeUnsignedShort4444Rev = 0x8365u,
	GLPixelTypeUnsignedShort1555Rev = 0x8366u,
	GLPixelTypeUnsignedInt8888Rev = 0x8367u,
	GLPixelTypeUnsignedInt2101010Rev = 0x8368u,
};

enum {
	GLPixelComponentNone = -1,
	GLPixelComponentRed = 0,
	GLPixelComponentGreen = 1,
	GLPixelComponentBlue = 2,
	GLPixelComponentAlpha = 3,
	GLPixelComponentLuminance = 4,
	GLPixelComponentIntensity = 5,
	GLPixelComponentIndex = 6,
};

static inline uint8_t GLPixelScaleUnsignedTo8(uint32_t value, int bits)
{
	if (bits <= 0) return 0;
	if (bits == 8) return (uint8_t)value;

	const uint64_t maxValue =
		(bits >= 32) ? 0xffffffffull : ((1ull << bits) - 1ull);
	if ((uint64_t)value >= maxValue) return 0xffu;
	return (uint8_t)(((uint64_t)value * 255ull + (maxValue / 2ull)) /
	                 maxValue);
}

static inline uint8_t GLPixelScaleSignedTo8(int32_t value, int bits)
{
	if (bits <= 1 || value <= 0) return 0;

	const int64_t maxValue =
		(bits >= 32) ? 0x7fffffffull : ((1ll << (bits - 1)) - 1ll);
	if ((int64_t)value >= maxValue) return 0xffu;
	return (uint8_t)(((int64_t)value * 255ll + (maxValue / 2ll)) /
	                 maxValue);
}

static inline int GLPixelFormatComponentCount(uint32_t format)
{
	switch (format) {
		case GLPixelFormatColorIndex:
		case GLPixelFormatRed:
		case GLPixelFormatGreen:
		case GLPixelFormatBlue:
		case GLPixelFormatAlpha:
		case GLPixelFormatLuminance:
		case GLPixelFormatIntensity:
			return 1;
		case GLPixelFormatLuminanceAlpha:
			return 2;
		case GLPixelFormatRGB:
		case GLPixelFormatBGR:
			return 3;
		case GLPixelFormatRGBA:
		case GLPixelFormatBGRA:
		case GLPixelFormatABGR:
			return 4;
		default:
			return 0;
	}
}

static inline int GLPixelFormatComponentAt(uint32_t format, int index)
{
	switch (format) {
		case GLPixelFormatColorIndex:
			return index == 0 ? GLPixelComponentIndex : GLPixelComponentNone;
		case GLPixelFormatRed:
			return index == 0 ? GLPixelComponentRed : GLPixelComponentNone;
		case GLPixelFormatGreen:
			return index == 0 ? GLPixelComponentGreen : GLPixelComponentNone;
		case GLPixelFormatBlue:
			return index == 0 ? GLPixelComponentBlue : GLPixelComponentNone;
		case GLPixelFormatAlpha:
			return index == 0 ? GLPixelComponentAlpha : GLPixelComponentNone;
		case GLPixelFormatLuminance:
			return index == 0 ? GLPixelComponentLuminance : GLPixelComponentNone;
		case GLPixelFormatIntensity:
			return index == 0 ? GLPixelComponentIntensity : GLPixelComponentNone;
		case GLPixelFormatLuminanceAlpha:
			if (index == 0) return GLPixelComponentLuminance;
			if (index == 1) return GLPixelComponentAlpha;
			return GLPixelComponentNone;
		case GLPixelFormatRGB:
			if (index == 0) return GLPixelComponentRed;
			if (index == 1) return GLPixelComponentGreen;
			if (index == 2) return GLPixelComponentBlue;
			return GLPixelComponentNone;
		case GLPixelFormatBGR:
			if (index == 0) return GLPixelComponentBlue;
			if (index == 1) return GLPixelComponentGreen;
			if (index == 2) return GLPixelComponentRed;
			return GLPixelComponentNone;
		case GLPixelFormatRGBA:
			if (index == 0) return GLPixelComponentRed;
			if (index == 1) return GLPixelComponentGreen;
			if (index == 2) return GLPixelComponentBlue;
			if (index == 3) return GLPixelComponentAlpha;
			return GLPixelComponentNone;
		case GLPixelFormatBGRA:
			if (index == 0) return GLPixelComponentBlue;
			if (index == 1) return GLPixelComponentGreen;
			if (index == 2) return GLPixelComponentRed;
			if (index == 3) return GLPixelComponentAlpha;
			return GLPixelComponentNone;
		case GLPixelFormatABGR:
			if (index == 0) return GLPixelComponentAlpha;
			if (index == 1) return GLPixelComponentBlue;
			if (index == 2) return GLPixelComponentGreen;
			if (index == 3) return GLPixelComponentRed;
			return GLPixelComponentNone;
		default:
			return GLPixelComponentNone;
	}
}

static inline int GLPixelScalarTypeBytes(uint32_t type)
{
	switch (type) {
		case GLPixelTypeByte:
		case GLPixelTypeUnsignedByte:
			return 1;
		case GLPixelTypeShort:
		case GLPixelTypeUnsignedShort:
			return 2;
		case GLPixelTypeInt:
		case GLPixelTypeUnsignedInt:
		case GLPixelTypeFloat:
			return 4;
		default:
			return 0;
	}
}

static inline bool GLPixelFormatIsThreeComponentPacked(uint32_t format)
{
	return format == GLPixelFormatRGB || format == GLPixelFormatBGR;
}

static inline bool GLPixelFormatIsFourComponentPacked(uint32_t format)
{
	return format == GLPixelFormatRGBA ||
	       format == GLPixelFormatBGRA ||
	       format == GLPixelFormatABGR;
}

static inline bool GLPixelLegacyUnsignedShortFormatCanUsePackedDecode(
	uint32_t format)
{
	return format == GLPixelFormatRGB ||
	       format == GLPixelFormatRGBA ||
	       format == GLPixelFormatBGR;
}

static inline int GLPixelSourceBytesPerPixel(uint32_t format, uint32_t type)
{
	switch (type) {
		case GLPixelTypeUnsignedByte332:
		case GLPixelTypeUnsignedByte233Rev:
			return GLPixelFormatIsThreeComponentPacked(format) ? 1 : 0;
		case GLPixelTypeUnsignedShort565:
		case GLPixelTypeUnsignedShort565Rev:
			return GLPixelFormatIsThreeComponentPacked(format) ? 2 : 0;
		case GLPixelTypeUnsignedShort4444:
		case GLPixelTypeUnsignedShort4444Rev:
		case GLPixelTypeUnsignedShort5551:
		case GLPixelTypeUnsignedShort1555Rev:
			return GLPixelFormatIsFourComponentPacked(format) ? 2 : 0;
		case GLPixelTypeUnsignedInt8888:
		case GLPixelTypeUnsignedInt8888Rev:
		case GLPixelTypeUnsignedInt1010102:
		case GLPixelTypeUnsignedInt2101010Rev:
			return GLPixelFormatIsFourComponentPacked(format) ? 4 : 0;
		default:
			break;
	}

	const int componentCount = GLPixelFormatComponentCount(format);
	const int scalarBytes = GLPixelScalarTypeBytes(type);
	if (componentCount <= 0 || scalarBytes <= 0) return 0;
	return componentCount * scalarBytes;
}

static inline int GLPixelLegacyUnsignedShortPackedBytesPerPixel(
	uint32_t format,
	uint32_t type)
{
	if (type != GLPixelTypeUnsignedShort)
		return 0;
	return GLPixelLegacyUnsignedShortFormatCanUsePackedDecode(format) ? 2 : 0;
}

static inline bool GLPixelLegacyUnsignedShortShouldUseLegacyPackedLayout(
	bool force_legacy_chain,
	bool exact_duplicated_byte_words,
	bool has_usable_palette)
{
	if (force_legacy_chain)
		return true;
	if (exact_duplicated_byte_words)
		return has_usable_palette;
	return true;
}

static inline bool GLPixelLegacyUnsignedShortShouldUsePalette(
	int sampled_words,
	int duplicated_byte_words,
	int non_extreme_duplicated_byte_words)
{
	if (sampled_words < 8)
		return false;
	if (non_extreme_duplicated_byte_words < 2)
		return false;
	return duplicated_byte_words * 4 > sampled_words * 3;
}

static inline bool GLPixelLegacyUnsignedShortShouldUseMacVideoPalette(
	int sampled_words,
	int duplicated_byte_words,
	int non_extreme_duplicated_byte_words,
	bool has_explicit_palette)
{
	return !has_explicit_palette &&
	       GLPixelLegacyUnsignedShortShouldUsePalette(
		       sampled_words,
		       duplicated_byte_words,
		       non_extreme_duplicated_byte_words);
}

static inline bool GLPixelLegacyUnsignedShortFormatCanUseBGR332Fallback(
	uint32_t format)
{
	return format == GLPixelFormatRGB ||
	       format == GLPixelFormatRGBA ||
	       format == GLPixelFormatBGR;
}

static inline bool GLPixelLegacyUnsignedShortHasLevelZeroOnlyFallback(
	bool legacy_bgr332_chain,
	bool legacy_scalar_no_palette_chain,
	uint32_t format,
	uint32_t type)
{
	if (type != GLPixelTypeUnsignedShort)
		return false;
	if (legacy_scalar_no_palette_chain &&
	    GLPixelLegacyUnsignedShortFormatCanUsePackedDecode(format)) {
		return true;
	}
	return legacy_bgr332_chain &&
	       GLPixelLegacyUnsignedShortFormatCanUseBGR332Fallback(format);
}

static inline bool GLPixelLegacyUnsignedShortShouldIgnoreClientMipForFallback(
	bool legacy_bgr332_chain,
	bool legacy_scalar_no_palette_chain,
	uint32_t format,
	uint32_t type,
	int level)
{
	return level > 0 &&
	       GLPixelLegacyUnsignedShortHasLevelZeroOnlyFallback(
		       legacy_bgr332_chain,
		       legacy_scalar_no_palette_chain,
		       format,
		       type);
}

typedef struct GLPixelLegacyUnsignedShortIndexStats {
	int sampled_words;
	int duplicated_byte_words;
	int non_extreme_duplicated_byte_words;
	int unique_low_byte_values;
	uint8_t min_low_byte;
	uint8_t max_low_byte;
	int low_byte_counts[256];
} GLPixelLegacyUnsignedShortIndexStats;

static inline void GLPixelLegacyUnsignedShortIndexStatsInit(
	GLPixelLegacyUnsignedShortIndexStats *stats)
{
	if (stats == 0) return;
	stats->sampled_words = 0;
	stats->duplicated_byte_words = 0;
	stats->non_extreme_duplicated_byte_words = 0;
	stats->unique_low_byte_values = 0;
	stats->min_low_byte = 0;
	stats->max_low_byte = 0;
	for (int i = 0; i < 256; i++)
		stats->low_byte_counts[i] = 0;
}

static inline void GLPixelLegacyUnsignedShortIndexStatsAddWord(
	GLPixelLegacyUnsignedShortIndexStats *stats,
	uint16_t packed)
{
	if (stats == 0) return;

	const uint8_t hi = (uint8_t)(packed >> 8);
	const uint8_t lo = (uint8_t)(packed & 0xffu);
	if (stats->sampled_words == 0) {
		stats->min_low_byte = lo;
		stats->max_low_byte = lo;
	} else {
		if (lo < stats->min_low_byte) stats->min_low_byte = lo;
		if (lo > stats->max_low_byte) stats->max_low_byte = lo;
	}
	stats->sampled_words++;

	if (stats->low_byte_counts[lo] == 0)
		stats->unique_low_byte_values++;
	stats->low_byte_counts[lo]++;

	if (hi == lo) {
		stats->duplicated_byte_words++;
		if (lo != 0x00u && lo != 0xffu)
			stats->non_extreme_duplicated_byte_words++;
	}
}

static inline bool GLPixelLegacyUnsignedShortIndexStatsShouldUsePalette(
	const GLPixelLegacyUnsignedShortIndexStats *stats)
{
	return stats != 0 &&
	       GLPixelLegacyUnsignedShortShouldUsePalette(
		       stats->sampled_words,
		       stats->duplicated_byte_words,
		       stats->non_extreme_duplicated_byte_words);
}

typedef struct GLPixelPaletteSummary {
	int entries;
	int non_black_entries;
	int different_from_first_entries;
	int non_gray_entries;
} GLPixelPaletteSummary;

static inline void GLPixelPaletteSummaryAnalyze(const uint8_t *palette_rgb,
                                                int entries,
                                                GLPixelPaletteSummary *summary)
{
	if (summary == 0) return;
	summary->entries = entries > 0 ? entries : 0;
	summary->non_black_entries = 0;
	summary->different_from_first_entries = 0;
	summary->non_gray_entries = 0;
	if (palette_rgb == 0 || entries <= 0) return;

	const uint8_t first_r = palette_rgb[0];
	const uint8_t first_g = palette_rgb[1];
	const uint8_t first_b = palette_rgb[2];

	for (int i = 0; i < entries; i++) {
		const uint8_t r = palette_rgb[i * 3 + 0];
		const uint8_t g = palette_rgb[i * 3 + 1];
		const uint8_t b = palette_rgb[i * 3 + 2];
		if (r != 0 || g != 0 || b != 0)
			summary->non_black_entries++;
		if (r != first_r || g != first_g || b != first_b)
			summary->different_from_first_entries++;
		if (r != g || g != b)
			summary->non_gray_entries++;
	}
}

static inline bool GLPixelPaletteHasUsableColor(const uint8_t *palette_rgb,
                                                int entries)
{
	GLPixelPaletteSummary summary;
	GLPixelPaletteSummaryAnalyze(palette_rgb, entries, &summary);
	return summary.non_black_entries > 0 &&
	       summary.different_from_first_entries > 0;
}

static inline uint8_t GLPixelLegacyUnsignedShortPaletteIndex(uint16_t packed)
{
	return (uint8_t)(packed & 0xffu);
}

static inline void GLPixelInitBGRA(uint8_t out_bgra[4])
{
	out_bgra[0] = 0;
	out_bgra[1] = 0;
	out_bgra[2] = 0;
	out_bgra[3] = 0xffu;
}

static inline void GLPixelStoreComponentToBGRA(int component,
                                               uint8_t value,
                                               uint8_t out_bgra[4])
{
	switch (component) {
		case GLPixelComponentRed:
			out_bgra[2] = value;
			break;
		case GLPixelComponentGreen:
			out_bgra[1] = value;
			break;
		case GLPixelComponentBlue:
			out_bgra[0] = value;
			break;
		case GLPixelComponentAlpha:
			out_bgra[3] = value;
			break;
		case GLPixelComponentLuminance:
			out_bgra[0] = value;
			out_bgra[1] = value;
			out_bgra[2] = value;
			break;
		case GLPixelComponentIntensity:
			out_bgra[0] = value;
			out_bgra[1] = value;
			out_bgra[2] = value;
			out_bgra[3] = value;
			break;
		default:
			break;
	}
}

static inline void GLPixelUnpackPackedToBGRA(uint32_t packed,
                                             const uint8_t *componentBits,
                                             int componentCount,
                                             bool reversed,
                                             uint32_t format,
                                             uint8_t out_bgra[4])
{
	GLPixelInitBGRA(out_bgra);

	int shift = 0;
	if (!reversed) {
		for (int i = 0; i < componentCount; i++)
			shift += componentBits[i];
	}

	for (int i = 0; i < componentCount; i++) {
		const int bits = componentBits[i];
		if (!reversed) shift -= bits;
		const uint32_t mask = (bits >= 32) ? 0xffffffffu : ((1u << bits) - 1u);
		const uint32_t raw = (packed >> shift) & mask;
		GLPixelStoreComponentToBGRA(
			GLPixelFormatComponentAt(format, i),
			GLPixelScaleUnsignedTo8(raw, bits),
			out_bgra);
		if (reversed) shift += bits;
	}
}

static inline void GLPixelUnpackRGB332ToBGRA(uint8_t packed,
                                             bool reversed,
                                             uint32_t format,
                                             uint8_t out_bgra[4])
{
	const uint8_t normalBits[3] = {3, 3, 2};
	const uint8_t reversedBits[3] = {2, 3, 3};
	GLPixelUnpackPackedToBGRA(packed,
	                          reversed ? reversedBits : normalBits,
	                          3, reversed, format, out_bgra);
}

static inline void GLPixelUnpackLegacyUnsignedShortRGB332ToBGRA(
	uint16_t packed,
	uint8_t out_bgra[4])
{
	GLPixelUnpackRGB332ToBGRA((uint8_t)(packed & 0xffu), false,
	                          GLPixelFormatRGB, out_bgra);
}

static inline void GLPixelUnpackLegacyUnsignedShortBGR332ToBGRA(
	uint16_t packed,
	uint8_t out_bgra[4])
{
	GLPixelUnpackRGB332ToBGRA((uint8_t)(packed & 0xffu), false,
	                          GLPixelFormatBGR, out_bgra);
}

static inline void GLPixelUnpackLegacyUnsignedShortIndexGrayToBGRA(
	uint16_t packed,
	uint8_t out_bgra[4])
{
	const uint8_t index = (uint8_t)(packed & 0xffu);
	out_bgra[0] = index;
	out_bgra[1] = index;
	out_bgra[2] = index;
	out_bgra[3] = 0xffu;
}

static inline void GLPixelUnpackRGB565ToBGRA(uint16_t packed,
                                             bool reversed,
                                             uint32_t format,
                                             uint8_t out_bgra[4])
{
	const uint8_t bits[3] = {5, 6, 5};
	GLPixelUnpackPackedToBGRA(packed, bits, 3, reversed, format, out_bgra);
}

static inline void GLPixelUnpackMacRGB555ToBGRA(uint16_t packed,
                                                uint32_t format,
                                                uint8_t out_bgra[4])
{
	const uint8_t bits[3] = {5, 5, 5};
	GLPixelUnpackPackedToBGRA(packed, bits, 3, false, format, out_bgra);
}

static inline void GLPixelUnpackRGBA4444ToBGRA(uint16_t packed,
                                               bool reversed,
                                               uint32_t format,
                                               uint8_t out_bgra[4])
{
	const uint8_t bits[4] = {4, 4, 4, 4};
	GLPixelUnpackPackedToBGRA(packed, bits, 4, reversed, format, out_bgra);
}

static inline void GLPixelUnpackRGBA5551ToBGRA(uint16_t packed,
                                               bool reversed,
                                               uint32_t format,
                                               uint8_t out_bgra[4])
{
	const uint8_t normalBits[4] = {5, 5, 5, 1};
	const uint8_t reversedBits[4] = {1, 5, 5, 5};
	GLPixelUnpackPackedToBGRA(packed,
	                          reversed ? reversedBits : normalBits,
	                          4, reversed, format, out_bgra);
}

static inline void GLPixelUnpackLegacyUnsignedShortToBGRA(uint16_t packed,
                                                          uint32_t format,
                                                          uint8_t out_bgra[4])
{
	if (format == GLPixelFormatRGBA) {
		GLPixelUnpackRGBA5551ToBGRA(packed, false, format, out_bgra);
		out_bgra[3] = 0xffu;
		return;
	}

	GLPixelUnpackMacRGB555ToBGRA(packed, format, out_bgra);
}

static inline void GLPixelUnpackRGBA8888ToBGRA(uint32_t packed,
                                               bool reversed,
                                               uint32_t format,
                                               uint8_t out_bgra[4])
{
	const uint8_t bits[4] = {8, 8, 8, 8};
	GLPixelUnpackPackedToBGRA(packed, bits, 4, reversed, format, out_bgra);
}

static inline void GLPixelUnpackRGBA1010102ToBGRA(uint32_t packed,
                                                  bool reversed,
                                                  uint32_t format,
                                                  uint8_t out_bgra[4])
{
	const uint8_t normalBits[4] = {10, 10, 10, 2};
	const uint8_t reversedBits[4] = {2, 10, 10, 10};
	GLPixelUnpackPackedToBGRA(packed,
	                          reversed ? reversedBits : normalBits,
	                          4, reversed, format, out_bgra);
}

#endif /* GL_PIXEL_UNPACK_POLICY_H */
