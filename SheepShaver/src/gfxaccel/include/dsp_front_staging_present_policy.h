/*
 *  dsp_front_staging_present_policy.h - front-staging change detection.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef DSP_FRONT_STAGING_PRESENT_POLICY_H
#define DSP_FRONT_STAGING_PRESENT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

struct DSpFrontStagingPresentState {
	bool     valid;
	bool     encoded;
	uint32_t last_hash;
	uint32_t last_size;
	uint32_t last_gamma_gen;
	uint32_t last_fade_active;
	uint32_t unchanged_skips;
};

static inline uint32_t DSpFrontStagingHashBytes(const uint8_t *bytes,
                                                uint32_t size)
{
	uint32_t hash = 2166136261u;
	if (bytes == 0) return hash;
	for (uint32_t i = 0; i < size; i++) {
		hash ^= (uint32_t)bytes[i];
		hash *= 16777619u;
	}
	return hash;
}

static inline void DSpFrontStagingRememberHashForGamma(
	DSpFrontStagingPresentState *state,
	uint32_t hash,
	uint32_t size,
	uint32_t gamma_gen,
	uint32_t fade_active)
{
	if (state == 0) return;
	state->valid = true;
	state->encoded = true;
	state->last_hash = hash;
	state->last_size = size;
	state->last_gamma_gen = gamma_gen;
	state->last_fade_active = fade_active;
	state->unchanged_skips = 0;
}

static inline void DSpFrontStagingRememberHash(
	DSpFrontStagingPresentState *state,
	uint32_t hash,
	uint32_t size)
{
	DSpFrontStagingRememberHashForGamma(state, hash, size, 0, 0);
}

static inline void DSpFrontStagingRememberSeedHash(
	DSpFrontStagingPresentState *state,
	uint32_t hash,
	uint32_t size)
{
	if (state == 0) return;
	state->valid = true;
	state->encoded = false;
	state->last_hash = hash;
	state->last_size = size;
	state->last_gamma_gen = 0;
	state->last_fade_active = 0;
	state->unchanged_skips = 0;
}

static inline void DSpFrontStagingRememberSeedBytes(
	DSpFrontStagingPresentState *state,
	const uint8_t *bytes,
	uint32_t size)
{
	DSpFrontStagingRememberSeedHash(
		state,
		DSpFrontStagingHashBytes(bytes, size),
		size);
}

static inline bool DSpFrontStagingShouldEncodeHashForGamma(
	DSpFrontStagingPresentState *state,
	uint32_t hash,
	uint32_t size,
	uint32_t gamma_gen,
	uint32_t fade_active)
{
	if (state == 0) return true;
	if (state->valid &&
	    state->encoded &&
	    state->last_hash == hash &&
	    state->last_size == size &&
	    state->last_gamma_gen == gamma_gen &&
	    state->last_fade_active == fade_active) {
		state->unchanged_skips++;
		return false;
	}
	DSpFrontStagingRememberHashForGamma(state, hash, size, gamma_gen,
	                                    fade_active);
	return true;
}

static inline bool DSpFrontStagingShouldEncodeHash(
	DSpFrontStagingPresentState *state,
	uint32_t hash,
	uint32_t size)
{
	return DSpFrontStagingShouldEncodeHashForGamma(state, hash, size, 0, 0);
}

#endif /* DSP_FRONT_STAGING_PRESENT_POLICY_H */
