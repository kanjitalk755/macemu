/*
 *  gl_ext.h - Minimal OpenGL extension entry points for FBO on desktop
 */
#ifndef GFXACCEL_GL_EXT_H
#define GFXACCEL_GL_EXT_H

#include <SDL.h>
#include <SDL_opengl.h>

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
typedef void (APIENTRY *PFNGLFOGCOORDFPROC)(GLfloat);

#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_COMBINE
#define GL_COMBINE 0x8570
#define GL_COMBINE_RGB 0x8571
#define GL_SOURCE0_RGB 0x8580
#define GL_SOURCE1_RGB 0x8581
#define GL_OPERAND0_RGB 0x8590
#define GL_OPERAND1_RGB 0x8591
#define GL_PRIMARY_COLOR 0x8577
#define GL_PREVIOUS 0x8578
#define GL_INTERPOLATE 0x8575
#define GL_CONSTANT 0x8576
#define GL_RGB_SCALE 0x8573
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
	PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = nullptr;
	PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = nullptr;
	PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = nullptr;
	PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D = nullptr;
	PFNGLGENRENDERBUFFERSPROC GenRenderbuffers = nullptr;
	PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers = nullptr;
	PFNGLBINDRENDERBUFFERPROC BindRenderbuffer = nullptr;
	PFNGLRENDERBUFFERSTORAGEPROC RenderbufferStorage = nullptr;
	PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer = nullptr;
	PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus = nullptr;
	PFNGLGENERATEMIPMAPPROC GenerateMipmap = nullptr;
	PFNGLACTIVETEXTUREPROC ActiveTexture = nullptr;
	PFNGLCLIENTACTIVETEXTUREPROC ClientActiveTexture = nullptr;
	PFNGLMULTITEXCOORD2FPROC MultiTexCoord2f = nullptr;
	PFNGLMULTITEXCOORD4FPROC MultiTexCoord4f = nullptr;
	PFNGLSECONDARYCOLOR3FPROC SecondaryColor3f = nullptr;
	PFNGLBLENDFUNCSEPARATEPROC BlendFuncSeparate = nullptr;
	PFNGLFOGCOORDFPROC FogCoordf = nullptr;
	bool fbo = false;
	bool multitex = false;
};

