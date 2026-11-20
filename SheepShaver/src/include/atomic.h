/*
 *  atomic.h - the subset of C11 <stdatomic.h> this tree uses, implemented
 *             per platform for compilers that predate it
 *
 *  (C) 2026 RandoOnSteam <battlemageloveryt@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef ATOMIC_H
#define ATOMIC_H

/* ---------------------------------------------------------------------- */
/*  Fixed-width integer types       */
/* ---------------------------------------------------------------------- */

#if defined(_MSC_VER) && _MSC_VER < 1600
typedef unsigned __int32 atomic_u32_t;
typedef unsigned __int64 atomic_u64_t;
#else
#include <stdint.h>
typedef uint32_t atomic_u32_t;
typedef uint64_t atomic_u64_t;
#endif

#if defined(__cplusplus)
#  define ATOMIC_INLINE inline
#elif defined(_MSC_VER)
#  define ATOMIC_INLINE static __inline
#elif defined(__GNUC__)
#  define ATOMIC_INLINE static __inline__
#else
#  define ATOMIC_INLINE static
#endif

/* ---------------------------------------------------------------------- */
/*  Backend selection                                                      */
/* ---------------------------------------------------------------------- */

#if !defined(__cplusplus) && defined(__STDC_VERSION__) && \
    __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
#  define ATOMIC_BACKEND_C11 1
#elif defined(_MSC_VER)
#  define ATOMIC_BACKEND_MSVC 1
#elif defined(__ATOMIC_RELAXED)
#  define ATOMIC_BACKEND_GNU 1          /* GCC 4.7+ / clang __atomic       */
#elif defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 1))
#  define ATOMIC_BACKEND_SYNC 1         /* GCC 4.1+ __sync                 */
#else
#  define ATOMIC_BACKEND_PLAIN 1        /* no intrinsics: volatile only    */
#endif

#if defined(_WIN64) || defined(_M_X64) || defined(_M_ARM64) || \
    defined(__x86_64__) || defined(__aarch64__) || defined(__powerpc64__) || \
    defined(__LP64__) || defined(_LP64)
#  define ATOMIC_NATIVE_64 1
#endif

/* ====================================================================== */
/*  memory_order                                                          */
/* ====================================================================== */

#if ATOMIC_BACKEND_C11
#include <stdatomic.h>
#else
enum {
	memory_order_relaxed = 0,
	memory_order_consume = 1,
	memory_order_acquire = 2,
	memory_order_release = 3,
	memory_order_acq_rel = 4,
	memory_order_seq_cst = 5
};
typedef int memory_order;
#endif

/* ====================================================================== */
/*  Types                                                                 */
/* ====================================================================== */

#if ATOMIC_BACKEND_C11

typedef _Atomic(atomic_u32_t) atomic_uint32;
typedef _Atomic(atomic_u64_t) atomic_uint64;
typedef _Atomic(int)          atomic_sint;
typedef _Atomic(double)       atomic_double;

#else

typedef volatile atomic_u32_t atomic_uint32;
typedef volatile atomic_u64_t atomic_uint64;
typedef volatile int          atomic_sint;
typedef volatile double       atomic_double;

#endif

/* ====================================================================== */
/*  Fences                                                                */
/* ====================================================================== */

#if ATOMIC_BACKEND_MSVC

#include <intrin.h>

#if defined(_M_ARM) || defined(_M_ARM64)
#  define ATOMIC_FENCE_ACQ()  __dmb(0xB)
#  define ATOMIC_FENCE_REL()  __dmb(0xB)
#  define ATOMIC_FENCE_FULL() __dmb(0xB)
#else
#  define ATOMIC_FENCE_ACQ()  _ReadWriteBarrier()
#  define ATOMIC_FENCE_REL()  _ReadWriteBarrier()
#  define ATOMIC_FENCE_FULL() _mm_mfence()
#endif

#elif ATOMIC_BACKEND_SYNC

#define ATOMIC_FENCE_ACQ()  __sync_synchronize()
#define ATOMIC_FENCE_REL()  __sync_synchronize()
#define ATOMIC_FENCE_FULL() __sync_synchronize()

#elif ATOMIC_BACKEND_PLAIN

#if defined(__GNUC__)
#  define ATOMIC_FENCE_ACQ()  __asm__ __volatile__ ("" : : : "memory")
#  define ATOMIC_FENCE_REL()  __asm__ __volatile__ ("" : : : "memory")
#  define ATOMIC_FENCE_FULL() __asm__ __volatile__ ("" : : : "memory")
#else
#  define ATOMIC_FENCE_ACQ()  ((void)0)
#  define ATOMIC_FENCE_REL()  ((void)0)
#  define ATOMIC_FENCE_FULL() ((void)0)
#endif

