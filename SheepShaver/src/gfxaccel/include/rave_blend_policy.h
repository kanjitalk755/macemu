/*
 *  rave_blend_policy.h - RAVE blend-mode output policy helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef RAVE_BLEND_POLICY_H
#define RAVE_BLEND_POLICY_H

static inline int RaveBlendModeUsesPremultipliedOutput(int blend_mode)
{
	return blend_mode == 0;
}

static inline int RaveBlendModeUsesGLPipeline(int blend_mode)
{
	return blend_mode == 2;
}

#endif /* RAVE_BLEND_POLICY_H */
