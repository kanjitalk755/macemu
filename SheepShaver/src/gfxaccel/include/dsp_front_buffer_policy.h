/*
 *  dsp_front_buffer_policy.h - DrawSprocket front-buffer metadata helpers.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 */

#ifndef DSP_FRONT_BUFFER_POLICY_H
#define DSP_FRONT_BUFFER_POLICY_H

#include <stdint.h>

#include "dsp_display_mode_policy.h"
#include "dsp_pixmap_offsets.h"

static inline uint32_t DSpFrontBufferDepth(uint32_t back_buffer_depth,
                                           uint32_t display_depth)
{
	return DSpDisplayModeDepth(back_buffer_depth, display_depth);
}

static inline bool DSpShouldReuseBackBufferCGrafPtrForFrontBuffer(
	uint32_t back_buffer_depth,
	uint32_t display_depth)
{
	/* The front buffer is the visible screen; the back buffer is the draw
	 * target. They must NOT alias, or the guest's double-buffering / page
	 * arithmetic desyncs (observed as a half-buffer roll in Diablo II's
	 * splash). Always vend a distinct front buffer — never reuse the back
	 * buffer's CGrafPtr. (Reverts the prior same-depth front==back reuse,
	 * which did not fix the RAVE crash it targeted and collapsed double
	 * buffering.) */
	(void)back_buffer_depth;
	(void)display_depth;
	return false;
}

static inline uint32_t DSpFrontBufferRowBytes(uint32_t width,
                                              uint32_t back_buffer_depth,
	uint32_t display_depth)
{
	return DSpDisplayModePitch(
		width,
		DSpFrontBufferDepth(back_buffer_depth, display_depth));
}

static inline bool DSpShouldPresentFrontBufferStaging(uint32_t back_buffer_depth,
                                                      uint32_t display_depth,
                                                      uint32_t front_staging_mac_addr,
                                                      uint32_t front_staging_size)
{
	(void)back_buffer_depth;
	(void)display_depth;
	return front_staging_mac_addr != 0 &&
	       front_staging_size != 0;
}

static inline bool DSpShouldPresentFrontBufferStagingForState(
	uint32_t back_buffer_depth,
	uint32_t display_depth,
	uint32_t front_staging_mac_addr,
	uint32_t front_staging_size,
	uint32_t context_state,
	uint32_t active_state)
{
	return context_state == active_state &&
	       DSpShouldPresentFrontBufferStaging(back_buffer_depth,
	                                          display_depth,
	                                          front_staging_mac_addr,
	                                          front_staging_size);
}

static inline bool DSpShouldPresentFrontBufferStagingForSwap(
	uint32_t back_buffer_depth,
	uint32_t display_depth,
	uint32_t front_staging_mac_addr,
	uint32_t front_staging_size,
	uint32_t context_state,
	uint32_t active_state)
{
	(void)back_buffer_depth;
	(void)display_depth;
	(void)front_staging_mac_addr;
	(void)front_staging_size;
	(void)context_state;
	(void)active_state;
	return false;
}

static inline bool DSpShouldSeedFrontBufferStagingFromBackStaging(
	uint32_t back_buffer_depth,
	uint32_t front_depth,
	uint32_t back_staging_mac_addr,
	uint32_t back_staging_size,
	uint32_t back_staging_row_bytes,
	uint32_t front_staging_size,
	uint32_t front_staging_row_bytes)
{
	return back_buffer_depth != 0 &&
	       front_depth == back_buffer_depth &&
	       back_staging_mac_addr != 0 &&
	       front_staging_size != 0 &&
	       back_staging_size >= front_staging_size &&
	       back_staging_row_bytes == front_staging_row_bytes;
}

static inline bool DSpShouldRefreshFrontBufferStagingFromBackStaging(
	uint32_t back_buffer_depth,
	uint32_t front_depth,
	uint32_t back_staging_mac_addr,
	uint32_t front_staging_mac_addr,
	uint32_t back_staging_size,
	uint32_t front_staging_size,
	uint32_t back_staging_row_bytes,
	uint32_t front_staging_row_bytes,
	uint32_t swap_generation,
	uint32_t front_staging_refresh_swap_generation,
	bool front_staging_geometry_changed)
{
	/* Only refresh when a SwapBuffers actually delivered a new visible
	 * frame since the last copy, or the staging is being re-vended with a
	 * new geometry (mode switch reusing the old allocation — its pixels
	 * have the wrong pitch and must be replaced). The front staging is
	 * the guest's live screen surface — front-buffer-direct clients
	 * (never swap) draw 2D straight into it, and an unconditional
	 * refresh replays the stale back-staging snapshot over their work on
	 * every GetFrontBuffer reassert (The Sims: UI panels flicker to
	 * black). Swap-driven and mixed clients still get one refresh per
	 * swap, which matches real hardware: the swap replaces the visible
	 * frame, front-buffer draws made after it persist until the next
	 * swap. */
	return front_staging_mac_addr != 0 &&
	       (swap_generation != front_staging_refresh_swap_generation ||
	        front_staging_geometry_changed) &&
	       DSpShouldSeedFrontBufferStagingFromBackStaging(
	           back_buffer_depth,
	           front_depth,
	           back_staging_mac_addr,
	           back_staging_size,
	           back_staging_row_bytes,
	           front_staging_size,
	           front_staging_row_bytes);
}

static inline uint32_t DSpFrontBufferPixMapRecordSize(void)
{
	return (uint32_t)DSP_MAINDEVICE_PIXMAP_SIZE;
}

static inline uint16_t DSpFrontBufferPixMapRowBytesField(uint32_t row_bytes)
{
	return (uint16_t)(0x8000u | (row_bytes & 0x3FFFu));
}

#endif /* DSP_FRONT_BUFFER_POLICY_H */
