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

static const uint32_t kRaveATIPrivateTagBase = 1000u;
static const uint32_t kRaveATIRage128RendererID = 0x00021000u;

/*
 * Unreal Tournament's first-time renderer picker creates a tiny RAVE context,
 * then reads ATI private int tag 1011.  Returning a boolean 1 leaves RAVE
 * disabled; match the Rage 128 renderer family that we advertise through RAVE
 * and AGL so the game takes its Rage 128 renderer path.
 */
static const uint32_t kRaveATIUnrealTournamentProbeTag = 1011u;
static const uint32_t kRaveATIUnrealTournamentProbeIndex =
	kRaveATIUnrealTournamentProbeTag - kRaveATIPrivateTagBase;

static const uint32_t kRaveATIRaveExtFuncsTag = 1021u;
static const uint32_t kRaveATIRaveExtFuncsIndex =
	kRaveATIRaveExtFuncsTag - kRaveATIPrivateTagBase;
/*
 * The real ATI table has at least 5 entries: Myth II (render_rave.c) calls
 * slot 4 as GetDrawBuffer(ctx, TQADevice*) right after sync() and reads
 * rowBytes (+4) and baseAddr (+0x14) from the returned TQADevice, so any
 * caller-provided struct is at least 5 pointers long. We over-allocate our
 * own table to 8 entries (slots 5-7 filled with a safe stub) in case other
 * titles index further, but only ever write 5 slots into a caller-provided
 * struct because 5 is the largest size proven by a real client.
 */
static const uint32_t kRaveATIRaveExtFuncsEntryCount = 8u;
static const uint32_t kRaveATIRaveExtFuncsKnownSlotCount = 5u;
static const uint32_t kRaveATIRaveExtFuncsSlotClearDrawBuffer = 0u;
static const uint32_t kRaveATIRaveExtFuncsSlotClearZBuffer = 1u;
static const uint32_t kRaveATIRaveExtFuncsSlotTextureUpdate = 2u;
static const uint32_t kRaveATIRaveExtFuncsSlotBindCodeBook = 3u;
static const uint32_t kRaveATIRaveExtFuncsSlotGetDrawBuffer = 4u;

static const uint32_t kRaveATIDepthWriteEnableTag = 1022u;
static const uint32_t kRaveATIDepthWriteEnableIndex =
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
