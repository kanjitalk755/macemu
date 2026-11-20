/*
 *  display_mode_controller.cpp - Authoritative owner of display-mode state
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Implements the authoritative writer of display state. The 4-state
 *  finite-state machine
 *  (Quiescent / QuickDrawOwner / ThreeDOwner / Transitioning / Blanking)
 *  plus owner enum captures every legal display-mode transition from
 *  native code and rejects illegal transitions with a named kDMCErr*
 *  code.
 *
 *  Subscriber dispatch:
 *    - dmc_subscribe stores a copy of the DMCSubscriber in a real
 *      std::vector<DMCSubscriber>, with name-keyed duplicate rejection.
 *    - Real display-mode replacement fires on_mode_exit in REGISTRATION
 *      ORDER (FIFO) while the old platform surface is valid, then
 *      on_mode_enter in REVERSE REGISTRATION ORDER (LIFO) after the new
 *      surface is installed and published. Producer, palette, gamma and
 *      blanking changes only publish a fresh immutable snapshot; they do not
 *      invalidate mode resources. If any mode on_mode_enter
 *      returns non-zero, the controller fires exit for the rejected
 *      snapshot, republishes the outgoing snapshot, fires advisory enter
 *      for the restored snapshot, and returns kDMCErrSubscriberRejected.
 *    - Late subscribers receive a synthetic on_mode_enter with the
 *      current snapshot so they start observably in-sync.
 *
 *  Concurrency primitives:
 *    - `s_write_mutex` (dmc_mutex_t: an SRWLOCK on Win32, a
 *      pthread_mutex_t elsewhere) serializes every public write API via the
 *      DMCLock scope guard at function entry; readers stay lock-free.
 *    - `s_current` is a `const DMCModeSnapshot *` slot published with
 *      atomic.h's release store and read with its acquire load; writers
 *      publish via memory_order_release, readers load via
 *      memory_order_acquire. The snapshot structure is IMMUTABLE once
 *      published (mutations allocate a fresh snapshot then swap, rather
 *      than mutating in place).
 *    - 2-generation retirement ring buffer (`s_retired[2]`): when
 *      generation N is published, the slot for N-2 is freed. This
 *      guarantees a reader that held the snapshot pointer at N-1 keeps
 *      a valid pointer for at least one VBL grace period (matches
 *      maximumDrawableCount=3 triple-buffering rhythm).
 *    - `DMCReentryScope` + `s_in_dmc_call` (thread-local storage) guards
 *      against subscriber callbacks re-entering the controller; the inner
 *      call returns kDMCErrReentrantRequest BEFORE touching the mutex
 *      (avoiding deadlock under non-recursive mutex semantics).
 *    - `s_emul_thread` captured at first dmc_create; subsequent write
 *      calls from any other thread trigger an assertion.
 *
 *  The controller does NOT touch emulated Mac memory - macos_util.h and
 *  thunks.h are intentionally omitted from the includes vs.
 *  rave_engine.cpp.
 */

#include "sysdeps.h"
#include "display_mode_controller.h"
#include "gfx_color_policy.h"

#include <cstring>
#include <cstdlib>
#include <vector>

// Concurrency primitives
#include "atomic.h"
#include <cassert>
#if defined(_WIN32)
#include <windows.h>
typedef uintptr_t dmc_thread_id_t;
static inline dmc_thread_id_t dmc_thread_self(void) { return (dmc_thread_id_t)GetCurrentThreadId(); }
// SRWLOCK and pthread_mutex_t both have a static initialiser, so the writer
// mutex needs no construction pass and no init-order guard.
typedef SRWLOCK dmc_mutex_t;
#define DMC_MUTEX_INIT      SRWLOCK_INIT
#define DMC_MUTEX_LOCK(m)   AcquireSRWLockExclusive(&(m))
#define DMC_MUTEX_UNLOCK(m) ReleaseSRWLockExclusive(&(m))
#define DMC_THREAD_LOCAL    __declspec(thread)
#else
#include <pthread.h>
typedef pthread_t dmc_thread_id_t;
static inline dmc_thread_id_t dmc_thread_self(void) { return pthread_self(); }
typedef pthread_mutex_t dmc_mutex_t;
#define DMC_MUTEX_INIT      PTHREAD_MUTEX_INITIALIZER
#define DMC_MUTEX_LOCK(m)   pthread_mutex_lock(&(m))
#define DMC_MUTEX_UNLOCK(m) pthread_mutex_unlock(&(m))
#define DMC_THREAD_LOCAL    __thread
#endif

#define DEBUG 0
#include "debug.h"

// Internal logging macros - promote to DMC_ERR for the rollback path
// so subscriber-rejected transitions are visible even in release builds.
#include "gfx_log.h"
#define DMC_LOG(fmt, ...) D(bug("[DMC] " fmt "\n", ##__VA_ARGS__))
#define DMC_ERR(...) GFX_DEBUG_EMIT("[DMC ERROR] ", __VA_ARGS__)

// ---------------------------------------------------------------------------
// Module-local state (writer-mutex serialized; atomic snapshot
// publication; 2-generation retirement ring; re-entry guard; thread-identity
// assertion).
//
// s_current is ATOMIC (lock-free read path for the VBL-tick thread). All
// OTHER state (s_state, s_subscribers, s_next_generation, s_dmc_initialized,
// s_retired[]) is protected by s_write_mutex. Writers always hold the mutex
// when mutating any of the non-atomic state AND when publishing to s_current;
// readers ONLY load s_current and never touch the rest.
// ---------------------------------------------------------------------------
static dmc_mutex_t                            s_write_mutex = DMC_MUTEX_INIT;
static const DMCModeSnapshot * volatile       s_current = NULL;

