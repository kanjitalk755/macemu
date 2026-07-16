/*
 *  display_mode_controller.h - Authoritative owner of display-mode state
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  C++ callable interface for the Display-Mode Controller (DMC), the
 *  authoritative writer of display state. Returns
 *  int/void so this header can be included from plain .cpp files without
 *  pulling in ObjC or Metal types. Not PPC-callable - no NATIVE_* opcode
 *  dispatch; callers are native C++/Obj-C++ code (video_sdl2.cpp,
 *  video.cpp, metal_compositor.mm, rave_metal_renderer.mm, gl_engine.cpp).
 *
 *  The controller owns a 4-state finite-state machine
 *  (Quiescent / QuickDrawOwner / ThreeDOwner / Transitioning / Blanking)
 *  with 12 legal transitions T1..T12. Every illegal transition returns a named kDMCErr* code with
 *  no silent state mutation.
 */

#ifndef DISPLAY_MODE_CONTROLLER_H
#define DISPLAY_MODE_CONTROLLER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMC error codes - match kQA* / OSErr negative-int32 convention
 * (range -3001 .. -3099).
 *
 * These represent PROGRAMMER errors in native C++ code, not PPC-instruction
 * decode failures. They must NEVER be suppressed by the user-facing
 * ignoreIllegalInstructions preference (PROJECT.md security constraint).
 */
enum DMCError {
	kDMCNoErr                          = 0,
	kDMCErrNotInitialized              = -3001,  /* call before dmc_create() */
	kDMCErrAlreadyInitialized          = -3002,  /* dmc_create() called twice */
	kDMCErrTransitionInProgress        = -3003,  /* request_* called while in Transitioning */
	kDMCErrInvalidModeDesc             = -3004,  /* unknown depth, zero width, row_bytes invalid, etc. */
	kDMCErrInvalidOwner                = -3005,  /* set_active_owner with unknown enum value */
	kDMCErrOwnerMismatch               = -3006,  /* caller releases an owner different from the current one */
	kDMCErrSubscriberRejected          = -3007,  /* reserved (on_mode_enter returned non-zero) */
	kDMCErrConcurrentWrite             = -3008,  /* reserved (writer mutex contention) */
	kDMCErrIllegalTransition           = -3009,  /* catch-all for state-matrix miss */
	kDMCErrBlankingAlreadyActive       = -3010,  /* request_blanking from Blanking state */
	kDMCErrNotBlanking                 = -3011,  /* end_blanking from non-Blanking state */
	kDMCErrSnapshotStale               = -3012,  /* reserved (reader held a pointer across a transition) */
	kDMCErrReentrantRequest            = -3013,  /* reserved (re-entry guard) */
	kDMCErrSubscriberNotFound          = -3014,  /* dmc_unsubscribe: name does not match any registered subscriber */
	kDMCErrSubscriberAlreadyRegistered = -3015, /* dmc_subscribe: duplicate name */
	kDMCErrOutOfMemory                 = -3016, /* allocation failure (distinct from param-invalid codes) */
	kDMCDriverGammaDeferred            = 1      /* success-with-info: driver gamma table stored, but NOT applied
	                                             * to the displayed LUT because a DSp fade is in progress; the
	                                             * fade end-state delivers it. Caller must NOT push the table to
	                                             * the compositor. */
};

/*
 * Owner identifies which rendering subsystem currently drives the display.
 * Exactly one owner at a time. RAVE and GL cannot both own simultaneously
 * - this matches the single overlay_active flag in metal_compositor.mm
 * plus the empirical reality that classic Mac apps use one 3D library per
 * rendering context. The "owner" drives mode semantics (VBL pacing gate,
 * palette application, blanking), not composition layering.
 */
enum DMCOwner {
	kDMCOwnerQuickDraw   = 0,  /* default - 2D framebuffer, no 3D overlay */
	kDMCOwnerRAVE        = 1,  /* RAVE 3D context active */
	kDMCOwnerGL          = 2,  /* OpenGL 3D context active */
	kDMCOwnerDSp         = 3,  /* reserved for M2 (DrawSprocket 1.7) */
	kDMCOwnerBlanking    = 4,  /* solid-color blanking (fade / app-suspend) */
	kDMCOwnerQuiescent   = 5   /* no display */
};

/*
 * FSM states. The 4th state (Transitioning) is the observable
 * intermediate for the exit-then-enter event sequence; the 5th state
 * (Blanking) is the DSp-fade foundation. Readers on the VBL thread
 * that see snapshot.transitioning != 0 must skip the frame.
 */
