/*
 *  gl_ext.h - Minimal OpenGL extension entry points for FBO on desktop
 *
 *	(C) 2026 RandoOnSteam (battlemageloveryt@gmail.com)
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
#ifndef GFXACCEL_GL_EXT_H
#define GFXACCEL_GL_EXT_H

#include <SDL.h>
#include <SDL_opengl.h>

void *GfxGLGetProcAddress(const char *name);

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_RGBA8 0x8058
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_BGRA 0x80E1
#endif

typedef void (APIENTRY *PFNGLGENFRAMEBUFFERSPROC)(GLsizei, GLuint *);
typedef void (APIENTRY *PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFNGLBINDFRAMEBUFFERPROC)(GLenum, GLuint);
typedef void (APIENTRY *PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRY *PFNGLGENRENDERBUFFERSPROC)(GLsizei, GLuint *);
typedef void (APIENTRY *PFNGLDELETERENDERBUFFERSPROC)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFNGLBINDRENDERBUFFERPROC)(GLenum, GLuint);
typedef void (APIENTRY *PFNGLRENDERBUFFERSTORAGEPROC)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *PFNGLFRAMEBUFFERRENDERBUFFERPROC)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (APIENTRY *PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum);
typedef void (APIENTRY *PFNGLGENERATEMIPMAPPROC)(GLenum);
typedef void (APIENTRY *PFNGLACTIVETEXTUREPROC)(GLenum);
typedef void (APIENTRY *PFNGLCLIENTACTIVETEXTUREPROC)(GLenum);
typedef void (APIENTRY *PFNGLMULTITEXCOORD2FPROC)(GLenum, GLfloat, GLfloat);
typedef void (APIENTRY *PFNGLMULTITEXCOORD4FPROC)(GLenum, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFNGLSECONDARYCOLOR3FPROC)(GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFNGLBLENDFUNCSEPARATEPROC)(GLenum, GLenum, GLenum, GLenum);
typedef void (APIENTRY *GFXPFNGLBLENDCOLORPROC)(GLclampf, GLclampf, GLclampf, GLclampf);
typedef void (APIENTRY *GFXPFNGLBLENDEQUATIONPROC)(GLenum);
typedef void (APIENTRY *PFNGLFOGCOORDFPROC)(GLfloat);
typedef void (APIENTRY *GFXPFNGLBLITFRAMEBUFFERPROC)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void (APIENTRY *GFXPFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (APIENTRY *GFXPFNGLGENPROGRAMSPROC)(GLsizei, GLuint *);
typedef void (APIENTRY *GFXPFNGLBINDPROGRAMPROC)(GLenum, GLuint);
typedef void (APIENTRY *GFXPFNGLPROGRAMSTRINGPROC)(GLenum, GLenum, GLsizei, const void *);
#ifndef GL_FRAGMENT_PROGRAM_ARB
#define GL_FRAGMENT_PROGRAM_ARB 0x8804
#define GL_PROGRAM_FORMAT_ASCII_ARB 0x8875
#define GL_PROGRAM_ERROR_POSITION_ARB 0x864B
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_STENCIL_ATTACHMENT
#define GL_STENCIL_ATTACHMENT 0x8D20
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES 0x8D57
#endif

#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_COMBINE
#define GL_COMBINE 0x8570
#define GL_COMBINE_RGB 0x8571
#define GL_COMBINE_ALPHA 0x8572
#define GL_SOURCE0_RGB 0x8580
#define GL_SOURCE1_RGB 0x8581
#define GL_SOURCE2_RGB 0x8582
#define GL_SOURCE0_ALPHA 0x8588
#define GL_SOURCE1_ALPHA 0x8589
#define GL_OPERAND0_RGB 0x8590
#define GL_OPERAND1_RGB 0x8591
#define GL_OPERAND2_RGB 0x8592
#define GL_OPERAND0_ALPHA 0x8598
#define GL_OPERAND1_ALPHA 0x8599
#define GL_PRIMARY_COLOR 0x8577
#define GL_PREVIOUS 0x8578
#define GL_INTERPOLATE 0x8575
#define GL_CONSTANT 0x8576
#define GL_RGB_SCALE 0x8573
#endif
#ifndef GL_SOURCE2_ALPHA
#define GL_SOURCE2_ALPHA 0x858A
#define GL_OPERAND2_ALPHA 0x859A
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
#ifndef GL_COLOR_SUM
#define GL_COLOR_SUM 0x8458
#endif
#ifndef GL_FOG_COORDINATE_SOURCE
#define GL_FOG_COORDINATE_SOURCE 0x8450
#define GL_FOG_COORDINATE 0x8451
#define GL_FRAGMENT_DEPTH 0x8452
#endif

struct GfxGLExt {
	/* A plain aggregate: the one instance lives in gfx_gl_ext() below with
	   static storage duration, so every member starts zeroed. */
	PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
	PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers;
	PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
	PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
	PFNGLGENRENDERBUFFERSPROC GenRenderbuffers;
	PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers;
	PFNGLBINDRENDERBUFFERPROC BindRenderbuffer;
	PFNGLRENDERBUFFERSTORAGEPROC RenderbufferStorage;
	PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer;
	PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
	PFNGLGENERATEMIPMAPPROC GenerateMipmap;
	PFNGLACTIVETEXTUREPROC ActiveTexture;
	PFNGLCLIENTACTIVETEXTUREPROC ClientActiveTexture;
	PFNGLMULTITEXCOORD2FPROC MultiTexCoord2f;
	PFNGLMULTITEXCOORD4FPROC MultiTexCoord4f;
	PFNGLSECONDARYCOLOR3FPROC SecondaryColor3f;
	PFNGLBLENDFUNCSEPARATEPROC BlendFuncSeparate;
	GFXPFNGLBLENDCOLORPROC BlendColor;
	GFXPFNGLBLENDEQUATIONPROC BlendEquation;
	PFNGLFOGCOORDFPROC FogCoordf;
	GFXPFNGLBLITFRAMEBUFFERPROC BlitFramebuffer;
	GFXPFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC RenderbufferStorageMultisample;
	GFXPFNGLGENPROGRAMSPROC GenPrograms;
	GFXPFNGLBINDPROGRAMPROC BindProgram;
	GFXPFNGLPROGRAMSTRINGPROC ProgramString;
	bool fbo;
	bool multitex;
};

void GfxGLExtLoad(GfxGLExt &e);

inline GfxGLExt &gfx_gl_ext()
{
	static GfxGLExt e;
	static bool loaded = false;
	if (!loaded) {
		loaded = true;
		GfxGLExtLoad(e);
	}
	return e;
}

#endif
