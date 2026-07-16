/*
 *  gfx_log.h - Portable logging for gfxaccel (os_log on Apple, fprintf elsewhere)
 *
 *  (C) 2026 OpenGL+SDL port
 */

#ifndef GFX_LOG_H
#define GFX_LOG_H

#include "accel_logging.h"
#include <stdio.h>

#ifdef __APPLE__
#include <os/log.h>
#define GFX_OS_LOG_AVAILABLE 1
#else
#define GFX_OS_LOG_AVAILABLE 0
#endif

/* Generic fprintf-backed logger used by OpenGL backends and non-Apple builds. */
#define GFX_FPRINTF_LOG(tag, fmt, ...) \
	do { fprintf(stderr, "[%s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)

#define GFX_FPRINTF_ERR(tag, fmt, ...) \
	do { fprintf(stderr, "[%s ERROR] " fmt "\n", tag, ##__VA_ARGS__); } while (0)

#endif /* GFX_LOG_H */
