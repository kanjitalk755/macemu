/*
 *  gfxaccel_resources_heap.cpp - pure C++ logic.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Pure C++ companion to gfxaccel_resources_heap.mm. Owns:
 *    - PSO cache: std::unordered_map keyed by shader pair +
 *      vertex descriptor + color format tuple.
 *    - LRU purgeable bookkeeping: std::deque of purgeable
 *      entries with last-use frame tracking.
 *    - Per-heap ceiling overrides for testing.
 *    - Init/shutdown idempotency (same pattern as gfxaccel_resources.cpp).
 *
 *  Metal allocation sites live in gfxaccel_resources_heap.mm and are
 *  reached via forward-declared extern "C" helpers. No id<MTLHeap> /
 *  id<MTLBuffer> / id<MTLTexture> types cross this .cpp file.
 *
 *  Threading: single-writer from PPC emul thread. No std::mutex,
 *  no std::atomic. The emul serial queue in the .mm side is the
 *  synchronization primitive for memory-warning dispatch.
 */

#include "gfxaccel_resources_heap.h"
#include "display_mode_controller.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <deque>
#include <vector>

// Forward-declared helpers implemented in gfxaccel_resources_heap.mm.
// Keeping them extern "C" so the linker resolves cross-file without
// mangling (same pattern as gfxaccel_resources.cpp).
extern "C" void gfxaccel_resources_heap_mm_init(void);
extern "C" void gfxaccel_resources_heap_mm_shutdown(void);
extern "C" void *gfxaccel_resources_heap_mm_get(uint32_t heap_id);
extern "C" void *gfxaccel_resources_heap_mm_alloc_buffer(uint32_t heap_id,
                                                          uint32_t length,
                                                          uint32_t options);
extern "C" void gfxaccel_resources_heap_mm_lru_purge(void);
extern "C" uint64_t gfxaccel_resources_heap_mm_reset(uint32_t heap_id);
extern "C" void gfxaccel_resources_heap_mm_note_allocation_released(uint32_t heap_id);
extern "C" uint32_t gfxaccel_resources_heap_mm_live_allocation_count(uint32_t heap_id);

namespace {

// ---------------------------------------------------------------------------
// File-scope state
// ---------------------------------------------------------------------------

static bool s_heap_initialized = false;

// ---------------------------------------------------------------------------
// PSO cache
// ---------------------------------------------------------------------------

struct PSOCacheKey {
	uint64_t shader_pair_hash;
	uint64_t vertex_descriptor_hash;
	uint32_t color_format[4];

	bool operator==(const PSOCacheKey &other) const {
		return shader_pair_hash == other.shader_pair_hash &&
		       vertex_descriptor_hash == other.vertex_descriptor_hash &&
		       color_format[0] == other.color_format[0] &&
		       color_format[1] == other.color_format[1] &&
		       color_format[2] == other.color_format[2] &&
		       color_format[3] == other.color_format[3];
	}
};

struct PSOCacheKeyHash {
	size_t operator()(const PSOCacheKey &k) const {
		// FNV-1a inspired hash combining all fields
		size_t h = 14695981039346656037ULL;
		h ^= k.shader_pair_hash;
		h *= 1099511628211ULL;
		h ^= k.vertex_descriptor_hash;
		h *= 1099511628211ULL;
		for (int i = 0; i < 4; ++i) {
			h ^= (size_t)k.color_format[i];
			h *= 1099511628211ULL;
		}
		return h;
	}
};

// Values are void* (id<MTLRenderPipelineState> as void* -- the .mm side
// manages the ARC lifetime via CFBridgingRetain/Release).
static std::unordered_map<PSOCacheKey, void*, PSOCacheKeyHash> g_pso_cache;

// ---------------------------------------------------------------------------
// LRU purgeable list
// ---------------------------------------------------------------------------

struct PurgeableEntry {
	uint32_t heap_id;
	void    *resource;       // id<MTLResource> as void*
	uint64_t last_use_frame;
};

static std::deque<PurgeableEntry> g_lru;

// ---------------------------------------------------------------------------
// DMC on_mode_exit bump-reset subscriber
// ---------------------------------------------------------------------------
//
// Registration order matters. The subscriber is added from
// gfxaccel_resources_heap_init(), which is itself called from
// gfxaccel_resources_init() AFTER the "gfxaccel_resources" DMC subscriber
// registers (index 1). The compositor registers FIRST (index 0 via
// MetalCompositorInit). Therefore the heap-reset subscriber claims
// index 2 and, per display_mode_controller.cpp's
// dmc_internal_fire_exit_events FIFO dispatch, fires LAST on every
// on_mode_exit. Any engine-owned subscribers (or gfxaccel_resources's own
// engine-detach fan-out) register at or before index 2 (they attach
// from video_install_accel inside the gfxaccel_resources engine fan-out,
// not as independent DMC subscribers). The reset path still enforces this
// ordering instead of trusting it: if a heap has live sub-allocations, reset
// logs an error and skips so placement heaps cannot alias live bytes.
//
// Registration failure (e.g., kDMCErrSubscriberAlreadyRegistered during
// a test fixture's repeated init/shutdown cycle) is logged but non-fatal:
// the module still works, only the automatic per-mode-exit reset is
// missed. Tests can call gfxaccel_resources_heap_reset directly to
// exercise the reset semantics without depending on a DMC fan-out.
static int32_t s_heap_reset_on_mode_exit(const struct DMCModeSnapshot *outgoing,
                                          void *ctx)
{
	(void)outgoing;
	(void)ctx;
	for (uint32_t i = 0; i < (uint32_t)kHeapCount; ++i) {
		if (i == (uint32_t)kHeapEngineDSp) continue;  // kHeapDSp is reset by DSp release/shutdown when idle, NEVER on DMC mode exit. This preserves DSp back buffers and alt buffers across mode switches.
		uint64_t reclaimed = gfxaccel_resources_heap_mm_reset(i);
		if (reclaimed > 0) {
			fprintf(stderr,
			        "[gfxaccel-heap] on_mode_exit: heap_id=%u reset "
			        "reclaimed %llu bytes\n",
			        (unsigned)i, (unsigned long long)reclaimed);
		}
	}
	return kDMCNoErr;
}

static DMCSubscriber s_heap_reset_subscriber = {
	/* .name          = */ "gfxaccel_heap_reset",
	/* .on_mode_exit  = */ s_heap_reset_on_mode_exit,
	/* .on_mode_enter = */ NULL,
	/* .ctx           = */ NULL
};

} // namespace (anonymous)

