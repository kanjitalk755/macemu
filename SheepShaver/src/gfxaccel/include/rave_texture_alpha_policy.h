/*
 *  rave_texture_alpha_policy.h - RAVE texture alpha policy helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef RAVE_TEXTURE_ALPHA_POLICY_H
#define RAVE_TEXTURE_ALPHA_POLICY_H

#include <stdint.h>

#define RAVE_GL_ZERO                      0x0000u
#define RAVE_GL_ONE                       0x0001u

#define RAVE_TEXTURE_OP_FORCE_OPAQUE_ALPHA 0x40000000

static inline int RaveTextureDiagUsesOpaqueAlphaGuard(uint32_t diag_alpha_zero)
{
	return diag_alpha_zero == 0;
}

static inline int RaveTextureRgbCoverageIsMostlyBlack(uint32_t rgb_nonzero_pixels,
                                                       uint32_t total_pixels)
{
	if (total_pixels == 0) return 0;
	return rgb_nonzero_pixels <= (total_pixels / 16u);
}

#endif /* RAVE_TEXTURE_ALPHA_POLICY_H */
