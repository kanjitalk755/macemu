/*
 *  gfxaccel_msvc.h - MSVC / non-Apple portability shims for gfxaccel
 */
#ifndef GFXACCEL_MSVC_H
#define GFXACCEL_MSVC_H

#if defined(_MSC_VER)

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES 1
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef __builtin_unreachable
#define __builtin_unreachable() __assume(0)
#endif

/* C11 atomics: prefer MSVC's stdatomic when available (VS 2022+), else wrap. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#include <stdatomic.h>
#else
#include <atomic>
/* Minimal C-compatible aliases used by dsp_engine.cpp */
using std::memory_order_relaxed;
using std::memory_order_acquire;
using std::memory_order_release;
#define atomic_fetch_or_explicit(p, v, o)  ((p)->fetch_or((v), (o)))
#define atomic_exchange_explicit(p, v, o)  ((p)->exchange((v), (o)))
#define atomic_store_explicit(p, v, o)     ((p)->store((v), (o)))
#define atomic_load_explicit(p, o)         ((p)->load((o)))
#define _Atomic(T) std::atomic<T>
#endif

#endif /* _MSC_VER */

#endif /* GFXACCEL_MSVC_H */
