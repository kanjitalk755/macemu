/*
 *  gl_device.h - Shared OpenGL device / SDL context for gfxaccel
 */

#ifndef GFXACCEL_GL_DEVICE_H
#define GFXACCEL_GL_DEVICE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create (or re-create) an OpenGL context on the global SDL window.
 * Safe to call multiple times. Returns true on success. */
bool GfxGLDeviceInit(void);

/* Make the gfxaccel GL context current on this thread. */
bool GfxGLDeviceMakeCurrent(void);

/* Unbind the GL context from the calling thread (so another thread can bind). */
void GfxGLDeviceReleaseCurrent(void);

/* Release the gfxaccel GL context. Idempotent. */
void GfxGLDeviceShutdown(void);

/* True if GfxGLDeviceInit succeeded. */
bool GfxGLDeviceIsReady(void);

/* Swap the SDL window's OpenGL buffers (present). */
void GfxGLDeviceSwap(void);

/* Query window drawable size in pixels. */
void GfxGLDeviceGetDrawableSize(int *out_w, int *out_h);

/*
 * Compatibility shims matching metal_device_shared.h signatures so engine
 * code can keep calling SharedMetalDevice / SharedMetalCommandQueue.
 * On OpenGL these return non-NULL sentinel pointers when the device is ready.
 */
void *SharedMetalDevice(void);
void *SharedMetalCommandQueue(void);
void MetalValidation_InstallErrorHandler(void *cmdBufPtr);

#ifdef __cplusplus
}
#endif

#endif /* GFXACCEL_GL_DEVICE_H */