// Scoped writer lock; the sole replacement for std::lock_guard<std::mutex>.
class DMCLock {
	dmc_mutex_t &m;
	DMCLock(const DMCLock &);
	DMCLock &operator=(const DMCLock &);
public:
	DMCLock(dmc_mutex_t &mutex) : m(mutex) { DMC_MUTEX_LOCK(m); }
	~DMCLock() { DMC_MUTEX_UNLOCK(m); }
};
static DMCState                               s_state = kDMCStateQuiescent;
static uint32_t                               s_next_generation = 1;
static bool                                   s_dmc_initialized = false;
/* A platform mode switch is a two-phase transaction. prepare fires resource
 * detach while the outgoing GL context is still alive; the video driver then
 * replaces its surface/context; request_mode_switch commits the bound surface
 * and reattaches resources. The current snapshot remains the outgoing stable
 * snapshot between the two calls so lock-free readers never see half-bound
 * geometry. */
static bool                                   s_mode_switch_prepared = false;
static DMCState                               s_mode_switch_source_state =
												kDMCStateQuiescent;
static DMCModeDesc                            s_mode_switch_requested = {};

// Real subscriber registration table (dynamic vector). Ordered-vector
// semantics: index 0 is first-registered, N-1 is last-registered. Exit
// dispatch walks 0..N-1 (FIFO); enter dispatch walks N-1..0 (LIFO).
// Mutated exclusively under s_write_mutex.
static std::vector<DMCSubscriber>             s_subscribers;

// 2-generation snapshot retirement ring buffer (matches
// maximumDrawableCount=3 triple-buffering rhythm). Indexed by
// (generation - 1) % 2. When a new snapshot is published at generation N,
// the slot that formerly held generation N-2 is now safe to free: any
// reader that held that pointer must have done so before N-1 was
// published, which was itself at least one release-store ago.
//
// Mutated exclusively under s_write_mutex.
static const DMCModeSnapshot *                s_retired[2] = { NULL, NULL };

// Re-entry guard (subscriber callback re-enters a dmc_*
// write API). Thread-local so a cross-thread writer doesn't see the flag
// set by a different thread's outer call (defense-in-depth; all writers are
// on the emul thread today).
static DMC_THREAD_LOCAL bool                  s_in_dmc_call = false;


// Thread-identity baseline (assertion gate). Captured on the first
// dmc_create call; subsequent writes from any other thread trigger
// assert(0).
static dmc_thread_id_t                        s_emul_thread = 0;

// ---------------------------------------------------------------------------
// Re-entry guard RAII helper.
//
// Instantiate at the TOP of every public write API, BEFORE acquiring
// s_write_mutex. If the calling thread is already inside a dmc_* call
// (e.g. a subscriber callback), `installed` is false and the caller
// MUST return kDMCErrReentrantRequest WITHOUT acquiring the mutex.
// Otherwise the flag is set for the duration of this scope and cleared
// on destruction.
// ---------------------------------------------------------------------------
struct DMCReentryScope {
	bool installed;
	DMCReentryScope() {
		if (s_in_dmc_call) {
			installed = false;
		} else {
			s_in_dmc_call = true;
			installed = true;
		}
	}
	~DMCReentryScope() {
		if (installed) {
			s_in_dmc_call = false;
		}
	}
};

// ---------------------------------------------------------------------------
// Thread-identity assertion macro. Asserts that dmc_* write APIs run on the
// emul thread that first called dmc_create.
// ---------------------------------------------------------------------------
#define DMC_ASSERT_EMUL_THREAD() \
	do { \
		if (s_emul_thread != 0 && dmc_thread_self() != s_emul_thread) { \
			DMC_ERR("dmc_* called from non-emul thread (got %p, expected %p)", \
					(void *)(uintptr_t)dmc_thread_self(), (void *)(uintptr_t)s_emul_thread); \
			assert(0 && "DMC write from wrong thread"); \
		} \
	} while (0)

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Validate a caller-supplied DMCModeDesc.
// Returns kDMCNoErr on valid input, kDMCErrInvalidModeDesc on any failure.
static int32_t dmc_validate_mode_desc(const DMCModeDesc *m) {
	if (m == NULL) {
		return kDMCErrInvalidModeDesc;
	}
	if (m->width == 0 || m->width > 4096) {
		return kDMCErrInvalidModeDesc;
	}
	if (m->height == 0 || m->height > 4096) {
		return kDMCErrInvalidModeDesc;
	}
	if (m->depth != 1 && m->depth != 2 && m->depth != 4 &&
		m->depth != 8 && m->depth != 16 && m->depth != 32) {
		return kDMCErrInvalidModeDesc;
	}
	if (m->row_bytes == 0) {
		return kDMCErrInvalidModeDesc;
	}
	if (m->pitch < m->row_bytes) {
		return kDMCErrInvalidModeDesc;
	}
	return kDMCNoErr;
}

static bool dmc_same_mode_geometry(const DMCModeDesc *a,
								   const DMCModeDesc *b)
{
	return a != NULL && b != NULL &&
		a->width == b->width &&
		a->height == b->height &&
		a->depth == b->depth &&
		a->row_bytes == b->row_bytes &&
		a->pitch == b->pitch;
}

// Internal allocation wrappers that funnel the 4 direct calloc/malloc sites
// in this file through a single pair of helpers. Callers MUST hold
// s_write_mutex when invoking these (every call site is already inside the
// writer mutex by construction).
static DMCModeSnapshot *dmc_alloc_zeroed_snapshot(void) {
	return (DMCModeSnapshot *)calloc(1, sizeof(DMCModeSnapshot));
}

static DMCModeSnapshot *dmc_alloc_raw_snapshot(void) {
	return (DMCModeSnapshot *)malloc(sizeof(DMCModeSnapshot));
}

