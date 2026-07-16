/*
 *  nqd_accel.h - Metal compute acceleration for Native QuickDraw operations
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef NQD_ACCEL_H
#define NQD_ACCEL_H

#include "sysdeps.h"

// ---------------------------------------------------------------------------
// accl_params field offsets — duplicated from video_defs.h to avoid
// including video_defs.h → macos_util.h from .mm files, which would
// conflict with MacTypes.h pulled in by <Metal/Metal.h> (Point, Rect,
// noErr, ProcPtr redefinitions).
// These MUST stay in sync with video_defs.h.
// ---------------------------------------------------------------------------
enum {
    NQD_acclTransferMode   = 0x00c,
    NQD_acclPenMode        = 0x010,
    NQD_acclForePen        = 0x01c,
    NQD_acclBackPen        = 0x020,
    NQD_acclSrcBaseAddr    = 0x030,
    NQD_acclSrcRowBytes    = 0x034,
    NQD_acclSrcBoundsRect  = 0x038,
    NQD_acclSrcPixelSize   = 0x048,
    NQD_acclDestBaseAddr   = 0x064,
    NQD_acclDestRowBytes   = 0x068,
    NQD_acclDestBoundsRect = 0x06c,
    NQD_acclDestPixelSize  = 0x07c,
    NQD_acclSrcRect        = 0x0cc,
    NQD_acclDestRect       = 0x0d4,
    NQD_acclDrawProc       = 0x174,

    // Mask-related offsets
    NQD_acclMaskBits       = 0x018,   // mask-related field (flags/indicator)
    NQD_acclMaskAddr       = 0x128,   // mask region pointer
    NQD_acclMaskExtra      = 0x130,   // additional mask field
};

// Metal compute acceleration state
extern bool nqd_metal_available;    // true after successful NQDMetalInit()
#include "accel_logging.h"
#if ACCEL_LOGGING_ENABLED
extern bool nqd_logging_enabled;    // toggle for NQD_LOG diagnostic output (default true while debugging)
#else
static constexpr bool nqd_logging_enabled = false;
#endif

// Initialize Metal compute infrastructure: device, command queue, shader pipelines,
// shared RAM buffer. Sets nqd_metal_available = true on success.
extern void NQDMetalInit(void);

// Release Metal resources and set nqd_metal_available = false.
extern void NQDMetalCleanup(void);

// Flush any pending batched NQD compute dispatches. Called at sync points
// (NQD_sync_hook) and frame boundaries (MetalCompositorPresent). Safe to
// call when no batch is pending (no-op).
extern void NQDMetalFlush(void);

// Metal-accelerated NQD operations. Each reads accl_params from Mac memory at address p.
extern void NQDMetalBitblt(uint32 p);
extern void NQDMetalFillRect(uint32 p);
extern void NQDMetalInvertRect(uint32 p);
extern void NQDMetalBltMask(uint32 p);
extern void NQDMetalFillMask(uint32 p);

// Check if a Mac address is within the Metal-mapped RAM region.
// Used by callers to decide whether Metal acceleration is safe.
extern bool NQDMetalAddrInBuffer(uint32 mac_addr);

// Hook-side probe: true when the bitblt accl_params at p describe src/dest
// rects that overlap on the same surface. NQD_bitblt_hook uses this to
// decline overlap families the flat GPU dispatch cannot order (packed-depth
// Boolean, arithmetic/hilite) to software QuickDraw; standard-depth Boolean
// overlaps stay accepted (NQDMetalBitblt diverts them to an ordered CPU
// scratch path).
extern bool NQDMetalBitbltSameSurfaceOverlap(uint32 p);

// ---------------------------------------------------------------------------
// 1:1 bitblt (DSpBlit_Fastest, sub-op 711).
//
// Strict 1:1 copy (srcRect == dstRect) reusing the proven nqd_bitblt kernel.
// src_base/dst_base are Mac (guest) addresses of the src/dst rect origins.
// transfer_mode is 0 (srcCopy) or 36 (transparent, for SrcKey with src_key as
// the transparent value). Rejects out-of-buffer baseAddr before dispatch.
// Returns true if encoded into the shared NQD batch; false otherwise (caller
// maps false -> kDSpInternalErr). NO new concurrency primitive.
extern bool NQDMetalBitblt1to1(uint32 src_base, int32 src_row_bytes,
                               uint32 dst_base, int32 dst_row_bytes,
                               uint32 pixel_size_bytes, uint32 bits_per_pixel,
                               uint32 width_pixels, uint32 height,
                               uint32 transfer_mode, uint32 src_key);

// ---------------------------------------------------------------------------
// Scaling bitblt (DSpBlit_Faster, sub-op 710).
//
// Dispatches the nqd_bitblt_scaled compute kernel (one thread per dst pixel;
// nearest-neighbor by default, bilinear when interpolate != 0; color-key
// honored per key_enable). src_base/dst_base are Mac (guest) addresses of the
// src/dst rect origins (the caller folds the rect top/left into the base).
// Rejects out-of-buffer baseAddr before dispatch. Returns true if the dispatch
// was encoded into the shared NQD batch; false on unavailable Metal, degenerate
// geometry, unsupported pixel size, or OOB address (caller maps false ->
// kDSpInternalErr). NO new concurrency primitive — encodes into the single
// shared MTLCommandQueue.
extern bool NQDMetalBitbltScaled(uint32 src_base, int32 src_row_bytes,
                                 uint32 dst_base, int32 dst_row_bytes,
                                 uint32 pixel_size_bytes, uint32 bits_per_pixel,
                                 uint32 src_w, uint32 src_h,
                                 uint32 dst_w, uint32 dst_h,
                                 uint32 interpolate,
                                 uint32 src_key, uint32 dst_key,
                                 uint32 key_enable);

#endif /* NQD_ACCEL_H */
