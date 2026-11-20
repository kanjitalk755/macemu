/*
 *  dsp_version_policy.h - app-visible DrawSprocket version policy.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef DSP_VERSION_POLICY_H
#define DSP_VERSION_POLICY_H

#include <stdint.h>

/* Macro form throughout: the packed version has to be usable as a case
 * label and an initialiser in both C and C++. */
#define kDSpVersionStage_Final 0x80u

#define DSpPackNumVersion(major, minor, bugfix, stage, nonReleaseRevision) \
	((((uint32_t)(major)) << 24) | \
	 ((((uint32_t)((((minor) & 0x0fu) << 4) | \
	               ((bugfix) & 0x0fu)))) << 16) | \
	 (((uint32_t)(stage)) << 8) | \
	 ((uint32_t)(nonReleaseRevision)))

#define kDSpVersion_1_7_5_0 \
	DSpPackNumVersion(1u, 7u, 5u, kDSpVersionStage_Final, 0u)
#define kDSpVersion_Current kDSpVersion_1_7_5_0
#define kDSpGestaltSelector 0x64737076u /* 'dspv' */

#endif
