/*
 *  rave_metal_renderer.h - Metal rendering infrastructure for RAVE
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  C++ callable interface for Metal renderer. No ObjC types exposed --
 *  all Metal objects live inside the opaque RaveMetalState struct defined
 *  in rave_metal_renderer.mm.
 */

#ifndef RAVE_METAL_RENDERER_H
#define RAVE_METAL_RENDERER_H

#include <stdint.h>

struct RaveDrawPrivate;

// Overlay lifecycle (per-engine ownership via gfxaccel_resources;
// no longer shared with GL, no refcount, no deferred-destroy).
extern void RaveCreateMetalOverlay(int32_t left, int32_t top, int32_t width, int32_t height);

// Fan-out hooks: small C-linkage probes so rave_engine.cpp's
// RaveOnAttach / RaveOnDetach handlers can interrogate / drive RAVE's
// per-engine overlay state, including the preserved logical overlay binding
// used after DMC mode-exit detach, without needing direct ObjC++ access.
#ifdef __cplusplus
extern "C" {
#endif
int  rave_has_active_overlay(void);
int  rave_get_overlay_dims(uint32_t *outW, uint32_t *outH);
void rave_release_overlay_for_detach(void);
#ifdef __cplusplus
}
#endif

// Per-context Metal resource management
extern void RaveInitMetalResources(struct RaveDrawPrivate *priv);
extern void RaveReleaseMetalResources(struct RaveDrawPrivate *priv);

// Render method implementations (called from rave_dispatch.cpp)
extern int32_t NativeRenderStart(uint32_t drawContextAddr, uint32_t dirtyRectAddr, uint32_t initialContextAddr);
extern int32_t NativeRenderEnd(uint32_t drawContextAddr, uint32_t modifiedRectAddr);
extern int32_t NativeRenderAbort(uint32_t drawContextAddr);
extern int32_t NativeFlush(uint32_t drawContextAddr);
extern int32_t NativeSync(uint32_t drawContextAddr);

// Draw method implementations (called from rave_dispatch.cpp)
extern int32_t NativeDrawTriGouraud(uint32_t drawContextAddr, uint32_t v0, uint32_t v1,
                                     uint32_t v2, uint32_t flags);
extern int32_t NativeDrawVGouraud(uint32_t drawContextAddr, uint32_t nVertices,
                                   uint32_t vertexMode, uint32_t verticesAddr,
                                   uint32_t flagsAddr);
extern int32_t NativeSubmitVerticesGouraud(uint32_t drawContextAddr, uint32_t nVertices,
                                            uint32_t verticesAddr);
extern int32_t NativeDrawPoint(uint32_t drawContextAddr, uint32_t v0Addr);
extern int32_t NativeDrawLine(uint32_t drawContextAddr, uint32_t v0Addr, uint32_t v1Addr);

// Textured draw method implementations (called from rave_dispatch.cpp)
extern int32_t NativeDrawTriTexture(uint32_t drawContextAddr, uint32_t v0, uint32_t v1,
                                     uint32_t v2, uint32_t flags);
extern int32_t NativeDrawVTexture(uint32_t drawContextAddr, uint32_t nVertices,
                                   uint32_t vertexMode, uint32_t verticesAddr,
                                   uint32_t flagsAddr);
extern int32_t NativeSubmitVerticesTexture(uint32_t drawContextAddr, uint32_t nVertices,
                                            uint32_t verticesAddr);

// Multi-texture vertex submission (RAVE 1.6 extension)
extern int32_t NativeSubmitMultiTextureParams(uint32_t drawContextAddr,
    uint32_t nVertices, uint32_t multiTexParamsAddr);

// Bitmap draw method (2D sprite/HUD rendering)
extern int32_t NativeDrawBitmap(uint32_t drawContextAddr, uint32_t vertexAddr, uint32_t bitmapMacAddr);

// Indexed triangle mesh drawing (uses previously submitted vertices from staging buffer)
extern int32_t NativeDrawTriMeshGouraud(uint32_t drawContextAddr, uint32_t numTriangles, uint32_t trianglesAddr);
extern int32_t NativeDrawTriMeshTexture(uint32_t drawContextAddr, uint32_t numTriangles, uint32_t trianglesAddr);

// Notice method implementations (called from rave_dispatch.cpp)
extern int32_t NativeSetNoticeMethod(uint32_t drawContextAddr, uint32_t method,
                                      uint32_t callback, uint32_t refCon);
extern int32_t NativeGetNoticeMethod(uint32_t drawContextAddr, uint32_t method,
                                      uint32_t callbackOutPtr, uint32_t refConOutPtr);

// RAVE 1.6 buffer access (draw/Z buffer readback + writeback)
// SDK: AccessDrawBuffer(ctx, TQAPixelBuffer*)   -- TQAPixelBuffer = {rowBytes, pixelType, width, height, baseAddr}
// SDK: AccessZBuffer(ctx, TQAZBuffer*)           -- TQAZBuffer = {width, height, rowBytes, zbuffer, zDepth, isBigEndian}
extern int32_t NativeAccessDrawBuffer(uint32_t drawContextAddr, uint32_t bufferStructAddr);
extern int32_t NativeAccessDrawBufferEnd(uint32_t drawContextAddr, uint32_t dirtyRectAddr);
extern int32_t NativeAccessZBuffer(uint32_t drawContextAddr, uint32_t bufferStructAddr);
extern int32_t NativeAccessZBufferEnd(uint32_t drawContextAddr, uint32_t dirtyRectAddr);

// RAVE 1.6 mid-frame buffer clear (sub-rect clear)
extern int32_t NativeClearDrawBuffer(uint32_t drawContextAddr, uint32_t rectAddr,
                                      uint32_t initialContextAddr);
extern int32_t NativeClearZBuffer(uint32_t drawContextAddr, uint32_t rectAddr, uint32_t initialContextAddr);

// RAVE 1.6 swap control, busy query, and render-to-texture (called from rave_dispatch.cpp)
extern int32_t NativeSwapBuffers(uint32_t drawContextAddr, uint32_t dirtyRectAddr);
extern int32_t NativeBusy(uint32_t drawContextAddr);
extern int32_t NativeTextureNewFromDrawContext(uint32_t drawContextAddr, uint32_t flags, uint32_t newTexturePtr);
extern int32_t NativeBitmapNewFromDrawContext(uint32_t drawContextAddr, uint32_t flags, uint32_t newBitmapPtr);

// Metal texture creation/upload functions (called from rave_engine.cpp via void* bridge)
extern void *RaveCreateMetalTexture(uint32_t width, uint32_t height, uint32_t mipLevels,
                                     const uint8_t *pixelData, uint32_t bytesPerRow);
// Copies pixelData into a Metal staging buffer before returning, then uploads
// asynchronously via the current RAVE command queue when available.
extern void RaveUploadMipLevel(void *metalTexture, uint32_t level, uint32_t width, uint32_t height,
                                const uint8_t *pixelData, uint32_t bytesPerRow);
extern void RaveGenerateMipmaps(void *metalTexture);
extern void RaveReleaseTexture(void *metalTexture);

#endif /* RAVE_METAL_RENDERER_H */
