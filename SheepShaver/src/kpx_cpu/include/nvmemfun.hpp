/*
 *  nvmemfun.hpp - Non-virtual member function wrappers
 *
 *  Kheperix (C) 2003 Gwenole Beauchesne
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

#ifndef NVMEMFUN_H
#define NVMEMFUN_H

#include <functional>

#ifdef __MINGW32__
#include "vm_alloc.h"
#endif
#if defined(_MSC_VER) && defined(_M_IX86)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string.h>
#endif

/* std::unary_function / binary_function were removed in C++17 */
#if !defined(MACEMU_HAVE_STD_FUNCTOR_BASES)
#if defined(_LIBCPP_STD_VER) && (_LIBCPP_STD_VER > 14)
#define MACEMU_NEED_FUNCTOR_BASES 1
#elif defined(_MSC_VER) && defined(_HAS_AUTO_PTR_ETC) && !_HAS_AUTO_PTR_ETC
#define MACEMU_NEED_FUNCTOR_BASES 1
#elif defined(__cplusplus) && (__cplusplus >= 201703L) && defined(_WIN32)
#define MACEMU_NEED_FUNCTOR_BASES 1
#endif
#ifdef MACEMU_NEED_FUNCTOR_BASES
namespace std {
template<class Arg, class Result>
struct unary_function {
	typedef Arg argument_type;
	typedef Result result_type;
};
template<class Arg1, class Arg2, class Result>
struct binary_function {
	typedef Arg1 first_argument_type;
	typedef Arg2 second_argument_type;
	typedef Result result_type;
};
} /* namespace std */
#endif
#endif

#if defined __GNUC__
#define HAVE_FAST_NV_MEM_FUN 1
#define MEM_FUN_WORDS  2
#if defined __GXX_ABI_VERSION /* GCC >= 3.0 */
#define MEM_FUN_OFFSET 0
#else
#define MEM_FUN_OFFSET 1
#endif
#endif

#if defined __ICC
#define HAVE_FAST_NV_MEM_FUN 1
#define MEM_FUN_WORDS  2
#define MEM_FUN_OFFSET 0 /* GNU C++ ABI v3 */
#endif

#if defined __EDG__  && defined __sgi
#define HAVE_FAST_NV_MEM_FUN 1
#define MEM_FUN_WORDS  3
#define MEM_FUN_OFFSET 2
#endif

/*
 * MSVC (x64): single-inheritance PMFs are a plain code pointer, and the
 * register calling convention passes `this` like a free function's first
 * argument. That matches what dyngen expects from execute.ptr().
 * x86 MSVC uses __thiscall for members vs __cdecl for free funcs - leave
 * the fast path off there (JIT should use MinGW-style thunks if needed).
 */
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64))
#define HAVE_FAST_NV_MEM_FUN 1
#define MEM_FUN_WORDS  1
#define MEM_FUN_OFFSET 0
#endif

#if HAVE_FAST_NV_MEM_FUN

#ifdef __MINGW32__
#define PF_CONVENTION __thiscall
#else
// TODO set a calling convention on other platforms/compilers where the default cc does not pass obj as first param
#define PF_CONVENTION
#endif
#endif /* HAVE_FAST_NV_MEM_FUN */

/* Always define: used on member defs like `void * PF_CONVENTION Class::method(...)`
 * even when the fast nv_mem_fun path is disabled (e.g. MSVC). */
#ifndef PF_CONVENTION
#define PF_CONVENTION
#endif

#if HAVE_FAST_NV_MEM_FUN
/* re-open block for templates below */

template< class PMF, class PF >
inline PF nv_mem_fun_of(PMF pmf) {
    /** Convert member function pointer to a regular function pointer that takes the object as first parameter
    */
	if (pmf == 0)
		return 0;
	union { PMF pmf; uintptr p[MEM_FUN_WORDS]; } x;
	x.pmf = pmf;
	return (PF)x.p[MEM_FUN_OFFSET];
}

template< class R, class T >
class nv_mem_fun_t : public std::unary_function<T, R> {
	typedef R (T::*pmf_t)();
	typedef R (* PF_CONVENTION pf_t)(T *);
	pf_t pf;
public:
	nv_mem_fun_t(pmf_t pmf) : pf(nv_mem_fun_of<pmf_t, pf_t>(pmf)) {}
	R operator()(T *p) const { return (*pf)(p); }
	pf_t ptr() const { return pf; }
};

template< class R, class T >
class const_nv_mem_fun_t : public std::unary_function<T, R> {
	typedef R (T::*pmf_t)();
	typedef R (* PF_CONVENTION pf_t)(T *);
	pf_t const pf;
public:
	const_nv_mem_fun_t(pmf_t const pmf) : pf(nv_mem_fun_of<pmf_t, pf_t>(pmf)) {}
	R operator()(const T *p) const { return (*pf)(p); }
	pf_t ptr() const { return pf; }
};

