/*
 *  gfx_color_policy.h - shared display gamma policy for accelerated paths.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef GFX_COLOR_POLICY_H
#define GFX_COLOR_POLICY_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * DMC snapshots store the Mac-side gamma/fade LUT: identity means "full
 * classic Mac intensity". The compositor converts that source LUT into the
 * display-ready LUT sampled by every visible path. Outside active DSp fades,
 * the default display policy composes the classic Mac 1.8 gamma space into
 * the modern sRGB-ish 2.2 display space. During an active fade, the LUT is
 * used directly so DSp's linear fade ramps stay linear instead of being
 * warped by a static display correction.
 *
 * Single-owner rule: on iOS the compositor LUT is the ONLY place driver
 * gamma is applied. The legacy CPU bake in video.cpp cscSetEntries
 * (palette entries pre-multiplied through the guest gamma table) is
 * disabled under TARGET_OS_IPHONE — with the LUT also carrying the guest
 * table, the bake would apply gamma twice on every indexed path. Any new
 * consumer of csSave->gammaTable must route through
 * publish_gamma_lut_to_display_controller instead of baking.
 */

static inline uint8_t GfxColorByteFromUnitFloat(float unit)
{
	if (!(unit > 0.0f)) return 0;
	if (unit >= 1.0f) return 255;
	return (uint8_t)(unit * 255.0f + 0.5f);
}

static inline uint8_t GfxColorClassicMacToSRGBByte(uint8_t value)
{
	if (value == 0 || value == 255) return value;
	const float unit = (float)value / 255.0f;
	return GfxColorByteFromUnitFloat(powf(unit, 1.8f / 2.2f));
}

static inline void GfxColorFillIdentityGammaLUT(uint8_t *lut)
{
	if (lut == NULL) return;
	for (int i = 0; i < 256; i++) {
		lut[i]       = (uint8_t)i;
		lut[256 + i] = (uint8_t)i;
		lut[512 + i] = (uint8_t)i;
	}
}

// apply_correction follows the user "Gamma ramp" pref: when true (OS defined)
// the classic-Mac 1.8 -> sRGB 2.2 display correction is composed in as before;
// when false (Linear) the LUT is left as a passthrough so the guest table is
// presented raw, giving the darker / less color-distorted image the pref
// promises and removing the display-side midtone lift.
static inline void GfxColorFillDefaultDisplayGammaLUT(uint8_t *lut,
                                                      bool apply_correction)
{
	if (lut == NULL) return;
	if (!apply_correction) {
		GfxColorFillIdentityGammaLUT(lut);
		return;
	}
	for (int i = 0; i < 256; i++) {
		const uint8_t corrected = GfxColorClassicMacToSRGBByte((uint8_t)i);
		lut[i]       = corrected;
		lut[256 + i] = corrected;
		lut[512 + i] = corrected;
	}
}

static inline void GfxColorBuildDisplayGammaLUT(const uint8_t *mac_lut,
                                                bool fade_active,
                                                bool apply_correction,
                                                uint8_t *display_lut)
{
	if (mac_lut == NULL || display_lut == NULL) return;

	// A fade ramp must stay linear, and Linear gamma mode also wants the guest
	// table verbatim — both are a straight passthrough of mac_lut.
	if (fade_active || !apply_correction) {
		memcpy(display_lut, mac_lut, 768);
		return;
	}

	for (int i = 0; i < 768; i++) {
		display_lut[i] = GfxColorClassicMacToSRGBByte(mac_lut[i]);
	}
}

#endif /* GFX_COLOR_POLICY_H */