// Initialize gamma_lut[768] to the Mac-side identity ramp (input == output).
// The compositor composes the display-space 1.8->2.2 policy when it uploads
// this snapshot into its GPU-visible LUT.
static void dmc_init_identity_gamma(DMCModeSnapshot *snap) {
	GfxColorFillIdentityGammaLUT(snap->gamma_lut);
	GfxColorFillIdentityGammaLUT(snap->driver_gamma_lut);
}

// Heap-allocate a fresh snapshot populated from a DMCModeDesc. Fields the
// descriptor does not carry (palette_gen, gamma_gen) default to 0 unless the
// caller later copies them from the outgoing snapshot.
//
// NOTE: transitioning is ALWAYS 0 in published snapshots under the
// immutability model. The transitioning=1 in-place sentinel has
// been replaced by s_state == kDMCStateTransitioning; readers simply tolerate
// seeing the old snapshot for a sub-microsecond transition window during
// which s_current still points at `outgoing`.
static DMCModeSnapshot *dmc_alloc_snapshot_from_desc(const DMCModeDesc *m,
													 uint32_t generation,
													 uint32_t active_owner,
													 const uint8_t blanking_rgba[4]) {
	DMCModeSnapshot *s = dmc_alloc_zeroed_snapshot();
	if (s == NULL) {
		return NULL;
	}
	s->generation       = generation;
	s->transitioning    = 0;
	s->width            = m->width;
	s->height           = m->height;
	s->depth            = m->depth;
	s->row_bytes        = m->row_bytes;
	s->pitch            = m->pitch;
	s->palette_gen      = 0;
	s->gamma_gen        = 0;
	s->fade_active      = 0;  // no fade in progress on a fresh mode
	s->vbl_usec         = m->vbl_usec;
	s->active_owner     = active_owner;
	if (blanking_rgba != NULL) {
		s->blanking_rgba[0] = blanking_rgba[0];
		s->blanking_rgba[1] = blanking_rgba[1];
		s->blanking_rgba[2] = blanking_rgba[2];
		s->blanking_rgba[3] = blanking_rgba[3];
	} else {
		s->blanking_rgba[0] = 0;
		s->blanking_rgba[1] = 0;
		s->blanking_rgba[2] = 0;
		s->blanking_rgba[3] = 0xFF;
	}
	s->screen_base_mac  = m->screen_base_mac;
	s->screen_base_host = m->screen_base_host;
	// identity gamma ramp for initial snapshot (no outgoing to copy from).
	dmc_init_identity_gamma(s);
	return s;
}

// Clone an existing snapshot, overriding active_owner (and optionally
// blanking_rgba). Used by dmc_set_active_owner / dmc_request_blanking /
// dmc_end_blanking to produce a new snapshot without re-validating a
// DMCModeDesc (the existing fields were validated on the previous commit).
static DMCModeSnapshot *dmc_clone_snapshot_with_owner(const DMCModeSnapshot *src,
													  uint32_t generation,
													  uint32_t active_owner,
													  const uint8_t blanking_rgba[4]) {
	DMCModeSnapshot *s = dmc_alloc_zeroed_snapshot();
	if (s == NULL) {
		return NULL;
	}
	// Copy existing fields verbatim.
	*s = *src;
	// Override mutable-per-commit fields.
	s->generation       = generation;
	s->transitioning    = 0;
	s->active_owner     = active_owner;
	if (blanking_rgba != NULL) {
		s->blanking_rgba[0] = blanking_rgba[0];
		s->blanking_rgba[1] = blanking_rgba[1];
		s->blanking_rgba[2] = blanking_rgba[2];
		s->blanking_rgba[3] = blanking_rgba[3];
	}
	return s;
}

// Translate an owner enum to the corresponding FSM target state.
// Returns kDMCStateQuickDrawOwner for QD, kDMCStateThreeDOwner for 3D engines.
// Callers that pass Blanking / Quiescent owners must handle those cases
// separately (they are not legal targets of dmc_set_active_owner).
static DMCState dmc_target_state_for_owner(uint32_t owner) {
	if (owner == kDMCOwnerRAVE || owner == kDMCOwnerGL ||
		owner == kDMCOwnerDSp || owner == kDMCOwnerGlide) {
		return kDMCStateThreeDOwner;
	}
	return kDMCStateQuickDrawOwner;
}

// Retire `old_snap` into the 2-generation ring buffer. `this_gen` is the
// generation of the snapshot THAT IS NOW PUBLISHED (i.e. the replacement).
// Call site MUST hold s_write_mutex.
//
// Slot assignment: the ring is indexed by (this_gen - 1) % 2. Each new
// publish at generation N overwrites the slot for generation N-2 (which
// was itself retired when N-1 was published). This preserves reader
// pointer validity for one full VBL cycle.
static void dmc_retire_snapshot(const DMCModeSnapshot *old_snap, uint32_t this_gen) {
	if (old_snap == NULL) {
		return;
	}
	int slot = (int)((this_gen - 1) % 2);
	if (s_retired[slot] != NULL) {
		free((void *)s_retired[slot]);
		s_retired[slot] = NULL;
	}
	s_retired[slot] = old_snap;
}

// ---------------------------------------------------------------------------
// Subscriber event dispatch
//
// dmc_internal_fire_exit_events iterates subscribers in REGISTRATION ORDER
// (index 0 -> N-1). Each subscriber's on_mode_exit return is advisory
// ("exit is advisory, not vetoable"); a non-zero return
// is logged and dispatch continues to the next subscriber.
//
// dmc_internal_fire_enter_events iterates subscribers in REVERSE REGISTRATION
// ORDER (index N-1 -> 0). A non-zero on_mode_enter return aborts further
// dispatch and propagates kDMCErrSubscriberRejected up to the caller, which
// compensates and rolls the transition back.
//
// Both helpers are called with s_write_mutex held (by the outer public
// API). Subscriber callbacks MUST NOT re-enter a dmc_* write API; if they
// do, the DMCReentryScope guard in the re-entered function returns
// kDMCErrReentrantRequest without acquiring the mutex.
// ---------------------------------------------------------------------------

