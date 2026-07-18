/*
 *  gfx_log.h - Portable logging for gfxaccel (os_log on Apple, fprintf elsewhere)
 *
 *  (C) 2026 OpenGL+SDL port
 */

#ifndef GFX_LOG_H
#define GFX_LOG_H

#include "accel_logging.h"
#include "gfx_debug_sink.h"
#include <stdio.h>

#ifdef __APPLE__
#include <os/log.h>
#define GFX_OS_LOG_AVAILABLE 1
#else
#define GFX_OS_LOG_AVAILABLE 0
#endif

/* Generic logger (runtime `tag`) routed through the shared stderr +
 * OutputDebugStringA sink. The tag is dynamic, so it is folded into the body
 * with an empty prefix. */
#define GFX_FPRINTF_LOG(tag, fmt, ...) \
	::gfx_debug::emit("", "[%s] " fmt, (tag), ##__VA_ARGS__)

#define GFX_FPRINTF_ERR(tag, fmt, ...) \
	::gfx_debug::emit("", "[%s ERROR] " fmt, (tag), ##__VA_ARGS__)

#endif /* GFX_LOG_H */
