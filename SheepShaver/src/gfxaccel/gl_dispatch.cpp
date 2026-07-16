/*
 *  gl_dispatch.cpp - OpenGL multiplexed dispatch from sub-opcode to handler functions
 *
 *  (C) 2026 Sierra Burkhart (sierra760)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Reads the sub-opcode from the scratch word written by the PPC thunk
 *  and dispatches to the appropriate GL/AGL/GLU/GLUT handler function.
 *  All dispatch entries have real implementations covering core GL 1.2.1,
 *  extensions, GLU, and GLUT function groups.
 */

#include <cstring>
#include <cstdio>

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "gl_engine.h"
#include "accel_logging.h"
#include "gl_defer.h"

// Logging state -- gated by GL_LOGGING env var (via subsystem_on).
#if ACCEL_LOGGING_ENABLED
bool gl_logging_enabled = accel_log_detail::subsystem_on("gl");
#endif

// PPC stack pointer, saved by glue code before dispatch for 9+ arg access
uint32_t gl_ppc_sp = 0;
// Stack arg offset: 1 when dispatch-table path shifts args, 0 normally
int gl_ppc_stack_arg_offset = 0;

/*
 *  gl_ppc_stack_arg -- read the Nth stack argument (0-based) from PPC stack
 *
 *  PPC ABI: stack args start at SP + 24 + 8*4 = SP + 56 (after linkage area
 *  and 8 parameter save words). But in practice with thunks, the caller's
 *  frame has params at SP + 24 (linkage) + param_offset.
 *  For PowerPC, args beyond r10 are at: caller SP + 24 + (arg_index) * 4
 *  where arg_index starts at 8 (since r3-r10 = args 0-7).
 *
 *  When the dispatch-table path is active (gl_ppc_stack_arg_offset == 1),
 *  stack indices are shifted by 1 because the context arg in r3 pushes
 *  all subsequent args one position further on the stack.
 */
uint32_t gl_ppc_stack_arg(int index)
{
	if (gl_ppc_sp == 0) return 0;
	return ReadMacInt32(gl_ppc_sp + 24 + (8 + gl_ppc_stack_arg_offset + index) * 4);
}