static void dmc_internal_fire_exit_events(const DMCModeSnapshot *outgoing) {
	for (size_t i = 0; i < s_subscribers.size(); ++i) {
		if (s_subscribers[i].on_mode_exit != NULL) {
			int32_t r = s_subscribers[i].on_mode_exit(outgoing, s_subscribers[i].ctx);
			if (r != kDMCNoErr) {
				DMC_LOG("subscriber %s on_mode_exit returned %d (advisory; not vetoable)",
						s_subscribers[i].name != NULL ? s_subscribers[i].name : "?", (int)r);
			}
		}
	}
}

static int32_t dmc_internal_fire_enter_events(const DMCModeSnapshot *incoming) {
	// Use a raw reverse index
	// (size - 1 - idx) instead of a filtered dispatch_pos counter so
	// enter's dispatch_index semantics mirror exit's raw forward index.
	// Before: one-sided-callback subscribers produced ambiguous enter
	// indices (filtered counter skipped NULL on_mode_enter). After: every
	// dispatch_index reflects the subscriber's position in the registration
	// table regardless of callback presence. Exit's `(uint32_t)i` stays
	// untouched - both paths now converge on raw subscriber index.
	for (size_t i = s_subscribers.size(); i > 0; --i) {
		size_t idx = i - 1;  // walk in reverse without size_t underflow
		if (s_subscribers[idx].on_mode_enter != NULL) {
			int32_t r = s_subscribers[idx].on_mode_enter(incoming, s_subscribers[idx].ctx);
			if (r != kDMCNoErr) {
				DMC_ERR("subscriber %s on_mode_enter returned %d - initiating rollback",
						s_subscribers[idx].name != NULL ? s_subscribers[idx].name : "?", (int)r);
				return kDMCErrSubscriberRejected;
			}
		}
	}
	return kDMCNoErr;
}

static void dmc_internal_fire_enter_events_advisory(
	const DMCModeSnapshot *incoming,
	const char *reason)
{
	if (incoming == NULL) return;
	for (size_t i = s_subscribers.size(); i > 0; --i) {
		size_t idx = i - 1;
		if (s_subscribers[idx].on_mode_enter != NULL) {
			int32_t r = s_subscribers[idx].on_mode_enter(incoming,
														 s_subscribers[idx].ctx);
			if (r != kDMCNoErr) {
				DMC_ERR("subscriber %s on_mode_enter returned %d during "
						"%s - rollback compensation continues",
						s_subscribers[idx].name != NULL ? s_subscribers[idx].name : "?",
						(int)r, reason != NULL ? reason : "rollback compensation");
			}
		}
	}
}

static void dmc_internal_compensate_rejected_transition(
	const DMCModeSnapshot *rejected,
	const DMCModeSnapshot *restored,
	DMCState restored_state,
	const char *reason)
{
	if (rejected != NULL) {
		dmc_internal_fire_exit_events(rejected);
	}
	atomic_store_explicit(&s_current, restored, memory_order_release);
	s_state = restored_state;
	dmc_internal_fire_enter_events_advisory(restored, reason);
}

// ---------------------------------------------------------------------------
// Public API
//
// Every public write entry point follows the same preamble:
//   1. DMCReentryScope reentry;
//      if (!reentry.installed) return kDMCErrReentrantRequest;
//   2. DMCLock guard(s_write_mutex);
//   3. DMC_ASSERT_EMUL_THREAD();
//   4. FSM / validation checks.
//   5. Allocate incoming snapshot, fire exits, publish (atomic release),
//      fire enters, retire outgoing (or compensate + rollback on enter veto).
//
// dmc_current_snapshot is the ONLY read path; it is lock-free and callable
// from any thread (acquire-load on s_current).
// ---------------------------------------------------------------------------

int32_t dmc_create(const struct DMCModeDesc *initial_mode) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	// (No DMC_ASSERT_EMUL_THREAD here: production captures s_emul_thread
	// on the FIRST dmc_create call. Subsequent writes assert.)

	if (s_dmc_initialized) {
		return kDMCErrAlreadyInitialized;
	}
	int32_t verr = dmc_validate_mode_desc(initial_mode);
	if (verr != kDMCNoErr) {
		return verr;
	}

	if (s_emul_thread == 0) {
		s_emul_thread = dmc_thread_self();
	}

	const uint8_t default_blanking[4] = { 0x00, 0x00, 0x00, 0xFF };
	DMCModeSnapshot *snap = dmc_alloc_snapshot_from_desc(initial_mode,
														 s_next_generation,
														 (uint32_t)kDMCOwnerQuickDraw,
														 default_blanking);
	if (snap == NULL) {
		return kDMCErrOutOfMemory;  // uniform OOM return
	}
	s_next_generation++;

	// T1 has no outgoing snapshot (source state is Quiescent), so there is
	// nothing to exit.
	atomic_store_explicit(&s_current, snap, memory_order_release);
	s_state = kDMCStateQuickDrawOwner;
	s_dmc_initialized = true;

	// Fire enter on T1 for any subscribers that registered before create
	// (cannot happen by contract, but safe to invoke an empty vector).
	int32_t enter_err = dmc_internal_fire_enter_events(snap);
	if (enter_err != kDMCNoErr) {
		// Rollback: undo T1. Since there is no prior snapshot, we tear
		// back down to Quiescent.
		dmc_internal_compensate_rejected_transition(
			snap, NULL, kDMCStateQuiescent, "dmc_create enter veto");
		free(snap);
		s_dmc_initialized = false;
		s_next_generation--;
		return kDMCErrSubscriberRejected;
	}
	return kDMCNoErr;
}

