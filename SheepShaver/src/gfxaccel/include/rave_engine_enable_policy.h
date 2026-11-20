/*
 *  rave_engine_enable_policy.h - RAVE manager enable/disable policy
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef RAVE_ENGINE_ENABLE_POLICY_H
#define RAVE_ENGINE_ENABLE_POLICY_H

#include <stdint.h>

#include "rave_engine_identity.h"

static const uint32_t kRaveVendorBestChoice = 0xffffffffu;

static inline bool RaveEngineEnableHandledByNative(uint32_t vendorID, uint32_t engineID)
{
	(void)engineID;
	return vendorID == kRaveVendorBestChoice || vendorID == kRaveAdvertisedVendorID;
}

#endif
