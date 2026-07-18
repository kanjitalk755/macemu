/*
 *  gfx_log.h - Portable logging for gfxaccel (os_log on Apple, fprintf elsewhere)
 */

#ifndef GFX_LOG_H
#define GFX_LOG_H

#include <stdio.h>

#ifdef __APPLE__
#include <os/log.h>
#define GFX_OS_LOG_AVAILABLE 1
#else
#define GFX_OS_LOG_AVAILABLE 0
#endif

#ifdef __cplusplus

#include <stdarg.h>

#if defined(_WIN32)
/* Declare OutputDebugStringA without dragging in <windows.h> (which pulls
 * winsock.h and clashes with winsock2.h in networking TUs). Matches the WinAPI
 * signature exactly; extern "C" so it binds to the same import. */
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char *lpOutputString);
#endif


#ifndef ACCEL_LOGGING_ENABLED
#define ACCEL_LOGGING_ENABLED 0   /* ship default OFF */
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
    /* Comma-wrap the value and the name and substring-search. Uses C strings
     * only: pulling <string> drags in libc++ <atomic>, which conflicts with
     * <stdatomic.h> in some TUs (e.g. the compositor) before C++23. */
    char hay[256];
    snprintf(hay, sizeof(hay), ",%s,", env);
    if (strstr(hay, ",all,")) return true;
    char needle[40];
    snprintf(needle, sizeof(needle), ",%s,", name);
    return strstr(hay, needle) != nullptr;
}

/* True if GFXACCEL_LOG_VERBOSE is set to a truthy value (1/t/y). */
inline bool accel_log_verbose_env() {
    const char *e = getenv("GFXACCEL_LOG_VERBOSE");
    if (!e || !*e) return false;
    return e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y';
}

/* Shared verbose flag, read once from the environment on first use. The
 * thread-safe local static is a single instance across all TUs (C++14),
 * so no per-subsystem wiring is needed. */
inline bool accel_log_verbose() {
    static bool v = accel_log_verbose_env(); return v; }

#define ACCEL_LOG_VERBOSE (accel_log_verbose())

#else  /* !ACCEL_LOGGING_ENABLED */
#define ACCEL_LOG_VERBOSE false
#endif

/* Emit one diagnostic line. `prefix` is a short tag written verbatim before the
 * formatted body (e.g. "[nqd] ", "CINEPAK: "); pass "" for none. A trailing
 * newline is appended. Goes to stderr (flushed) and, on Windows, also to the
 * debugger output stream. Never conditionally compiled. */
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

    fputs(record, stderr);
    fflush(stderr);
#if defined(_WIN32)
    OutputDebugStringA(record);
#endif
}

static inline void gfx_log_emit(const char *prefix, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    gfx_log_emitv(prefix, format, args);
    va_end(args);
}

#define GFX_DEBUG_EMIT(prefix, ...) gfx_log_emit((prefix), __VA_ARGS__)

#endif /* __cplusplus */

/* Generic logger (runtime `tag`) routed through the shared stderr +
 * OutputDebugStringA sink. The tag is dynamic, so it is folded into the body
 * with an empty prefix. */
#define GFX_FPRINTF_LOG(tag, fmt, ...) \
	gfx_log_emit("", "[%s] " fmt, (tag), ##__VA_ARGS__)

#define GFX_FPRINTF_ERR(tag, fmt, ...) \
	gfx_log_emit("", "[%s ERROR] " fmt, (tag), ##__VA_ARGS__)

#ifndef QD3D_INIT_LOGGING_ENABLED
#define QD3D_INIT_LOGGING_ENABLED 0
#endif

#ifndef QD3D_GRAPHICS_LOGGING_ENABLED
#define QD3D_GRAPHICS_LOGGING_ENABLED 0
#endif

#ifndef QD3D_AUDIO_LOGGING_ENABLED
#define QD3D_AUDIO_LOGGING_ENABLED 0
#endif

#ifndef QD3D_MEDIA_LOGGING_ENABLED
#define QD3D_MEDIA_LOGGING_ENABLED 0
#endif

#ifndef QD3D_WAIT_LOGGING_ENABLED
#define QD3D_WAIT_LOGGING_ENABLED 0
#endif

/* Per-VIA-tick emu-thread wall-time accounting. PC sampling can't find diffuse
 * cost (the movie handoff is slow everywhere, not stuck at one PC). This brackets
 * each emu-thread consumer (EMUL_OP handlers incl. nested Execute68k, native
 * RAVE/GL/DSp dispatch, native Cinepak decode) and dumps the breakdown once per
 * guest tick, so a fat tick shows which bucket ate it and 'unacc' (= tick wall
 * minus accounted) exposes raw-interpreter grind. Enable for a targeted capture. */
#ifndef GFX_TICKPROF_ENABLED
#define GFX_TICKPROF_ENABLED 0
#endif

/* Descent II mid-movie SetDepth(16) video-hitch instrumentation (present
 * heartbeat, framebuffer-region hash, switch log, Cinepak blit probe, upload
 * gate probe). DEFAULT OFF - targeted debugging only. */
#ifndef DESCENT_HITCH_DEBUG
#define DESCENT_HITCH_DEBUG 0
#endif

#if QD3D_INIT_LOGGING_ENABLED || QD3D_GRAPHICS_LOGGING_ENABLED || \
    QD3D_AUDIO_LOGGING_ENABLED || QD3D_MEDIA_LOGGING_ENABLED || \
    QD3D_WAIT_LOGGING_ENABLED

static inline void qd3d_log(const char *category, const char *file, int line,
                       const char *format, ...)
{
	char message[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	message[sizeof(message) - 1] = '\0';

	/* Prefix carries the category + source location; body is the message.
	 * The shared sink appends the trailing newline and handles both outputs. */
	char prefix[512];
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