int32_t dmc_shutdown(void) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	// Fire exit on the current snapshot for every subscriber in
	// registration order. There is NO incoming snapshot in Quiescent, so
	// we do not fire enter (reverse enter is conditional on an incoming
	// snapshot).
	const DMCModeSnapshot *cur = atomic_load_explicit(&s_current, memory_order_relaxed);
	if (cur != NULL) {
		dmc_internal_fire_exit_events(cur);
	}
	atomic_store_explicit(&s_current, (const DMCModeSnapshot *)NULL,
	                      memory_order_release);
	s_state = kDMCStateQuiescent;
	s_dmc_initialized = false;
	s_mode_switch_prepared = false;
	s_mode_switch_source_state = kDMCStateQuiescent;
	memset(&s_mode_switch_requested, 0, sizeof(s_mode_switch_requested));

	// Free the outgoing snapshot AND the retirement ring (no readers
	// remain after shutdown; production callers are quiescent during
	// shutdown).
	if (cur != NULL) {
		free((void *)cur);
	}
	for (int i = 0; i < 2; ++i) {
		if (s_retired[i] != NULL) {
			free((void *)s_retired[i]);
			s_retired[i] = NULL;
		}
	}
	// Clear subscriber list on shutdown - a subsequent create+subscribe
	// sequence starts clean.
	s_subscribers.clear();
	return kDMCNoErr;
}

int32_t dmc_subscribe(const struct DMCSubscriber *sub) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	if (sub == NULL || sub->name == NULL) {
		return kDMCErrSubscriberRejected;
	}
	// Duplicate-name check.
	for (size_t i = 0; i < s_subscribers.size(); ++i) {
		if (s_subscribers[i].name != NULL &&
			strcmp(s_subscribers[i].name, sub->name) == 0) {
			return kDMCErrSubscriberAlreadyRegistered;
		}
	}
	DMCSubscriber copy = *sub;
	s_subscribers.push_back(copy);

	// Catchup synthesized on_mode_enter for mid-transition subscriber
	// ("subscribe always leaves you in a consistent state"). The new
	// subscriber observes the
	// current snapshot as its enter event. Return value is IGNORED here:
	// a mid-init subscriber cannot veto an already-committed state - it
	// must deal with whatever mode is currently published or fail in its
	// own init path.
	const DMCModeSnapshot *cur = atomic_load_explicit(&s_current, memory_order_relaxed);
	if (cur != NULL && copy.on_mode_enter != NULL) {
		(void)copy.on_mode_enter(cur, copy.ctx);
	}
	return kDMCNoErr;
}

int32_t dmc_unsubscribe(const char *name) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	if (name == NULL) {
		return kDMCErrSubscriberNotFound;
	}
	for (size_t i = 0; i < s_subscribers.size(); ++i) {
		if (s_subscribers[i].name != NULL &&
			strcmp(s_subscribers[i].name, name) == 0) {
			s_subscribers.erase(s_subscribers.begin() + i);
			return kDMCNoErr;
		}
	}
	return kDMCErrSubscriberNotFound;
}

// Reader path - lock-free. Callable from any thread. The returned pointer
// is valid until at LEAST the next VBL (ring-buffer retirement guarantee).
const struct DMCModeSnapshot *dmc_current_snapshot(void) {
	return atomic_load_explicit(&s_current, memory_order_acquire);
}

int32_t dmc_prepare_mode_switch(const struct DMCModeDesc *new_mode) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	if (s_state == kDMCStateTransitioning || s_mode_switch_prepared) {
		return kDMCErrTransitionInProgress;
	}
	if (s_state == kDMCStateBlanking) {
		return kDMCErrIllegalTransition;
	}
	int32_t verr = dmc_validate_mode_desc(new_mode);
	if (verr != kDMCNoErr) {
		return verr;
	}

	const DMCModeSnapshot *outgoing =
		atomic_load_explicit(&s_current, memory_order_relaxed);
	s_mode_switch_source_state = s_state;
	s_mode_switch_requested = *new_mode;
	s_mode_switch_prepared = true;
	s_state = kDMCStateTransitioning;

	if (outgoing != NULL) {
		dmc_internal_fire_exit_events(outgoing);
	}
	return kDMCNoErr;
}

int32_t dmc_cancel_prepared_mode_switch(void) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	if (!s_mode_switch_prepared ||
		s_state != kDMCStateTransitioning) {
		return kDMCErrIllegalTransition;
	}

	const DMCModeSnapshot *outgoing =
		atomic_load_explicit(&s_current, memory_order_relaxed);
	s_state = s_mode_switch_source_state;
	s_mode_switch_prepared = false;
	memset(&s_mode_switch_requested, 0, sizeof(s_mode_switch_requested));
	dmc_internal_fire_enter_events_advisory(
		outgoing, "dmc_cancel_prepared_mode_switch");
	return kDMCNoErr;
}