#endif /* fence backends */

#if !ATOMIC_BACKEND_C11 && !ATOMIC_BACKEND_GNU
#define ATOMIC_ORDER_BEFORE_LOAD(o)   ((void)0)
#define ATOMIC_ORDER_AFTER_LOAD(o) \
	do { if ((o) != memory_order_relaxed) ATOMIC_FENCE_ACQ(); } while (0)
#define ATOMIC_ORDER_BEFORE_STORE(o) \
	do { if ((o) != memory_order_relaxed) ATOMIC_FENCE_REL(); } while (0)
#define ATOMIC_ORDER_AFTER_STORE(o) \
	do { if ((o) == memory_order_seq_cst) ATOMIC_FENCE_FULL(); } while (0)
#endif

/* ====================================================================== */
/*  32-bit operations                                                     */
/* ====================================================================== */

#if ATOMIC_BACKEND_C11

ATOMIC_INLINE atomic_u32_t atomic_load32_explicit(const atomic_uint32 *p, memory_order o)
	{ return atomic_load_explicit(p, o); }
ATOMIC_INLINE void atomic_store32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ atomic_store_explicit(p, v, o); }
ATOMIC_INLINE atomic_u32_t atomic_fetch_add32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ return atomic_fetch_add_explicit(p, v, o); }
ATOMIC_INLINE atomic_u32_t atomic_fetch_or32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ return atomic_fetch_or_explicit(p, v, o); }
ATOMIC_INLINE atomic_u32_t atomic_fetch_and32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ return atomic_fetch_and_explicit(p, v, o); }
ATOMIC_INLINE atomic_u32_t atomic_exchange32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ return atomic_exchange_explicit(p, v, o); }

#elif ATOMIC_BACKEND_GNU

ATOMIC_INLINE atomic_u32_t atomic_load32_explicit(const atomic_uint32 *p, memory_order o)
	{ return __atomic_load_n((atomic_uint32 *)p, o); }
ATOMIC_INLINE void atomic_store32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ __atomic_store_n(p, v, o); }
ATOMIC_INLINE atomic_u32_t atomic_fetch_add32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ return __atomic_fetch_add(p, v, o); }
ATOMIC_INLINE atomic_u32_t atomic_fetch_or32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ return __atomic_fetch_or(p, v, o); }
ATOMIC_INLINE atomic_u32_t atomic_fetch_and32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ return __atomic_fetch_and(p, v, o); }
ATOMIC_INLINE atomic_u32_t atomic_exchange32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ return __atomic_exchange_n(p, v, o); }

#elif ATOMIC_BACKEND_MSVC

ATOMIC_INLINE atomic_u32_t atomic_load32_explicit(const atomic_uint32 *p, memory_order o)
{
	atomic_u32_t v;
	ATOMIC_ORDER_BEFORE_LOAD(o);
	v = *p;
	ATOMIC_ORDER_AFTER_LOAD(o);
	return v;
}
ATOMIC_INLINE void atomic_store32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
{
	ATOMIC_ORDER_BEFORE_STORE(o);
	*p = v;
	ATOMIC_ORDER_AFTER_STORE(o);
}
ATOMIC_INLINE atomic_u32_t atomic_fetch_add32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ (void)o; return (atomic_u32_t)_InterlockedExchangeAdd((volatile long *)p, (long)v); }
ATOMIC_INLINE atomic_u32_t atomic_fetch_or32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ (void)o; return (atomic_u32_t)_InterlockedOr((volatile long *)p, (long)v); }
ATOMIC_INLINE atomic_u32_t atomic_fetch_and32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ (void)o; return (atomic_u32_t)_InterlockedAnd((volatile long *)p, (long)v); }
ATOMIC_INLINE atomic_u32_t atomic_exchange32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ (void)o; return (atomic_u32_t)_InterlockedExchange((volatile long *)p, (long)v); }

#elif ATOMIC_BACKEND_SYNC

