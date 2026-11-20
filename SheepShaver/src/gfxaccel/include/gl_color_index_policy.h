/*
 *  gl_color_index_policy.h - OpenGL color-index upload conversion helpers
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef GL_COLOR_INDEX_POLICY_H
#define GL_COLOR_INDEX_POLICY_H

#include <stdint.h>

static inline uint8_t GLColorIndexFloatToByte(float value)
{
	if (value <= 0.0f)
		return 0u;
	if (value >= 1.0f)
		return 0xffu;
	return (uint8_t)(value * 255.0f + 0.5f);
}

static inline float GLColorIndexLookupPixelMap(uint8_t index,
                                               const float *map,
                                               int map_size,
                                               float fallback)
{
	if (!map || map_size <= 0)
		return fallback;
	int map_index = index;
	if (map_index >= map_size)
		map_index = map_size - 1;
	return map[map_index];
}

static inline void GLColorIndexToBGRA8(uint8_t index, uint8_t out_bgra[4])
{
	out_bgra[0] = index;
	out_bgra[1] = index;
	out_bgra[2] = index;
	out_bgra[3] = 0xffu;
}

static inline bool GLColorIndexPixelMapsDefined(int i_to_r_size,
                                                int i_to_g_size,
                                                int i_to_b_size,
                                                int i_to_a_size)
{
	return i_to_r_size > 0 || i_to_g_size > 0 ||
	       i_to_b_size > 0 || i_to_a_size > 0;
}

static inline void GLColorIndexToBGRA8WithPalette(uint8_t index,
                                                  const uint8_t *palette_rgb,
                                                  int palette_entries,
                                                  uint8_t out_bgra[4])
{
	if (!palette_rgb || palette_entries <= 0) {
		GLColorIndexToBGRA8(index, out_bgra);
		return;
	}

	int palette_index = index;
	if (palette_index >= palette_entries)
		palette_index = palette_entries - 1;
	const uint8_t *entry = palette_rgb + palette_index * 3;
	out_bgra[0] = entry[2];
	out_bgra[1] = entry[1];
	out_bgra[2] = entry[0];
	out_bgra[3] = 0xffu;
}

static inline void GLColorIndexToBGRA8WithPixelMaps(uint8_t index,
                                                    const float *i_to_r,
                                                    int i_to_r_size,
                                                    const float *i_to_g,
                                                    int i_to_g_size,
                                                    const float *i_to_b,
                                                    int i_to_b_size,
                                                    const float *i_to_a,
                                                    int i_to_a_size,
                                                    uint8_t out_bgra[4])
{
	const float normalized_index = (float)index / 255.0f;
	const float red = GLColorIndexLookupPixelMap(index, i_to_r, i_to_r_size,
	                                             normalized_index);
	const float green = GLColorIndexLookupPixelMap(index, i_to_g, i_to_g_size,
	                                               normalized_index);
	const float blue = GLColorIndexLookupPixelMap(index, i_to_b, i_to_b_size,
	                                              normalized_index);
	const float alpha = GLColorIndexLookupPixelMap(index, i_to_a, i_to_a_size,
	                                               1.0f);
	out_bgra[0] = GLColorIndexFloatToByte(blue);
	out_bgra[1] = GLColorIndexFloatToByte(green);
	out_bgra[2] = GLColorIndexFloatToByte(red);
	out_bgra[3] = GLColorIndexFloatToByte(alpha);
}

static inline void GLColorIndexToBGRA8Resolved(uint8_t index,
                                               const float *i_to_r,
                                               int i_to_r_size,
                                               const float *i_to_g,
                                               int i_to_g_size,
                                               const float *i_to_b,
                                               int i_to_b_size,
                                               const float *i_to_a,
                                               int i_to_a_size,
                                               const uint8_t *palette_rgb,
                                               int palette_entries,
                                               uint8_t out_bgra[4])
{
	if (GLColorIndexPixelMapsDefined(i_to_r_size, i_to_g_size,
	                                 i_to_b_size, i_to_a_size)) {
		GLColorIndexToBGRA8WithPixelMaps(index,
			i_to_r, i_to_r_size,
			i_to_g, i_to_g_size,
			i_to_b, i_to_b_size,
			i_to_a, i_to_a_size,
			out_bgra);
		return;
	}

	GLColorIndexToBGRA8WithPalette(index, palette_rgb, palette_entries,
	                               out_bgra);
}

#endif /* GL_COLOR_INDEX_POLICY_H */
