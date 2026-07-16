/*
 *  metal_device_shared.h - Shared Metal device singleton interface
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  C++ callable interface for the shared Metal device singleton.
 *  Returns void* so this header can be included from plain .cpp files
 *  without pulling in ObjC types.  Callers cast to id<MTLDevice> /
 *  id<MTLCommandQueue> in .mm files.
 */

#ifndef METAL_DEVICE_SHARED_H
#define METAL_DEVICE_SHARED_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the shared id<MTLDevice> as void*.
 * Thread-safe (uses dispatch_once internally).
 * Returns NULL if no Metal GPU is available. */
void *SharedMetalDevice(void);

/* Returns the shared id<MTLCommandQueue> as void*.
 * Thread-safe (uses dispatch_once internally).
 * Implicitly creates the device if not yet initialised. */
void *SharedMetalCommandQueue(void);

/* Install debug-only command buffer error handler.
 * cmdBufPtr is void* pointing to id<MTLCommandBuffer>.
 * In DEBUG builds, adds a completedHandler that asserts on non-nil error.
 * In Release builds, this is a no-op. */
void MetalValidation_InstallErrorHandler(void *cmdBufPtr);


#ifdef __cplusplus
}
#endif

#endif /* METAL_DEVICE_SHARED_H */
