/*
 *  gfxaccel_resources_heap.mm - Metal heap implementation.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Metal-side half of gfxaccel_resources_heap. Owns:
 *    - Per-engine MTLHeap creation (lazy).
 *    - Sub-allocation from heaps via the `:offset:` API variants
 *      (newBufferWithLength:options:offset:, newTextureWithDescriptor:offset:).
 *    - Per-heap bump sub-allocator (next_offset), advanced on each alloc
 *      and reset on DMC on_mode_exit.
 *    - Three-tier eviction pipeline: LRU purge, PSO flush, loud fail.
 *    - Memory-warning C shim: atomically marks an eviction request that
 *      SubmitFrame drains on the engine call path.
 *    - Compositor handshake: SubmitFrame drains pending evictions.
 *
 *  Placement-heap policy is used on iOS 17+ via the `:offset:` alloc API
 *  variants. A per-heap bump allocator tracks next_offset, advanced on
 *  each alloc and reset on DMC on_mode_exit. This gives exact memory
 *  accounting.
 *
 *  Alignment floor: each allocation aligns to max(Metal-reported-align,
 *  256). Over-commit returns NULL + os_log_fault without invoking
 *  Metal alloc (prevents Placement validation assertion from firing).
 *
 *  ARC ownership: g_heaps[] are strong references under ARC. Setting nil
 *  releases the heap and all sub-allocations.
 *
 *  Threading: heap allocation/reset/purge remains single-writer on the
 *  engine call path. The UIKit memory-warning callback only toggles an
 *  atomic pending flag; it does not mutate heaps.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <os/log.h>
#import <TargetConditionals.h>

#include <cstdint>
#include <cstdio>
#include <dispatch/dispatch.h>
#include <array>
#include <atomic>

#include "gfxaccel_resources_heap.h"
#include "metal_device_shared.h"

// ---------------------------------------------------------------------------
// File-scope Metal state
// ---------------------------------------------------------------------------

static id<MTLHeap> g_heaps[kHeapCount] = { nil, nil, nil, nil, nil };

// Per-heap monotonic bump offset (bump allocator).
// Reset to 0 on DMC on_mode_exit. All allocations
// compute their aligned start offset from this counter and advance it.
// No std::atomic: single-writer invariant (emul serial queue).
static NSUInteger g_next_offset[kHeapCount] = { 0, 0, 0, 0, 0 };

// Per-heap live sub-allocation count. A placement heap's bump offset can only
// be reset when this is zero; otherwise offset 0 could alias a live resource.
static uint32_t g_live_allocations[kHeapCount] = { 0, 0, 0, 0, 0 };

// Tracks heaps explicitly marked MTLPurgeableStateEmpty by eviction. A heap
// set Empty is restored to NonVolatile before the next placement allocation.
static bool g_heap_purgeable_empty[kHeapCount] = { false, false, false, false, false };

// Per-heap GPU-completion latch. A bump reset must not zero next_offset
// while a committed-but-incomplete command buffer may still be reading
// heap memory: the heaps are MTLHazardTrackingModeUntracked and
// g_live_allocations tracks CPU ownership, not GPU completion, so a
// reset + immediate re-alloc could placement-alias in-flight bytes.
// Engines note each command buffer that references heap memory BEFORE
// committing it; the addCompletedHandler bumps the completed counter
// (same completion-handler shape as the RAVE ring slot gate below).
// Reset defers (returns 0) while pending != completed; the DSp VBL
// release drain retries every tick, so a deferred reset lands one VBL
// later. pending is single-writer on the emul thread; completed is
// bumped from Metal's completion-handler thread, hence atomic.
static uint64_t g_gpu_commits_pending[kHeapCount] = { 0, 0, 0, 0, 0 };
static std::atomic<uint64_t> g_gpu_commits_completed[kHeapCount];

// Heap sizes (initial = ceiling constants from header; overridable via
// gfxaccel_heap_testing_set_ceiling for tests).
static NSUInteger g_heap_sizes[kHeapCount] = {
	kHeapCeiling_NQD,
	kHeapCeiling_RAVE,
	kHeapCeiling_GL,
	kHeapCeiling_Compositor,
	kHeapCeiling_DSp
};

// Default ceiling values for reset.
static const NSUInteger g_heap_defaults[kHeapCount] = {
	kHeapCeiling_NQD,
	kHeapCeiling_RAVE,
	kHeapCeiling_GL,
	kHeapCeiling_Compositor,
	kHeapCeiling_DSp
};

// os_log subsystem for heap exhaustion + eviction timeout
static os_log_t gfxaccel_heap_log(void)
{
	static os_log_t log = NULL;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		log = os_log_create("com.carbjo.pocketshaver.gfxaccel", "heap");
	});
	return log;
}

static std::atomic_bool g_eviction_pending(false);

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Forward declarations of C++ side eviction functions.
extern "C" void gfxaccel_heap_lru_purge_volatile(void);
extern "C" void gfxaccel_heap_pso_cache_clear(void);

// Align `offset` up to `alignment`, honoring the floor of 256
// bytes. Metal's reported alignment may be lower (e.g., 16 bytes for a
// simple buffer); 256 is the conservative cache-line / MTL offset
// alignment requirement that also matches the RAVE ring buffer stride.
static inline NSUInteger align_offset_up(NSUInteger offset,
                                          NSUInteger alignment)
{
	if (alignment == 0) {
		alignment = 256;
	}
	NSUInteger a = alignment < 256 ? 256 : alignment;
	return (offset + (a - 1)) & ~(a - 1);
}

static bool restore_heap_if_purgeable(uint32_t heap_id)
{
	if (heap_id >= kHeapCount || g_heaps[heap_id] == nil) {
		return false;
	}
	if (!g_heap_purgeable_empty[heap_id]) {
		return true;
	}

	MTLPurgeableState previous =
	    [g_heaps[heap_id] setPurgeableState:MTLPurgeableStateNonVolatile];
	g_heap_purgeable_empty[heap_id] = false;
	if (previous == MTLPurgeableStateEmpty) {
		os_log(gfxaccel_heap_log(),
		       "heap_id=%u restored from MTLPurgeableStateEmpty before reuse",
		       (unsigned)heap_id);
	}
	return true;
}

// ---------------------------------------------------------------------------
// extern "C" helpers called from gfxaccel_resources_heap.cpp
// ---------------------------------------------------------------------------

extern "C" void gfxaccel_resources_heap_mm_init(void)
{
	// No-op: heaps are lazy.
	g_eviction_pending.store(false, std::memory_order_release);
}

extern "C" void gfxaccel_resources_heap_mm_shutdown(void)
{
	// Release all heaps (ARC).
	for (int i = 0; i < kHeapCount; ++i) {
		g_heaps[i] = nil;
	}

	// Reset heap sizes to defaults.
	for (int i = 0; i < kHeapCount; ++i) {
		g_heap_sizes[i] = g_heap_defaults[i];
	}

	// Reset per-heap bump offsets.
	for (int i = 0; i < kHeapCount; ++i) {
		g_next_offset[i] = 0;
	}

	// All sub-allocations were released with the heaps.
	for (int i = 0; i < kHeapCount; ++i) {
		g_live_allocations[i] = 0;
	}

	for (int i = 0; i < kHeapCount; ++i) {
		g_heap_purgeable_empty[i] = false;
	}

	// Reset eviction state.
	g_eviction_pending.store(false, std::memory_order_release);
}

extern "C" void *gfxaccel_resources_heap_mm_get(uint32_t heap_id)
{
	if (heap_id >= kHeapCount) {
		return NULL;
	}

	// Lazy-create the heap on first access.
	if (g_heaps[heap_id] != nil) {
		restore_heap_if_purgeable(heap_id);
		return (__bridge void *)g_heaps[heap_id];
	}

	id<MTLDevice> device = (__bridge id<MTLDevice>)SharedMetalDevice();
	if (device == nil) {
		fprintf(stderr, "[gfxaccel-heap] heap_get(id=%u): SharedMetalDevice() "
		                "returned nil\n", (unsigned)heap_id);
		return NULL;
	}

	MTLHeapDescriptor *desc = [[MTLHeapDescriptor alloc] init];
	desc.size = g_heap_sizes[heap_id];
#if TARGET_OS_SIMULATOR
	// iOS simulator Metal driver requires MTLStorageModePrivate for heaps.
	// On-device builds use Shared for CPU+GPU access.
	desc.storageMode = MTLStorageModePrivate;
#else
	desc.storageMode = MTLStorageModeShared;
#endif
	desc.hazardTrackingMode = MTLHazardTrackingModeUntracked;

	// Placement on iOS 17+ for exact memory accounting. Alloc paths use
	// the :offset: API variants required by Placement. Automatic retained
	// for iOS 13.4-16.x.
	if (@available(iOS 17, *)) {
		desc.type = MTLHeapTypePlacement;
	} else {
		desc.type = MTLHeapTypeAutomatic;
	}

	g_heaps[heap_id] = [device newHeapWithDescriptor:desc];
	if (g_heaps[heap_id] == nil) {
		os_log_fault(gfxaccel_heap_log(),
		             "Failed to create MTLHeap for heap_id=%u size=%lu",
		             (unsigned)heap_id, (unsigned long)g_heap_sizes[heap_id]);
		return NULL;
	}
	g_heap_purgeable_empty[heap_id] = false;

	return (__bridge void *)g_heaps[heap_id];
}

extern "C" void *gfxaccel_resources_heap_mm_alloc_buffer(uint32_t heap_id,
                                                          uint32_t length,
                                                          uint32_t options)
{
	// Ensure heap exists (lazy create).
	if (gfxaccel_resources_heap_mm_get(heap_id) == NULL) {
		return NULL;
	}

	id<MTLHeap> heap = g_heaps[heap_id];

	// On simulator, heap uses Private storage; force buffer options to match.
#if TARGET_OS_SIMULATOR
	MTLResourceOptions allocOptions = MTLResourceStorageModePrivate;
#else
	MTLResourceOptions allocOptions = (MTLResourceOptions)options;
#endif

	// Bump allocator: query the device for the size/alignment
	// requirement (Metal's heapBuffer/heapTexture size-and-align APIs live
	// on MTLDevice, not MTLHeap), align the current bump offset up, call
	// the :offset: variant required by Placement heaps, and advance the
	// bump offset.
	id<MTLDevice> device = (__bridge id<MTLDevice>)SharedMetalDevice();
	MTLSizeAndAlign sa = [device heapBufferSizeAndAlignWithLength:(NSUInteger)length
	                                                       options:allocOptions];

	id<MTLBuffer> buf = nil;
	for (int tier = 0; tier < 3; ++tier) {
		restore_heap_if_purgeable(heap_id);
		NSUInteger aligned = align_offset_up(g_next_offset[heap_id], sa.align);

		// Over-commit pre-check: if the aligned placement would exceed
		// the heap's size, skip the Metal call (Placement would fire a
		// validation assertion) and fall through to the eviction tier.
		if (aligned + sa.size > g_heap_sizes[heap_id]) {
			// Fall through to eviction below.
		} else {
			buf = [heap newBufferWithLength:(NSUInteger)length
			                        options:allocOptions
			                         offset:aligned];
			if (buf != nil) {
				g_next_offset[heap_id] = aligned + sa.size;
				g_live_allocations[heap_id]++;
				return (__bridge_retained void *)buf;
			}
			// Metal refused at this offset; advance past the failed
			// attempt so we don't re-hit the same offset next tier.
			g_next_offset[heap_id] = aligned + sa.size;
		}

		// Eviction pipeline: tier 0 -> LRU purge, tier 1 -> PSO flush.
		// Tier 2 has no additional eviction action; it's the final retry.
		if (tier == 0) {
			gfxaccel_heap_lru_purge_volatile();
		} else if (tier == 1) {
			gfxaccel_heap_pso_cache_clear();
		}
	}

	// Tier 3 - Loud failure. No silent fallback.
	os_log_fault(gfxaccel_heap_log(),
	             "Heap exhausted for heap_id=%u: alloc_buffer(length=%u) "
	             "failed after 3-tier eviction (next_offset=%lu, size=%lu, align=%lu)",
	             (unsigned)heap_id, (unsigned)length,
	             (unsigned long)g_next_offset[heap_id],
	             (unsigned long)sa.size, (unsigned long)sa.align);

	return NULL;
}

// Per-heap bump-offset reset. Zeros g_next_offset[heap_id] and returns
// the number of bytes reclaimed (= the previous next_offset). Does NOT
// release g_heaps[heap_id] — only the offset counter is reset.
// Invalid heap_id is a no-op returning 0 (bounds check at function entry).
// gpu_idle_asserted skips the GPU-completion latch for callers that have
// already proven idleness (explicit waitUntilCompleted on the shared
// queue); the live-allocation gate always applies.
static uint64_t heap_mm_reset_internal(uint32_t heap_id,
                                       bool gpu_idle_asserted)
{
	if (heap_id >= kHeapCount) {
		return 0;
	}
	if (g_live_allocations[heap_id] != 0) {
		os_log_fault(gfxaccel_heap_log(),
		             "heap reset skipped for heap_id=%u: %u live sub-allocations "
		             "(next_offset=%lu)",
		             (unsigned)heap_id,
		             (unsigned)g_live_allocations[heap_id],
		             (unsigned long)g_next_offset[heap_id]);
		fprintf(stderr,
		        "[gfxaccel-heap] reset skipped: heap_id=%u live=%u "
		        "next_offset=%llu\n",
		        (unsigned)heap_id,
		        (unsigned)g_live_allocations[heap_id],
		        (unsigned long long)g_next_offset[heap_id]);
		return 0;
	}
	if (!gpu_idle_asserted &&
	    g_gpu_commits_completed[heap_id].load(std::memory_order_acquire) <
	        g_gpu_commits_pending[heap_id]) {
		// In-flight GPU reads of heap memory: deferring keeps offset 0
		// from aliasing bytes a committed blit may still touch. Expected
		// transient — the caller's next reset attempt (e.g. the DSp VBL
		// release drain) lands after the completed handler fires.
		os_log(gfxaccel_heap_log(),
		       "heap reset deferred for heap_id=%u: command buffer(s) "
		       "referencing this heap still in flight (next_offset=%lu)",
		       (unsigned)heap_id,
		       (unsigned long)g_next_offset[heap_id]);
		return 0;
	}
	NSUInteger reclaimed = g_next_offset[heap_id];
	g_next_offset[heap_id] = 0;
	return (uint64_t)reclaimed;
}

extern "C" uint64_t gfxaccel_resources_heap_mm_reset(uint32_t heap_id)
{
	return heap_mm_reset_internal(heap_id, false);
}

extern "C" uint64_t gfxaccel_resources_heap_reset_gpu_idle(uint32_t heap_id)
{
	return heap_mm_reset_internal(heap_id, true);
}

extern "C" void gfxaccel_resources_heap_note_gpu_commit(uint32_t heap_id,
                                                         void *command_buffer)
{
	if (heap_id >= kHeapCount || command_buffer == NULL) {
		return;
	}
	id<MTLCommandBuffer> cmd = (__bridge id<MTLCommandBuffer>)command_buffer;
	std::atomic<uint64_t> *completed = &g_gpu_commits_completed[heap_id];
	g_gpu_commits_pending[heap_id]++;
	[cmd addCompletedHandler:^(id<MTLCommandBuffer> completedCommandBuffer) {
		(void)completedCommandBuffer;
		completed->fetch_add(1, std::memory_order_release);
	}];
}

extern "C" void gfxaccel_resources_heap_mm_note_allocation_released(uint32_t heap_id)
{
	if (heap_id >= kHeapCount) {
		return;
	}
	if (g_live_allocations[heap_id] == 0) {
		os_log_fault(gfxaccel_heap_log(),
		             "heap live allocation underflow for heap_id=%u",
		             (unsigned)heap_id);
		fprintf(stderr,
		        "[gfxaccel-heap] live allocation underflow: heap_id=%u\n",
		        (unsigned)heap_id);
		return;
	}
	g_live_allocations[heap_id]--;
}

extern "C" uint32_t gfxaccel_resources_heap_mm_live_allocation_count(uint32_t heap_id)
{
	if (heap_id >= kHeapCount) {
		return 0;
	}
	return g_live_allocations[heap_id];
}

extern "C" void gfxaccel_resources_heap_mm_lru_purge(void)
{
	// Walk heaps and set purgeable state to Empty only for heaps with no
	// live resources. Live heaps are skipped because Empty makes their
	// contents OS-discardable.
	for (int i = 0; i < kHeapCount; ++i) {
		if (g_heaps[i] == nil) {
			continue;
		}
		if (g_live_allocations[i] != 0) {
			os_log(gfxaccel_heap_log(),
			       "heap purge skipped for heap_id=%d: %u live sub-allocations",
			       i, (unsigned)g_live_allocations[i]);
			continue;
		}
		[g_heaps[i] setPurgeableState:MTLPurgeableStateEmpty];
		g_heap_purgeable_empty[i] = true;
	}
}

// ---------------------------------------------------------------------------
// Memory-warning handler
// ---------------------------------------------------------------------------

extern "C" void gfxaccel_handle_memory_warning(void)
{
	g_eviction_pending.store(true, std::memory_order_release);
}

extern "C" int32_t gfxaccel_heap_wait_for_eviction(uint64_t frame_interval_usec)
{
	(void)frame_interval_usec;
	if (!g_eviction_pending.exchange(false, std::memory_order_acq_rel)) {
		return kGfxAccelResNoErr;
	}

	gfxaccel_heap_lru_purge_volatile();
	gfxaccel_heap_pso_cache_clear();
	return kGfxAccelResNoErr;
}

// ---------------------------------------------------------------------------
// Public wrappers (route through heap_mm_ variants)
// ---------------------------------------------------------------------------

// gfxaccel_resources_heap_get and gfxaccel_resources_heap_alloc_buffer are
// implemented in the .cpp side and call through to the _mm_ variants declared
// above.

// ---------------------------------------------------------------------------
// RaveRingBuffer implementation
//
// Triple-buffered ring for RAVE per-draw vertex staging. Eliminates 15+
// newBufferWithBytes: calls per frame. Allocated directly from the device
// (not from kHeapEngineRAVE heap) because CPU write access (memcpy) requires
// MTLResourceStorageModeShared, which is incompatible with the simulator's
// MTLStorageModePrivate heap requirement. Matches GL ring buffer allocation
// pattern (gl_metal_renderer.mm:641).
// ---------------------------------------------------------------------------

#define RAVE_RING_BUFFER_SIZE  (4 * 1024 * 1024)   // 4 MiB per slot (matches GL precedent)
#define RAVE_RING_BUFFER_COUNT 3                     // triple-buffered

static id<MTLBuffer> g_rave_ring_buffer = nil;
static uint32_t      g_rave_ring_slot_index = 0;
static NSUInteger    g_rave_ring_offset = 0;  // write offset within current slot
// Per-slot credits rather than one anonymous counting semaphore: each RAVE
// context commits on its OWN command queue and Metal orders completion only
// within a queue, so an anonymous credit returned by queue B's finished
// buffer must not let round-robin staging wrap onto a slot queue A's
// still-executing buffer reads. Each slot is gated on completion of the
// submission that consumed that specific slot.
static dispatch_semaphore_t g_rave_ring_slot_sems[RAVE_RING_BUFFER_COUNT];
static bool          g_rave_ring_slot_acquired = false;
static uint32_t      g_rave_ring_slots_in_submission = 0;
static uint32_t      g_rave_ring_submission_first_slot = 0;

// Backstop only: the renderer commits the submission window before it can
// hold every credit (gfxaccel_rave_ring_submission_near_exhaustion), so a
// wait here is bounded by in-flight GPU completion. If that invariant ever
// breaks, fail the stage loudly instead of hanging the emul/UI thread.
#define RAVE_RING_ACQUIRE_TIMEOUT_NSEC  (1ull * NSEC_PER_SEC)

static bool gfxaccel_rave_ring_acquire_slot(void)
{
	dispatch_semaphore_t slot_sem = g_rave_ring_slot_sems[g_rave_ring_slot_index];
	if (slot_sem == NULL) {
		return false;
	}

	if (dispatch_semaphore_wait(slot_sem,
	        dispatch_time(DISPATCH_TIME_NOW, RAVE_RING_ACQUIRE_TIMEOUT_NSEC)) != 0) {
		os_log_fault(gfxaccel_heap_log(),
		             "rave_ring_acquire_slot: slot %u still in flight after "
		             "%llu ms; dropping stage instead of deadlocking",
		             (unsigned)g_rave_ring_slot_index,
		             (unsigned long long)(RAVE_RING_ACQUIRE_TIMEOUT_NSEC / NSEC_PER_MSEC));
		return false;
	}
	if (g_rave_ring_slots_in_submission == 0) {
		g_rave_ring_submission_first_slot = g_rave_ring_slot_index;
	}
	g_rave_ring_slot_acquired = true;
	g_rave_ring_slots_in_submission++;
	return true;
}

static void gfxaccel_rave_ring_advance_slot(void)
{
	g_rave_ring_slot_index = (g_rave_ring_slot_index + 1) % RAVE_RING_BUFFER_COUNT;
	g_rave_ring_offset = 0;
	g_rave_ring_slot_acquired = false;
}

static void gfxaccel_rave_ring_signal_slot_range(
    const std::array<dispatch_semaphore_t, RAVE_RING_BUFFER_COUNT> &sems,
    uint32_t first_slot, uint32_t slot_count)
{
	for (uint32_t i = 0; i < slot_count; i++) {
		dispatch_semaphore_t sem = sems[(first_slot + i) % RAVE_RING_BUFFER_COUNT];
		if (sem != NULL) {
			dispatch_semaphore_signal(sem);
		}
	}
}

extern "C" int32_t gfxaccel_rave_ring_init(void)
{
	if (g_rave_ring_buffer != nil) {
		return kGfxAccelResNoErr;  // idempotent
	}

	id<MTLDevice> device = (__bridge id<MTLDevice>)SharedMetalDevice();
	if (device == nil) {
		fprintf(stderr, "[gfxaccel-heap] rave_ring_init: SharedMetalDevice() nil\n");
		return -4006;
	}

	NSUInteger totalSize = (NSUInteger)RAVE_RING_BUFFER_SIZE * RAVE_RING_BUFFER_COUNT;
	g_rave_ring_buffer = [device newBufferWithLength:totalSize
	                                         options:MTLResourceStorageModeShared];
	if (g_rave_ring_buffer == nil) {
		os_log_fault(gfxaccel_heap_log(),
		             "rave_ring_init: failed to allocate %lu byte ring buffer",
		             (unsigned long)totalSize);
		return -4006;
	}

	for (uint32_t i = 0; i < RAVE_RING_BUFFER_COUNT; i++) {
		g_rave_ring_slot_sems[i] = dispatch_semaphore_create(1);
	}
	g_rave_ring_slot_index = 0;
	g_rave_ring_offset = 0;
	g_rave_ring_slot_acquired = false;
	g_rave_ring_slots_in_submission = 0;
	g_rave_ring_submission_first_slot = 0;

	return kGfxAccelResNoErr;
}

extern "C" void gfxaccel_rave_ring_shutdown(void)
{
	g_rave_ring_buffer = nil;
	for (uint32_t i = 0; i < RAVE_RING_BUFFER_COUNT; i++) {
		g_rave_ring_slot_sems[i] = NULL;
	}
	g_rave_ring_slot_index = 0;
	g_rave_ring_offset = 0;
	g_rave_ring_slot_acquired = false;
	g_rave_ring_slots_in_submission = 0;
	g_rave_ring_submission_first_slot = 0;
}

extern "C" void *gfxaccel_rave_ring_stage(const void *data, uint32_t size,
                                          uint32_t *out_offset)
{
	if (g_rave_ring_buffer == nil || data == NULL || out_offset == NULL) {
		return NULL;
	}

	// Align size to 256 bytes (Metal vertex buffer offset alignment).
	NSUInteger alignedSize = ((NSUInteger)size + 255) & ~(NSUInteger)255;
	if (alignedSize > (NSUInteger)RAVE_RING_BUFFER_SIZE) {
		fprintf(stderr, "[gfxaccel-heap] rave_ring_stage: size %u exceeds slot "
		        "capacity\n", (unsigned)size);
		return NULL;
	}

	if (!g_rave_ring_slot_acquired &&
	    !gfxaccel_rave_ring_acquire_slot()) {
		return NULL;
	}

	// Check if current slot has room.
	if (g_rave_ring_offset + alignedSize > (NSUInteger)RAVE_RING_BUFFER_SIZE) {
		// Current slot full -- wait before staging into the next slot.
		gfxaccel_rave_ring_advance_slot();
		if (!gfxaccel_rave_ring_acquire_slot()) {
			return NULL;
		}
	}

	NSUInteger slotBase = (NSUInteger)g_rave_ring_slot_index * RAVE_RING_BUFFER_SIZE;
	NSUInteger writeOff = slotBase + g_rave_ring_offset;

	// Verify write stays within slot boundary.
	if (writeOff + size > slotBase + RAVE_RING_BUFFER_SIZE) {
		fprintf(stderr, "[gfxaccel-heap] rave_ring_stage: size %u exceeds slot "
		        "capacity at offset %lu\n", (unsigned)size,
		        (unsigned long)g_rave_ring_offset);
		return NULL;
	}

	memcpy((char *)[g_rave_ring_buffer contents] + writeOff, data, size);
	*out_offset = (uint32_t)writeOff;

	g_rave_ring_offset += alignedSize;

	return (__bridge void *)g_rave_ring_buffer;
}

extern "C" void gfxaccel_rave_ring_frame_end(void *command_buffer)
{
	if (g_rave_ring_buffer == nil) {
		return;
	}

	uint32_t slots_consumed = g_rave_ring_slots_in_submission;
	if (slots_consumed == 0) {
		return;
	}

	// Copy the semaphores into the handler so a shutdown between commit and
	// completion cannot strand un-signaled (and un-deallocatable) slots.
	std::array<dispatch_semaphore_t, RAVE_RING_BUFFER_COUNT> sems;
	for (uint32_t i = 0; i < RAVE_RING_BUFFER_COUNT; i++) {
		sems[i] = g_rave_ring_slot_sems[i];
	}
	uint32_t first_slot = g_rave_ring_submission_first_slot;
	g_rave_ring_slots_in_submission = 0;
	gfxaccel_rave_ring_advance_slot();

	id<MTLCommandBuffer> cmd = (__bridge id<MTLCommandBuffer>)command_buffer;
	if (cmd != nil) {
		[cmd addCompletedHandler:^(id<MTLCommandBuffer> completedCommandBuffer) {
			(void)completedCommandBuffer;
			gfxaccel_rave_ring_signal_slot_range(sems, first_slot, slots_consumed);
		}];
	} else {
		gfxaccel_rave_ring_signal_slot_range(sems, first_slot, slots_consumed);
	}
}

extern "C" int32_t gfxaccel_rave_ring_submission_near_exhaustion(void)
{
	if (g_rave_ring_buffer == nil) {
		return 0;
	}
	// A single draw sequence can need up to two more slot acquires beyond
	// the slots the window already holds (overflow of the vertex stage plus
	// a second stage such as multi-texture UVs). Credits consumed by the
	// uncommitted window are only signal-registered at frame_end (commit),
	// so once the window holds COUNT-1 slots a further acquire could wait
	// on a credit the window itself holds. The renderer commits and
	// restarts the pass at a draw-sequence boundary when this fires.
	return (g_rave_ring_slots_in_submission >= RAVE_RING_BUFFER_COUNT - 1) ? 1 : 0;
}

