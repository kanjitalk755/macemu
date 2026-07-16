/*
 *  gfxaccel_threading_policy.h - iOS gfxaccel threading policy.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef GFXACCEL_THREADING_POLICY_H
#define GFXACCEL_THREADING_POLICY_H

/*
 * Current iOS ground truth:
 *   SDL_main runs on the UIKit main thread and main_unix.cpp runs the PPC
 *   emulator inline from that call path. Therefore the UIKit main thread is
 *   the emul thread for PocketShaver's iOS build.
 *
 * Consequence:
 *   Historical comments that say "main thread vs emul thread" usually do
 *   not describe parallel execution today. They describe same-thread
 *   re-entry: the emulator pumps the UIKit runloop and guest callbacks can
 *   run code that re-enters gfxaccel before the outer handler has returned.
 *
 * Important pump/re-entry points:
 *   - HandleInterrupt -> SDL_PumpEvents
 *   - CallMacOS* / call_macos* guest callback trampolines
 *   - CADisplayLink VBL primary and secondary callback fan-out
 *   - UIKit lifecycle notifications delivered while the runloop is pumped
 *
 * Rule for new code:
 *   Do not add locks just to protect state that is single-threaded only
 *   because main==emul today. Instead, make re-entry explicit: defer work to
 *   a known drain point, guard nested callback fan-out, and revalidate
 *   handles after any guest callback or runloop pump.
 *
 * If emulation ever moves off the UIKit main thread, every translation unit
 * that includes this header is an audit site. Plain statics and pointer
 * tables guarded only by this policy must be marshaled or protected by real
 * synchronization before the split ships.
 */

#endif /* GFXACCEL_THREADING_POLICY_H */