// ---- GL state handler externs (implemented in gl_state.cpp) ----
extern void NativeGLMatrixMode(GLContext *ctx, uint32_t mode);
extern void NativeGLLoadIdentity(GLContext *ctx);
extern void NativeGLLoadMatrixf(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLLoadMatrixd(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLMultMatrixf(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLMultMatrixd(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLPushMatrix(GLContext *ctx);
extern void NativeGLPopMatrix(GLContext *ctx);
extern void NativeGLRotatef(GLContext *ctx, float angle, float x, float y, float z);
extern void NativeGLRotated(GLContext *ctx, double angle, double x, double y, double z);
extern void NativeGLTranslatef(GLContext *ctx, float x, float y, float z);
extern void NativeGLTranslated(GLContext *ctx, double x, double y, double z);
extern void NativeGLScalef(GLContext *ctx, float x, float y, float z);
extern void NativeGLScaled(GLContext *ctx, double x, double y, double z);
extern void NativeGLFrustum(GLContext *ctx, double l, double r, double b, double t, double n, double f);
extern void NativeGLOrtho(GLContext *ctx, double l, double r, double b, double t, double n, double f);
extern void NativeGLEnable(GLContext *ctx, uint32_t cap);
extern void NativeGLDisable(GLContext *ctx, uint32_t cap);
extern uint32_t NativeGLIsEnabled(GLContext *ctx, uint32_t cap);
extern void NativeGLDepthFunc(GLContext *ctx, uint32_t func);
extern void NativeGLDepthMask(GLContext *ctx, uint32_t flag);
extern void NativeGLDepthRange(GLContext *ctx, double near_val, double far_val);
extern void NativeGLBlendFunc(GLContext *ctx, uint32_t src, uint32_t dst);
extern void NativeGLAlphaFunc(GLContext *ctx, uint32_t func, float ref);
extern void NativeGLStencilFunc(GLContext *ctx, uint32_t func, int32_t ref, uint32_t mask);
extern void NativeGLStencilOp(GLContext *ctx, uint32_t sfail, uint32_t dpfail, uint32_t dppass);
extern void NativeGLStencilMask(GLContext *ctx, uint32_t mask);
extern void NativeGLViewport(GLContext *ctx, int32_t x, int32_t y, int32_t w, int32_t h);
extern void NativeGLScissor(GLContext *ctx, int32_t x, int32_t y, int32_t w, int32_t h);
extern void NativeGLClearColor(GLContext *ctx, float r, float g, float b, float a);
extern void NativeGLClearDepth(GLContext *ctx, double depth);
extern void NativeGLClearStencil(GLContext *ctx, int32_t s);
extern void NativeGLClear(GLContext *ctx, uint32_t mask);
extern void NativeGLShadeModel(GLContext *ctx, uint32_t mode);
extern void NativeGLCullFace(GLContext *ctx, uint32_t mode);
extern void NativeGLFrontFace(GLContext *ctx, uint32_t mode);
extern void NativeGLPolygonMode(GLContext *ctx, uint32_t face, uint32_t mode);
extern void NativeGLColorMask(GLContext *ctx, uint32_t r, uint32_t g, uint32_t b, uint32_t a);
extern void NativeGLLineWidth(GLContext *ctx, float w);
extern void NativeGLPointSize(GLContext *ctx, float s);
extern void NativeGLPolygonOffset(GLContext *ctx, float factor, float units);
extern void NativeGLHint(GLContext *ctx, uint32_t target, uint32_t mode);
extern void NativeGLLogicOp(GLContext *ctx, uint32_t op);
extern void NativeGLLightf(GLContext *ctx, uint32_t light, uint32_t pname, float param);
extern void NativeGLLightfv(GLContext *ctx, uint32_t light, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLLighti(GLContext *ctx, uint32_t light, uint32_t pname, int32_t param);
extern void NativeGLLightiv(GLContext *ctx, uint32_t light, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLLightModelf(GLContext *ctx, uint32_t pname, float param);
extern void NativeGLLightModelfv(GLContext *ctx, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLLightModeli(GLContext *ctx, uint32_t pname, int32_t param);
extern void NativeGLLightModeliv(GLContext *ctx, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLMaterialf(GLContext *ctx, uint32_t face, uint32_t pname, float param);
extern void NativeGLMaterialfv(GLContext *ctx, uint32_t face, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLMateriali(GLContext *ctx, uint32_t face, uint32_t pname, int32_t param);
extern void NativeGLMaterialiv(GLContext *ctx, uint32_t face, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLColorMaterial(GLContext *ctx, uint32_t face, uint32_t mode);
extern void NativeGLFogf(GLContext *ctx, uint32_t pname, float param);
extern void NativeGLFogfv(GLContext *ctx, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLFogi(GLContext *ctx, uint32_t pname, int32_t param);
extern void NativeGLFogiv(GLContext *ctx, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLTexEnvf(GLContext *ctx, uint32_t target, uint32_t pname, float param);
extern void NativeGLTexEnvfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLTexEnvi(GLContext *ctx, uint32_t target, uint32_t pname, int32_t param);
extern void NativeGLTexEnviv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLTexGeni(GLContext *ctx, uint32_t coord, uint32_t pname, int32_t param);
extern void NativeGLTexGenf(GLContext *ctx, uint32_t coord, uint32_t pname, float param);
extern void NativeGLTexGend(GLContext *ctx, uint32_t coord, uint32_t pname, double param);
extern void NativeGLTexGenfv(GLContext *ctx, uint32_t coord, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLTexGeniv(GLContext *ctx, uint32_t coord, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLTexGendv(GLContext *ctx, uint32_t coord, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLPixelStorei(GLContext *ctx, uint32_t pname, int32_t param);
extern void NativeGLPixelStoref(GLContext *ctx, uint32_t pname, float param);
// ---- Texture management externs (implemented in gl_state.cpp) ----
extern void NativeGLGenTextures(GLContext *ctx, uint32_t n, uint32_t mac_ptr);
extern void NativeGLDeleteTextures(GLContext *ctx, uint32_t n, uint32_t mac_ptr);
extern void NativeGLBindTexture(GLContext *ctx, uint32_t target, uint32_t texture);
extern void NativeGLTexParameteri(GLContext *ctx, uint32_t target, uint32_t pname, int32_t param);
extern void NativeGLTexParameterf(GLContext *ctx, uint32_t target, uint32_t pname, float param);
extern void NativeGLTexParameterfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLTexParameteriv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLTexImage2D(GLContext *ctx, uint32_t target, int32_t level,
                                int32_t internalformat, int32_t width, int32_t height,
                                int32_t border, uint32_t format, uint32_t type,
                                uint32_t mac_pixels);
extern void NativeGLTexSubImage2D(GLContext *ctx, uint32_t target, int32_t level,
                                   int32_t xoffset, int32_t yoffset,
                                   int32_t width, int32_t height,
                                   uint32_t format, uint32_t type, uint32_t mac_pixels);
extern void NativeGLTexImage1D(GLContext *ctx, uint32_t target, int32_t level,
                                int32_t internalformat, int32_t width, int32_t border,
                                uint32_t format, uint32_t type, uint32_t mac_pixels);
extern void NativeGLTexSubImage1D(GLContext *ctx, uint32_t target, int32_t level,
                                   int32_t xoffset, int32_t width,
                                   uint32_t format, uint32_t type, uint32_t mac_pixels);
extern void NativeGLCopyTexImage2D(GLContext *ctx, uint32_t target, int32_t level,
                                    uint32_t internalformat, int32_t x, int32_t y,
                                    int32_t width, int32_t height, int32_t border);
extern void NativeGLCopyTexSubImage2D(GLContext *ctx, uint32_t target, int32_t level,
                                       int32_t xoffset, int32_t yoffset,
                                       int32_t x, int32_t y, int32_t width, int32_t height);
extern uint32_t NativeGLIsTexture(GLContext *ctx, uint32_t texture);
extern void NativeGLActiveTextureARB(GLContext *ctx, uint32_t texture);
extern void NativeGLClientActiveTextureARB(GLContext *ctx, uint32_t texture);
// ---- Metal renderer texture externs (implemented in gl_metal_renderer.mm) ----
extern void NativeGLReadPixels(GLContext *ctx, int32_t x, int32_t y, int32_t width, int32_t height,
                                uint32_t format, uint32_t type, uint32_t mac_pixels);
extern void NativeGLGetIntegerv(GLContext *ctx, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLGetFloatv(GLContext *ctx, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLGetBooleanv(GLContext *ctx, uint32_t pname, uint32_t mac_ptr);
extern uint32_t NativeGLGetError(GLContext *ctx);
extern uint32_t NativeGLGetString(GLContext *ctx, uint32_t name);
extern void NativeGLPushAttrib(GLContext *ctx, uint32_t mask);
extern void NativeGLPopAttrib(GLContext *ctx);
extern void NativeGLPushClientAttrib(GLContext *ctx, uint32_t mask);
extern void NativeGLPopClientAttrib(GLContext *ctx);

// ---- Metal renderer / immediate mode externs (implemented in gl_metal_renderer.mm) ----
extern void NativeGLBegin(GLContext *ctx, uint32_t mode);
extern void NativeGLEnd(GLContext *ctx);
extern void NativeGLVertex2f(GLContext *ctx, float x, float y);
extern void NativeGLVertex3f(GLContext *ctx, float x, float y, float z);
extern void NativeGLVertex4f(GLContext *ctx, float x, float y, float z, float w);
extern void NativeGLVertex2d(GLContext *ctx, double x, double y);
extern void NativeGLVertex3d(GLContext *ctx, double x, double y, double z);
extern void NativeGLVertex4d(GLContext *ctx, double x, double y, double z, double w);
extern void NativeGLVertex2i(GLContext *ctx, int32_t x, int32_t y);
extern void NativeGLVertex3i(GLContext *ctx, int32_t x, int32_t y, int32_t z);
extern void NativeGLVertex4i(GLContext *ctx, int32_t x, int32_t y, int32_t z, int32_t w);
extern void NativeGLVertex2s(GLContext *ctx, int16_t x, int16_t y);
extern void NativeGLVertex3s(GLContext *ctx, int16_t x, int16_t y, int16_t z);
extern void NativeGLVertex4s(GLContext *ctx, int16_t x, int16_t y, int16_t z, int16_t w);
extern void NativeGLVertex2fv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLVertex3fv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLVertex4fv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLVertex2dv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLVertex3dv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLVertex4dv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLVertex2iv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLVertex3iv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLVertex4iv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLVertex2sv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLVertex3sv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLVertex4sv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLColor3f(GLContext *ctx, float r, float g, float b);
extern void NativeGLColor4f(GLContext *ctx, float r, float g, float b, float a);
extern void NativeGLColor3d(GLContext *ctx, double r, double g, double b);
extern void NativeGLColor4d(GLContext *ctx, double r, double g, double b, double a);
extern void NativeGLColor3b(GLContext *ctx, int8_t r, int8_t g, int8_t b);
extern void NativeGLColor4b(GLContext *ctx, int8_t r, int8_t g, int8_t b, int8_t a);
extern void NativeGLColor3ub(GLContext *ctx, uint8_t r, uint8_t g, uint8_t b);
extern void NativeGLColor4ub(GLContext *ctx, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
extern void NativeGLColor3i(GLContext *ctx, int32_t r, int32_t g, int32_t b);
extern void NativeGLColor4i(GLContext *ctx, int32_t r, int32_t g, int32_t b, int32_t a);
extern void NativeGLColor3s(GLContext *ctx, int16_t r, int16_t g, int16_t b);
extern void NativeGLColor4s(GLContext *ctx, int16_t r, int16_t g, int16_t b, int16_t a);
extern void NativeGLColor3ui(GLContext *ctx, uint32_t r, uint32_t g, uint32_t b);
extern void NativeGLColor4ui(GLContext *ctx, uint32_t r, uint32_t g, uint32_t b, uint32_t a);
extern void NativeGLColor3us(GLContext *ctx, uint16_t r, uint16_t g, uint16_t b);
extern void NativeGLColor4us(GLContext *ctx, uint16_t r, uint16_t g, uint16_t b, uint16_t a);
extern void NativeGLColor3fv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLColor4fv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLColor3bv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLColor4bv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLColor3ubv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLColor4ubv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLColor3dv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLColor4dv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLColor3iv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLColor4iv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLColor3sv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLColor4sv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLColor3uiv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLColor4uiv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLColor3usv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLColor4usv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLNormal3f(GLContext *ctx, float x, float y, float z);
extern void NativeGLNormal3d(GLContext *ctx, double x, double y, double z);
extern void NativeGLNormal3b(GLContext *ctx, int8_t x, int8_t y, int8_t z);
extern void NativeGLNormal3i(GLContext *ctx, int32_t x, int32_t y, int32_t z);
extern void NativeGLNormal3s(GLContext *ctx, int16_t x, int16_t y, int16_t z);
extern void NativeGLNormal3fv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLNormal3dv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLNormal3bv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLNormal3iv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLNormal3sv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLTexCoord1f(GLContext *ctx, float s);
extern void NativeGLTexCoord2f(GLContext *ctx, float s, float t);
extern void NativeGLTexCoord3f(GLContext *ctx, float s, float t, float r);
extern void NativeGLTexCoord4f(GLContext *ctx, float s, float t, float r, float q);
extern void NativeGLTexCoord1d(GLContext *ctx, double s);
extern void NativeGLTexCoord2d(GLContext *ctx, double s, double t);
extern void NativeGLTexCoord3d(GLContext *ctx, double s, double t, double r);
extern void NativeGLTexCoord4d(GLContext *ctx, double s, double t, double r, double q);
extern void NativeGLTexCoord1i(GLContext *ctx, int32_t s);
extern void NativeGLTexCoord2i(GLContext *ctx, int32_t s, int32_t t);
extern void NativeGLTexCoord3i(GLContext *ctx, int32_t s, int32_t t, int32_t r);
extern void NativeGLTexCoord4i(GLContext *ctx, int32_t s, int32_t t, int32_t r, int32_t q);
extern void NativeGLTexCoord1s(GLContext *ctx, int16_t s);
extern void NativeGLTexCoord2s(GLContext *ctx, int16_t s, int16_t t);
extern void NativeGLTexCoord3s(GLContext *ctx, int16_t s, int16_t t, int16_t r);
extern void NativeGLTexCoord4s(GLContext *ctx, int16_t s, int16_t t, int16_t r, int16_t q);
extern void NativeGLTexCoord1fv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLTexCoord2fv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLTexCoord3fv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLTexCoord4fv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLTexCoord1dv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLTexCoord2dv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLTexCoord3dv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLTexCoord4dv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLTexCoord1iv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLTexCoord2iv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLTexCoord3iv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLTexCoord4iv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLTexCoord1sv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLTexCoord2sv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLTexCoord3sv(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLTexCoord4sv(GLContext *ctx, uint32_t mac_ptr);

// ---- Remaining core GL externs (gl_state.cpp) ----
extern void NativeGLAccum(GLContext *ctx, uint32_t op, float value);
extern void NativeGLClearAccum(GLContext *ctx, float r, float g, float b, float a);
extern void NativeGLClearIndex(GLContext *ctx, float c);
extern void NativeGLClipPlane(GLContext *ctx, uint32_t plane, uint32_t mac_ptr);
extern void NativeGLGetClipPlane(GLContext *ctx, uint32_t plane, uint32_t mac_ptr);
extern void NativeGLRasterPos2f(GLContext *ctx, float x, float y);
extern void NativeGLRasterPos3f(GLContext *ctx, float x, float y, float z);
extern void NativeGLRasterPos4f(GLContext *ctx, float x, float y, float z, float w);
extern void NativeGLRasterPos2d(GLContext *ctx, double x, double y);
extern void NativeGLRasterPos3d(GLContext *ctx, double x, double y, double z);
extern void NativeGLRasterPos4d(GLContext *ctx, double x, double y, double z, double w);
extern void NativeGLRasterPos2i(GLContext *ctx, int32_t x, int32_t y);
extern void NativeGLRasterPos3i(GLContext *ctx, int32_t x, int32_t y, int32_t z);
extern void NativeGLRasterPos4i(GLContext *ctx, int32_t x, int32_t y, int32_t z, int32_t w);
extern void NativeGLRasterPos2s(GLContext *ctx, int16_t x, int16_t y);
extern void NativeGLRasterPos3s(GLContext *ctx, int16_t x, int16_t y, int16_t z);
extern void NativeGLRasterPos4s(GLContext *ctx, int16_t x, int16_t y, int16_t z, int16_t w);
extern void NativeGLRasterPos2fv(GLContext *ctx, uint32_t p);
extern void NativeGLRasterPos3fv(GLContext *ctx, uint32_t p);
extern void NativeGLRasterPos4fv(GLContext *ctx, uint32_t p);
extern void NativeGLRasterPos2dv(GLContext *ctx, uint32_t p);
extern void NativeGLRasterPos3dv(GLContext *ctx, uint32_t p);
extern void NativeGLRasterPos4dv(GLContext *ctx, uint32_t p);
extern void NativeGLRasterPos2iv(GLContext *ctx, uint32_t p);
extern void NativeGLRasterPos3iv(GLContext *ctx, uint32_t p);
extern void NativeGLRasterPos4iv(GLContext *ctx, uint32_t p);
extern void NativeGLRasterPos2sv(GLContext *ctx, uint32_t p);
extern void NativeGLRasterPos3sv(GLContext *ctx, uint32_t p);
extern void NativeGLRasterPos4sv(GLContext *ctx, uint32_t p);
extern void NativeGLBitmap(GLContext *ctx, int32_t w, int32_t h, float xo, float yo, float xm, float ym, uint32_t bm);
extern void NativeGLDrawPixels(GLContext *ctx, int32_t w, int32_t h, uint32_t fmt, uint32_t type, uint32_t px);
extern void NativeGLCopyPixels(GLContext *ctx, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t type);
extern void NativeGLReadBuffer(GLContext *ctx, uint32_t mode);
extern void NativeGLDrawBuffer(GLContext *ctx, uint32_t mode);
extern void NativeGLRectf(GLContext *ctx, float x1, float y1, float x2, float y2);
extern void NativeGLRectd(GLContext *ctx, double x1, double y1, double x2, double y2);
extern void NativeGLRecti(GLContext *ctx, int32_t x1, int32_t y1, int32_t x2, int32_t y2);
extern void NativeGLRects(GLContext *ctx, int16_t x1, int16_t y1, int16_t x2, int16_t y2);
extern void NativeGLRectfv(GLContext *ctx, uint32_t v1, uint32_t v2);
extern void NativeGLRectdv(GLContext *ctx, uint32_t v1, uint32_t v2);
extern void NativeGLRectiv(GLContext *ctx, uint32_t v1, uint32_t v2);
extern void NativeGLRectsv(GLContext *ctx, uint32_t v1, uint32_t v2);
extern void NativeGLMap1f(GLContext *ctx, uint32_t t, float u1, float u2, int32_t s, int32_t o, uint32_t p);
extern void NativeGLMap1d(GLContext *ctx, uint32_t t, double u1, double u2, int32_t s, int32_t o, uint32_t p);
extern void NativeGLMap2f(GLContext *ctx, uint32_t t, float u1, float u2, int32_t us, int32_t uo, float v1, float v2, int32_t vs, int32_t vo, uint32_t p);
extern void NativeGLMap2d(GLContext *ctx, uint32_t t, double u1, double u2, int32_t us, int32_t uo, double v1, double v2, int32_t vs, int32_t vo, uint32_t p);
extern void NativeGLMapGrid1f(GLContext *ctx, int32_t un, float u1, float u2);
extern void NativeGLMapGrid1d(GLContext *ctx, int32_t un, double u1, double u2);
extern void NativeGLMapGrid2f(GLContext *ctx, int32_t un, float u1, float u2, int32_t vn, float v1, float v2);
extern void NativeGLMapGrid2d(GLContext *ctx, int32_t un, double u1, double u2, int32_t vn, double v1, double v2);
extern void NativeGLEvalCoord1f(GLContext *ctx, float u);
extern void NativeGLEvalCoord1d(GLContext *ctx, double u);
extern void NativeGLEvalCoord2f(GLContext *ctx, float u, float v);
extern void NativeGLEvalCoord2d(GLContext *ctx, double u, double v);
extern void NativeGLEvalCoord1fv(GLContext *ctx, uint32_t p);
extern void NativeGLEvalCoord1dv(GLContext *ctx, uint32_t p);
extern void NativeGLEvalCoord2fv(GLContext *ctx, uint32_t p);
extern void NativeGLEvalCoord2dv(GLContext *ctx, uint32_t p);
extern void NativeGLEvalMesh1(GLContext *ctx, uint32_t mode, int32_t i1, int32_t i2);
extern void NativeGLEvalMesh2(GLContext *ctx, uint32_t mode, int32_t i1, int32_t i2, int32_t j1, int32_t j2);
extern void NativeGLEvalPoint1(GLContext *ctx, int32_t i);
extern void NativeGLEvalPoint2(GLContext *ctx, int32_t i, int32_t j);
extern uint32_t NativeGLRenderMode(GLContext *ctx, uint32_t mode);
extern void NativeGLSelectBuffer(GLContext *ctx, int32_t size, uint32_t buffer_ptr);
extern void NativeGLFeedbackBuffer(GLContext *ctx, int32_t size, uint32_t type, uint32_t buffer_ptr);
extern void NativeGLInitNames(GLContext *ctx);
extern void NativeGLPushName(GLContext *ctx, uint32_t name);
extern void NativeGLPopName(GLContext *ctx);
extern void NativeGLLoadName(GLContext *ctx, uint32_t name);
extern void NativeGLPassThrough(GLContext *ctx, float token);
extern void NativeGLPixelTransferf(GLContext *ctx, uint32_t pname, float param);
extern void NativeGLPixelTransferi(GLContext *ctx, uint32_t pname, int32_t param);
extern void NativeGLPixelMapfv(GLContext *ctx, uint32_t map, int32_t mapsize, uint32_t values);
extern void NativeGLPixelMapuiv(GLContext *ctx, uint32_t map, int32_t mapsize, uint32_t values);
extern void NativeGLPixelMapusv(GLContext *ctx, uint32_t map, int32_t mapsize, uint32_t values);
extern void NativeGLGetPixelMapfv(GLContext *ctx, uint32_t map, uint32_t values);
extern void NativeGLGetPixelMapuiv(GLContext *ctx, uint32_t map, uint32_t values);
extern void NativeGLGetPixelMapusv(GLContext *ctx, uint32_t map, uint32_t values);
extern void NativeGLPixelZoom(GLContext *ctx, float xfactor, float yfactor);
extern void NativeGLIndexf(GLContext *ctx, float c);
extern void NativeGLIndexd(GLContext *ctx, double c);
extern void NativeGLIndexi(GLContext *ctx, int32_t c);
extern void NativeGLIndexs(GLContext *ctx, int16_t c);
extern void NativeGLIndexub(GLContext *ctx, uint8_t c);
extern void NativeGLIndexfv(GLContext *ctx, uint32_t p);
extern void NativeGLIndexdv(GLContext *ctx, uint32_t p);
extern void NativeGLIndexiv(GLContext *ctx, uint32_t p);
extern void NativeGLIndexsv(GLContext *ctx, uint32_t p);
extern void NativeGLIndexubv(GLContext *ctx, uint32_t p);
extern void NativeGLIndexMask(GLContext *ctx, uint32_t mask);
extern void NativeGLIndexPointer(GLContext *ctx, uint32_t type, int32_t stride, uint32_t pointer);
extern void NativeGLEdgeFlag(GLContext *ctx, uint32_t flag);
extern void NativeGLEdgeFlagv(GLContext *ctx, uint32_t p);
extern void NativeGLEdgeFlagPointer(GLContext *ctx, int32_t stride, uint32_t pointer);
extern void NativeGLLineStipple(GLContext *ctx, int32_t factor, uint32_t pattern);
extern void NativeGLPolygonStipple(GLContext *ctx, uint32_t mask_ptr);
extern void NativeGLGetPolygonStipple(GLContext *ctx, uint32_t mask_ptr);
extern void NativeGLAreTexturesResident(GLContext *ctx, int32_t n, uint32_t textures_ptr, uint32_t residences_ptr);
extern void NativeGLPrioritizeTextures(GLContext *ctx, int32_t n, uint32_t textures_ptr, uint32_t priorities_ptr);
extern void NativeGLNewList(GLContext *ctx, uint32_t list, uint32_t mode);
extern void NativeGLEndList(GLContext *ctx);
extern void NativeGLCallList(GLContext *ctx, uint32_t list);
extern void NativeGLCallLists(GLContext *ctx, int32_t n, uint32_t type, uint32_t lists_ptr);
extern uint32_t NativeGLGenLists(GLContext *ctx, int32_t range);
extern void NativeGLDeleteLists(GLContext *ctx, uint32_t list, int32_t range);
extern uint32_t NativeGLIsList(GLContext *ctx, uint32_t list);
extern void NativeGLListBase(GLContext *ctx, uint32_t base);
extern void NativeGLVertexPointer(GLContext *ctx, int32_t size, uint32_t type, int32_t stride, uint32_t pointer);
extern void NativeGLNormalPointer(GLContext *ctx, uint32_t type, int32_t stride, uint32_t pointer);
extern void NativeGLColorPointer(GLContext *ctx, int32_t size, uint32_t type, int32_t stride, uint32_t pointer);
extern void NativeGLTexCoordPointer(GLContext *ctx, int32_t size, uint32_t type, int32_t stride, uint32_t pointer);
extern void NativeGLEnableClientState(GLContext *ctx, uint32_t array);
extern void NativeGLDisableClientState(GLContext *ctx, uint32_t array);
extern void NativeGLArrayElement(GLContext *ctx, int32_t i);
extern void NativeGLDrawArrays(GLContext *ctx, uint32_t mode, int32_t first, int32_t count);
extern void NativeGLDrawElements(GLContext *ctx, uint32_t mode, int32_t count, uint32_t type, uint32_t indices_ptr);
extern void NativeGLInterleavedArrays(GLContext *ctx, uint32_t format, int32_t stride, uint32_t pointer);
extern void NativeGLDrawRangeElements(GLContext *ctx, uint32_t mode, uint32_t start, uint32_t end, int32_t count, uint32_t type, uint32_t indices_ptr);
extern void NativeGLCopyTexImage1D(GLContext *ctx, uint32_t target, int32_t level, uint32_t ifmt, int32_t x, int32_t y, int32_t w, int32_t border);
extern void NativeGLCopyTexSubImage1D(GLContext *ctx, uint32_t target, int32_t level, int32_t xoff, int32_t x, int32_t y, int32_t w);
extern void NativeGLGetTexEnvfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLGetTexEnviv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLGetTexGendv(GLContext *ctx, uint32_t coord, uint32_t pname, uint32_t p);
extern void NativeGLGetTexGenfv(GLContext *ctx, uint32_t coord, uint32_t pname, uint32_t p);
extern void NativeGLGetTexGeniv(GLContext *ctx, uint32_t coord, uint32_t pname, uint32_t p);
extern void NativeGLGetTexImage(GLContext *ctx, uint32_t target, int32_t level, uint32_t fmt, uint32_t type, uint32_t p);
extern void NativeGLGetTexLevelParameterfv(GLContext *ctx, uint32_t target, int32_t level, uint32_t pname, uint32_t p);
extern void NativeGLGetTexLevelParameteriv(GLContext *ctx, uint32_t target, int32_t level, uint32_t pname, uint32_t p);
extern void NativeGLGetTexParameterfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLGetTexParameteriv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLGetLightfv(GLContext *ctx, uint32_t light, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLGetLightiv(GLContext *ctx, uint32_t light, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLGetMaterialfv(GLContext *ctx, uint32_t face, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLGetMaterialiv(GLContext *ctx, uint32_t face, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLGetMapdv(GLContext *ctx, uint32_t target, uint32_t query, uint32_t p);
extern void NativeGLGetMapfv(GLContext *ctx, uint32_t target, uint32_t query, uint32_t p);
extern void NativeGLGetMapiv(GLContext *ctx, uint32_t target, uint32_t query, uint32_t p);
extern void NativeGLGetDoublev(GLContext *ctx, uint32_t pname, uint32_t mac_ptr);
extern void NativeGLGetPointerv(GLContext *ctx, uint32_t pname, uint32_t mac_ptr);
// ---- Extension externs (gl_state.cpp) ----
extern void NativeGLBlendColorEXT(GLContext *ctx, float r, float g, float b, float a);
extern void NativeGLBlendEquationEXT(GLContext *ctx, uint32_t mode);
extern void NativeGLLockArraysEXT(GLContext *ctx, int32_t first, int32_t count);
extern void NativeGLUnlockArraysEXT(GLContext *ctx);
// ARB_multitexture
extern void NativeGLMultiTexCoord1dARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord1dvARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord1fARB(GLContext *ctx, uint32_t target, float s);
extern void NativeGLMultiTexCoord1fvARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord1iARB(GLContext *ctx, uint32_t target, int32_t s);
extern void NativeGLMultiTexCoord1ivARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord1sARB(GLContext *ctx, uint32_t target, int16_t s);
extern void NativeGLMultiTexCoord1svARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord2dARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord2dvARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord2fARB(GLContext *ctx, uint32_t target, float s, float t);
extern void NativeGLMultiTexCoord2fvARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord2iARB(GLContext *ctx, uint32_t target, int32_t s, int32_t t);
extern void NativeGLMultiTexCoord2ivARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord2sARB(GLContext *ctx, uint32_t target, int16_t s, int16_t t);
extern void NativeGLMultiTexCoord2svARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord3dARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord3dvARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord3fARB(GLContext *ctx, uint32_t target, float s, float t, float r);
extern void NativeGLMultiTexCoord3fvARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord3iARB(GLContext *ctx, uint32_t target, int32_t s, int32_t t, int32_t r);
extern void NativeGLMultiTexCoord3ivARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord3sARB(GLContext *ctx, uint32_t target, int16_t s, int16_t t, int16_t r);
extern void NativeGLMultiTexCoord3svARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord4dARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord4dvARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord4fARB(GLContext *ctx, uint32_t target, float s, float t, float r, float q);
extern void NativeGLMultiTexCoord4fvARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord4iARB(GLContext *ctx, uint32_t target, int32_t s, int32_t t, int32_t r, int32_t q);
extern void NativeGLMultiTexCoord4ivARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
extern void NativeGLMultiTexCoord4sARB(GLContext *ctx, uint32_t target, int16_t s, int16_t t, int16_t r, int16_t q);
extern void NativeGLMultiTexCoord4svARB(GLContext *ctx, uint32_t target, uint32_t mac_ptr);
// ARB_transpose_matrix
extern void NativeGLLoadTransposeMatrixdARB(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLLoadTransposeMatrixfARB(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLMultTransposeMatrixdARB(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLMultTransposeMatrixfARB(GLContext *ctx, uint32_t mac_ptr);
// ARB_texture_compression
extern void NativeGLCompressedTexImage3DARB(GLContext *ctx, uint32_t target, int32_t level, uint32_t ifmt, int32_t w, int32_t h, int32_t d, int32_t border, int32_t imageSize, uint32_t data_ptr);
extern void NativeGLCompressedTexImage2DARB(GLContext *ctx, uint32_t target, int32_t level, uint32_t ifmt, int32_t w, int32_t h, int32_t border, int32_t imageSize, uint32_t data_ptr);
extern void NativeGLCompressedTexImage1DARB(GLContext *ctx, uint32_t target, int32_t level, uint32_t ifmt, int32_t w, int32_t border, int32_t imageSize, uint32_t data_ptr);
extern void NativeGLCompressedTexSubImage3DARB(GLContext *ctx, uint32_t target, int32_t level, int32_t xoff, int32_t yoff, int32_t zoff, int32_t w, int32_t h, int32_t d, uint32_t fmt, int32_t imageSize, uint32_t data_ptr);
extern void NativeGLCompressedTexSubImage2DARB(GLContext *ctx, uint32_t target, int32_t level, int32_t xoff, int32_t yoff, int32_t w, int32_t h, uint32_t fmt, int32_t imageSize, uint32_t data_ptr);
extern void NativeGLCompressedTexSubImage1DARB(GLContext *ctx, uint32_t target, int32_t level, int32_t xoff, int32_t w, uint32_t fmt, int32_t imageSize, uint32_t data_ptr);
extern void NativeGLGetCompressedTexImageARB(GLContext *ctx, uint32_t target, int32_t level, uint32_t img_ptr);
// EXT_secondary_color
extern void NativeGLSecondaryColor3bEXT(GLContext *ctx, int8_t r, int8_t g, int8_t b);
extern void NativeGLSecondaryColor3bvEXT(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLSecondaryColor3dEXT(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLSecondaryColor3dvEXT(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLSecondaryColor3fEXT(GLContext *ctx, float r, float g, float b);
extern void NativeGLSecondaryColor3fvEXT(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLSecondaryColor3iEXT(GLContext *ctx, int32_t r, int32_t g, int32_t b);
extern void NativeGLSecondaryColor3ivEXT(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLSecondaryColor3sEXT(GLContext *ctx, int16_t r, int16_t g, int16_t b);
extern void NativeGLSecondaryColor3svEXT(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLSecondaryColor3ubEXT(GLContext *ctx, uint8_t r, uint8_t g, uint8_t b);
extern void NativeGLSecondaryColor3ubvEXT(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLSecondaryColor3uiEXT(GLContext *ctx, uint32_t r, uint32_t g, uint32_t b);
extern void NativeGLSecondaryColor3uivEXT(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLSecondaryColor3usEXT(GLContext *ctx, uint16_t r, uint16_t g, uint16_t b);
extern void NativeGLSecondaryColor3usvEXT(GLContext *ctx, uint32_t mac_ptr);
extern void NativeGLSecondaryColorPointerEXT(GLContext *ctx, int32_t size, uint32_t type, int32_t stride, uint32_t pointer);
// OpenGL 1.2 imaging subset
extern void NativeGLColorTable(GLContext *ctx, uint32_t target, uint32_t ifmt, int32_t w, uint32_t fmt, uint32_t type, uint32_t data);
extern void NativeGLColorTableParameterfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params);
extern void NativeGLColorTableParameteriv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params);
extern void NativeGLCopyColorTable(GLContext *ctx, uint32_t target, uint32_t ifmt, int32_t x, int32_t y, int32_t w);
extern void NativeGLGetColorTable(GLContext *ctx, uint32_t target, uint32_t fmt, uint32_t type, uint32_t data);
extern void NativeGLGetColorTableParameterfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params);
extern void NativeGLGetColorTableParameteriv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params);
extern void NativeGLColorSubTable(GLContext *ctx, uint32_t target, int32_t start, int32_t count, uint32_t fmt, uint32_t type, uint32_t data);
extern void NativeGLCopyColorSubTable(GLContext *ctx, uint32_t target, int32_t start, int32_t x, int32_t y, int32_t w);
extern void NativeGLConvolutionFilter1D(GLContext *ctx, uint32_t target, uint32_t ifmt, int32_t w, uint32_t fmt, uint32_t type, uint32_t data);
extern void NativeGLConvolutionFilter2D(GLContext *ctx, uint32_t target, uint32_t ifmt, int32_t w, int32_t h, uint32_t fmt, uint32_t type, uint32_t data);
extern void NativeGLConvolutionParameterf(GLContext *ctx, uint32_t target, uint32_t pname, float param);
extern void NativeGLConvolutionParameterfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params);
extern void NativeGLConvolutionParameteri(GLContext *ctx, uint32_t target, uint32_t pname, int32_t param);
extern void NativeGLConvolutionParameteriv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params);
extern void NativeGLCopyConvolutionFilter1D(GLContext *ctx, uint32_t target, uint32_t ifmt, int32_t x, int32_t y, int32_t w);
extern void NativeGLCopyConvolutionFilter2D(GLContext *ctx, uint32_t target, uint32_t ifmt, int32_t x, int32_t y, int32_t w, int32_t h);
extern void NativeGLGetConvolutionFilter(GLContext *ctx, uint32_t target, uint32_t fmt, uint32_t type, uint32_t data);
extern void NativeGLGetConvolutionParameterfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params);
extern void NativeGLGetConvolutionParameteriv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params);
extern void NativeGLGetSeparableFilter(GLContext *ctx, uint32_t target, uint32_t fmt, uint32_t type, uint32_t row, uint32_t col, uint32_t span);
extern void NativeGLSeparableFilter2D(GLContext *ctx, uint32_t target, uint32_t ifmt, int32_t w, int32_t h, uint32_t fmt, uint32_t type, uint32_t row, uint32_t col);
extern void NativeGLGetHistogram(GLContext *ctx, uint32_t target, uint32_t reset, uint32_t fmt, uint32_t type, uint32_t data);
extern void NativeGLGetHistogramParameterfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params);
extern void NativeGLGetHistogramParameteriv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params);
extern void NativeGLGetMinmax(GLContext *ctx, uint32_t target, uint32_t reset, uint32_t fmt, uint32_t type, uint32_t data);
extern void NativeGLGetMinmaxParameterfv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params);
extern void NativeGLGetMinmaxParameteriv(GLContext *ctx, uint32_t target, uint32_t pname, uint32_t params);
extern void NativeGLHistogram(GLContext *ctx, uint32_t target, int32_t width, uint32_t ifmt, uint32_t sink);
extern void NativeGLMinmax(GLContext *ctx, uint32_t target, uint32_t ifmt, uint32_t sink);
extern void NativeGLResetHistogram(GLContext *ctx, uint32_t target);
extern void NativeGLResetMinmax(GLContext *ctx, uint32_t target);
extern void NativeGLTexImage3DEXT(GLContext *ctx, uint32_t target, int32_t level, int32_t ifmt, int32_t w, int32_t h, int32_t d, int32_t border, uint32_t fmt, uint32_t type, uint32_t data);
extern void NativeGLTexSubImage3DEXT(GLContext *ctx, uint32_t target, int32_t level, int32_t xoff, int32_t yoff, int32_t zoff, int32_t w, int32_t h, int32_t d, uint32_t fmt, uint32_t type, uint32_t data);
extern void NativeGLCopyTexSubImage3DEXT(GLContext *ctx, uint32_t target, int32_t level, int32_t xoff, int32_t yoff, int32_t zoff, int32_t x, int32_t y, int32_t w, int32_t h);

// ---- Metal renderer externs (gl_metal_renderer.mm) ----
extern void NativeGLFinish(GLContext *ctx);
extern void NativeGLFlush(GLContext *ctx);

// ---- AGL handler externs (implemented in gl_engine.cpp) ----
extern uint32_t NativeAGLChoosePixelFormat(uint32_t gdevs, uint32_t ndev, uint32_t attribs);
extern uint32_t NativeAGLCreateContext(uint32_t pixelFormat, uint32_t shareContext);
extern uint32_t NativeAGLSetCurrentContext(uint32_t ctx);
extern uint32_t NativeAGLSetDrawable(uint32_t ctx, uint32_t drawable);
extern uint32_t NativeAGLSwapBuffers(uint32_t ctx);
extern uint32_t NativeAGLDestroyContext(uint32_t ctx);
extern uint32_t NativeAGLGetCurrentContext();
extern uint32_t NativeAGLGetError();
extern uint32_t NativeAGLGetVersion(uint32_t majorPtr, uint32_t minorPtr);
extern uint32_t NativeAGLDestroyPixelFormat(uint32_t pix);
extern uint32_t NativeAGLNextPixelFormat(uint32_t pix);
extern uint32_t NativeAGLDescribePixelFormat(uint32_t pix, uint32_t attrib, uint32_t valuePtr);
extern uint32_t NativeAGLCopyContext(uint32_t src, uint32_t dst, uint32_t mask);
extern uint32_t NativeAGLUpdateContext(uint32_t ctx);
extern uint32_t NativeAGLSetOffScreen(uint32_t ctx, uint32_t width, uint32_t height,
                                       uint32_t rowbytes, uint32_t baseaddr);
extern uint32_t NativeAGLSetFullScreen(uint32_t ctx, uint32_t width, uint32_t height,
                                        uint32_t freq, uint32_t device);
extern uint32_t NativeAGLGetDrawable(uint32_t ctx);
extern uint32_t NativeAGLSetVirtualScreen(uint32_t ctx, uint32_t screen);
extern uint32_t NativeAGLGetVirtualScreen(uint32_t ctx);
extern uint32_t NativeAGLConfigure(uint32_t pname, uint32_t param);
extern uint32_t NativeAGLEnable(uint32_t ctx, uint32_t pname);
extern uint32_t NativeAGLDisable(uint32_t ctx, uint32_t pname);
extern uint32_t NativeAGLIsEnabled(uint32_t ctx, uint32_t pname);
extern uint32_t NativeAGLSetInteger(uint32_t ctx, uint32_t pname, uint32_t params);
extern uint32_t NativeAGLGetInteger(uint32_t ctx, uint32_t pname, uint32_t params);
extern uint32_t NativeAGLUseFont(uint32_t ctx, uint32_t fontID, uint32_t face,
                                  uint32_t size, uint32_t first, uint32_t count, uint32_t base);
extern uint32_t NativeAGLErrorString(uint32_t code);
extern uint32_t NativeAGLResetLibrary();
extern uint32_t NativeAGLQueryRendererInfo(uint32_t gdevs, uint32_t ndev);
extern uint32_t NativeAGLDestroyRendererInfo(uint32_t rend);
extern uint32_t NativeAGLNextRendererInfo(uint32_t rend);
extern uint32_t NativeAGLDescribeRenderer(uint32_t rend, uint32_t prop, uint32_t valuePtr);
extern uint32_t NativeAGLDevicesOfPixelFormat(uint32_t pix, uint32_t ndevsPtr);

// ---- GLU handler externs (implemented in gl_engine.cpp) ----
extern void NativeGLUPerspective(GLContext *ctx, double fovy, double aspect, double zNear, double zFar);
extern void NativeGLULookAt(GLContext *ctx, double eyeX, double eyeY, double eyeZ,
                            double centerX, double centerY, double centerZ,
                            double upX, double upY, double upZ);
extern void NativeGLUOrtho2D(GLContext *ctx, double left, double right, double bottom, double top);
extern void NativeGLUPickMatrix(GLContext *ctx, double x, double y, double deltaX, double deltaY, uint32_t viewport_ptr);
extern uint32_t NativeGLUProject(GLContext *ctx, double objX, double objY, double objZ,
                                  uint32_t model_ptr, uint32_t proj_ptr, uint32_t viewport_ptr,
                                  uint32_t winX_ptr, uint32_t winY_ptr, uint32_t winZ_ptr);
extern uint32_t NativeGLUUnProject(GLContext *ctx, double winX, double winY, double winZ,
                                    uint32_t model_ptr, uint32_t proj_ptr, uint32_t viewport_ptr,
                                    uint32_t objX_ptr, uint32_t objY_ptr, uint32_t objZ_ptr);
extern uint32_t NativeGLUBuild2DMipmaps(GLContext *ctx, uint32_t target, int32_t internalFormat,
                                         int32_t width, int32_t height, uint32_t format, uint32_t type, uint32_t data_ptr);
extern uint32_t NativeGLUBuild1DMipmaps(GLContext *ctx, uint32_t target, int32_t internalFormat,
                                         int32_t width, uint32_t format, uint32_t type, uint32_t data_ptr);
extern uint32_t NativeGLUScaleImage(GLContext *ctx, uint32_t format,
                                     int32_t wIn, int32_t hIn, uint32_t typeIn, uint32_t dataIn,
                                     int32_t wOut, int32_t hOut, uint32_t typeOut, uint32_t dataOut);
extern uint32_t NativeGLUNewQuadric();
extern void NativeGLUDeleteQuadric(uint32_t quad_handle);
extern void NativeGLUQuadricNormals(uint32_t quad_handle, uint32_t normal);
extern void NativeGLUQuadricTexture(uint32_t quad_handle, uint32_t texture);
extern void NativeGLUQuadricDrawStyle(uint32_t quad_handle, uint32_t draw);
extern void NativeGLUQuadricOrientation(uint32_t quad_handle, uint32_t orient);
extern void NativeGLUQuadricCallback(uint32_t quad_handle, uint32_t which, uint32_t callback);
extern void NativeGLUSphere(GLContext *ctx, uint32_t quad_handle, double radius, int32_t slices, int32_t stacks);
extern void NativeGLUCylinder(GLContext *ctx, uint32_t quad_handle, double base, double top, double height, int32_t slices, int32_t stacks);
extern void NativeGLUDisk(GLContext *ctx, uint32_t quad_handle, double inner, double outer, int32_t slices, int32_t loops);
extern void NativeGLUPartialDisk(GLContext *ctx, uint32_t quad_handle, double inner, double outer, int32_t slices, int32_t loops, double start, double sweep);
extern uint32_t NativeGLUNewTess();
extern void NativeGLUDeleteTess(uint32_t tess_handle);
extern void NativeGLUTessBeginPolygon(uint32_t tess, uint32_t data);
extern void NativeGLUTessEndPolygon(uint32_t tess);
extern void NativeGLUTessBeginContour(uint32_t tess);
extern void NativeGLUTessEndContour(uint32_t tess);
extern void NativeGLUTessVertex(uint32_t tess, uint32_t location, uint32_t data);
extern void NativeGLUTessCallback(uint32_t tess, uint32_t which, uint32_t callback);
extern void NativeGLUTessProperty(uint32_t tess, uint32_t which, double data);
extern void NativeGLUTessNormal(uint32_t tess, double x, double y, double z);
extern void NativeGLUBeginPolygon(uint32_t tess);
extern void NativeGLUEndPolygon(uint32_t tess);
extern void NativeGLUNextContour(uint32_t tess, uint32_t type);
extern void NativeGLUGetTessProperty(uint32_t tess, uint32_t which, uint32_t data_ptr);
extern uint32_t NativeGLUNewNurbsRenderer();
extern uint32_t NativeGLUNewNurbsTessellatorEXT();
extern void NativeGLUDeleteNurbsRenderer(uint32_t nurb);
extern void NativeGLUDeleteNurbsTessellatorEXT(uint32_t nurb);
extern void NativeGLUBeginCurve(uint32_t nurb);
extern void NativeGLUEndCurve(uint32_t nurb);
extern void NativeGLUBeginSurface(uint32_t nurb);
extern void NativeGLUEndSurface(uint32_t nurb);
extern void NativeGLUBeginTrim(uint32_t nurb);
extern void NativeGLUEndTrim(uint32_t nurb);
extern void NativeGLUNurbsCallback(uint32_t nurb, uint32_t which, uint32_t callback);
extern void NativeGLUNurbsCallbackDataEXT(uint32_t nurb, uint32_t userData);
extern void NativeGLUNurbsCurve(uint32_t nurb, int32_t knotCount, uint32_t knots, int32_t stride, uint32_t control, int32_t order, uint32_t type);
extern void NativeGLUNurbsProperty(uint32_t nurb, uint32_t property, float value);
extern void NativeGLUNurbsSurface(uint32_t nurb, int32_t sKnots, uint32_t sKnotsPtr, int32_t tKnots, uint32_t tKnotsPtr, int32_t sStride, int32_t tStride, uint32_t control, int32_t sOrder, int32_t tOrder, uint32_t type);
extern void NativeGLUGetNurbsProperty(uint32_t nurb, uint32_t property, uint32_t data_ptr);
extern void NativeGLULoadSamplingMatrices(uint32_t nurb, uint32_t model, uint32_t persp, uint32_t view);
extern void NativeGLUPwlCurve(uint32_t nurb, int32_t count, uint32_t data, int32_t stride, uint32_t type);
extern uint32_t NativeGLUErrorString(uint32_t error);
extern uint32_t NativeGLUGetString(uint32_t name);

// ---- GLUT handler externs (implemented in gl_engine.cpp) ----
extern void NativeGLUTInitMac(uint32_t argcp, uint32_t argv);
extern void NativeGLUTInitDisplayMode(uint32_t mode);
extern void NativeGLUTInitDisplayString(uint32_t string_ptr);
extern void NativeGLUTInitWindowPosition(int32_t x, int32_t y);
extern void NativeGLUTInitWindowSize(int32_t width, int32_t height);
extern void NativeGLUTMainLoop();
extern uint32_t NativeGLUTCreateWindow(uint32_t title_ptr);
extern uint32_t NativeGLUTCreateSubWindow(int32_t win, int32_t x, int32_t y, int32_t width, int32_t height);
extern void NativeGLUTDestroyWindow(int32_t win);
extern void NativeGLUTPostRedisplay();
extern void NativeGLUTPostWindowRedisplay(int32_t win);
extern void NativeGLUTSwapBuffers();
extern uint32_t NativeGLUTGetWindow();
extern void NativeGLUTSetWindow(int32_t win);
extern void NativeGLUTSetWindowTitle(uint32_t title_ptr);
extern void NativeGLUTSetIconTitle(uint32_t title_ptr);
extern void NativeGLUTPositionWindow(int32_t x, int32_t y);
extern void NativeGLUTReshapeWindow(int32_t width, int32_t height);
extern void NativeGLUTPopWindow();
extern void NativeGLUTPushWindow();
extern void NativeGLUTIconifyWindow();
extern void NativeGLUTShowWindow();
extern void NativeGLUTHideWindow();
extern void NativeGLUTFullScreen();
extern void NativeGLUTSetCursor(int32_t cursor);
extern void NativeGLUTWarpPointer(int32_t x, int32_t y);
extern void NativeGLUTEstablishOverlay();
extern void NativeGLUTRemoveOverlay();
extern void NativeGLUTUseLayer(uint32_t layer);
extern void NativeGLUTPostOverlayRedisplay();
extern void NativeGLUTPostWindowOverlayRedisplay(int32_t win);
extern void NativeGLUTShowOverlay();
extern void NativeGLUTHideOverlay();
extern uint32_t NativeGLUTCreateMenu(uint32_t callback);
extern void NativeGLUTDestroyMenu(int32_t menu);
extern uint32_t NativeGLUTGetMenu();
extern void NativeGLUTSetMenu(int32_t menu);
extern void NativeGLUTAddMenuEntry(uint32_t label, int32_t value);
extern void NativeGLUTAddSubMenu(uint32_t label, int32_t submenu);
extern void NativeGLUTChangeToMenuEntry(int32_t item, uint32_t label, int32_t value);
extern void NativeGLUTChangeToSubMenu(int32_t item, uint32_t label, int32_t submenu);
extern void NativeGLUTRemoveMenuItem(int32_t item);
extern void NativeGLUTAttachMenu(int32_t button);
extern void NativeGLUTAttachMenuName(int32_t button, uint32_t name);
extern void NativeGLUTDetachMenu(int32_t button);
extern void NativeGLUTDisplayFunc(uint32_t func);
extern void NativeGLUTReshapeFunc(uint32_t func);
extern void NativeGLUTKeyboardFunc(uint32_t func);
extern void NativeGLUTMouseFunc(uint32_t func);
extern void NativeGLUTMotionFunc(uint32_t func);
extern void NativeGLUTPassiveMotionFunc(uint32_t func);
extern void NativeGLUTEntryFunc(uint32_t func);
extern void NativeGLUTVisibilityFunc(uint32_t func);
extern void NativeGLUTIdleFunc(uint32_t func);
extern void NativeGLUTTimerFunc(uint32_t millis, uint32_t func, int32_t value);
extern void NativeGLUTMenuStateFunc(uint32_t func);
extern void NativeGLUTSpecialFunc(uint32_t func);
extern void NativeGLUTSpaceballMotionFunc(uint32_t func);
extern void NativeGLUTSpaceballRotateFunc(uint32_t func);
extern void NativeGLUTSpaceballButtonFunc(uint32_t func);
extern void NativeGLUTButtonBoxFunc(uint32_t func);
extern void NativeGLUTDialsFunc(uint32_t func);
extern void NativeGLUTTabletMotionFunc(uint32_t func);
extern void NativeGLUTTabletButtonFunc(uint32_t func);
extern void NativeGLUTMenuStatusFunc(uint32_t func);
extern void NativeGLUTOverlayDisplayFunc(uint32_t func);
extern void NativeGLUTWindowStatusFunc(uint32_t func);
extern void NativeGLUTKeyboardUpFunc(uint32_t func);
extern void NativeGLUTSpecialUpFunc(uint32_t func);
extern void NativeGLUTJoystickFunc(uint32_t func, int32_t pollInterval);
extern void NativeGLUTSetColor(int32_t cell, float red, float green, float blue);
extern float NativeGLUTGetColor(int32_t ndx, int32_t component);
extern void NativeGLUTCopyColormap(int32_t win);
extern int32_t NativeGLUTGet(uint32_t type);
extern int32_t NativeGLUTDeviceGet(uint32_t type);
extern int32_t NativeGLUTExtensionSupported(uint32_t name_ptr);
extern int32_t NativeGLUTGetModifiers();
extern int32_t NativeGLUTLayerGet(uint32_t type);
extern void NativeGLUTBitmapCharacter(GLContext *ctx, uint32_t font, int32_t character);
extern int32_t NativeGLUTBitmapWidth(uint32_t font, int32_t character);
extern void NativeGLUTStrokeCharacter(GLContext *ctx, uint32_t font, int32_t character);
extern int32_t NativeGLUTStrokeWidth(uint32_t font, int32_t character);
extern int32_t NativeGLUTBitmapLength(uint32_t font, uint32_t string_ptr);
extern int32_t NativeGLUTStrokeLength(uint32_t font, uint32_t string_ptr);
extern void NativeGLUTSolidSphere(GLContext *ctx, double radius, int32_t slices, int32_t stacks);
extern void NativeGLUTWireSphere(GLContext *ctx, double radius, int32_t slices, int32_t stacks);
extern void NativeGLUTSolidCone(GLContext *ctx, double base, double height, int32_t slices, int32_t stacks);
extern void NativeGLUTWireCone(GLContext *ctx, double base, double height, int32_t slices, int32_t stacks);
extern void NativeGLUTSolidCube(GLContext *ctx, double size);
extern void NativeGLUTWireCube(GLContext *ctx, double size);
extern void NativeGLUTSolidTorus(GLContext *ctx, double innerRadius, double outerRadius, int32_t sides, int32_t rings);
extern void NativeGLUTWireTorus(GLContext *ctx, double innerRadius, double outerRadius, int32_t sides, int32_t rings);
extern void NativeGLUTSolidDodecahedron(GLContext *ctx);
extern void NativeGLUTWireDodecahedron(GLContext *ctx);
extern void NativeGLUTSolidTeapot(GLContext *ctx, double size);
extern void NativeGLUTWireTeapot(GLContext *ctx, double size);
extern void NativeGLUTSolidOctahedron(GLContext *ctx);
extern void NativeGLUTWireOctahedron(GLContext *ctx);
extern void NativeGLUTSolidTetrahedron(GLContext *ctx);
extern void NativeGLUTWireTetrahedron(GLContext *ctx);
extern void NativeGLUTSolidIcosahedron(GLContext *ctx);
extern void NativeGLUTWireIcosahedron(GLContext *ctx);
extern int32_t NativeGLUTVideoResizeGet(uint32_t param);
extern void NativeGLUTSetupVideoResizing();
extern void NativeGLUTStopVideoResizing();
extern void NativeGLUTVideoResize(int32_t x, int32_t y, int32_t w, int32_t h);
extern void NativeGLUTVideoPan(int32_t x, int32_t y, int32_t w, int32_t h);
extern void NativeGLUTReportErrors();
extern void NativeGLUTIgnoreKeyRepeat(int32_t ignore);
extern void NativeGLUTSetKeyRepeat(int32_t repeatMode);
extern void NativeGLUTForceJoystickFunc();
extern void NativeGLUTGameModeString(uint32_t string_ptr);
extern int32_t NativeGLUTEnterGameMode();
extern void NativeGLUTLeaveGameMode();
extern int32_t NativeGLUTGameModeGet(uint32_t mode);

#ifdef __APPLE__
#include <os/log.h>
os_log_t gl_log = OS_LOG_DEFAULT;
os_log_t gl_metal_log = OS_LOG_DEFAULT;

static struct GLLogInit {
	GLLogInit() {
		gl_log = os_log_create("com.pocketshaver.gl", "engine");
		gl_metal_log = os_log_create("com.pocketshaver.gl", "metal");
	}
} gl_log_init;
#endif

/*
 *  Helpers for FPR arguments captured by sheepshaver_glue.cpp.
 *
 *  Wire format: one 32-bit float payload per PPC FPR argument. The glue casts
 *  each guest float/double FPR value down to float and copies that raw word into
 *  float_bits[n]. Dispatchers for *d entry points must therefore widen that one
 *  word back to double; they must not reconstruct a 64-bit double from adjacent
 *  float_bits slots.
 */
static inline float float_arg(const uint32_t* float_bits, int index) {
	float f;
	memcpy(&f, &float_bits[index], 4);
	return f;
}

static inline double double_arg(const uint32_t* float_bits, int index) {
	return (double)float_arg(float_bits, index);
}

// ============================================================================
//  Display List Recording and Replay
// ============================================================================

/*
 *  IsNonRecordableOpcode -- returns true for commands that must NOT be recorded
 *  in display lists (per GL spec: list management, query, and flush/sync).
 */
static bool IsNonRecordableOpcode(uint32_t op) {
	switch (op) {
	// Display list management
	case GL_SUB_NEW_LIST:
	case GL_SUB_END_LIST:
	case GL_SUB_CALL_LIST:
	case GL_SUB_CALL_LISTS:
	case GL_SUB_GEN_LISTS:
	case GL_SUB_DELETE_LISTS:
	case GL_SUB_IS_LIST:
	case GL_SUB_LIST_BASE:
	// Query/state-reading commands
	case GL_SUB_GET_BOOLEANV:
	case GL_SUB_GET_DOUBLEV:
	case GL_SUB_GET_FLOATV:
	case GL_SUB_GET_INTEGERV:
	case GL_SUB_GET_ERROR:
	case GL_SUB_GET_STRING:
	case GL_SUB_IS_ENABLED:
	case GL_SUB_IS_TEXTURE:
	case GL_SUB_GET_LIGHTFV:
	case GL_SUB_GET_LIGHTIV:
	case GL_SUB_GET_MATERIALFV:
	case GL_SUB_GET_MATERIALIV:
	case GL_SUB_GET_TEX_ENVFV:
	case GL_SUB_GET_TEX_ENVIV:
	case GL_SUB_GET_TEX_GENDV:
	case GL_SUB_GET_TEX_GENFV:
	case GL_SUB_GET_TEX_GENIV:
	case GL_SUB_GET_TEX_IMAGE:
	case GL_SUB_GET_TEX_LEVEL_PARAMETERFV:
	case GL_SUB_GET_TEX_LEVEL_PARAMETERIV:
	case GL_SUB_GET_TEX_PARAMETERFV:
	case GL_SUB_GET_TEX_PARAMETERIV:
	case GL_SUB_GET_POLYGON_STIPPLE:
	case GL_SUB_GET_CLIP_PLANE:
	case GL_SUB_GET_MAPDV:
	case GL_SUB_GET_MAPFV:
	case GL_SUB_GET_MAPIV:
	case GL_SUB_GET_PIXEL_MAPFV:
	case GL_SUB_GET_PIXEL_MAPUIV:
	case GL_SUB_GET_PIXEL_MAPUSV:
	case GL_SUB_GET_POINTERV:
	case GL_SUB_RENDER_MODE:
	case GL_SUB_FEEDBACK_BUFFER:
	case GL_SUB_SELECT_BUFFER:
	case GL_SUB_ARE_TEXTURES_RESIDENT:
	case GL_SUB_READ_PIXELS:
	// Sync
	case GL_SUB_FLUSH:
	case GL_SUB_FINISH:
		return true;
	default:
		return false;
	}
}


/*
 *  IsPointerCommand -- returns true if the sub_opcode passes a PPC pointer
 *  argument that references data which must be captured at record time.
 *  Returns the pointer arg register index (0-based from r3) and byte count
 *  to capture, or 0 if not a pointer command.
 */
static int PointerDataSize(uint32_t op, uint32_t r3, uint32_t r4, uint32_t r5,
                            int &arg_index) {
	arg_index = -1;
	switch (op) {
	// Matrix loads: 16 floats (64 bytes) or 16 doubles (128 bytes) at pointer in r3
	case GL_SUB_LOAD_MATRIXF:
	case GL_SUB_LOAD_MATRIXD:
	case GL_SUB_MULT_MATRIXF:
	case GL_SUB_MULT_MATRIXD:
		arg_index = 0; return (op == GL_SUB_LOAD_MATRIXD || op == GL_SUB_MULT_MATRIXD) ? 128 : 64;
	// Vertex pointer variants: 2-4 floats
	case GL_SUB_VERTEX2FV: arg_index = 0; return 8;
	case GL_SUB_VERTEX3FV: arg_index = 0; return 12;
	case GL_SUB_VERTEX4FV: arg_index = 0; return 16;
	case GL_SUB_VERTEX2DV: arg_index = 0; return 16;
	case GL_SUB_VERTEX3DV: arg_index = 0; return 24;
	case GL_SUB_VERTEX4DV: arg_index = 0; return 32;
	case GL_SUB_VERTEX2IV: arg_index = 0; return 8;
	case GL_SUB_VERTEX3IV: arg_index = 0; return 12;
	case GL_SUB_VERTEX4IV: arg_index = 0; return 16;
	case GL_SUB_VERTEX2SV: arg_index = 0; return 4;
	case GL_SUB_VERTEX3SV: arg_index = 0; return 6;
	case GL_SUB_VERTEX4SV: arg_index = 0; return 8;
	// Normal pointer variants: 3 components
	case GL_SUB_NORMAL3FV: arg_index = 0; return 12;
	case GL_SUB_NORMAL3DV: arg_index = 0; return 24;
	case GL_SUB_NORMAL3IV: arg_index = 0; return 12;
	case GL_SUB_NORMAL3SV: arg_index = 0; return 6;
	case GL_SUB_NORMAL3BV: arg_index = 0; return 3;
	// Color pointer variants
	case GL_SUB_COLOR3FV: arg_index = 0; return 12;
	case GL_SUB_COLOR4FV: arg_index = 0; return 16;
	case GL_SUB_COLOR3DV: arg_index = 0; return 24;
	case GL_SUB_COLOR4DV: arg_index = 0; return 32;
	case GL_SUB_COLOR3BV: arg_index = 0; return 3;
	case GL_SUB_COLOR4BV: arg_index = 0; return 4;
	case GL_SUB_COLOR3UBV: arg_index = 0; return 3;
	case GL_SUB_COLOR4UBV: arg_index = 0; return 4;
	case GL_SUB_COLOR3IV: arg_index = 0; return 12;
	case GL_SUB_COLOR4IV: arg_index = 0; return 16;
	case GL_SUB_COLOR3SV: arg_index = 0; return 6;
	case GL_SUB_COLOR4SV: arg_index = 0; return 8;
	case GL_SUB_COLOR3USV: arg_index = 0; return 6;
	case GL_SUB_COLOR4USV: arg_index = 0; return 8;
	// TexCoord pointer variants
	case GL_SUB_TEX_COORD1FV: arg_index = 0; return 4;
	case GL_SUB_TEX_COORD2FV: arg_index = 0; return 8;
	case GL_SUB_TEX_COORD3FV: arg_index = 0; return 12;
	case GL_SUB_TEX_COORD4FV: arg_index = 0; return 16;
	case GL_SUB_TEX_COORD1DV: arg_index = 0; return 8;
	case GL_SUB_TEX_COORD2DV: arg_index = 0; return 16;
	case GL_SUB_TEX_COORD3DV: arg_index = 0; return 24;
	case GL_SUB_TEX_COORD4DV: arg_index = 0; return 32;
	case GL_SUB_TEX_COORD1IV: arg_index = 0; return 4;
	case GL_SUB_TEX_COORD2IV: arg_index = 0; return 8;
	case GL_SUB_TEX_COORD3IV: arg_index = 0; return 12;
	case GL_SUB_TEX_COORD4IV: arg_index = 0; return 16;
	case GL_SUB_TEX_COORD1SV: arg_index = 0; return 2;
	case GL_SUB_TEX_COORD2SV: arg_index = 0; return 4;
	case GL_SUB_TEX_COORD3SV: arg_index = 0; return 6;
	case GL_SUB_TEX_COORD4SV: arg_index = 0; return 8;
	// Light/Material float vectors: 4 floats at pointer in r5 (third arg)
	case GL_SUB_LIGHTFV: arg_index = 2; return 16;
	case GL_SUB_MATERIALFV: arg_index = 2; return 16;
	case GL_SUB_LIGHT_MODELFV: arg_index = 1; return 16;
	// Clip plane: 4 doubles at pointer in r4
	case GL_SUB_CLIP_PLANE: arg_index = 1; return 32;
	// Fog fv: up to 4 floats at pointer in r4
	case GL_SUB_FOGFV: arg_index = 1; return 16;
	// TexParameter fv: up to 4 floats in r5
	case GL_SUB_TEX_PARAMETERFV: arg_index = 2; return 16;
	// TexEnv fv: up to 4 floats in r5
	case GL_SUB_TEX_ENVFV: arg_index = 2; return 16;
	// Map1/Map2, PixelMap, PolygonStipple -- rare in games, skip for now
	default: return 0;
	}
}


/*
 *  GLRecordCommand -- pack a GL command into the current display list
 *
 *  Stores the sub_opcode, register args (r3-r10), float bits,
 *  and captured pointer data into a GLCommand struct.
 */
static void GLRecordCommand(GLContext *ctx, uint32_t sub_opcode,
                             uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                             uint32_t r7, uint32_t r8, uint32_t r9, uint32_t r10,
                             const uint32_t *float_bits, int num_float_args)
{
	GLCommand cmd;
	cmd.opcode = (uint16_t)sub_opcode;

	// Pack integer/unsigned args
	cmd.args.u[0] = r3;
	cmd.args.u[1] = r4;
	cmd.args.u[2] = r5;
	cmd.args.u[3] = r6;
	cmd.args.u[4] = r7;
	cmd.args.u[5] = r8;
	cmd.args.u[6] = r9;
	cmd.args.u[7] = r10;

	// Store float args in the data vector (after any pointer data)
	// We pack float_bits[0..num_float_args-1] as raw bytes
	int flt_bytes = num_float_args * 4;

	// Check if this command has pointer data to capture
	int ptr_arg_index = -1;
	int ptr_data_size = PointerDataSize(sub_opcode, r3, r4, r5, ptr_arg_index);

	if (ptr_data_size > 0 && ptr_arg_index >= 0) {
		// Get the Mac pointer from the appropriate register
		uint32_t mac_ptr = 0;
		switch (ptr_arg_index) {
		case 0: mac_ptr = r3; break;
		case 1: mac_ptr = r4; break;
		case 2: mac_ptr = r5; break;
		case 3: mac_ptr = r6; break;
		default: break;
		}

		if (mac_ptr) {
			// Capture pointer data into cmd.data
			cmd.data.resize(ptr_data_size + flt_bytes);
			for (int b = 0; b < ptr_data_size; b++)
				cmd.data[b] = ReadMacInt8(mac_ptr + b);
			// Append float bits after pointer data
			if (flt_bytes > 0)
				memcpy(cmd.data.data() + ptr_data_size, float_bits, flt_bytes);
		}
	} else if (flt_bytes > 0) {
		cmd.data.resize(flt_bytes);
		memcpy(cmd.data.data(), float_bits, flt_bytes);
	}

	ctx->display_lists[ctx->current_list_name].commands.push_back(std::move(cmd));
}


/*
 *  GLReplayCommand -- replay a single recorded display list command
 *
 *  For commands with captured pointer data, we write the data to a small
 *  scratch buffer in Mac memory and point the register arg at it.
 *  For commands without pointer data, we call handlers directly with
 *  stored register args and float bits.
 */
static uint32_t gl_dlist_scratch_addr = 0;  // Mac-side scratch for pointer replay

void GLReplayCommand(GLContext *ctx, const GLCommand &cmd)
{
	// Reconstruct args
	uint32_t r3 = cmd.args.u[0];
	uint32_t r4 = cmd.args.u[1];
	uint32_t r5 = cmd.args.u[2];
	uint32_t r6 = cmd.args.u[3];
	uint32_t r7 = cmd.args.u[4];
	uint32_t r8 = cmd.args.u[5];
	uint32_t r9 = cmd.args.u[6];
	uint32_t r10 = cmd.args.u[7];

	// Check if this command had pointer data captured
	int ptr_arg_index = -1;
	int ptr_data_size = PointerDataSize(cmd.opcode, r3, r4, r5, ptr_arg_index);

	const uint32_t *float_bits = nullptr;
	int num_float_args = 0;

	if (ptr_data_size > 0 && ptr_arg_index >= 0 && (int)cmd.data.size() >= ptr_data_size) {
		// Write captured data to Mac scratch buffer for the handler to read
		if (!gl_dlist_scratch_addr) {
			extern uint32 Mac_sysalloc(uint32 size);
			gl_dlist_scratch_addr = Mac_sysalloc(256);  // 256 bytes plenty for any single command
		}

		for (int b = 0; b < ptr_data_size; b++)
			WriteMacInt8(gl_dlist_scratch_addr + b, cmd.data[b]);

		// Point the register arg at our scratch buffer
		switch (ptr_arg_index) {
		case 0: r3 = gl_dlist_scratch_addr; break;
		case 1: r4 = gl_dlist_scratch_addr; break;
		case 2: r5 = gl_dlist_scratch_addr; break;
		case 3: r6 = gl_dlist_scratch_addr; break;
		}

		// Float bits follow pointer data in cmd.data
		if ((int)cmd.data.size() > ptr_data_size) {
			float_bits = (const uint32_t *)(cmd.data.data() + ptr_data_size);
			num_float_args = ((int)cmd.data.size() - ptr_data_size) / 4;
		}
	} else if (!cmd.data.empty()) {
		// No pointer data -- cmd.data contains only float bits
		float_bits = (const uint32_t *)cmd.data.data();
		num_float_args = (int)cmd.data.size() / 4;
	}

	// Temporarily clear in_display_list to prevent re-recording during replay
	bool was_in_list = ctx->in_display_list;
	ctx->in_display_list = false;

	// Write sub-opcode to scratch word so GLDispatch can read it
	WriteMacInt32(gl_scratch_addr, cmd.opcode);

	// Re-dispatch through GLDispatch
	GLDispatch(r3, r4, r5, r6, r7, r8, r9, r10,
	           float_bits ? float_bits : (const uint32_t *)"\0\0\0\0",
	           num_float_args);

	ctx->in_display_list = was_in_list;
}


/*
 *  GLDispatch - multiplexed dispatch entry point
 *
 *  Called from execute_native_op() when NATIVE_OPENGL_DISPATCH fires.
 *  Receives PPC registers r3-r10 as explicit arguments plus extracted
 *  float bits from FPR registers.
 *
 *  Returns the value to be placed in gpr(3) by the caller.
 */
uint32_t GLDispatch(uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6,
                    uint32_t r7, uint32_t r8, uint32_t r9, uint32_t r10,
                    const uint32_t *float_bits, int num_float_args)
{
	// Read sub-opcode from scratch word (same pattern as RAVE)
	uint32_t sub_opcode = ReadMacInt32(gl_scratch_addr);

	// Unmapped context-handle dispatch slot (see GLPopulateDispatchTable):
	// the game read a GL function descriptor from a dispatch-table slot we do
	// not map to a real GL function (encoded as GL_DT_DIAG_BASE + slot). Return
	// a safe no-op so the game does not call uninitialized guest heap as code
	// (root cause of the Cro-Mag Rally gameplay-launch crash), and log which
	// slot so the extension behind it can be wired to its real handler.
	if (sub_opcode >= GL_DT_DIAG_BASE) {
		uint32_t slot = sub_opcode - GL_DT_DIAG_BASE;
		static int s_unmapped_dt_log_count = 0;
		if (s_unmapped_dt_log_count < 128) {
			s_unmapped_dt_log_count++;
			GL_LOG("*** UNMAPPED dispatch-table slot %u (handle+0x%x) called -- "
			       "no GL function wired here; returning 0 no-op. Wire this slot to "
			       "its extension handler if a game feature is missing.",
			       slot, (unsigned)(4 + slot * 4));
		}
		return 0;
	}

	// Track A immediate-mode batching: this sub-opcode IS the explicit drain trigger.
	if (sub_opcode == GL_SUB_DRAIN) { GLDrainDeferred(); return 0; }
	// Drain any buffered deferred immediate-mode records before executing an observable
	// GL call, preserving program order. Guard on the ring being allocated (addr!=0) so
	// this is a true no-op until GLThunksInit reserves the ring (Task 6), and skip while
	// already draining (the replay re-enters GLDispatch).
	if (!gl_defer_draining && gl_defer_count_addr && ReadMacInt32(gl_defer_count_addr)) {
		GLDrainDeferred();
	}

	// Task 9 fallback counter: a deferrable opcode reaching GLDispatch while NOT
	// draining means the PPC stub fell back (ring full or deferral disabled).
	// Don't count calls re-entered by the drain replay (gl_defer_draining==true).
	if (!gl_defer_draining && GLDeferIsDeferrable(sub_opcode)) {
		g_defer_fallbacks++;
	}

	GL_VLOG("GLDispatch sub_opcode=%u r3=0x%x r4=0x%x r5=0x%x r6=0x%x r7=0x%x r8=0x%x r9=0x%x r10=0x%x ctx=%p",
	        sub_opcode, r3, r4, r5, r6, r7, r8, r9, r10, gl_current_context);

	// ---- Display list recording check ----
	// When compiling a display list, most commands are recorded rather than executed.
	// Non-recordable commands (list management, queries, sync) always execute.
	if (gl_current_context && gl_current_context->in_display_list &&
	    !IsNonRecordableOpcode(sub_opcode)) {
		GLRecordCommand(gl_current_context, sub_opcode,
		                r3, r4, r5, r6, r7, r8, r9, r10,
		                float_bits, num_float_args);

		if (gl_current_context->current_list_mode == 0x1301) {
			// GL_COMPILE_AND_EXECUTE: fall through to execute
		} else {
			// GL_COMPILE: do not execute, just record
			return 0;
		}
	}

	// Guard: core GL calls (sub_opcode < 600) require an active context.
	// Games like THPS 2 may call glGetError before aglSetCurrentContext.
	if (sub_opcode < GL_SUB_AGL_CHOOSEPIXELFORMAT && !gl_current_context) {
		GL_LOG("GLDispatch sub_opcode=%u ignored (no current context)", sub_opcode);
		// Return GL_INVALID_OPERATION for glGetError, 0 for everything else
		return (sub_opcode == GL_SUB_GET_ERROR) ? 0x0502 /* GL_INVALID_OPERATION */ : 0;
	}

	switch (sub_opcode) {

		// --- Core GL: Accumulation, Alpha, Textures ---
		case GL_SUB_ACCUM: NativeGLAccum(gl_current_context, r3, float_arg(float_bits, 0)); return 0;
		case GL_SUB_ALPHA_FUNC:
			NativeGLAlphaFunc(gl_current_context, r3, float_arg(float_bits, 0));
			return 0;
		case GL_SUB_ARE_TEXTURES_RESIDENT: NativeGLAreTexturesResident(gl_current_context, (int32_t)r3, r4, r5); return 0;
		case GL_SUB_ARRAY_ELEMENT: NativeGLArrayElement(gl_current_context, (int32_t)r3); return 0;
		case GL_SUB_BEGIN:
			NativeGLBegin(gl_current_context, r3);
			return 0;
		case GL_SUB_BIND_TEXTURE:
			NativeGLBindTexture(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_BITMAP: /* w,h w0-1; 4 floats w2-5; bitmap ptr w6=r9 */ NativeGLBitmap(gl_current_context, (int32_t)r3, (int32_t)r4, float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2), float_arg(float_bits, 3), r9); return 0;
		case GL_SUB_BLEND_FUNC:
			NativeGLBlendFunc(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_CALL_LIST: NativeGLCallList(gl_current_context, r3); return 0;
		case GL_SUB_CALL_LISTS: NativeGLCallLists(gl_current_context, (int32_t)r3, r4, r5); return 0;
		// --- Core GL: Clear, Clip, Color ---
		case GL_SUB_CLEAR:
			NativeGLClear(gl_current_context, r3);
			return 0;
		case GL_SUB_CLEAR_ACCUM: NativeGLClearAccum(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2), float_arg(float_bits, 3)); return 0;
		case GL_SUB_CLEAR_COLOR:
			NativeGLClearColor(gl_current_context,
				float_arg(float_bits, 0), float_arg(float_bits, 1),
				float_arg(float_bits, 2), float_arg(float_bits, 3));
			return 0;
		case GL_SUB_CLEAR_DEPTH: {
			// FPR extraction in sheepshaver_glue.cpp casts all FPR values to
			// float (truncating doubles).  glClearDepth takes a double, but
			// float_bits[0] contains the float-truncated value.  Read it as
			// a float instead of trying to reconstruct a double.
			NativeGLClearDepth(gl_current_context, (double)float_arg(float_bits, 0));
			return 0;
		}
		case GL_SUB_CLEAR_INDEX: NativeGLClearIndex(gl_current_context, float_arg(float_bits, 0)); return 0;
		case GL_SUB_CLEAR_STENCIL:
			NativeGLClearStencil(gl_current_context, (int32_t)r3);
			return 0;
		case GL_SUB_CLIP_PLANE: NativeGLClipPlane(gl_current_context, r3, r4); return 0;
		case GL_SUB_COLOR3B:
			NativeGLColor3b(gl_current_context, (int8_t)r3, (int8_t)r4, (int8_t)r5);
			return 0;
		case GL_SUB_COLOR3BV:
			NativeGLColor3bv(gl_current_context, r3);
			return 0;
		case GL_SUB_COLOR3D: {
			NativeGLColor3d(gl_current_context,
			                double_arg(float_bits, 0),
			                double_arg(float_bits, 1),
			                double_arg(float_bits, 2));
			return 0;
		}
		case GL_SUB_COLOR3DV:
			NativeGLColor3dv(gl_current_context, r3);
			return 0;
		case GL_SUB_COLOR3F:
			NativeGLColor3f(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2));
			return 0;
		case GL_SUB_COLOR3FV:
			NativeGLColor3fv(gl_current_context, r3);
			return 0;
		case GL_SUB_COLOR3I:
			NativeGLColor3i(gl_current_context, (int32_t)r3, (int32_t)r4, (int32_t)r5);
			return 0;
		case GL_SUB_COLOR3IV:
			NativeGLColor3iv(gl_current_context, r3);
			return 0;
		case GL_SUB_COLOR3S:
			NativeGLColor3s(gl_current_context, (int16_t)r3, (int16_t)r4, (int16_t)r5);
			return 0;
		case GL_SUB_COLOR3SV:
			NativeGLColor3sv(gl_current_context, r3);
			return 0;
		case GL_SUB_COLOR3UB:
			NativeGLColor3ub(gl_current_context, (uint8_t)r3, (uint8_t)r4, (uint8_t)r5);
			return 0;
		case GL_SUB_COLOR3UBV:
			NativeGLColor3ubv(gl_current_context, r3);
			return 0;
		case GL_SUB_COLOR3UI:
			NativeGLColor3ui(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_COLOR3UIV:
			NativeGLColor3uiv(gl_current_context, r3);
			return 0;
		case GL_SUB_COLOR3US:
			NativeGLColor3us(gl_current_context, (uint16_t)r3, (uint16_t)r4, (uint16_t)r5);
			return 0;
		case GL_SUB_COLOR3USV:
			NativeGLColor3usv(gl_current_context, r3);
			return 0;
		case GL_SUB_COLOR4B:
			NativeGLColor4b(gl_current_context, (int8_t)r3, (int8_t)r4, (int8_t)r5, (int8_t)r6);
			return 0;
		case GL_SUB_COLOR4BV:
			NativeGLColor4bv(gl_current_context, r3);
			return 0;
		case GL_SUB_COLOR4D: {
			NativeGLColor4d(gl_current_context,
			                double_arg(float_bits, 0),
			                double_arg(float_bits, 1),
			                double_arg(float_bits, 2),
			                double_arg(float_bits, 3));
			return 0;
		}
		case GL_SUB_COLOR4DV:
			NativeGLColor4dv(gl_current_context, r3);
			return 0;
		case GL_SUB_COLOR4F:
			NativeGLColor4f(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1),
			                float_arg(float_bits, 2), float_arg(float_bits, 3));
			return 0;
		case GL_SUB_COLOR4FV:
			NativeGLColor4fv(gl_current_context, r3);
			return 0;
		case GL_SUB_COLOR4I:
			NativeGLColor4i(gl_current_context, (int32_t)r3, (int32_t)r4, (int32_t)r5, (int32_t)r6);
			return 0;
		case GL_SUB_COLOR4IV:
			NativeGLColor4iv(gl_current_context, r3);
			return 0;
		case GL_SUB_COLOR4S:
			NativeGLColor4s(gl_current_context, (int16_t)r3, (int16_t)r4, (int16_t)r5, (int16_t)r6);
			return 0;
		case GL_SUB_COLOR4SV:
			NativeGLColor4sv(gl_current_context, r3);
			return 0;
		case GL_SUB_COLOR4UB:
			NativeGLColor4ub(gl_current_context, (uint8_t)r3, (uint8_t)r4, (uint8_t)r5, (uint8_t)r6);
			return 0;
		case GL_SUB_COLOR4UBV:
			NativeGLColor4ubv(gl_current_context, r3);
			return 0;
		case GL_SUB_COLOR4UI:
			NativeGLColor4ui(gl_current_context, r3, r4, r5, r6);
			return 0;
		case GL_SUB_COLOR4UIV:
			NativeGLColor4uiv(gl_current_context, r3);
			return 0;
		case GL_SUB_COLOR4US:
			NativeGLColor4us(gl_current_context, (uint16_t)r3, (uint16_t)r4, (uint16_t)r5, (uint16_t)r6);
			return 0;
		case GL_SUB_COLOR4USV:
			NativeGLColor4usv(gl_current_context, r3);
			return 0;
		case GL_SUB_COLOR_MASK:
			NativeGLColorMask(gl_current_context, r3, r4, r5, r6);
			return 0;
		case GL_SUB_COLOR_MATERIAL:
			NativeGLColorMaterial(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_COLOR_POINTER: NativeGLColorPointer(gl_current_context, (int32_t)r3, r4, (int32_t)r5, r6); return 0;
		// --- Core GL: Copy, Cull, Delete, Depth, Disable ---
		case GL_SUB_COPY_PIXELS: NativeGLCopyPixels(gl_current_context, (int32_t)r3, (int32_t)r4, (int32_t)r5, (int32_t)r6, r7); return 0;
		case GL_SUB_COPY_TEX_IMAGE1D: NativeGLCopyTexImage1D(gl_current_context, r3, (int32_t)r4, r5, (int32_t)r6, (int32_t)r7, (int32_t)r8, (int32_t)r9); return 0;
		case GL_SUB_COPY_TEX_IMAGE2D:
			NativeGLCopyTexImage2D(gl_current_context, r3, (int32_t)r4, r5,
			                        (int32_t)r6, (int32_t)r7, (int32_t)r8, (int32_t)r9, (int32_t)r10);
			return 0;
		case GL_SUB_COPY_TEX_SUB_IMAGE1D: NativeGLCopyTexSubImage1D(gl_current_context, r3, (int32_t)r4, (int32_t)r5, (int32_t)r6, (int32_t)r7, (int32_t)r8); return 0;
		case GL_SUB_COPY_TEX_SUB_IMAGE2D:
			NativeGLCopyTexSubImage2D(gl_current_context, r3, (int32_t)r4,
			                           (int32_t)r5, (int32_t)r6, (int32_t)r7, (int32_t)r8,
			                           (int32_t)r9, (int32_t)r10);
			return 0;
		case GL_SUB_CULL_FACE:
			NativeGLCullFace(gl_current_context, r3);
			return 0;
		case GL_SUB_DELETE_LISTS: NativeGLDeleteLists(gl_current_context, r3, (int32_t)r4); return 0;
		case GL_SUB_DELETE_TEXTURES:
			NativeGLDeleteTextures(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_DEPTH_FUNC:
			NativeGLDepthFunc(gl_current_context, r3);
			return 0;
		case GL_SUB_DEPTH_MASK:
			NativeGLDepthMask(gl_current_context, r3);
			return 0;
		case GL_SUB_DEPTH_RANGE: {
			// FPR extraction truncates doubles to float -- read as floats
			NativeGLDepthRange(gl_current_context,
			                   (double)float_arg(float_bits, 0),
			                   (double)float_arg(float_bits, 1));
			return 0;
		}
		case GL_SUB_DISABLE:
			NativeGLDisable(gl_current_context, r3);
			return 0;
		case GL_SUB_DISABLE_CLIENT_STATE: NativeGLDisableClientState(gl_current_context, r3); return 0;
		// --- Core GL: Draw, Edge, Enable, End, Eval ---
		case GL_SUB_DRAW_ARRAYS: NativeGLDrawArrays(gl_current_context, r3, (int32_t)r4, (int32_t)r5); return 0;
		case GL_SUB_DRAW_BUFFER: NativeGLDrawBuffer(gl_current_context, r3); return 0;
		case GL_SUB_DRAW_ELEMENTS: NativeGLDrawElements(gl_current_context, r3, (int32_t)r4, r5, r6); return 0;
		case GL_SUB_DRAW_PIXELS: NativeGLDrawPixels(gl_current_context, (int32_t)r3, (int32_t)r4, r5, r6, r7); return 0;
		case GL_SUB_EDGE_FLAG: NativeGLEdgeFlag(gl_current_context, r3); return 0;
		case GL_SUB_EDGE_FLAG_POINTER: NativeGLEdgeFlagPointer(gl_current_context, (int32_t)r3, r4); return 0;
		case GL_SUB_EDGE_FLAGV: NativeGLEdgeFlagv(gl_current_context, r3); return 0;
		case GL_SUB_ENABLE:
			NativeGLEnable(gl_current_context, r3);
			return 0;
		case GL_SUB_ENABLE_CLIENT_STATE: NativeGLEnableClientState(gl_current_context, r3); return 0;
		case GL_SUB_END:
			NativeGLEnd(gl_current_context);
			return 0;
		case GL_SUB_END_LIST:
			NativeGLEndList(gl_current_context);
			// Re-enable deferral now that list compilation is done -- UNLESS the
			// failsafe has latched off (ring corruption). Respecting the latch here
			// stops glEndList from un-degrading a tripped failsafe every batch.
			// Guard on addr!=0: no write to guest address 0 before ring is allocated (Task 6).
			if (gl_defer_enabled_addr && !gl_defer_failsafe_tripped) WriteMacInt32(gl_defer_enabled_addr, 1);
			return 0;
		case GL_SUB_EVAL_COORD1D:
			NativeGLEvalCoord1d(gl_current_context, double_arg(float_bits, 0));
			return 0;
		case GL_SUB_EVAL_COORD1DV: NativeGLEvalCoord1dv(gl_current_context, r3); return 0;
		case GL_SUB_EVAL_COORD1F: NativeGLEvalCoord1f(gl_current_context, float_arg(float_bits, 0)); return 0;
		case GL_SUB_EVAL_COORD1FV: NativeGLEvalCoord1fv(gl_current_context, r3); return 0;
		case GL_SUB_EVAL_COORD2D:
			NativeGLEvalCoord2d(gl_current_context,
			                    double_arg(float_bits, 0),
			                    double_arg(float_bits, 1));
			return 0;
		case GL_SUB_EVAL_COORD2DV: NativeGLEvalCoord2dv(gl_current_context, r3); return 0;
		case GL_SUB_EVAL_COORD2F: NativeGLEvalCoord2f(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1)); return 0;
		case GL_SUB_EVAL_COORD2FV: NativeGLEvalCoord2fv(gl_current_context, r3); return 0;
		case GL_SUB_EVAL_MESH1: NativeGLEvalMesh1(gl_current_context, r3, (int32_t)r4, (int32_t)r5); return 0;
		case GL_SUB_EVAL_MESH2: NativeGLEvalMesh2(gl_current_context, r3, (int32_t)r4, (int32_t)r5, (int32_t)r6, (int32_t)r7); return 0;
		case GL_SUB_EVAL_POINT1: NativeGLEvalPoint1(gl_current_context, (int32_t)r3); return 0;
		case GL_SUB_EVAL_POINT2: NativeGLEvalPoint2(gl_current_context, (int32_t)r3, (int32_t)r4); return 0;
		// --- Core GL: Feedback, Finish, Flush, Fog, Front, Frustum ---
		case GL_SUB_FEEDBACK_BUFFER: NativeGLFeedbackBuffer(gl_current_context, (int32_t)r3, r4, r5); return 0;
		case GL_SUB_FINISH: NativeGLFinish(gl_current_context); return 0;
		case GL_SUB_FLUSH: NativeGLFlush(gl_current_context); return 0;
		case GL_SUB_FOGF:
			NativeGLFogf(gl_current_context, r3, float_arg(float_bits, 0));
			return 0;
		case GL_SUB_FOGFV:
			NativeGLFogfv(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_FOGI:
			NativeGLFogi(gl_current_context, r3, (int32_t)r4);
			return 0;
		case GL_SUB_FOGIV:
			NativeGLFogiv(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_FRONT_FACE:
			NativeGLFrontFace(gl_current_context, r3);
			return 0;
		case GL_SUB_FRUSTUM:
			// FPR extraction already converts doubles to floats in float_bits[]
			NativeGLFrustum(gl_current_context,
				(double)float_arg(float_bits, 0), (double)float_arg(float_bits, 1),
				(double)float_arg(float_bits, 2), (double)float_arg(float_bits, 3),
				(double)float_arg(float_bits, 4), (double)float_arg(float_bits, 5));
			return 0;
		// --- Core GL: Gen, Get ---
		case GL_SUB_GEN_LISTS: return NativeGLGenLists(gl_current_context, (int32_t)r3);
		case GL_SUB_GEN_TEXTURES:
			NativeGLGenTextures(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_GET_BOOLEANV:
			NativeGLGetBooleanv(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_GET_CLIP_PLANE: NativeGLGetClipPlane(gl_current_context, r3, r4); return 0;
		case GL_SUB_GET_DOUBLEV: NativeGLGetDoublev(gl_current_context, r3, r4); return 0;
		case GL_SUB_GET_ERROR:
			return NativeGLGetError(gl_current_context);
		case GL_SUB_GET_FLOATV:
			NativeGLGetFloatv(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_GET_INTEGERV:
			NativeGLGetIntegerv(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_GET_LIGHTFV: NativeGLGetLightfv(gl_current_context, r3, r4, r5); return 0;
		case GL_SUB_GET_LIGHTIV: NativeGLGetLightiv(gl_current_context, r3, r4, r5); return 0;
		case GL_SUB_GET_MAPDV: NativeGLGetMapdv(gl_current_context, r3, r4, r5); return 0;
		case GL_SUB_GET_MAPFV: NativeGLGetMapfv(gl_current_context, r3, r4, r5); return 0;
		case GL_SUB_GET_MAPIV: NativeGLGetMapiv(gl_current_context, r3, r4, r5); return 0;
		case GL_SUB_GET_MATERIALFV: NativeGLGetMaterialfv(gl_current_context, r3, r4, r5); return 0;
		case GL_SUB_GET_MATERIALIV: NativeGLGetMaterialiv(gl_current_context, r3, r4, r5); return 0;
		case GL_SUB_GET_PIXEL_MAPFV: NativeGLGetPixelMapfv(gl_current_context, r3, r4); return 0;
		case GL_SUB_GET_PIXEL_MAPUIV: NativeGLGetPixelMapuiv(gl_current_context, r3, r4); return 0;
		case GL_SUB_GET_PIXEL_MAPUSV: NativeGLGetPixelMapusv(gl_current_context, r3, r4); return 0;
		case GL_SUB_GET_POINTERV: NativeGLGetPointerv(gl_current_context, r3, r4); return 0;
		case GL_SUB_GET_POLYGON_STIPPLE: NativeGLGetPolygonStipple(gl_current_context, r3); return 0;
		case GL_SUB_GET_STRING:
			return NativeGLGetString(gl_current_context, r3);
		case GL_SUB_GET_TEX_ENVFV: NativeGLGetTexEnvfv(gl_current_context, r3, r4, r5); return 0;
		case GL_SUB_GET_TEX_ENVIV: NativeGLGetTexEnviv(gl_current_context, r3, r4, r5); return 0;
		case GL_SUB_GET_TEX_GENDV: NativeGLGetTexGendv(gl_current_context, r3, r4, r5); return 0;
		case GL_SUB_GET_TEX_GENFV: NativeGLGetTexGenfv(gl_current_context, r3, r4, r5); return 0;
		case GL_SUB_GET_TEX_GENIV: NativeGLGetTexGeniv(gl_current_context, r3, r4, r5); return 0;
		case GL_SUB_GET_TEX_IMAGE: NativeGLGetTexImage(gl_current_context, r3, (int32_t)r4, r5, r6, r7); return 0;
		case GL_SUB_GET_TEX_LEVEL_PARAMETERFV: NativeGLGetTexLevelParameterfv(gl_current_context, r3, (int32_t)r4, r5, r6); return 0;
		case GL_SUB_GET_TEX_LEVEL_PARAMETERIV: NativeGLGetTexLevelParameteriv(gl_current_context, r3, (int32_t)r4, r5, r6); return 0;
		case GL_SUB_GET_TEX_PARAMETERFV: NativeGLGetTexParameterfv(gl_current_context, r3, r4, r5); return 0;
		case GL_SUB_GET_TEX_PARAMETERIV: NativeGLGetTexParameteriv(gl_current_context, r3, r4, r5); return 0;
		// --- Core GL: Hint, Index ---
		case GL_SUB_HINT:
			NativeGLHint(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_INDEX_MASK: NativeGLIndexMask(gl_current_context, r3); return 0;
		case GL_SUB_INDEX_POINTER: NativeGLIndexPointer(gl_current_context, r3, (int32_t)r4, r5); return 0;
		case GL_SUB_INDEXD:
			NativeGLIndexd(gl_current_context, double_arg(float_bits, 0));
			return 0;
		case GL_SUB_INDEXDV: NativeGLIndexdv(gl_current_context, r3); return 0;
		case GL_SUB_INDEXF: NativeGLIndexf(gl_current_context, float_arg(float_bits, 0)); return 0;
		case GL_SUB_INDEXFV: NativeGLIndexfv(gl_current_context, r3); return 0;
		case GL_SUB_INDEXI: NativeGLIndexi(gl_current_context, (int32_t)r3); return 0;
		case GL_SUB_INDEXIV: NativeGLIndexiv(gl_current_context, r3); return 0;
		case GL_SUB_INDEXS: NativeGLIndexs(gl_current_context, (int16_t)r3); return 0;
		case GL_SUB_INDEXSV: NativeGLIndexsv(gl_current_context, r3); return 0;
		case GL_SUB_INDEXUB: NativeGLIndexub(gl_current_context, (uint8_t)r3); return 0;
		case GL_SUB_INDEXUBV: NativeGLIndexubv(gl_current_context, r3); return 0;
		// --- Core GL: Init, Interleaved, Is ---
		case GL_SUB_INIT_NAMES: NativeGLInitNames(gl_current_context); return 0;
		case GL_SUB_INTERLEAVED_ARRAYS: NativeGLInterleavedArrays(gl_current_context, r3, (int32_t)r4, r5); return 0;
		case GL_SUB_IS_ENABLED:
			return NativeGLIsEnabled(gl_current_context, r3);
		case GL_SUB_IS_LIST: return NativeGLIsList(gl_current_context, r3);
		case GL_SUB_IS_TEXTURE:
			return NativeGLIsTexture(gl_current_context, r3);
		// --- Core GL: Light, Line, List, Load, Logic ---
		case GL_SUB_LIGHT_MODELF:
			NativeGLLightModelf(gl_current_context, r3, float_arg(float_bits, 0));
			return 0;
		case GL_SUB_LIGHT_MODELFV:
			NativeGLLightModelfv(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_LIGHT_MODELI:
			NativeGLLightModeli(gl_current_context, r3, (int32_t)r4);
			return 0;
		case GL_SUB_LIGHT_MODELIV:
			NativeGLLightModeliv(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_LIGHTF:
			NativeGLLightf(gl_current_context, r3, r4, float_arg(float_bits, 0));
			return 0;
		case GL_SUB_LIGHTFV:
			NativeGLLightfv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_LIGHTI:
			NativeGLLighti(gl_current_context, r3, r4, (int32_t)r5);
			return 0;
		case GL_SUB_LIGHTIV:
			NativeGLLightiv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_LINE_STIPPLE: NativeGLLineStipple(gl_current_context, (int32_t)r3, r4); return 0;
		case GL_SUB_LINE_WIDTH:
			NativeGLLineWidth(gl_current_context, float_arg(float_bits, 0));
			return 0;
		case GL_SUB_LIST_BASE: NativeGLListBase(gl_current_context, r3); return 0;
		case GL_SUB_LOAD_IDENTITY:
			NativeGLLoadIdentity(gl_current_context);
			return 0;
		case GL_SUB_LOAD_MATRIXD:
			NativeGLLoadMatrixd(gl_current_context, r3);
			return 0;
		case GL_SUB_LOAD_MATRIXF:
			NativeGLLoadMatrixf(gl_current_context, r3);
			return 0;
		case GL_SUB_LOAD_NAME: NativeGLLoadName(gl_current_context, r3); return 0;
		case GL_SUB_LOGIC_OP:
			NativeGLLogicOp(gl_current_context, r3);
			return 0;
		// --- Core GL: Map, Material, Matrix, Mult ---
		case GL_SUB_MAP1D: /* target w0; u1/u2 doubles w1-4; stride w5=r8, order w6=r9, points w7=r10 */ NativeGLMap1d(gl_current_context, r3, float_arg(float_bits, 0), float_arg(float_bits, 1), (int32_t)r8, (int32_t)r9, r10); return 0;
		case GL_SUB_MAP1F: /* target w0; u1/u2 floats w1-2; stride w3=r6, order w4=r7, points w5=r8 */ NativeGLMap1f(gl_current_context, r3, float_arg(float_bits, 0), float_arg(float_bits, 1), (int32_t)r6, (int32_t)r7, r8); return 0;
		case GL_SUB_MAP2D: /* target w0; u1/u2 w1-4; ustride w5=r8, uorder w6=r9; v1 w7-8, v2 w9-10; vstride w11, vorder w12, points w13 */ NativeGLMap2d(gl_current_context, r3, float_arg(float_bits, 0), float_arg(float_bits, 1), (int32_t)r8, (int32_t)r9, float_arg(float_bits, 2), float_arg(float_bits, 3), (int32_t)gl_ppc_stack_arg(3), (int32_t)gl_ppc_stack_arg(4), gl_ppc_stack_arg(5)); return 0;
		case GL_SUB_MAP2F: /* target w0; u1/u2 floats w1-2; ustride w3=r6, uorder w4=r7; v1/v2 w5-6; vstride w7=r10, vorder w8, points w9 */ NativeGLMap2f(gl_current_context, r3, float_arg(float_bits, 0), float_arg(float_bits, 1), (int32_t)r6, (int32_t)r7, float_arg(float_bits, 2), float_arg(float_bits, 3), (int32_t)r10, (int32_t)gl_ppc_stack_arg(0), gl_ppc_stack_arg(1)); return 0;
		case GL_SUB_MAP_GRID1D: NativeGLMapGrid1d(gl_current_context, (int32_t)r3, float_arg(float_bits, 0), float_arg(float_bits, 1)); return 0;
		case GL_SUB_MAP_GRID1F: NativeGLMapGrid1f(gl_current_context, (int32_t)r3, float_arg(float_bits, 0), float_arg(float_bits, 1)); return 0;
		case GL_SUB_MAP_GRID2D: /* un w0; u1/u2 doubles w1-4; vn w5=r8 */ NativeGLMapGrid2d(gl_current_context, (int32_t)r3, float_arg(float_bits, 0), float_arg(float_bits, 1), (int32_t)r8, float_arg(float_bits, 2), float_arg(float_bits, 3)); return 0;
		case GL_SUB_MAP_GRID2F: /* un w0; u1/u2 floats w1-2; vn w3=r6 */ NativeGLMapGrid2f(gl_current_context, (int32_t)r3, float_arg(float_bits, 0), float_arg(float_bits, 1), (int32_t)r6, float_arg(float_bits, 2), float_arg(float_bits, 3)); return 0;
		case GL_SUB_MATERIALF:
			NativeGLMaterialf(gl_current_context, r3, r4, float_arg(float_bits, 0));
			return 0;
		case GL_SUB_MATERIALFV:
			NativeGLMaterialfv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_MATERIALI:
			NativeGLMateriali(gl_current_context, r3, r4, (int32_t)r5);
			return 0;
		case GL_SUB_MATERIALIV:
			NativeGLMaterialiv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_MATRIX_MODE:
			NativeGLMatrixMode(gl_current_context, r3);
			return 0;
		case GL_SUB_MULT_MATRIXD:
			NativeGLMultMatrixd(gl_current_context, r3);
			return 0;
		case GL_SUB_MULT_MATRIXF:
			NativeGLMultMatrixf(gl_current_context, r3);
			return 0;
		// --- Core GL: New, Normal ---
		case GL_SUB_NEW_LIST:
			NativeGLNewList(gl_current_context, r3, r4);
			// Disable deferral while compiling a display list so immediate-mode calls
			// go through the recording path (existing in_display_list check above),
			// not into the deferred ring. Any pending records were already drained by
			// the drain-first hook earlier in this dispatch, preserving program order.
			// Guard on addr!=0: no write to guest address 0 before ring is allocated (Task 6).
			if (gl_defer_enabled_addr) WriteMacInt32(gl_defer_enabled_addr, 0);
			return 0;
		case GL_SUB_NORMAL3B:
			NativeGLNormal3b(gl_current_context, (int8_t)r3, (int8_t)r4, (int8_t)r5);
			return 0;
		case GL_SUB_NORMAL3BV:
			NativeGLNormal3bv(gl_current_context, r3);
			return 0;
		case GL_SUB_NORMAL3D: {
			NativeGLNormal3d(gl_current_context,
			                 double_arg(float_bits, 0),
			                 double_arg(float_bits, 1),
			                 double_arg(float_bits, 2));
			return 0;
		}
		case GL_SUB_NORMAL3DV:
			NativeGLNormal3dv(gl_current_context, r3);
			return 0;
		case GL_SUB_NORMAL3F:
			NativeGLNormal3f(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2));
			return 0;
		case GL_SUB_NORMAL3FV:
			NativeGLNormal3fv(gl_current_context, r3);
			return 0;
		case GL_SUB_NORMAL3I:
			NativeGLNormal3i(gl_current_context, (int32_t)r3, (int32_t)r4, (int32_t)r5);
			return 0;
		case GL_SUB_NORMAL3IV:
			NativeGLNormal3iv(gl_current_context, r3);
			return 0;
		case GL_SUB_NORMAL3S:
			NativeGLNormal3s(gl_current_context, (int16_t)r3, (int16_t)r4, (int16_t)r5);
			return 0;
		case GL_SUB_NORMAL3SV:
			NativeGLNormal3sv(gl_current_context, r3);
			return 0;
		case GL_SUB_NORMAL_POINTER: NativeGLNormalPointer(gl_current_context, r3, (int32_t)r4, r5); return 0;
		// --- Core GL: Ortho, Pass, Pixel ---
		case GL_SUB_ORTHO:
			// FPR extraction already converts doubles to floats in float_bits[]
			NativeGLOrtho(gl_current_context,
				(double)float_arg(float_bits, 0), (double)float_arg(float_bits, 1),
				(double)float_arg(float_bits, 2), (double)float_arg(float_bits, 3),
				(double)float_arg(float_bits, 4), (double)float_arg(float_bits, 5));
			return 0;
		case GL_SUB_PASS_THROUGH: NativeGLPassThrough(gl_current_context, float_arg(float_bits, 0)); return 0;
		case GL_SUB_PIXEL_MAPFV: NativeGLPixelMapfv(gl_current_context, r3, (int32_t)r4, r5); return 0;
		case GL_SUB_PIXEL_MAPUIV: NativeGLPixelMapuiv(gl_current_context, r3, (int32_t)r4, r5); return 0;
		case GL_SUB_PIXEL_MAPUSV: NativeGLPixelMapusv(gl_current_context, r3, (int32_t)r4, r5); return 0;
		case GL_SUB_PIXEL_STOREF:
			NativeGLPixelStoref(gl_current_context, r3, float_arg(float_bits, 0));
			return 0;
		case GL_SUB_PIXEL_STOREI:
			NativeGLPixelStorei(gl_current_context, r3, (int32_t)r4);
			return 0;
		case GL_SUB_PIXEL_TRANSFERF: NativeGLPixelTransferf(gl_current_context, r3, float_arg(float_bits, 0)); return 0;
		case GL_SUB_PIXEL_TRANSFERI: NativeGLPixelTransferi(gl_current_context, r3, (int32_t)r4); return 0;
		case GL_SUB_PIXEL_ZOOM: NativeGLPixelZoom(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1)); return 0;
		// --- Core GL: Point, Polygon, Pop, Prioritize, Push ---
		case GL_SUB_POINT_SIZE:
			NativeGLPointSize(gl_current_context, float_arg(float_bits, 0));
			return 0;
		case GL_SUB_POLYGON_MODE:
			NativeGLPolygonMode(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_POLYGON_OFFSET:
			NativeGLPolygonOffset(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1));
			return 0;
		case GL_SUB_POLYGON_STIPPLE: NativeGLPolygonStipple(gl_current_context, r3); return 0;
		case GL_SUB_POP_ATTRIB:
			NativeGLPopAttrib(gl_current_context);
			return 0;
		case GL_SUB_POP_CLIENT_ATTRIB:
			NativeGLPopClientAttrib(gl_current_context);
			return 0;
		case GL_SUB_POP_MATRIX:
			NativeGLPopMatrix(gl_current_context);
			return 0;
		case GL_SUB_POP_NAME: NativeGLPopName(gl_current_context); return 0;
		case GL_SUB_PRIORITIZE_TEXTURES: NativeGLPrioritizeTextures(gl_current_context, (int32_t)r3, r4, r5); return 0;
		case GL_SUB_PUSH_ATTRIB:
			NativeGLPushAttrib(gl_current_context, r3);
			return 0;
		case GL_SUB_PUSH_CLIENT_ATTRIB:
			NativeGLPushClientAttrib(gl_current_context, r3);
			return 0;
		case GL_SUB_PUSH_MATRIX:
			NativeGLPushMatrix(gl_current_context);
			return 0;
		case GL_SUB_PUSH_NAME: NativeGLPushName(gl_current_context, r3); return 0;
		// --- Core GL: Raster, Read, Rect, Render, Rotate, Scale ---
		case GL_SUB_RASTER_POS2D:
			NativeGLRasterPos2d(gl_current_context,
			                    double_arg(float_bits, 0),
			                    double_arg(float_bits, 1));
			return 0;
		case GL_SUB_RASTER_POS2DV: NativeGLRasterPos2dv(gl_current_context, r3); return 0;
		case GL_SUB_RASTER_POS2F: NativeGLRasterPos2f(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1)); return 0;
		case GL_SUB_RASTER_POS2FV: NativeGLRasterPos2fv(gl_current_context, r3); return 0;
		case GL_SUB_RASTER_POS2I: NativeGLRasterPos2i(gl_current_context, (int32_t)r3, (int32_t)r4); return 0;
		case GL_SUB_RASTER_POS2IV: NativeGLRasterPos2iv(gl_current_context, r3); return 0;
		case GL_SUB_RASTER_POS2S: NativeGLRasterPos2s(gl_current_context, (int16_t)r3, (int16_t)r4); return 0;
		case GL_SUB_RASTER_POS2SV: NativeGLRasterPos2sv(gl_current_context, r3); return 0;
		case GL_SUB_RASTER_POS3D:
			NativeGLRasterPos3d(gl_current_context,
			                    double_arg(float_bits, 0),
			                    double_arg(float_bits, 1),
			                    double_arg(float_bits, 2));
			return 0;
		case GL_SUB_RASTER_POS3DV: NativeGLRasterPos3dv(gl_current_context, r3); return 0;
		case GL_SUB_RASTER_POS3F: NativeGLRasterPos3f(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2)); return 0;
		case GL_SUB_RASTER_POS3FV: NativeGLRasterPos3fv(gl_current_context, r3); return 0;
		case GL_SUB_RASTER_POS3I: NativeGLRasterPos3i(gl_current_context, (int32_t)r3, (int32_t)r4, (int32_t)r5); return 0;
		case GL_SUB_RASTER_POS3IV: NativeGLRasterPos3iv(gl_current_context, r3); return 0;
		case GL_SUB_RASTER_POS3S: NativeGLRasterPos3s(gl_current_context, (int16_t)r3, (int16_t)r4, (int16_t)r5); return 0;
		case GL_SUB_RASTER_POS3SV: NativeGLRasterPos3sv(gl_current_context, r3); return 0;
		case GL_SUB_RASTER_POS4D:
			NativeGLRasterPos4d(gl_current_context,
			                    double_arg(float_bits, 0),
			                    double_arg(float_bits, 1),
			                    double_arg(float_bits, 2),
			                    double_arg(float_bits, 3));
			return 0;
		case GL_SUB_RASTER_POS4DV: NativeGLRasterPos4dv(gl_current_context, r3); return 0;
		case GL_SUB_RASTER_POS4F: NativeGLRasterPos4f(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2), float_arg(float_bits, 3)); return 0;
		case GL_SUB_RASTER_POS4FV: NativeGLRasterPos4fv(gl_current_context, r3); return 0;
		case GL_SUB_RASTER_POS4I: NativeGLRasterPos4i(gl_current_context, (int32_t)r3, (int32_t)r4, (int32_t)r5, (int32_t)r6); return 0;
		case GL_SUB_RASTER_POS4IV: NativeGLRasterPos4iv(gl_current_context, r3); return 0;
		case GL_SUB_RASTER_POS4S: NativeGLRasterPos4s(gl_current_context, (int16_t)r3, (int16_t)r4, (int16_t)r5, (int16_t)r6); return 0;
		case GL_SUB_RASTER_POS4SV: NativeGLRasterPos4sv(gl_current_context, r3); return 0;
		case GL_SUB_READ_BUFFER: NativeGLReadBuffer(gl_current_context, r3); return 0;
		case GL_SUB_READ_PIXELS:
			NativeGLReadPixels(gl_current_context, (int32_t)r3, (int32_t)r4,
			                    (int32_t)r5, (int32_t)r6, r7, r8, r9);
			return 0;
		case GL_SUB_RECTD: NativeGLRectd(gl_current_context, (double)float_arg(float_bits,0), (double)float_arg(float_bits,1), (double)float_arg(float_bits,2), (double)float_arg(float_bits,3)); return 0;
		case GL_SUB_RECTDV: NativeGLRectdv(gl_current_context, r3, r4); return 0;
		case GL_SUB_RECTF: NativeGLRectf(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2), float_arg(float_bits, 3)); return 0;
		case GL_SUB_RECTFV: NativeGLRectfv(gl_current_context, r3, r4); return 0;
		case GL_SUB_RECTI: NativeGLRecti(gl_current_context, (int32_t)r3, (int32_t)r4, (int32_t)r5, (int32_t)r6); return 0;
		case GL_SUB_RECTIV: NativeGLRectiv(gl_current_context, r3, r4); return 0;
		case GL_SUB_RECTS: NativeGLRects(gl_current_context, (int16_t)r3, (int16_t)r4, (int16_t)r5, (int16_t)r6); return 0;
		case GL_SUB_RECTSV: NativeGLRectsv(gl_current_context, r3, r4); return 0;
		case GL_SUB_RENDER_MODE: return NativeGLRenderMode(gl_current_context, r3);
		case GL_SUB_ROTATED:
			NativeGLRotated(gl_current_context,
				(double)float_arg(float_bits, 0), (double)float_arg(float_bits, 1),
				(double)float_arg(float_bits, 2), (double)float_arg(float_bits, 3));
			return 0;
		case GL_SUB_ROTATEF:
			NativeGLRotatef(gl_current_context,
				float_arg(float_bits, 0), float_arg(float_bits, 1),
				float_arg(float_bits, 2), float_arg(float_bits, 3));
			return 0;
		case GL_SUB_SCALED:
			NativeGLScaled(gl_current_context,
				(double)float_arg(float_bits, 0), (double)float_arg(float_bits, 1),
				(double)float_arg(float_bits, 2));
			return 0;
		case GL_SUB_SCALEF:
			NativeGLScalef(gl_current_context,
				float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2));
			return 0;
		// --- Core GL: Scissor, Select, Shade, Stencil ---
		case GL_SUB_SCISSOR:
			NativeGLScissor(gl_current_context, (int32_t)r3, (int32_t)r4, (int32_t)r5, (int32_t)r6);
			return 0;
		case GL_SUB_SELECT_BUFFER: NativeGLSelectBuffer(gl_current_context, (int32_t)r3, r4); return 0;
		case GL_SUB_SHADE_MODEL:
			NativeGLShadeModel(gl_current_context, r3);
			return 0;
		case GL_SUB_STENCIL_FUNC:
			NativeGLStencilFunc(gl_current_context, r3, (int32_t)r4, r5);
			return 0;
		case GL_SUB_STENCIL_MASK:
			NativeGLStencilMask(gl_current_context, r3);
			return 0;
		case GL_SUB_STENCIL_OP:
			NativeGLStencilOp(gl_current_context, r3, r4, r5);
			return 0;
		// --- Core GL: TexCoord ---
		case GL_SUB_TEX_COORD1D: {
			NativeGLTexCoord1d(gl_current_context, double_arg(float_bits, 0)); return 0;
		}
		case GL_SUB_TEX_COORD1DV: NativeGLTexCoord1dv(gl_current_context, r3); return 0;
		case GL_SUB_TEX_COORD1F: NativeGLTexCoord1f(gl_current_context, float_arg(float_bits, 0)); return 0;
		case GL_SUB_TEX_COORD1FV: NativeGLTexCoord1fv(gl_current_context, r3); return 0;
		case GL_SUB_TEX_COORD1I: NativeGLTexCoord1i(gl_current_context, (int32_t)r3); return 0;
		case GL_SUB_TEX_COORD1IV: NativeGLTexCoord1iv(gl_current_context, r3); return 0;
		case GL_SUB_TEX_COORD1S: NativeGLTexCoord1s(gl_current_context, (int16_t)r3); return 0;
		case GL_SUB_TEX_COORD1SV: NativeGLTexCoord1sv(gl_current_context, r3); return 0;
		case GL_SUB_TEX_COORD2D: {
			NativeGLTexCoord2d(gl_current_context,
			                   double_arg(float_bits, 0),
			                   double_arg(float_bits, 1)); return 0;
		}
		case GL_SUB_TEX_COORD2DV: NativeGLTexCoord2dv(gl_current_context, r3); return 0;
		case GL_SUB_TEX_COORD2F: NativeGLTexCoord2f(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1)); return 0;
		case GL_SUB_TEX_COORD2FV: NativeGLTexCoord2fv(gl_current_context, r3); return 0;
		case GL_SUB_TEX_COORD2I: NativeGLTexCoord2i(gl_current_context, (int32_t)r3, (int32_t)r4); return 0;
		case GL_SUB_TEX_COORD2IV: NativeGLTexCoord2iv(gl_current_context, r3); return 0;
		case GL_SUB_TEX_COORD2S: NativeGLTexCoord2s(gl_current_context, (int16_t)r3, (int16_t)r4); return 0;
		case GL_SUB_TEX_COORD2SV: NativeGLTexCoord2sv(gl_current_context, r3); return 0;
		case GL_SUB_TEX_COORD3D: {
			NativeGLTexCoord3d(gl_current_context,
			                   double_arg(float_bits, 0),
			                   double_arg(float_bits, 1),
			                   double_arg(float_bits, 2)); return 0;
		}
		case GL_SUB_TEX_COORD3DV: NativeGLTexCoord3dv(gl_current_context, r3); return 0;
		case GL_SUB_TEX_COORD3F: NativeGLTexCoord3f(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2)); return 0;
		case GL_SUB_TEX_COORD3FV: NativeGLTexCoord3fv(gl_current_context, r3); return 0;
		case GL_SUB_TEX_COORD3I: NativeGLTexCoord3i(gl_current_context, (int32_t)r3, (int32_t)r4, (int32_t)r5); return 0;
		case GL_SUB_TEX_COORD3IV: NativeGLTexCoord3iv(gl_current_context, r3); return 0;
		case GL_SUB_TEX_COORD3S: NativeGLTexCoord3s(gl_current_context, (int16_t)r3, (int16_t)r4, (int16_t)r5); return 0;
		case GL_SUB_TEX_COORD3SV: NativeGLTexCoord3sv(gl_current_context, r3); return 0;
		case GL_SUB_TEX_COORD4D: {
			NativeGLTexCoord4d(gl_current_context,
			                   double_arg(float_bits, 0),
			                   double_arg(float_bits, 1),
			                   double_arg(float_bits, 2),
			                   double_arg(float_bits, 3)); return 0;
		}
		case GL_SUB_TEX_COORD4DV: NativeGLTexCoord4dv(gl_current_context, r3); return 0;
		case GL_SUB_TEX_COORD4F: NativeGLTexCoord4f(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2), float_arg(float_bits, 3)); return 0;
		case GL_SUB_TEX_COORD4FV: NativeGLTexCoord4fv(gl_current_context, r3); return 0;
		case GL_SUB_TEX_COORD4I: NativeGLTexCoord4i(gl_current_context, (int32_t)r3, (int32_t)r4, (int32_t)r5, (int32_t)r6); return 0;
		case GL_SUB_TEX_COORD4IV: NativeGLTexCoord4iv(gl_current_context, r3); return 0;
		case GL_SUB_TEX_COORD4S: NativeGLTexCoord4s(gl_current_context, (int16_t)r3, (int16_t)r4, (int16_t)r5, (int16_t)r6); return 0;
		case GL_SUB_TEX_COORD4SV: NativeGLTexCoord4sv(gl_current_context, r3); return 0;
		// --- Core GL: TexEnv, TexGen, TexImage, TexParameter, TexSubImage ---
		case GL_SUB_TEX_COORD_POINTER: NativeGLTexCoordPointer(gl_current_context, (int32_t)r3, r4, (int32_t)r5, r6); return 0;
		case GL_SUB_TEX_ENVF:
			NativeGLTexEnvf(gl_current_context, r3, r4, float_arg(float_bits, 0));
			return 0;
		case GL_SUB_TEX_ENVFV:
			NativeGLTexEnvfv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_TEX_ENVI:
			NativeGLTexEnvi(gl_current_context, r3, r4, (int32_t)r5);
			return 0;
		case GL_SUB_TEX_ENVIV:
			NativeGLTexEnviv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_TEX_GEND: {
			NativeGLTexGend(gl_current_context, r3, r4,
			                double_arg(float_bits, 0));
			return 0;
		}
		case GL_SUB_TEX_GENDV:
			NativeGLTexGendv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_TEX_GENF:
			NativeGLTexGenf(gl_current_context, r3, r4, float_arg(float_bits, 0));
			return 0;
		case GL_SUB_TEX_GENFV:
			NativeGLTexGenfv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_TEX_GENI:
			NativeGLTexGeni(gl_current_context, r3, r4, (int32_t)r5);
			return 0;
		case GL_SUB_TEX_GENIV:
			NativeGLTexGeniv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_TEX_IMAGE1D:
			NativeGLTexImage1D(gl_current_context, r3, (int32_t)r4, (int32_t)r5,
			                    (int32_t)r6, (int32_t)r7, r8, r9, r10);
			return 0;
		case GL_SUB_TEX_IMAGE2D:
			{
				extern uint32_t gl_ppc_stack_arg(int index);
				extern uint32_t gl_ppc_sp;
				extern int gl_ppc_stack_arg_offset;
				// glTexImage2D has 9 args. In the dispatch-table path (offset=1),
				// r10=0 (unused) and the real args 8-9 (type, pixels) are on the
				// stack at shifted positions. Read type from stack arg -1 (the slot
				// that would have been r10) and pixels from stack arg 0.
				uint32_t type, pixels;
				// This branch handles the dispatch-table calling convention
				// (gl_dt_flag_addr == 1). The "else" branch handles the stub-call
				// convention (gl_dt_flag_addr == 0). Both are real dispatch paths;
				// neither is a "stub" in the unimplemented sense. See the gl_thunks.cpp
				// comment for the full calling-convention contract.
				if (gl_ppc_stack_arg_offset) {
					// Dispatch-table path: type and pixels are both on the stack
					type   = gl_ppc_stack_arg(-1);  // game's r10 equivalent
					pixels = gl_ppc_stack_arg(0);    // game's stack arg 0
				} else {
					// FindLibSymbol path: type is r10, pixels is stack arg 0
					type   = r10;
					pixels = gl_ppc_stack_arg(0);
				}
				GL_VLOG("glTexImage2D target=0x%x level=%d ifmt=%d w=%d h=%d border=%d fmt=0x%x type=0x%x pixels=0x%08x (sp=0x%08x dt=%d)",
				       r3, (int32_t)r4, (int32_t)r5, (int32_t)r6, (int32_t)r7, (int32_t)r8, r9, type, pixels, gl_ppc_sp, gl_ppc_stack_arg_offset);
				NativeGLTexImage2D(gl_current_context, r3, (int32_t)r4, (int32_t)r5,
				                    (int32_t)r6, (int32_t)r7, (int32_t)r8, r9, type, pixels);
			}
			return 0;
		case GL_SUB_TEX_PARAMETERF:
			NativeGLTexParameterf(gl_current_context, r3, r4, float_arg(float_bits, 0));
			return 0;
		case GL_SUB_TEX_PARAMETERFV:
			NativeGLTexParameterfv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_TEX_PARAMETERI:
			NativeGLTexParameteri(gl_current_context, r3, r4, (int32_t)r5);
			return 0;
		case GL_SUB_TEX_PARAMETERIV:
			NativeGLTexParameteriv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_TEX_SUB_IMAGE1D:
			NativeGLTexSubImage1D(gl_current_context, r3, (int32_t)r4,
			                      (int32_t)r5, (int32_t)r6, r7, r8, r9);
			return 0;
		case GL_SUB_TEX_SUB_IMAGE2D:
			{
				// 9 args: target, level, xoff, yoff, width, height, format, type, pixels
				extern uint32_t gl_ppc_stack_arg(int index);
				extern int gl_ppc_stack_arg_offset;
				uint32_t type, pixels;
				if (gl_ppc_stack_arg_offset) {
					type   = gl_ppc_stack_arg(-1);
					pixels = gl_ppc_stack_arg(0);
				} else {
					type   = r10;
					pixels = gl_ppc_stack_arg(0);
				}
				NativeGLTexSubImage2D(gl_current_context, r3, (int32_t)r4,
				                       (int32_t)r5, (int32_t)r6, (int32_t)r7, (int32_t)r8,
				                       r9, type, pixels);
			}
			return 0;
		// --- Core GL: Translate, Vertex, Viewport ---
		case GL_SUB_TRANSLATED:
			NativeGLTranslated(gl_current_context,
				(double)float_arg(float_bits, 0), (double)float_arg(float_bits, 1),
				(double)float_arg(float_bits, 2));
			return 0;
		case GL_SUB_TRANSLATEF:
			NativeGLTranslatef(gl_current_context,
				float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2));
			return 0;
		case GL_SUB_VERTEX2D: {
			NativeGLVertex2d(gl_current_context,
			                 double_arg(float_bits, 0),
			                 double_arg(float_bits, 1)); return 0;
		}
		case GL_SUB_VERTEX2DV: NativeGLVertex2dv(gl_current_context, r3); return 0;
		case GL_SUB_VERTEX2F:
			NativeGLVertex2f(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1));
			return 0;
		case GL_SUB_VERTEX2FV: NativeGLVertex2fv(gl_current_context, r3); return 0;
		case GL_SUB_VERTEX2I: NativeGLVertex2i(gl_current_context, (int32_t)r3, (int32_t)r4); return 0;
		case GL_SUB_VERTEX2IV: NativeGLVertex2iv(gl_current_context, r3); return 0;
		case GL_SUB_VERTEX2S: NativeGLVertex2s(gl_current_context, (int16_t)r3, (int16_t)r4); return 0;
		case GL_SUB_VERTEX2SV: NativeGLVertex2sv(gl_current_context, r3); return 0;
		case GL_SUB_VERTEX3D: {
			NativeGLVertex3d(gl_current_context,
			                 double_arg(float_bits, 0),
			                 double_arg(float_bits, 1),
			                 double_arg(float_bits, 2)); return 0;
		}
		case GL_SUB_VERTEX3DV: NativeGLVertex3dv(gl_current_context, r3); return 0;
		case GL_SUB_VERTEX3F:
			NativeGLVertex3f(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2));
			return 0;
		case GL_SUB_VERTEX3FV: NativeGLVertex3fv(gl_current_context, r3); return 0;
		case GL_SUB_VERTEX3I: NativeGLVertex3i(gl_current_context, (int32_t)r3, (int32_t)r4, (int32_t)r5); return 0;
		case GL_SUB_VERTEX3IV: NativeGLVertex3iv(gl_current_context, r3); return 0;
		case GL_SUB_VERTEX3S: NativeGLVertex3s(gl_current_context, (int16_t)r3, (int16_t)r4, (int16_t)r5); return 0;
		case GL_SUB_VERTEX3SV: NativeGLVertex3sv(gl_current_context, r3); return 0;
		case GL_SUB_VERTEX4D: {
			NativeGLVertex4d(gl_current_context,
			                 double_arg(float_bits, 0),
			                 double_arg(float_bits, 1),
			                 double_arg(float_bits, 2),
			                 double_arg(float_bits, 3)); return 0;
		}
		case GL_SUB_VERTEX4DV: NativeGLVertex4dv(gl_current_context, r3); return 0;
		case GL_SUB_VERTEX4F:
			NativeGLVertex4f(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1),
			                 float_arg(float_bits, 2), float_arg(float_bits, 3));
			return 0;
		case GL_SUB_VERTEX4FV: NativeGLVertex4fv(gl_current_context, r3); return 0;
		case GL_SUB_VERTEX4I: NativeGLVertex4i(gl_current_context, (int32_t)r3, (int32_t)r4, (int32_t)r5, (int32_t)r6); return 0;
		case GL_SUB_VERTEX4IV: NativeGLVertex4iv(gl_current_context, r3); return 0;
		case GL_SUB_VERTEX4S: NativeGLVertex4s(gl_current_context, (int16_t)r3, (int16_t)r4, (int16_t)r5, (int16_t)r6); return 0;
		case GL_SUB_VERTEX4SV: NativeGLVertex4sv(gl_current_context, r3); return 0;
		case GL_SUB_VERTEX_POINTER: NativeGLVertexPointer(gl_current_context, (int32_t)r3, r4, (int32_t)r5, r6); return 0;
		case GL_SUB_VIEWPORT:
			NativeGLViewport(gl_current_context, (int32_t)r3, (int32_t)r4, (int32_t)r5, (int32_t)r6);
			return 0;

		// --- GL Extensions (400-503) ---

		// EXT_blend_color / EXT_blend_equation
		case GL_SUB_BLEND_COLOR_EXT:
			NativeGLBlendColorEXT(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2), float_arg(float_bits, 3));
			return 0;
		case GL_SUB_BLEND_EQUATION_EXT:
			NativeGLBlendEquationEXT(gl_current_context, r3);
			return 0;

		// EXT_compiled_vertex_array
		case GL_SUB_LOCK_ARRAYS_EXT:
			NativeGLLockArraysEXT(gl_current_context, (int32_t)r3, (int32_t)r4);
			return 0;
		case GL_SUB_UNLOCK_ARRAYS_EXT:
			NativeGLUnlockArraysEXT(gl_current_context);
			return 0;

		// ARB_multitexture
		case GL_SUB_CLIENT_ACTIVE_TEXTURE_ARB:
			NativeGLClientActiveTextureARB(gl_current_context, r3);
			return 0;
		case GL_SUB_ACTIVE_TEXTURE_ARB:
			NativeGLActiveTextureARB(gl_current_context, r3);
			return 0;

		// MultiTexCoord 1D variants
		case GL_SUB_MULTI_TEX_COORD1D_ARB: {
			NativeGLMultiTexCoord1fARB(gl_current_context, r3,
			                           float_arg(float_bits, 0));
			return 0;
		}
		case GL_SUB_MULTI_TEX_COORD1DV_ARB:
			NativeGLMultiTexCoord1dvARB(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_MULTI_TEX_COORD1F_ARB:
			NativeGLMultiTexCoord1fARB(gl_current_context, r3, float_arg(float_bits, 0));
			return 0;
		case GL_SUB_MULTI_TEX_COORD1FV_ARB:
			NativeGLMultiTexCoord1fvARB(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_MULTI_TEX_COORD1I_ARB:
			NativeGLMultiTexCoord1iARB(gl_current_context, r3, (int32_t)r4);
			return 0;
		case GL_SUB_MULTI_TEX_COORD1IV_ARB:
			NativeGLMultiTexCoord1ivARB(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_MULTI_TEX_COORD1S_ARB:
			NativeGLMultiTexCoord1sARB(gl_current_context, r3, (int16_t)r4);
			return 0;
		case GL_SUB_MULTI_TEX_COORD1SV_ARB:
			NativeGLMultiTexCoord1svARB(gl_current_context, r3, r4);
			return 0;

		// MultiTexCoord 2D variants
		case GL_SUB_MULTI_TEX_COORD2D_ARB: {
			NativeGLMultiTexCoord2fARB(gl_current_context, r3,
			                           float_arg(float_bits, 0),
			                           float_arg(float_bits, 1));
			return 0;
		}
		case GL_SUB_MULTI_TEX_COORD2DV_ARB:
			NativeGLMultiTexCoord2dvARB(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_MULTI_TEX_COORD2F_ARB:
			NativeGLMultiTexCoord2fARB(gl_current_context, r3, float_arg(float_bits, 0), float_arg(float_bits, 1));
			return 0;
		case GL_SUB_MULTI_TEX_COORD2FV_ARB:
			NativeGLMultiTexCoord2fvARB(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_MULTI_TEX_COORD2I_ARB:
			NativeGLMultiTexCoord2iARB(gl_current_context, r3, (int32_t)r4, (int32_t)r5);
			return 0;
		case GL_SUB_MULTI_TEX_COORD2IV_ARB:
			NativeGLMultiTexCoord2ivARB(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_MULTI_TEX_COORD2S_ARB:
			NativeGLMultiTexCoord2sARB(gl_current_context, r3, (int16_t)r4, (int16_t)r5);
			return 0;
		case GL_SUB_MULTI_TEX_COORD2SV_ARB:
			NativeGLMultiTexCoord2svARB(gl_current_context, r3, r4);
			return 0;

		// MultiTexCoord 3D variants
		case GL_SUB_MULTI_TEX_COORD3D_ARB: {
			NativeGLMultiTexCoord3fARB(gl_current_context, r3,
			                           float_arg(float_bits, 0),
			                           float_arg(float_bits, 1),
			                           float_arg(float_bits, 2));
			return 0;
		}
		case GL_SUB_MULTI_TEX_COORD3DV_ARB:
			NativeGLMultiTexCoord3dvARB(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_MULTI_TEX_COORD3F_ARB:
			NativeGLMultiTexCoord3fARB(gl_current_context, r3, float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2));
			return 0;
		case GL_SUB_MULTI_TEX_COORD3FV_ARB:
			NativeGLMultiTexCoord3fvARB(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_MULTI_TEX_COORD3I_ARB:
			NativeGLMultiTexCoord3iARB(gl_current_context, r3, (int32_t)r4, (int32_t)r5, (int32_t)r6);
			return 0;
		case GL_SUB_MULTI_TEX_COORD3IV_ARB:
			NativeGLMultiTexCoord3ivARB(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_MULTI_TEX_COORD3S_ARB:
			NativeGLMultiTexCoord3sARB(gl_current_context, r3, (int16_t)r4, (int16_t)r5, (int16_t)r6);
			return 0;
		case GL_SUB_MULTI_TEX_COORD3SV_ARB:
			NativeGLMultiTexCoord3svARB(gl_current_context, r3, r4);
			return 0;

		// MultiTexCoord 4D variants
		case GL_SUB_MULTI_TEX_COORD4D_ARB: {
			NativeGLMultiTexCoord4fARB(gl_current_context, r3,
			                           float_arg(float_bits, 0),
			                           float_arg(float_bits, 1),
			                           float_arg(float_bits, 2),
			                           float_arg(float_bits, 3));
			return 0;
		}
		case GL_SUB_MULTI_TEX_COORD4DV_ARB:
			NativeGLMultiTexCoord4dvARB(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_MULTI_TEX_COORD4F_ARB:
			NativeGLMultiTexCoord4fARB(gl_current_context, r3, float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2), float_arg(float_bits, 3));
			return 0;
		case GL_SUB_MULTI_TEX_COORD4FV_ARB:
			NativeGLMultiTexCoord4fvARB(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_MULTI_TEX_COORD4I_ARB:
			NativeGLMultiTexCoord4iARB(gl_current_context, r3, (int32_t)r4, (int32_t)r5, (int32_t)r6, (int32_t)r7);
			return 0;
		case GL_SUB_MULTI_TEX_COORD4IV_ARB:
			NativeGLMultiTexCoord4ivARB(gl_current_context, r3, r4);
			return 0;
		case GL_SUB_MULTI_TEX_COORD4S_ARB:
			NativeGLMultiTexCoord4sARB(gl_current_context, r3, (int16_t)r4, (int16_t)r5, (int16_t)r6, (int16_t)r7);
			return 0;
		case GL_SUB_MULTI_TEX_COORD4SV_ARB:
			NativeGLMultiTexCoord4svARB(gl_current_context, r3, r4);
			return 0;

		// ARB_transpose_matrix
		case GL_SUB_LOAD_TRANSPOSE_MATRIXD_ARB:
			NativeGLLoadTransposeMatrixdARB(gl_current_context, r3);
			return 0;
		case GL_SUB_LOAD_TRANSPOSE_MATRIXF_ARB:
			NativeGLLoadTransposeMatrixfARB(gl_current_context, r3);
			return 0;
		case GL_SUB_MULT_TRANSPOSE_MATRIXD_ARB:
			NativeGLMultTransposeMatrixdARB(gl_current_context, r3);
			return 0;
		case GL_SUB_MULT_TRANSPOSE_MATRIXF_ARB:
			NativeGLMultTransposeMatrixfARB(gl_current_context, r3);
			return 0;

		// ARB_texture_compression
		case GL_SUB_COMPRESSED_TEX_IMAGE3D_ARB:
			NativeGLCompressedTexImage3DARB(gl_current_context, r3, (int32_t)r4, r5, (int32_t)r6, (int32_t)r7, (int32_t)r8, (int32_t)r9, (int32_t)r10, 0 /*stack*/);
			return 0;
		case GL_SUB_COMPRESSED_TEX_IMAGE2D_ARB:
			NativeGLCompressedTexImage2DARB(gl_current_context, r3, (int32_t)r4, r5, (int32_t)r6, (int32_t)r7, (int32_t)r8, (int32_t)r9, r10);
			return 0;
		case GL_SUB_COMPRESSED_TEX_IMAGE1D_ARB:
			NativeGLCompressedTexImage1DARB(gl_current_context, r3, (int32_t)r4, r5, (int32_t)r6, (int32_t)r7, (int32_t)r8, r9);
			return 0;
		case GL_SUB_COMPRESSED_TEX_SUB_IMAGE3D_ARB:
			NativeGLCompressedTexSubImage3DARB(gl_current_context, r3, (int32_t)r4, (int32_t)r5, (int32_t)r6, (int32_t)r7, (int32_t)r8, (int32_t)r9, (int32_t)r10, 0, 0, 0 /*stack args*/);
			return 0;
		case GL_SUB_COMPRESSED_TEX_SUB_IMAGE2D_ARB:
			NativeGLCompressedTexSubImage2DARB(gl_current_context, r3, (int32_t)r4, (int32_t)r5, (int32_t)r6, (int32_t)r7, (int32_t)r8, r9, (int32_t)r10, 0 /*stack*/);
			return 0;
		case GL_SUB_COMPRESSED_TEX_SUB_IMAGE1D_ARB:
			NativeGLCompressedTexSubImage1DARB(gl_current_context, r3, (int32_t)r4, (int32_t)r5, (int32_t)r6, r7, (int32_t)r8, r9);
			return 0;
		case GL_SUB_GET_COMPRESSED_TEX_IMAGE_ARB:
			NativeGLGetCompressedTexImageARB(gl_current_context, r3, (int32_t)r4, r5);
			return 0;

		// EXT_secondary_color
		case GL_SUB_SECONDARYCOLOR3B_EXT:
			NativeGLSecondaryColor3bEXT(gl_current_context, (int8_t)r3, (int8_t)r4, (int8_t)r5);
			return 0;
		case GL_SUB_SECONDARYCOLOR3BV_EXT:
			NativeGLSecondaryColor3bvEXT(gl_current_context, r3);
			return 0;
		case GL_SUB_SECONDARYCOLOR3D_EXT: {
			NativeGLSecondaryColor3fEXT(gl_current_context,
			                            float_arg(float_bits, 0),
			                            float_arg(float_bits, 1),
			                            float_arg(float_bits, 2));
			return 0;
		}
		case GL_SUB_SECONDARY_COLOR3DV_EXT:
			NativeGLSecondaryColor3dvEXT(gl_current_context, r3);
			return 0;
		case GL_SUB_SECONDARY_COLOR3F_EXT:
			NativeGLSecondaryColor3fEXT(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2));
			return 0;
		case GL_SUB_SECONDARY_COLOR3FV_EXT:
			NativeGLSecondaryColor3fvEXT(gl_current_context, r3);
			return 0;
		case GL_SUB_SECONDARY_COLOR3I_EXT:
			NativeGLSecondaryColor3iEXT(gl_current_context, (int32_t)r3, (int32_t)r4, (int32_t)r5);
			return 0;
		case GL_SUB_SECONDARY_COLOR3IV_EXT:
			NativeGLSecondaryColor3ivEXT(gl_current_context, r3);
			return 0;
		case GL_SUB_SECONDARY_COLOR3S_EXT:
			NativeGLSecondaryColor3sEXT(gl_current_context, (int16_t)r3, (int16_t)r4, (int16_t)r5);
			return 0;
		case GL_SUB_SECONDARY_COLOR3SV_EXT:
			NativeGLSecondaryColor3svEXT(gl_current_context, r3);
			return 0;
		case GL_SUB_SECONDARY_COLOR3UB_EXT:
			NativeGLSecondaryColor3ubEXT(gl_current_context, (uint8_t)r3, (uint8_t)r4, (uint8_t)r5);
			return 0;
		case GL_SUB_SECONDARY_COLOR3UBV_EXT:
			NativeGLSecondaryColor3ubvEXT(gl_current_context, r3);
			return 0;
		case GL_SUB_SECONDARY_COLOR3UI_EXT:
			NativeGLSecondaryColor3uiEXT(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_SECONDARY_COLOR3UIV_EXT:
			NativeGLSecondaryColor3uivEXT(gl_current_context, r3);
			return 0;
		case GL_SUB_SECONDARY_COLOR3US_EXT:
			NativeGLSecondaryColor3usEXT(gl_current_context, (uint16_t)r3, (uint16_t)r4, (uint16_t)r5);
			return 0;
		case GL_SUB_SECONDARY_COLOR3USV_EXT:
			NativeGLSecondaryColor3usvEXT(gl_current_context, r3);
			return 0;
		case GL_SUB_SECONDARY_COLOR_POINTER_EXT:
			NativeGLSecondaryColorPointerEXT(gl_current_context, (int32_t)r3, r4, (int32_t)r5, r6);
			return 0;

		// OpenGL 1.2 imaging subset
		case GL_SUB_BLEND_COLOR_1_2:
			NativeGLBlendColorEXT(gl_current_context, float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2), float_arg(float_bits, 3));
			return 0;
		case GL_SUB_BLEND_EQUATION_1_2:
			NativeGLBlendEquationEXT(gl_current_context, r3);
			return 0;
		case GL_SUB_DRAW_RANGE_ELEMENTS: NativeGLDrawRangeElements(gl_current_context, r3, r4, r5, (int32_t)r6, r7, r8); return 0;
		case GL_SUB_COLOR_TABLE:
			NativeGLColorTable(gl_current_context, r3, r4, (int32_t)r5, r6, r7, r8);
			return 0;
		case GL_SUB_COLOR_TABLE_PARAMETERFV:
			NativeGLColorTableParameterfv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_COLOR_TABLE_PARAMETERIV:
			NativeGLColorTableParameteriv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_COPY_COLOR_TABLE:
			NativeGLCopyColorTable(gl_current_context, r3, r4, (int32_t)r5, (int32_t)r6, (int32_t)r7);
			return 0;
		case GL_SUB_GET_COLOR_TABLE:
			NativeGLGetColorTable(gl_current_context, r3, r4, r5, r6);
			return 0;
		case GL_SUB_GET_COLOR_TABLE_PARAMETERFV:
			NativeGLGetColorTableParameterfv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_GET_COLOR_TABLE_PARAMETERIV:
			NativeGLGetColorTableParameteriv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_COLOR_SUB_TABLE:
			NativeGLColorSubTable(gl_current_context, r3, (int32_t)r4, (int32_t)r5, r6, r7, r8);
			return 0;
		case GL_SUB_COPY_COLOR_SUB_TABLE:
			NativeGLCopyColorSubTable(gl_current_context, r3, (int32_t)r4, (int32_t)r5, (int32_t)r6, (int32_t)r7);
			return 0;
		case GL_SUB_CONVOLUTION_FILTER1D:
			NativeGLConvolutionFilter1D(gl_current_context, r3, r4, (int32_t)r5, r6, r7, r8);
			return 0;
		case GL_SUB_CONVOLUTION_FILTER2D:
			NativeGLConvolutionFilter2D(gl_current_context, r3, r4, (int32_t)r5, (int32_t)r6, r7, r8, r9);
			return 0;
		case GL_SUB_CONVOLUTION_PARAMETERF:
			NativeGLConvolutionParameterf(gl_current_context, r3, r4, float_arg(float_bits, 0));
			return 0;
		case GL_SUB_CONVOLUTION_PARAMETERFV:
			NativeGLConvolutionParameterfv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_CONVOLUTION_PARAMETERI:
			NativeGLConvolutionParameteri(gl_current_context, r3, r4, (int32_t)r5);
			return 0;
		case GL_SUB_CONVOLUTION_PARAMETERIV:
			NativeGLConvolutionParameteriv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_COPY_CONVOLUTION_FILTER1D:
			NativeGLCopyConvolutionFilter1D(gl_current_context, r3, r4, (int32_t)r5, (int32_t)r6, (int32_t)r7);
			return 0;
		case GL_SUB_COPY_CONVOLUTION_FILTER2D:
			NativeGLCopyConvolutionFilter2D(gl_current_context, r3, r4, (int32_t)r5, (int32_t)r6, (int32_t)r7, (int32_t)r8);
			return 0;
		case GL_SUB_GET_CONVOLUTION_FILTER:
			NativeGLGetConvolutionFilter(gl_current_context, r3, r4, r5, r6);
			return 0;
		case GL_SUB_GET_CONVOLUTION_PARAMETERFV:
			NativeGLGetConvolutionParameterfv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_GET_CONVOLUTION_PARAMETERIV:
			NativeGLGetConvolutionParameteriv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_GET_SEPARABLE_FILTER:
			NativeGLGetSeparableFilter(gl_current_context, r3, r4, r5, r6, r7, r8);
			return 0;
		case GL_SUB_SEPARABLE_FILTER2D:
			NativeGLSeparableFilter2D(gl_current_context, r3, r4, (int32_t)r5, (int32_t)r6, r7, r8, r9, r10);
			return 0;
		case GL_SUB_GET_HISTOGRAM:
			NativeGLGetHistogram(gl_current_context, r3, r4, r5, r6, r7);
			return 0;
		case GL_SUB_GET_HISTOGRAM_PARAMETERFV:
			NativeGLGetHistogramParameterfv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_GET_HISTOGRAM_PARAMETERIV:
			NativeGLGetHistogramParameteriv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_GET_MINMAX:
			NativeGLGetMinmax(gl_current_context, r3, r4, r5, r6, r7);
			return 0;
		case GL_SUB_GET_MINMAX_PARAMETERFV:
			NativeGLGetMinmaxParameterfv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_GET_MINMAX_PARAMETERIV:
			NativeGLGetMinmaxParameteriv(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_HISTOGRAM:
			NativeGLHistogram(gl_current_context, r3, (int32_t)r4, r5, r6);
			return 0;
		case GL_SUB_MINMAX:
			NativeGLMinmax(gl_current_context, r3, r4, r5);
			return 0;
		case GL_SUB_RESET_HISTOGRAM:
			NativeGLResetHistogram(gl_current_context, r3);
			return 0;
		case GL_SUB_RESET_MINMAX:
			NativeGLResetMinmax(gl_current_context, r3);
			return 0;
		case GL_SUB_TEX_IMAGE3D_EXT:
			NativeGLTexImage3DEXT(gl_current_context, r3, (int32_t)r4, (int32_t)r5, (int32_t)r6, (int32_t)r7, (int32_t)r8, (int32_t)r9, r10, 0 /*stack*/, 0 /*stack*/);
			return 0;
		case GL_SUB_TEX_SUB_IMAGE3D_EXT:
			NativeGLTexSubImage3DEXT(gl_current_context, r3, (int32_t)r4, (int32_t)r5, (int32_t)r6, (int32_t)r7, (int32_t)r8, (int32_t)r9, (int32_t)r10, 0, 0, 0 /*stack args*/);
			return 0;
		case GL_SUB_COPY_TEX_SUB_IMAGE3D_EXT:
			NativeGLCopyTexSubImage3DEXT(gl_current_context, r3, (int32_t)r4, (int32_t)r5, (int32_t)r6, (int32_t)r7, (int32_t)r8, (int32_t)r9, (int32_t)r10, 0 /*stack*/);
			return 0;

		// --- AGL - Apple GL (600-632) --- real handlers in gl_engine.cpp
		case GL_SUB_AGL_CHOOSEPIXELFORMAT:    return NativeAGLChoosePixelFormat(r3, r4, r5);
		case GL_SUB_AGL_DESTROYPIXELFORMAT:   return NativeAGLDestroyPixelFormat(r3);
		case GL_SUB_AGL_NEXTPIXELFORMAT:      return NativeAGLNextPixelFormat(r3);
		case GL_SUB_AGL_DESCRIBEPIXELFORMAT:  return NativeAGLDescribePixelFormat(r3, r4, r5);
		case GL_SUB_AGL_DEVICESOFPIXELFORMAT: return NativeAGLDevicesOfPixelFormat(r3, r4);
		case GL_SUB_AGL_QUERYRENDERERINFO:    return NativeAGLQueryRendererInfo(r3, r4);
		case GL_SUB_AGL_DESTROYRENDERERINFO:  return NativeAGLDestroyRendererInfo(r3);
		case GL_SUB_AGL_NEXTRENDERERINFO:     return NativeAGLNextRendererInfo(r3);
		case GL_SUB_AGL_DESCRIBERENDERER:     return NativeAGLDescribeRenderer(r3, r4, r5);
		case GL_SUB_AGL_CREATECONTEXT:        return NativeAGLCreateContext(r3, r4);
		case GL_SUB_AGL_DESTROYCONTEXT:       return NativeAGLDestroyContext(r3);
		case GL_SUB_AGL_COPYCONTEXT:          return NativeAGLCopyContext(r3, r4, r5);
		case GL_SUB_AGL_UPDATECONTEXT:        return NativeAGLUpdateContext(r3);
		case GL_SUB_AGL_SETCURRENTCONTEXT:    return NativeAGLSetCurrentContext(r3);
		case GL_SUB_AGL_GETCURRENTCONTEXT:    return NativeAGLGetCurrentContext();
		case GL_SUB_AGL_SETDRAWABLE:          return NativeAGLSetDrawable(r3, r4);
		case GL_SUB_AGL_SETOFFSCREEN:         return NativeAGLSetOffScreen(r3, r4, r5, r6, r7);
		case GL_SUB_AGL_SETFULLSCREEN:        return NativeAGLSetFullScreen(r3, r4, r5, r6, r7);
		case GL_SUB_AGL_GETDRAWABLE:          return NativeAGLGetDrawable(r3);
		case GL_SUB_AGL_SETVIRTUALSCREEN:     return NativeAGLSetVirtualScreen(r3, r4);
		case GL_SUB_AGL_GETVIRTUALSCREEN:     return NativeAGLGetVirtualScreen(r3);
		case GL_SUB_AGL_GETVERSION:           return NativeAGLGetVersion(r3, r4);
		case GL_SUB_AGL_CONFIGURE:            return NativeAGLConfigure(r3, r4);
		case GL_SUB_AGL_SWAPBUFFERS: {
			// Task 9 periodic diagnostic report: log deferred-batching counters
			// every 120 frames when logging is enabled. Silent in shipping builds.
			static uint32_t s_swap_count = 0;
			if (++s_swap_count >= 120) {
				s_swap_count = 0;
				GL_LOG("GLDefer stats (120-frame): drain_calls=%llu records_drained=%llu fallbacks=%llu",
				       (unsigned long long)g_defer_drain_calls,
				       (unsigned long long)g_defer_records_drained,
				       (unsigned long long)g_defer_fallbacks);
			}
			return NativeAGLSwapBuffers(r3);
		}
		case GL_SUB_AGL_ENABLE:               return NativeAGLEnable(r3, r4);
		case GL_SUB_AGL_DISABLE:              return NativeAGLDisable(r3, r4);
		case GL_SUB_AGL_ISENABLED:            return NativeAGLIsEnabled(r3, r4);
		case GL_SUB_AGL_SETINTEGER:           return NativeAGLSetInteger(r3, r4, r5);
		case GL_SUB_AGL_GETINTEGER:           return NativeAGLGetInteger(r3, r4, r5);
		case GL_SUB_AGL_USEFONT:              return NativeAGLUseFont(r3, r4, r5, r6, r7, r8, r9);
		case GL_SUB_AGL_GETERROR:             return NativeAGLGetError();
		case GL_SUB_AGL_ERRORSTRING:          return NativeAGLErrorString(r3);
		case GL_SUB_AGL_RESETLIBRARY:         return NativeAGLResetLibrary();

		// --- GLU (700-753) --- real handlers in gl_engine.cpp
		//
		// CFM/PowerOpen wire format for mixed float/int signatures: every
		// parameter consumes words sequentially (doubles 2, everything
		// else 1); words 0-7 map to r3-r10 and words >=8 to
		// gl_ppc_stack_arg(word-8); float/double VALUES arrive in the
		// FPR capture (float_bits) while their GPR word slots are
		// SKIPPED (undefined in prototyped calls). Post-float ints/
		// pointers therefore land in LATER registers than a dense
		// count suggests — reading the skipped shadow registers yields
		// garbage (the D-5-2 guest-memory-corruption class).
		case GL_SUB_GLU_BEGINCURVE:    NativeGLUBeginCurve(r3); return 0;
		case GL_SUB_GLU_BEGINPOLYGON:  NativeGLUBeginPolygon(r3); return 0;
		case GL_SUB_GLU_BEGINSURFACE:  NativeGLUBeginSurface(r3); return 0;
		case GL_SUB_GLU_BEGINTRIM:     NativeGLUBeginTrim(r3); return 0;
		case GL_SUB_GLU_BUILD1DMIPMAPS:
			return NativeGLUBuild1DMipmaps(gl_current_context, r3, (int32_t)r4, (int32_t)r5, r6, r7, r8);
		case GL_SUB_GLU_BUILD2DMIPMAPS:
			return NativeGLUBuild2DMipmaps(gl_current_context, r3, (int32_t)r4, (int32_t)r5, (int32_t)r6, r7, r8, r9);
		case GL_SUB_GLU_CYLINDER: {
			NativeGLUCylinder(gl_current_context, r3,
			                  double_arg(float_bits, 0),
			                  double_arg(float_bits, 1),
			                  double_arg(float_bits, 2),
			                  /* quad w0=r3; 3 doubles w1-6; slices w7;
			                   * stacks w8 (first stack word) */
			                  (int32_t)r10, (int32_t)gl_ppc_stack_arg(0));
			return 0;
		}
		case GL_SUB_GLU_DELETENURBSRENDERER:      NativeGLUDeleteNurbsRenderer(r3); return 0;
		case GL_SUB_GLU_DELETENURBSTESSELLATOREXT: NativeGLUDeleteNurbsTessellatorEXT(r3); return 0;
		case GL_SUB_GLU_DELETEQUADRIC:             NativeGLUDeleteQuadric(r3); return 0;
		case GL_SUB_GLU_DELETETESS:                NativeGLUDeleteTess(r3); return 0;
		case GL_SUB_GLU_DISK: {
			NativeGLUDisk(gl_current_context, r3,
			              double_arg(float_bits, 0),
			              double_arg(float_bits, 1),
			              /* quad w0=r3; 2 doubles w1-4; slices w5; loops w6 */
			              (int32_t)r8, (int32_t)r9);
			return 0;
		}
		case GL_SUB_GLU_ENDCURVE:     NativeGLUEndCurve(r3); return 0;
		case GL_SUB_GLU_ENDPOLYGON:   NativeGLUEndPolygon(r3); return 0;
		case GL_SUB_GLU_ENDSURFACE:   NativeGLUEndSurface(r3); return 0;
		case GL_SUB_GLU_ENDTRIM:      NativeGLUEndTrim(r3); return 0;
		case GL_SUB_GLU_ERRORSTRING:  return NativeGLUErrorString(r3);
		case GL_SUB_GLU_GETNURBSPROPERTY: NativeGLUGetNurbsProperty(r3, r4, r5); return 0;
		case GL_SUB_GLU_GETSTRING:    return NativeGLUGetString(r3);
		case GL_SUB_GLU_GETTESSPROPERTY: NativeGLUGetTessProperty(r3, r4, r5); return 0;
		case GL_SUB_GLU_LOADSAMPLINGMATRICES: NativeGLULoadSamplingMatrices(r3, r4, r5, r6); return 0;
		case GL_SUB_GLU_LOOKAT: {
			double args[9] = {0};
			for (int i = 0; i < 9 && i < num_float_args; i++) {
				args[i] = double_arg(float_bits, i);
			}
			NativeGLULookAt(gl_current_context, args[0], args[1], args[2],
			                args[3], args[4], args[5], args[6], args[7], args[8]);
			return 0;
		}
		case GL_SUB_GLU_NEWNURBSRENDERER:      return NativeGLUNewNurbsRenderer();
		case GL_SUB_GLU_NEWNURBSTESSELLATOREXT: return NativeGLUNewNurbsTessellatorEXT();
		case GL_SUB_GLU_NEWQUADRIC:             return NativeGLUNewQuadric();
		case GL_SUB_GLU_NEWTESS:                return NativeGLUNewTess();
		case GL_SUB_GLU_NEXTCONTOUR:   NativeGLUNextContour(r3, r4); return 0;
		case GL_SUB_GLU_NURBSCALLBACK: NativeGLUNurbsCallback(r3, r4, r5); return 0;
		case GL_SUB_GLU_NURBSCALLBACKDATAEXT: NativeGLUNurbsCallbackDataEXT(r3, r4); return 0;
		case GL_SUB_GLU_NURBSCURVE:    NativeGLUNurbsCurve(r3, (int32_t)r4, r5, (int32_t)r6, r7, (int32_t)r8, r9); return 0;
		case GL_SUB_GLU_NURBSPROPERTY: NativeGLUNurbsProperty(r3, r4, float_arg(float_bits, 0)); return 0;
		case GL_SUB_GLU_NURBSSURFACE:  NativeGLUNurbsSurface(r3, (int32_t)r4, r5, (int32_t)r6, r7, (int32_t)r8, (int32_t)r9, r10, gl_ppc_stack_arg(0), gl_ppc_stack_arg(1), gl_ppc_stack_arg(2)); return 0;
		case GL_SUB_GLU_ORTHO2D:
			NativeGLUOrtho2D(gl_current_context,
				(double)float_arg(float_bits, 0), (double)float_arg(float_bits, 1),
				(double)float_arg(float_bits, 2), (double)float_arg(float_bits, 3));
			return 0;
		case GL_SUB_GLU_PARTIALDISK: {
			NativeGLUPartialDisk(gl_current_context, r3,
				(double)float_arg(float_bits, 0), (double)float_arg(float_bits, 1),
				/* quad w0=r3; inner/outer w1-4; slices w5; loops w6 */
				(int32_t)r8, (int32_t)r9,
				(double)float_arg(float_bits, 2), (double)float_arg(float_bits, 3));
			return 0;
		}
		case GL_SUB_GLU_PERSPECTIVE:
			NativeGLUPerspective(gl_current_context,
				(double)float_arg(float_bits, 0), (double)float_arg(float_bits, 1),
				(double)float_arg(float_bits, 2), (double)float_arg(float_bits, 3));
			return 0;
		case GL_SUB_GLU_PICKMATRIX: {
			NativeGLUPickMatrix(gl_current_context,
			                    double_arg(float_bits, 0),
			                    double_arg(float_bits, 1),
			                    double_arg(float_bits, 2),
			                    double_arg(float_bits, 3),
			                    /* 4 doubles w0-7; viewport ptr w8 */
			                    gl_ppc_stack_arg(0));
			return 0;
		}
		case GL_SUB_GLU_PROJECT: {
			/* 3 doubles w0-5; model w6=r9, proj w7=r10, viewport w8,
			 * winx/winy/winz out-pointers w9-11. The old r3-r8 read the
			 * doubles' skipped shadows — WriteMacDouble through those
			 * garbage registers corrupted guest memory. */
			return NativeGLUProject(gl_current_context,
			                        double_arg(float_bits, 0),
			                        double_arg(float_bits, 1),
			                        double_arg(float_bits, 2),
			                        r9, r10, gl_ppc_stack_arg(0),
			                        gl_ppc_stack_arg(1), gl_ppc_stack_arg(2),
			                        gl_ppc_stack_arg(3));
		}
		case GL_SUB_GLU_PWLCURVE:        NativeGLUPwlCurve(r3, (int32_t)r4, r5, (int32_t)r6, r7); return 0;
		case GL_SUB_GLU_QUADRICCALLBACK:  NativeGLUQuadricCallback(r3, r4, r5); return 0;
		case GL_SUB_GLU_QUADRICDRAWSTYLE: NativeGLUQuadricDrawStyle(r3, r4); return 0;
		case GL_SUB_GLU_QUADRICNORMALS:   NativeGLUQuadricNormals(r3, r4); return 0;
		case GL_SUB_GLU_QUADRICORIENTATION: NativeGLUQuadricOrientation(r3, r4); return 0;
		case GL_SUB_GLU_QUADRICTEXTURE:   NativeGLUQuadricTexture(r3, r4); return 0;
		case GL_SUB_GLU_SCALEIMAGE:
			return NativeGLUScaleImage(gl_current_context, r3, (int32_t)r4, (int32_t)r5, r6, r7, (int32_t)r8, (int32_t)r9, r10, gl_ppc_stack_arg(0));
		case GL_SUB_GLU_SPHERE: {
			NativeGLUSphere(gl_current_context, r3,
			                double_arg(float_bits, 0),
			                /* quad w0=r3; radius w1-2; slices w3; stacks w4 */
			                (int32_t)r6, (int32_t)r7);
			return 0;
		}
		case GL_SUB_GLU_TESSBEGINCONTOUR: NativeGLUTessBeginContour(r3); return 0;
		case GL_SUB_GLU_TESSBEGINPOLYGON: NativeGLUTessBeginPolygon(r3, r4); return 0;
		case GL_SUB_GLU_TESSCALLBACK:     NativeGLUTessCallback(r3, r4, r5); return 0;
		case GL_SUB_GLU_TESSENDCONTOUR:   NativeGLUTessEndContour(r3); return 0;
		case GL_SUB_GLU_TESSENDPOLYGON:   NativeGLUTessEndPolygon(r3); return 0;
		case GL_SUB_GLU_TESSNORMAL: {
			NativeGLUTessNormal(r3,
			                    double_arg(float_bits, 0),
			                    double_arg(float_bits, 1),
			                    double_arg(float_bits, 2));
			return 0;
		}
		case GL_SUB_GLU_TESSPROPERTY: {
			NativeGLUTessProperty(r3, r4, double_arg(float_bits, 0));
			return 0;
		}
		case GL_SUB_GLU_TESSVERTEX:  NativeGLUTessVertex(r3, r4, r5); return 0;
		case GL_SUB_GLU_UNPROJECT: {
			/* Same layout as gluProject above. */
			return NativeGLUUnProject(gl_current_context,
			                          double_arg(float_bits, 0),
			                          double_arg(float_bits, 1),
			                          double_arg(float_bits, 2),
			                          r9, r10, gl_ppc_stack_arg(0),
			                          gl_ppc_stack_arg(1), gl_ppc_stack_arg(2),
			                          gl_ppc_stack_arg(3));
		}

		// --- GLUT (800-915) --- real handlers in gl_engine.cpp
		case GL_SUB_GLUT_INITMAC:           NativeGLUTInitMac(r3, r4); return 0;
		case GL_SUB_GLUT_INITDISPLAYMODE:   NativeGLUTInitDisplayMode(r3); return 0;
		case GL_SUB_GLUT_INITDISPLAYSTRING: NativeGLUTInitDisplayString(r3); return 0;
		case GL_SUB_GLUT_INITWINDOWPOSITION: NativeGLUTInitWindowPosition((int32_t)r3, (int32_t)r4); return 0;
		case GL_SUB_GLUT_INITWINDOWSIZE:    NativeGLUTInitWindowSize((int32_t)r3, (int32_t)r4); return 0;
		case GL_SUB_GLUT_MAINLOOP:          NativeGLUTMainLoop(); return 0;
		case GL_SUB_GLUT_CREATEWINDOW:      return NativeGLUTCreateWindow(r3);
		case GL_SUB_GLUT_CREATEPLAINWINDOW: return NativeGLUTCreateWindow(r3);  // same as CreateWindow
		case GL_SUB_GLUT_CREATESUBWINDOW:   return NativeGLUTCreateSubWindow((int32_t)r3, (int32_t)r4, (int32_t)r5, (int32_t)r6, (int32_t)r7);
		case GL_SUB_GLUT_DESTROYWINDOW:     NativeGLUTDestroyWindow((int32_t)r3); return 0;
		case GL_SUB_GLUT_POSTREDISPLAY:     NativeGLUTPostRedisplay(); return 0;
		case GL_SUB_GLUT_POSTWINDOWREDISPLAY: NativeGLUTPostWindowRedisplay((int32_t)r3); return 0;
		case GL_SUB_GLUT_SWAPBUFFERS:       NativeGLUTSwapBuffers(); return 0;
		case GL_SUB_GLUT_GETWINDOW:         return NativeGLUTGetWindow();
		case GL_SUB_GLUT_SETWINDOW:         NativeGLUTSetWindow((int32_t)r3); return 0;
		case GL_SUB_GLUT_SETWINDOWTITLE:    NativeGLUTSetWindowTitle(r3); return 0;
		case GL_SUB_GLUT_SETICONTITLE:      NativeGLUTSetIconTitle(r3); return 0;
		case GL_SUB_GLUT_POSITIONWINDOW:    NativeGLUTPositionWindow((int32_t)r3, (int32_t)r4); return 0;
		case GL_SUB_GLUT_RESHAPEWINDOW:     NativeGLUTReshapeWindow((int32_t)r3, (int32_t)r4); return 0;
		case GL_SUB_GLUT_POPWINDOW:         NativeGLUTPopWindow(); return 0;
		case GL_SUB_GLUT_PUSHWINDOW:        NativeGLUTPushWindow(); return 0;
		case GL_SUB_GLUT_ICONIFYWINDOW:     NativeGLUTIconifyWindow(); return 0;
		case GL_SUB_GLUT_SHOWWINDOW:        NativeGLUTShowWindow(); return 0;
		case GL_SUB_GLUT_HIDEWINDOW:        NativeGLUTHideWindow(); return 0;
		case GL_SUB_GLUT_FULLSCREEN:        NativeGLUTFullScreen(); return 0;
		case GL_SUB_GLUT_SETCURSOR:         NativeGLUTSetCursor((int32_t)r3); return 0;
		case GL_SUB_GLUT_WARPPOINTER:       NativeGLUTWarpPointer((int32_t)r3, (int32_t)r4); return 0;
		case GL_SUB_GLUT_ESTABLISHOVERLAY:  NativeGLUTEstablishOverlay(); return 0;
		case GL_SUB_GLUT_REMOVEOVERLAY:     NativeGLUTRemoveOverlay(); return 0;
		case GL_SUB_GLUT_USELAYER:          NativeGLUTUseLayer(r3); return 0;
		case GL_SUB_GLUT_POSTOVERLAYREDISPLAY: NativeGLUTPostOverlayRedisplay(); return 0;
		case GL_SUB_GLUT_POSTWINDOWOVERLAYREDISPLAY: NativeGLUTPostWindowOverlayRedisplay((int32_t)r3); return 0;
		case GL_SUB_GLUT_SHOWOVERLAY:       NativeGLUTShowOverlay(); return 0;
		case GL_SUB_GLUT_HIDEOVERLAY:       NativeGLUTHideOverlay(); return 0;
		case GL_SUB_GLUT_CREATEMENU:        return NativeGLUTCreateMenu(r3);
		case GL_SUB_GLUT_DESTROYMENU:       NativeGLUTDestroyMenu((int32_t)r3); return 0;
		case GL_SUB_GLUT_GETMENU:           return NativeGLUTGetMenu();
		case GL_SUB_GLUT_SETMENU:           NativeGLUTSetMenu((int32_t)r3); return 0;
		case GL_SUB_GLUT_ADDMENUENTRY:      NativeGLUTAddMenuEntry(r3, (int32_t)r4); return 0;
		case GL_SUB_GLUT_ADDSUBMENU:        NativeGLUTAddSubMenu(r3, (int32_t)r4); return 0;
		case GL_SUB_GLUT_CHANGETOMENUENTRY: NativeGLUTChangeToMenuEntry((int32_t)r3, r4, (int32_t)r5); return 0;
		case GL_SUB_GLUT_CHANGETOSUBMENU:   NativeGLUTChangeToSubMenu((int32_t)r3, r4, (int32_t)r5); return 0;
		case GL_SUB_GLUT_REMOVEMENUITEM:    NativeGLUTRemoveMenuItem((int32_t)r3); return 0;
		case GL_SUB_GLUT_ATTACHMENU:        NativeGLUTAttachMenu((int32_t)r3); return 0;
		case GL_SUB_GLUT_ATTACHMENUNAME:    NativeGLUTAttachMenuName((int32_t)r3, r4); return 0;
		case GL_SUB_GLUT_DETACHMENU:        NativeGLUTDetachMenu((int32_t)r3); return 0;
		case GL_SUB_GLUT_DISPLAYFUNC:       NativeGLUTDisplayFunc(r3); return 0;
		case GL_SUB_GLUT_RESHAPEFUNC:       NativeGLUTReshapeFunc(r3); return 0;
		case GL_SUB_GLUT_KEYBOARDFUNC:      NativeGLUTKeyboardFunc(r3); return 0;
		case GL_SUB_GLUT_MOUSEFUNC:         NativeGLUTMouseFunc(r3); return 0;
		case GL_SUB_GLUT_MOTIONFUNC:        NativeGLUTMotionFunc(r3); return 0;
		case GL_SUB_GLUT_PASSIVEMOTIONFUNC: NativeGLUTPassiveMotionFunc(r3); return 0;
		case GL_SUB_GLUT_ENTRYFUNC:         NativeGLUTEntryFunc(r3); return 0;
		case GL_SUB_GLUT_VISIBILITYFUNC:    NativeGLUTVisibilityFunc(r3); return 0;
		case GL_SUB_GLUT_IDLEFUNC:          NativeGLUTIdleFunc(r3); return 0;
		case GL_SUB_GLUT_TIMERFUNC:         NativeGLUTTimerFunc(r3, r4, (int32_t)r5); return 0;
		case GL_SUB_GLUT_MENUSTATEFUNC:     NativeGLUTMenuStateFunc(r3); return 0;
		case GL_SUB_GLUT_SPECIALFUNC:       NativeGLUTSpecialFunc(r3); return 0;
		case GL_SUB_GLUT_SPACEBALLMOTIONFUNC: NativeGLUTSpaceballMotionFunc(r3); return 0;
		case GL_SUB_GLUT_SPACEBALLROTATEFUNC: NativeGLUTSpaceballRotateFunc(r3); return 0;
		case GL_SUB_GLUT_SPACEBALLBUTTONFUNC: NativeGLUTSpaceballButtonFunc(r3); return 0;
		case GL_SUB_GLUT_BUTTONBOXFUNC:     NativeGLUTButtonBoxFunc(r3); return 0;
		case GL_SUB_GLUT_DIALSFUNC:         NativeGLUTDialsFunc(r3); return 0;
		case GL_SUB_GLUT_TABLETMOTIONFUNC:  NativeGLUTTabletMotionFunc(r3); return 0;
		case GL_SUB_GLUT_TABLETBUTTONFUNC:  NativeGLUTTabletButtonFunc(r3); return 0;
		case GL_SUB_GLUT_MENUSTATUSFUNC:    NativeGLUTMenuStatusFunc(r3); return 0;
		case GL_SUB_GLUT_OVERLAYDISPLAYFUNC: NativeGLUTOverlayDisplayFunc(r3); return 0;
		case GL_SUB_GLUT_WINDOWSTATUSFUNC:  NativeGLUTWindowStatusFunc(r3); return 0;
		case GL_SUB_GLUT_KEYBOARDUPFUNC:    NativeGLUTKeyboardUpFunc(r3); return 0;
		case GL_SUB_GLUT_SPECIALUPFUNC:     NativeGLUTSpecialUpFunc(r3); return 0;
		case GL_SUB_GLUT_JOYSTICKFUNC:      NativeGLUTJoystickFunc(r3, (int32_t)r4); return 0;
		case GL_SUB_GLUT_SETCOLOR:
			NativeGLUTSetColor((int32_t)r3, float_arg(float_bits, 0), float_arg(float_bits, 1), float_arg(float_bits, 2));
			return 0;
		case GL_SUB_GLUT_GETCOLOR: {
			float c = NativeGLUTGetColor((int32_t)r3, (int32_t)r4);
			uint32_t ret; memcpy(&ret, &c, 4); return ret;
		}
		case GL_SUB_GLUT_COPYCOLORMAP:      NativeGLUTCopyColormap((int32_t)r3); return 0;
		case GL_SUB_GLUT_GET:               return (uint32_t)NativeGLUTGet(r3);
		case GL_SUB_GLUT_DEVICEGET:         return (uint32_t)NativeGLUTDeviceGet(r3);
		case GL_SUB_GLUT_EXTENSIONSUPPORTED: return (uint32_t)NativeGLUTExtensionSupported(r3);
		case GL_SUB_GLUT_GETMODIFIERS:      return (uint32_t)NativeGLUTGetModifiers();
		case GL_SUB_GLUT_LAYERGET:          return (uint32_t)NativeGLUTLayerGet(r3);
		case GL_SUB_GLUT_BITMAPCHARACTER:   NativeGLUTBitmapCharacter(gl_current_context, r3, (int32_t)r4); return 0;
		case GL_SUB_GLUT_BITMAPWIDTH:       return (uint32_t)NativeGLUTBitmapWidth(r3, (int32_t)r4);
		case GL_SUB_GLUT_STROKECHARACTER:   NativeGLUTStrokeCharacter(gl_current_context, r3, (int32_t)r4); return 0;
		case GL_SUB_GLUT_STROKEWIDTH:       return (uint32_t)NativeGLUTStrokeWidth(r3, (int32_t)r4);
		case GL_SUB_GLUT_BITMAPLENGTH:      return (uint32_t)NativeGLUTBitmapLength(r3, r4);
		case GL_SUB_GLUT_STROKELENGTH:      return (uint32_t)NativeGLUTStrokeLength(r3, r4);
		case GL_SUB_GLUT_WIRESPHERE: {
			/* radius w0-1; slices w2=r5, stacks w3=r6 */
			NativeGLUTWireSphere(gl_current_context, double_arg(float_bits, 0),
			                     (int32_t)r5, (int32_t)r6); return 0;
		}
		case GL_SUB_GLUT_SOLIDSPHERE: {
			/* radius w0-1; slices w2=r5, stacks w3=r6 */
			NativeGLUTSolidSphere(gl_current_context, double_arg(float_bits, 0),
			                      (int32_t)r5, (int32_t)r6); return 0;
		}
		case GL_SUB_GLUT_WIRECONE: {
			/* base/height w0-3; slices w4=r7, stacks w5=r8 */
			NativeGLUTWireCone(gl_current_context,
			                   double_arg(float_bits, 0),
			                   double_arg(float_bits, 1),
			                   (int32_t)r7, (int32_t)r8); return 0;
		}
		case GL_SUB_GLUT_SOLIDCONE: {
			/* base/height w0-3; slices w4=r7, stacks w5=r8 */
			NativeGLUTSolidCone(gl_current_context,
			                    double_arg(float_bits, 0),
			                    double_arg(float_bits, 1),
			                    (int32_t)r7, (int32_t)r8); return 0;
		}
		case GL_SUB_GLUT_WIRECUBE: {
			NativeGLUTWireCube(gl_current_context, double_arg(float_bits, 0)); return 0;
		}
		case GL_SUB_GLUT_SOLIDCUBE: {
			NativeGLUTSolidCube(gl_current_context, double_arg(float_bits, 0)); return 0;
		}
		case GL_SUB_GLUT_WIRETORUS: {
			/* inner/outer w0-3; nsides w4=r7, rings w5=r8 */
			NativeGLUTWireTorus(gl_current_context,
			                    double_arg(float_bits, 0),
			                    double_arg(float_bits, 1),
			                    (int32_t)r7, (int32_t)r8); return 0;
		}
		case GL_SUB_GLUT_SOLIDTORUS: {
			/* inner/outer w0-3; nsides w4=r7, rings w5=r8 */
			NativeGLUTSolidTorus(gl_current_context,
			                     double_arg(float_bits, 0),
			                     double_arg(float_bits, 1),
			                     (int32_t)r7, (int32_t)r8); return 0;
		}
		case GL_SUB_GLUT_WIREDODECAHEDRON:  NativeGLUTWireDodecahedron(gl_current_context); return 0;
		case GL_SUB_GLUT_SOLIDDODECAHEDRON: NativeGLUTSolidDodecahedron(gl_current_context); return 0;
		case GL_SUB_GLUT_WIRETEAPOT: {
			NativeGLUTWireTeapot(gl_current_context, double_arg(float_bits, 0)); return 0;
		}
		case GL_SUB_GLUT_SOLIDTEAPOT: {
			NativeGLUTSolidTeapot(gl_current_context, double_arg(float_bits, 0)); return 0;
		}
		case GL_SUB_GLUT_WIREOCTAHEDRON:    NativeGLUTWireOctahedron(gl_current_context); return 0;
		case GL_SUB_GLUT_SOLIDOCTAHEDRON:   NativeGLUTSolidOctahedron(gl_current_context); return 0;
		case GL_SUB_GLUT_WIRETETRAHEDRON:   NativeGLUTWireTetrahedron(gl_current_context); return 0;
		case GL_SUB_GLUT_SOLIDTETRAHEDRON:  NativeGLUTSolidTetrahedron(gl_current_context); return 0;
		case GL_SUB_GLUT_WIREICOSAHEDRON:   NativeGLUTWireIcosahedron(gl_current_context); return 0;
		case GL_SUB_GLUT_SOLIDICOSAHEDRON:  NativeGLUTSolidIcosahedron(gl_current_context); return 0;
		case GL_SUB_GLUT_VIDEORESIZEGET:    return (uint32_t)NativeGLUTVideoResizeGet(r3);
		case GL_SUB_GLUT_SETUPVIDEORESIZING: NativeGLUTSetupVideoResizing(); return 0;
		case GL_SUB_GLUT_STOPVIDEORESIZING: NativeGLUTStopVideoResizing(); return 0;
		case GL_SUB_GLUT_VIDEORESIZE:       NativeGLUTVideoResize((int32_t)r3, (int32_t)r4, (int32_t)r5, (int32_t)r6); return 0;
		case GL_SUB_GLUT_VIDEOPAN:          NativeGLUTVideoPan((int32_t)r3, (int32_t)r4, (int32_t)r5, (int32_t)r6); return 0;
		case GL_SUB_GLUT_REPORTERRORS:      NativeGLUTReportErrors(); return 0;
		case GL_SUB_GLUT_IGNOREKEYREPEAT:   NativeGLUTIgnoreKeyRepeat((int32_t)r3); return 0;
		case GL_SUB_GLUT_SETKEYREPEAT:      NativeGLUTSetKeyRepeat((int32_t)r3); return 0;
		case GL_SUB_GLUT_FORCEJOYSTICKFUNC: NativeGLUTForceJoystickFunc(); return 0;
		case GL_SUB_GLUT_GAMEMODESTRING:    NativeGLUTGameModeString(r3); return 0;
		case GL_SUB_GLUT_ENTERGAMEMODE:     return (uint32_t)NativeGLUTEnterGameMode();
		case GL_SUB_GLUT_LEAVEGAMEMODE:     NativeGLUTLeaveGameMode(); return 0;
		case GL_SUB_GLUT_GAMEMODEGET:       return (uint32_t)NativeGLUTGameModeGet(r3);

	default:
		GL_LOG("unknown sub-opcode %d", sub_opcode);
		return 0;
	}
}