int32_t dmc_request_mode_switch(const struct DMCModeDesc *new_mode) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	const bool prepared = s_mode_switch_prepared &&
		s_state == kDMCStateTransitioning;
	if (s_state == kDMCStateTransitioning && !prepared) {
		return kDMCErrTransitionInProgress;
	}
	if (s_state == kDMCStateBlanking) {
		return kDMCErrIllegalTransition;
	}
	int32_t verr = dmc_validate_mode_desc(new_mode);
	if (verr == kDMCNoErr && prepared &&
		!dmc_same_mode_geometry(&s_mode_switch_requested, new_mode)) {
		verr = kDMCErrInvalidModeDesc;
	}
	if (verr != kDMCNoErr) {
		if (prepared) {
			const DMCModeSnapshot *outgoing =
				atomic_load_explicit(&s_current, memory_order_relaxed);
			s_state = s_mode_switch_source_state;
			s_mode_switch_prepared = false;
			memset(&s_mode_switch_requested, 0,
				   sizeof(s_mode_switch_requested));
			dmc_internal_fire_enter_events_advisory(
				outgoing, "dmc_request_mode_switch invalid prepared commit");
		}
		return verr;
	}

	// Read current snapshot via relaxed (mutex is held; readers only see
	// release-published values).
	const DMCModeSnapshot *outgoing = atomic_load_explicit(&s_current, memory_order_relaxed);
	DMCState         src_state = prepared
		? s_mode_switch_source_state : s_state;
	uint32_t prior_owner = (outgoing != NULL) ? outgoing->active_owner : (uint32_t)kDMCOwnerQuickDraw;
	uint32_t new_owner;
	if (prior_owner == kDMCOwnerRAVE ||
		prior_owner == kDMCOwnerGL ||
		prior_owner == kDMCOwnerDSp ||
		prior_owner == kDMCOwnerGlide) {
		new_owner = prior_owner;
	} else {
		new_owner = (uint32_t)kDMCOwnerQuickDraw;
	}
	// Carry palette_gen / gamma_gen / fade_active / blanking_rgba across the switch.
	uint32_t carry_palette_gen = (outgoing != NULL) ? outgoing->palette_gen : 0;
	uint32_t carry_gamma_gen   = (outgoing != NULL) ? outgoing->gamma_gen   : 0;
	// A mid-fade mode switch must NOT drop the fade flag - a
	// DSp game can FadeGammaOut across a resolution change.
	uint32_t carry_fade_active = (outgoing != NULL) ? outgoing->fade_active : 0;
	const uint8_t carry_blanking[4] = {
		(outgoing != NULL) ? outgoing->blanking_rgba[0] : (uint8_t)0,
		(outgoing != NULL) ? outgoing->blanking_rgba[1] : (uint8_t)0,
		(outgoing != NULL) ? outgoing->blanking_rgba[2] : (uint8_t)0,
		(outgoing != NULL) ? outgoing->blanking_rgba[3] : (uint8_t)0xFF
	};

	// Enter Transitioning state. We no longer mutate outgoing in
	// place (snapshots are immutable once published); the transition is
	// observable via s_state. Readers that cared about the former
	// snapshot.transitioning sentinel should read s_state or simply accept
	// a sub-microsecond window during which s_current still points at the
	// stable `outgoing`.
	if (!prepared) {
		s_state = kDMCStateTransitioning;

		// Legacy single-phase observer: detach immediately before publishing.
		// The canonical video driver uses prepare/commit so this callback runs
		// while the old platform context is still alive.
		if (outgoing != NULL) {
			dmc_internal_fire_exit_events(outgoing);
		}
	}

	// Build incoming snapshot.
	DMCModeSnapshot *incoming = dmc_alloc_snapshot_from_desc(new_mode,
															 s_next_generation,
															 new_owner,
															 carry_blanking);
	if (incoming == NULL) {
		// Allocation failure - roll back to prior stable state. No enter
		// has fired yet, but exits already ran and must be compensated.
		s_state = src_state;
		s_mode_switch_prepared = false;
		memset(&s_mode_switch_requested, 0,
			   sizeof(s_mode_switch_requested));
		dmc_internal_fire_enter_events_advisory(
			outgoing, "dmc_request_mode_switch allocation rollback");
		return kDMCErrOutOfMemory;
	}
	incoming->palette_gen = carry_palette_gen;
	incoming->gamma_gen   = carry_gamma_gen;
	incoming->fade_active = carry_fade_active;  // carry mid-fade flag
	// Carry gamma_lut across mode switch. dmc_alloc_snapshot_from_desc
	// initialized identity gamma; overwrite with outgoing LUT if available.
	if (outgoing != NULL) {
		memcpy(incoming->gamma_lut, outgoing->gamma_lut, 768);
		memcpy(incoming->driver_gamma_lut, outgoing->driver_gamma_lut, 768);
	}
	s_next_generation++;

	// Publish (atomic release-store).
	atomic_store_explicit(&s_current, incoming, memory_order_release);
	DMCState target = dmc_target_state_for_owner(new_owner);
	s_state = target;
	s_mode_switch_prepared = false;
	memset(&s_mode_switch_requested, 0, sizeof(s_mode_switch_requested));

	// Fire enter (LIFO) AFTER publication.
	int32_t enter_err = dmc_internal_fire_enter_events(incoming);
	if (enter_err != kDMCNoErr) {
		// Rollback: compensate subscribers, re-publish outgoing, and
		// retire the rejected incoming.
		dmc_internal_compensate_rejected_transition(
			incoming, outgoing, src_state,
			"dmc_request_mode_switch enter veto");
		// Route the rejected snapshot through
		// the retirement ring so concurrent readers get one more publish of
		// grace before it is freed. incoming->generation + 1 is the ring slot
		// the NEXT successful publish would have used; concurrent readers
		// holding `incoming` finish their frame before this slot is
		// overwritten (at gen + 3 publish). The rejected generation stays
		// consumed - we do NOT decrement s_next_generation because
		// the ring-slot calculation relies on the bumped counter.
		dmc_retire_snapshot(incoming, incoming->generation + 1);
		return kDMCErrSubscriberRejected;
	}

	// Retire outgoing into the 2-generation ring buffer.
	dmc_retire_snapshot(outgoing, incoming->generation);
	return kDMCNoErr;
}

