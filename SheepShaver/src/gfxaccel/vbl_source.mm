/*
 *  vbl_source.mm - VBL tick source implementation.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Wraps CAMetalDisplayLink (iOS 17+) and CADisplayLink (13.4-16) behind
 *  the C-callable API declared in vbl_source.h.  The iOS 17+ path
 *  delivers the drawable and target presentation timestamp directly in
 *  the callback; the legacy path delivers timing only.
 *
 *  Design notes:
 *    - @available(iOS 17, *) branch inside vbl_source_init().
 *    - Cadence derived from inter-callback delta (17+) or
 *      CADisplayLink.duration (13.4-16).
 *    - Monotonic uint64 tick count incremented per VBL callback.
 *    - Per-engine 3D deadline pacing via mach_wait_until.
 *
 *  Threading: VBL callbacks fire on the UIKit main RunLoop, which is also
 *  the emul thread in the current iOS build. They can therefore re-enter
 *  gfxaccel at runloop pump points rather than racing in parallel. Tick and
 *  cadence counters remain C11 _Atomic for read-mostly/future-thread-split
 *  access; see gfxaccel_threading_policy.h.
 */

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>

#include "vbl_source.h"
#include "gfxaccel_threading_policy.h"

#import "PerformanceCounterObjCCppHeader.h"

#include <stdatomic.h>
#include <cstdio>
#include <mach/kern_return.h>
#include <mach/mach_time.h>

// ---------------------------------------------------------------------------
// Logging macros
// ---------------------------------------------------------------------------

