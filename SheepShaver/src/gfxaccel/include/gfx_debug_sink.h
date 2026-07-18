/*
 *  gfx_debug_sink.h - One shared diagnostic output sink for macemu
 *
 *  Every logging family in this tree (NQD/RAVE/GL/DSp/COMPOSITOR/CINEPAK/DMC
 *  on the accel side, QD3D_* on the init/audio/media/wait side, and the ad-hoc
 *  fprintf loggers) funnels its already-formatted line through gfx_debug_emit()
 *  so a single call reaches BOTH:
 *    - the console / redirected stderr, and
 *    - the Windows debugger (Visual Studio Output, CDB/WinDbg, DebugView) via
 *      OutputDebugStringA — GUI builds have no console otherwise.
 *
 *  IMPORTANT: this sink is ALWAYS compiled in. It is never behind a logging
 *  #if. Whether a given line is produced is decided by the per-subsystem gate
 *  macros that call it; the emit function itself always exists so any caller,
 *  gated or not, can reach it. Keep it self-contained (only <cstdio>/<cstdarg>
 *  and, on Windows, <windows.h>) so it is safe to include from any TU.
 */

#ifndef GFX_DEBUG_SINK_H
#define GFX_DEBUG_SINK_H

#ifdef __cplusplus

#include <cstdio>
#include <cstdarg>

#if defined(_WIN32)
/* Declare OutputDebugStringA without dragging in <windows.h> (which pulls
 * winsock.h and clashes with winsock2.h in networking TUs). Matches the WinAPI
 * signature exactly; extern "C" so it binds to the same import. */
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char *lpOutputString);
#endif

namespace gfx_debug {

/* Emit one diagnostic line. `prefix` is a short tag written verbatim before the
 * formatted body (e.g. "[nqd] ", "CINEPAK: "); pass "" for none. A trailing
 * newline is appended. Goes to stderr (flushed) and, on Windows, also to the
 * debugger output stream. Never conditionally compiled. */
inline void emitv(const char *prefix, const char *format, va_list args)
{
    char body[2048];
    std::vsnprintf(body, sizeof(body), format, args);
    body[sizeof(body) - 1] = '\0';

    char record[2304];
    std::snprintf(record, sizeof(record), "%s%s\n",
                  prefix ? prefix : "", body);
    record[sizeof(record) - 1] = '\0';

    std::fputs(record, stderr);
    std::fflush(stderr);
#if defined(_WIN32)
    OutputDebugStringA(record);
#endif
}

inline void emit(const char *prefix, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    emitv(prefix, format, args);
    va_end(args);
}

} /* namespace gfx_debug */

/* Function-style helper so existing "fprintf(stderr, TAG fmt "\n", ...)" call
 * sites convert to "GFX_DEBUG_EMIT(TAG, fmt, ...)" with minimal edits. */
#define GFX_DEBUG_EMIT(prefix, ...) ::gfx_debug::emit((prefix), __VA_ARGS__)

#endif /* __cplusplus */

#endif /* GFX_DEBUG_SINK_H */