ATOMIC_INLINE atomic_u32_t atomic_load32_explicit(const atomic_uint32 *p, memory_order o)
{
	atomic_u32_t v = *p;
	ATOMIC_ORDER_AFTER_LOAD(o);
	return v;
}
ATOMIC_INLINE void atomic_store32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
{
	ATOMIC_ORDER_BEFORE_STORE(o);
	*p = v;
	ATOMIC_ORDER_AFTER_STORE(o);
}
ATOMIC_INLINE atomic_u32_t atomic_fetch_add32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ (void)o; return __sync_fetch_and_add(p, v); }
ATOMIC_INLINE atomic_u32_t atomic_fetch_or32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ (void)o; return __sync_fetch_and_or(p, v); }
ATOMIC_INLINE atomic_u32_t atomic_fetch_and32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ (void)o; return __sync_fetch_and_and(p, v); }
ATOMIC_INLINE atomic_u32_t atomic_exchange32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
{
	atomic_u32_t old;
	(void)o;
	do { old = *p; } while (__sync_val_compare_and_swap(p, old, v) != old);
	return old;
}

#else /* ATOMIC_BACKEND_PLAIN */

ATOMIC_INLINE atomic_u32_t atomic_load32_explicit(const atomic_uint32 *p, memory_order o)
{
	atomic_u32_t v = *p;
	ATOMIC_ORDER_AFTER_LOAD(o);
	return v;
}
ATOMIC_INLINE void atomic_store32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
{
	ATOMIC_ORDER_BEFORE_STORE(o);
	*p = v;
}
ATOMIC_INLINE atomic_u32_t atomic_fetch_add32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ atomic_u32_t old = *p; (void)o; *p = old + v; return old; }
ATOMIC_INLINE atomic_u32_t atomic_fetch_or32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ atomic_u32_t old = *p; (void)o; *p = old | v; return old; }
ATOMIC_INLINE atomic_u32_t atomic_fetch_and32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ atomic_u32_t old = *p; (void)o; *p = old & v; return old; }
ATOMIC_INLINE atomic_u32_t atomic_exchange32_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ atomic_u32_t old = *p; (void)o; *p = v; return old; }

#endif /* 32-bit backends */

/* ====================================================================== */
/*  64-bit operations                                                     */
/* ====================================================================== */

#if ATOMIC_BACKEND_C11

ATOMIC_INLINE atomic_u64_t atomic_load64_explicit(const atomic_uint64 *p, memory_order o)
	{ return atomic_load_explicit(p, o); }
ATOMIC_INLINE void atomic_store64_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
	{ atomic_store_explicit(p, v, o); }
ATOMIC_INLINE atomic_u64_t atomic_fetch_add64_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
	{ return atomic_fetch_add_explicit(p, v, o); }
ATOMIC_INLINE atomic_u64_t atomic_exchange64_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
	{ return atomic_exchange_explicit(p, v, o); }

#elif ATOMIC_BACKEND_GNU

ATOMIC_INLINE atomic_u64_t atomic_load64_explicit(const atomic_uint64 *p, memory_order o)
	{ return __atomic_load_n((atomic_uint64 *)p, o); }
ATOMIC_INLINE void atomic_store64_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
	{ __atomic_store_n(p, v, o); }
ATOMIC_INLINE atomic_u64_t atomic_fetch_add64_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
	{ return __atomic_fetch_add(p, v, o); }
ATOMIC_INLINE atomic_u64_t atomic_exchange64_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
	{ return __atomic_exchange_n(p, v, o); }

#elif ATOMIC_BACKEND_MSVC

ATOMIC_INLINE atomic_u64_t atomic_load64_explicit(const atomic_uint64 *p, memory_order o)
{
#if ATOMIC_NATIVE_64
	atomic_u64_t v;
	ATOMIC_ORDER_BEFORE_LOAD(o);
	v = *p;
	ATOMIC_ORDER_AFTER_LOAD(o);
	return v;
#else
	(void)o;
	return (atomic_u64_t)_InterlockedCompareExchange64(
		(volatile __int64 *)p, 0, 0);
#endif
}
ATOMIC_INLINE atomic_u64_t atomic_exchange64_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
{
#if ATOMIC_NATIVE_64
	(void)o;
	return (atomic_u64_t)_InterlockedExchange64((volatile __int64 *)p, (__int64)v);
#else
	__int64 old;
	(void)o;
	do {
		old = *(volatile __int64 *)p;
	} while (_InterlockedCompareExchange64((volatile __int64 *)p, (__int64)v, old) != old);
	return (atomic_u64_t)old;
#endif
}
ATOMIC_INLINE void atomic_store64_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
{
#if ATOMIC_NATIVE_64
	ATOMIC_ORDER_BEFORE_STORE(o);
	*p = v;
	ATOMIC_ORDER_AFTER_STORE(o);
#else
	(void)atomic_exchange64_explicit(p, v, o);
#endif
}
ATOMIC_INLINE atomic_u64_t atomic_fetch_add64_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
{
#if ATOMIC_NATIVE_64
	(void)o;
	return (atomic_u64_t)_InterlockedExchangeAdd64((volatile __int64 *)p, (__int64)v);
#else
	__int64 old;
	(void)o;
	do {
		old = *(volatile __int64 *)p;
	} while (_InterlockedCompareExchange64((volatile __int64 *)p,
	                                       old + (__int64)v, old) != old);
	return (atomic_u64_t)old;
#endif
}

