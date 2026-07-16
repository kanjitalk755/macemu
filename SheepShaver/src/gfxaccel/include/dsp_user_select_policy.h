/*
 *  dsp_user_select_policy.h - DrawSprocket user-select context policy.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  DSpCanUserSelectContext answers whether more than one context satisfies
 *  the requested minimum attributes. This is a mode-choice question, not a
 *  physical-monitor count question.
 */

#ifndef DSP_USER_SELECT_POLICY_H
#define DSP_USER_SELECT_POLICY_H

#include <stddef.h>
#include <stdint.h>

#include "dsp_engine.h"

static inline uint32_t DSpUserSelectDepthMaskForDepth(uint32_t depth)
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

static inline bool DSpUserSelectDepthAllowed(uint32_t mask, uint32_t depth)
{
	if (mask == 0) return true;
	const uint32_t bit = DSpUserSelectDepthMaskForDepth(depth);
	return bit != 0 && (mask & bit) != 0;
}

static inline uint32_t DSpUserSelectRequestedDisplayDepthMask(
    const DSpContextAttributes &req)
{
	return req.displayDepthMask != 0 ? req.displayDepthMask
	                                 : req.backBufferDepthMask;
}

static inline uint32_t DSpUserSelectRequestedDisplayBestDepth(
    const DSpContextAttributes &req)
{
	return req.displayBestDepth != 0 ? req.displayBestDepth
	                                 : req.backBufferBestDepth;
}

static inline bool DSpContextMeetsUserSelectRequest(
    const DSpContextAttributes &mode, const DSpContextAttributes &req)
{
	if (req.displayWidth != 0 && mode.displayWidth < req.displayWidth) {
		return false;
	}
	if (req.displayHeight != 0 && mode.displayHeight < req.displayHeight) {
		return false;
	}
	const uint32_t desiredDisplayDepth =
	    DSpUserSelectRequestedDisplayBestDepth(req);
	if (desiredDisplayDepth != 0 &&
	    mode.displayBestDepth < desiredDisplayDepth) {
		return false;
	}
	if (!DSpUserSelectDepthAllowed(
	        DSpUserSelectRequestedDisplayDepthMask(req),
	        mode.displayBestDepth)) {
		return false;
	}
	if (req.pageCount != 0 && mode.pageCount < req.pageCount) {
		return false;
	}
	return true;
}

static inline size_t DSpCountUserSelectableContexts(
    const DSpContextAttributes *modes, size_t count,
    const DSpContextAttributes &req)
{
	if (modes == nullptr) return 0;

	size_t selectable = 0;
	for (size_t i = 0; i < count; i++) {
		if (DSpContextMeetsUserSelectRequest(modes[i], req)) {
			selectable++;
		}
	}
	return selectable;
}

static inline bool DSpCanUserSelectContextFromCount(size_t selectableCount)
{
	return selectableCount > 1;
}

#endif /* DSP_USER_SELECT_POLICY_H */
