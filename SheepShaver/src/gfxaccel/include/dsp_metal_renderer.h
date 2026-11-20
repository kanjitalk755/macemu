/*
 *  dsp_metal_renderer.h - DSp Metal back-buffer allocation.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  C-callable interface. ObjC types are passed via void*; the
 *  implementation lives in dsp_metal_renderer.mm. Pattern analog:
 *  rave_metal_renderer.h. Every DSp back-buffer uses MTLStorageModeShared,
 *  allocated through the DSp resource heap. SwapBuffers publishes those
 *  bytes through DSpCopyBackBufferToCanonicalScreen; this interface does not
 *  expose an alternate presentation surface.
 */

#ifndef DSP_METAL_RENDERER_H
#define DSP_METAL_RENDERER_H

#include <stdint.h>
#include <stdbool.h>

struct DSpContextPrivate;

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  Allocate the back-buffer MTLBuffer + MTLTexture view through the
 *  bump sub-allocator on kHeapCompositor. Both are MTLStorageModeShared.
 *  Texture is a VIEW over the buffer memory - no separate heap allocation.
 *
 *  Returns true on success (ctx->back_buffer / back_texture populated
 *  and retained). On failure, ctx state is NOT modified - caller is
 *  responsible for error return to emulated PPC.
 *
 *  bpp must be one of {8, 16, 32}. 1/2/4 bpp is out of scope here.
 */
extern bool DSpAllocateBackBuffer(struct DSpContextPrivate *ctx,
                                   uint32_t w, uint32_t h, uint32_t bpp);

/*
 *  Synchronous release - texture FIRST, buffer SECOND (iOS ARC
 *  view-before-backing invariant). Caller must ensure no frame is in
 *  flight; the normal Release path uses the VBL-bounded queue via
 *  DSpQueueReleaseAtVBL in dsp_draw_context.mm.
 */
extern void DSpReleaseBackBufferNow(struct DSpContextPrivate *ctx);
extern void DSpReleaseBackBufferStaging(struct DSpContextPrivate *ctx);

/*
 *  Emit a CGrafPort-shaped struct into emulated Mac RAM for
 *  GetBackBuffer vending (stable-pointer-within-mode contract).
 *  Allocates once per context via SheepMem::Reserve; subsequent calls
 *  return the cached Mac address. Populates baseAddr, rowBytes, bounds,
 *  pixelType, pixelSize, cmpCount, cmpSize per the emulated app's
 *  expectations for its depth.
 *
 *  baseAddr uses Host2MacAddr((uint8 *)ctx->back_buffer.contents) when
 *  the contents pointer falls inside the vm_alloc emulated-RAM region;
	 *  otherwise (the heap lives outside that region on arm64 iOS) the
	 *  function reserves a separate Mac system-heap staging region the same
	 *  size as the back-buffer, validates the full guest-RAM span, and
	 *  SwapBuffers memcpys staging -> back_buffer before publishing to the
	 *  canonical screen. The fallback path preserves
 *  guest-writable CGrafPtr semantics.
 *
 *  Returns the CGrafPort's Mac address, or 0 on failure (caller returns
 *  kDSpInternalErr).
 */
extern uint32_t DSpGetBackBufferCGrafPtr(struct DSpContextPrivate *ctx);

/*
 *  Always-on overrun guard for guest-RAM pixel-staging writes. Looks up the
 *  staging block backing `mac_addr` and clamps `size` to the block's TRUE
 *  Mac_sysalloc allocation (never the grown logical size), logging loudly if
 *  the caller requested more than was allocated. Returns the safe byte count
 *  to pass to memcpy/memset. `site` is a short tag identifying the call site
 *  in the diagnostic log. A return < `size` means an overrun was prevented -
 *  the smoking gun for guest-heap corruption from a mis-sized staging write.
 */
extern uint32_t DSpGuardStagingWrite(uint32_t mac_addr, uint32_t size,
                                      const char *site);

#ifdef __cplusplus
}
#endif

#endif /* DSP_METAL_RENDERER_H */
