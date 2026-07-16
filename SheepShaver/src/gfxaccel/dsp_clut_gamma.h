/*
 *  dsp_clut_gamma.h - DrawSprocket CLUT get/set (sub-ops 300/301).
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  CLUT get/set handlers (+ cores) were
 *  extracted from dsp_draw_context.mm into dsp_clut_gamma.mm (de-bloat).
 *  The gamma FADE subsystem stays in dsp_draw_context.mm (it is coupled to the
 *  VBL machinery + context-table walk). The fade handlers and Reserve
 *  colorTable path reuse the extracted cores/helpers declared here.
 */

#ifndef DSP_CLUT_GAMMA_H
#define DSP_CLUT_GAMMA_H

#include <cstdint>

struct DSpContextPrivate;  /* pointer-only use; full def in dsp_context_private.h */

/* Write (last-first+1)*3 RGB bytes into the context CLUT, pushing to the
 * compositor/DMC when the context is Active. Shared with Reserve colorTable
 * application (dsp_draw_context.mm). */
int32_t DSpSetCLUTCore(DSpContextPrivate *ctx,
                       uint32_t first, uint32_t last,
                       const uint8_t *entries_host_range);

/* Read (last-first+1)*3 RGB bytes of the context's latched CLUT into
 * entries_out_host_range. Used by the GetCLUTEntries handler
 * (dsp_clut_gamma.mm). */
int32_t DSpGetCLUTCore(DSpContextPrivate *ctx,
                       uint32_t first, uint32_t last,
                       uint8_t *entries_out_host_range);

/* Parse a guest parametric RGBColor at colorAddr into 8-bit R/G/B. Shared with
 * the fade handlers (which read the fade target colour). */
void DSpReadParametricColorFromGuest(uint32_t colorAddr,
                                     uint8_t *out_r,
                                     uint8_t *out_g,
                                     uint8_t *out_b);

#endif /* DSP_CLUT_GAMMA_H */
