/*
 *  dsp_alt_buffer.h - DrawSprocket AltBuffer subsystem (sub-ops 700-705)
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Extracted from dsp_draw_context.mm (de-bloat, no behaviour change). The
 *  alt-buffer record table, lifecycle helpers, and the production handlers
 *  (DSpAltBuffer_New/Dispose/GetCGrafPtr/InvalRect + underlay Get/Set) all
 *  live in dsp_alt_buffer.mm.
 *
 *  Only the cross-translation-unit surface used by the core present /
 *  underlay-restore path (DSpRestoreBackBufferFromUnderlay, still in
 *  dsp_draw_context.mm) is declared here. The record table itself stays
 *  file-private to dsp_alt_buffer.mm.
 */

#ifndef DSP_ALT_BUFFER_H
#define DSP_ALT_BUFFER_H

#include <cstdint>

/* DSP_MAX_ALT_BUFFERS=16: two per context worst case (1 underlay + 1 staging)
 * across the 8-context table; classic sprite games use exactly one underlay
 * (the documented use case). */
#define DSP_MAX_ALT_BUFFERS 16

#ifdef __OBJC__
#import <Metal/Metal.h>

struct DSpAltBufferRecord {
	bool                  in_use;
	id<MTLBuffer>         backing;             /* DSp-heap MTLBuffer (BGRA8Unorm-viewable) */
	id<MTLTexture>        texture;             /* BGRA8Unorm view for compositor routing */
	uint32_t              cgrafptr_mac_addr;   /* cached guest CGrafPort (0 until first GetCGrafPtr) */
	uint32_t              baseaddr_mac;        /* guest-RAM staging baseAddr backing the CGrafPort */
	uint32_t              baseaddr_size;       /* byte size of the pixel-staging block (for teardown release) */
	bool                  baseaddr_owned_staging; /* true => baseaddr_mac is a DSpReserveGuestPixelStaging block to quarantine on teardown */
	uint32_t              width;
	uint32_t              height;
	uint32_t              options;             /* DSpAltBufferOption bits */
	bool                  underlay_capable;    /* NULL inAttributes => true (PDF p.49) */
	/* Dirty-rect union — same fields/semantics as DSpContextPrivate's
	 * dirty_*; single-writer, no primitive. */
	int16_t               dirty_left, dirty_top, dirty_right, dirty_bottom;
	bool                  dirty_empty;
};

/* Resolve a 1-based alt-buffer handle to its record (nullptr if invalid or not
 * in use). Mirrors DSpGetContext. */
DSpAltBufferRecord *DSpGetAltBuffer(uint32_t handle);

/* Mirror an alt-buffer's guest-RAM staging (where the guest draws through the
 * GetCGrafPtr CGrafPort) into its Metal backing, so the CPU
 * underlay-restore copy reads the guest's pixels. No-op on StorageModePrivate
 * (simulator) or when the alt-buffer has no owned guest staging. */
void DSpSyncAltBufferStagingToBacking(DSpAltBufferRecord *rec);
#endif /* __OBJC__ */

/* Free every in-use alt-buffer record. Used by the test-harness context reset
 * so each case starts with an empty table. Safe to call from non-ObjC TUs. */
void DSpResetAltBufferTable(void);

/* Background/foreground Metal-backing lifecycle (D-4-1 heap ratchet fix).
 * Release drops every in-use record's backing+texture (guest staging, dims,
 * and in_use survive) so the DSp bump heap can reach live==0 and reset while
 * backgrounded; restore re-allocates and repopulates from guest staging.
 * Called from DSpHandleBackgroundFromEmulThread / ...Foreground... only.
 * Safe to call from non-ObjC TUs. */
void DSpReleaseAltBufferBackingsForBackground(void);
void DSpRestoreAltBufferBackingsForForeground(void);

#endif /* DSP_ALT_BUFFER_H */
