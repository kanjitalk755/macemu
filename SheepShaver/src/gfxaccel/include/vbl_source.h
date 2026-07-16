/*
 *  vbl_source.h - VBL tick source wrapping CAMetalDisplayLink (iOS 17+) /
 *                 CADisplayLink (13.4-16).
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Display-link abstraction that synchronizes frame presentation to the
 *  device's native refresh rate.  Wraps CAMetalDisplayLink on iOS 17+
 *  (delivers drawable + target presentation timestamp) and falls back to
 *  CADisplayLink on iOS 13.4-16 (delivers timing only; caller must use
 *  nextDrawable).
 *
 *  Design notes:
 *    - @available(iOS 17, *) branch inside vbl_source_init().
 *    - vbl_source_get_cadence_usec() for the dynamic frame interval.
 *    - vbl_source_get_tick_count() monotonic counter.
 *    - 3D deadline pacing uses mach_wait_until with independent per-engine
 *      frame deadlines.
 *
 *  Threading: VBL callbacks fire on the main RunLoop thread.  Tick count
 *  and cadence are read from the emul thread via C11 _Atomic.
 *
 *  C-callable throughout: the header can be included from .cpp, .mm, or
 *  Swift-via-bridging-header without pulling in QuartzCore / Metal types.
 *  CAMetalLayer* is passed as void* and bridge-cast in vbl_source.mm.
 */

#ifndef VBL_SOURCE_H
#define VBL_SOURCE_H

#include <stdint.h>
#include "gfx_frame_pacing_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Error codes extending the -4000 block (continuing from -4009).
 */
enum GfxAccelVBLError {
	kGfxAccelErrVBLNotInitialized = -4009,
	kGfxAccelErrVBLTimeout        = -4010,
	kGfxAccelErrVBLAlreadyRunning = -4011
};

/*
 * VBL callback signature.
 *   ctx                     - opaque user context passed back every callback.
 *   drawable                - id<CAMetalDrawable> as void* on iOS 17+
 *                             (CAMetalDisplayLink delivers it); NULL on
 *                             iOS 13.4-16 (caller must use nextDrawable).
 *   target_presentation_ts  - CFTimeInterval target timestamp for
 *                             present(at:).
 */
typedef void (*VBLSourceCallbackFn)(void *ctx,
                                    void *drawable,
                                    double target_presentation_ts);

/* --- Lifecycle --- */

/*
 * Initialize the VBL source.  cametal_layer is id<CAMetalLayer> as void*.
 * On iOS 17+, creates a CAMetalDisplayLink from the layer.
 * On iOS 13.4-16, creates a CADisplayLink.
 * callback fires on each VBL tick; ctx is passed back unchanged.
 * Returns kGfxAccelNoErr (0) on success, kGfxAccelErrVBLAlreadyRunning
 * if already initialized.
 */
int32_t  vbl_source_init(void *cametal_layer,
                         VBLSourceCallbackFn callback,
                         void *ctx);

/*
 * Shut down the VBL source.  Invalidates display link, nil-s out statics,
 * and resets tick/cadence/deadline state. Idempotent.
 */
void     vbl_source_shutdown(void);

/* --- Queries --- */

/*
 * Current frame interval in microseconds.  Dynamic -- updates
 * with ProMotion rate changes.  Default 16667 (60 Hz) before first
 * callback fires.
 */
uint64_t vbl_source_get_cadence_usec(void);

/*
 * Monotonically increasing tick count, incremented per VBL callback.
 * Returns 0 before first VBL fires.
 */
uint64_t vbl_source_get_tick_count(void);

/*
 * Returns non-zero if CAMetalDisplayLink is active (iOS 17+).
 * When true, callers must NOT call [layer nextDrawable] — drawables
 * are delivered via the VBL callback instead.
 */
int      vbl_source_uses_metal_display_link(void);

/*
 * Pause/resume the display link (for background/suspend).
 * paused != 0 pauses; paused == 0 resumes.
 */
void     vbl_source_set_paused(int paused);

/*
 * Returns nonzero while the calling thread is inside the VBL primary/
 * secondary callback chain (same-thread nesting query; see
 * gfxaccel_threading_policy.h). Used by synchronous lifecycle drains to
 * defer to the in-flight tick's own drain instead of nesting.
 */
int      vbl_source_in_callback_chain(void);

/* --- 3D deadline pacing --- */

/*
 * Blocks the calling thread until that engine's next frame deadline.
 * Deadlines are derived from the last display-link tick plus the current
 * cadence. If ticks stop, the caller falls back to one cadence interval from
 * now instead of a fixed 33 ms timeout. Returns kGfxAccelNoErr on success,
 * kGfxAccelErrVBLTimeout if mach_wait_until fails, or
 * kGfxAccelErrVBLNotInitialized if VBL source is not up.
 */
int32_t  vbl_source_sync_3d_pacing_for_engine(int32_t engine_id);

/*
 * Legacy wrapper kept for staged caller migration. New engine code should call
 * vbl_source_sync_3d_pacing_for_engine() with a GfxFramePacingEngine value.
 */
int32_t  vbl_source_sync_3d_pacing(void);

/*
 * Record a VBL tick for deadline pacing. Called internally from the VBL
 * callback. Exposed here for tests and API continuity; callers should not
 * normally invoke this directly.
 */
void     vbl_source_signal_3d_pacing(void);

/* --- Secondary-callback fan-out --- */

/*
 * Register a secondary VBL callback — fires AFTER the primary callback
 * inside the same main-thread tick. The DSp release-queue drain uses
 * this; the background/foreground drain chains off the same hook.
 * Alternative "hook DSp drain from inside the compositor VBL callback"
 * REJECTED — violates the compositor-blindness invariant
 * (metal_compositor cannot know about DSp).
 *
 * Returns 0 on success; kGfxAccelErrVBLAlreadyRunning if the table is
 * full (max VBL_SECONDARY_CALLBACK_MAX slots). Registration is a
 * one-shot at DSpInit time before any VBL tick fires, so no cross-thread
 * race is possible under the single-writer model.
 */
#define VBL_SECONDARY_CALLBACK_MAX 5   /* adds DSpVBLCompositorPublishCallback as the 5th slot. */
int32_t vbl_source_register_secondary_callback(VBLSourceCallbackFn cb,
                                                void *ctx);
void    vbl_source_unregister_secondary_callback(VBLSourceCallbackFn cb);

#ifdef __cplusplus
}
#endif

#endif /* VBL_SOURCE_H */