inline GfxGLExt &gfx_gl_ext()
{
	static GfxGLExt e;
	static bool loaded = false;
	if (!loaded) {
		loaded = true;
		e.GenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)SDL_GL_GetProcAddress("glGenFramebuffers");
		if (!e.GenFramebuffers)
			e.GenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)SDL_GL_GetProcAddress("glGenFramebuffersEXT");
		e.DeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)SDL_GL_GetProcAddress("glDeleteFramebuffers");
		if (!e.DeleteFramebuffers)
			e.DeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)SDL_GL_GetProcAddress("glDeleteFramebuffersEXT");
		e.BindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)SDL_GL_GetProcAddress("glBindFramebuffer");
		if (!e.BindFramebuffer)
			e.BindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)SDL_GL_GetProcAddress("glBindFramebufferEXT");
		e.FramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)SDL_GL_GetProcAddress("glFramebufferTexture2D");
		if (!e.FramebufferTexture2D)
			e.FramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)SDL_GL_GetProcAddress("glFramebufferTexture2DEXT");
		e.GenRenderbuffers = (PFNGLGENRENDERBUFFERSPROC)SDL_GL_GetProcAddress("glGenRenderbuffers");
		if (!e.GenRenderbuffers)
			e.GenRenderbuffers = (PFNGLGENRENDERBUFFERSPROC)SDL_GL_GetProcAddress("glGenRenderbuffersEXT");
		e.DeleteRenderbuffers = (PFNGLDELETERENDERBUFFERSPROC)SDL_GL_GetProcAddress("glDeleteRenderbuffers");
		if (!e.DeleteRenderbuffers)
			e.DeleteRenderbuffers = (PFNGLDELETERENDERBUFFERSPROC)SDL_GL_GetProcAddress("glDeleteRenderbuffersEXT");
		e.BindRenderbuffer = (PFNGLBINDRENDERBUFFERPROC)SDL_GL_GetProcAddress("glBindRenderbuffer");
		if (!e.BindRenderbuffer)
			e.BindRenderbuffer = (PFNGLBINDRENDERBUFFERPROC)SDL_GL_GetProcAddress("glBindRenderbufferEXT");
		e.RenderbufferStorage = (PFNGLRENDERBUFFERSTORAGEPROC)SDL_GL_GetProcAddress("glRenderbufferStorage");
		if (!e.RenderbufferStorage)
			e.RenderbufferStorage = (PFNGLRENDERBUFFERSTORAGEPROC)SDL_GL_GetProcAddress("glRenderbufferStorageEXT");
		e.FramebufferRenderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
		if (!e.FramebufferRenderbuffer)
			e.FramebufferRenderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)SDL_GL_GetProcAddress("glFramebufferRenderbufferEXT");
		e.CheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
		if (!e.CheckFramebufferStatus)
			e.CheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)SDL_GL_GetProcAddress("glCheckFramebufferStatusEXT");
		e.GenerateMipmap = (PFNGLGENERATEMIPMAPPROC)SDL_GL_GetProcAddress("glGenerateMipmap");
		if (!e.GenerateMipmap)
			e.GenerateMipmap = (PFNGLGENERATEMIPMAPPROC)SDL_GL_GetProcAddress("glGenerateMipmapEXT");
		e.ActiveTexture = (PFNGLACTIVETEXTUREPROC)SDL_GL_GetProcAddress("glActiveTexture");
		if (!e.ActiveTexture)
			e.ActiveTexture = (PFNGLACTIVETEXTUREPROC)SDL_GL_GetProcAddress("glActiveTextureARB");
		e.ClientActiveTexture = (PFNGLCLIENTACTIVETEXTUREPROC)SDL_GL_GetProcAddress("glClientActiveTexture");
		if (!e.ClientActiveTexture)
			e.ClientActiveTexture = (PFNGLCLIENTACTIVETEXTUREPROC)SDL_GL_GetProcAddress("glClientActiveTextureARB");
		e.MultiTexCoord2f = (PFNGLMULTITEXCOORD2FPROC)SDL_GL_GetProcAddress("glMultiTexCoord2f");
		if (!e.MultiTexCoord2f)
			e.MultiTexCoord2f = (PFNGLMULTITEXCOORD2FPROC)SDL_GL_GetProcAddress("glMultiTexCoord2fARB");
		e.MultiTexCoord4f = (PFNGLMULTITEXCOORD4FPROC)SDL_GL_GetProcAddress("glMultiTexCoord4f");
		if (!e.MultiTexCoord4f)
			e.MultiTexCoord4f = (PFNGLMULTITEXCOORD4FPROC)SDL_GL_GetProcAddress("glMultiTexCoord4fARB");
		e.SecondaryColor3f = (PFNGLSECONDARYCOLOR3FPROC)SDL_GL_GetProcAddress("glSecondaryColor3f");
		if (!e.SecondaryColor3f)
			e.SecondaryColor3f = (PFNGLSECONDARYCOLOR3FPROC)SDL_GL_GetProcAddress("glSecondaryColor3fEXT");
		e.BlendFuncSeparate = (PFNGLBLENDFUNCSEPARATEPROC)SDL_GL_GetProcAddress("glBlendFuncSeparate");
		if (!e.BlendFuncSeparate)
			e.BlendFuncSeparate = (PFNGLBLENDFUNCSEPARATEPROC)SDL_GL_GetProcAddress("glBlendFuncSeparateEXT");
		e.FogCoordf = (PFNGLFOGCOORDFPROC)SDL_GL_GetProcAddress("glFogCoordf");
		if (!e.FogCoordf)
			e.FogCoordf = (PFNGLFOGCOORDFPROC)SDL_GL_GetProcAddress("glFogCoordfEXT");
		e.fbo = e.GenFramebuffers && e.BindFramebuffer && e.FramebufferTexture2D &&
		        e.GenRenderbuffers && e.BindRenderbuffer && e.RenderbufferStorage &&
		        e.FramebufferRenderbuffer && e.CheckFramebufferStatus && e.DeleteFramebuffers;
		e.multitex = e.ActiveTexture != nullptr &&
		             (e.MultiTexCoord2f != nullptr || e.MultiTexCoord4f != nullptr);
	}
	return e;
}

#endif
