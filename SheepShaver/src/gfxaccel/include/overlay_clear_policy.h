/*
 *  overlay_clear_policy.h - shared accelerated overlay clear policy.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef OVERLAY_CLEAR_POLICY_H
#define OVERLAY_CLEAR_POLICY_H

#include <stdbool.h>

/*
 * RAVE and GL window drawables are premultiplied overlay textures composited
 * over the QuickDraw framebuffer. A zero-alpha guest clear with nonzero RGB is
 * treated as an opaque background clear, matching legacy 3D titles that use
 * glClearColor/RAVE clear state as the scene backdrop. Offscreen GL drawables
 * preserve the guest alpha because they are read back as guest-owned pixels.
 */

static inline float OverlayClearClampAlpha(float guest_clear_alpha)
{
	if (guest_clear_alpha < 0.0f)
		return 0.0f;
	if (guest_clear_alpha > 1.0f)
		return 1.0f;
	return guest_clear_alpha;
}

static inline float OverlayClearEffectiveAlpha(bool is_offscreen,
                                               float guest_clear_alpha)
{
	const float alpha = OverlayClearClampAlpha(guest_clear_alpha);
	if (is_offscreen)
		return alpha;
	return alpha == 0.0f ? 1.0f : alpha;
}

static inline float OverlayClearPremultipliedComponent(
	float guest_clear_component, float effective_clear_alpha)
{
	return guest_clear_component *
	       OverlayClearClampAlpha(effective_clear_alpha);
}

#endif /* OVERLAY_CLEAR_POLICY_H */