int32_t dmc_set_active_owner(uint32_t owner) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	// Out-of-enum-range check. kDMCOwnerGlide is the highest legal owner
	// that set_active_owner may request. Blanking / Quiescent are reached
	// only via dmc_request_blanking / dmc_shutdown.
	if (owner > (uint32_t)kDMCOwnerGlide) {
		return kDMCErrInvalidOwner;
	}
	if (owner == (uint32_t)kDMCOwnerBlanking ||
		owner == (uint32_t)kDMCOwnerQuiescent) {
		return kDMCErrInvalidOwner;
	}
	if (s_state == kDMCStateBlanking) {
		return kDMCErrIllegalTransition;
	}
	if (s_state == kDMCStateTransitioning) {
		return kDMCErrTransitionInProgress;
	}

	const DMCModeSnapshot *outgoing = atomic_load_explicit(&s_current, memory_order_relaxed);
	// Idempotent early-return when the requested
	// owner + the current FSM state already match. Once engines
	// start vending overlays per frame (which drives a dmc_set_active_owner call
	// per frame), this prevents 60 Hz calloc + subscriber-dispatch churn. Both
	// owner AND state must match - a state transition (e.g. back from Blanking)
	// still requires a real transition.
	if (outgoing != NULL &&
		outgoing->active_owner == owner &&
		s_state == dmc_target_state_for_owner(owner)) {
		return kDMCNoErr;
	}
	DMCModeSnapshot *incoming = dmc_clone_snapshot_with_owner(outgoing,
															  s_next_generation,
															  owner,
															  NULL);
	if (incoming == NULL) {
		return kDMCErrOutOfMemory;
	}
	s_next_generation++;
	atomic_store_explicit(&s_current, incoming, memory_order_release);
	DMCState target = dmc_target_state_for_owner(owner);
	s_state = target;

	/* active_owner identifies the most recent screen producer, not a resource
	 * lifetime. No display surface or GL/Metal context changed here, so firing
	 * mode callbacks would destroy the texture the producer is about to
	 * submit and then re-vend an unrelated replacement under the same frame. */
	dmc_retire_snapshot(outgoing, incoming->generation);
	return kDMCNoErr;
}

int32_t dmc_request_blanking(const uint8_t rgba[4]) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	if (s_state == kDMCStateBlanking) {
		return kDMCErrBlankingAlreadyActive;
	}
	if (s_state == kDMCStateTransitioning) {
		return kDMCErrTransitionInProgress;
	}

	const DMCModeSnapshot *outgoing = atomic_load_explicit(&s_current, memory_order_relaxed);

	DMCModeSnapshot *incoming = dmc_clone_snapshot_with_owner(outgoing,
															  s_next_generation,
															  (uint32_t)kDMCOwnerBlanking,
															  rgba);
	if (incoming == NULL) {
		return kDMCErrOutOfMemory;
	}
	s_next_generation++;
	atomic_store_explicit(&s_current, incoming, memory_order_release);
	s_state = kDMCStateBlanking;

	/* Blanking changes visible policy, not framebuffer identity. Preserve all
	 * engine allocations so a fade cannot invalidate the frame it is fading. */
	dmc_retire_snapshot(outgoing, incoming->generation);
	return kDMCNoErr;
}

int32_t dmc_end_blanking(void) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	if (s_state != kDMCStateBlanking) {
		return kDMCErrNotBlanking;
	}

	const DMCModeSnapshot *outgoing = atomic_load_explicit(&s_current, memory_order_relaxed);

	DMCModeSnapshot *incoming = dmc_clone_snapshot_with_owner(outgoing,
															  s_next_generation,
															  (uint32_t)kDMCOwnerQuickDraw,
															  NULL);
	if (incoming == NULL) {
		return kDMCErrOutOfMemory;
	}
	s_next_generation++;
	atomic_store_explicit(&s_current, incoming, memory_order_release);
	s_state = kDMCStateQuickDrawOwner;

	dmc_retire_snapshot(outgoing, incoming->generation);
	return kDMCNoErr;
}

// palette_gen / gamma_gen bump requires ALLOCATING A FRESH SNAPSHOT under
// the immutability invariant. The published snapshot is NEVER
// mutated in place. snapshot.generation is bumped on every mutation
// (including pure palette/gamma bumps) so the retirement ring slot math
// is consistent.
//
// No subscriber events are fired - these are pure snapshot updates, not
// FSM transitions. Subscribers that care about palette/gamma changes
// observe them by comparing palette_gen / gamma_gen across successive
// calls to dmc_current_snapshot().
int32_t dmc_record_palette_change(void) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	const DMCModeSnapshot *old = atomic_load_explicit(&s_current, memory_order_relaxed);
	if (old == NULL) {
		return kDMCErrNotInitialized;
	}
	DMCModeSnapshot *fresh = dmc_alloc_raw_snapshot();
	if (fresh == NULL) {
		return kDMCErrOutOfMemory;
	}
	memcpy(fresh, old, sizeof(DMCModeSnapshot));
	fresh->generation  = s_next_generation++;
	fresh->palette_gen = old->palette_gen + 1;

	atomic_store_explicit(&s_current, fresh, memory_order_release);
	dmc_retire_snapshot(old, fresh->generation);
	return kDMCNoErr;
}

