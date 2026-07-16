/*
 *  gfxaccel_arc_shim.mm - ObjC++ @autoreleasepool wrappers for PPC dispatch
 *                         entry points.
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  PURPOSE
 *  -------
 *  The PPC emul thread (main_unix.cpp:emul_func -> jump_to_rom) runs the
 *  PowerPC interpreter in a tight loop with NO outer @autoreleasepool.
 *  When RAVE / GL draw methods create autoreleased ObjC objects
 *  (MTLCommandBuffer, MTLRenderCommandEncoder, MTLRenderPassDescriptor,
 *  MTLBlitCommandEncoder, MTLTextureDescriptor, MTLVertexDescriptor, and
 *  the many transient NSString/NSError objects Metal creates under
 *  validation), those objects accumulate in the thread's top-level
 *  autorelease pool and are never drained during a gameplay session.
 *  The result is LINEAR memory growth tied to rendered frames -- exactly
 *  what Nanosaur exhibited after the 97ab9856+2c873706+fa1a003c+44510413
 *  fix chain landed and steady-state gameplay became possible.
 *
 *  FIX
 *  ---
 *  Wrap the RAVE and GL dispatch entry points in @autoreleasepool.  The
 *  pool drains on every NATIVE_RAVE_DISPATCH / NATIVE_GL_DISPATCH call
 *  (every RAVE draw method, every GL entry point invoked by the emulated
 *  app).  This is the idiomatic pattern for ObjC++ code running outside
 *  a Cocoa run loop and matches MetalCompositorPresent (which already
 *  wraps its per-frame body).
 *
 *  sheepshaver_glue.cpp:NATIVE_RAVE_DISPATCH / NATIVE_GL_DISPATCH are
 *  compiled as C++; they cannot host the @autoreleasepool directive.
 *  This shim lives in ObjC++ and is called via the extern "C" signatures
 *  declared in rave_engine.h / gl_engine.h.
 *
 *  IMMEDIATE-MODE FAST PATH
 *  ------------------------
 *  GLDispatchARC additionally SKIPS the pool for a small allowlist of
 *  immediate-mode coordinate/attribute setters (glBegin, glVertex*,
 *  glColor*, glTexCoord*, glNormal*) whose Native* handlers
 *  (gl_metal_renderer.mm) only mutate the GLContext struct or push to a
 *  std::vector and allocate ZERO autoreleased ObjC objects.  For these the
 *  pool would never have anything to drain, yet they dominate per-frame call
 *  volume in immediate-mode titles (F/A-18 Korea: ~576k of ~696k GLDispatch
 *  calls/run).  See GLOpcodeIsObjCFreeImmediate below.
 */

#import <Foundation/Foundation.h>

#include "sysdeps.h"
#include "cpu_emulation.h"   // ReadMacInt32 for the immediate-mode fast path
#include "rave_engine.h"
#include "gl_engine.h"

uint32_t RaveDispatchARC(uint32_t r3, uint32_t r4, uint32_t r5,
                                     uint32_t r6, uint32_t r7, uint32_t r8)
{
	uint32_t result = 0;
	@autoreleasepool {
		result = RaveDispatch(r3, r4, r5, r6, r7, r8);
	}
	return result;
}

// Immediate-mode opcodes whose Native* handlers (gl_metal_renderer.mm) only
// mutate the GLContext struct or push to ctx->im_vertices (a std::vector) and
// allocate ZERO autoreleased ObjC objects.  Wrapping each of these in a
// per-call @autoreleasepool is pure overhead because the pool never has
// anything to drain.  glEnd (GL_SUB_END) is deliberately NOT here -- it alone
// flushes the accumulated vertices to Metal (GLMetalFlushImmediateMode), so it
// keeps the pool.  The display-list record path (GLRecordCommand, gl_dispatch.cpp)
// these same opcodes can take when compiling a list is likewise pure C++.
//
// The ranges are the contiguous coordinate/attribute setter families from the
// GL_SUB_* enum (gl_engine.h); the neighbouring *_POINTER and unrelated opcodes
// fall just outside them and keep the pool:
//   GL_SUB_BEGIN                                       (4)    glBegin
//   GL_SUB_COLOR3B     .. GL_SUB_COLOR4USV       (17..48)    glColor{3,4}{b,d,f,i,s,ub,ui,us}[v]
//   GL_SUB_NORMAL3B    .. GL_SUB_NORMAL3SV     (178..187)    glNormal3{b,d,f,i,s}[v]
//   GL_SUB_TEX_COORD1D .. GL_SUB_TEX_COORD4SV  (257..288)    glTexCoord{1..4}{d,f,i,s}[v]
//   GL_SUB_VERTEX2D    .. GL_SUB_VERTEX4SV     (310..333)    glVertex{2..4}{d,f,i,s}[v]
static inline bool GLOpcodeIsObjCFreeImmediate(uint32_t op)
{
	return op == GL_SUB_BEGIN
	    || (op >= GL_SUB_COLOR3B     && op <= GL_SUB_COLOR4USV)
	    || (op >= GL_SUB_NORMAL3B    && op <= GL_SUB_NORMAL3SV)
	    || (op >= GL_SUB_TEX_COORD1D && op <= GL_SUB_TEX_COORD4SV)
	    || (op >= GL_SUB_VERTEX2D    && op <= GL_SUB_VERTEX4SV);
}

uint32_t GLDispatchARC(uint32_t r3, uint32_t r4, uint32_t r5,
                                   uint32_t r6, uint32_t r7, uint32_t r8,
                                   uint32_t r9, uint32_t r10,
                                   const uint32_t *float_bits, int num_float_args)
{
	// GLDispatch reads the sub-opcode from gl_scratch_addr; read the same word
	// here (there is no intervening write) to decide whether a pool is needed.
	// For the ObjC-free immediate-mode setters the pool would never have
	// anything to drain, so dispatch directly.  Behaviour is identical -- those
	// handlers create no autoreleased objects, so there is nothing whose drain
	// timing could differ; every other opcode still gets its @autoreleasepool.
	if (GLOpcodeIsObjCFreeImmediate(ReadMacInt32(gl_scratch_addr))) {
		return GLDispatch(r3, r4, r5, r6, r7, r8, r9, r10,
		                  float_bits, num_float_args);
	}

	uint32_t result = 0;
	@autoreleasepool {
		result = GLDispatch(r3, r4, r5, r6, r7, r8, r9, r10,
		                    float_bits, num_float_args);
	}
	return result;
}
