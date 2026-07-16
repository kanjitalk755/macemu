/*
 *  gfxaccel_arc_shim.cpp - No-op ARC wrappers (OpenGL backend; no ObjC pools)
 */

#include "sysdeps.h"
#include "rave_engine.h"
#include "gl_engine.h"

uint32_t RaveDispatchARC(uint32_t r3, uint32_t r4, uint32_t r5,
                         uint32_t r6, uint32_t r7, uint32_t r8)
{
	return RaveDispatch(r3, r4, r5, r6, r7, r8);
}

uint32_t GLDispatchARC(uint32_t r3, uint32_t r4, uint32_t r5,
                       uint32_t r6, uint32_t r7, uint32_t r8,
                       uint32_t r9, uint32_t r10,
                       const uint32_t *float_bits, int num_float_args)
{
	return GLDispatch(r3, r4, r5, r6, r7, r8, r9, r10, float_bits, num_float_args);
}