template< class R, class T, class A >
class nv_mem_fun1_t : public std::binary_function<T, A, R> {
	typedef R (T::*pmf_t)(A);
	typedef R (* PF_CONVENTION pf_t)(T *, A x);
#ifdef __MINGW32__
	typedef R (* default_call_conv_pf_t)(T *, A x);
#endif
	pf_t pf;
public:
	nv_mem_fun1_t(pmf_t pmf) : pf(nv_mem_fun_of<pmf_t, pf_t>(pmf)) {
	#ifdef __MINGW32__
		init_func();
	#endif
	}
	R operator()(T *p, A x) const { return (*pf)(p, x); }

	#ifdef __MINGW32__

	#define NVMEMFUN_THUNK_DEBUG 0

private:
	#ifdef __MINGW64__
	#define DO_CONVENTION_CALL_PF_PLACEHOLDER 0x123456789abcdef0
	#else
	#define DO_CONVENTION_CALL_PF_PLACEHOLDER 0x12345678
	#endif

	#define DO_CONVENTION_CALL_STATICS
	static bool do_convention_call_init_done;
	static int do_convention_call_code_len;
	static int do_convention_call_pf_offset;
	unsigned char * do_convention_call_instance_copy;

	static void init_do_convention_call() {
		if (do_convention_call_init_done) return;

		const int max_code_bytes = 100;
		#ifdef __MINGW64__
		const unsigned char last_code_byte_value = 0xff; // JMP
		#else
		const unsigned char last_code_byte_value = 0xc3;
		#endif

		// figure out the size of the function
		unsigned char * func_pos = (unsigned char *) &do_convention_call;
		int i;
		for (i = 0; i < max_code_bytes; i++) {
			if (func_pos[i] == last_code_byte_value) {
				break;
			}
		}
		do_convention_call_code_len = i + 1;

		#if NVMEMFUN_THUNK_DEBUG
		printf("do_convention_call func size %d ", do_convention_call_code_len);
		#endif

		// find the position of the pf placeholder in the function
		int placeholder_matches = 0;
		for (i = 0; i < do_convention_call_code_len - 3; i++) {
			pf_t * cur_ptr = (pf_t*)(func_pos + i);
			if (*cur_ptr == (pf_t) DO_CONVENTION_CALL_PF_PLACEHOLDER) {
				do_convention_call_pf_offset = i;
		#if NVMEMFUN_THUNK_DEBUG
				printf("ptr pos offset %x", (uint32)cur_ptr - (uint32)func_pos);
		#endif
				++placeholder_matches;
			}
		}

		#if NVMEMFUN_THUNK_DEBUG
		printf("\n");
		fflush(stdout);
		#endif

		assert(placeholder_matches == 1);

		do_convention_call_init_done = true;
	}