// ---------------------------------------------------------------------------
// Public API - Lifecycle
// ---------------------------------------------------------------------------

extern "C" int32_t gfxaccel_resources_heap_init(void)
{
	if (s_heap_initialized) {
		// Idempotent per header contract.
		return kGfxAccelResNoErr;
	}

	// Initialize the Metal-side state (no-op -- heaps are lazy).
	gfxaccel_resources_heap_mm_init();

	// Register the DMC on_mode_exit subscriber that
	// zeros the bump-allocator offset counters after engine detach fires.
	// See s_heap_reset_subscriber comment for ordering rationale. Non-fatal
	// on failure (tests can call gfxaccel_resources_heap_reset directly).
	int32_t sub_err = dmc_subscribe(&s_heap_reset_subscriber);
	if (sub_err != kDMCNoErr &&
	    sub_err != kDMCErrSubscriberAlreadyRegistered) {
		fprintf(stderr,
		        "[gfxaccel-heap] dmc_subscribe('gfxaccel_heap_reset') "
		        "returned %d (non-fatal; automatic per-mode-exit reset "
		        "disabled for this session)\n",
		        (int)sub_err);
	}

	s_heap_initialized = true;
	return kGfxAccelResNoErr;
}

extern "C" void gfxaccel_resources_heap_shutdown(void)
{
	if (!s_heap_initialized) {
		return;
	}

	// Unregister the DMC bump-reset subscriber BEFORE
	// tearing down Metal-side state. Tolerate kDMCErrSubscriberNotFound so
	// repeated shutdowns (or shutdown-before-init via testing_reset) don't
	// emit noise. Using the static .name pointer directly — matches the
	// gfxaccel_resources unsubscribe precedent.
	(void)dmc_unsubscribe(s_heap_reset_subscriber.name);

	// Clear PSO cache (Tier 2 eviction -- release all cached PSOs).
	gfxaccel_heap_pso_cache_clear();

	// Drain LRU list.
	g_lru.clear();

	// Release Metal-side heaps.
	gfxaccel_resources_heap_mm_shutdown();

	s_heap_initialized = false;
}

// ---------------------------------------------------------------------------
// Public API - Heap access + sub-allocation
// ---------------------------------------------------------------------------

extern "C" void *gfxaccel_resources_heap_get(uint32_t heap_id)
{
	if (heap_id >= kHeapCount) {
		fprintf(stderr, "[gfxaccel-heap] heap_get: heap_id=%u out of range "
		                "(max=%u)\n", (unsigned)heap_id, (unsigned)kHeapCount);
		return NULL;
	}
	return gfxaccel_resources_heap_mm_get(heap_id);
}

extern "C" void *gfxaccel_resources_heap_alloc_buffer(uint32_t heap_id,
                                                       uint32_t length,
                                                       uint32_t options)
{
	if (heap_id >= kHeapCount) {
		fprintf(stderr, "[gfxaccel-heap] alloc_buffer: heap_id=%u out of range\n",
		        (unsigned)heap_id);
		return NULL;
	}
	return gfxaccel_resources_heap_mm_alloc_buffer(heap_id, length, options);
}

// Public wrapper for the per-heap bump-offset reset. Delegates to the
// .mm side which owns the
// offset counter. Bounds checking is done both here (defensive) and in
// the .mm side (authoritative).
extern "C" uint64_t gfxaccel_resources_heap_reset(uint32_t heap_id)
{
	if (heap_id >= kHeapCount) {
		return 0;
	}
	return gfxaccel_resources_heap_mm_reset(heap_id);
}

extern "C" void gfxaccel_resources_heap_note_allocation_released(uint32_t heap_id)
{
	if (heap_id >= kHeapCount) {
		fprintf(stderr, "[gfxaccel-heap] note_allocation_released: heap_id=%u "
		                "out of range\n", (unsigned)heap_id);
		return;
	}
	gfxaccel_resources_heap_mm_note_allocation_released(heap_id);
}

extern "C" uint32_t gfxaccel_resources_heap_live_allocation_count(uint32_t heap_id)
{
	if (heap_id >= kHeapCount) {
		return 0;
	}
	return gfxaccel_resources_heap_mm_live_allocation_count(heap_id);
}

// ---------------------------------------------------------------------------
// Public API - Eviction pipeline
// ---------------------------------------------------------------------------

extern "C" void gfxaccel_heap_pso_cache_clear(void)
{
	// PSO cache values are void* handles managed by the .mm side via
	// CFBridgingRetain/Release. PSO entries are not yet inserted
	// (that happens when engines migrate later). The clear path
	// delegates release to the .mm side for future entries; for now,
	// just clearing the map is sufficient.
	g_pso_cache.clear();
}

extern "C" void gfxaccel_heap_lru_purge_volatile(void)
{
	// Delegate the actual Metal purgeable-state work to the .mm side
	// which has access to the Metal objects.
	gfxaccel_resources_heap_mm_lru_purge();
	g_lru.clear();
}

