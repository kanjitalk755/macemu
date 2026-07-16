/*
 *  gl_drawable_owner_policy.h - AGL drawable display-owner restore helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef GL_DRAWABLE_OWNER_POLICY_H
#define GL_DRAWABLE_OWNER_POLICY_H

#include "display_mode_controller.h"

static inline bool GLShouldSnapshotDrawableOwner(bool had_drawable,
                                                 bool snapshot_valid)
{
	return !had_drawable && !snapshot_valid;
}

static inline uint32_t GLRestorableDrawableOwner(uint32_t previous_owner)
{
	switch (previous_owner) {
	case kDMCOwnerRAVE:
	case kDMCOwnerDSp:
		return previous_owner;
	default:
		return kDMCOwnerQuickDraw;
	}
}

#endif /* GL_DRAWABLE_OWNER_POLICY_H */
