/*
 *  dsp_get_attributes_policy.h - DrawSprocket GetAttributes compatibility.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef DSP_GET_ATTRIBUTES_POLICY_H
#define DSP_GET_ATTRIBUTES_POLICY_H

#include "dsp_display_mode_policy.h"
#include "dsp_engine.h"

static inline uint32_t DSpDepthMaskForDepth(uint32_t depth)
{
	switch (depth) {
	case 1:  return kDSpDepthMask_1;
	case 2:  return kDSpDepthMask_2;
	case 4:  return kDSpDepthMask_4;
	case 8:  return kDSpDepthMask_8;
	case 16: return kDSpDepthMask_16;
	case 32: return kDSpDepthMask_32;
	default: return 0;
	}
}

static inline void DSpNormalizeGetAttributesForState(DSpContextAttributes *attr,
                                                     uint32_t state)
{
	if (attr == nullptr) return;
	if (state != (uint32_t)kDSpContextState_Active) return;

	const uint32_t display_depth =
	    DSpDisplayModeDepth(attr->backBufferBestDepth, attr->displayBestDepth);
	if (display_depth == 0 || display_depth == attr->backBufferBestDepth) return;

	const uint32_t display_mask = DSpDepthMaskForDepth(display_depth);
	if (display_mask == 0) return;

	attr->backBufferDepthMask = display_mask;
	attr->displayDepthMask = display_mask;
	attr->backBufferBestDepth = display_depth;
	attr->displayBestDepth = display_depth;
}

#endif /* DSP_GET_ATTRIBUTES_POLICY_H */
