/*
 *  dsp_video_status_policy.h - DrawSprocket Display Manager status override.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef DSP_VIDEO_STATUS_POLICY_H
#define DSP_VIDEO_STATUS_POLICY_H

#include <stdint.h>

#include "display_mode_controller.h"
#include "dsp_display_id_policy.h"
#include "video.h"

static inline bool DSpVideoStatusForSnapshot(const DMCModeSnapshot *snap,
                                             const VideoInfo *modes,
                                             uint16 *apple_mode,
                                             uint32 *apple_id)
{
	if (snap == nullptr || modes == nullptr) return false;
	if (snap->active_owner != (uint32_t)kDMCOwnerDSp) return false;
	if (snap->width == 0 || snap->height == 0 || snap->depth == 0) return false;

	const uint32 mode = (uint32)DepthModeForPixelDepth((int)snap->depth);
	const uint32 preferred_id = DSpDisplayIDForMode(snap->width, snap->height);
	const VideoInfo *fallback = nullptr;
	for (const VideoInfo *v = modes; v->viType != DIS_INVALID; v++) {
		if ((uint32)v->viXsize != snap->width) continue;
		if ((uint32)v->viYsize != snap->height) continue;
		if (v->viAppleMode != mode) continue;

		if (v->viAppleID != preferred_id) {
			if (fallback == nullptr) fallback = v;
			continue;
		}

		if (apple_mode != nullptr) *apple_mode = (uint16)v->viAppleMode;
		if (apple_id != nullptr) *apple_id = v->viAppleID;
		return true;
	}

	if (fallback != nullptr) {
		if (apple_mode != nullptr) *apple_mode = (uint16)fallback->viAppleMode;
		if (apple_id != nullptr) *apple_id = fallback->viAppleID;
		return true;
	}

	return false;
}

#endif /* DSP_VIDEO_STATUS_POLICY_H */