#define VBL_LOG(fmt, ...) \
	do { printf("[gfxaccel-vbl] " fmt "\n", ##__VA_ARGS__); } while (0)

#define VBL_ERR(fmt, ...) \
	do { printf("[gfxaccel-vbl ERROR] " fmt "\n", ##__VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------
// File-scope static state
// ---------------------------------------------------------------------------

static VBLSourceCallbackFn s_callback     = NULL;
static void               *s_callback_ctx = NULL;
static int                 s_callback_depth = 0;

/* Up to 4 secondary callbacks, invoked AFTER the
 * primary callback in every VBL tick path (CADisplayLink displayLinkFired:
 * AND the iOS 17+ metalDisplayLink:needsUpdate: delegate).
 *
 * Threading: No _Atomic is needed on the secondary-callback array.
 * Registration happens at DSpInit time on the main==emul thread BEFORE any
 * VBL tick fires; deregistration at DSpShutdown; no in-flight mutations.
 * This matches the existing s_callback non-atomic pattern. */
static VBLSourceCallbackFn s_secondary_cb[VBL_SECONDARY_CALLBACK_MAX]  = {NULL};
static void               *s_secondary_ctx[VBL_SECONDARY_CALLBACK_MAX] = {NULL};

static inline void DrainSecondaryCallbacks(void *drawable, double ts)
{
	for (int i = 0; i < VBL_SECONDARY_CALLBACK_MAX; i++) {
		if (s_secondary_cb[i] != NULL) {
			s_secondary_cb[i](s_secondary_ctx[i], drawable, ts);
		}
	}
}

static inline void FireVBLCallbackChain(void *drawable, double ts)
{
	if (s_callback_depth != 0) {
		/* Potentially per-tick for titles whose guest VBL/fade procs pump
		 * the runloop across a vsync boundary — rate-limit to the first
		 * skip plus one summary per 600 skips (~10 s at 60 Hz) so the
		 * always-on VBL_LOG cannot become per-frame stdout spam. */
		static uint64_t s_nested_skip_count = 0;
		s_nested_skip_count++;
		if (s_nested_skip_count == 1 || (s_nested_skip_count % 600u) == 0) {
			VBL_LOG("skipping nested VBL callback chain (skips=%llu)",
			        (unsigned long long)s_nested_skip_count);
		}
		return;
	}

	s_callback_depth++;
	if (s_callback) {
		s_callback(s_callback_ctx, drawable, ts);
	}
	DrainSecondaryCallbacks(drawable, ts);
	s_callback_depth--;

	// FPS tally for the Preferences overlay. Counted here — after the chain
	// completes — rather than at the MetalCompositorPresent site, because that
	// site sits inside the present -> nift-drain window whose timing is frozen
	// for the Diablo II bisection (see the NOTE in MetalCompositorPresent).
	// One completed chain corresponds to one serviced VBL and hence one
	// compositor present in steady state; when emulation stalls, this thread
	// stops pumping and the count (correctly) falls to zero. Exact
	// present-site accounting can return once the nift bisection concludes.
	objc_reportFrameRender();
}

extern "C" int vbl_source_in_callback_chain(void)
{
	return s_callback_depth != 0;
}

// C11 _Atomic for read-mostly counters -- minimal threading exception;
// no mutex needed. Written once per VBL callback on the main==emul thread;
// read by the same thread today and by tests/future split code.
static _Atomic uint64_t    s_tick_count   = 0;
static _Atomic uint64_t    s_cadence_usec = GFX_FRAME_PACING_DEFAULT_USEC;
static _Atomic uint64_t    s_cadence_abs  = 0;
static _Atomic uint64_t    s_last_tick_abs = 0;
static _Atomic uint64_t    s_engine_next_deadline_abs[kGfxFramePacingEngineCount];
static _Atomic uint64_t    s_engine_seen_tick[kGfxFramePacingEngineCount];

static int                  s_initialized      = 0;
static mach_timebase_info_data_t s_timebase     = {0, 0};

// iOS 17+ state -- stored as `id` to avoid compile-time type references
// on older deployment targets.
static id                   s_metal_display_link = nil;

// iOS 13.4-16 state
static id                   s_ca_display_link    = nil;

static uint64_t vbl_source_usec_to_abs(uint64_t usec)
{
	if (s_timebase.numer == 0 || s_timebase.denom == 0) {
		if (mach_timebase_info(&s_timebase) != KERN_SUCCESS ||
		    s_timebase.numer == 0 || s_timebase.denom == 0) {
			return usec * 1000;
		}
	}
	__uint128_t ns = (__uint128_t)usec * 1000u;
	ns *= s_timebase.denom;
	ns /= s_timebase.numer;
	if (ns == 0) return 1;
	return (uint64_t)ns;
}

static void vbl_source_store_cadence_usec(uint64_t cadence_usec)
{
	uint64_t clamped = GfxFramePacingClampCadenceUsec(cadence_usec);
	atomic_store_explicit(&s_cadence_usec, clamped, memory_order_relaxed);
	atomic_store_explicit(&s_cadence_abs, vbl_source_usec_to_abs(clamped),
	                      memory_order_relaxed);
}

static void vbl_source_reset_deadlines(void)
{
	for (int i = 0; i < kGfxFramePacingEngineCount; i++) {
		atomic_store_explicit(&s_engine_next_deadline_abs[i], 0,
		                      memory_order_relaxed);
		atomic_store_explicit(&s_engine_seen_tick[i], 0,
		                      memory_order_relaxed);
	}
}

// ---------------------------------------------------------------------------
// iOS 17+ delegate: CAMetalDisplayLinkDelegate
// ---------------------------------------------------------------------------

API_AVAILABLE(ios(17))
@interface VBLMetalDisplayLinkDelegate : NSObject <CAMetalDisplayLinkDelegate>
@end

@implementation VBLMetalDisplayLinkDelegate

- (void)metalDisplayLink:(CAMetalDisplayLink *)link
             needsUpdate:(CAMetalDisplayLinkUpdate *)update
{
	// Increment tick count
	atomic_fetch_add_explicit(&s_tick_count, 1, memory_order_relaxed);

	// Update cadence from inter-callback delta (more accurate
	// than preferredFrameRateRange).
	static CFTimeInterval s_last_target_ts = 0;
	if (s_last_target_ts > 0 &&
	    update.targetPresentationTimestamp > s_last_target_ts) {
		uint64_t delta_usec = (uint64_t)(
			(update.targetPresentationTimestamp - s_last_target_ts) * 1e6);
		if (delta_usec > 0) {
			vbl_source_store_cadence_usec(delta_usec);
		}
	}
	s_last_target_ts = update.targetPresentationTimestamp;

	// Record VBL time for 3D deadline pacing.
	vbl_source_signal_3d_pacing();

	// Fire primary + secondary callbacks, guarded against same-thread
	// re-entrancy through guest CallMacOS*/runloop pump points.
	FireVBLCallbackChain((__bridge void *)update.drawable,
	                     update.targetPresentationTimestamp);
}

@end

static VBLMetalDisplayLinkDelegate *s_delegate = nil;

// ---------------------------------------------------------------------------
// iOS 13.4-16 target: CADisplayLink callback
// ---------------------------------------------------------------------------

@interface VBLDisplayLinkTarget : NSObject
- (void)displayLinkFired:(CADisplayLink *)link;
@end

@implementation VBLDisplayLinkTarget

- (void)displayLinkFired:(CADisplayLink *)link
{
	// Increment tick count
	atomic_fetch_add_explicit(&s_tick_count, 1, memory_order_relaxed);

	// Update cadence from link.duration
	uint64_t dur_usec = (uint64_t)(link.duration * 1e6);
	if (dur_usec > 0) {
		vbl_source_store_cadence_usec(dur_usec);
	}

	// Record VBL time for 3D deadline pacing.
	vbl_source_signal_3d_pacing();

	// Fire primary + secondary callbacks, guarded against same-thread
	// re-entrancy through guest CallMacOS*/runloop pump points.
	FireVBLCallbackChain(NULL, link.targetTimestamp);
}

@end

static VBLDisplayLinkTarget *s_link_target = nil;

// ---------------------------------------------------------------------------
// Public API: lifecycle
// ---------------------------------------------------------------------------

extern "C"
int32_t vbl_source_init(void *cametal_layer,
                        VBLSourceCallbackFn callback,
                        void *ctx)
{
	if (s_initialized) {
		VBL_ERR("vbl_source_init: already running");
		return kGfxAccelErrVBLAlreadyRunning;
	}

	s_callback     = callback;
	s_callback_ctx = ctx;

	vbl_source_store_cadence_usec(GFX_FRAME_PACING_DEFAULT_USEC);
	vbl_source_reset_deadlines();

	// CAMetalDisplayLink (iOS 17+) is disabled: it forbids
	// [layer nextDrawable] and requires all rendering to happen inside the
	// delegate callback. PocketShaver's current iOS model runs PPC emulation,
	// UIKit pumping, and presentation on the same main==emul thread, so moving
	// rendering wholly inside the delegate would require a major architectural
	// refactor. ProMotion is already honored
	// via CADisplayLink's preferredFrameRateRange = (60,120) — there is no
	// pacing benefit to the revive. Using CADisplayLink for timing for ALL iOS
	// versions; nextDrawable for drawables. vbl_source_uses_metal_display_link()
	// returns 0 by design.
	{
		// --- CADisplayLink path (all iOS versions) ---
		if (cametal_layer == NULL) {
			VBL_LOG("vbl_source_init: nil layer (headless)");
			s_initialized = 1;
			return 0;
		}

		s_link_target = [[VBLDisplayLinkTarget alloc] init];
		CADisplayLink *link =
			[CADisplayLink displayLinkWithTarget:s_link_target
			                            selector:@selector(displayLinkFired:)];

		if (@available(iOS 15, *)) {
			link.preferredFrameRateRange =
				CAFrameRateRangeMake(60, 120, 0);
		} else {
			link.preferredFramesPerSecond = 60;
		}

		[link addToRunLoop:[NSRunLoop mainRunLoop]
		           forMode:NSDefaultRunLoopMode];

		s_ca_display_link = link;
		VBL_LOG("vbl_source_init: CADisplayLink created");
	}

	s_initialized = 1;
	VBL_LOG("vbl_source_init: ready");
	return 0;  // kGfxAccelNoErr
}

extern "C"
void vbl_source_shutdown(void)
{
	if (!s_initialized) {
		return;
	}

	// Invalidate display link
	if (@available(iOS 17, *)) {
		if (s_metal_display_link != nil) {
			CAMetalDisplayLink *link =
				(CAMetalDisplayLink *)s_metal_display_link;
			[link invalidate];
			s_metal_display_link = nil;
			s_delegate = nil;
		}
	}

	if (s_ca_display_link != nil) {
		CADisplayLink *link = (CADisplayLink *)s_ca_display_link;
		[link invalidate];
		s_ca_display_link = nil;
		s_link_target = nil;
	}

	s_callback     = NULL;
	s_callback_ctx = NULL;

	atomic_store_explicit(&s_tick_count, 0, memory_order_relaxed);
	atomic_store_explicit(&s_last_tick_abs, 0, memory_order_relaxed);
	vbl_source_store_cadence_usec(GFX_FRAME_PACING_DEFAULT_USEC);
	vbl_source_reset_deadlines();
	s_initialized = 0;

	VBL_LOG("vbl_source_shutdown: done");
}

// ---------------------------------------------------------------------------
// Public API: queries
// ---------------------------------------------------------------------------

extern "C"
uint64_t vbl_source_get_cadence_usec(void)
{
	return atomic_load_explicit(&s_cadence_usec, memory_order_relaxed);
}

extern "C"
uint64_t vbl_source_get_tick_count(void)
{
	return atomic_load_explicit(&s_tick_count, memory_order_relaxed);
}

extern "C"
int vbl_source_uses_metal_display_link(void)
{
	if (@available(iOS 17, *)) {
		return (s_metal_display_link != nil) ? 1 : 0;
	}
	return 0;
}

extern "C"
void vbl_source_set_paused(int paused)
{
	if (@available(iOS 17, *)) {
		if (s_metal_display_link != nil) {
			CAMetalDisplayLink *link =
				(CAMetalDisplayLink *)s_metal_display_link;
			link.paused = (paused != 0);
		}
	}

	if (s_ca_display_link != nil) {
		CADisplayLink *link = (CADisplayLink *)s_ca_display_link;
		link.paused = (paused != 0);
	}
}

// ---------------------------------------------------------------------------
// Public API: 3D deadline pacing
// ---------------------------------------------------------------------------

extern "C"
int32_t vbl_source_sync_3d_pacing_for_engine(int32_t engine_id)
{
	if (!s_initialized) {
		return kGfxAccelErrVBLNotInitialized;
	}

	if (engine_id < 0 || engine_id >= kGfxFramePacingEngineCount) {
		engine_id = kGfxFramePacingEngineLegacy;
	}

	uint64_t period_abs = atomic_load_explicit(&s_cadence_abs,
	                                           memory_order_relaxed);
	if (period_abs == 0) {
		period_abs = vbl_source_usec_to_abs(GFX_FRAME_PACING_DEFAULT_USEC);
	}

	uint64_t now = mach_absolute_time();
	uint64_t tick = atomic_load_explicit(&s_tick_count, memory_order_relaxed);
	uint64_t last_tick_abs = atomic_load_explicit(&s_last_tick_abs,
	                                             memory_order_relaxed);
	uint64_t deadline = atomic_load_explicit(
		&s_engine_next_deadline_abs[engine_id], memory_order_relaxed);

	bool tick_is_fresh =
		last_tick_abs != 0 &&
		now <= last_tick_abs + period_abs * GFX_FRAME_PACING_STALE_TICKS;

	if (!tick_is_fresh) {
		uint64_t fallback_deadline = now + period_abs;
		if (deadline == 0 || deadline <= now ||
		    deadline > fallback_deadline + period_abs) {
			deadline = fallback_deadline;
		}
	} else if (deadline == 0 ||
	           deadline > last_tick_abs + 2u * period_abs) {
		/* First sync for this engine, or its deadline chain ran
		 * implausibly far ahead of the live tick grid (cadence change /
		 * long idle): re-anchor to the first boundary after the latest
		 * tick. NOTE: deliberately NOT re-anchored on every new tick —
		 * doing so discarded the engine's own already-crossed boundary
		 * and forced a full period wait per call, which made co-resident
		 * engines (DSp+GL) pace to CONSECUTIVE boundaries: two periods
		 * per guest frame, the CORE-10 half-rate symptom. */
		deadline = last_tick_abs + period_abs;
	}

	if (tick_is_fresh && deadline <= now) {
		/* A vblank boundary already passed since this engine's last
		 * sync (frame took >= one period, or another engine's wait on
		 * this shared thread carried us across the boundary). The
		 * boundary is consumed WITHOUT sleeping so every co-resident
		 * engine paces to the SAME vblank, and a slow frame under the
		 * DSP-06 throttle counts its render time toward the cap instead
		 * of stacking full waits on top of it. */
		while (deadline <= now) {
			deadline += period_abs;
		}
		atomic_store_explicit(&s_engine_next_deadline_abs[engine_id],
		                      deadline, memory_order_relaxed);
		atomic_store_explicit(&s_engine_seen_tick[engine_id], tick,
		                      memory_order_relaxed);
		return 0;  // kGfxAccelNoErr (boundary already satisfied)
	}

	while (deadline <= now) {
		deadline += period_abs;
	}

	kern_return_t wait_result = mach_wait_until(deadline);
	if (wait_result != KERN_SUCCESS) {
		return kGfxAccelErrVBLTimeout;
	}

	atomic_store_explicit(&s_engine_next_deadline_abs[engine_id],
	                      deadline + period_abs, memory_order_relaxed);
	atomic_store_explicit(&s_engine_seen_tick[engine_id], tick,
	                      memory_order_relaxed);
	return 0;  // kGfxAccelNoErr
}

extern "C"
int32_t vbl_source_sync_3d_pacing(void)
{
	return vbl_source_sync_3d_pacing_for_engine(kGfxFramePacingEngineLegacy);
}

extern "C"
void vbl_source_signal_3d_pacing(void)
{
	atomic_store_explicit(&s_last_tick_abs, mach_absolute_time(),
	                      memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Secondary-callback fan-out registration API
// ---------------------------------------------------------------------------

extern "C"
int32_t vbl_source_register_secondary_callback(VBLSourceCallbackFn cb,
                                                void *ctx)
{
	if (cb == NULL) return kGfxAccelErrVBLAlreadyRunning;  /* reject NULL cb */
	for (int i = 0; i < VBL_SECONDARY_CALLBACK_MAX; i++) {
		if (s_secondary_cb[i] == NULL) {
			s_secondary_cb[i]  = cb;
			s_secondary_ctx[i] = ctx;
			return 0;
		}
	}
	return kGfxAccelErrVBLAlreadyRunning;  /* table full */
}

extern "C"
void vbl_source_unregister_secondary_callback(VBLSourceCallbackFn cb)
{
	for (int i = 0; i < VBL_SECONDARY_CALLBACK_MAX; i++) {
		if (s_secondary_cb[i] == cb) {
			s_secondary_cb[i]  = NULL;
			s_secondary_ctx[i] = NULL;
			return;
		}
	}
}

