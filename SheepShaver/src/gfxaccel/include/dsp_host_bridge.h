/*
 *  dsp_host_bridge.h - DSp <-> iOS host C-callable bridge.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Pure-C header. No Metal / Objective-C / Swift types — consumable from
 *  .cpp / .mm / Swift bridging header alike. Isolates the DSp engine's
 *  need to toggle UIApplication.shared.isIdleTimerDisabled behind a
 *  single C-linkage entry point; the Swift-side observer
 *  (DSpIdleTimerService.swift) reads the flag via
 *  DSpHostBridge_GetActiveFullscreen on foreground re-apply.
 *
 *  The fullscreen-only idle-timer suppression deliberately diverges from
 *  the RAVE/NQD engine-blind compositor boundary.
 */

#ifndef DSP_HOST_BRIDGE_H
#define DSP_HOST_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/*
 *  Mark the DSp engine as holding an Active fullscreen context (true)
 *  or not (false). Called from DSpContext_SetStateHandler
 *  on Active/Paused/Inactive transitions — the handler computes
 *  "any DSp context Active AND fullscreen-mode" and calls this setter
 *  on the transition edge (fullscreen-only idle-timer
 *  suppression per DSp 1.7 spec p.88).
 *
 *  Writes a file-static flag in dsp_host_bridge.mm AND posts
 *  Notification.Name("DSpHostBridge.activeFullscreenChanged") so
 *  DSpIdleTimerService can toggle UIApplication.shared.isIdleTimerDisabled
 *  on the main thread (iOS requires UIApplication on main).
 *
 *  Threading: single-writer emul-thread (DSpContext_SetStateHandler
 *  always runs on emul thread) + single-reader
 *  main-thread (DSpIdleTimerService reads on foreground re-apply).
 *  The flag is stored as `_Atomic bool` with relaxed memory ordering —
 *  matches the single-word read-mostly cross-thread flag pattern
 *  (minimum primitive sufficient;
 *  no happens-before requirements beyond monotonic writes).
 */
void DSpHostBridge_SetActiveFullscreen(bool active);

/*
 *  Read the current active-fullscreen flag. Called by
 *  DSpIdleTimerService on foreground re-apply (UIApplication
 *  .willEnterForegroundNotification) to decide whether to re-assert
 *  isIdleTimerDisabled after the OS restored it on background entry.
 *
 *  Returns the value of the file-static atomic flag.
 */
bool DSpHostBridge_GetActiveFullscreen(void);

#ifdef __cplusplus
}
#endif

#endif /* DSP_HOST_BRIDGE_H */
