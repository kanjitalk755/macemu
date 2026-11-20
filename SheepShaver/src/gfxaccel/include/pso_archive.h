/*
 *  pso_archive.h - PSO binary archive module.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Provides MTLBinaryArchive-backed PSO persistence so first-launch
 *  has zero runtime shader compiles for stock pipelines. The archive
 *  is loaded from the app bundle at init time (iOS 14+ only; iOS 13.x
 *  is a no-op fallback to runtime compile).
 *
 *  Design notes:
 *    - New module pso_archive.{h,mm} in SheepShaver/src/gfxaccel/.
 *    - Build-time archive generation via PSOArchiveCaptureTests.swift.
 *    - PSO cache integration: archive as backing store.
 *    - First-launch budget: <=150 ms for first frame on iPhone 12.
 *
 *  Scope: Compositor + NQD stock descriptors only. RAVE and GL
 *  PSO descriptors are highly variable (many shader permutations) and
 *  will be added later once those engines stabilize the shader
 *  permutation set.
 *
 *  C-callable throughout: the header can be included from .cpp, .mm, or
 *  Swift-via-bridging-header without pulling in Metal types; PSO return
 *  values are exposed as void* and bridge-cast inside the .mm
 *  implementation.
 */

#ifndef PSO_ARCHIVE_H
#define PSO_ARCHIVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Error codes extending the -4000 block (continuing from -4012).
 */
enum GfxAccelPSOArchiveError {
	kGfxAccelErrPSOArchiveLoadFailed   = -4012,
	kGfxAccelErrPSOArchiveNotAvailable = -4013  /* iOS < 14 */
};

/* --- Lifecycle --- */

/*
 * Load the stock PSO binary archive from the app bundle.
 *
 * iOS 14+ gate: on iOS 13.x, sets s_initialized = true and returns
 * kGfxAccelErrPSOArchiveNotAvailable (non-fatal; caller knows archive
 * is unavailable and runtime compile is the only path).
 *
 * If the archive file (stock_pipelines.metalarchive) is not present in
 * the bundle, returns 0 with s_archive = nil (no archive available;
 * runtime compile fallback for all PSOs).
 *
 * Must be called after SharedMetalDevice() is available.
 *
 * Returns 0 on success, kGfxAccelErrPSOArchiveLoadFailed on Metal
 * error, kGfxAccelErrPSOArchiveNotAvailable on iOS < 14.
 */
int32_t pso_archive_init(void);

/*
 * Release the archive and reset state. Idempotent.
 */
void pso_archive_shutdown(void);

/* --- Lookup --- */

/*
 * Look up a pre-compiled render PSO from the binary archive.
 *
 * descriptor: void* pointing to MTLRenderPipelineDescriptor.
 * Sets descriptor.binaryArchives = @[s_archive] so Metal checks the
 * archive first. On archive hit, returns the pre-compiled PSO
 * (id<MTLRenderPipelineState> as void*). On archive miss, clears
 * descriptor.binaryArchives and returns NULL (caller should fall back
 * to runtime compile).
 *
 * Returns NULL if no archive is loaded or descriptor is NULL.
 */
void *pso_archive_lookup_render(void *descriptor);

/*
 * Look up a pre-compiled compute PSO from the binary archive.
 *
 * descriptor: void* pointing to MTLComputePipelineDescriptor.
 * Same pattern as pso_archive_lookup_render but for compute PSOs.
 *
 * Returns NULL if no archive is loaded or descriptor is NULL.
 */
void *pso_archive_lookup_compute(void *descriptor);

/*
 * Returns 1 if archive is loaded and available, 0 otherwise.
 */
uint32_t pso_archive_is_available(void);

/*
 * Set binaryArchives on a render pipeline descriptor if an archive
 * is loaded. No-op if no archive is available. This is the
 * integration point: callers set this on their descriptor BEFORE
 * calling [device newRenderPipelineStateWithDescriptor:error:] so
 * Metal checks the shipped archive before runtime compilation.
 *
 * descriptor: void* pointing to MTLRenderPipelineDescriptor.
 */
void pso_archive_set_on_descriptor(void *descriptor);

#ifdef __cplusplus
}
#endif

#endif /* PSO_ARCHIVE_H */
