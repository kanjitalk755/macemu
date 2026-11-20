/*
 *  dsp_fragment_name_policy.h - DrawSprocket CFM fragment-name aliases.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef DSP_FRAGMENT_NAME_POLICY_H
#define DSP_FRAGMENT_NAME_POLICY_H

static inline int DSpFragmentCandidateCount(void)
{
	return 7;
}

static inline const char *DSpFragmentCandidateAt(int index)
{
	static const char * const candidates[] = {
		"\017DrawSprocketLib",      /* canonical per DSp SDK */
		"\024DrawSprocket 1.7.5.0", /* current four-component alias */
		"\022DrawSprocket 1.7.5",   /* current three-component alias */
		"\022DrawSprocket 1.7.3",   /* Diablo II-era exact alias */
		"\022DrawSprocket 1.7.0",   /* older exact alias */
		"\020DrawSprocket 1.7",     /* two-component fallback */
		"\014DrawSprocket",         /* bare-name fallback */
	};
	return (index >= 0 && index < DSpFragmentCandidateCount())
		? candidates[index]
		: 0;
}

#endif