enum DMCState {
	kDMCStateQuiescent       = 0,
	kDMCStateQuickDrawOwner  = 1,
	kDMCStateThreeDOwner     = 2,
	kDMCStateTransitioning   = 3,
	kDMCStateBlanking        = 4
};

/*
 * Mode descriptor - input shape for dmc_create() and
 * dmc_request_mode_switch(). Validated
 * before any state mutation: depth in {1,2,4,8,16,32}, width/height in
 * (0, 4096], row_bytes > 0, pitch >= row_bytes. Validation failures return
 * kDMCErrInvalidModeDesc and leave controller state unchanged.
 */
struct DMCModeDesc {
	uint32_t  width;            /* pixels, > 0, <= 4096 */
	uint32_t  height;           /* pixels, > 0, <= 4096 */
	uint32_t  depth;            /* VIDEO_DEPTH_* constant; one of {1,2,4,8,16,32} */
	uint32_t  row_bytes;        /* per-row stride in bytes; > 0 */
	uint32_t  pitch;            /* allocation stride (>= row_bytes) */
	uint32_t  vbl_usec;         /* microseconds per VBL frame; 0 means "compute from objc_getFrameRateSetting" */
	uint32_t  screen_base_mac;  /* Mac address of framebuffer (optional - 0 means "set later by compositor enter event") */
	void     *screen_base_host; /* host pointer of framebuffer (optional - NULL allowed) */
};

/*
 * Immutable snapshot of the published display state. Readers from any
 * thread obtain the current snapshot via dmc_current_snapshot() and must
 * treat it as read-only. A later revision will replace the pointer with an
 * std::atomic<const DMCModeSnapshot *>; this revision is single-threaded.
 */
struct DMCModeSnapshot {
	uint32_t       generation;              /* monotonic, bumped per commit; 0 reserved */
	int32_t        transitioning;           /* 0 = stable; non-zero = in Transitioning state */
	uint32_t       width;
	uint32_t       height;
	uint32_t       depth;
	uint32_t       row_bytes;
	uint32_t       pitch;
	uint32_t       palette_gen;             /* bumped on every CLUT change */
	uint32_t       gamma_gen;               /* bumped on every gamma change */
	uint8_t        gamma_lut[768];          /* Mac-side planar LUT: 256 R + 256 G + 256 B */
	uint32_t       fade_active;             /* 1 while a DSp gamma fade is in progress; gates the compositor's static 1.8->2.2 display-LUT composition so the LUT marches linearly (DSp-1.7). Published atomically WITH gamma_lut. */
	uint8_t        driver_gamma_lut[768];   /* last guest SetGamma table (identity until the driver publishes one).
	                                         * The "original intensity" DSp fades restore to: games install
	                                         * functional ramps (e.g. overbright doubling) that a fade-in must
	                                         * land on, not the identity ramp. Outside fades, gamma_lut ==
	                                         * driver_gamma_lut. */
	uint32_t       vbl_usec;
	uint32_t       active_owner;            /* DMCOwner value */
	uint8_t        blanking_rgba[4];        /* RGBA; only meaningful in Blanking state */
	uint32_t       screen_base_mac;
	void          *screen_base_host;
};

/*
 * Subscriber callback contract. Both callbacks are nullable. Exit fires
 * BEFORE the outgoing snapshot is retired; Enter fires AFTER the incoming
 * snapshot is published. Exits dispatch in registration order (FIFO);
 * enters dispatch in reverse registration order (LIFO). If enter rejects a
 * transition, rollback compensates with exit(rejected) then advisory
 * enter(restored).
 *
 * Return kDMCNoErr to accept the transition; non-zero return on enter
 * triggers a rollback. Exit returns are advisory only.
 */
typedef int32_t (*DMCExitFn)(const struct DMCModeSnapshot *outgoing, void *ctx);
typedef int32_t (*DMCEnterFn)(const struct DMCModeSnapshot *incoming, void *ctx);

struct DMCSubscriber {
	const char    *name;                    /* "compositor", "rave", "gl", "nqd" - for logs */
	DMCExitFn      on_mode_exit;            /* nullable */
	DMCEnterFn     on_mode_enter;           /* nullable */
	void          *ctx;
};

/* --- Public API --- */

/*
 * Initialize the display-mode controller with the first committed mode.
 *
 * Must be called exactly once, from the PPC emulator thread, before any
 * other dmc_* call. Transitions the controller from Quiescent to
 * QuickDrawOwner (T1) and publishes the first DMCModeSnapshot with
 * generation=1.
 *
 * Parameters:
 *   initial_mode - pointer to DMCModeDesc describing width, height, depth,
 *                  row_bytes, pitch, and the initial framebuffer address.
 *                  Validated before use.
 *
 * Returns kDMCNoErr on success; kDMCErrAlreadyInitialized if called twice;
 *         kDMCErrInvalidModeDesc on validation failure (state unchanged).
 */