// Companion to
// dmc_record_gamma_change_with_lut that ALSO publishes a fade_active flag in
// the SAME snapshot bump. Clone-mutate-publish under the EXISTING s_write_mutex
// (no NEW MTLFence / MTLSharedEvent / mutex / @synchronized / atomic - the
// flag rides the existing atomic-release publish). Publishing fade_active
// and the interpolated LUT together avoids a frame where the LUT is mid-fade
// but the flag is stale, which would warp one fade frame: a separate
// fade_active setter would produce exactly that torn frame.
int32_t dmc_record_gamma_change_with_lut_fade(const uint8_t *lut, int fade_active) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	const DMCModeSnapshot *old = atomic_load_explicit(&s_current, memory_order_relaxed);
	if (old == NULL) {
		return kDMCErrNotInitialized;
	}
	DMCModeSnapshot *fresh = dmc_alloc_raw_snapshot();
	if (fresh == NULL) {
		return kDMCErrOutOfMemory;
	}
	memcpy(fresh, old, sizeof(DMCModeSnapshot));
	fresh->generation  = s_next_generation++;
	fresh->gamma_gen   = old->gamma_gen + 1;
	fresh->fade_active = (uint32_t)(fade_active ? 1 : 0);

	if (lut != NULL) {
		memcpy(fresh->gamma_lut, lut, 768);
	}
	// If lut == NULL, the memcpy from old already carried the existing LUT

	atomic_store_explicit(&s_current, fresh, memory_order_release);
	dmc_retire_snapshot(old, fresh->generation);
	return kDMCNoErr;
}

// Record a gamma ramp change with optional LUT data. If lut is non-NULL,
// the 768 bytes (planar: 256 R + 256 G + 256 B) overwrite the snapshot's
// gamma_lut field. If lut is NULL, the memcpy from old snapshot carries the
// existing LUT data unchanged (backward compatible with dmc_record_gamma_change).
//
// A plain SetGamma is NOT a fade, so this
// 1-arg form delegates with fade_active=0. The fade-driver uses the explicit
// 2-arg dmc_record_gamma_change_with_lut_fade overload instead.
int32_t dmc_record_gamma_change_with_lut(const uint8_t *lut) {
	return dmc_record_gamma_change_with_lut_fade(lut, 0);
}

// Record a DRIVER (guest SetGamma) table: always store it in
// driver_gamma_lut - the "original intensity" the DSp fades blend toward -
// and apply it to the displayed gamma_lut only when no fade is in progress.
// During a fade the displayed LUT is left alone (an immediate apply would
// visibly pop the faded screen); the fade's end-state push delivers the
// table instead. Same clone-mutate-publish discipline as the other
// dmc_record_* writers (existing s_write_mutex; no new primitives).
int32_t dmc_record_driver_gamma_change(const uint8_t *lut) {
	if (lut == NULL) {
		return kDMCErrInvalidModeDesc;
	}
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	const DMCModeSnapshot *old = atomic_load_explicit(&s_current, memory_order_relaxed);
	if (old == NULL) {
		return kDMCErrNotInitialized;
	}
	DMCModeSnapshot *fresh = dmc_alloc_raw_snapshot();
	if (fresh == NULL) {
		return kDMCErrOutOfMemory;
	}
	memcpy(fresh, old, sizeof(DMCModeSnapshot));
	fresh->generation = s_next_generation++;
	memcpy(fresh->driver_gamma_lut, lut, 768);

	const bool apply_now = (old->fade_active == 0);
	if (apply_now) {
		fresh->gamma_gen = old->gamma_gen + 1;
		memcpy(fresh->gamma_lut, lut, 768);
	}

	atomic_store_explicit(&s_current, fresh, memory_order_release);
	dmc_retire_snapshot(old, fresh->generation);
	return apply_now ? kDMCNoErr : kDMCDriverGammaDeferred;
}

// Legacy API: bump gamma_gen without changing LUT data. Delegates to
// dmc_record_gamma_change_with_lut(NULL) for backward compatibility.
int32_t dmc_record_gamma_change(void) {
	return dmc_record_gamma_change_with_lut(NULL);
}

// Assign the snapshot's blanking color WITHOUT
// entering the Blanking FSM state. DSpSetBlankingColor (sub-op 760) only sets
// the color the library will use the next time the screen IS blanked - it does
// NOT blank now (DSp 1.7 PDF p.30). This is the no-state-transition twin of
// dmc_record_gamma_change_with_lut: clone the current snapshot, mutate the one
// field (blanking_rgba), bump generation, publish, retire old. It deliberately
// does NOT call any blanking-enter path (dmc_request_blanking transitions the
// FSM to Blanking - wrong for SetBlankingColor).
//
// Reuses the EXISTING DMC single-writer primitive (s_write_mutex +
// DMCReentryScope + DMC_ASSERT_EMUL_THREAD); adds ZERO new MTLFence /
// MTLSharedEvent / mutex / @synchronized / atomic. The DMC is the
// documented single-writer exception, so this is compliant.
//
// No subscriber events are fired - this is a pure snapshot field update, not an
// FSM transition (same posture as dmc_record_palette_change / gamma_change).
int32_t dmc_set_blanking_color(const uint8_t rgba[4]) {
	DMCReentryScope reentry;
	if (!reentry.installed) {
		return kDMCErrReentrantRequest;
	}
	DMCLock guard(s_write_mutex);
	DMC_ASSERT_EMUL_THREAD();

	if (!s_dmc_initialized) {
		return kDMCErrNotInitialized;
	}
	if (rgba == NULL) {
		return kDMCErrInvalidModeDesc;   /* defensive: param-invalid */
	}
	const DMCModeSnapshot *old = atomic_load_explicit(&s_current, memory_order_relaxed);
	if (old == NULL) {
		return kDMCErrNotInitialized;
	}
	DMCModeSnapshot *fresh = dmc_alloc_raw_snapshot();
	if (fresh == NULL) {
		return kDMCErrOutOfMemory;
	}
	memcpy(fresh, old, sizeof(DMCModeSnapshot));
	fresh->generation = s_next_generation++;
	memcpy(fresh->blanking_rgba, rgba, 4);

	atomic_store_explicit(&s_current, fresh, memory_order_release);
	dmc_retire_snapshot(old, fresh->generation);
	return kDMCNoErr;
}