	void init_func() {
		if (!do_convention_call_init_done) {
			init_do_convention_call();
		}

		// copy do_convention_call and patch in the address of pf

		do_convention_call_instance_copy = (unsigned char *)VirtualAlloc(NULL, do_convention_call_code_len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		// Thunk buffer needs to be around while default_call_conv_ptr() is still in use,
		// longer than nv_mem_fun1_t lifetime
		//FIXME track the lifetime of this
		if (do_convention_call_instance_copy == NULL) return;
		unsigned char * func_pos = (unsigned char *) &do_convention_call;
		memcpy((void *)do_convention_call_instance_copy, func_pos, do_convention_call_code_len);

		// replace byte sequence in buf copy
		*(pf_t*)(do_convention_call_instance_copy + do_convention_call_pf_offset) = pf;

		#if NVMEMFUN_THUNK_DEBUG
		printf("patching do_convention_call to %x; func size %d ", do_convention_call_instance_copy, do_convention_call_code_len);
		for (int i = 0 ; i < do_convention_call_code_len; i ++) {
			printf("%02x ", do_convention_call_instance_copy[i]);
		}
		printf("\n");
		fflush(stdout);
		#endif

		vm_protect(do_convention_call_instance_copy, do_convention_call_code_len, VM_PAGE_READ | VM_PAGE_EXECUTE);
	}

	// Cheesy thunk solution to adapt the calling convention:
	// do_convention_call accepts the default calling convention and calls pf with PF_CONVENTION
	// Each instance makes its own copy of do_convention_call in a buffer and patches the address of pf into it
	static R do_convention_call(T * obj, A x) {
		pf_t fn = (pf_t) DO_CONVENTION_CALL_PF_PLACEHOLDER;
		return (*fn)(obj, x);
	}

public:

	default_call_conv_pf_t default_call_conv_ptr() const { return (default_call_conv_pf_t) do_convention_call_instance_copy; }

	#else

	pf_t ptr() const { return pf; }

	#endif


};

template< class R, class T, class A >
class const_nv_mem_fun1_t : public std::binary_function<T, A, R> {
	typedef R (T::*pmf_t)(A);
	typedef R (* PF_CONVENTION pf_t)(T *, A x);
	pf_t const pf;
public:
	const_nv_mem_fun1_t(pmf_t const pmf) : pf(nv_mem_fun_of<pmf_t, pf_t>(pmf)) {}
	R operator()(const T *p, A x) const { return (*pf)(p, x); }
	pf_t ptr() const { return pf; }
};

#else

template< class R, class T >
class nv_mem_fun_t : public std::unary_function<T, R> {
	typedef R (T::*pmf_t)();
	pmf_t pf;
public:
	nv_mem_fun_t(R (T::*pmf)()) : pf(pmf) {}
	R operator()(T *p) const { return (p->*pf)(); }
	pmf_t ptr() const { return pf; }
};

template< class R, class T >
class const_nv_mem_fun_t : public std::unary_function<T, R> {
	typedef R (T::*pmf_t)() const;
	pmf_t pf;
public:
	const_nv_mem_fun_t(R (T::*pmf)() const) : pf(pmf) {}
	R operator()(const T *p) const { return (p->*pf)(); }
	pmf_t ptr() const { return pf; }
};

template< class R, class T, class A >
class nv_mem_fun1_t : public std::binary_function<T, A, R> {
	typedef R (T::*pmf_t)(A);
	typedef R (* cdecl_pf_t)(T *, A);
	pmf_t pf;
#if defined(_MSC_VER) && defined(_M_IX86)
	/* cdecl entry that adapts to __thiscall for dyngen generic calls */
	unsigned char *thunk;
	void *pmf_storage; /* absolute address referenced by thunk */
	void init_thunk() {
		/*
		 *  8B 4C 24 08       mov  ecx, [esp+8]     ; this
		 *  FF 74 24 0C       push dword [esp+0xC]  ; arg
		 *  FF 15 xx xx xx xx call dword ptr [pmf_storage]
		 *  C3                ret
		 */
		const size_t code_len = 4 + 4 + 6 + 1;
		const size_t total = code_len + sizeof(void *);
		unsigned char *buf = (unsigned char *)VirtualAlloc(
			NULL, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (!buf) {
			thunk = 0;
			pmf_storage = 0;
			return;
		}
		unsigned char *code = buf;
		/* mov ecx, [esp+8] */
		code[0] = 0x8B; code[1] = 0x4C; code[2] = 0x24; code[3] = 0x08;
		/* push dword [esp+0xC] */
		code[4] = 0xFF; code[5] = 0x74; code[6] = 0x24; code[7] = 0x0C;
		/* call dword ptr [imm32] - absolute address of slot after code */
		void **slot = (void **)(buf + code_len);
		/* Single-inheritance MSVC PMF is a plain code pointer */
		union { pmf_t m; void *raw; } u;
		u.m = pf;
		*slot = u.raw;
		code[8] = 0xFF; code[9] = 0x15;
		unsigned long abs_addr = (unsigned long)slot;
		code[10] = (unsigned char)(abs_addr);
		code[11] = (unsigned char)(abs_addr >> 8);
		code[12] = (unsigned char)(abs_addr >> 16);
		code[13] = (unsigned char)(abs_addr >> 24);
		/* ret */
		code[14] = 0xC3;
		DWORD old_prot;
		VirtualProtect(buf, total, PAGE_EXECUTE_READ, &old_prot);
		thunk = buf;
		pmf_storage = slot;
	}
#endif
public:
	nv_mem_fun1_t(R (T::*pmf)(A)) : pf(pmf)
#if defined(_MSC_VER) && defined(_M_IX86)
		, thunk(0), pmf_storage(0)
#endif
	{
#if defined(_MSC_VER) && defined(_M_IX86)
		init_thunk();
#endif
	}
	R operator()(T *p, A x) const { return (p->*pf)(x); }
	pmf_t ptr() const { return pf; }
#if defined(_MSC_VER) && defined(_M_IX86)
	cdecl_pf_t default_call_conv_ptr() const { return (cdecl_pf_t)thunk; }
#endif
};

template< class R, class T, class A >
class const_nv_mem_fun1_t : public std::binary_function<T, A, R> {
	typedef R (T::*pmf_t)(A) const;
	pmf_t pf;
public:
	const_nv_mem_fun1_t(R (T::*pmf)(A) const) : pf(pmf) {}
	R operator()(const T *p, A x) const { return (p->*pf)(x); }
	pmf_t ptr() const { return pf; }
};

#endif

template< class R, class T >
inline nv_mem_fun_t<R, T> nv_mem_fun(R (T::*pmf)()) {
	return nv_mem_fun_t<R, T>(pmf);
}

template< class R, class T >
inline const_nv_mem_fun_t<R, T> nv_mem_fun(R (T::*pmf)() const) {
	return const_nv_mem_fun_t<R, T>(pmf);
}

template< class R, class T, class A >
inline nv_mem_fun1_t<R, T, A> nv_mem_fun(R (T::*pmf)(A)) {
	return nv_mem_fun1_t<R, T, A>(pmf);
}

template< class R, class T, class A >
inline const_nv_mem_fun1_t<R, T, A> nv_mem_fun(R (T::*pmf)(A) const) {
	return const_nv_mem_fun1_t<R, T, A>(pmf);
}

#endif /* NVMEMFUN_H */