int32_t dmc_create(const struct DMCModeDesc *initial_mode);

/*
 * Tear down all controller state. Transitions to Quiescent from any state
 * (T10/T11/T12). Frees every retained snapshot; clears subscriber list.
 *
 * Returns kDMCNoErr on success; kDMCErrNotInitialized if already quiescent.
 */
int32_t dmc_shutdown(void);

/*
 * Register a subscriber for mode-transition callbacks.
 *
 * The subscriber struct is COPIED into the controller's registration
 * table (the `name` C-string pointer is borrowed, not duplicated - the
 * caller must keep the name buffer alive for the lifetime of the
 * registration). Subsequent transitions dispatch `on_mode_exit` in
 * REGISTRATION ORDER (FIFO) BEFORE the snapshot swap and `on_mode_enter`
 * in REVERSE REGISTRATION ORDER (LIFO) AFTER the swap.
 *
 * Mid-transition catchup: if the controller already has a published
 * snapshot when the subscriber registers (i.e. s_state != Quiescent),
 * the new subscriber receives a synthetic `on_mode_enter(current)` call
 * so it is observably in sync with the current mode. The catchup return
 * value is ignored (informational only - a mid-init subscriber cannot
 * veto an already-committed transition).
 *
 * Returns kDMCNoErr on success;
 *         kDMCErrNotInitialized if called before dmc_create();
 *         kDMCErrSubscriberRejected on NULL sub / NULL sub->name (bad input);
 *         kDMCErrSubscriberAlreadyRegistered if a subscriber with the same
 *         name is already present.
 */
int32_t dmc_subscribe(const struct DMCSubscriber *sub);

/*
 * Remove a subscriber matching the given name.
 *
 * Returns kDMCNoErr on success;
 *         kDMCErrNotInitialized if called before dmc_create();
 *         kDMCErrSubscriberNotFound if no subscriber matches.
 */
int32_t dmc_unsubscribe(const char *name);

/*
 * Read the currently published snapshot. Returns NULL in Quiescent.
 *
 * THIS REVISION: plain pointer return. A later revision wraps in an atomic
 * load with acquire ordering and returns a reference-counted handle.
 *
 * Caller must not free or mutate the returned pointer.
 */
const struct DMCModeSnapshot *dmc_current_snapshot(void);

/*
 * Request a mode switch to new_mode. Transitions T2 (from QuickDrawOwner)
 * or T6 (from ThreeDOwner). On success, the active_owner is preserved
 * across the switch: T6 from ThreeDOwner returns to ThreeDOwner.
 *
 * THIS REVISION: transitions occur synchronously - the commit fires
 * immediately (no subscriber dispatch yet). A later revision adds the
 * exit-then-enter broadcast and the writer mutex.
 *
 * Returns kDMCNoErr on success;
 *         kDMCErrNotInitialized if called before dmc_create();
 *         kDMCErrTransitionInProgress if already mid-switch;
 *         kDMCErrIllegalTransition from Blanking (end_blanking first);
 *         kDMCErrInvalidModeDesc on validation failure.
 */
int32_t dmc_request_mode_switch(const struct DMCModeDesc *new_mode);

/*
 * Set the active owner. Transitions T4 (QuickDrawOwner -> ThreeDOwner)
 * when owner is RAVE / GL / DSp; T5 (ThreeDOwner -> QuickDrawOwner)
 * when owner is QuickDraw.
 *
 * The enum value is passed as uint32_t for C-linkage width safety across
 * the extern "C" boundary.
 *
 * Returns kDMCNoErr on success;
 *         kDMCErrNotInitialized if called before dmc_create();
 *         kDMCErrInvalidOwner if owner is out of enum range;
 *         kDMCErrIllegalTransition if called from Blanking (end_blanking first);
 *         kDMCErrTransitionInProgress if called mid-switch.
 */
int32_t dmc_set_active_owner(uint32_t owner);

/*
 * Enter Blanking mode with the given RGBA solid color. Transition T8.
 *
 * Foundation for M2 DSpContext_FadeGamma and DSpSetBlankingColor + app
 * suspend. In Blanking, no engine is rendering; the compositor draws
 * the blanking color every frame.
 *
 * Returns kDMCNoErr on success;
 *         kDMCErrNotInitialized if called before dmc_create();
 *         kDMCErrBlankingAlreadyActive if already in Blanking state;
 *         kDMCErrTransitionInProgress if called mid-switch.
 */
int32_t dmc_request_blanking(const uint8_t rgba[4]);

