/*
 *  rave_ati_tag_policy.h - ATI RAVE private tag defaults
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef RAVE_ATI_TAG_POLICY_H
#define RAVE_ATI_TAG_POLICY_H

#include <stdint.h>

#define RAVE_ATI_TAG_COUNT 43  /* kATITriCache(0) through kATIMeshAsStrip(42) */

static constexpr uint32_t kRaveATIPrivateTagBase = 1000u;
static constexpr uint32_t kRaveATIRage128RendererID = 0x00021000u;

/*
 * Unreal Tournament's first-time renderer picker creates a tiny RAVE context,
 * then reads ATI private int tag 1011.  Returning a boolean 1 leaves RAVE
 * disabled; match the Rage 128 renderer family that we advertise through RAVE
 * and AGL so the game takes its Rage 128 renderer path.
 */
static constexpr uint32_t kRaveATIUnrealTournamentProbeTag = 1011u;
static constexpr uint32_t kRaveATIUnrealTournamentProbeIndex =
	kRaveATIUnrealTournamentProbeTag - kRaveATIPrivateTagBase;

static constexpr uint32_t kRaveATIRaveExtFuncsTag = 1021u;
static constexpr uint32_t kRaveATIRaveExtFuncsIndex =
	kRaveATIRaveExtFuncsTag - kRaveATIPrivateTagBase;
static constexpr uint32_t kRaveATIRaveExtFuncsEntryCount = 4u;
static constexpr uint32_t kRaveATIRaveExtFuncsSlotClearDrawBuffer = 0u;
static constexpr uint32_t kRaveATIRaveExtFuncsSlotClearZBuffer = 1u;
static constexpr uint32_t kRaveATIRaveExtFuncsSlotTextureUpdate = 2u;
static constexpr uint32_t kRaveATIRaveExtFuncsSlotBindCodeBook = 3u;

static constexpr uint32_t kRaveATIDepthWriteEnableTag = 1022u;
static constexpr uint32_t kRaveATIDepthWriteEnableIndex =
	kRaveATIDepthWriteEnableTag - kRaveATIPrivateTagBase;

static inline uint32_t RaveATIDefaultIntTagValue(uint32_t tag)
{
	if (tag == kRaveATIUnrealTournamentProbeTag) return kRaveATIRage128RendererID;
	if (tag == kRaveATIDepthWriteEnableTag) return 1u;
	return 0u;
}

static inline void RaveATIInitializeIntDefaults(uint32_t *intSlots,
                                                uint32_t slotCount)
{
	if (slotCount > kRaveATIUnrealTournamentProbeIndex) {
		intSlots[kRaveATIUnrealTournamentProbeIndex] =
			RaveATIDefaultIntTagValue(kRaveATIUnrealTournamentProbeTag);
	}
	if (slotCount > kRaveATIDepthWriteEnableIndex) {
		intSlots[kRaveATIDepthWriteEnableIndex] =
			RaveATIDefaultIntTagValue(kRaveATIDepthWriteEnableTag);
	}
}

#endif /* RAVE_ATI_TAG_POLICY_H */
