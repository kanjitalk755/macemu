/*
 *  gfxaccel_resources_heap.h - Per-engine MTLHeap sub-allocator.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Per-engine MTLHeap sub-allocator, PSO cache, LRU purgeable list,
 *  three-tier eviction pipeline, ring buffer primitive, and
 *  memory-warning C shim entry point.
 *
 *  Design notes:
 *    - Separate module from gfxaccel_resources.{h,cpp,mm}.
 *    - Five MTLHeaps (NQD / RAVE / GL / Compositor / DSp).
 *    - iOS 17+ -> MTLHeapTypePlacement; 13.4-16.x -> Automatic.
 *    - Per-heap ceilings (32 / 128 / 96 / 48 / 32 MiB).
 *    - Heap creation is lazy (first use, not init).
 *    - Bump reset is legal only when the heap has no live sub-allocations.
 *    - Three-tier eviction: LRU purge -> PSO flush -> loud assert.
 *    - Memory-warning C shim called from Swift observer.
 *    - Compositor handshake via dispatch_semaphore.
 *    - In-memory PSO cache (std::unordered_map).
 *    - Engine attach/detach plugs into the resource fan-out.
 *
 *  Threading: single-writer from PPC emul thread. Memory-warning
 *  handler marshals main -> emul via dispatch_async onto a serial queue
 *  owned by this module. NO std::mutex, NO std::atomic.
 *
 *  C-callable throughout: the header can be included from .cpp, .mm, or
 *  Swift-via-bridging-header without pulling in Metal types; id<MTLHeap>,
 *  id<MTLBuffer>, id<MTLTexture> return values are exposed as void* and
 *  bridge-cast inside the .mm implementation.
 */

#ifndef GFXACCEL_RESOURCES_HEAP_H
#define GFXACCEL_RESOURCES_HEAP_H

#include <stdint.h>

#include "gfxaccel_resources.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GfxHeapId enum.
 *
 * Five heaps partition ownership:
 *   - NQD:        mask buffer, scratch textures (engine-private).
 *   - RAVE:       engine-private transient heap resources.
 *   - GL:         engine-private transient heap resources.
 *   - Compositor: resources crossing the engine<->compositor boundary
 *                 when a future resource has mode-bounded heap lifetime.
 *   - DSp:        back buffers and alt buffers.
 */
typedef enum {
	kHeapEngineNQD   = 0,
	kHeapEngineRAVE  = 1,
	kHeapEngineGL    = 2,
	kHeapCompositor  = 3,
	kHeapEngineDSp   = 4,   // 5th per-engine heap; exempt from on_mode_exit reset (bump-with-conditional-reset).
	kHeapCount       = 5
} GfxHeapId;

/*
 * Error codes extending the -4000 block (-4001..-4006 used elsewhere).
 */
enum GfxAccelHeapError {
	kGfxAccelResErrHeapExhausted       = -4007,
	kGfxAccelResErrMemoryWarningTimeout = -4008
};

/*
 * Per-heap ceilings. Initial values; tunable via UAT.
 * CI assertion in HeapBudgetTests.testHeapBudgetsWithinClassFloor
 * verifies the sum + system overhead stays below kHeapCeiling_TotalMax.
 */
#define kHeapCeiling_NQD            (32  * 1024 * 1024)
#define kHeapCeiling_RAVE           (128 * 1024 * 1024)
#define kHeapCeiling_GL             (96  * 1024 * 1024)
#define kHeapCeiling_Compositor     (48  * 1024 * 1024)
#define kHeapCeiling_DSp            (32  * 1024 * 1024)  // back buffers + per-context overlays (32 MiB)
#define kHeapCeiling_SystemOverhead (96  * 1024 * 1024)
#define kHeapCeiling_TotalMax       (544 * 1024 * 1024)  // was 512; +32 MiB for the DSp heap

/* --- Lifecycle --- */

/*
 * Initialize the heap sub-allocator. Idempotent - a second call returns
 * kGfxAccelResNoErr without side effects. Must be called after
 * SharedMetalDevice() is available.
 *
 * Heaps themselves are NOT created here (lazy on first use).
 */
int32_t gfxaccel_resources_heap_init(void);

/*
 * Shut down the heap sub-allocator. Releases all heaps, clears the PSO
 * cache, drains the LRU list. Idempotent.
 */
void gfxaccel_resources_heap_shutdown(void);

/* --- Heap access + sub-allocation --- */

/*
 * Return the MTLHeap for the given heap_id as void*. Lazy-creates the
 * heap on first call. Returns NULL if heap creation fails.
 */
void *gfxaccel_resources_heap_get(uint32_t heap_id);

/*
 * Sub-allocate a buffer from the named heap. Returns id<MTLBuffer> as
 * void*. On failure (heap exhausted after eviction), returns NULL.
 * options: MTLResourceOptions (e.g. MTLResourceStorageModeShared).
 */
void *gfxaccel_resources_heap_alloc_buffer(uint32_t heap_id,
                                           uint32_t length,
                                           uint32_t options);

/*
 * Reset the bump allocator's offset counter for a single heap. Returns
 * the number of bytes reclaimed (equal to the previous next_offset).
 * Idempotent: a second call on a freshly-reset heap returns 0.
 * Does NOT release the underlying MTLHeap — only resets the offset.
 *
 * Typically called from the DMC on_mode_exit fan-out (bump allocator is
 * per-mode-scope). Direct callers outside the DMC subscriber exist for
 * tests only. Invalid heap_id (>= kHeapCount) is a no-op that returns 0.
 */