#elif ATOMIC_BACKEND_SYNC

ATOMIC_INLINE atomic_u64_t atomic_load64_explicit(const atomic_uint64 *p, memory_order o)
{
#if ATOMIC_NATIVE_64
	atomic_u64_t v = *p;
	ATOMIC_ORDER_AFTER_LOAD(o);
	return v;
#else
	(void)o;
	return __sync_val_compare_and_swap((atomic_uint64 *)p, 0, 0);
#endif
}
ATOMIC_INLINE atomic_u64_t atomic_exchange64_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
{
	atomic_u64_t old;
	(void)o;
	do { old = *p; } while (__sync_val_compare_and_swap(p, old, v) != old);
	return old;
}
ATOMIC_INLINE void atomic_store64_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
{
#if ATOMIC_NATIVE_64
	ATOMIC_ORDER_BEFORE_STORE(o);
	*p = v;
	ATOMIC_ORDER_AFTER_STORE(o);
#else
	(void)atomic_exchange64_explicit(p, v, o);
#endif
}
ATOMIC_INLINE atomic_u64_t atomic_fetch_add64_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
	{ (void)o; return __sync_fetch_and_add(p, v); }

#else /* ATOMIC_BACKEND_PLAIN */

ATOMIC_INLINE atomic_u64_t atomic_load64_explicit(const atomic_uint64 *p, memory_order o)
{
	atomic_u64_t v = *p;
	ATOMIC_ORDER_AFTER_LOAD(o);
	return v;
}
ATOMIC_INLINE void atomic_store64_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
{
	ATOMIC_ORDER_BEFORE_STORE(o);
	*p = v;
}
ATOMIC_INLINE atomic_u64_t atomic_fetch_add64_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
	{ atomic_u64_t old = *p; (void)o; *p = old + v; return old; }
ATOMIC_INLINE atomic_u64_t atomic_exchange64_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
	{ atomic_u64_t old = *p; (void)o; *p = v; return old; }

#endif /* 64-bit backends */

/* ====================================================================== */
/*  int and double                   */
/* ====================================================================== */

#if ATOMIC_BACKEND_C11

ATOMIC_INLINE int atomic_loadi_explicit(const atomic_sint *p, memory_order o)
	{ return atomic_load_explicit(p, o); }
ATOMIC_INLINE void atomic_storei_explicit(atomic_sint *p, int v, memory_order o)
	{ atomic_store_explicit(p, v, o); }
ATOMIC_INLINE int atomic_fetch_addi_explicit(atomic_sint *p, int v, memory_order o)
	{ return atomic_fetch_add_explicit(p, v, o); }
ATOMIC_INLINE int atomic_exchangei_explicit(atomic_sint *p, int v, memory_order o)
	{ return atomic_exchange_explicit(p, v, o); }
ATOMIC_INLINE double atomic_loadd_explicit(const atomic_double *p, memory_order o)
	{ return atomic_load_explicit(p, o); }
ATOMIC_INLINE void atomic_stored_explicit(atomic_double *p, double v, memory_order o)
	{ atomic_store_explicit(p, v, o); }

#else

ATOMIC_INLINE int atomic_loadi_explicit(const atomic_sint *p, memory_order o)
	{ return (int)atomic_load32_explicit((const atomic_uint32 *)p, o); }
ATOMIC_INLINE void atomic_storei_explicit(atomic_sint *p, int v, memory_order o)
	{ atomic_store32_explicit((atomic_uint32 *)p, (atomic_u32_t)v, o); }
ATOMIC_INLINE int atomic_fetch_addi_explicit(atomic_sint *p, int v, memory_order o)
	{ return (int)atomic_fetch_add32_explicit((atomic_uint32 *)p, (atomic_u32_t)v, o); }
