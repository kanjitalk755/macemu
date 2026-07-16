/*
 *  dsp_host_bridge.mm - DSp <-> iOS host bridge implementation.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  The setter performs the full NotificationCenter wiring.
 *  DSpIdleTimerService.swift observes the posted notification on the
 *  main queue and toggles UIApplication.shared.isIdleTimerDisabled.
 *
 *  Threading:
 *    - Setter called from DSpContext_SetStateHandler on the emul thread.
 *    - NotificationCenter.postNotification is thread-safe by Apple's
 *      documented contract (internal lock; safe to call from any thread).
 *    - Observer closure runs on main queue (queue: .main in addObserver
 *      call) — UIApplication.shared.isIdleTimerDisabled write happens on
 *      main thread per UIKit's main-thread contract.
 *    - Getter: single-word _Atomic bool read (memory_order_relaxed);
 *      callable from any thread.
 *
 *  Threading rationale for `_Atomic bool`:
 *    - Single writer: DSpContext_SetStateHandler on emul thread
 *      (NATIVE_DSP_DISPATCH serialized).
 *    - Readers: DSpIdleTimerService on main thread via the
 *      willEnterForegroundNotification observer; the emul-thread
 *      conditional-set read in DSpContext_SetStateHandler (both via
 *      DSpHostBridge_GetActiveFullscreen).
 *    - Cross-thread single-word bool — `_Atomic bool` with
 *      memory_order_relaxed is the minimum sanctioned primitive per the
 *      read-mostly precedent. The threading-grep CI gate
 *      (MTLFence|MTLSharedEvent|std::mutex|@synchronized) does not
 *      match _Atomic by design.
 *
 *  The `_Atomic bool s_dsp_active_fullscreen` flag is kept
 *  because DSpIdleTimerService re-reads it on willEnterForegroundNotification
 *  — the notification post path alone would miss the case where DSp
 *  transitioned to Active-fullscreen WHILE the app was backgrounded (no
 *  observer firing during that window). The flag provides the source of
 *  truth for foreground re-apply decisions.
 */

#import <Foundation/Foundation.h>

#import "dsp_host_bridge.h"

#include <stdatomic.h>

/* Module-scope storage — single writer emul-thread; readers: main-thread
 * Swift observer on foreground re-apply, plus the emul-thread
 * conditional-set read via the getter. Initial state = false (no DSp
 * context is Active at DSp boot time; DSpContext_Reserve's default
 * state is Inactive). */
static _Atomic bool s_dsp_active_fullscreen = false;

extern "C" void DSpHostBridge_SetActiveFullscreen(bool active)
{
	/* Write the flag first so readers (including the observer
	 * closure that the notification post will trigger) see the new value
	 * atomically. _Atomic bool with memory_order_relaxed is the minimum
	 * sanctioned primitive — the Swift observer's queue: .main hop
	 * provides the happens-before for the subsequent UIApplication write
	 * (main-runloop iteration boundary is a synchronization point). */
	atomic_store_explicit(&s_dsp_active_fullscreen, active,
	                      memory_order_relaxed);

	/* Post notification to DSpIdleTimerService. Uses the
	 * same name string the Swift observer registered for:
	 * Notification.Name("DSpHostBridge.activeFullscreenChanged"). The
	 * Swift observer's queue: .main hops delivery to main thread before
	 * writing UIApplication.shared.isIdleTimerDisabled.
	 *
	 * @autoreleasepool guards against any autoreleased objects the post
	 * might create internally (defensive — the NATIVE_DSP_DISPATCH emul
	 * thread does not have an enclosing autorelease pool). Negligible
	 * cost on a transition-edge-only call path. */
	@autoreleasepool {
		[[NSNotificationCenter defaultCenter]
		    postNotificationName:@"DSpHostBridge.activeFullscreenChanged"
		                  object:nil];
	}
}

extern "C" bool DSpHostBridge_GetActiveFullscreen(void)
{
	return atomic_load_explicit(&s_dsp_active_fullscreen,
	                            memory_order_relaxed);
}
