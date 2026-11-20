/*
 *  gfx_log.h - Portable logging for gfxaccel (os_log on Apple, fprintf elsewhere)
 */

#ifndef GFX_LOG_H
#define GFX_LOG_H
#include "sysdeps.h"
#include "debug.h"

#ifdef __APPLE__
#include <os/log.h>
#define GFX_OS_LOG_AVAILABLE 1
#else
#define GFX_OS_LOG_AVAILABLE 0
#endif

#ifdef __cplusplus

#include <stdarg.h>

#ifndef ACCEL_LOGGING_ENABLED
#define ACCEL_LOGGING_ENABLED 0
#endif
#if ACCEL_LOGGING_ENABLED

#include <stdlib.h>
#include <string.h>


/* True if GFXACCEL_LOG is unset, "all", or a comma list containing `name`.
 * Unset => all-on, preserving the legacy "compile flag on => logs on".
 * Each subsystem calls this to initialise its gate bool at definition,
 * which is order-independent (no static-init ordering hazards). */
inline bool accel_log_subsystem_on(const char *name) {
    const char *env = getenv("GFXACCEL_LOG");
    if (!env || !*env) return true;
    /* Comma-wrap the value and the name and substring-search, using C
     * strings only so this header never drags <string> into every TU. */
    char hay[256];
    snprintf(hay, sizeof(hay), ",%s,", env);
    if (strstr(hay, ",all,")) return true;
    char needle[40];
    snprintf(needle, sizeof(needle), ",%s,", name);
    return strstr(hay, needle) != NULL;
}

/* True if GFXACCEL_LOG_VERBOSE is set to a truthy value (1/t/y). */
inline bool accel_log_verbose_env() {
    const char *e = getenv("GFXACCEL_LOG_VERBOSE");
    if (!e || !*e) return false;
    return e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y';
}

/* Shared verbose flag, read once from the environment on first use. The
 * function-local static is a single instance across all TUs, so no
 * per-subsystem wiring is needed. It is initialised on the first call, which
 * happens on the emul thread before any other thread logs. */
inline bool accel_log_verbose() {
    static bool v = accel_log_verbose_env(); return v; }

#define ACCEL_LOG_VERBOSE true//(accel_log_verbose())
#define GFX_DEBUG_EMIT(prefix, ...) gfx_log_emit((prefix), __VA_ARGS__)
#else  /* !ACCEL_LOGGING_ENABLED */
#define ACCEL_LOG_VERBOSE false
#define GFX_DEBUG_EMIT(prefix, ...) gfx_log_emit((prefix), __VA_ARGS__)
#endif

static inline void gfx_log_emitv(const char *prefix,
    const char *format, va_list args)
{
    char body[2048];
    vsnprintf(body, sizeof(body), format, args);
    body[sizeof(body) - 1] = '\0';

    char record[2304];
    snprintf(record, sizeof(record), "%s%s\n",
                  prefix ? prefix : "", body);
    record[sizeof(record) - 1] = '\0';

    bug("%s", record);
}

static inline void gfx_log_emit(const char *prefix, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    gfx_log_emitv(prefix, format, args);
    va_end(args);
}

#endif /* __cplusplus */

#define GFX_FPRINTF_LOG(tag, fmt, ...) \
	gfx_log_emit("", "[%s] " fmt, (tag), ##__VA_ARGS__)

#define GFX_FPRINTF_ERR(tag, fmt, ...) \
	gfx_log_emit("", "[%s ERROR] " fmt, (tag), ##__VA_ARGS__)

#ifndef QD3D_INIT_LOGGING_ENABLED
#define QD3D_INIT_LOGGING_ENABLED 0
#endif

#ifndef QD3D_GRAPHICS_LOGGING_ENABLED
#define QD3D_GRAPHICS_LOGGING_ENABLED 1
#endif

#ifndef QD3D_AUDIO_LOGGING_ENABLED
#define QD3D_AUDIO_LOGGING_ENABLED 0
#endif

#ifndef QD3D_MEDIA_LOGGING_ENABLED
#define QD3D_MEDIA_LOGGING_ENABLED 0
#endif

#ifndef QD3D_WAIT_LOGGING_ENABLED
#define QD3D_WAIT_LOGGING_ENABLED 1
#endif

#if QD3D_INIT_LOGGING_ENABLED || QD3D_GRAPHICS_LOGGING_ENABLED || \
    QD3D_AUDIO_LOGGING_ENABLED || QD3D_MEDIA_LOGGING_ENABLED || \
    QD3D_WAIT_LOGGING_ENABLED

static inline void qd3d_log(const char *category, const char *file, int line,
                       const char *format, ...)
{
	char prefix[512];
	char message[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	message[sizeof(message) - 1] = '\0';

	snprintf(prefix, sizeof(prefix), "[QD3D:%s] %s:%d: ",
	              category, file, line);
	prefix[sizeof(prefix) - 1] = '\0';
	gfx_log_emit(prefix, "%s", message);
}

#if QD3D_AUDIO_LOGGING_ENABLED
#define QD3D_AUDIO_LOG(...) \
	qd3d_log("audio", __FILE__, __LINE__, __VA_ARGS__)
#else
#define QD3D_AUDIO_LOG(...) do { } while (0)
#endif

#if QD3D_MEDIA_LOGGING_ENABLED
#define QD3D_MEDIA_LOG(...) \
	qd3d_log("media", __FILE__, __LINE__, __VA_ARGS__)
#else
#define QD3D_MEDIA_LOG(...) do { } while (0)
#endif

#if QD3D_WAIT_LOGGING_ENABLED
#define QD3D_WAIT_LOG(...) \
	qd3d_log("wait", __FILE__, __LINE__, __VA_ARGS__)
#else
#define QD3D_WAIT_LOG(...) do { } while (0)
#endif

#if QD3D_INIT_LOGGING_ENABLED

#define QD3D_INIT_LOG(...) \
	qd3d_log("init", __FILE__, __LINE__, __VA_ARGS__)

#else

#define QD3D_INIT_LOG(...) do { } while (0)

#endif

#if QD3D_GRAPHICS_LOGGING_ENABLED

#define QD3D_STATE_LOG(...) \
	qd3d_log("state", __FILE__, __LINE__, __VA_ARGS__)
#define QD3D_RESOURCE_LOG(...) \
	qd3d_log("resource", __FILE__, __LINE__, __VA_ARGS__)
#define QD3D_RENDER_LOG(...) \
	qd3d_log("render", __FILE__, __LINE__, __VA_ARGS__)

#else

#define QD3D_STATE_LOG(...) do { } while (0)
#define QD3D_RESOURCE_LOG(...) do { } while (0)
#define QD3D_RENDER_LOG(...) do { } while (0)

#endif

#else

#define QD3D_INIT_LOG(...) do { } while (0)
#define QD3D_STATE_LOG(...) do { } while (0)
#define QD3D_RESOURCE_LOG(...) do { } while (0)
#define QD3D_RENDER_LOG(...) do { } while (0)
#define QD3D_AUDIO_LOG(...) do { } while (0)
#define QD3D_MEDIA_LOG(...) do { } while (0)
#define QD3D_WAIT_LOG(...) do { } while (0)

#endif

#endif /* GFX_LOG_H */
