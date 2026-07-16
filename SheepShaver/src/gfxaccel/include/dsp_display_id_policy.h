/*
 *  dsp_display_id_policy.h - DrawSprocket DisplayID helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef DSP_DISPLAY_ID_POLICY_H
#define DSP_DISPLAY_ID_POLICY_H

#include <stdint.h>

enum {
	kDSpDisplayID640x480   = 0x81,
	kDSpDisplayIDW640x480  = 0x82,
	kDSpDisplayID800x600   = 0x83,
	kDSpDisplayIDW800x600  = 0x84,
	kDSpDisplayID1024x768  = 0x85,
	kDSpDisplayID1152x768  = 0x86,
	kDSpDisplayID1152x900  = 0x87,
	kDSpDisplayID1280x1024 = 0x88,
	kDSpDisplayID1600x1200 = 0x89,
	kDSpDisplayIDCustom    = 0x8a
};

static inline uint32_t DSpDisplayIDForMode(uint32_t width, uint32_t height)
{
	if (width == 640 && height == 480) return kDSpDisplayID640x480;
	if (width == 800 && height == 600) return kDSpDisplayID800x600;
	if (width == 1024 && height == 768) return kDSpDisplayID1024x768;
	if (width == 1152 && height == 768) return kDSpDisplayID1152x768;
	if (width == 1152 && height == 900) return kDSpDisplayID1152x900;
	if (width == 1280 && height == 1024) return kDSpDisplayID1280x1024;
	if (width == 1600 && height == 1200) return kDSpDisplayID1600x1200;
	return kDSpDisplayIDCustom;
}

static inline bool DSpAcceptsSingleDisplayID(uint32_t display_id)
{
	(void)display_id;
	return true;
}

#endif /* DSP_DISPLAY_ID_POLICY_H */