ATOMIC_INLINE int atomic_exchangei_explicit(atomic_sint *p, int v, memory_order o)
	{ return (int)atomic_exchange32_explicit((atomic_uint32 *)p, (atomic_u32_t)v, o); }

union atomic_dbl_bits { double d; atomic_u64_t u; };

ATOMIC_INLINE double atomic_loadd_explicit(const atomic_double *p, memory_order o)
{
	union atomic_dbl_bits b;
	b.u = atomic_load64_explicit((const atomic_uint64 *)p, o);
	return b.d;
}
ATOMIC_INLINE void atomic_stored_explicit(atomic_double *p, double v, memory_order o)
{
	union atomic_dbl_bits b;
	b.d = v;
	atomic_store64_explicit((atomic_uint64 *)p, b.u, o);
}

#endif

/* ====================================================================== */
/*  Pointer slots                                                         */
/* ====================================================================== */

#if ATOMIC_NATIVE_64
#  define ATOMIC_PTR_AS_U64 1
#endif

ATOMIC_INLINE void *atomic_loadp_explicit(void *const volatile *p, memory_order o)
{
#if ATOMIC_PTR_AS_U64
	return (void *)(size_t)atomic_load64_explicit((const atomic_uint64 *)p, o);
#else
	return (void *)(size_t)atomic_load32_explicit((const atomic_uint32 *)p, o);
#endif
}
ATOMIC_INLINE void atomic_storep_explicit(void *volatile *p, void *v, memory_order o)
{
#if ATOMIC_PTR_AS_U64
	atomic_store64_explicit((atomic_uint64 *)p, (atomic_u64_t)(size_t)v, o);
#else
	atomic_store32_explicit((atomic_uint32 *)p, (atomic_u32_t)(size_t)v, o);
#endif
}

/* ====================================================================== */
/*  C++ spelling            */
/* ====================================================================== */

#if defined(__cplusplus) && !ATOMIC_BACKEND_C11

inline atomic_u32_t atomic_load_explicit(const atomic_uint32 *p, memory_order o)
	{ return atomic_load32_explicit(p, o); }
inline void atomic_store_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ atomic_store32_explicit(p, v, o); }
inline atomic_u32_t atomic_fetch_add_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ return atomic_fetch_add32_explicit(p, v, o); }
inline atomic_u32_t atomic_fetch_or_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ return atomic_fetch_or32_explicit(p, v, o); }
inline atomic_u32_t atomic_fetch_and_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ return atomic_fetch_and32_explicit(p, v, o); }
inline atomic_u32_t atomic_exchange_explicit(atomic_uint32 *p, atomic_u32_t v, memory_order o)
	{ return atomic_exchange32_explicit(p, v, o); }

inline atomic_u64_t atomic_load_explicit(const atomic_uint64 *p, memory_order o)
	{ return atomic_load64_explicit(p, o); }
inline void atomic_store_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
	{ atomic_store64_explicit(p, v, o); }
inline atomic_u64_t atomic_fetch_add_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
	{ return atomic_fetch_add64_explicit(p, v, o); }
inline atomic_u64_t atomic_exchange_explicit(atomic_uint64 *p, atomic_u64_t v, memory_order o)
	{ return atomic_exchange64_explicit(p, v, o); }

inline int atomic_load_explicit(const atomic_sint *p, memory_order o)
	{ return atomic_loadi_explicit(p, o); }
inline void atomic_store_explicit(atomic_sint *p, int v, memory_order o)
	{ atomic_storei_explicit(p, v, o); }
inline int atomic_fetch_add_explicit(atomic_sint *p, int v, memory_order o)
	{ return atomic_fetch_addi_explicit(p, v, o); }
inline int atomic_exchange_explicit(atomic_sint *p, int v, memory_order o)
	{ return atomic_exchangei_explicit(p, v, o); }

inline double atomic_load_explicit(const atomic_double *p, memory_order o)
	{ return atomic_loadd_explicit(p, o); }
inline void atomic_store_explicit(atomic_double *p, double v, memory_order o)
	{ atomic_stored_explicit(p, v, o); }

template <class T>
inline T *atomic_load_explicit(T *volatile *p, memory_order o)
	{ return (T *)atomic_loadp_explicit((void *const volatile *)p, o); }
template <class T, class U>
inline void atomic_store_explicit(T *volatile *p, U *v, memory_order o)
	{ atomic_storep_explicit((void *volatile *)p, (void *)v, o); }

#endif /* __cplusplus && !C11 */

#endif /* ATOMIC_H */
