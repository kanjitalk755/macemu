/*
 *  gfxaccel_resources.cpp - resource-manager fan-out logic.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Pure C++ companion to gfxaccel_resources.mm. Owns:
 *    - the DMC subscription handshake (dmc_subscribe under name
 *      "gfxaccel_resources");
 *    - GfxRes_OnModeExit / GfxRes_OnModeEnter fan-out callbacks that
 *      relay to engine-owned attach/detach handlers;
 *    - register_engine / unregister_engine bookkeeping.
 *
 *  Metal allocation sites live in gfxaccel_resources.mm and are reached
 *  via two forward-declared extern "C" helpers (init_metal_state,
 *  shutdown_metal_state). No id<MTLTexture> / id<MTLBuffer> types cross
 *  this .cpp file.
 *
 *  Threading (documented carve-out):
 *    Single-threaded from the PPC emul thread. No std::mutex, no atomic.
 *    The DMC threading exception remains the ONLY
 *    acceleration-code module with concurrency primitives.
 */

#include "gfxaccel_resources.h"
#include "gfxaccel_resources_heap.h"
#include "pso_archive.h"
#include "display_mode_controller.h"

#include <cstdio>
#include <cstring>
#include <vector>

// Forward-declared helpers implemented in gfxaccel_resources.mm.
// Keeping them extern "C" so the linker resolves cross-file without
// mangling and so the .mm side can keep using plain Obj-C++ function
// definitions without C++ namespace boilerplate.
extern "C" void gfxaccel_resources_mm_init_metal_state(void);
extern "C" void gfxaccel_resources_mm_shutdown_metal_state(void);
// Background/foreground lifecycle handlers implemented in
// gfxaccel_resources.mm. Forward declarations here so the linker sees them
// from both .cpp and .mm translation units.
extern "C" void gfxaccel_handle_background_enter(void);
extern "C" void gfxaccel_handle_foreground_enter(void);
extern "C" void gfxaccel_set_dsp_background_hook(GfxAccelLifecycleHookFn fn, void *ctx);
extern "C" void gfxaccel_set_dsp_foreground_hook(GfxAccelLifecycleHookFn fn, void *ctx);

namespace {

// Internal registry entry. NOT exposed in the public header -
// callers interact via register_engine / unregister_engine only.
struct GfxResEngineRegistration {
	uint32_t                    engine_id;
	struct GfxResEngineHandlers handlers;
};

// File-scope state. Single-writer / single-reader = emul thread only.
// No concurrency primitives here; see file header.
static std::vector<GfxResEngineRegistration> s_engine_handlers;
static bool                                  s_initialized = false;

// DMC subscriber entry. Borrows the name's C-string storage (static
// duration) - matches the "compositor" subscriber pattern established
// in metal_compositor.mm.
static int32_t GfxRes_OnModeExit(const struct DMCModeSnapshot *outgoing, void *ctx);
static int32_t GfxRes_OnModeEnter(const struct DMCModeSnapshot *incoming, void *ctx);

static DMCSubscriber s_dmc_subscriber = {
	/* .name          = */ "gfxaccel_resources",
	/* .on_mode_exit  = */ GfxRes_OnModeExit,
	/* .on_mode_enter = */ GfxRes_OnModeEnter,
	/* .ctx           = */ NULL
};

// ---------------------------------------------------------------------------
// Fan-out callbacks
// ---------------------------------------------------------------------------

// on_mode_exit: FIFO engine registration order.
// Each detach return is advisory (logged only) - the DMC exit-fire
// contract does not allow vetoing an already-committed transition.
static int32_t GfxRes_OnModeExit(const struct DMCModeSnapshot *outgoing, void *ctx)
{
	(void)ctx;
	for (size_t i = 0; i < s_engine_handlers.size(); ++i) {
		const GfxResEngineRegistration &entry = s_engine_handlers[i];
		if (entry.handlers.detach != NULL) {
			int32_t r = entry.handlers.detach(entry.engine_id, outgoing,
			                                   entry.handlers.ctx);
			if (r != 0) {
				fprintf(stderr,
				        "[gfxaccel_resources] detach(engine_id=%u) returned %d "
				        "during on_mode_exit (advisory; ignored)\n",
				        (unsigned)entry.engine_id, (int)r);
			}
		}
	}
	return kDMCNoErr;
}

// on_mode_enter: LIFO (reverse registration order) - mirrors the DMC
// delivery contract so the FIRST engine that registered with us is the
// LAST to receive an attach callback. A non-zero attach return
// propagates back to DMC as kDMCErrSubscriberRejected so the
// rollback path fires.
static int32_t GfxRes_OnModeEnter(const struct DMCModeSnapshot *incoming, void *ctx)
{
	(void)ctx;
	for (std::vector<GfxResEngineRegistration>::reverse_iterator it = s_engine_handlers.rbegin();
	     it != s_engine_handlers.rend(); ++it) {
		if (it->handlers.attach != NULL) {
			int32_t r = it->handlers.attach(it->engine_id, incoming,
			                                 it->handlers.ctx);
			if (r != 0) {
				fprintf(stderr,
				        "[gfxaccel_resources] attach(engine_id=%u) returned %d "
				        "during on_mode_enter - propagating as subscriber rejection\n",
				        (unsigned)it->engine_id, (int)r);
				return kDMCErrSubscriberRejected;
			}
		}
	}
	return kDMCNoErr;
}

} // namespace (anonymous)

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" int32_t gfxaccel_resources_init(void)
{
	if (s_initialized) {
		// Idempotent per header contract.
		return kGfxAccelResNoErr;
	}

	// Subscribe to DMC SECOND (metal_compositor subscribes FIRST during
	// MetalCompositorInit - see the gfxaccel_resources.h file header).
	// Tolerate kDMCErrSubscriberAlreadyRegistered so
	// repeated bring-up/tear-down cycles within a single process don't
	// leave us stuck.
	int32_t sub_err = dmc_subscribe(&s_dmc_subscriber);
	if (sub_err != kDMCNoErr && sub_err != kDMCErrSubscriberAlreadyRegistered) {
		fprintf(stderr,
		        "[gfxaccel_resources] dmc_subscribe('gfxaccel_resources') FAILED "
		        "err=%d - resource manager not usable\n", (int)sub_err);
		return kGfxAccelResErrGeneric;
	}

	// Initialize the Metal-side state (zero out the overlay fleet; the
	// framebuffer MTLBuffer stays nil until first vend).
	gfxaccel_resources_mm_init_metal_state();

	// Initialize the heap sub-allocator. Heaps themselves are
	// lazy; this just sets up bookkeeping + eviction semaphore.
	gfxaccel_resources_heap_init();

	// Initialize the PSO binary archive. Loads the
	// shipped MTLBinaryArchive from the app bundle so PSO cache misses
	// check the archive before falling back to runtime compile. Non-fatal:
	// if the archive is missing or iOS < 14, runtime compile is the only
	// path. Return code is logged but does not fail the parent init.
	{
		int32_t pso_err = pso_archive_init();
		if (pso_err != 0 && pso_err != kGfxAccelErrPSOArchiveNotAvailable) {
			fprintf(stderr,
			        "[gfxaccel_resources] pso_archive_init() returned %d "
			        "(non-fatal; runtime compile fallback)\n", (int)pso_err);
		}
	}

	s_initialized = true;
	return kGfxAccelResNoErr;
}

