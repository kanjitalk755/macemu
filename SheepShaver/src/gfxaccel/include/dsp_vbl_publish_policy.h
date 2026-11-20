/*
 *  dsp_vbl_publish_policy.h - DSp VBL publish gating.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef DSP_VBL_PUBLISH_POLICY_H
#define DSP_VBL_PUBLISH_POLICY_H

#include <stdbool.h>
#include <stdint.h>

static inline bool DSpShouldPublishActiveContextOnVBL(uint32_t active_owner,
                                                      uint32_t dsp_owner,
                                                      bool has_active_context,
                                                      bool has_presentable_front_staging,
                                                      bool explicit_swap_observed)
{
	if (!has_active_context) return false;
	if (active_owner != dsp_owner) return has_presentable_front_staging;
	return has_presentable_front_staging || !explicit_swap_observed;
}

static inline bool DSpShouldFlushNQDBeforeStagingDrain(bool has_back_buffer_staging,
                                                       bool has_presentable_front_staging)
{
	return has_back_buffer_staging || has_presentable_front_staging;
}

#endif /* DSP_VBL_PUBLISH_POLICY_H */