uint64_t gfxaccel_resources_heap_reset(uint32_t heap_id);

/*
 * Record that one heap sub-allocation has been released by its owner.
 * This is accounting only; ownership of the underlying Objective-C object
 * stays with the caller. Reset refuses to zero a heap's bump offset while
 * this count is non-zero.
 */
void gfxaccel_resources_heap_note_allocation_released(uint32_t heap_id);

/*
 * Return the number of live heap sub-allocations tracked for heap_id.
 * Invalid heap_id returns 0.
 */
uint32_t gfxaccel_resources_heap_live_allocation_count(uint32_t heap_id);

/* --- GPU-completion latch --- */

/*
 * Record that a command buffer referencing memory sub-allocated from
 * heap_id is about to be committed. MUST be called BEFORE the commit
 * (addCompletedHandler is illegal after commit). The bump reset
 * (gfxaccel_resources_heap_reset) defers — returns 0, next_offset
 * untouched — while any noted command buffer has not yet completed, so
 * a reset + immediate re-alloc cannot placement-alias bytes an
 * in-flight GPU read may still touch (heaps are hazard-untracked; the
 * live-allocation count covers CPU ownership only). command_buffer is
 * an id<MTLCommandBuffer> bridged as void*; NULL is a no-op.
 */
void gfxaccel_resources_heap_note_gpu_commit(uint32_t heap_id,
                                             void *command_buffer);

/*
 * Bump-reset variant for callers that have already proven GPU idleness
 * (e.g. the background-enter path runs after an explicit
 * waitUntilCompleted on the shared queue — see gfxaccel_resources.mm
 * Step 2). Skips the GPU-completion latch; the live-allocation gate
 * still applies. Same return semantics as gfxaccel_resources_heap_reset.
 */
uint64_t gfxaccel_resources_heap_reset_gpu_idle(uint32_t heap_id);

/* --- Memory-warning handler --- */

/*
 * C shim called from the Swift MemoryWarningObserver on the main thread.
 * Marks an atomic pending-eviction flag and returns immediately. The next
 * SubmitFrame call drains the eviction request on the engine call path.
 */
void gfxaccel_handle_memory_warning(void);

/*
 * Compositor handshake: called from SubmitFrame to run a pending memory-warning
 * eviction on the engine call path. frame_interval_usec is retained for ABI
 * compatibility. Returns kGfxAccelResNoErr if no eviction was pending or the
 * eviction pipeline was drained.
 */
int32_t gfxaccel_heap_wait_for_eviction(uint64_t frame_interval_usec);

/* --- Eviction pipeline --- */

/*
 * Tier 1: purge LRU volatile resources. Called as part of the eviction
 * pipeline or independently.
 */
void gfxaccel_heap_lru_purge_volatile(void);

/*
 * Tier 2: flush the in-memory PSO cache. Drops all cached
 * pipeline states. Next frame pays recompile cost.
 */
void gfxaccel_heap_pso_cache_clear(void);

/* --- RAVE Ring Buffer --- */

/*
 * Initialize the RAVE triple-buffered ring buffer. Allocates a single
 * MTLBuffer of RAVE_RING_BUFFER_SIZE * RAVE_RING_BUFFER_COUNT (4 MiB x 3)
 * from the device (MTLResourceStorageModeShared for CPU write access).
 * Creates dispatch_semaphore for triple-buffer synchronization.
 * Idempotent. Returns 0 on success, -4006 on failure.
 */
int32_t gfxaccel_rave_ring_init(void);

/*
 * Shut down the RAVE ring buffer. Releases the MTLBuffer and semaphore.
 */
void gfxaccel_rave_ring_shutdown(void);

/*
 * Stage vertex data into the ring buffer. Copies 'size' bytes from 'data'
 * into the current write position. Sets *out_offset to the byte offset
 * into the ring buffer for use with setVertexBuffer:offset:atIndex:.
 * Advances write offset by size aligned to 256 bytes.
 * Waits before the first write to each slot. If current slot overflows,
 * advances to the next slot and waits before staging there.
 * Returns the ring buffer as void* (caller bridge-casts to id<MTLBuffer>).
 * Returns NULL if ring is not initialized or data/out_offset is NULL.
 */
void *gfxaccel_rave_ring_stage(const void *data, uint32_t size,
                               uint32_t *out_offset);

/*
 * Called when a RAVE command buffer that may reference ring-backed vertex data
 * is submitted. command_buffer is an id<MTLCommandBuffer> bridged as void*.
 * Registers completion-handler signals for every slot consumed since the
 * previous submission and advances to the next slot for subsequent staging.
 */
void gfxaccel_rave_ring_frame_end(void *command_buffer);

/*
 * Returns 1 when the in-progress (uncommitted) submission window holds
 * enough ring slots that a worst-case draw sequence could block acquiring
 * a credit the window itself holds — only frame_end (commit) returns those
 * credits, so without a mid-frame commit the wait could never complete.
 * The RAVE renderer polls this at draw-sequence boundaries (before per-draw
 * encoder state is applied) and commits + restarts the render pass when set.
 * Returns 0 if the ring is not initialized.
 */
int32_t gfxaccel_rave_ring_submission_near_exhaustion(void);

#ifdef __cplusplus
}
#endif

#endif /* GFXACCEL_RESOURCES_HEAP_H */