/*
 * Exit Blanking mode. Transition T9 (-> QuickDrawOwner).
 *
 * Returns kDMCNoErr on success;
 *         kDMCErrNotInitialized if called before dmc_create();
 *         kDMCErrNotBlanking if called from non-Blanking state.
 */
int32_t dmc_end_blanking(void);

/*
 * Bump the palette_gen counter on the current snapshot. Called from
 * video_sdl2.cpp set_palette() and video.cpp set_palette() seams.
 * Compositor reads the new CLUT next frame.
 *
 * THIS REVISION: mutates the published snapshot in place. A later revision
 * replaces with a generation-bumped fresh snapshot to maintain the
 * immutability invariant.
 *
 * Returns kDMCNoErr on success;
 *         kDMCErrNotInitialized if called before dmc_create().
 */
int32_t dmc_record_palette_change(void);

/*
 * Bump the gamma_gen counter on the current snapshot. Called from
 * video.cpp set_gamma() seam.
 *
 * Returns kDMCNoErr on success;
 *         kDMCErrNotInitialized if called before dmc_create().
 */
int32_t dmc_record_gamma_change(void);

/*
 * Record a gamma ramp change with LUT data. The lut parameter
 * points to 768 bytes: 256 R entries, then 256 G entries, then 256 B
 * entries (planar layout). If lut is NULL, the existing LUT data is
 * preserved and only gamma_gen is bumped (same as dmc_record_gamma_change).
 *
 * Returns kDMCNoErr on success;
 *         kDMCErrNotInitialized if called before dmc_create();
 *         kDMCErrOutOfMemory if snapshot allocation fails.
 */
int32_t dmc_record_gamma_change_with_lut(const uint8_t *lut);

/*
 * Companion to
 * dmc_record_gamma_change_with_lut that ALSO publishes a fade_active flag
 * in the SAME snapshot bump (avoids a transient frame where the
 * LUT is mid-fade but fade_active is stale). The DSp fade-driver
 * (DSpVBLGammaFadeCallback) calls this with fade_active=1 on each mid-fade
 * push and fade_active=0 on the final-frame push; a plain SetGamma stays on
 * the legacy 1-arg form (fade_active=0). The flag rides the EXISTING DMC
 * single-writer publish (s_write_mutex + atomic-release store) — ZERO new
 * concurrency primitives. lut semantics are identical to the 1-arg
 * form (NULL preserves the existing LUT; non-NULL overwrites 768 bytes).
 *
 * Returns kDMCNoErr on success;
 *         kDMCErrNotInitialized if called before dmc_create();
 *         kDMCErrOutOfMemory if snapshot allocation fails.
 */
int32_t dmc_record_gamma_change_with_lut_fade(const uint8_t *lut, int fade_active);

/*
 * Record a DRIVER (guest SetGamma) table. Always stores the 768-byte planar
 * table into driver_gamma_lut — the "original intensity" reference the DSp
 * fade paths blend toward — then applies it to the displayed gamma_lut only
 * when no fade is in progress. When fade_active is set on the current
 * snapshot the displayed LUT is left alone (the screen is faded/fading; an
 * immediate apply would visibly pop) and the pending fade's end-state
 * delivers the table instead.
 *
 * Returns kDMCNoErr if the table was applied to the displayed LUT
 *         (caller should push it to the compositor as usual);
 *         kDMCDriverGammaDeferred if stored but NOT applied (fade in
 *         progress — caller must NOT push to the compositor);
 *         kDMCErrNotInitialized if called before dmc_create();
 *         kDMCErrInvalidModeDesc if lut is NULL;
 *         kDMCErrOutOfMemory if snapshot allocation fails.
 */
int32_t dmc_record_driver_gamma_change(const uint8_t *lut);

/*
 * Assign the snapshot's blanking color WITHOUT
 * entering the Blanking FSM state. DSpSetBlankingColor (sub-op 760) only sets
 * the color the library uses the next time the screen is blanked — it does NOT
 * blank now (DSp 1.7 PDF p.30). No-state-transition twin of
 * dmc_record_gamma_change_with_lut: clone-mutate-publish the blanking_rgba
 * field under the EXISTING s_write_mutex (no NEW concurrency primitive); does
 * NOT call dmc_request_blanking (which would transition to Blanking — wrong
 * for SetBlankingColor). rgba is 4 bytes (R, G, B, A).
 *
 * Returns kDMCNoErr on success;
 *         kDMCErrNotInitialized if called before dmc_create();
 *         kDMCErrInvalidModeDesc if rgba is NULL;
 *         kDMCErrOutOfMemory if snapshot allocation fails.
 */
int32_t dmc_set_blanking_color(const uint8_t rgba[4]);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_MODE_CONTROLLER_H */
