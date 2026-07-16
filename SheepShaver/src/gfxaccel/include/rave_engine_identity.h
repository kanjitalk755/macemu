/*
 *  rave_engine_identity.h - Advertised RAVE hardware identity
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef RAVE_ENGINE_IDENTITY_H
#define RAVE_ENGINE_IDENTITY_H

#include <stdint.h>

/*
 * Keep these distinct from RAVE_ENGINE_MAGIC. The magic value is an internal
 * sentinel handle marker; these are the hardware IDs returned to guest RAVE
 * clients during engine enumeration.
 *
 * Classic games such as Unreal Tournament use the engine identity as part of
 * their hardware renderer picker. Advertise the same ATI Rage 128 family ID
 * used by the AGL renderer policy instead of PocketShaver's internal magic.
 */
static const uint32_t kRaveAdvertisedVendorID = 1u;          // kQAVendor_ATI
static const uint32_t kRaveAdvertisedEngineID = 0x00021000u; // ATI Rage 128
static const uint32_t kRaveAdvertisedRevision = 0x00010000u; // 1.0
static const uint32_t kRaveAdvertisedTextureMemoryBytes = 64u * 1024u * 1024u;

#endif