extern "C" void gfxaccel_resources_shutdown(void)
{
	if (!s_initialized) {
		return;
	}

	// Shut down the PSO binary archive BEFORE draining the fan-out
	// registry and heap sub-allocator. The archive is read-only and
	// has no dependency on heaps, but shutting it down first ensures
	// clean ordering.
	pso_archive_shutdown();

	// Drain the fan-out registry. Any engine that forgot to unregister
	// is silently dropped - this is a defensive final tear-down path;
	// engines that leak should be caught by their own shutdown tests,
	// not by this module.
	s_engine_handlers.clear();

	// Shut down the heap sub-allocator BEFORE releasing Metal resources.
	// This ensures heap-backed buffers are released while the heaps
	// are still alive, maintaining proper ARC release ordering.
	gfxaccel_resources_heap_shutdown();

	// Release Metal resources BEFORE unsubscribing from DMC so any in-
	// flight DMC exit event doesn't race a detach against a freed
	// texture. (The fan-out is single-threaded so this is belt-and-
	// suspenders; the heap work adds real asynchrony.)
	gfxaccel_resources_mm_shutdown_metal_state();

	int32_t unsub_err = dmc_unsubscribe(s_dmc_subscriber.name);
	if (unsub_err != kDMCNoErr && unsub_err != kDMCErrSubscriberNotFound) {
		fprintf(stderr,
		        "[gfxaccel_resources] dmc_unsubscribe('gfxaccel_resources') "
		        "returned err=%d (tolerated)\n", (int)unsub_err);
	}

	s_initialized = false;
}

extern "C" void gfxaccel_resources_register_engine(uint32_t engine_id,
                                                   const struct GfxResEngineHandlers *h)
{
	if (h == NULL) {
		fprintf(stderr, "[gfxaccel_resources] register_engine(%u) with NULL handlers "
		                "- ignored\n", (unsigned)engine_id);
		return;
	}
	if (engine_id >= (uint32_t)kGfxEngineCount) {
		fprintf(stderr, "[gfxaccel_resources] register_engine: engine_id=%u out of "
		                "range (max=%u)\n",
		        (unsigned)engine_id, (unsigned)kGfxEngineCount);
		return;
	}

	// Replace any existing entry for this engine_id. Engines own their
	// own lifecycle - a second register call means "use these handlers
	// now" not "add another subscriber".
	for (size_t i = 0; i < s_engine_handlers.size(); ++i) {
		if (s_engine_handlers[i].engine_id == engine_id) {
			s_engine_handlers[i].handlers = *h;
			return;
		}
	}

	GfxResEngineRegistration entry;
	entry.engine_id = engine_id;
	entry.handlers  = *h;
	s_engine_handlers.push_back(entry);
}

extern "C" void gfxaccel_resources_unregister_engine(uint32_t engine_id)
{
	for (size_t i = 0; i < s_engine_handlers.size(); ++i) {
		if (s_engine_handlers[i].engine_id == engine_id) {
			s_engine_handlers.erase(s_engine_handlers.begin() + (std::ptrdiff_t)i);
			return;
		}
	}
	// Silent no-op if engine wasn't registered - matches the header
	// contract (engines that never registered shouldn't trigger a
	// diagnostic at shutdown).
}

