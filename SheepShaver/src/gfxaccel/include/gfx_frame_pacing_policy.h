/*
 *  gfx_frame_pacing_policy.h - 3D frame pacing policy.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef GFX_FRAME_PACING_POLICY_H
#define GFX_FRAME_PACING_POLICY_H

#include <stdint.h>

enum GfxFramePacingEngine {
	kGfxFramePacingEngineDSp = 0,
	kGfxFramePacingEngineGL = 1,
	kGfxFramePacingEngineRAVE = 2,
	kGfxFramePacingEngineLegacy = 3,
	kGfxFramePacingEngineCount = 4
};

/*
 * Deadline pacing is per engine. Each engine advances one deadline per
 * submitted 3D frame and never consumes banked VBL credits from another
 * engine. When display-link ticks stop, callers fall back to the current
 * cadence rather than the old fixed 33 ms semaphore timeout.
 *
 * Satisfied-boundary rule: a vblank boundary that has already passed
 * since an engine's previous sync counts as that sync's boundary — the
 * call returns without sleeping and the engine's deadline chain advances
 * to the next future boundary. Consequences this rule exists for:
 *   - Co-resident engines on the shared main==emul thread (DSp+GL: The
 *     Sims) pace to the SAME vblank: the first engine's wait carries the
 *     thread across the boundary, the second engine sees it satisfied.
 *     Without this, engines paced to consecutive boundaries — two
 *     periods per guest frame, a structural half-rate cap.
 *   - A frame that rendered longer than one period is not penalized
 *     with an additional full-period wait (adaptive, not hard-vsync).
 *   - The DSpContext_SetMaxFrameRate throttle loop (N syncs per swap)
 *     counts render time toward the cap instead of stacking N full
 *     waits on top of it.
 */
static inline uint64_t GfxFramePacingClampCadenceUsec(uint64_t cadence_usec)
{
	if (cadence_usec < 8333) return 8333;     /* 120 Hz ceiling */
	if (cadence_usec > 33333) return 33333;   /* 30 Hz floor */
	return cadence_usec;
}

#define GFX_FRAME_PACING_DEFAULT_USEC 16667
#define GFX_FRAME_PACING_STALE_TICKS  4

#endif /* GFX_FRAME_PACING_POLICY_H */
