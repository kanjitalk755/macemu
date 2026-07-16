/*
 *  accel_logging.h - Compile-time + runtime control plane for accel logging
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Master compile-time switch for NQD, RAVE, GL, DSp, and compositor logging.
 *  When ACCEL_LOGGING_ENABLED is 1: all subsystem logging macros (NQD_LOG,
 *  RAVE_LOG, GL_LOG, GL_METAL_LOG, DSP_LOG, COMPOSITOR_LOG and their *_VLOG
 *  verbose variants) compile in, gated by per-subsystem runtime bools that are
 *  initialised from the GFXACCEL_LOG environment variable at startup.
 *  When ACCEL_LOGGING_ENABLED is 0 (ship default): all diagnostic logging
 *  compiles to zero-cost no-ops, runtime bools become compile-time false for
 *  dead-code elimination. Always-on *_ERR macros are NOT gated by this flag.
 *
 *  Runtime control (when compiled in), toggled via tools/gfxlog.sh:
 *    GFXACCEL_LOG          = "all" | comma list of gl,rave,dsp,nqd,comp
 *                            (unset => all subsystems on)
 *    GFXACCEL_LOG_VERBOSE  = 1 to enable the per-draw/per-frame verbose tier
 */

#ifndef ACCEL_LOGGING_H
#define ACCEL_LOGGING_H

#ifndef ACCEL_LOGGING_ENABLED
#define ACCEL_LOGGING_ENABLED 0   /* ship default OFF; toggle via tools/gfxlog.sh */
#endif

#ifdef __cplusplus
#if ACCEL_LOGGING_ENABLED

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace accel_log_detail {

/* True if GFXACCEL_LOG is unset, "all", or a comma list containing `name`.
 * Unset => all-on, preserving the legacy "compile flag on => logs on".
 * Each subsystem calls this to initialise its gate bool at definition,
 * which is order-independent (no static-init ordering hazards). */
inline bool subsystem_on(const char *name) {
    const char *env = std::getenv("GFXACCEL_LOG");
    if (!env || !*env) return true;
    /* Comma-wrap the value and the name and substring-search. Uses C strings
     * only: pulling <string> drags in libc++ <atomic>, which conflicts with
     * <stdatomic.h> in some TUs (e.g. the compositor) before C++23. */
    char hay[256];
    std::snprintf(hay, sizeof(hay), ",%s,", env);
    if (std::strstr(hay, ",all,")) return true;
    char needle[40];
    std::snprintf(needle, sizeof(needle), ",%s,", name);
    return std::strstr(hay, needle) != nullptr;
}

/* True if GFXACCEL_LOG_VERBOSE is set to a truthy value (1/t/y). */
inline bool verbose_env() {
    const char *e = std::getenv("GFXACCEL_LOG_VERBOSE");
    if (!e || !*e) return false;
    return e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y';
}

/* Shared verbose flag, read once from the environment on first use. The
 * thread-safe local static is a single instance across all TUs (C++14),
 * so no per-subsystem wiring is needed. */
inline bool verbose() { static bool v = verbose_env(); return v; }

} /* namespace accel_log_detail */

#define ACCEL_LOG_VERBOSE (accel_log_detail::verbose())

#else  /* !ACCEL_LOGGING_ENABLED */
#define ACCEL_LOG_VERBOSE false
#endif
#endif /* __cplusplus */

#endif /* ACCEL_LOGGING_H */
