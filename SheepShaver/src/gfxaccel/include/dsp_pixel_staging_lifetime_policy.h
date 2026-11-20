/*
 *  dsp_pixel_staging_lifetime_policy.h - guest pixel staging ownership.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef DSP_PIXEL_STAGING_LIFETIME_POLICY_H
#define DSP_PIXEL_STAGING_LIFETIME_POLICY_H

#include <stdbool.h>
#include <stdint.h>

static inline bool DSpPixelStagingShouldReturnExposedAllocationToMacHeap(
	bool allocated_from_mac_system_heap)
{
	(void)allocated_from_mac_system_heap;
	return false;
}

static inline bool DSpPixelStagingCanReuseQuarantinedAllocation(
	uint32_t allocation_size,
	uint32_t required_size)
{
	return allocation_size != 0 &&
	       required_size != 0 &&
	       allocation_size >= required_size;
}

#endif /* DSP_PIXEL_STAGING_LIFETIME_POLICY_H */
