/*
 *  rave_mipmap_bias_policy.h - RAVE mipmap-bias translation helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef RAVE_MIPMAP_BIAS_POLICY_H
#define RAVE_MIPMAP_BIAS_POLICY_H

/*
 * RAVE's kQATag_MipmapBias default is 0.5. Metal's sample bias is centered at
 * 0.0, where positive values choose lower-detail mip levels. Translate the RAVE
 * value into Metal's centered space before passing it to tex.sample().
 */
static inline float RaveMetalSamplerMipBias(float rave_bias)
{
	return rave_bias - 0.5f;
}

#endif /* RAVE_MIPMAP_BIAS_POLICY_H */
