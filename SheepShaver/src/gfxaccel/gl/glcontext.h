/**
*	@file glcontext.h
*
*	 #####  #        #####  ####### #     # ####### ####### #     # #######
*	#     # #       #     # #     # ##    #    #    #        #   #     #
*	#       #       #       #     # # #   #    #    #         # #      #
*	#  #### #       #       #     # #  #  #    #    #####      #       #
*	#     # #       #       #     # #   # #    #    #         # #      #
*	#     # #       #     # #     # #    ##    #    #        #   #     #
*	 #####  #######  #####  ####### #     #    #    ####### #     #    #
*
*	Single header file library for cross-platform OpenGL initialization.
*
*	Around 2009 OpenGL 3.0 released with version targeting,
*	core/compatibility profiles and attributes for context. For example,
*	Windows supports the newer 3.0 features through
*	wglCreateContextAttribsARB() while keeping wglCreateContext()
*	for compatibility.
*
*	###Instructions###
*
*	Define GLCONTEXT_IMPLEMENTATION in one source file for the implementation
*
*	###Example###
*	GLCONTEXT glc;
*	if(GLContext_Create(&glc, NULL, NULL, NULL))
*	{
*		if(GLContext_SetCurrent(&glc))
*		{
*			glClear(GL_COLOR_BUFFER_BIT);
*			glBegin(GL_TRIANGLES);
*			glColor3f(1.0, 0.0, 0.0);
*			glVertex2f(0.0, 1.0);
*			glColor3f(0.0, 1.0, 0.0);
*			glVertex2f(-1.0, -1.0);
*			glColor3f(0.0, 0.0, 1.0);
*			glVertex2f(1.0, -1.0);
*			glEnd();
*			glFlush();
*			GLContext_SwapBuffers(&glc);
*		}
*		GLContext_Destroy(&glc); // call when done
*	}
*
*	###OPTIONS###
*	- Windows:
*	-- Define GLCONTEXT_STATICGDI32 to not dynload gdi32.dll functions
*	-- Define GLCONTEXT_STATICOPENGL32 to not dynload opengl32.dll functions
*	-- Define GLCONTEXT_NOWINNT31 to remove Windows NT 3.1 support
*
*	###NOTES###
*	- ASCII art, mostly "Banner", from
*	-- budavariam.github.io/asciiart-text/multi
*	- On Windows if you have a non-default DPI and do not call
*	SetProcessDPIAware or set it in your manifest the OpenGL
*	coordinates will not match OSes!
*
*	@version 0.4
*
*	Version History
*
*	0.5		Combined GetNativeProc and GetProc. SetDrawable(NULL) fixes
*
*	0.4		Fix for Qt (drawable getting was incorrect)
*
*	0.3		Fixed a C++ comp error on Unix
*
*	0.2		WSMSIXOPT for MSIX apps.
*				- GLCONTEXT_STATICOPEGL32->GLCONTEXT_STATICOPENGL32
*
*   0.1		Initial
*
*	@copyright ALTERNATIVE A - 0BSD (www.opensource.org/license/0BSD)
*
*	Copyright (c) 2026 RandoOnSteam
*
*	Permission to use, copy, modify, and/or distribute this software for
*	any purpose with or without fee is hereby granted.
*
*	THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
*	WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
*	OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE
*	FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY
*	DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
*	AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
*	OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*
*	@copyright ALTERNATIVE B - Public Domain (www.unlicense.org)
*
*	This is free and unencumbered software released into the public domain.
*
*	Anyone is free to copy, modify, publish, use, compile, sell, or distribute
*	this software, either in source code form or as a compiled binary, for any
*	purpose, commercial or non-commercial, and by any means.
*
*	In jurisdictions that recognize copyright laws, the author or authors of
*	this software dedicate any and all copyright interest in the software to
*	the public domain. We make this dedication for the benefit of the public
*	at large and to the detriment of our heirs and successors. We intend this
*	dedication to be an overt act of relinquishment in perpetuity of all
*	present and future rights to this software under copyright law.
*
*	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*	AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
*	ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
*	CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
#ifndef __GLCONTEXT_H__
#define __GLCONTEXT_H__
#ifdef __cplusplus
	extern "C" {
#endif
/*======================== Forward Definitions ==============================*/
typedef struct GLCONTEXT GLCONTEXT;
#if defined(_WIN32)
	#if defined(__TINYC__)
		#if defined(_WIN64)
			#define __stdcall
		#else
			#define __stdcall __attribute__((__stdcall__))
		#endif
	#endif
	typedef struct HWND__* GLWINDOWHANDLE;
	typedef struct HDC__* GLDRAWABLEHANDLE;
	typedef struct HGLRC__* GLCONTEXTHANDLE;
	typedef struct tagPIXELFORMATDESCRIPTOR tagPIXELFORMATDESCRIPTOR;
	typedef int GLPIXELFORMATHANDLE;
	typedef int attrib_t;
	typedef int (__stdcall* WIN32PROC)();
	typedef int (__stdcall* ChoosePixelFormatProc)(
		struct HDC__*, const tagPIXELFORMATDESCRIPTOR*);
	typedef int  (__stdcall* SetPixelFormatProc)(struct HDC__*, int,
		const tagPIXELFORMATDESCRIPTOR*);
	typedef GLCONTEXTHANDLE (__stdcall* wglGetCurrentContextProc)(void);
	typedef int (__stdcall* SwapBuffersProc)(struct HDC__*);
	typedef GLCONTEXTHANDLE (__stdcall* wglCreateContextProc)(struct HDC__*);
	typedef int (__stdcall* wglDeleteContextProc)(GLCONTEXTHANDLE);
	typedef WIN32PROC (__stdcall* wglGetProcAddressProc)(const char*);
	typedef int (__stdcall* wglMakeCurrentProc)(struct HDC__*,
		GLCONTEXTHANDLE);
	typedef int (__stdcall* wglShareListsProc)(GLCONTEXTHANDLE,
		GLCONTEXTHANDLE);
	typedef int (__stdcall* wglSwapIntervalEXTProc)(int interval);
	typedef int (__stdcall* wglGetSwapIntervalEXTProc)(void);
	#if defined(WSMSIXOPT)
		#if !defined(GLCONTEXT_STATICOPENGL32)
			#define GLCONTEXT_STATICOPENGL32
		#endif
		#if !defined(GLCONTEXT_STATICGDI32)
			#define GLCONTEXT_STATICGDI32
		#endif
	#endif
	#if defined(GLCONTEXT_STATICOPENGL32) && !defined(GLCONTEXT_NOWINNT31)
		#define GLCONTEXT_NOWINNT31
	#endif
	#if defined(GLCONTEXT_STATICGDI32)
		#define GLCONTEXTGDI32CALL(x) x
	#else
		#define GLCONTEXTGDI32CALL(x) (*(pThis)->mpfn##x)
	#endif /* GLCONTEXT_STATICGDI32 */
	#if defined(GLCONTEXT_STATICOPENGL32)
		#define GLCONTEXTOPENGL32CALL(x) x
	#else
		#define GLCONTEXTOPENGL32CALL(x) (*(pThis)->mpfn##x)
	#endif /* GLCONTEXT_STATICOPENGL32 */
	typedef struct GLCONTEXT
	{
		GLCONTEXTHANDLE mhContext;
		GLWINDOWHANDLE mhWnd;
		GLDRAWABLEHANDLE mhDC;
		unsigned short mAtom;
		unsigned short mHasWGL_EXT_swap_control_tear;
		int mPixelFormat;
	#if !defined(GLCONTEXT_STATICGDI32)
		struct HINSTANCE__* mhGDI32;
		ChoosePixelFormatProc mpfnChoosePixelFormat;
		SetPixelFormatProc mpfnSetPixelFormat;
		SwapBuffersProc mpfnSwapBuffers;
	#endif
		struct HINSTANCE__* mhOpenGL32;
	#if !defined(GLCONTEXT_STATICOPENGL32)
		wglGetCurrentContextProc mpfnwglGetCurrentContext;
		wglCreateContextProc mpfnwglCreateContext;
		wglDeleteContextProc mpfnwglDeleteContext;
		wglGetProcAddressProc mpfnwglGetProcAddress;
		wglMakeCurrentProc mpfnwglMakeCurrent;
		wglShareListsProc mpfnwglShareLists;
	#endif
		wglSwapIntervalEXTProc mpfnwglSwapIntervalEXT;
		wglGetSwapIntervalEXTProc mpfnwglGetSwapIntervalEXT;
	} GLCONTEXT;
	#define GLCONTEXT_GDI32CALL
#elif defined(__amigaos__)
	#define GLContextHasHardwareFullscreen() TRUE
	typedef struct amigamesa_context* GLCONTEXTHANDLE;
	typedef struct Window* GLDRAWABLEHANDLE;
	typedef GLDRAWABLEHANDLE GLWINDOWHANDLE;
	typedef unsigned long attrib_t; /* struct TagItem == 2 ULONGs */
	typedef struct GLCONTEXT
	{
		GLCONTEXTHANDLE mhContext;
	} GLCONTEXT;
#elif (!defined(__MACH__) || defined(WSMACCARBONSYSTEM)) && \
	(defined(__SC__) || defined(__MRC__) \
	|| (defined(__MACH__) && !defined(__i386__) && !defined(__INTEL__)) \
	|| (defined(__MWERKS__) && defined(macintosh) && !defined(__INTEL__)) \
	|| (defined(__GNUC__) \
	&& (defined(__m68k__) || defined(__ppc__)) && !defined(__unix__))) \
	&& !defined(__amigaos__)
	typedef struct OpaqueWindowPtr* GLDRAWABLEHANDLE;
	typedef struct __AGLPixelFormatRec* GLPIXELFORMATHANDLE;
	typedef struct __AGLContextRec* GLCONTEXTHANDLE;
	typedef long attrib_t; /* GLint is long in AGL */
	typedef GLDRAWABLEHANDLE GLWINDOWHANDLE;
	#define GLMAC
	#define GLContextHasHardwareFullscreen() TRUE
	typedef struct GLCONTEXT
	{
		struct __AGLContextRec* mhContext;
		GLWINDOWHANDLE mhWindow;
	} GLCONTEXT;
#elif defined(__MACH__)
	/*#include <ConditionalMacros.h>*/
	#if !defined(DECLARE_OBJC_CLASS)
		#if (defined(__GNUC__) && defined(__APPLE__))
			#if defined(__OBJC__)
				#define DECLARE_OBJC_CLASS(name) \
					@class name;
			#else /*  not defined(__OBJC__) */
				#define DECLARE_OBJC_CLASS(name) \
					typedef struct name name;
			#endif /*  defined(__OBJC__) */
		#else /*  not Apple's gcc */
			#define DECLARE_OBJC_CLASS(name) \
				typedef struct objc_object name;
		#endif /*  (defined(__GNUC__) && defined(__APPLE__)) */
	#endif /*  !defined(DECLARE_OBJC_CLASS) */

	#if defined(__has_builtin) && __has_builtin(__is_target_arch) \
		&& __has_builtin(__is_target_vendor) && __has_builtin(__is_target_os) \
		&& __has_builtin(__is_target_environment) \
		&& (__is_target_arch(x86_64) \
			&& __is_target_vendor(apple) && __is_target_os(ios)) \
			|| (__is_target_arch(x86_64) && __is_target_vendor(apple) \
				&& __is_target_os(ios) && __is_target_environment(simulator))
		#define GLMACIOS
	#endif
	#if defined(GLMACIOS)
		DECLARE_OBJC_CLASS(EAGLContext);
		DECLARE_OBJC_CLASS(NSString);
		DECLARE_OBJC_CLASS(UIView);
		DECLARE_OBJC_CLASS(UIWindow);
		typedef UIView* GLDRAWABLEHANDLE;
		typedef NSString* GLPIXELFORMATHANDLE;
		typedef EAGLContext* GLCONTEXTHANDLE;
		typedef UIWindow* GLWINDOWHANDLE;
		typedef int attrib_t;
	#elif defined(__MACH__) && defined(__OBJC__)
		#define GLMACOSXCOCOA
		DECLARE_OBJC_CLASS(NSOpenGLContext);
		DECLARE_OBJC_CLASS(NSOpenGLPixelFormat);
		DECLARE_OBJC_CLASS(NSView);
		DECLARE_OBJC_CLASS(NSWindow);
		typedef NSView* GLDRAWABLEHANDLE;
		typedef NSOpenGLPixelFormat* GLPIXELFORMATHANDLE;
		typedef NSOpenGLContext* GLCONTEXTHANDLE;
		typedef NSWindow* GLWINDOWHANDLE;
		typedef unsigned int attrib_t; /* NSOpenGLPixelFormatAttribute */
	#else
		typedef struct OpaqueWindowPtr* GLDRAWABLEHANDLE;
		typedef struct __AGLPixelFormatRec* GLPIXELFORMATHANDLE;
		typedef struct __AGLContextRec* GLCONTEXTHANDLE;
		typedef int attrib_t; /* OSX version is proper int */
		typedef GLDRAWABLEHANDLE GLWINDOWHANDLE;
		#define GLContextHasHardwareFullscreen() TRUE
	#endif
	typedef struct GLCONTEXT
	{
		GLCONTEXTHANDLE mhContext;
		GLWINDOWHANDLE mhWindow;
	} GLCONTEXT;
/*#elif defined(__amigaos__)*/
#else
	#define GLUNIX
	#if defined(HAVE_EGL)
		typedef void* GLCONTEXTHANDLE; /* EGLContext==void* */
		typedef void* GLPIXELFORMATHANDLE; /* EGLConfig==void* */
		typedef void* GLDRAWABLEHANDLE; /* Window/wl_egl_window* */
		typedef void* GLSURFACEHANDLE; /* EGLSurface==void* */
		typedef void* GLDISPLAYHANDLE; /* EGLDisplay==void* */
		typedef void* GLWINDOWHANDLE; /* Window/wl_surface* */
	#else
		typedef struct __GLXcontextRec* GLCONTEXTHANDLE; /* GLXContext */
		typedef struct _XVisualInfo* GLPIXELFORMATHANDLE; /* XVisualInfo */
		typedef unsigned long GLDRAWABLEHANDLE; /* Window which is an XID */
		typedef GLDRAWABLEHANDLE GLWINDOWHANDLE;
		typedef struct __GLXFBConfigRec** GLSURFACEHANDLE;
		typedef struct _XDisplay* GLDISPLAYHANDLE;
		typedef void (*glXSwapIntervalEXTProc)(
			GLDISPLAYHANDLE, GLDRAWABLEHANDLE, int);
		typedef int (*glXSwapIntervalMESAProc)(int);
		typedef int (*glXGetSwapIntervalMESAProc)(void);
		typedef int (*glXSwapIntervalSGIProc)(int);
		#define GLCONTEXT_SWAPINTERVALTYPE_NONE 0
		#define GLCONTEXT_SWAPINTERVALTYPE_EXT 1
		#define GLCONTEXT_SWAPINTERVALTYPE_MESA 2
		#define GLCONTEXT_SWAPINTERVALTYPE_SGI 3
	#endif
	typedef int attrib_t;
	typedef struct GLCONTEXT
	{
		GLCONTEXTHANDLE mhContext;
		GLPIXELFORMATHANDLE mhPixelFormat;
		GLWINDOWHANDLE mhWindow;
		GLSURFACEHANDLE mhSurface;
		GLDISPLAYHANDLE mhDisplay;
		#if defined(HAVE_EGL)
			struct _XDisplay* mhXDisplay;
			struct wl_display* mhWDisplay;
			GLDRAWABLEHANDLE mhDrawable;
			struct wl_output* mhWOutput;
			int mWillCloseDisplay;
			int mSwapInterval;
		#else
			int mWillCloseDisplay;
			int mSwapIntervalType;
			union
			{
				struct
				{
					glXSwapIntervalEXTProc mpfnglXSwapIntervalEXT;
					int mSwapInterval;
					int mHasGLX_EXT_swap_control_tear;
				} EXT;
				struct
				{
					glXSwapIntervalMESAProc mpfnglXSwapIntervalMESA;
					glXGetSwapIntervalMESAProc mpfnglXGetSwapIntervalMESA;
				} MESA;
				struct
				{
					glXSwapIntervalSGIProc mpfnglXSwapIntervalSGI;
					int mSwapInterval;
				} SGI;
			} mSI;
		#endif
		int mIsFullscreen;
	} GLCONTEXT;
#endif /* Done defining GLCONTEXT object */
#if defined(GLUNIX) && !defined(HAVE_EGL)
	#define GLDRAWABLE_NONE 0
#else
	#define GLDRAWABLE_NONE NULL
#endif
#ifndef GLContextHasHardwareFullscreen
	#define GLContextHasHardwareFullscreen() FALSE
#endif
attrib_t* GLAttributeFind(attrib_t* attribs, int numattribs, attrib_t attrib);
void GLAttributeAdd(attrib_t* attribs, int* numattribs, attrib_t attrib);
void GLAttributeAddBits(attrib_t* attribs,
	int* numattribs, attrib_t attrib, int bits);

/*======================== Context Attributes ===============================*/
#define GLCONTEXT_MAX_CONTEXTATTRIBUTES 32
typedef struct GLCONTEXTATTRIBUTES
{
	attrib_t attribs[GLCONTEXT_MAX_CONTEXTATTRIBUTES];
	int numattribs;
} GLCONTEXTATTRIBUTES;
#define GLContextAttributes_Create(pThis) (pThis)->numattribs = 0
#define GLContextAttributes_Get(pThis) (pThis)->attribs
#define GLContextAttributes_GetCount(pThis) (pThis)->numattribs
#define GLContextAttributes_Find(pThis, val) \
	GLAttributeFind((pThis)->attribs, (pThis)->numattribs, val)
void GLContextAttributes_Add(GLCONTEXTATTRIBUTES* pThis, attrib_t attribute);
void GLContextAttributes_AddBits(GLCONTEXTATTRIBUTES* pThis,
	attrib_t attribute, int bits);
void GLContextAttributes_CoreProfile(GLCONTEXTATTRIBUTES* pThis);
void GLContextAttributes_MajorVersion(GLCONTEXTATTRIBUTES* pThis, int val);
void GLContextAttributes_MinorVersion(GLCONTEXTATTRIBUTES* pThis, int val);
void GLContextAttributes_CompatibilityProfile(GLCONTEXTATTRIBUTES* pThis);
void GLContextAttributes_ForwardCompatible(GLCONTEXTATTRIBUTES* pThis);
void GLContextAttributes_ES2(GLCONTEXTATTRIBUTES* pThis);
void GLContextAttributes_DebugCtx(GLCONTEXTATTRIBUTES* pThis);
void GLContextAttributes_Robust(GLCONTEXTATTRIBUTES* pThis);
void GLContextAttributes_NoResetNotify(GLCONTEXTATTRIBUTES* pThis);
void GLContextAttributes_LoseOnReset(GLCONTEXTATTRIBUTES* pThis);
void GLContextAttributes_ResetIsolation(GLCONTEXTATTRIBUTES* pThis);
void GLContextAttributes_ReleaseFlush(GLCONTEXTATTRIBUTES* pThis, int val);
void GLContextAttributes_PlatformDefaults(GLCONTEXTATTRIBUTES* pThis);
void GLContextAttributes_EndList(GLCONTEXTATTRIBUTES* pThis);
#define GLContextAttributes_SetNeedsARB(pThis) \
	(pThis)->numattribs = (pThis)->numattribs
#define GLContextAttributes_GetNeedsARB(pThis) 0
/*======================== Pixel Attributes ===============================*/
#define GLCONTEXT_MAX_PIXELATTRIBUTES 54
typedef struct GLPIXELATTRIBUTES
{
	attrib_t attribs[GLCONTEXT_MAX_PIXELATTRIBUTES];
	int numattribs;
} GLPIXELATTRIBUTES;
#define GLPixelAttributes_Create(pThis) (pThis)->numattribs = 0
#define GLPixelAttributes_Get(pThis) (pThis)->attribs
#define GLPixelAttributes_GetCount(pThis) (pThis)->numattribs
#define GLPixelAttributes_Find(pThis, val) \
	GLAttributeFind((pThis)->attribs, (pThis)->numattribs, val)
void GLPixelAttributes_Add(GLPIXELATTRIBUTES* pThis, attrib_t attribute);
void GLPixelAttributes_AddBits(GLPIXELATTRIBUTES* pThis,
	attrib_t attribute, int bits);
void GLPixelAttributes_RGBA(GLPIXELATTRIBUTES* pThis);
void GLPixelAttributes_BufferSize(GLPIXELATTRIBUTES* pThis, int val);
void GLPixelAttributes_Level(GLPIXELATTRIBUTES* pThis, int val);
void GLPixelAttributes_DoubleBuffer(GLPIXELATTRIBUTES* pThis);
void GLPixelAttributes_Stereo(GLPIXELATTRIBUTES* pThis);
void GLPixelAttributes_AuxBuffers(GLPIXELATTRIBUTES* pThis, int val);
void GLPixelAttributes_MinRGBA(GLPIXELATTRIBUTES* pThis,
	int mRed, int mGreen, int mBlue, int mAlpha);
void GLPixelAttributes_Depth(GLPIXELATTRIBUTES* pThis, int val);
void GLPixelAttributes_Stencil(GLPIXELATTRIBUTES* pThis, int val);
void GLPixelAttributes_MinAcumRGBA(GLPIXELATTRIBUTES* pThis,
	int mRed, int mGreen, int mBlue, int mAlpha);
void GLPixelAttributes_PlatformDefaults(GLPIXELATTRIBUTES* pThis);
void GLPixelAttributes_SampleBuffers(GLPIXELATTRIBUTES* pThis, int val);
void GLPixelAttributes_Samplers(GLPIXELATTRIBUTES* pThis, int val);
void GLPixelAttributes_FrameBuffersRGB(GLPIXELATTRIBUTES* pThis);
void GLPixelAttributes_EndList(GLPIXELATTRIBUTES* pThis);
#define GLPixelAttributes_SetNeedsARB(pThis) \
	(pThis)->numattribs = (pThis)->numattribs
#define GLPixelAttributes_GetNeedsARB(pThis) 0
/*======================== GL Context ===============================*/
typedef void (*GLContextEmptyProc)(void);
#define GLContext_IsCreated(pThis) (GLContext_GetHandle(pThis) != NULL)
#define GLContext_GetHandle(pThis) (pThis)->mhContext
int GLContext_Create(GLCONTEXT* pThis, GLDRAWABLEHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs);
int GLContext_CreateWithWindow(GLCONTEXT* pThis, GLWINDOWHANDLE window,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs);
void GLContext_Destroy(GLCONTEXT* pThis);
int GLContext_SwapBuffers(GLCONTEXT* pThis);
int GLContext_SetCurrent(GLCONTEXT* pThis);
/* extended API */
int GLContext_CreateClone(GLCONTEXT* pThis, GLCONTEXTHANDLE clone,
	GLDRAWABLEHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs);
int GLContext_CreateCloneWithWindow(GLCONTEXT* pThis, GLCONTEXTHANDLE clone,
	GLWINDOWHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs);
int GLContext_SetDrawable(GLCONTEXT* pThis, GLDRAWABLEHANDLE drawable);
GLWINDOWHANDLE GLContext_GetWindowHandle(GLCONTEXT* pThis);
GLCONTEXTHANDLE GLContext_GetCurrent(GLCONTEXT* pThis);
#define GLContext_IsCurrent(pThis) \
	(GLContext_GetHandle(pThis) == GLContext_GetCurrent(pThis))
int GLContext_SetSwapInterval(GLCONTEXT* pThis, int interval);
int GLContext_GetSwapInterval(GLCONTEXT* pThis);
GLContextEmptyProc GLContext_GetProc(GLCONTEXT* pThis, const char* proc);
void GLContext_ResizeDrawable(GLCONTEXT* pThis, int width, int height);
int GLContext_SetCurrentHandle(GLCONTEXT* pThis, GLCONTEXTHANDLE handle);
const char* GLContext_GetNativeExtensions(GLCONTEXT* pThis);
int GLContextFindExtension(const char* list, const char* extension);
/*======================== GL Headers ===============================*/
#if defined(GLCONTEXT_INCLUDEOPENGLHEADERS)
#	if defined(macintosh)
#		include "gl.h"
#		include "glu.h"
#	elif defined(__MACH__)
#		include <OpenGL/gl.h> /* has to be caps */
#		include <OpenGL/glu.h> /* has to be caps */
#	else
#		include <GL/gl.h> /* has to be caps */
#		include <GL/glu.h> /* has to be caps */
#	endif
#endif /* GLCONTEXT_INCLUDEOPENGLHEADERS */
#ifdef __cplusplus
	} /* end extern "C" */
#endif
#endif /* __GLCONTEXT_H__ */
#if defined(GLCONTEXT_IMPLEMENTATION)
/*=============================================================================
		 #####  #####                                     #####
		 #     #     #  ####  #    # #    #  ####  #    #     #
		 #     #       #    # ##  ## ##  ## #    # ##   #     #
		 #     #       #    # # ## # # ## # #    # # #  #     #
		 #     #       #    # #    # #    # #    # #  # #     #
		 #     #     # #    # #    # #    # #    # #   ##     #
		 #####  #####   ####  #    # #    #  ####  #    # #####
=============================================================================*/
#include <string.h>
#ifdef __cplusplus
	extern "C" {
#endif
/*---------------- GLContextFindExtension() ---------------------------------*/
int GLContextFindExtension(const char* list, const char* extension)
{
	const char* p;
	for (p = list; *p; ++p)
	{
		p = strstr(p, extension);
		if (!p)
			break;
		if (p == list || p[-1] == ' ')
		{ /* make sure it is not contained in the name of another extension */
			char c = p[strlen(extension)];
			if (c == '\0' || c == ' ')
				return 1;
		}
	}
	return 0;
}

/*---------------- GLAttribute[XXX]() helpers -------------------------------*/
attrib_t* GLAttributeFind(attrib_t* attribs, int numattribs, attrib_t attrib)
{
	int i;
	for(i = 0; attribs[i] != attrib && i < numattribs; ++i) {}
	return &attribs[i];
}
void GLAttributeAdd(attrib_t* attribs, int* numattribs, attrib_t attrib)
{
	attribs[*numattribs] = attrib;
	++*numattribs;
}
void GLAttributeAddBits(attrib_t* attribs, int* numattribs,
	attrib_t attrib, int bits)
{
	int i;
	for(i = 0; attribs[i] != attrib && i < *numattribs; ++i) {}
	if(i == *numattribs)
	{
		attribs[i] = bits;
		++*numattribs;
	}
	else
		attribs[i] |= bits;
}

/*------------- GL[Pixel/Context]Attribute[XXX]() helpers -------------------*/
void GLContextAttributes_Add(GLCONTEXTATTRIBUTES* pThis, attrib_t attribute)
{
	GLAttributeAdd(pThis->attribs, &pThis->numattribs, attribute);
}
void GLContextAttributes_AddBits(GLCONTEXTATTRIBUTES* pThis,
	attrib_t attribute, int bits)
{
	GLAttributeAddBits(pThis->attribs, &pThis->numattribs, attribute, bits);
}

void GLPixelAttributes_Add(GLPIXELATTRIBUTES* pThis, attrib_t attribute)
{
	GLAttributeAdd(pThis->attribs, &pThis->numattribs, attribute);
}
void GLPixelAttributes_AddBits(GLPIXELATTRIBUTES* pThis,
	attrib_t attribute, int bits)
{
	GLAttributeAddBits(pThis->attribs, &pThis->numattribs, attribute, bits);
}
#ifdef __cplusplus
	} /* end extern "C" */
#endif
/*=============================================================================
		 #     # ### #     #  #####   #####     #     #  #####  #
		 #  #  #  #  ##    # #     # #     #    #  #  # #     # #
		 #  #  #  #  # #   #       #       #    #  #  # #       #
		 #  #  #  #  #  #  #  #####   #####     #  #  # #  #### #
		 #  #  #  #  #   # #       # #          #  #  # #     # #
		 #  #  #  #  #    ## #     # #          #  #  # #     # #
		  ## ##  ### #     #  #####  #######     ## ##   #####  #######
=============================================================================*/
#if defined(_WIN32)
#include <windows.h>
#if !defined(CDS_FULLSCREEN)
	#define CDS_FULLSCREEN 4
#endif
#ifdef _MSC_VER
	#pragma comment(lib, "opengl32")
	#pragma comment(lib, "glu32")
#endif

#ifndef WGL_ARB_pixel_format
#define WGL_ARB_pixel_format
#define WGL_DRAW_TO_WINDOW_ARB            0x2001
#define WGL_DRAW_TO_BITMAP_ARB            0x2002
#define WGL_ACCELERATION_ARB              0x2003
#define WGL_NEED_PALETTE_ARB              0x2004
#define WGL_NEED_SYSTEM_PALETTE_ARB       0x2005
#define WGL_SWAP_LAYER_BUFFERS_ARB        0x2006
#define WGL_SWAP_METHOD_ARB               0x2007
#define WGL_NUMBER_OVERLAYS_ARB           0x2008
#define WGL_NUMBER_UNDERLAYS_ARB          0x2009
#define WGL_SUPPORT_GDI_ARB               0x200F
#define WGL_SUPPORT_OPENGL_ARB            0x2010
#define WGL_DOUBLE_BUFFER_ARB             0x2011
#define WGL_STEREO_ARB                    0x2012
#define WGL_PIXEL_TYPE_ARB                0x2013
#define WGL_COLOR_BITS_ARB                0x2014
#define WGL_RED_BITS_ARB                  0x2015
#define WGL_GREEN_BITS_ARB                0x2017
#define WGL_BLUE_BITS_ARB                 0x2019
#define WGL_ALPHA_BITS_ARB                0x201B
#define WGL_ACCUM_BITS_ARB                0x201D
#define WGL_ACCUM_RED_BITS_ARB            0x201E
#define WGL_ACCUM_GREEN_BITS_ARB          0x201F
#define WGL_ACCUM_BLUE_BITS_ARB           0x2020
#define WGL_ACCUM_ALPHA_BITS_ARB          0x2021
#define WGL_DEPTH_BITS_ARB                0x2022
#define WGL_STENCIL_BITS_ARB              0x2023
#define WGL_AUX_BUFFERS_ARB               0x2024
#define WGL_NO_ACCELERATION_ARB           0x2025
#define WGL_GENERIC_ACCELERATION_ARB      0x2026
#define WGL_FULL_ACCELERATION_ARB         0x2027
#define WGL_SWAP_EXCHANGE_ARB             0x2028
#define WGL_SWAP_COPY_ARB                 0x2029
#define WGL_TYPE_RGBA_ARB                 0x202B
#define WGL_TYPE_COLORINDEX_ARB           0x202C
#endif

#ifndef WGL_ARB_multisample
#define WGL_ARB_multisample
#define WGL_SAMPLE_BUFFERS_ARB            0x2041
#define WGL_SAMPLES_ARB                   0x2042
#endif

#ifndef WGL_ARB_framebuffer_sRGB
#define WGL_ARB_framebuffer_sRGB
#define WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB  0x20A9
#endif

#ifndef WGL_ARB_create_context
#define WGL_ARB_create_context
#define WGL_CONTEXT_MAJOR_VERSION_ARB			0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB			0x2092
#define WGL_CONTEXT_LAYER_PLANE_ARB				0x2093
#define WGL_CONTEXT_FLAGS_ARB					0x2094
#define WGL_CONTEXT_DEBUG_BIT_ARB				0x0001
#define WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB  0x0002
#endif

#ifndef WGL_ARB_create_context_profile
#define WGL_ARB_create_context_profile
#define WGL_CONTEXT_PROFILE_MASK_ARB              0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB          0x00000001
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
#endif

#ifndef WGL_ARB_create_context_robustness
#define WGL_ARB_create_context_robustness
#define WGL_CONTEXT_ROBUST_ACCESS_BIT_ARB				0x00000004
#define WGL_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB     0x8256
#define WGL_NO_RESET_NOTIFICATION_ARB                   0x8261
#define WGL_LOSE_CONTEXT_ON_RESET_ARB                   0x8252
#endif

#ifndef WGL_ARB_robustness_application_isolation
#define WGL_ARB_robustness_application_isolation
#define WGL_CONTEXT_RESET_ISOLATION_BIT_ARB             0x00000008
#endif
#ifndef WGL_ARB_robustness_share_group_isolation
#define WGL_ARB_robustness_share_group_isolation
#endif

#ifndef WGL_ARB_context_flush_control
#define WGL_ARB_context_flush_control
#define WGL_CONTEXT_RELEASE_BEHAVIOR_ARB            0x2097
#define WGL_CONTEXT_RELEASE_BEHAVIOR_NONE_ARB       0
#define WGL_CONTEXT_RELEASE_BEHAVIOR_FLUSH_ARB      0x2098
#endif

#ifndef WGL_EXT_create_context_es2_profile
#define WGL_EXT_create_context_es2_profile
#define WGL_CONTEXT_ES2_PROFILE_BIT_EXT           0x00000004
#endif

#ifndef WGL_EXT_create_context_es_profile
#define WGL_EXT_create_context_es_profile
#define WGL_CONTEXT_ES_PROFILE_BIT_EXT            0x00000004
#endif

#ifdef __cplusplus
	extern "C" {
#endif
/*------------------ GLCONTEXTATTRIBUTES ------------------------------------*/

void GLContextAttributes_CoreProfile(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_AddBits(pThis,
		WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}

void GLContextAttributes_MajorVersion(GLCONTEXTATTRIBUTES* pThis, int val)
{
	if (val > 0)
	{
		GLContextAttributes_Add(pThis, WGL_CONTEXT_MAJOR_VERSION_ARB);
		GLContextAttributes_Add(pThis, val);
		if (val >= 3)
			GLContextAttributes_SetNeedsARB(pThis);
	}
}

void GLContextAttributes_MinorVersion(GLCONTEXTATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLContextAttributes_Add(pThis, WGL_CONTEXT_MINOR_VERSION_ARB);
		GLContextAttributes_Add(pThis, val);
	}
}

void GLContextAttributes_CompatibilityProfile(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_AddBits(pThis, WGL_CONTEXT_PROFILE_MASK_ARB,
		WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}

void GLContextAttributes_ForwardCompatible(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_AddBits(pThis,
		WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}

void GLContextAttributes_ES2(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_AddBits(pThis, WGL_CONTEXT_PROFILE_MASK_ARB,
		WGL_CONTEXT_ES2_PROFILE_BIT_EXT);
	GLContextAttributes_SetNeedsARB(pThis);
}

void GLContextAttributes_DebugCtx(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_AddBits(pThis, WGL_CONTEXT_FLAGS_ARB,
		WGL_CONTEXT_DEBUG_BIT_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}

void GLContextAttributes_Robust(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_AddBits(pThis,WGL_CONTEXT_FLAGS_ARB,
		WGL_CONTEXT_ROBUST_ACCESS_BIT_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}

void GLContextAttributes_NoResetNotify(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_Add(pThis,
		WGL_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB);
	GLContextAttributes_Add(pThis, WGL_NO_RESET_NOTIFICATION_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}

void GLContextAttributes_LoseOnReset(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_Add(pThis,
		WGL_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB);
	GLContextAttributes_Add(pThis, WGL_LOSE_CONTEXT_ON_RESET_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}

void GLContextAttributes_ResetIsolation(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_AddBits(pThis, WGL_CONTEXT_FLAGS_ARB,
				  WGL_CONTEXT_RESET_ISOLATION_BIT_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}

void GLContextAttributes_ReleaseFlush(GLCONTEXTATTRIBUTES* pThis, int val)
{
	GLContextAttributes_Add(pThis, WGL_CONTEXT_RELEASE_BEHAVIOR_ARB);
	if (val == 1)
		GLContextAttributes_Add(pThis, WGL_CONTEXT_RELEASE_BEHAVIOR_FLUSH_ARB);
	else
		GLContextAttributes_Add(pThis, WGL_CONTEXT_RELEASE_BEHAVIOR_NONE_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}

void GLContextAttributes_PlatformDefaults(GLCONTEXTATTRIBUTES* pThis)
{
	(void)(pThis);
}

void GLContextAttributes_EndList(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_Add(pThis, 0);
}

/*------------------ GLPIXELATTRIBUTES --------------------------------------*/

void GLPixelAttributes_RGBA(GLPIXELATTRIBUTES* pThis)
{
	GLPixelAttributes_Add(pThis, WGL_PIXEL_TYPE_ARB);
	GLPixelAttributes_Add(pThis, WGL_TYPE_RGBA_ARB);
}

void GLPixelAttributes_BufferSize(GLPIXELATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLPixelAttributes_Add(pThis, WGL_COLOR_BITS_ARB);
		GLPixelAttributes_Add(pThis, val);
	}
}

void GLPixelAttributes_Level(GLPIXELATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLPixelAttributes_Add(pThis, WGL_NUMBER_OVERLAYS_ARB);
		GLPixelAttributes_Add(pThis, val);
	}
	else
	{
		GLPixelAttributes_Add(pThis, WGL_NUMBER_UNDERLAYS_ARB);
		GLPixelAttributes_Add(pThis, -val);
	}
	if (val < -1 || val > 1)
	{
		GLPixelAttributes_SetNeedsARB(pThis);
	}
}

void GLPixelAttributes_DoubleBuffer(GLPIXELATTRIBUTES* pThis)
{
	GLPixelAttributes_Add(pThis, WGL_DOUBLE_BUFFER_ARB);
	GLPixelAttributes_Add(pThis, TRUE);
}

void GLPixelAttributes_Stereo(GLPIXELATTRIBUTES* pThis)
{
	GLPixelAttributes_Add(pThis, WGL_STEREO_ARB);
	GLPixelAttributes_Add(pThis, TRUE);
}

void GLPixelAttributes_AuxBuffers(GLPIXELATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLPixelAttributes_Add(pThis, WGL_AUX_BUFFERS_ARB);
		GLPixelAttributes_Add(pThis, val);
	}
}

void GLPixelAttributes_MinRGBA(GLPIXELATTRIBUTES* pThis,
	int red, int green, int blue, int alpha)
{
	int colorBits = 0;
	if (red >= 0)
	{
		GLPixelAttributes_Add(pThis, WGL_RED_BITS_ARB);
		GLPixelAttributes_Add(pThis, red);
		colorBits += red;
	}
	if (green >= 0)
	{
		GLPixelAttributes_Add(pThis, WGL_GREEN_BITS_ARB);
		GLPixelAttributes_Add(pThis, green);
		colorBits += green;
	}
	if (blue >= 0)
	{
		GLPixelAttributes_Add(pThis, WGL_BLUE_BITS_ARB);
		GLPixelAttributes_Add(pThis, blue);
		colorBits += blue;
	}
	if (alpha >= 0)
	{
		GLPixelAttributes_Add(pThis, WGL_ALPHA_BITS_ARB);
		GLPixelAttributes_Add(pThis, alpha);
		/* color bits is RGB only */
	}
	if (colorBits)
	{
		GLPixelAttributes_Add(pThis, WGL_COLOR_BITS_ARB);
		GLPixelAttributes_Add(pThis, colorBits);
	}
}

void GLPixelAttributes_Depth(GLPIXELATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLPixelAttributes_Add(pThis, WGL_DEPTH_BITS_ARB);
		GLPixelAttributes_Add(pThis, val);
	}
}

void GLPixelAttributes_Stencil(GLPIXELATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLPixelAttributes_Add(pThis, WGL_STENCIL_BITS_ARB);
		GLPixelAttributes_Add(pThis, val);
	}
}

void GLPixelAttributes_MinAcumRGBA(GLPIXELATTRIBUTES* pThis,
	int red, int green, int blue, int alpha)
{
	int acumBits = 0;
	if (red >= 0)
	{
		GLPixelAttributes_Add(pThis, WGL_ACCUM_RED_BITS_ARB);
		GLPixelAttributes_Add(pThis, red);
		acumBits += red;
	}
	if (green >= 0)
	{
		GLPixelAttributes_Add(pThis, WGL_ACCUM_GREEN_BITS_ARB);
		GLPixelAttributes_Add(pThis, green);
		acumBits += green;
	}
	if (blue >= 0)
	{
		GLPixelAttributes_Add(pThis, WGL_ACCUM_BLUE_BITS_ARB);
		GLPixelAttributes_Add(pThis, blue);
		acumBits += blue;
	}
	if (alpha >= 0)
	{
		GLPixelAttributes_Add(pThis, WGL_ACCUM_ALPHA_BITS_ARB);
		GLPixelAttributes_Add(pThis, alpha);
		acumBits += alpha;
	}
	if (acumBits)
	{
		GLPixelAttributes_Add(pThis, WGL_ACCUM_BITS_ARB);
		GLPixelAttributes_Add(pThis, acumBits);
	}
}

void GLPixelAttributes_SampleBuffers(GLPIXELATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLPixelAttributes_Add(pThis, WGL_SAMPLE_BUFFERS_ARB);
		GLPixelAttributes_Add(pThis, val);
		GLContextAttributes_SetNeedsARB(pThis);
	}
}

void GLPixelAttributes_Samplers(GLPIXELATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLPixelAttributes_Add(pThis, WGL_SAMPLES_ARB);
		GLPixelAttributes_Add(pThis, val);
		GLContextAttributes_SetNeedsARB(pThis);
	}
}

void GLPixelAttributes_FrameBuffersRGB(GLPIXELATTRIBUTES* pThis)
{
	GLPixelAttributes_Add(pThis, WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB);
	GLPixelAttributes_Add(pThis, TRUE);
	GLContextAttributes_SetNeedsARB(pThis);
}

void GLPixelAttributes_EndList(GLPIXELATTRIBUTES* pThis)
{
	GLPixelAttributes_Add(pThis, 0);
}

void GLPixelAttributes_PlatformDefaults(GLPIXELATTRIBUTES* pThis)
{
	GLPixelAttributes_Add(pThis, WGL_DRAW_TO_WINDOW_ARB);
	GLPixelAttributes_Add(pThis, TRUE);
	GLPixelAttributes_Add(pThis, WGL_SUPPORT_OPENGL_ARB);
	GLPixelAttributes_Add(pThis, TRUE);
	GLPixelAttributes_Add(pThis, WGL_ACCELERATION_ARB);
	GLPixelAttributes_Add(pThis, WGL_FULL_ACCELERATION_ARB);
}


typedef const char* (__stdcall *wglGetExtensionsStringARBProcPtr)(HDC hdc);
typedef const char* (__stdcall *wglGetExtensionsStringEXTProcPtr)();
typedef BOOL (__stdcall *wglChoosePixelFormatARBProcPtr)(HDC hdc,
	const int* piAttribIList, const FLOAT* pfAttribFList, UINT nMaxFormats,
	int* piFormats, UINT* nNumFormats);
typedef HGLRC(__stdcall *wglCreateContextAttribsARBProcPtr)(HDC hDC,
	HGLRC hShareContext, const int *attribList);
/* TODO: GLForceWindowToFront just does ShowWindow(hWnd, SW_SHOW) */
int GLForceWindowToFront(HWND hWnd)
{
	DWORD ForegroundThreadID;
	DWORD ThisThreadID;
	DWORD timeout;
	ShowWindow(hWnd, SW_SHOW);
	return TRUE;
	/* Uniconize -> SetFocus -> Raise -> ShowWindow(SW_SHOW) */
	if (IsIconic (hWnd))
	{
		ShowWindow (hWnd, SW_RESTORE);
	}
	if (GetForegroundWindow () == hWnd)
	{
		return 1;
	}
	else
	{
		/*
			Windows 98+ doesn't allow a window to be on front normally
			if another window has focus. This means the window may not have
			focus with DirectInput and related, especially with full screen
			apps. Windows 95 will disregard SystemParametersInfo() calls.
		*/
#if 0 /* check for Win98 - not needed as Win95 just ignores SPI calls */
		int Win32Platform;
		int Win32MajorVersion;
		int Win32MinorVersion;
		OSVERSIONINFO VersionInfo;
		VersionInfo.dwOSVersionInfoSize=sizeof (OSVERSIONINFO);
		GetVersionEx(&VersionInfo);
		Win32Platform = VersionInfo.dwPlatformId;
		Win32MajorVersion = VersionInfo.dwMajorVersion;
		Win32MinorVersion = VersionInfo.dwMinorVersion;
		if (((Win32Platform == VER_PLATFORM_WIN32_NT)
				&& (Win32MajorVersion> 4))
			|| ((Win32Platform == VER_PLATFORM_WIN32_WINDOWS)
				&& ((Win32MajorVersion > 4) || ((Win32MajorVersion == 4) &&
						(Win32MinorVersion > 0)))))
#endif
		{
			/*
				Variation:
				HWND hCurWnd = GetForegroundWindow();
				DWORD dwMyID = GetCurrentThreadId();
				DWORD dwCurID = GetWindowThreadProcessId(hCurWnd, NULL);
				AttachThreadInput(dwCurID, dwMyID, TRUE);
				SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0,
					SWP_NOSIZE | SWP_NOMOVE);
				SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0,
					SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE);
				SetForegroundWindow(hWnd);
				SetFocus(hWnd);
				SetActiveWindow(hWnd);
				AttachThreadInput(dwCurID, dwMyID, FALSE);
				ShowWindow(hWnd, SW_RESTORE);
			*/
			ForegroundThreadID = GetWindowThreadProcessId(
				GetForegroundWindow(), NULL);
			ThisThreadID = GetWindowThreadProcessId(hWnd, NULL);
			if (AttachThreadInput(ThisThreadID, ForegroundThreadID, TRUE))
			{
				BringWindowToTop(hWnd); /* IE 5.5 */
				SetForegroundWindow(hWnd);
				AttachThreadInput(ThisThreadID, ForegroundThreadID, FALSE);
				if (GetForegroundWindow() != hWnd)
				{
					int waittime = 0;
			#define SPI_GETFOREGROUNDLOCKTIMEOUT 0x2000
			#define SPI_SETFOREGROUNDLOCKTIMEOUT 0x2001
					SystemParametersInfoA(SPI_GETFOREGROUNDLOCKTIMEOUT,
						0, &timeout, 0);
					SystemParametersInfoA(SPI_SETFOREGROUNDLOCKTIMEOUT,
						0, 0, SPIF_SENDCHANGE);
					/* or SPIF_SENDWININICHANGE | SPIF_UPDATEINIFILE */
					BringWindowToTop (hWnd); /* IE 5.5; normally you'd call
										ShowWindow(hWnd, SW_SHOWNORMAL) */
					SetForegroundWindow (hWnd);
					while (GetForegroundWindow() != hWnd && waittime < 1000)
					{ /* It can take some time for Windows to be ready
					  to make it top, wait and keep trying */
						Sleep(100);
						SetForegroundWindow(hWnd);
						waittime += 100;
					}
					/* UpdateWindow(hWnd); */
					SystemParametersInfoA(SPI_SETFOREGROUNDLOCKTIMEOUT,
						0, &timeout, SPIF_SENDCHANGE);
					/* or SPIF_SENDWININICHANGE | SPIF_UPDATEINIFILE */
				}
			}

		}
#if 0 /* Win95 only */
		else
		{
			BringWindowToTop (hWnd); /* IE 5.5 */
			SetForegroundWindow (hWnd);
		}
#endif
		return GetForegroundWindow () == hWnd;
	}
}
void GLContext_CreateCleanup_(GLCONTEXT* pThis)
{
	if(pThis->mhContext)
		GLCONTEXTOPENGL32CALL(wglDeleteContext)(pThis->mhContext);
	ReleaseDC(pThis->mhWnd, pThis->mhDC);
	if(pThis->mAtom)
	{
		if(pThis->mhWnd)
			DestroyWindow(pThis->mhWnd);
		UnregisterClassA((LPCSTR)MAKEINTATOM(pThis->mAtom),
			GetModuleHandleA(NULL));
	}
#if !defined(GLCONTEXT_STATICOPENGL32)
	FreeLibrary(pThis->mhOpenGL32);
#endif
#if !defined(GLCONTEXT_STATICGDI32)
	FreeLibrary(pThis->mhGDI32);
#endif
}

int GLContext_CreateWindowClass(GLCONTEXT* pThis, HINSTANCE hInstance)
{
	WNDCLASSA wc;
	char classname[48];
	wsprintfA(classname, "GLWrapper%p", pThis);
	wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
	wc.lpfnWndProc = DefWindowProcA;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = hInstance;
	wc.hIcon = NULL; /* A.K.A. LoadIcon(NULL, IDI_WINLOGO); */
	wc.hCursor = NULL; /* A.K.A. LoadCursor((HINSTANCE)NULL, IDC_ARROW); */
	wc.hbrBackground = NULL;
	wc.lpszMenuName = NULL;
	wc.lpszClassName = classname;
	pThis->mAtom = RegisterClassA(&wc); /* !atom == error */
	if(pThis->mAtom == 0)
	{
		pThis->mhWnd = NULL;
		return FALSE;
	}
	return TRUE;
}
int GLContext_CreateWindow(GLCONTEXT* pThis, HINSTANCE hInstance,
	int x, int y, int w, int h)
{
	pThis->mhWnd = CreateWindowA(
		(LPCSTR)MAKEINTATOM(pThis->mAtom), "",
		WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
		x, y, w, h, NULL, NULL,
		hInstance, NULL);
	return pThis->mhWnd != NULL;
}
typedef struct GLCONTEXTMONITORINFO
{
	DWORD cbSize;
	RECT rcMonitor;
	RECT rcWork;
	DWORD dwFlags;
} GLCONTEXTMONITORINFO;
typedef void* (__stdcall* GLContextMonitorFromPointProc)(POINT, DWORD);
typedef BOOL (__stdcall* GLContextGetMonitorInfoProc)(void*,
	GLCONTEXTMONITORINFO*);
#define GLCONTEXT_MONITOR_DEFAULTTOPRIMARY 0x00000001
void GLContextWinGetPrimaryMonitorRect(RECT* rect)
{
	HMODULE user32 = GetModuleHandleA("user32.dll");
	GLContextMonitorFromPointProc monitorfrompoint = NULL;
	GLContextGetMonitorInfoProc getmonitorinfo = NULL;
	GLCONTEXTMONITORINFO monitorinfo;
	POINT origin;
	rect->left = 0;
	rect->top = 0;
	rect->right = GetSystemMetrics(SM_CXSCREEN);
	rect->bottom = GetSystemMetrics(SM_CYSCREEN);
	if(user32)
	{
		monitorfrompoint = (GLContextMonitorFromPointProc)
			GetProcAddress(user32, "MonitorFromPoint");
		getmonitorinfo = (GLContextGetMonitorInfoProc)
			GetProcAddress(user32, "GetMonitorInfoA");
	}
	if(!monitorfrompoint || !getmonitorinfo)
		return;
	origin.x = 0;
	origin.y = 0;
	monitorinfo.cbSize = sizeof(monitorinfo);
	if((*getmonitorinfo)((*monitorfrompoint)(origin,
			GLCONTEXT_MONITOR_DEFAULTTOPRIMARY), &monitorinfo))
		*rect = monitorinfo.rcMonitor;
}

const char* GLContext_GetNativeExtensions(GLCONTEXT* pThis)
{
	wglGetExtensionsStringARBProcPtr pfnwglGetExtensionsStringARB;
	wglGetExtensionsStringEXTProcPtr pfnwglGetExtensionsStringEXT;
	const char* exts;
	pfnwglGetExtensionsStringARB = (wglGetExtensionsStringARBProcPtr)
		GLCONTEXTOPENGL32CALL(wglGetProcAddress)("wglGetExtensionsStringARB");
	if (pfnwglGetExtensionsStringARB) /* wglGetCurrentDC() */
			exts = (*pfnwglGetExtensionsStringARB)(pThis->mhDC);
	else
	{
		pfnwglGetExtensionsStringEXT = (wglGetExtensionsStringEXTProcPtr)
			GLCONTEXTOPENGL32CALL(wglGetProcAddress)(
				"wglGetExtensionsStringEXT");
		if(pfnwglGetExtensionsStringEXT)
			exts = (*pfnwglGetExtensionsStringEXT)();
		else
			exts = "";
	}
	return exts;
}
HINSTANCE GLContextWinGetFunctionBase(void* func)
{
	MEMORY_BASIC_INFORMATION mbi;
	mbi.AllocationBase = NULL;
	VirtualQuery(func, &mbi, sizeof(mbi));
	return (HINSTANCE)(mbi.AllocationBase);
}

int GLContext_CreateClone(GLCONTEXT* pThis, GLCONTEXTHANDLE clone,
	GLDRAWABLEHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ /* https://stackoverflow.com/questions/1400654/
  how-do-i-put-my-opengl-app-into-fullscreen-mode */
	PIXELFORMATDESCRIPTOR pfd =
	{
		sizeof(PIXELFORMATDESCRIPTOR),
		1,						/* version */
		PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,	/* flags */
		/*PFD_TYPE_COLORINDEX*/
		PFD_TYPE_RGBA,
		/* color format */
		32,						/* color depth */
		0, 0, 0, 0, 0, 0,		/* color bits */
		0,						/* alpha buffer */
		0,						/* shift bit */
		0,						/* accumulation buffer */
		0, 0, 0, 0,				/* accumulation bits */
		24,						/* z-buffer/depth buffer */
		0,						/* stencil buffer */
		0,						/* auxiliary buffer */
		PFD_MAIN_PLANE,			/* plane flags */
		0,						/* reserved */
		0, 0, 0					/* layer masks */
	};
	int pixelformat;
	const char* exts;
	wglChoosePixelFormatARBProcPtr pfnwglChoosePixelFormatARB;
	wglCreateContextAttribsARBProcPtr pfnwglCreateContextAttribsARB;
	int* attribs = pixelattribs ?
		GLPixelAttributes_Get(pixelattribs) : NULL;
	HINSTANCE hInstance = GetModuleHandleA(NULL);
	/* Setup dynloads if applicable */
#if !defined(GLCONTEXT_STATICOPENGL32)
	pThis->mhOpenGL32 = LoadLibraryA("opengl32.dll");
	if(!pThis->mhOpenGL32)
		return FALSE;
	pThis->mpfnwglGetCurrentContext = (wglGetCurrentContextProc)
		GetProcAddress(pThis->mhOpenGL32, "wglGetCurrentContext");
	pThis->mpfnwglCreateContext = (wglCreateContextProc)
		GetProcAddress(pThis->mhOpenGL32, "wglCreateContext");
	pThis->mpfnwglDeleteContext = (wglDeleteContextProc)
		GetProcAddress(pThis->mhOpenGL32, "wglDeleteContext");
	pThis->mpfnwglGetProcAddress = (wglGetProcAddressProc)
		GetProcAddress(pThis->mhOpenGL32, "wglGetProcAddress");
	pThis->mpfnwglMakeCurrent = (wglMakeCurrentProc)
		GetProcAddress(pThis->mhOpenGL32, "wglMakeCurrent");
	pThis->mpfnwglShareLists = (wglShareListsProc)
		GetProcAddress(pThis->mhOpenGL32, "wglShareLists");
#else
	pThis->mhOpenGL32 = GLContextWinGetFunctionBase(wglGetProcAddress);
#endif
#if !defined(GLCONTEXT_STATICGDI32)
	pThis->mhGDI32 = LoadLibraryA("gdi32.dll");
	pThis->mpfnChoosePixelFormat = (ChoosePixelFormatProc)
		GetProcAddress(pThis->mhGDI32, "ChoosePixelFormat");
	pThis->mpfnSetPixelFormat = (SetPixelFormatProc)
		GetProcAddress(pThis->mhGDI32, "SetPixelFormat");
	pThis->mpfnSwapBuffers = (SwapBuffersProc) /* MSDN says OpenGL32... */
		GetProcAddress(pThis->mhGDI32, "SwapBuffers");
#endif

	/*
		Create a background empty (dummy) window due to 3 rules in WGL:

		1) A rendering context must be created and be current in order
		to get newer WGL functions via wglGetProcAddress()
		https://learn.microsoft.com/en-us/windows/
		win32/api/wingdi/nf-wingdi-wglgetprocaddress

		2) A pixel format must be set for a hdc before creating a WGL
		context for it
		https://msdn.microsoft.com/en-us/library/dd374379%28v=vs.85%29.aspx

		3) A pixel format cannot be set more than once for a context
		https://msdn.microsoft.com/en-us/library/dd369049%28v=vs.85%29.aspx

		GetDC[Ex] have 5 common DCs available in Windows 95 when not called
		on a window with a CS_OWNDC style. CS_OWNDC windows require 800
		bytes each from the 64KB GDI local heap, so it isn't advisable to
		use the flag.

		However, if one of the common DCs is held on for too long,
		Windows 95 will reassign and cause any future calls to fail,
		including Windows OpenGL's wglMakeCurrent. This is prevented via
		CS_OWNDC as surmised, but the better resolution is just not to
		hold onto common DCs for too long. This also means in OpenGL the
		context needs to be recreated every now and then most likely.

		As a side note (WS_CLIPCHILDREN | WS_CLIPSIBLINGS) should
		be specified that use OpenGL on them. While not at this time
		explicitly mentioned in the docs, they are mentioned in books
		and the MSDN knowledge base.
	*/
	if(!GLContext_CreateWindowClass(pThis, hInstance)
		|| !GLContext_CreateWindow(pThis, hInstance, 0, 0, 0, 0))
	{
		GLContext_CreateCleanup_(pThis);
		return 0;
	}
	pThis->mhDC = GetDC(pThis->mhWnd);

	/* Create a disposable WGL context so we can call wglGetProcAddress
		Make sure to wglMakeCurrent it too or wglGetProcAddress will fail! */
	GLCONTEXTGDI32CALL(SetPixelFormat)(pThis->mhDC, 1, &pfd);
	pThis->mhContext = GLCONTEXTOPENGL32CALL(wglCreateContext)(pThis->mhDC);
	if (!pThis->mhContext
		|| !GLContext_SetCurrent(pThis))
	{
		GLContext_CreateCleanup_(pThis);
		return 0;
	}


	/*
		Get the list of supported extensions
	*/
	exts = GLContext_GetNativeExtensions(pThis);

	/* Grab the newer OpenGL 3+ context creation function */
	pfnwglCreateContextAttribsARB = (wglCreateContextAttribsARBProcPtr)
		GLCONTEXTOPENGL32CALL(wglGetProcAddress)("wglCreateContextAttribsARB");


	/* Now call either wglChoosePixelFormatARB or ChoosePixelFormat */
	{
		UINT numFormats;
		pfnwglChoosePixelFormatARB = (wglChoosePixelFormatARBProcPtr)
			GLCONTEXTOPENGL32CALL(wglGetProcAddress)("wglChoosePixelFormatARB");
		if(pfnwglChoosePixelFormatARB
			&& GLContextFindExtension(exts, "WGL_ARB_multisample"))
		{
			if (!(*pfnwglChoosePixelFormatARB)(pThis->mhDC,
					attribs, NULL, 1, &pixelformat, &numFormats)
			/*
				https://www.opengl.org/registry/specs/ARB/wgl_pixel_format.txt
				numFormats == 0 but wglChoosePixelFormatARB == TRUE means
				it was successful but no matching pixel formats were found
				and pixelformat remains uninitialized
			*/
					|| !numFormats)
				pixelformat = 0;
		}
		else
			pixelformat = 0;
	}
	/*for testing*/
	/*pixelformat = 0;*/
	if(!pixelformat)
	{
		/* if(!needsarb) */
		{
			/*
				Merge the user's requested attribs with our defaults

				Normally APIs use PFD_TYPE_COLORINDEX as the default. However,
				on earlier Windows or low spec this can default to formats
				where only one color is usable, glColor is ignored, and odd
				culling issues happen.
			*/
			if(attribs)
			{
				int iattrib;
				/*pfd.dwFlags &= ~PFD_DOUBLEBUFFER;*/
				pfd.iPixelType = PFD_TYPE_RGBA; /*PFD_TYPE_COLORINDEX;*/

				for (iattrib = 0; attribs[iattrib]; ++iattrib)
				{
					switch (attribs[iattrib])
					{
					case WGL_DRAW_TO_WINDOW_ARB:
						if (attribs[++iattrib])
							pfd.dwFlags |= PFD_DRAW_TO_WINDOW;
						break;

					case WGL_DRAW_TO_BITMAP_ARB:
						if (attribs[++iattrib])
							pfd.dwFlags |= PFD_DRAW_TO_BITMAP;
						break;

					case WGL_ACCELERATION_ARB:
						if (attribs[++iattrib] == WGL_GENERIC_ACCELERATION_ARB
							|| attribs[iattrib] == WGL_FULL_ACCELERATION_ARB)
							pfd.dwFlags |= PFD_GENERIC_ACCELERATED;
						break;

					case WGL_NEED_PALETTE_ARB:
						if (attribs[++iattrib])
							pfd.dwFlags |= PFD_NEED_PALETTE;
						break;

					case WGL_NEED_SYSTEM_PALETTE_ARB:
						if (attribs[++iattrib])
							pfd.dwFlags |= PFD_NEED_SYSTEM_PALETTE;
						break;

					case WGL_SWAP_LAYER_BUFFERS_ARB:
						if (attribs[++iattrib])
							pfd.dwFlags |= PFD_SWAP_LAYER_BUFFERS;
						break;

					case WGL_SWAP_METHOD_ARB:
						if (attribs[++iattrib] == WGL_SWAP_EXCHANGE_ARB)
							pfd.dwFlags |= PFD_SWAP_EXCHANGE;
						else if (attribs[iattrib] == WGL_SWAP_COPY_ARB)
							pfd.dwFlags |= PFD_SWAP_COPY;
						break;

					case WGL_NUMBER_OVERLAYS_ARB: /* bits 0-3 of bReserved */
						pfd.bReserved |= attribs[++iattrib] & 15;
						break;

					case WGL_NUMBER_UNDERLAYS_ARB: /* bits 4-7 of bReserved */
						pfd.bReserved |= attribs[++iattrib] & 240;
						break;

					case WGL_SUPPORT_GDI_ARB:
						if (attribs[++iattrib])
							pfd.dwFlags |= PFD_SUPPORT_GDI;
						break;

					case WGL_SUPPORT_OPENGL_ARB:
						if (attribs[++iattrib])
							pfd.dwFlags |= PFD_SUPPORT_OPENGL;
						break;

					case WGL_DOUBLE_BUFFER_ARB:
						if (attribs[++iattrib])
							pfd.dwFlags |= PFD_DOUBLEBUFFER;
						break;

					case WGL_STEREO_ARB:
						if (attribs[++iattrib])
							pfd.dwFlags |= PFD_STEREO;
						break;

					case WGL_PIXEL_TYPE_ARB:
						if (attribs[++iattrib] == WGL_TYPE_RGBA_ARB)
							pfd.iPixelType = PFD_TYPE_RGBA;
						else
							pfd.iPixelType = PFD_TYPE_COLORINDEX;
						break;

					case WGL_COLOR_BITS_ARB:
						pfd.cColorBits = (BYTE)attribs[++iattrib];
						break;

					case WGL_RED_BITS_ARB:
						pfd.cRedBits = (BYTE)attribs[++iattrib];
						break;

					case WGL_GREEN_BITS_ARB:
						pfd.cGreenBits = (BYTE)attribs[++iattrib];
						break;

					case WGL_BLUE_BITS_ARB:
						pfd.cBlueBits = (BYTE)attribs[++iattrib];
						break;

					case WGL_ALPHA_BITS_ARB:
						pfd.cAlphaBits = (BYTE)attribs[++iattrib];
						break;

					case WGL_ACCUM_BITS_ARB:
						pfd.cAccumBits = (BYTE)attribs[++iattrib];
						break;

					case WGL_ACCUM_RED_BITS_ARB:
						pfd.cAccumRedBits = (BYTE)attribs[++iattrib];
						break;

					case WGL_ACCUM_GREEN_BITS_ARB:
						pfd.cAccumGreenBits = (BYTE)attribs[++iattrib];
						break;

					case WGL_ACCUM_BLUE_BITS_ARB:
						pfd.cAccumBlueBits = (BYTE)attribs[++iattrib];
						break;

					case WGL_ACCUM_ALPHA_BITS_ARB:
						pfd.cAccumAlphaBits = (BYTE)attribs[++iattrib];
						break;

					case WGL_DEPTH_BITS_ARB:
						pfd.cDepthBits = (BYTE)attribs[++iattrib];
						break;

					case WGL_STENCIL_BITS_ARB:
						pfd.cStencilBits = (BYTE)attribs[++iattrib];
						break;

					case WGL_AUX_BUFFERS_ARB:
						pfd.cAuxBuffers = (BYTE)attribs[++iattrib];
						break;

					default:
						break;
					}
				}
			}

			pixelformat = GLCONTEXTGDI32CALL(ChoosePixelFormat)(
				pThis->mhDC, &pfd);
		}

		if(!pixelformat)
		{
			GLContext_CreateCleanup_(pThis);
			return 0;
		}
	}

	/* We have a valid pixel format, destroy the dummy window */
	GLCONTEXTOPENGL32CALL(wglMakeCurrent)(pThis->mhDC, NULL);
	GLCONTEXTOPENGL32CALL(wglDeleteContext)(pThis->mhContext);
	ReleaseDC(pThis->mhWnd, pThis->mhDC);
	DestroyWindow(pThis->mhWnd);
	pThis->mhContext = NULL;

	if(drawable)
	{ /* just clean up the wndclass */
		pThis->mhDC = drawable;
		UnregisterClassA((LPCSTR)MAKEINTATOM(pThis->mAtom), hInstance);
		pThis->mAtom = 0;
	}
	else
	{
		RECT screenrect;
		GLContextWinGetPrimaryMonitorRect(&screenrect);

		/* Create the full screen window */
		if(!GLContext_CreateWindow(pThis, hInstance,
				screenrect.left, screenrect.top,
				screenrect.right - screenrect.left,
				screenrect.bottom - screenrect.top))
		{
			GLContext_CreateCleanup_(pThis);
			return 0;
		}
#if 0
		/* Hide the start bar */
		if(ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN)
				!= DISP_CHANGE_SUCCESSFUL)
			return 0;
#elif 0
		int changed;
		DEVMODEA dm;
		EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
		dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
		if (width <= (int)dm.dmPelsWidth && height <= (int)dm.dmPelsHeight)
			dm.dmFields |= DM_DISPLAYFREQUENCY;
		changed = (ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN)
			== DISP_CHANGE_SUCCESSFUL);
		if (!changed && (dm.dmFields & DM_DISPLAYFREQUENCY))
		{
		   dm.dmFields &= ~DM_DISPLAYFREQUENCY;
		   changed = (ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN)
			   == DISP_CHANGE_SUCCESSFUL);
		}
#endif
		pThis->mhDC = GetDC(pThis->mhWnd);
	}

	/* Call SetPixelFormat */
	if(pixelformat == 0
		|| !GLCONTEXTGDI32CALL(SetPixelFormat)(pThis->mhDC, pixelformat, &pfd))
	{
		/*DWORD dwGLE = GetLastError();*/
		GLContext_CreateCleanup_(pThis);
		return 0;
	}
	pThis->mPixelFormat = pixelformat;
	DescribePixelFormat(pThis->mhDC, pixelformat, sizeof(pfd), &pfd);
	/*int stencil = DescribePixelFormat(pThis->mhDC, pixelformat, sizeof(pfd), &pfd);*/

	/* Call wglCreateContext; 2nd param is shared list glcontext,
		equiv of calling wglShareLists(this, other) after wglCreateContext */
	if(pfnwglCreateContextAttribsARB)
	{ /* default NULL attribs are fine here */
		attribs = contextattribs ?
			GLContextAttributes_Get(contextattribs) : NULL;
		pThis->mhContext = (*pfnwglCreateContextAttribsARB)(
			pThis->mhDC, clone, attribs);
	}
	if(!pThis->mhContext)
	{
		/*if(!needsarb)*/
			pThis->mhContext = GLCONTEXTOPENGL32CALL(wglCreateContext)(
				pThis->mhDC);
		if(!pThis->mhContext
			|| (clone && !GLCONTEXTOPENGL32CALL(wglShareLists)(
					pThis->mhContext, clone)))
		{
			GLContext_CreateCleanup_(pThis);
			return 0;
		}
	}
	{
		GLCONTEXTHANDLE pPrev = GLContext_GetCurrent(pThis);
		GLContext_SetCurrent(pThis);
		/* Get/Set Swap Interval procs and capabilities */
		if(GLContextFindExtension(exts, "WGL_EXT_swap_control"))
		{
			pThis->mpfnwglSwapIntervalEXT = (wglSwapIntervalEXTProc)
					GLCONTEXTOPENGL32CALL(wglGetProcAddress)(
						"wglSwapIntervalEXT");
			pThis->mpfnwglGetSwapIntervalEXT = (wglGetSwapIntervalEXTProc)
					GLCONTEXTOPENGL32CALL(wglGetProcAddress)(
						"wglGetSwapIntervalEXT");
			pThis->mHasWGL_EXT_swap_control_tear = (unsigned short)
				GLContextFindExtension(exts, "WGL_EXT_swap_control_tear");
		}
		else
		{
			pThis->mpfnwglSwapIntervalEXT = NULL;
			pThis->mpfnwglGetSwapIntervalEXT = NULL;
		}
		GLContext_SetCurrentHandle(pThis, pPrev);
	}
	if(pThis->mAtom)
		GLForceWindowToFront(pThis->mhWnd);
	return 1;
}
int GLContext_Create(GLCONTEXT* pThis, GLDRAWABLEHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return GLContext_CreateClone(pThis, NULL, drawable,
	contextattribs, pixelattribs); }
void GLContext_Destroy(GLCONTEXT* pThis)
{
	GLCONTEXTOPENGL32CALL(wglDeleteContext)(pThis->mhContext);
	if(pThis->mhWnd)
		ReleaseDC(pThis->mhWnd, pThis->mhDC);
	if(pThis->mAtom)
	{
		DestroyWindow(pThis->mhWnd);
		UnregisterClassA((LPCSTR)MAKEINTATOM(pThis->mAtom),
			GetModuleHandleA(NULL));
		#if 0
			ChangeDisplaySettingsA(NULL, 0); /* undo CDS_FULLSCREEN */
		#endif
	}
#if !defined(GLCONTEXT_STATICOPENGL32)
	FreeLibrary(pThis->mhOpenGL32);
#endif
#if !defined(GLCONTEXT_STATICGDI32)
	FreeLibrary(pThis->mhGDI32);
#endif
}
int GLContext_SwapBuffers(GLCONTEXT* pThis)
{
	/*
		Win32 SwapBuffers() behavior: after a few calls it will display change with
		a full screen window  (takes several seconds) if:
		A) Pass the non-DPI scaled res w/DPI aware ON,
		B) Pass the DPI-scaled res w/DPI aware OFF.
		Either a compat thing or the GPU is turning hardware accel on.
	*/
	return GLCONTEXTGDI32CALL(SwapBuffers)(pThis->mhDC);
}
int GLContext_SetCurrent(GLCONTEXT* pThis)
{ /* just change the DC to change the drawable */
	return GLCONTEXTOPENGL32CALL(wglMakeCurrent)(pThis->mhDC, pThis->mhContext);
}
int GLContext_SetCurrentHandle(GLCONTEXT* pThis, GLCONTEXTHANDLE handle)
{ /* just change the DC to change the drawable */
	return GLCONTEXTOPENGL32CALL(wglMakeCurrent)(pThis->mhDC, handle);
}
int GLContext_SetDrawable(GLCONTEXT* pThis, GLDRAWABLEHANDLE drawable)
{
	HINSTANCE hInstance;
	PIXELFORMATDESCRIPTOR pfd;
	memset(&pfd, 0, sizeof(pfd));
	pfd.nSize = sizeof(pfd);
	pfd.nVersion = 1;
	if(drawable)
	{
		if(pThis->mAtom)
		{
			#if 0
				if(ChangeDisplaySettingsA(NULL, 0) /* undo CDS_FULLSCREEN */
						!= DISP_CHANGE_SUCCESSFUL)
					return 0;
			#endif
			hInstance = GetModuleHandleA(NULL);
			ReleaseDC(pThis->mhWnd, pThis->mhDC);
			DestroyWindow(pThis->mhWnd);
			UnregisterClassA((LPCSTR)MAKEINTATOM(pThis->mAtom), hInstance);
			pThis->mAtom = 0; pThis->mhWnd = NULL;
		}
		pThis->mhDC = drawable;
		GLCONTEXTGDI32CALL(SetPixelFormat)(pThis->mhDC,
			pThis->mPixelFormat, &pfd);
	}
	else
	{
		RECT screenrect;
		hInstance = GetModuleHandleA(NULL);
		if(!pThis->mAtom)
		{
			if(!GLContext_CreateWindowClass(pThis, hInstance))
				return 0;
		}
		else
			return 1; /* already set */
		if(pThis->mhWnd)
			ReleaseDC(pThis->mhWnd, pThis->mhDC);
		GLContextWinGetPrimaryMonitorRect(&screenrect);
		if(
		#if 0
			ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN)
				!= DISP_CHANGE_SUCCESSFUL
			||
		#endif
			 !GLContext_CreateWindow(pThis, hInstance,
					screenrect.left, screenrect.top,
					screenrect.right - screenrect.left,
					screenrect.bottom - screenrect.top))
		{
			UnregisterClassA((LPCSTR)MAKEINTATOM(pThis->mAtom), hInstance);
			pThis->mAtom = 0; pThis->mhWnd = NULL;
			return 0;
		}
		pThis->mhDC = GetDC(pThis->mhWnd);
		if(!GLCONTEXTGDI32CALL(SetPixelFormat)(pThis->mhDC,
				pThis->mPixelFormat, &pfd))
		{
			ReleaseDC(pThis->mhWnd, pThis->mhDC);
			DestroyWindow(pThis->mhWnd);
			UnregisterClassA((LPCSTR)MAKEINTATOM(pThis->mAtom), hInstance);
			pThis->mAtom = 0; pThis->mhWnd = NULL; pThis->mhDC = NULL;
			return 0;
		}
		GLForceWindowToFront(pThis->mhWnd);
	}
	return GLContext_SetCurrent(pThis);
}
GLWINDOWHANDLE GLContext_GetWindowHandle(GLCONTEXT* pThis)
{ return pThis->mhWnd; }
GLCONTEXTHANDLE GLContext_GetCurrent(GLCONTEXT* pThis)
{ return GLCONTEXTOPENGL32CALL(wglGetCurrentContext)(); }
int GLContext_SetSwapInterval(GLCONTEXT* pThis, int interval)
{
	if(!pThis->mpfnwglSwapIntervalEXT
			|| ((interval < 0) && !pThis->mHasWGL_EXT_swap_control_tear))
		return 0;
	return (*pThis->mpfnwglSwapIntervalEXT)(interval);
}
int GLContext_GetSwapInterval(GLCONTEXT* pThis)
{
	if(!pThis->mpfnwglGetSwapIntervalEXT)
		return 0;
	return (*pThis->mpfnwglGetSwapIntervalEXT)();
}
GLContextEmptyProc GLContext_GetProc(GLCONTEXT* pThis, const char* proc)
{
	GLContextEmptyProc foundproc;
	foundproc = (GLContextEmptyProc)
		GLCONTEXTOPENGL32CALL(wglGetProcAddress)(proc);
	if(foundproc == (GLContextEmptyProc)(size_t)-1 /* old Intel/ATI */
			|| foundproc <= (GLContextEmptyProc)(size_t)3) /* covers NULL */
		foundproc = (GLContextEmptyProc)GetProcAddress(pThis->mhOpenGL32, proc);
	return foundproc;
}
void GLContext_ResizeDrawable(GLCONTEXT* pThis, int width, int height)
{
	(void)(pThis);(void)(width);(void)(height);
}
int GLContext_CreateCloneWithWindow(GLCONTEXT* pThis, GLCONTEXTHANDLE clone,
	GLWINDOWHANDLE window,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{
	HDC dc;
	if(window)
	{
		pThis->mhWnd = window;
		dc = GetDC(window);
	}
	else
		dc = NULL;
	if(!GLContext_CreateClone(pThis, clone, dc,
		contextattribs, pixelattribs))
	{
		pThis->mhWnd = NULL;
		return 0;
	}

	return 1;
}
int GLContext_CreateWithWindow(GLCONTEXT* pThis, GLWINDOWHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return GLContext_CreateCloneWithWindow(pThis, NULL, drawable,
	contextattribs, pixelattribs); }


#ifdef __cplusplus
	} /* end extern "C" */
#endif
/*=============================================================================
		#    #     # ###  #####     #       #     # #######  #####     #
	   # #   ##   ##  #  #     #   # #      ##   ## #       #     #   # #
	  #   #  # # # #  #  #        #   #     # # # # #       #        #   #
	 #     # #  #  #  #  #  #### #     #    #  #  # #####    #####  #     #
	 ####### #     #  #  #     # #######    #     # #             # #######
	 #     # #     #  #  #     # #     #    #     # #       #     # #     #
	 #     # #     # ###  #####  #     #    #     # #######  #####  #     #
=============================================================================*/
#elif defined(__amigaos__)
#include <GL/Amigamesa.h>
#ifdef __cplusplus
	extern "C" {
#endif
/*------------------ GLCONTEXTATTRIBUTES ------------------------------------*/
void GLContextAttributes_CoreProfile(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_MajorVersion(GLCONTEXTATTRIBUTES* pThis, int val)
{ }
void GLContextAttributes_MinorVersion(GLCONTEXTATTRIBUTES* pThis, int val)
{ }
void GLContextAttributes_CompatibilityProfile(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_ForwardCompatible(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_ES2(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_DebugCtx(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_Robust(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_NoResetNotify(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_LoseOnReset(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_ResetIsolation(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_ReleaseFlush(GLCONTEXTATTRIBUTES* pThis, int val)
{ }
void GLContextAttributes_PlatformDefaults(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_EndList(GLCONTEXTATTRIBUTES* pThis)
{ }
/*------------------ GLPIXELATTRIBUTES --------------------------------------*/
void GLPixelAttributes_AddOrAlter(GLPIXELATTRIBUTES* pThis,
	attrib_t key, attrib_t val)
{
	attrib_t* loc = GLPixelAttributes_Find(pThis, key);
	if(loc)
		loc[1] = val;
	else
	{
		GLPixelAttributes_Add(pThis, key);
		GLPixelAttributes_Add(pThis, val);
	}
}

void GLPixelAttributes_RGBA(GLPIXELATTRIBUTES* pThis)
{
	GLPixelAttributes_AddOrAlter(pThis, AMA_RGBMode, TRUE);
}
void GLPixelAttributes_BufferSize(GLPIXELATTRIBUTES* pThis, int val)
{
}
void GLPixelAttributes_Level(GLPIXELATTRIBUTES* pThis, int val)
{
}
void GLPixelAttributes_DoubleBuffer(GLPIXELATTRIBUTES* pThis)
{
	GLPixelAttributes_Add(pThis, AMA_DoubleBuf);
	GLPixelAttributes_Add(pThis, TRUE);
}

void GLPixelAttributes_Stereo(GLPIXELATTRIBUTES* pThis)
{
}
void GLPixelAttributes_AuxBuffers(GLPIXELATTRIBUTES* pThis, int val)
{
}
void GLPixelAttributes_MinRGBA(GLPIXELATTRIBUTES* pThis,
	int red, int green, int blue, int alpha)
{
	if (red >= 0 || green >= 0 || blue >= 0 || alpha >= 0)
		GLPixelAttributes_AddOrAlter(pThis, AMA_RGBMode, TRUE);
}
void GLPixelAttributes_Depth(GLPIXELATTRIBUTES* pThis, int val)
{
	GLPixelAttributes_AddOrAlter(pThis, AMA_NoDepth, FALSE);
}
void GLPixelAttributes_Stencil(GLPIXELATTRIBUTES* pThis, int val)
{
	GLPixelAttributes_AddOrAlter(pThis, AMA_NoStencil, FALSE);
}
void GLPixelAttributes_MinAcumRGBA(GLPIXELATTRIBUTES* pThis,
	int red, int green, int blue, int alpha)
{
	if (red >= 0 || green >= 0 || blue >= 0 || alpha >= 0)
		GLPixelAttributes_AddOrAlter(pThis, AMA_NoAccum, FALSE);
}
void GLPixelAttributes_SampleBuffers(GLPIXELATTRIBUTES* pThis, int val)
{ }
void GLPixelAttributes_Samplers(GLPIXELATTRIBUTES* pThis, int val)
{ }

void GLPixelAttributes_FrameBuffersRGB(GLPIXELATTRIBUTES* pThis)
{ }

void GLPixelAttributes_EndList(GLPIXELATTRIBUTES* pThis)
{ /* This will be handled by GLContext_Create() */ }

void GLPixelAttributes_PlatformDefaults(GLPIXELATTRIBUTES* pThis)
{ /* StormMesa works in reverse of traditional pixel formats,
  so rather adding things with values we setup everything to be
  turned off by default */
	GLPixelAttributes_Add(pThis, AMA_RGBMode);
	GLPixelAttributes_Add(pThis, FALSE);
	GLPixelAttributes_Add(pThis, AMA_NoDepth);
	GLPixelAttributes_Add(pThis, TRUE);
	GLPixelAttributes_Add(pThis, AMA_NoStencil);
	GLPixelAttributes_Add(pThis, TRUE);
	GLPixelAttributes_Add(pThis, AMA_NoAccum);
	GLPixelAttributes_Add(pThis, TRUE);
	GLPixelAttributes_Add(pThis, AMA_Fast);
	GLPixelAttributes_Add(pThis, TRUE);
	GLPixelAttributes_Add(pThis, AMA_AlphaFlag);
	GLPixelAttributes_Add(pThis, TRUE); /* FALSE by default */
}
/*-----------------------------------------------------------------------------
						GLCONTEXT
-----------------------------------------------------------------------------*/

int GLContext_CreateClone(GLCONTEXT* pThis, GLCONTEXTHANDLE clone,
	GLDRAWABLEHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return FALSE; } /* StormMesa doesn't support shared display lists */
int GLContext_Create(GLCONTEXT* pThis, GLDRAWABLEHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{
	GLPIXELATTRIBUTES realpa;
	struct TagItem* attribs;
	if(pixelattribs)
		realpa = *pixelattribs;
	else
		GLPixelAttributes_Create(&realpa);
	if(drawable)
	{
		GLPixelAttributes_Add(&realpa, AMA_Window);
		GLPixelAttributes_Add(&realpa, (unsigned long)drawable);
		GLPixelAttributes_Add(&realpa, AMA_Left);
		GLPixelAttributes_Add(&realpa, 0);
		GLPixelAttributes_Add(&realpa, AMA_Bottom);
		GLPixelAttributes_Add(&realpa, 0);
		GLPixelAttributes_Add(&realpa, AMA_Width);
		GLPixelAttributes_Add(&realpa,
			drawable->Width - drawable->BorderLeft-drawable->BorderRight);
		GLPixelAttributes_Add(&realpa, AMA_Height);
		GLPixelAttributes_Add(&realpa,
			drawable->Height - drawable->BorderBottom-drawable->BorderTop);
		GLPixelAttributes_Add(&realpa, AMA_DirectRender);
		GLPixelAttributes_Add(&realpa, TRUE);
	}
	else
	{
		GLPixelAttributes_Add(&realpa, AMA_Fullscreen);
		GLPixelAttributes_Add(&realpa, TRUE);
	}
	GLPixelAttributes_Add(&realpa, TAG_DONE);
	GLPixelAttributes_Add(&realpa, TAG_DONE); /* To make TagItem size */
	attribs = (struct TagItem*)GLPixelAttributes_Get(&realpa);

	pThis->mhContext = AmigaMesaCreateContext(attribs);
	return pThis->mhContext != NULL;
}
void GLContext_Destroy(GLCONTEXT* pThis)
{
	AmigaMesaDestroyContext(pThis->mhContext);
}
int GLContext_SwapBuffers(GLCONTEXT* pThis)
{
	AmigaMesaSwapBuffers(pThis->mhContext);
	return TRUE;
}
int GLContext_SetCurrent(GLCONTEXT* pThis)
{
	AmigaMesaMakeCurrent(pThis->mhContext, pThis->mhContext->buffer);
	return TRUE;
}
int GLContext_SetCurrentHandle(GLCONTEXT* pThis, GLCONTEXTHANDLE handle)
{ return 0; }
GLCONTEXTHANDLE GLContext_GetCurrent(GLCONTEXT* pThis)
{ return NULL; }

int GLContext_SetDrawable(GLCONTEXT* pThis, GLDRAWABLEHANDLE drawable)
{
	GLPIXELATTRIBUTES realpa;
	struct TagItem* attribs;
	GLPixelAttributes_Create(&realpa);
	if(drawable)
	{
		GLPixelAttributes_Add(&realpa, AMA_Window);
		GLPixelAttributes_Add(&realpa, (unsigned long)drawable);
		GLPixelAttributes_Add(&realpa, AMA_Left);
		GLPixelAttributes_Add(&realpa, 0);
		GLPixelAttributes_Add(&realpa, AMA_Bottom);
		GLPixelAttributes_Add(&realpa, 0);
		GLPixelAttributes_Add(&realpa, AMA_Width);
		GLPixelAttributes_Add(&realpa,
			drawable->Width - drawable->BorderLeft-drawable->BorderRight);
		GLPixelAttributes_Add(&realpa, AMA_Height);
		GLPixelAttributes_Add(&realpa,
			drawable->Height - drawable->BorderBottom-drawable->BorderTop);
		GLPixelAttributes_Add(&realpa, AMA_DirectRender);
		GLPixelAttributes_Add(&realpa, TRUE);
	}
	else
	{
		GLPixelAttributes_Add(&realpa, AMA_Fullscreen);
		GLPixelAttributes_Add(&realpa, TRUE);
	}
	GLPixelAttributes_Add(&realpa, TAG_DONE);
	GLPixelAttributes_Add(&realpa, TAG_DONE); /* To make TagItem size */
	attribs = (struct TagItem*)GLPixelAttributes_Get(&realpa);

	AmigaMesaSetRast(pThis->mhContext, attribs);
	return TRUE;
}
GLWINDOWHANDLE GLContext_GetWindowHandle(GLCONTEXT* pThis)
{ return NULL; }
#ifdef __cplusplus
	} /* end extern "C" */
#endif
/*=============================================================================
	 #     #    #     #####  ### #     # ####### #######  #####  #     #
	 ##   ##   # #   #     #  #  ##    #    #    #     # #     # #     #
	 # # # #  #   #  #        #  # #   #    #    #     # #       #     #
	 #  #  # #     # #        #  #  #  #    #    #     #  #####  #######
	 #     # ####### #        #  #   # #    #    #     #       # #     #
	 #     # #     # #     #  #  #    ##    #    #     # #     # #     #
	 #     # #     #  #####  ### #     #    #    #######  #####  #     #

							#     #####  #
						   # #   #     # #
						  #   #  #       #
						 #     # #  #### #
						 ####### #     # #
						 #     # #     # #
						 #     #  #####  #######
=============================================================================*/
#elif defined(GLMAC) || (defined(__MACH__) && !defined(__OBJC__))
#if defined(__MACH__)
	#include <AGL/agl.h>
	#include <Carbon/Carbon.h>
#else
	#include "agl.h"
	#include "MacWindows.h"
#endif
#if !defined(AGL_SAMPLE_BUFFERS_ARB) /* earlier headers don't have ARB */
	#define AGL_SAMPLE_BUFFERS_ARB    55
	#define AGL_SAMPLES_ARB	          56
#endif
#ifdef __cplusplus
	extern "C" {
#endif
/*------------------ [Utilities] --------------------------------------*/
int GLIsExtensionSupported(const char *extension)
{
	const char* extensions;
	struct __AGLPixelFormatRec* fmt = aglChoosePixelFormat(NULL, 0, NULL);
	struct __AGLContextRec* ctx = aglCreateContext(fmt, NULL);
	struct __AGLContextRec* oldctx;
	if (!ctx)
		return false;
	oldctx = aglGetCurrentContext();
	aglSetCurrentContext(ctx);
	extensions = (const char*)glGetString(GL_EXTENSIONS); /* const GLubyte* */
	aglSetCurrentContext(oldctx);
	aglDestroyPixelFormat(fmt);
	aglDestroyContext(ctx);
	return GLContextFindExtension(extensions, extension);
}

int GLIsAGLMultiSampleAvailable()
{
	return GLIsExtensionSupported("GL_ARB_multisample");
}
/*------------------ GLCONTEXTATTRIBUTES ------------------------------------*/
void GLContextAttributes_CoreProfile(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_MajorVersion(GLCONTEXTATTRIBUTES* pThis, int val)
{ }
void GLContextAttributes_MinorVersion(GLCONTEXTATTRIBUTES* pThis, int val)
{ }
void GLContextAttributes_CompatibilityProfile(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_ForwardCompatible(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_ES2(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_DebugCtx(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_Robust(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_NoResetNotify(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_LoseOnReset(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_ResetIsolation(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_ReleaseFlush(GLCONTEXTATTRIBUTES* pThis, int val)
{ }
void GLContextAttributes_PlatformDefaults(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_EndList(GLCONTEXTATTRIBUTES* pThis)
{ }
/*------------------ GLPIXELATTRIBUTES --------------------------------------*/
void GLPixelAttributes_RGBA(GLPIXELATTRIBUTES* pThis)
{
	GLPixelAttributes_Add(pThis, AGL_RGBA);
}

void GLPixelAttributes_BufferSize(GLPIXELATTRIBUTES* pThis, int val)
{
	GLPixelAttributes_Add(pThis, AGL_BUFFER_SIZE);
	GLPixelAttributes_Add(pThis, val);
}

void GLPixelAttributes_Level(GLPIXELATTRIBUTES* pThis, int val)
{
	GLPixelAttributes_Add(pThis, AGL_LEVEL);
	GLPixelAttributes_Add(pThis, val);
}

void GLPixelAttributes_DoubleBuffer(GLPIXELATTRIBUTES* pThis)
{
	GLPixelAttributes_Add(pThis, AGL_DOUBLEBUFFER);
}

void GLPixelAttributes_Stereo(GLPIXELATTRIBUTES* pThis)
{
	GLPixelAttributes_Add(pThis, AGL_STEREO);
}

void GLPixelAttributes_AuxBuffers(GLPIXELATTRIBUTES* pThis, int val)
{
	GLPixelAttributes_Add(pThis, AGL_AUX_BUFFERS);
	GLPixelAttributes_Add(pThis, val);
}

void GLPixelAttributes_MinRGBA(GLPIXELATTRIBUTES* pThis,
	int red, int green, int blue, int alpha)
{
	if (red >= 0)
	{
		GLPixelAttributes_Add(pThis, AGL_RED_SIZE);
		GLPixelAttributes_Add(pThis, red);
	}
	if (green >= 0)
	{
		GLPixelAttributes_Add(pThis, AGL_GREEN_SIZE);
		GLPixelAttributes_Add(pThis, green);
	}
	if (blue >= 0)
	{
		GLPixelAttributes_Add(pThis, AGL_BLUE_SIZE);
		GLPixelAttributes_Add(pThis, blue);
	}
	if (alpha >= 0)
	{
		GLPixelAttributes_Add(pThis, AGL_ALPHA_SIZE);
		GLPixelAttributes_Add(pThis, alpha);
	}
}

void GLPixelAttributes_Depth(GLPIXELATTRIBUTES* pThis, int val)
{
	GLPixelAttributes_Add(pThis, AGL_DEPTH_SIZE);
	GLPixelAttributes_Add(pThis, val);
}

void GLPixelAttributes_Stencil(GLPIXELATTRIBUTES* pThis, int val)
{
	GLPixelAttributes_Add(pThis, AGL_STENCIL_SIZE);
	GLPixelAttributes_Add(pThis, val);
}

void GLPixelAttributes_MinAcumRGBA(GLPIXELATTRIBUTES* pThis,
	int red, int green, int blue, int alpha)
{
	if (red >= 0)
	{
		GLPixelAttributes_Add(pThis, AGL_ACCUM_RED_SIZE);
		GLPixelAttributes_Add(pThis, red);
	}
	if (green >= 0)
	{
		GLPixelAttributes_Add(pThis, AGL_ACCUM_GREEN_SIZE);
		GLPixelAttributes_Add(pThis, green);
	}
	if (blue >= 0)
	{
		GLPixelAttributes_Add(pThis, AGL_ACCUM_BLUE_SIZE);
		GLPixelAttributes_Add(pThis, blue);
	}
	if (alpha >= 0)
	{
		GLPixelAttributes_Add(pThis, AGL_ACCUM_ALPHA_SIZE);
		GLPixelAttributes_Add(pThis, alpha);
	}
}

void GLPixelAttributes_SampleBuffers(GLPIXELATTRIBUTES* pThis, int val)
{
	GLPixelAttributes_Add(pThis, AGL_SAMPLE_BUFFERS_ARB);
	if (val >= 0) /* prohibit software fallback */
		GLPixelAttributes_Add(pThis, AGL_NO_RECOVERY);
}

void GLPixelAttributes_Samplers(GLPIXELATTRIBUTES* pThis, int val)
{
	GLPixelAttributes_Add(pThis, AGL_SAMPLES_ARB);
	GLPixelAttributes_Add(pThis, val);
}

void GLPixelAttributes_FrameBuffersRGB(GLPIXELATTRIBUTES* pThis)
{
	/* AGL does not natively support sRGB framebuffers */
}

void GLPixelAttributes_EndList(GLPIXELATTRIBUTES* pThis)
{
	GLPixelAttributes_Add(pThis, AGL_NONE);
}

void GLPixelAttributes_PlatformDefaults(GLPIXELATTRIBUTES* pThis)
{
	/* GLX _SIZE tag compatability */
	GLPixelAttributes_Add(pThis, AGL_MINIMUM_POLICY);
	/* Depth is liattribest depth buffer for agl */
	GLPixelAttributes_Depth(pThis, 1);
	GLPixelAttributes_MinRGBA(pThis, 1, 1, 1, 0);
}
/*-----------------------------------------------------------------------------
						GLCONTEXT
-----------------------------------------------------------------------------*/
const char* GLContext_GetNativeExtensions(GLCONTEXT* pThis)
{
	return NULL;
}
AGLDrawable GLContextMacWindowPort(GLWINDOWHANDLE window)
{
#if defined(__MACH__) || (defined(TARGET_API_MAC_CARBON) && TARGET_API_MAC_CARBON)
	return (AGLDrawable)GetWindowPort((WindowRef)window);
#else
	return (AGLDrawable)window;
#endif
}
int GLContextMacCreateFullscreenWindow(GLCONTEXT* pThis)
{
	Rect bounds = (**GetMainDevice()).gdRect;
	pThis->mhWindow = (GLWINDOWHANDLE)NewCWindow(NULL, &bounds,
		(ConstStr255Param)"", 1, plainDBox, (WindowRef)-1L, 0, 0);
	return pThis->mhWindow != NULL;
}
void GLContextMacDestroyFullscreenWindow(GLCONTEXT* pThis)
{
	if(pThis->mhWindow)
		DisposeWindow((WindowRef)pThis->mhWindow);
	pThis->mhWindow = NULL;
}
int GLContext_CreateClone(GLCONTEXT* pThis, GLCONTEXTHANDLE clone,
	GLDRAWABLEHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ /* https://stackoverflow.com/questions/1400654/
  how-do-i-put-my-opengl-app-into-fullscreen-mode */
	attrib_t* aglattribs = pixelattribs ?
		GLPixelAttributes_Get(pixelattribs) : NULL;
	attrib_t fullscreenattribs[GLCONTEXT_MAX_PIXELATTRIBUTES + 2];
	AGLPixelFormat pf = NULL;
	GDHandle maindevice;
	pThis->mhWindow = NULL;
	if(!drawable)
	{
		int count = 0;
		if(pixelattribs)
		{
			count = GLPixelAttributes_GetCount(pixelattribs);
			if(count > 0 && aglattribs[count - 1] == AGL_NONE)
				--count;
			memcpy(fullscreenattribs, aglattribs, count * sizeof(attrib_t));
		}
		fullscreenattribs[count++] = AGL_FULLSCREEN;
		fullscreenattribs[count] = AGL_NONE;
		maindevice = GetMainDevice();
		pf = aglChoosePixelFormat(&maindevice, 1, fullscreenattribs);
	}
	if(!pf)
		pf = aglChoosePixelFormat(NULL, 0, aglattribs);
	if(!pf)
	{
		pThis->mhContext = NULL;
		return 0;
	}

	/* 2nd iattrib is another context for sharing display lists */
	pThis->mhContext = aglCreateContext(pf, clone);
	aglDestroyPixelFormat(pf); /* free the pixel format memory */
	if(!pThis->mhContext)
		return 0;
	if(!GLContext_SetDrawable(pThis, drawable))
	{
		GLContext_Destroy(pThis);
		return 0;
	}
	return 1;
}
int GLContext_Create(GLCONTEXT* pThis, GLDRAWABLEHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return GLContext_CreateClone(pThis, NULL, drawable,
	contextattribs, pixelattribs); }
void GLContext_Destroy(GLCONTEXT* pThis)
{
	aglDestroyContext(pThis->mhContext);
	GLContextMacDestroyFullscreenWindow(pThis);
}
int GLContext_SwapBuffers(GLCONTEXT* pThis)
{
	aglSwapBuffers(pThis->mhContext);
	return true;
}
int GLContext_SetCurrent(GLCONTEXT* pThis)
{
	return aglSetCurrentContext(pThis->mhContext);
}
int GLContext_SetCurrentHandle(GLCONTEXT* pThis, GLCONTEXTHANDLE handle)
{ return aglSetCurrentContext(handle); }
/*
void GLContext_SetColor(GLCONTEXT* pThis, unsigned long color)
{
	float r = (float)((color & 0x000000FF)) / 255.0f;
	float g = (float)((color & 0x0000FF00) >> 8) / 255.0f;
	float b = (float)((color & 0x00FF0000) >> 16) / 255.0f;
	glColor3f(r, g, b);
}*/
void GLContext_SetViewport(GLCONTEXT* pThis, Rect* r)
{
	/*
		Needed every time the window changes position/size

		aglUpdateContext
		+ aglSetCurrentContext
		+ aglSetInteger(pThis->mhContext, AGL_BUFFER_RECT, parms);

		r->left = window position within parent
		r->top = window position within parent
		r->right = r->left + client width
		r->bottom = r->top + client height

		Client carbon position is part of
		OSErr err =
		GetWindowBounds((WindowRef)drawable, kWindowContentRgn, r);

		If window isn't shown rect needs to moved
		off screen
	*/
	GLint parms[4];
	parms[0] = r->left;
	parms[1] = r->top;
	parms[2] = r->right - r->left;
	parms[3] = r->bottom - r->top;
	aglSetInteger(pThis->mhContext, AGL_BUFFER_RECT, parms);
	aglEnable(pThis->mhContext, AGL_BUFFER_RECT);
	aglUpdateContext(pThis->mhContext);
}
int GLContext_SetDrawable(GLCONTEXT* pThis, GLDRAWABLEHANDLE drawable)
{
	if(drawable)
	{
		if(!aglSetDrawable(pThis->mhContext, GLContextMacWindowPort(drawable)))
			return 0;
		GLContextMacDestroyFullscreenWindow(pThis);
		return 1;
	}
	if(pThis->mhWindow)
		return 1; /* wants fullscreen and already fullscreen */
	if(aglSetFullScreen(pThis->mhContext, 0, 0, 0, 0))
		return 1;
	if(!GLContextMacCreateFullscreenWindow(pThis))
		return 0;
	if(!aglSetDrawable(pThis->mhContext,
			GLContextMacWindowPort(pThis->mhWindow)))
	{
		GLContextMacDestroyFullscreenWindow(pThis);
		return 0;
	}
	return 1;
}
GLWINDOWHANDLE GLContext_GetWindowHandle(GLCONTEXT* pThis)
{ return pThis->mhWindow; }
GLCONTEXTHANDLE GLContext_GetCurrent(GLCONTEXT* pThis)
{
	return aglGetCurrentContext();
}
int GLContext_SetSwapInterval(GLCONTEXT* pThis, int ininterval)
{
	GLint interval = ininterval;
	aglSetInteger(pThis->mhContext, AGL_SWAP_INTERVAL, &interval);
	return aglGetError() == AGL_NO_ERROR;
}
int GLContext_GetSwapInterval(GLCONTEXT* pThis)
{
	GLint interval = 0;
	aglGetInteger(pThis->mhContext, AGL_SWAP_INTERVAL, &interval);
	return interval;
}
GLContextEmptyProc GLContext_GetProc(GLCONTEXT* pThis, const char* proc)
{
	return (GLContextEmptyProc)NULL;
}
void GLContext_ResizeDrawable(GLCONTEXT* pThis, int width, int height)
{
	(void)(pThis);(void)(width);(void)(height);
}
int GLContext_CreateCloneWithWindow(GLCONTEXT* pThis, GLCONTEXTHANDLE clone,
	GLWINDOWHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return GLContext_CreateClone(pThis, clone, drawable,
		contextattribs, pixelattribs); }
int GLContext_CreateWithWindow(GLCONTEXT* pThis, GLWINDOWHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return GLContext_Create(pThis, drawable, contextattribs, pixelattribs); }

#ifdef __cplusplus
	} /* end extern "C" */
#endif
/*=============================================================================
			 #     #    #     #####     #######  #####  #
			 ##   ##   # #   #     #    #       #     # #
			 # # # #  #   #  #          #       #       #
			 #  #  # #     # #          #####   #  #### #
			 #     # ####### #          #       #     # #
			 #     # #     # #     #    #       #     # #
			 #     # #     #  #####     #######  #####  #######

		 #     #  #####  ####### ######  ####### #     #  #####  #
		 ##    # #     # #     # #     # #       ##    # #     # #
		 # #   # #       #     # #     # #       # #   # #       #
		 #  #  #  #####  #     # ######  #####   #  #  # #  #### #
		 #   # #       # #     # #       #       #   # # #     # #
		 #    ## #     # #     # #       #       #    ## #     # #
		 #     #  #####  ####### #       ####### #     #  #####  #######
=============================================================================*/
#elif defined(__MACH__)
/*
	https://developer.apple.com/library/mac/documentation/Cocoa/Reference/
	ApplicationKit/Classes/NSOpenGLPixelFormat_Class/index.html
*/
#define GLOpenGLPFAAllRenderers				1
#define GLOpenGLPFATripleBuffer				3
#define GLOpenGLPFADoubleBuffer				5
#define GLOpenGLPFAStereo					6
#define GLOpenGLPFAAuxBuffers				7
#define GLOpenGLPFAColorSize				8
#define GLOpenGLPFAAlphaSize				11
#define GLOpenGLPFADepthSize				12
#define GLOpenGLPFAStencilSize				13
#define GLOpenGLPFAAccumSize				14
#define GLOpenGLPFAMinimumPolicy			51
#define GLOpenGLPFAMaximumPolicy			52
#define GLOpenGLPFAOffScreen				53
#define GLOpenGLPFAFullScreen				54
#define GLOpenGLPFASampleBuffers			55
#define GLOpenGLPFASamples					56
#define GLOpenGLPFAAuxDepthStencil			57
#define GLOpenGLPFAColorFloat				58
#define GLOpenGLPFAMultisample				59
#define GLOpenGLPFASupersample				60
#define GLOpenGLPFASampleAlpha				61
#define GLOpenGLPFARendererID				70
#define GLOpenGLPFASingleRenderer			71
#define GLOpenGLPFANoRecovery				72
#define GLOpenGLPFAAccelerated				73
#define GLOpenGLPFAClosestPolicy			74
#define GLOpenGLPFARobust					75
#define GLOpenGLPFABackingStore				76
#define GLOpenGLPFAMPSafe					78
#define GLOpenGLPFAWindow					80
#define GLOpenGLPFAMultiScreen				81
#define GLOpenGLPFACompliant				83
#define GLOpenGLPFAScreenMask				84
#define GLOpenGLPFAPixelBuffer				90
#define GLOpenGLPFARemotePixelBuffer		91
#define GLOpenGLPFAAllowOfflineRenderers	96
#define GLOpenGLPFAAcceleratedCompute		97
#define GLOpenGLPFAOpenGLProfile			99
#define GLOpenGLPFAVirtualScreenCount		128
#define GLOpenGLProfileVersionLegacy		0x1000
#define GLOpenGLProfileVersion3_2Core		0x3200
/*
	GLXXX platform specific utilities. Starting with OSX
*/
#if defined(GLMACOSXCOCOA)
#import <Cocoa/Cocoa.h>
GLCONTEXTHANDLE GLCreateContext(GLPIXELFORMATHANDLE pixelFormat,
	GLCONTEXTHANDLE shareContext)
{ return [[NSOpenGLContext alloc]
		initWithFormat:pixelFormat shareContext: shareContext]; }
void GLDestroyContext(GLCONTEXTHANDLE context)
{ [context release]; }
void GLSwapBuffers(GLCONTEXTHANDLE context)
{ [context flushBuffer]; }
GLCONTEXTHANDLE GLGetCurrentContext()
{ return [NSOpenGLContext currentContext]; }
int GLSetCurrentContext(GLCONTEXTHANDLE context)
{
	[context makeCurrentContext];
	return TRUE;
}
void GLDestroyPixelFormat(GLPIXELFORMATHANDLE pixelFormat)
{ [pixelFormat release]; }
GLPIXELFORMATHANDLE GLChoosePixelFormat(
	GLCONTEXTATTRIBUTES* ctxattrs, GLPIXELATTRIBUTES* pixelattrs)
{ /* OSX uses just one attribute list */
	NSOpenGLPixelFormatAttribute data[
		GLCONTEXT_MAX_CONTEXTATTRIBUTES + GLCONTEXT_MAX_PIXELATTRIBUTES];
	int ctxattrcount = ctxattrs ?
		GLContextAttributes_GetCount(ctxattrs) : 0;
	int pixelattrcount = pixelattrs ?
		GLPixelAttributes_GetCount(pixelattrs) : 0;
	if(ctxattrcount)
		memcpy(data, GLContextAttributes_Get(ctxattrs),
			(ctxattrcount - 1) * sizeof(NSOpenGLPixelFormatAttribute));
	if(pixelattrcount)
		memcpy(data, GLPixelAttributes_Get(pixelattrs),
			(pixelattrcount - 1) * sizeof(NSOpenGLPixelFormatAttribute));
	data[ctxattrcount + pixelattrcount] = 0;
	return [[NSOpenGLPixelFormat alloc] initWithAttributes:data];
}
#if MAC_OS_X_VERSION_MAX_ALLOWED < 101200
    #define NSWindowStyleMaskBorderless NSBorderlessWindowMask
#endif
GLWINDOWHANDLE GLCreateFullScreenWindow(GLCONTEXTHANDLE context)
{
	NSView* innerview;
	NSRect screenRect = [[NSScreen mainScreen] frame];
	NSWindow* window = [[NSWindow alloc] initWithContentRect:screenRect
		styleMask:NSWindowStyleMaskBorderless
		backing:NSBackingStoreBuffered defer:NO];
	[window setLevel:NSMainMenuWindowLevel + 1];
	[window setOpaque:YES];
	[window setHidesOnDeactivate:NO];

	innerview = [[NSView alloc] initWithFrame:screenRect];
	[window setContentView:innerview];
	[window makeKeyAndOrderFront:nil];
	return window;
}
GLDRAWABLEHANDLE GLGetWindowDrawable(GLWINDOWHANDLE window)
{
	return window.contentView;
}
#define GLDestroyFullScreenWindow(window) \
{ \
	window.contentView = nil; \
	window = nil; \
}
int GLSetDrawable(GLCONTEXTHANDLE context, GLDRAWABLEHANDLE drawable)
{
	[context setView:drawable];
	[context update];
	return TRUE;
}

int GLSetSwapInterval(GLCONTEXTHANDLE context, int interval)
{
	[context setValues:&interval forParameter:NSOpenGLCPSwapInterval];
	return TRUE;
}

int GLGetSwapInterval(GLCONTEXTHANDLE context)
{
	GLint interval = 0;
	[context getValues:&interval forParameter:NSOpenGLCPSwapInterval];
	return interval;
}
/*
						IPHONE
*/
#else /* IOS */
#import <GLKit/GLKit.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/EAGLDrawable.h>
#import <QuartzCore/QuartzCore.h>
GLCONTEXTHANDLE GLCreateContext(GLPIXELFORMATHANDLE pixelFormat,
	GLCONTEXTHANDLE shareContext)
{ /* IOS doesn't use pixel formats */
	return [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES1];
}
void GLDestroyContext(GLCONTEXTHANDLE context)
{ [context release]; }
void GLSwapBuffers(GLCONTEXTHANDLE context)
{ [context presentRenderbuffer:GL_RENDERBUFFER]; }
GLCONTEXTHANDLE GLGetCurrentContext()
{ return [EAGLContext currentContext]; }
int GLSetCurrentContext(GLCONTEXTHANDLE context)
{
	[EAGLContext setCurrentContext:context];
	return true;
}
void GLDestroyPixelFormat(GLPIXELFORMATHANDLE pixelFormat)
{ }
GLPIXELFORMATHANDLE GLChoosePixelFormat(
	GLCONTEXTATTRIBUTES* ctxattrs, GLPIXELATTRIBUTES* pixelattrs)
{ return NULL; }
GLWINDOWHANDLE GLCreateFullScreenWindow(GLCONTEXTHANDLE context)
{
	UIWindow* window = [[UIWindow alloc]
		initWithFrame:[[UIScreen mainScreen] bounds]];
	window.rootViewController = [[GLKViewController alloc] init];
	[window.rootViewController.view setContext:context];
	[window.rootViewController.view setDrawableDepthFormat:
		GLKViewDrawableDepthFormat24];
	[window makeKeyAndVisible];
	return window;
}
GLDRAWABLEHANDLE GLGetWindowDrawable(GLWINDOWHANDLE window)
{
	return window.rootViewController.view;
}
#define GLDestroyFullScreenWindow(window) \
{ \
	window.rootViewController = nil; \
	window = nil; \
}
int GLSetDrawable(GLCONTEXTHANDLE context, GLDRAWABLEHANDLE drawable)
{
	[drawable setContext:context];
	[drawable bindDrawable];
	return true;
}
/*
*	EAGLContext doesn't support swap intervals. If you wanted the equivelent
*	of a swap interval of 1 you'd do:
*   CADisplayLink* displayLink = [CADisplayLink displayLinkWithTarget:self
		selector:@selector(render)];
	displayLink.preferredFramesPerSecond = 60; // Set to display refresh rate
	[displayLink addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
	- (void)render
	{
		//drawing code ...
		GLSwapBuffers()
	}
*/
int GLSetSwapInterval(GLCONTEXTHANDLE context, int interval)
{ return FALSE; }
int GLGetSwapInterval(GLCONTEXTHANDLE context)
{ return FALSE;}

#endif /* IOS/OSX */
#ifdef __cplusplus
	extern "C" {
#endif
/* --------------- GLContextAttributes */
void GLContextAttributes_CoreProfile(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_Add(pThis, GLOpenGLPFAOpenGLProfile);
	GLContextAttributes_Add(pThis, GLOpenGLProfileVersion3_2Core);
}
void GLContextAttributes_MajorVersion(GLCONTEXTATTRIBUTES* pThis, int val)
{ (void)(val); }
void GLContextAttributes_MinorVersion(GLCONTEXTATTRIBUTES* pThis, int val)
{ (void)(val); }
void GLContextAttributes_CompatibilityProfile(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_Add(pThis, GLOpenGLPFAOpenGLProfile);
	GLContextAttributes_Add(pThis, GLOpenGLProfileVersionLegacy);
}
void GLContextAttributes_ForwardCompatible(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_ES2(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_DebugCtx(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_Robust(GLCONTEXTATTRIBUTES* pThis)
{ /* Can use NSOpenGLPFARobust, deprecated in 10.5 */ }
void GLContextAttributes_NoResetNotify(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_LoseOnReset(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_ResetIsolation(GLCONTEXTATTRIBUTES* pThis)
{}
void GLContextAttributes_ReleaseFlush(GLCONTEXTATTRIBUTES* pThis, int val)
{ (void)(val); }
void GLContextAttributes_PlatformDefaults(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_EndList(GLCONTEXTATTRIBUTES* pThis)
{ GLContextAttributes_Add(pThis, 0); }

/* --------------------------- GLPixelAttributes */
void GLPixelAttributes_RGBA(GLPIXELATTRIBUTES* pThis)
{ }
void GLPixelAttributes_BufferSize(GLPIXELATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLPixelAttributes_Add(pThis, GLOpenGLPFAColorSize);
		GLPixelAttributes_Add(pThis, val);
	}
}
void GLPixelAttributes_Level(GLPIXELATTRIBUTES* pThis, int val)
{ (void)(val); }
void GLPixelAttributes_DoubleBuffer(GLPIXELATTRIBUTES* pThis)
{ GLPixelAttributes_Add(pThis, GLOpenGLPFADoubleBuffer); }
void GLPixelAttributes_Stereo(GLPIXELATTRIBUTES* pThis)
{ GLPixelAttributes_Add(pThis, GLOpenGLPFAStereo); }
void GLPixelAttributes_AuxBuffers(GLPIXELATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLPixelAttributes_Add(pThis, GLOpenGLPFAAuxBuffers);
		GLPixelAttributes_Add(pThis, val);
	}
}
void GLPixelAttributes_MinRGBA(GLPIXELATTRIBUTES* pThis,
	int mRed, int mGreen, int mBlue, int mAlpha)
{
	/* NSOpenGL (NSOpenGLPixelFormat) doesn't allow for
	individual RGB component minimums, instead we do something
	similar by setting the highest minimum of RGB with GLOpenGLPFAColorSize */
	int minColorBits = 0;
	if (mRed > minColorBits)
		minColorBits = mRed;
	if (mGreen > minColorBits)
		minColorBits = mGreen;
	if (mBlue > minColorBits)
		minColorBits = mBlue;
	minColorBits *= 4; /* the value includes alpha calcs but not alpha min */
	if (minColorBits > 0)
	{
		GLPixelAttributes_Add(pThis, GLOpenGLPFAColorSize);
		GLPixelAttributes_Add(pThis, minColorBits);
	}
	if (mAlpha >= 0)
	{
		GLPixelAttributes_Add(pThis, GLOpenGLPFAAlphaSize);
		GLPixelAttributes_Add(pThis, mAlpha);
	}
}
void GLPixelAttributes_Depth(GLPIXELATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLPixelAttributes_Add(pThis, GLOpenGLPFADepthSize);
		GLPixelAttributes_Add(pThis, val);
	}
}
void GLPixelAttributes_Stencil(GLPIXELATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLPixelAttributes_Add(pThis, GLOpenGLPFAStencilSize);
		GLPixelAttributes_Add(pThis, val);
	}
}
void GLPixelAttributes_MinAcumRGBA(GLPIXELATTRIBUTES* pThis,
	int mRed, int mGreen, int mBlue, int mAlpha)
{ /* similar to GLPixelAttributes_MinRGBA() */
	int minAcumBits = 0;
	if ( mRed > minAcumBits )
		minAcumBits = mRed;
	if ( mGreen > minAcumBits )
		minAcumBits = mGreen;
	if ( mBlue > minAcumBits )
		minAcumBits = mBlue;
	if ( mAlpha > minAcumBits )
		minAcumBits = mAlpha;
	minAcumBits *= 4;
	if ( minAcumBits >= 0 )
	{
		GLPixelAttributes_Add(pThis, GLOpenGLPFAAccumSize);
		GLPixelAttributes_Add(pThis, minAcumBits);
	}
}
void GLPixelAttributes_SampleBuffers(GLPIXELATTRIBUTES* pThis, int val)
{
	if ( val >= 0 )
	{
		GLPixelAttributes_Add(pThis, GLOpenGLPFASampleBuffers);
		GLPixelAttributes_Add(pThis, val);
		/* GLOpenGLPFANoRecovery == no software fallback similar to AGL */
		GLPixelAttributes_Add(pThis, GLOpenGLPFANoRecovery);
	}
}

void GLPixelAttributes_Samplers(GLPIXELATTRIBUTES* pThis, int val)
{
	if ( val >= 0 )
	{
		GLPixelAttributes_Add(pThis, GLOpenGLPFASamples);
		GLPixelAttributes_Add(pThis, val);
	}
}
void GLPixelAttributes_FrameBuffersRGB(GLPIXELATTRIBUTES* pThis)
{ }
void GLPixelAttributes_EndList(GLPIXELATTRIBUTES* pThis)
{ GLPixelAttributes_Add(pThis, 0); }
void GLPixelAttributes_PlatformDefaults(GLPIXELATTRIBUTES* pThis)
{
	GLPIXELFORMATHANDLE testFormat;
	GLCONTEXTATTRIBUTES ctxattrs; GLPIXELATTRIBUTES pixelattrs;
	/* GLOpenGLPFAMinimumPolicy == _SIZE tags are minimum sizes */
	GLPixelAttributes_Add(pThis, GLOpenGLPFAMinimumPolicy);
	/*
		Test if the native GL supports hardware acceleration. Naturally, it
		should be added if supported. However, despite Apple documentation
		explicitly stating:

		"If present, this attribute indicates that only hardware-accelerated
		renderers are considered. If not present, accelerated renderers are
		still preferred."

		https://developer.apple.com/documentation/appkit/
		1436213-opengl_pixel_format_attributes/nsopenglpfaaccelerated

		In practice hardware acceleration is not always used if it is not
		specified, so it is explicitly specify here unless it is not supported.
	*/
	GLPixelAttributes_Create(&pixelattrs);
	GLContextAttributes_Create(&ctxattrs);
	GLPixelAttributes_Add(&pixelattrs, GLOpenGLPFAAccelerated);
	GLPixelAttributes_EndList(&pixelattrs);
	testFormat = GLChoosePixelFormat(&ctxattrs, &pixelattrs);
	if (testFormat) /* hardware acceleration supported? */
	{
		GLPixelAttributes_Add(pThis, GLOpenGLPFAAccelerated);
		GLDestroyPixelFormat(testFormat);
	}
}
/*=================================== GLContext =============================*/
int GLContext_CreateClone(GLCONTEXT* pThis, GLCONTEXTHANDLE clone,
	GLDRAWABLEHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{
	GLPIXELFORMATHANDLE pf = GLChoosePixelFormat(contextattribs, pixelattribs);
	if (pf)
	{
		pThis->mhContext = GLCreateContext(pf, clone);
		if(pThis->mhContext)
		{
			if(!drawable)
			{ /* full screen */
				pThis->mhWindow =
					GLCreateFullScreenWindow(pThis->mhContext);
				drawable = GLGetWindowDrawable(pThis->mhWindow);
			}
			else
				pThis->mhWindow = NULL;
			GLSetDrawable(pThis->mhContext, drawable);
		}
		GLDestroyPixelFormat(pf);
	}
	else
		pThis->mhContext = NULL;
	return pThis->mhContext != NULL;
}
int GLContext_Create(GLCONTEXT* pThis, GLDRAWABLEHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return GLContext_CreateClone(pThis, NULL, drawable,
	contextattribs, pixelattribs); }
void GLContext_Destroy(GLCONTEXT* pThis)
{
	GLDestroyContext(pThis->mhContext);
	if(pThis->mhWindow)
		GLDestroyFullScreenWindow(pThis->mhWindow);
}
int GLContext_SwapBuffers(GLCONTEXT* pThis)
{ GLSwapBuffers(pThis->mhContext); return TRUE; }
int GLContext_SetCurrent(GLCONTEXT* pThis)
{ return GLSetCurrentContext(pThis->mhContext); }
int GLContext_SetCurrentHandle(GLCONTEXT* pThis, GLCONTEXTHANDLE handle)
{ return GLSetCurrentContext(handle); }
int GLContext_SetDrawable(GLCONTEXT* pThis, GLDRAWABLEHANDLE drawable)
{
	if(!drawable)
	{ /* full screen */
		if(pThis->mhWindow)
			return 1; /* wants fullscreen and already fullscreen; done */
		pThis->mhWindow =
			GLCreateFullScreenWindow(pThis->mhContext);
		drawable = GLGetWindowDrawable(pThis->mhWindow);
	}
	else
	{
		if(pThis->mhWindow)
		{
			GLDestroyFullScreenWindow(pThis->mhWindow);
			pThis->mhWindow = NULL;
		}
	}
	if(pThis->mhContext == GLGetCurrentContext())
		GLSetCurrentContext(NULL);
	return GLSetDrawable(pThis->mhContext, drawable);
}
GLWINDOWHANDLE GLContext_GetWindowHandle(GLCONTEXT* pThis)
{ return pThis->mhWindow; }
GLCONTEXTHANDLE GLContext_GetCurrent(GLCONTEXT* pThis)
{ return GLGetCurrentContext(); }
int GLContext_SetSwapInterval(GLCONTEXT* pThis, int interval)
{ return GLSetSwapInterval(pThis->mhContext, interval); }
int GLContext_GetSwapInterval(GLCONTEXT* pThis)
{ return GLGetSwapInterval(pThis->mhContext); }
GLContextEmptyProc GLContext_GetProc(GLCONTEXT* pThis, const char* proc)
{ return (GLContextEmptyProc)NULL; }
GLContextEmptyProc GLContext_GetNativeProc(GLCONTEXT* pThis, const char* proc)
{ return (GLContextEmptyProc)NULL; }
void GLContext_ResizeDrawable(GLCONTEXT* pThis, int width, int height)
{ (void)(pThis);(void)(width);(void)(height); }
int GLContext_CreateCloneWithWindow(GLCONTEXT* pThis, GLCONTEXTHANDLE clone,
	GLWINDOWHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return GLContext_CreateClone(pThis, clone,
	GLGetWindowDrawable(drawable), contextattribs, pixelattribs); }
int GLContext_CreateWithWindow(GLCONTEXT* pThis, GLWINDOWHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return GLContext_Create(pThis,
	GLGetWindowDrawable(drawable), contextattribs, pixelattribs); }
#ifdef __cplusplus
	} /* end extern "C" */
#endif
/*=============================================================================
		 #     # #     # ### #     #    #######  #####  #
		 #     # ##    #  #   #   #     #       #     # #
		 #     # # #   #  #    # #      #       #       #
		 #     # #  #  #  #     #       #####   #  #### #
		 #     # #   # #  #    # #      #       #     # #
		 #     # #    ##  #   #   #     #       #     # #
		  #####  #     # ### #     #    #######  #####  #######

		Note that EGL rendering with X11 has no VSYNC on my machine
		so every call to eglSwapBuffers causes screen tear, thus a
		fullscreen wayland window is created if no drawable is specified.
*============================================================================*/
#elif defined(HAVE_EGL)
#if !defined(HAVE_QT) && (defined(QT_VERSION) || defined(WSQTOPT))
	#define HAVE_QT
#endif
#if defined(HAVE_QT) && defined(__cplusplus)
	#include <QWidget>
	#include <QScreen>
	#include <QGuiApplication>
	#include <QApplication>
	#include <QWindow>
	#include <qpa/qplatformnativeinterface.h>
	#include <X11/Xlib.h>
	#include <wayland-client.h>
	#include <wayland-egl.h>
#elif !defined(HAVE_QT)
	#include <gdk/gdk.h>
	#ifndef GDK_CHECK_VERSION
		#define GDK_CHECK_VERSION(major, minor, micro) \
			(GDK_MAJOR_VERSION > (major) || \
			(GDK_MAJOR_VERSION  == (major) \
				&& GDK_MINOR_VERSION > (minor)) || \
			(GDK_MAJOR_VERSION == (major) \
				&& GDK_MINOR_VERSION == (minor) \
				&& GDK_MICRO_VERSION >= (micro)))
	#endif
	#include <wayland-egl.h>
	#ifdef GDK_WINDOWING_WAYLAND
		#if GDK_CHECK_VERSION(4, 0, 0)
			#include <gdk/wayland/gdkwayland.h>
		#else
			#include <gdk/gdkwayland.h>
		#endif
	#endif
	#ifdef GDK_WINDOWING_X11
		#if GDK_CHECK_VERSION(4, 0, 0)
			#include <gdk/x11/gdkx.h>
		#else
			#include <gdk/gdkx.h>
		#endif
	#endif
	#include <gtk/gtk.h>
#else
	#include <X11/Xlib.h>
	#include <wayland-client.h>
	#include <wayland-egl.h>
	#include <stdlib.h>
	#include <stdio.h>
#endif

#include <EGL/egl.h>
#include <EGL/eglext.h>
/*---------------------- [Utilities] ---------------------*/
#define GLDISPLAYTYPE_NONE 0
#define GLDISPLAYTYPE_X11 1
#define GLDISPLAYTYPE_WAYLAND 2
typedef struct GLDISPLAYINFO
{
	union
	{
		void *dpy;
		struct _XDisplay* x11dpy;
		struct wl_display* waylanddpy;
	} dpy;
	int type;
} GLDISPLAYINFO;
#if defined(HAVE_QT) && defined(__cplusplus)
	void GLGetDisplayInfo(GLDISPLAYINFO* info)
	{
		QPlatformNativeInterface* platni =
			QGuiApplication::platformNativeInterface();
		QString platname = qApp->platformName();
		info->dpy.dpy = NULL; info->type = GLDISPLAYTYPE_NONE;
		if(platname.isEmpty() || platni == NULL)
		{
			int argc = 1; /* Use 1 ele to avoid QSettings warning */
			char* argv[] = { (char*)"wayland", NULL };
			qputenv("QT_QPA_PLATFORM", "wayland");
			new QApplication(argc, argv);
			platname = qApp->platformName();
			platni = QGuiApplication::platformNativeInterface();
		}

		if(platname.isEmpty() == false && platni != NULL)
		{
			if (platname == "wayland")
			{
				info->dpy.waylanddpy = (wl_display*)
					platni->nativeResourceForIntegration("display");
				info->type = GLDISPLAYTYPE_WAYLAND;
				return;
			}
			else if (platname == "xcb")
			{
				info->dpy.x11dpy = (Display*)
					platni->nativeResourceForIntegration("display");
				info->type = GLDISPLAYTYPE_X11;
				return;
			}
		}
		info->dpy.x11dpy = XOpenDisplay(NULL);
		info->type = GLDISPLAYTYPE_X11;
	}
	wl_compositor* GLGetWaylandCompositor()
	{ return (wl_compositor*) QGuiApplication::platformNativeInterface(
		)->nativeResourceForIntegration("compositor"); }
#else
	#if defined(GDK_CHECK_VERSION)
		#if GDK_CHECK_VERSION(3,22,25)
			int GLGDKIsTypeName(void* instance, const char* string)
			{
				const char* name = g_type_name(G_TYPE_FROM_INSTANCE(instance));
				return strncmp(string, name, strlen(string)) == 0;
			}
		#endif
	#endif /* GDK 3.22.25 */

	void GLGetDisplayInfo(GLDISPLAYINFO* info)
	{
		info->dpy.dpy = NULL; info->type = GLDISPLAYTYPE_NONE;
	#if defined(GDK_WINDOWING_WAYLAND) || defined(GDK_WINDOWING_X11)
		GdkDisplay* dpy = gdk_display_get_default();
		if(!dpy)
		{
#if !GTK_CHECK_VERSION (4, 0, 0)
			int argc = 0; char** argv = NULL;
#endif
#if !GTK_CHECK_VERSION (4, 0, 0)
			gdk_display_open(NULL);
			gtk_init_check(&argc, &argv);
#else
			gtk_init_check();
#endif
			dpy = gdk_display_get_default();
			if(!dpy)
				return;
		}
	#endif

	#ifdef GDK_WINDOWING_X11
		#if GDK_CHECK_VERSION(3,22,25)
			if (GLGDKIsTypeName(dpy, "GdkX11"))
		#endif
			{ /* GDK_DISPLAY() in < 3 */
				info->dpy.x11dpy = GDK_DISPLAY_XDISPLAY(dpy);
				info->type = GLDISPLAYTYPE_X11;
				return;
			}
	#endif
	#ifdef GDK_WINDOWING_WAYLAND
		if (GLGDKIsTypeName(dpy, "GdkWayland"))
		{
			info->dpy.waylanddpy = gdk_wayland_display_get_wl_display(dpy);
			info->type = GLDISPLAYTYPE_WAYLAND;
			return;
		}
	#endif
	}
	#ifdef GDK_WINDOWING_WAYLAND
		struct wl_compositor* GLGetWaylandCompositor()
		{
			return gdk_wayland_display_get_wl_compositor(
				gdk_display_get_default());
		}
	#endif
#endif
static void GLContextWOutputGeometryHandler(void *data, struct wl_output *output,
	int32_t x, int32_t y, int32_t physical_width, int32_t physical_height,
	int32_t subpixel, const char *make, const char *model, int32_t transform) {}
static void GLContextWOutputModeHandler(void *con, struct wl_output *output,
	uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
	GLCONTEXT* data = (GLCONTEXT*)con;
	if(data->mhDrawable)
		wl_egl_window_resize((struct wl_egl_window*)data->mhDrawable,
			width, height, 0, 0);
	else
		data->mhDrawable = (GLDRAWABLEHANDLE) wl_egl_window_create(
			(struct wl_surface*)data->mhWindow, width, height);
}
static void GLContextWOutputDoneHandler(void *data, struct wl_output *output) {}
static void GLContextWOutputScaleHandler(void *data,
	struct wl_output *output, int32_t scale) {}
static void GLContextWOutputDescriptionHandler(void *data,
	struct wl_output *output, const char* scale) {}

void GLContextWRegistryHandler(void *con, struct wl_registry *registry,
	uint32_t id, const char *interface, uint32_t version)
{
	GLCONTEXT* data = (GLCONTEXT*)con;
	if (strcmp(interface, wl_compositor_interface.name) == 0)
		data->mhSurface = (GLSURFACEHANDLE)(struct wl_compositor*)
			wl_registry_bind(registry, id, &wl_compositor_interface, 1);
	else if (strcmp(interface, wl_shell_interface.name) == 0)
		data->mhSurface = (GLSURFACEHANDLE)(struct wl_shell*)
			wl_registry_bind(registry, id, &wl_shell_interface, 1);
	else if (strcmp(interface, wl_output_interface.name) == 0)
		data->mhWOutput = (struct wl_output*)
			wl_registry_bind(registry, id, &wl_output_interface, 1);
}
void GLContextWRegistryRemover(void *data,
	struct wl_registry *registry, uint32_t id) {}
GLWINDOWHANDLE GLCreateFullscreenX11Window(Display* dpy)
{
	XSetWindowAttributes swa;
	Atom wmState, wmFullscreen;
	Window window;
	int screen = DefaultScreen(dpy);
	Visual* visual = DefaultVisual(dpy, screen);
	swa.colormap = XCreateColormap(dpy, RootWindow(dpy, screen),
		visual, AllocNone);
	swa.event_mask = ExposureMask | KeyPressMask;
	window = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0,
		DisplayWidth(dpy, screen), DisplayHeight(dpy, screen),
		0, DefaultDepth(dpy, screen), InputOutput,
		visual, CWColormap | CWEventMask, &swa);
	if(window)
	{
		wmState = XInternAtom(dpy, "_NET_WM_STATE", False);
		wmFullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
		#ifndef XA_ATOM
			#define DEFINE_XA_ATOM
			#define XA_ATOM ((Atom)4)
		#endif
		XChangeProperty(dpy, window, wmState, XA_ATOM, 32,
			PropModeReplace, (unsigned char*)&wmFullscreen, 1);
		#ifdef DEFINE_XA_ATOM
			#undef DEFINE_XA_ATOM
			#undef XA_ATOM
		#endif
		XMapWindow(dpy, window);
		XStoreName(dpy, window, "EGL Fullscreen");
	}
	return (void*)window;
}
GLDRAWABLEHANDLE GLGetWindowDrawable(GLWINDOWHANDLE window)
{
#if defined(HAVE_QT) && defined(__cplusplus)
	if(qApp->platformName() == "wayland")
	{
		// winId() does not return wl_surface on wayland. There has
		// been numerous tickets by the Qt team to do this but as
		// of 2024 the native interface still has to be used
		// currently winId() just returns an incremented counter
		QWindow* nativeWindow = ((QWidget*)window)->windowHandle();
		QPlatformNativeInterface* native =
			QGuiApplication::platformNativeInterface();
		return (GLDRAWABLEHANDLE)
			(native->nativeResourceForWindow("surface", nativeWindow));
	}
	return (GLDRAWABLEHANDLE)((QWidget*)window)->winId();
#elif defined(GDK_CHECK_VERSION)
	GLDISPLAYINFO di;
	GLGetDisplayInfo(&di);
	if(di.type == GLDISPLAYTYPE_X11)
		#if GTK_CHECK_VERSION(4,0,0)
			return (GLDRAWABLEHANDLE)gdk_x11_surface_get_xid(
				gtk_native_get_surface(GTK_NATIVE(window)));
		#elif GTK_CHECK_VERSION(3,0,0)
			return (GLDRAWABLEHANDLE)gdk_x11_window_get_xid(
				/*GDK_WINDOW(window));*/
				gtk_widget_get_window(GTK_WIDGET(window)));
		#else
			return (GLDRAWABLEHANDLE)GDK_WINDOW_XWINDOW(
				gtk_widget_get_window(GTK_WIDGET(window)));
				/*window);*/
		#endif
	#if GTK_CHECK_VERSION(3,0,0)
		else if(di.type == GLDISPLAYTYPE_WAYLAND)
		#if GTK_CHECK_VERSION(4,0,0)
			return gdk_wayland_surface_get_wl_surface(
					gtk_native_get_surface(GTK_NATIVE(window)));
		#else
			return gdk_wayland_window_get_wl_surface(
				/*GDK_WINDOW(window));*/
					gtk_widget_get_window(GTK_WIDGET(window)));
		#endif
	#endif
	else
		return NULL;
#else
	return window;
#endif
}
/*
	Wayland proxy userdata is really unreliable with wl_egl_window.
	By the time it gets to GLDestroyFullscreenWaylandWindow the
	proxy user data pointer is invalid and after calling eglDestroySurface
	on the EGLSurface associated with the wl_egl_window the proxy user data
	is NULL.
*/
void GLContext_CreateWaylandWindow(GLCONTEXT* pThis)
{
	struct wl_registry_listener listener;
	struct wl_compositor* compositor;
	struct wl_output* output;
	struct wl_output_listener outputlistener;
	struct wl_shell* shell;
	struct wl_shell_surface* shellsurface;
	struct wl_registry* registry;
	if(pThis->mhWindow) /* user provided */
	{
		pThis->mhDrawable = (GLDRAWABLEHANDLE) wl_egl_window_create(
			(struct wl_surface*)pThis->mhWindow, 1, 1); /* can't be 0,0 */
	}
	else /* make fullscreen window from scratch */
	{
		registry = wl_display_get_registry(pThis->mhWDisplay);
		listener.global = GLContextWRegistryHandler;
		listener.global_remove = GLContextWRegistryRemover;
		wl_registry_add_listener(registry, &listener, pThis);
		outputlistener.done = GLContextWOutputDoneHandler;
		outputlistener.mode = GLContextWOutputModeHandler;
		outputlistener.scale = GLContextWOutputScaleHandler;
		outputlistener.description = GLContextWOutputDescriptionHandler;
		outputlistener.geometry = GLContextWOutputGeometryHandler;
		/* pThis->mhSurface is a temp used for the compositor+shell */
		wl_display_roundtrip(pThis->mhWDisplay);
		compositor = (struct wl_compositor*)pThis->mhSurface;
		pThis->mhWindow = (GLWINDOWHANDLE)
			wl_compositor_create_surface(compositor);
		/* Set to full screen via the shell surface then clean it up */
		shell = (struct wl_shell*)pThis->mhSurface;
		shellsurface =
			wl_shell_get_shell_surface(shell,
				(struct wl_surface*)pThis->mhWindow);
		wl_shell_surface_set_fullscreen(shellsurface,
			WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT, 0, NULL);
		wl_shell_surface_destroy(shellsurface);
		wl_shell_destroy(shell);
		/* Now get the output and create the eglwindow (from the callback) */
		pThis->mhDrawable = NULL;
		wl_output_add_listener((struct wl_output*)pThis->mhWOutput,
			&outputlistener, pThis);
		wl_display_roundtrip(pThis->mhWDisplay);
	}

	if(!pThis->mhDrawable) /* Can happen if size 0,0 is set */
	{
		if(pThis->mhWOutput)
		{
			wl_output_destroy((struct wl_output*)pThis->mhWOutput);
			wl_surface_destroy((struct wl_surface*)pThis->mhWindow);
			pThis->mhWOutput = NULL;
			pThis->mhWindow = NULL;
		}
		/*fprintf(stderr, "Failed to create wl_egl_window\n");
		perror("Failed to create wl_egl_window");
		fflush(stderr);*/
	}
}
void GLContext_DestroyWaylandWindow(GLCONTEXT* pThis)
{
	if(pThis->mhWOutput)
	{
		wl_output_destroy((struct wl_output*)pThis->mhWOutput);
		wl_surface_destroy((struct wl_surface*)pThis->mhWindow);
	}
	wl_egl_window_destroy((struct wl_egl_window*)pThis->mhDrawable);
}
void GLContext_CreateFullscreenWindow(GLCONTEXT* pThis)
{
	EGLNativeWindowType nativewindow;
#if defined(HAVE_QT) && defined(__cplusplus)
	QScreen* screen = QGuiApplication::primaryScreen();
	if(!screen)
		return; /* no QApplication likely */

	QWidget* window = new QWidget;
	/* Not needed as setFullScreen() will take care of it */
	/* QRect screenGeometry = screen->geometry();
		window->setGeometry(screenGeometry);*/
	window->winId(); /* Force backend window creation */
	/* Not needed as it is primary screen by default */
	/*
	 *	window->windowHandle()->setScreen(screen); */
	window->showFullScreen();

	/* Do a display roundtrip to sync fullscreen and avoid race conditions */
	if (QNativeInterface::QWaylandApplication* waylandApp =
			qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
				wl_display_roundtrip(waylandApp->display());

	pThis->mhWindow = (GLWINDOWHANDLE)window;
	if(pThis->mhWindow && pThis->mhWDisplay)
	{
		/* don't do window->show(); that undoes the fullscreen */
		GLWINDOWHANDLE realwindow = pThis->mhWindow;
		pThis->mhWindow =
			GLGetWindowDrawable(pThis->mhWindow);
		GLContext_CreateWaylandWindow(pThis);
		pThis->mhWindow = realwindow;
		GLContext_ResizeDrawable(pThis, window->width(), window->height());
		/* just need to process a couple events to get the window to show
			in wayland on Qt. X11 does it automatically. */
		qApp->processEvents(QEventLoop::AllEvents);
	}
	/*qDebug() << window->isFullScreen();
	qDebug() << window->windowState();
	qDebug() << window->geometry();
	qDebug() << window->frameGeometry();
	qDebug() << window->size();*/

#elif defined(GDK_CHECK_VERSION)
		GdkDisplay *display = gdk_display_get_default();
		int w, h;
	#if GDK_CHECK_VERSION(4,0,0)
		GtkWidget* window;
		GdkRectangle geo;
		GListModel* monitors;
		GdkMonitor* monitor;
		if(!display)
			return;
		window = gtk_window_new();
		monitors = gdk_display_get_monitors(display);
		monitor = (GdkMonitor*)
			g_list_model_get_item(monitors, 0);
		gdk_monitor_get_geometry(monitor, &geo);
		gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
		gtk_window_set_default_size(GTK_WINDOW(window),
			geo.width, geo.height);
		gtk_window_present(GTK_WINDOW(window));
		/*GdkDisplay* gdkdisplay = gtk_widget_get_display(window);
		struct wl_display* waydpy =
			gdk_wayland_display_get_wl_display(gdkdisplay);*/
		pThis->mhWindow = (GLWINDOWHANDLE)window;
		w = geo.width; h = geo.height;
		while(g_main_context_pending(NULL))
			g_main_context_iteration(NULL, FALSE);
	#else
		/* Gdk's X11 is OK with just a GdkWindow, but Wayland requires
		a Gtk interface */
		GdkScreen* screen = gdk_display_get_default_screen(display);
		GdkWindowAttr attributes;
		GdkWindow* window;
		GtkWindow* gtk_window;
		if(!display)
			return;
		attributes.window_type = GDK_WINDOW_TOPLEVEL;
		attributes.x = 0;
		attributes.y = 0;
		attributes.width = gdk_screen_get_width(screen);
		attributes.height = gdk_screen_get_height(screen);
		attributes.wclass = GDK_INPUT_OUTPUT;
		attributes.event_mask = GDK_EXPOSURE_MASK | GDK_KEY_PRESS_MASK;
		window = gdk_window_new(NULL, &attributes, GDK_WA_X | GDK_WA_Y);
		/*gdk_window_show(window);*/
		gtk_window = (GtkWindow*)gtk_window_new(GTK_WINDOW_TOPLEVEL);
		gtk_widget_set_window(GTK_WIDGET(gtk_window), window);
		gtk_widget_show(GTK_WIDGET(gtk_window));

		pThis->mhWindow = (GLWINDOWHANDLE)gtk_window;
		w = attributes.width; h = attributes.height;
		while(gtk_events_pending())
			gtk_main_iteration_do(FALSE);
	#endif
	if(pThis->mhWindow && pThis->mhWDisplay)
	{
		GLWINDOWHANDLE realwindow = pThis->mhWindow;
		pThis->mhWindow =
			GLGetWindowDrawable(pThis->mhWindow);
		GLContext_CreateWaylandWindow(pThis);
		pThis->mhWindow = realwindow;
		GLContext_ResizeDrawable(pThis, w, h);
	}
#else
	GLDISPLAYINFO di;
	GLGetDisplayInfo(&di);
	if(pThis->mhWDisplay)
		GLContext_CreateWaylandWindow(pThis);
	else
		pThis->mhWindow = (GLWINDOWHANDLE)
			GLCreateFullscreenX11Window(pThis->mhXDisplay);
#endif
}
void GLContext_DestroyFullscreenWindow(GLCONTEXT* pThis)
{
#if defined(HAVE_QT) && defined(__cplusplus)
	delete (QWidget*)pThis->mhWindow;
	if(pThis->mhWDisplay)
	{
		pThis->mhWindow = NULL;
		GLContext_DestroyWaylandWindow(pThis);
	}
#elif defined(GDK_CHECK_VERSION)
	#if GTK_CHECK_VERSION(4,0,0)
		gtk_window_destroy((GtkWindow*)pThis->mhWindow);
	#else
		/*gdk_window_destroy((GdkWindow*)pThis->mhWindow);*/
		gtk_widget_destroy((GtkWidget*)pThis->mhWindow);
	#endif
	if(pThis->mhWDisplay)
	{
		pThis->mhWindow = NULL;
		GLContext_DestroyWaylandWindow(pThis);
	}
#else
	if(pThis->mhWDisplay)
		GLContext_DestroyWaylandWindow(pThis);
	else
		XDestroyWindow(pThis->mhXDisplay, (Window)pThis->mhWindow);
#endif
}

#ifdef __cplusplus
	extern "C" {
#endif
/*---------------------- GLContextAttributes ---------------------*/
void GLContextAttributes_CoreProfile(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_AddBits(pThis, EGL_CONTEXT_OPENGL_PROFILE_MASK,
				  EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT);
}
void GLContextAttributes_MajorVersion(GLCONTEXTATTRIBUTES* pThis, int val)
{
	if (val > 0)
	{
		GLContextAttributes_Add(pThis, EGL_CONTEXT_MAJOR_VERSION);
		GLContextAttributes_Add(pThis, val);
	}
}
void GLContextAttributes_MinorVersion(GLCONTEXTATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLContextAttributes_Add(pThis, EGL_CONTEXT_MINOR_VERSION);
		GLContextAttributes_Add(pThis, val);
	}
}
void GLContextAttributes_CompatibilityProfile(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_AddBits(pThis, EGL_CONTEXT_OPENGL_PROFILE_MASK,
				  EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT);
}
void GLContextAttributes_ForwardCompatible(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_Add(pThis, EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE);
	GLContextAttributes_Add(pThis, EGL_TRUE);
}
void GLContextAttributes_ES2(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_AddBits(pThis, EGL_RENDERABLE_TYPE,
				  EGL_OPENGL_ES2_BIT);
}
void GLContextAttributes_DebugCtx(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_Add(pThis, EGL_CONTEXT_OPENGL_DEBUG);
	GLContextAttributes_Add(pThis, EGL_TRUE);
}
void GLContextAttributes_Robust(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_Add(pThis, EGL_CONTEXT_OPENGL_ROBUST_ACCESS);
	GLContextAttributes_Add(pThis, EGL_TRUE);
}
void GLContextAttributes_NoResetNotify(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_Add(pThis,
		EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY);
	GLContextAttributes_Add(pThis, EGL_NO_RESET_NOTIFICATION);
}
void GLContextAttributes_LoseOnReset(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_Add(pThis,
		EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY);
	GLContextAttributes_Add(pThis, EGL_LOSE_CONTEXT_ON_RESET);
}
void GLContextAttributes_ResetIsolation(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_ReleaseFlush(GLCONTEXTATTRIBUTES* pThis, int val)
{ (void)(val); }
void GLContextAttributes_PlatformDefaults(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_EndList(GLCONTEXTATTRIBUTES* pThis)
{ GLContextAttributes_Add(pThis, EGL_NONE); }

/*---------------------- GLPixelAttributes ---------------------*/
void GLPixelAttributes_RGBA(GLPIXELATTRIBUTES* pThis)
{ }
void GLPixelAttributes_BufferSize(GLPIXELATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLPixelAttributes_Add(pThis, EGL_BUFFER_SIZE);
		GLPixelAttributes_Add(pThis, val);
	}
}
void GLPixelAttributes_Level(GLPIXELATTRIBUTES* pThis, int val)
{
	GLPixelAttributes_Add(pThis, EGL_LEVEL);
	GLPixelAttributes_Add(pThis, val);
}
void GLPixelAttributes_DoubleBuffer(GLPIXELATTRIBUTES* pThis)
{ }
void GLPixelAttributes_Stereo(GLPIXELATTRIBUTES* pThis)
{ }
void GLPixelAttributes_AuxBuffers(GLPIXELATTRIBUTES* pThis, int val)
{ (void)(val); }
void GLPixelAttributes_MinRGBA(GLPIXELATTRIBUTES* pThis,
	int mRed, int mGreen, int mBlue, int mAlpha)
{
	if (mRed >= 0)
	{
		GLPixelAttributes_Add(pThis, EGL_RED_SIZE);
		GLPixelAttributes_Add(pThis, mRed);
	}
	if (mGreen >= 0)
	{
		GLPixelAttributes_Add(pThis, EGL_GREEN_SIZE);
		GLPixelAttributes_Add(pThis, mGreen);
	}
	if (mBlue >= 0)
	{
		GLPixelAttributes_Add(pThis, EGL_BLUE_SIZE);
		GLPixelAttributes_Add(pThis, mBlue);
	}
	if (mAlpha >= 0)
	{
		GLPixelAttributes_Add(pThis, EGL_ALPHA_SIZE);
		GLPixelAttributes_Add(pThis, mAlpha);
	}
}
void GLPixelAttributes_Depth(GLPIXELATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLPixelAttributes_Add(pThis, EGL_DEPTH_SIZE);
		GLPixelAttributes_Add(pThis, val);
	}
}
void GLPixelAttributes_Stencil(GLPIXELATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLPixelAttributes_Add(pThis, EGL_STENCIL_SIZE);
		GLPixelAttributes_Add(pThis, val);
	}
}
void GLPixelAttributes_MinAcumRGBA(GLPIXELATTRIBUTES* pThis,
	int minred, int mingreen, int minblue, int minalpha)
{ (void)(minred); (void)(mingreen); (void)(minblue); (void)(minalpha); }
void GLPixelAttributes_SampleBuffers(GLPIXELATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLPixelAttributes_Add(pThis, EGL_SAMPLE_BUFFERS);
		GLPixelAttributes_Add(pThis, val);
	}
}
void GLPixelAttributes_Samplers(GLPIXELATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLPixelAttributes_Add(pThis, EGL_SAMPLES);
		GLPixelAttributes_Add(pThis, val);
	}
}
void GLPixelAttributes_FrameBuffersRGB(GLPIXELATTRIBUTES* pThis)
{ }
void GLPixelAttributes_EndList(GLPIXELATTRIBUTES* pThis)
{ GLPixelAttributes_Add(pThis, EGL_NONE); }
void GLPixelAttributes_PlatformDefaults(GLPIXELATTRIBUTES* pThis)
{ /* EGL doesn't have any special attributes */ }

/*=========================== GLContext =====================================*/
void GLContext_SafeCloseDisplays(GLCONTEXT* pThis)
{
	if(pThis->mhWDisplay)
	{
		if(pThis->mWillCloseDisplay)
			wl_display_disconnect(pThis->mhWDisplay);
		pThis->mhWDisplay = NULL;
	}
	else
	{
		if(pThis->mWillCloseDisplay)
			XCloseDisplay(pThis->mhXDisplay);
		pThis->mhXDisplay = NULL;
	}
}
int GLIsExtensionSupported(const char *extension)
{
	return GLContextFindExtension(eglQueryString(NULL, EGL_EXTENSIONS), extension);
}
int GLContext_IsExtensionSupported(GLCONTEXT* pThis, const char *extension)
{
	return GLContextFindExtension(eglQueryString(pThis->mhDisplay, EGL_EXTENSIONS),
		extension);
}
const char* GLContext_GetNativeExtensions(GLCONTEXT* pThis)
{
	return eglQueryString(pThis->mhDisplay, EGL_EXTENSIONS);
}

int GLContext_CreateClone_(GLCONTEXT* pThis, GLCONTEXTHANDLE clone,
	GLWINDOWHANDLE window, GLDRAWABLEHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{
	const int* contextattriblist = (!contextattribs
			|| GLContextAttributes_GetCount(contextattribs) == 0) ?
				NULL : GLContextAttributes_Get(contextattribs);
	const int* pixelattriblist = (!pixelattribs
			|| GLPixelAttributes_GetCount(pixelattribs) == 0) ?
				NULL : GLPixelAttributes_Get(pixelattribs);
	typedef EGLDisplay (*GetPlatformDisplayProc)(EGLenum platform,
		void* native_display, const EGLAttrib* attrib_list);
	GetPlatformDisplayProc pfnGetPlatformDisplay;
	GLDISPLAYINFO dpyinfo;
	EGLConfig* configs;
	EGLint eglMajor, eglMinor;
	EGLint numconfigs;
	int i;
	/* Get the EGLDisplay */
	GLGetDisplayInfo(&dpyinfo);
	if (GLIsExtensionSupported("EGL_EXT_platform_base"))
	{
		pfnGetPlatformDisplay = (GetPlatformDisplayProc)
				eglGetProcAddress("eglGetPlatformDisplay");
		if (!pfnGetPlatformDisplay) /* old function */
			pfnGetPlatformDisplay = (GetPlatformDisplayProc)
				eglGetProcAddress("eglGetPlatformDisplayEXT");
	}
	if(!dpyinfo.dpy.dpy)
	{
		pThis->mhWDisplay = wl_display_connect(NULL);
		if(!pThis->mhWDisplay)
		{
			pThis->mhXDisplay = XOpenDisplay(NULL);
			if(!pThis->mhXDisplay)
				return 0;
			dpyinfo.dpy.x11dpy = pThis->mhXDisplay;
			dpyinfo.type = GLDISPLAYTYPE_X11;
		}
		else
		{
			dpyinfo.dpy.waylanddpy = pThis->mhWDisplay;
			dpyinfo.type = GLDISPLAYTYPE_WAYLAND;
		}
		pThis->mWillCloseDisplay = 1;
	}
	else
	{
		if(dpyinfo.type == GLDISPLAYTYPE_WAYLAND)
			pThis->mhWDisplay = dpyinfo.dpy.waylanddpy;
		else
			pThis->mhXDisplay = dpyinfo.dpy.x11dpy;
		pThis->mWillCloseDisplay = 0;
	}

	if (!pfnGetPlatformDisplay)
		pThis->mhDisplay =
			eglGetDisplay((EGLNativeDisplayType)(dpyinfo.dpy.dpy));
	else
	{
		EGLenum platform;
		switch (dpyinfo.type)
		{
			case GLDISPLAYTYPE_X11:
				platform = EGL_PLATFORM_X11_EXT;
				break;
			case GLDISPLAYTYPE_WAYLAND:
				platform = EGL_PLATFORM_WAYLAND_EXT;
				break;
			default:
				printf("GLContext: Unknown Windowing System\n");
				return 0;
		}
		pThis->mhDisplay =
			(*pfnGetPlatformDisplay)(platform, dpyinfo.dpy.dpy, NULL);
	}
	/* Create the EGLConfig */
	if (!eglInitialize(pThis->mhDisplay, &eglMajor, &eglMinor))
	{
		printf("GLContext: eglInitialize failed\n");
		GLContext_SafeCloseDisplays(pThis);
		return 0;
	}
	/* With GetPlatformDisplayProc handling we should be 1.0 safe */
	/*if (eglMajor < 1 || (eglMajor == 1 && eglMinor < 5))
	{
		eglTerminate(pThis->mhDisplay);
		printf("GLContext: EGL version [%d.%d] is less "
			"than the required 1.5\n", eglMajor, eglMinor);
		return 0;
	}*/
	if (!eglBindAPI(EGL_OPENGL_API))
	{
		printf("GLContext: eglBindAPI failed\n");
		GLContext_SafeCloseDisplays(pThis);
		return 0;
	}
	pThis->mhPixelFormat = NULL;
	if(pixelattriblist)
	{ /* Check if a minimum alpha of > 0 is specified; else non-alpha */
		for (i = 0; pixelattriblist[i] != EGL_NONE; i += 2)
		{
			if (pixelattriblist[i] == EGL_ALPHA_SIZE
					&& pixelattriblist[i + 1] > 0)
			{ /* alpha > 1. any config the driver suggests is fine */
				if ( !eglChooseConfig(pThis->mhDisplay, pixelattriblist,
							&pThis->mhPixelFormat, 1, &numconfigs)
						|| numconfigs != 1 )
				{
					GLContext_SafeCloseDisplays(pThis);
					return 0; /* no match */
				}
			}
		}
	}
	if(!pThis->mhPixelFormat)
	{
		if (!eglChooseConfig(pThis->mhDisplay,
					pixelattriblist, NULL, 0, &numconfigs)
				|| !numconfigs)
		{
			GLContext_SafeCloseDisplays(pThis);
			return 0;
		}
		configs = (EGLConfig*)malloc(sizeof(EGLConfig) * numconfigs);
		if (!configs
			|| !eglChooseConfig(pThis->mhDisplay, pixelattriblist,
					configs, numconfigs, &numconfigs))
		{
			printf("GLContext: eglChooseConfig group collection failed\n");
			GLContext_SafeCloseDisplays(pThis);
			return 0;
		}

		for (i = 0; i < numconfigs; ++i)
		{
			EGLint alpha;
			if (!eglGetConfigAttrib(pThis->mhDisplay,
					configs[i], EGL_ALPHA_SIZE, &alpha))
			{
				printf("GLContext: "
					"eglGetConfigAttrib(EGL_ALPHA_SIZE) failed\n");
				continue;
			}
			if (alpha == 0)
			{
				pThis->mhPixelFormat = configs[i];
				break;
			}
		}
		if(!pThis->mhPixelFormat) /* fail to driver's choice */
			pThis->mhPixelFormat = configs[0];
		free(configs);
	}
	pThis->mhContext = eglCreateContext(pThis->mhDisplay, pThis->mhPixelFormat,
		clone, contextattriblist);
	/* create the surface */
	if(pThis->mhContext)
	{
		if(!drawable)
		{/* if mhWDisplay or mhXDisplay valid == GTK/QT isn't running */

			if(pThis->mhWDisplay)
			{
				if(window) /* create egl_window holder */
				{
					pThis->mhWindow = window;
					GLContext_CreateWaylandWindow(pThis);
					pThis->mhWindow = NULL;
				}
				else /* create fullscreen window and eglwindow */
				{
					pThis->mhWindow = NULL;
					GLContext_CreateFullscreenWindow(pThis);
				}
				drawable = pThis->mhDrawable;
			}
			else
			{
				if(window)
				{
					pThis->mhWindow = window;
					drawable = window;
					pThis->mhDrawable = NULL;
				}
				else
				{
					GLContext_CreateFullscreenWindow(pThis);
					drawable = GLGetWindowDrawable(pThis->mhWindow);
					pThis->mhDrawable = drawable;
				}
			}
			if(!drawable)
			{
				printf("GLContext: Couldn't create fullscreen window\n");
				eglDestroyContext(pThis->mhDisplay, pThis->mhContext);
				pThis->mhContext = NULL;
			}
		}
		else
		{
			pThis->mhWindow = NULL;
			pThis->mhDrawable = NULL;
		}
		pThis->mhSurface = eglCreateWindowSurface(pThis->mhDisplay,
			pThis->mhPixelFormat, (EGLNativeWindowType)drawable, NULL);
		if(!pThis->mhSurface)
		{
			printf("GLContext: Couldn't create EGLSurface\n");
			eglDestroyContext(pThis->mhDisplay, pThis->mhContext);
			pThis->mhContext = NULL;
		}
	}
	/* done */
	if(pThis->mhContext == NULL)
	{
		GLContext_SafeCloseDisplays(pThis);
		return 0;
	}
	/* X11 blocks for a second if the drawable is completely occluded,
	should it be set to 0 by default on X11? "The default swap interval is 1"
	registry.khronos.org/EGL/sdk/docs/man/html/eglSwapInterval.xhtml */
	pThis->mSwapInterval = 1;
	return 1;
}
int GLContext_CreateClone(GLCONTEXT* pThis, GLCONTEXTHANDLE clone,
	GLDRAWABLEHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{
	return GLContext_CreateClone_(pThis, clone,
		NULL, drawable, contextattribs, pixelattribs);
}
int GLContext_Create(GLCONTEXT* pThis, GLDRAWABLEHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{
	return GLContext_CreateClone_(pThis, NULL,
		NULL, drawable, contextattribs, pixelattribs);
}
void GLContext_Destroy(GLCONTEXT* pThis)
{
	if (pThis->mhContext == eglGetCurrentContext())
		eglMakeCurrent(pThis->mhDisplay, EGL_NO_SURFACE,
					   EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroySurface(pThis->mhDisplay, pThis->mhSurface);
	eglDestroyContext(pThis->mhDisplay, pThis->mhContext);
	if(pThis->mhWDisplay)
	{
		if(pThis->mhDrawable)
		{
			if(!pThis->mhWindow)
				GLContext_DestroyWaylandWindow(pThis);
			else
				GLContext_DestroyFullscreenWindow(pThis);
		}
		if(pThis->mWillCloseDisplay)
			wl_display_disconnect(pThis->mhWDisplay);
	}
	else
	{
		if(pThis->mhDrawable)
			GLContext_DestroyFullscreenWindow(pThis);
		if(pThis->mWillCloseDisplay)
			XCloseDisplay(pThis->mhXDisplay);
	}
}

int GLContext_SetCurrent(GLCONTEXT* pThis)
{
	return eglMakeCurrent(pThis->mhDisplay, pThis->mhSurface,
		pThis->mhSurface, pThis->mhContext);
}
int GLContext_SetCurrentHandle(GLCONTEXT* pThis, GLCONTEXTHANDLE handle)
{ return eglMakeCurrent(pThis->mhDisplay, pThis->mhSurface,
		pThis->mhSurface, handle); }
int GLContext_SwapBuffers(GLCONTEXT* pThis)
{
	return eglSwapBuffers(pThis->mhDisplay, pThis->mhSurface);
}
int GLContext_SetDrawable(GLCONTEXT* pThis, GLDRAWABLEHANDLE drawable)
{
	if(!drawable && ((pThis->mhWDisplay && pThis->mhWindow)
			|| (!pThis->mhWDisplay && pThis->mhDrawable)))
		return 1; /* wants fullscreen and already fullscreen */

	if (pThis->mhContext == eglGetCurrentContext())
		eglMakeCurrent(pThis->mhDisplay, EGL_NO_SURFACE,
					   EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroySurface(pThis->mhDisplay, pThis->mhSurface);
	if(pThis->mhWDisplay)
	{
		if(pThis->mhDrawable)
		{
			if(!pThis->mhWindow)
				GLContext_DestroyWaylandWindow(pThis);
			else
				GLContext_DestroyFullscreenWindow(pThis);
		}
	}
	else if(pThis->mhDrawable)
			GLContext_DestroyFullscreenWindow(pThis);
	if(!drawable)
	{
		pThis->mhWindow = NULL;
		pThis->mhDrawable = NULL;
		GLContext_CreateFullscreenWindow(pThis);
		if(!pThis->mhWindow)
			return 0;
		if(pThis->mhWDisplay)
			drawable = pThis->mhDrawable;
		else
		{
			drawable = GLGetWindowDrawable(pThis->mhWindow);
			pThis->mhDrawable = drawable;
		}
	}
	else
	{
		pThis->mhDrawable = NULL;
		pThis->mhWindow = NULL;
	}
	pThis->mhSurface = eglCreateWindowSurface(pThis->mhDisplay,
			pThis->mhPixelFormat, (EGLNativeWindowType)drawable, NULL);
	return pThis->mhSurface != NULL;
}
GLWINDOWHANDLE GLContext_GetWindowHandle(GLCONTEXT* pThis)
{ return pThis->mhWindow; }

GLCONTEXTHANDLE GLContext_GetCurrent(GLCONTEXT* pThis)
{
	return eglGetCurrentContext();
}
int GLContext_SetSwapInterval(GLCONTEXT* pThis, int interval)
{
	if(!eglSwapInterval(pThis->mhDisplay, interval))
		return 0;
	pThis->mSwapInterval = interval;
	return 1;
}
int GLContext_GetSwapInterval(GLCONTEXT* pThis)
{
	return pThis->mSwapInterval;
}
GLContextEmptyProc GLContext_GetProc(GLCONTEXT* pThis, const char* proc)
{ return eglGetProcAddress(proc); }
void GLContext_ResizeDrawable(GLCONTEXT* pThis, int width, int height)
{
	if(pThis->mhDrawable && pThis->mhWDisplay)
		wl_egl_window_resize((struct wl_egl_window*)pThis->mhDrawable,
			width, height, 0, 0);
}
int GLContext_CreateCloneWithWindow(GLCONTEXT* pThis, GLCONTEXTHANDLE clone,
	GLWINDOWHANDLE window,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return GLContext_CreateClone_(pThis, clone,
		window, NULL, contextattribs, pixelattribs); }
int GLContext_CreateWithWindow(GLCONTEXT* pThis, GLWINDOWHANDLE window,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return GLContext_CreateClone_(pThis, NULL,
		window, NULL, contextattribs, pixelattribs); }

#ifdef __cplusplus
	} /* end extern "C" */
#endif
/*=============================================================================
			 #     #   #     #       #####  #       #     #
			  #   #   ##    ##      #     # #        #   #
			   # #   # #   # #      #       #         # #
				#      #     #      #  #### #          #
			   # #     #     #      #     # #         # #
			  #   #    #     #      #     # #        #   #
			 #     # ##### #####     #####  ####### #     #
=============================================================================*/
#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) \
	|| defined(__linux__)
#if !defined(HAVE_QT) && (defined(QT_VERSION) || defined(WSQTOPT))
	#define HAVE_QT
#endif
#if defined(HAVE_QT) && defined(__cplusplus)
	#include <QGuiApplication>
	#include <QWidget>
	#include <qpa/qplatformnativeinterface.h>
#elif !defined(HAVE_QT)
	#include <gdk/gdk.h>
	#ifndef GDK_CHECK_VERSION
		#define GDK_CHECK_VERSION(major, minor, micro) \
			(GDK_MAJOR_VERSION > (major) || \
			(GDK_MAJOR_VERSION  == (major) \
				&& GDK_MINOR_VERSION > (minor)) || \
			(GDK_MAJOR_VERSION == (major) \
				&& GDK_MINOR_VERSION == (minor) \
				&& GDK_MICRO_VERSION >= (micro)))
	#endif
	#if GDK_CHECK_VERSION(4, 0, 0) /* req for getting X11 handle */
		#include <gdk/x11/gdkx.h>
		#include <gtk/gtk.h>
	#else
		#include <gdk/gdkx.h>
	#endif
#endif
#include <GL/glx.h> /* Conflicts if included before QT headers */
/* XVisualInfo is an anonymous struct and cannot be forward declared
i.e. typedef struct { ... } XVisualInfo; Nothing after struct == anonymous */
#define GLContext_PixelFormat(pThis) \
	(*((XVisualInfo**)(&(pThis)->mhPixelFormat)))
/*	IRIX uses the GLX_SGIS_multisample extension over ARB
	https://people.freedesktop.org/~marcheu/extensions/SGIS/multisample.html */
#ifdef __SGI__
	#ifndef GLX_SAMPLE_BUFFERS_ARB
		#define GLX_SAMPLE_BUFFERS_ARB GLX_SAMPLE_BUFFERS_SGIS
	#endif
	#ifndef GLX_SAMPLES_ARB
		#define GLX_SAMPLES_ARB GLX_SAMPLES_SGIS
	#endif
#endif /* __SGI__ */
#ifndef GLX_NONE_EXT
	#define GLX_NONE_EXT                       0x8000
#endif
#ifndef GLX_ARB_multisample
	#define GLX_ARB_multisample
	#define GLX_SAMPLE_BUFFERS_ARB             100000
	#define GLX_SAMPLES_ARB                    100001
#endif
#ifndef GLX_EXT_visual_rating
	#define GLX_EXT_visual_rating
	#define GLX_VISUAL_CAVEAT_EXT              0x20
	#define GLX_NONE_EXT                       0x8000
	#define GLX_SLOW_VISUAL_EXT                0x8001
	#define GLX_NON_CONFORMANT_VISUAL_EXT      0x800D
#endif
#ifndef GLX_EXT_visual_info
	#define GLX_EXT_visual_info
	#define GLX_X_VISUAL_TYPE_EXT              0x22
	#define GLX_DIRECT_COLOR_EXT               0x8003
#endif
#ifndef GLX_ARB_framebuffer_sRGB
	#define GLX_ARB_framebuffer_sRGB
	#define GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB   0x20B2
#endif
#ifndef GLX_ARB_create_context
	#define GLX_ARB_create_context
	#define GLX_CONTEXT_MAJOR_VERSION_ARB      0x2091
	#define GLX_CONTEXT_MINOR_VERSION_ARB      0x2092
	#define GLX_CONTEXT_FLAGS_ARB              0x2094
	#define GLX_CONTEXT_DEBUG_BIT_ARB          0x0001
	#define GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB 0x0002
	/* Typedef for the GL 3.0 context creation function */
	typedef GLXContext(*PFNGLXCREATECONTEXTATTRIBSARBPROC)
		(Display * dpy, GLXFBConfig config, GLXContext share_context,
		Bool direct, const int *attrib_list);
#endif
#ifndef GLX_ARB_create_context_profile
	#define GLX_ARB_create_context_profile
	#define GLX_CONTEXT_PROFILE_MASK_ARB       0x9126
	#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB   0x00000001
	#define GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
#endif
#ifndef GLX_ARB_create_context_robustness
	#define GLX_ARB_create_context_robustness
	#define GLX_CONTEXT_ROBUST_ACCESS_BIT_ARB  0x00000004
	#define GLX_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB     0x8256
	#define GLX_NO_RESET_NOTIFICATION_ARB                   0x8261
	#define GLX_LOSE_CONTEXT_ON_RESET_ARB                   0x8252
#endif
#ifndef GLX_ARB_robustness_application_isolation
	#define GLX_ARB_robustness_application_isolation
	#define GLX_CONTEXT_RESET_ISOLATION_BIT_ARB             0x00000008
#endif
#ifndef GLX_ARB_robustness_share_group_isolation
	#define GLX_ARB_robustness_share_group_isolation
#endif
#ifndef GLX_ARB_context_flush_control
	#define GLX_ARB_context_flush_control
	#define GLX_CONTEXT_RELEASE_BEHAVIOR_ARB            0x2097
	#define GLX_CONTEXT_RELEASE_BEHAVIOR_NONE_ARB       0
	#define GLX_CONTEXT_RELEASE_BEHAVIOR_FLUSH_ARB      0x2098
#endif
#ifndef GLX_EXT_create_context_es2_profile
	#define GLX_EXT_create_context_es2_profile
	#define GLX_CONTEXT_ES2_PROFILE_BIT_EXT    0x00000004
#endif
#ifndef GLX_EXT_create_context_es_profile
	#define GLX_EXT_create_context_es_profile
	#define GLX_CONTEXT_ES2_PROFILE_BIT_EXT    0x00000004
#endif

/*---------------------- [Utilities] ---------------------*/
typedef struct GLXDEFDISPLAY
{
	Display* dpy;
	int needsfree;
} GLXDEFDISPLAY;
#if defined(GDK_CHECK_VERSION)
	#if GDK_CHECK_VERSION(3,22,25)
		int GLGDKIsTypeName(void* instance, const char* string)
		{
			const char* name = g_type_name(G_TYPE_FROM_INSTANCE(instance));
			return strncmp(string, name, strlen(string)) == 0;
		}
	#endif /* GDK 3.22.25 */
#endif
Display* GLGetXDisplay()
{
#if defined(HAVE_QT) && defined(__cplusplus)
	if (qApp->platformName() == "xcb")
		return (Display*) QGuiApplication::platformNativeInterface(
				)->nativeResourceForIntegration("display");
	return NULL;
#elif defined(GDK_CHECK_VERSION)
	#if defined(GDK_WINDOWING_X11) && GDK_CHECK_VERSION(3,0,0)
		GdkDisplay* dpy = gdk_display_get_default();
		if(dpy)
		{
		#if GDK_CHECK_VERSION(3,22,25)
			if (GLGDKIsTypeName(dpy, "GdkX11"))
		#endif
				return GDK_DISPLAY_XDISPLAY(dpy);
		}
		return NULL;
	#elif GDK_CHECK_VERSION(3,0,0)
		return NULL;
	#else
		return GDK_DISPLAY();
	#endif
#else
	return NULL;
#endif
}
void GLX11DefDisplayCreate(GLXDEFDISPLAY* info)
{
	info->dpy = GLGetXDisplay();
	if(!info->dpy)
	{
		info->dpy = XOpenDisplay(NULL);
		info->needsfree = 1;
	}
	else
		info->needsfree = 0;
}
void GLX11DefDisplayDestroy(GLXDEFDISPLAY* info)
{
	if(info->needsfree)
		XCloseDisplay(info->dpy);
}

int GLGetGLXVersionWithDisplay(Display* dpy)
{
	int glxMajorVer, glxMinorVer;
	if (glXQueryVersion(dpy, &glxMajorVer, &glxMinorVer))
		return (glxMajorVer * 10) + glxMinorVer;
	else
		return 10;
}
int GLGetGLXVersion()
{
	GLXDEFDISPLAY dpy;
	int version;
	GLX11DefDisplayCreate(&dpy);
	version = GLGetGLXVersionWithDisplay(dpy.dpy);
	GLX11DefDisplayDestroy(&dpy);
	return version;
}
int GLIsGLXMultiSampleAvailable()
{
	GLXDEFDISPLAY dpy;
	int result;
	GLX11DefDisplayCreate(&dpy);
	result = GLContextFindExtension(glXQueryExtensionsString(dpy.dpy,
		DefaultScreen(dpy.dpy)), "GLX_ARB_multisample");
	GLX11DefDisplayDestroy(&dpy);
	return result;
}

#ifdef __cplusplus
	extern "C" {
#endif
/*---------------------- GLContextAttributes ---------------------*/

void GLContextAttributes_CoreProfile(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_AddBits(pThis, GLX_CONTEXT_PROFILE_MASK_ARB,
				  GLX_CONTEXT_CORE_PROFILE_BIT_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}
void GLContextAttributes_MajorVersion(GLCONTEXTATTRIBUTES* pThis, int val)
{
	if (val > 0)
	{
		GLContextAttributes_Add(pThis, GLX_CONTEXT_MAJOR_VERSION_ARB);
		GLContextAttributes_Add(pThis, val);
		if (val >= 3)
			GLContextAttributes_SetNeedsARB(pThis);
	}
}
void GLContextAttributes_MinorVersion(GLCONTEXTATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLContextAttributes_Add(pThis, GLX_CONTEXT_MINOR_VERSION_ARB);
		GLContextAttributes_Add(pThis, val);
	}
}
void GLContextAttributes_CompatibilityProfile(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_AddBits(pThis, GLX_CONTEXT_PROFILE_MASK_ARB,
				  GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}
void GLContextAttributes_ForwardCompatible(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_AddBits(pThis, GLX_CONTEXT_FLAGS_ARB,
				  GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}
void GLContextAttributes_ES2(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_AddBits(pThis, GLX_CONTEXT_PROFILE_MASK_ARB,
				  GLX_CONTEXT_ES2_PROFILE_BIT_EXT);
	GLContextAttributes_SetNeedsARB(pThis);
}
void GLContextAttributes_DebugCtx(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_AddBits(pThis, GLX_CONTEXT_FLAGS_ARB,
				  GLX_CONTEXT_DEBUG_BIT_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}
void GLContextAttributes_Robust(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_AddBits(pThis, GLX_CONTEXT_FLAGS_ARB,
				  GLX_CONTEXT_ROBUST_ACCESS_BIT_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}
void GLContextAttributes_NoResetNotify(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_Add(pThis,
		GLX_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB);
	GLContextAttributes_Add(pThis, GLX_NO_RESET_NOTIFICATION_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}
void GLContextAttributes_LoseOnReset(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_Add(pThis,
		GLX_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB);
	GLContextAttributes_Add(pThis, GLX_LOSE_CONTEXT_ON_RESET_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}
void GLContextAttributes_ResetIsolation(GLCONTEXTATTRIBUTES* pThis)
{
	GLContextAttributes_AddBits(pThis, GLX_CONTEXT_FLAGS_ARB,
				  GLX_CONTEXT_RESET_ISOLATION_BIT_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}
void GLContextAttributes_ReleaseFlush(GLCONTEXTATTRIBUTES* pThis, int val)
{
	GLContextAttributes_Add(pThis, GLX_CONTEXT_RELEASE_BEHAVIOR_ARB);
	if (val == 1)
		GLContextAttributes_Add(pThis, GLX_CONTEXT_RELEASE_BEHAVIOR_FLUSH_ARB);
	else
		GLContextAttributes_Add(pThis, GLX_CONTEXT_RELEASE_BEHAVIOR_NONE_ARB);
	GLContextAttributes_SetNeedsARB(pThis);
}
void GLContextAttributes_PlatformDefaults(GLCONTEXTATTRIBUTES* pThis)
{ /* none really */ }
void GLContextAttributes_EndList(GLCONTEXTATTRIBUTES* pThis)
{ GLContextAttributes_Add(pThis, None); }

/*---------------------- GLPixelAttributes ---------------------*/
/*
	GLX 1.3+ uses different methods for some attributes

	-  < 1.3: http://www.opengl.org/sdk/docs/man/xhtml/glXChooseVisual.xml
	- >= 1.3: http://www.opengl.org/sdk/docs/man/xhtml/glXChooseFBConfig.xml

	- GLX_RGBA is a boolean attribute in the old version in < 1.3
	-- In >= 1.3 it is a value of GLX_RENDER_TYPE in the new one
	- In < 1.3 boolean attributes such as GLX_DOUBLEBUFFER are alone
	-- In >= 1.3 GLX_DOUBLEBUFFER and others must be followed by True or False
*/
void GLPixelAttributes_RGBA(GLPIXELATTRIBUTES* pThis)
{
	if (GLGetGLXVersion() >= 13)
		GLPixelAttributes_AddBits(pThis, GLX_RENDER_TYPE, GLX_RGBA_BIT);
	else
		GLPixelAttributes_Add(pThis, GLX_RGBA);
}
void GLPixelAttributes_BufferSize(GLPIXELATTRIBUTES* pThis, int val)
{
	if (val >= 0)
	{
		GLPixelAttributes_Add(pThis, GLX_BUFFER_SIZE);
		GLPixelAttributes_Add(pThis, val);
	}
}
void GLPixelAttributes_Level(GLPIXELATTRIBUTES* pThis, int val)
{
	GLPixelAttributes_Add(pThis, GLX_LEVEL);
	GLPixelAttributes_Add(pThis, val);
}
void GLPixelAttributes_DoubleBuffer(GLPIXELATTRIBUTES* pThis)
{
	GLPixelAttributes_Add(pThis, GLX_DOUBLEBUFFER);
	if ( GLGetGLXVersion() >= 13 )
		GLPixelAttributes_Add(pThis, True);

}
void GLPixelAttributes_Stereo(GLPIXELATTRIBUTES* pThis)
{
	GLPixelAttributes_Add(pThis, GLX_STEREO);
	if ( GLGetGLXVersion() >= 13 )
		GLPixelAttributes_Add(pThis, True);
}
void GLPixelAttributes_AuxBuffers(GLPIXELATTRIBUTES* pThis, int val)
{
	if ( val >= 0 )
	{
		GLPixelAttributes_Add(pThis, GLX_AUX_BUFFERS);
		GLPixelAttributes_Add(pThis, val);
	}
}
void GLPixelAttributes_MinRGBA(GLPIXELATTRIBUTES* pThis,
	int mRed, int mGreen, int mBlue, int mAlpha)
{
	if ( mRed >= 0)
	{
		GLPixelAttributes_Add(pThis, GLX_RED_SIZE);
		GLPixelAttributes_Add(pThis, mRed);
	}
	if ( mGreen >= 0)
	{
		GLPixelAttributes_Add(pThis, GLX_GREEN_SIZE);
		GLPixelAttributes_Add(pThis, mGreen);
	}
	if ( mBlue >= 0)
	{
		GLPixelAttributes_Add(pThis, GLX_BLUE_SIZE);
		GLPixelAttributes_Add(pThis, mBlue);
	}
	if ( mAlpha >= 0)
	{
		GLPixelAttributes_Add(pThis, GLX_ALPHA_SIZE);
		GLPixelAttributes_Add(pThis, mAlpha);
	}
}
void GLPixelAttributes_Depth(GLPIXELATTRIBUTES* pThis, int val)
{
	if ( val >= 0 )
	{
		GLPixelAttributes_Add(pThis, GLX_DEPTH_SIZE);
		GLPixelAttributes_Add(pThis, val);
	}
}
void GLPixelAttributes_Stencil(GLPIXELATTRIBUTES* pThis, int val)
{
	if ( val >= 0 )
	{
		GLPixelAttributes_Add(pThis, GLX_STENCIL_SIZE);
		GLPixelAttributes_Add(pThis, val);
	}
}
void GLPixelAttributes_MinAcumRGBA(GLPIXELATTRIBUTES* pThis,
	int mRed, int mGreen, int mBlue, int mAlpha)
{
	if ( mRed >= 0)
	{
		GLPixelAttributes_Add(pThis, GLX_ACCUM_RED_SIZE);
		GLPixelAttributes_Add(pThis, mRed);
	}
	if ( mGreen >= 0)
	{
		GLPixelAttributes_Add(pThis, GLX_ACCUM_GREEN_SIZE);
		GLPixelAttributes_Add(pThis, mGreen);
	}
	if ( mBlue >= 0)
	{
		GLPixelAttributes_Add(pThis, GLX_ACCUM_BLUE_SIZE);
		GLPixelAttributes_Add(pThis, mBlue);
	}
	if ( mAlpha >= 0)
	{
		GLPixelAttributes_Add(pThis, GLX_ACCUM_ALPHA_SIZE);
		GLPixelAttributes_Add(pThis, mAlpha);
	}
}
void GLPixelAttributes_SampleBuffers(GLPIXELATTRIBUTES* pThis, int val)
{
#ifdef GLX_SAMPLE_BUFFERS_ARB
	if ( val >= 0 && GLIsGLXMultiSampleAvailable() )
	{
		GLPixelAttributes_Add(pThis, GLX_SAMPLE_BUFFERS_ARB);
		GLPixelAttributes_Add(pThis, val);
	}
#endif
}
void GLPixelAttributes_Samplers(GLPIXELATTRIBUTES* pThis, int val)
{
#ifdef GLX_SAMPLES_ARB
	if ( val >= 0 && GLIsGLXMultiSampleAvailable() )
	{
		GLPixelAttributes_Add(pThis, GLX_SAMPLES_ARB);
		GLPixelAttributes_Add(pThis, val);
	}
#endif
}
void GLPixelAttributes_FrameBuffersRGB(GLPIXELATTRIBUTES* pThis)
{
	GLPixelAttributes_Add(pThis, GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB);
	GLPixelAttributes_Add(pThis, True);
}
void GLPixelAttributes_EndList(GLPIXELATTRIBUTES* pThis)
{
	GLPixelAttributes_Add(pThis, None);
}
void GLPixelAttributes_PlatformDefaults(GLPIXELATTRIBUTES* pThis)
{
	/* For testing */
	/*
		GLPixelAttributes_Add(pThis, GLX_DEPTH_SIZE);
		GLPixelAttributes_Add(pThis, 24);
	*/
}

/*------------------------- GLContext ---------------------------------------*/
int GLSetCurrentContext(GLCONTEXTHANDLE context, GLDRAWABLEHANDLE drawable,
	Display* dpy)
{
	if (GLGetGLXVersion() >= 13) /* 1.3 is glXMakeContextCurrent */
		return glXMakeContextCurrent(dpy,
			drawable, drawable, context);
	else /* 1.2 is glXMakeCurrent; glXMakeContextCurrent doesn't exist */
		return glXMakeCurrent(dpy,
			drawable, context);
}

/* Sooo... on OpenGL 3+ the executable will terminate on context creation
if the global error handler isn't handled. Return value is ignored by X11.
TECHNICALLY we should also treat this being called as the context
being invalid, but X11 seems to return NULL in those cases. */
static int CTXErrorHandler(Display* dpy, XErrorEvent* ev)
{ (void)(dpy); (void)(ev); return 0; }

int GLContext_CreateFullscreenWindow(GLCONTEXT* pThis, Display* dpy)
{
	XSetWindowAttributes swa;
	Atom wmState, wmFullscreen;
	int screen = DefaultScreen(dpy);
	swa.colormap = XCreateColormap(dpy, RootWindow(dpy, screen),
		GLContext_PixelFormat(pThis)->visual, AllocNone);
	swa.event_mask = ExposureMask | KeyPressMask;
	pThis->mhWindow = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0,
		DisplayWidth(dpy, screen), DisplayHeight(dpy, screen),
		0, GLContext_PixelFormat(pThis)->depth, InputOutput,
		GLContext_PixelFormat(pThis)->visual,
		CWColormap | CWEventMask, &swa);
	if(!pThis->mhWindow)
		return 0;
	pThis->mIsFullscreen = 1;
	wmState = XInternAtom(dpy, "_NET_WM_STATE", False);
	wmFullscreen = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	#ifndef XA_ATOM
		#define DEFINE_XA_ATOM
		#define XA_ATOM   ((Atom) 4)
	#endif
	XChangeProperty(dpy, pThis->mhWindow, wmState, XA_ATOM, 32,
		PropModeReplace, (unsigned char*)&wmFullscreen, 1);
	#ifdef DEFINE_XA_ATOM
		#undef DEFINE_XA_ATOM
		#undef XA_ATOM
	#endif
	XMapWindow(dpy, pThis->mhWindow);
	XStoreName(dpy, pThis->mhWindow, "GLX Fullscreen");
	return 1;
}
void GLContext_DestroyFullscreenWindow(GLCONTEXT* pThis, Display* dpy)
{ XDestroyWindow(dpy, pThis->mhWindow); }

const char* GLContext_GetNativeExtensions(GLCONTEXT* pThis)
{
	return glXQueryExtensionsString(pThis->mhDisplay,
		DefaultScreen(pThis->mhDisplay));
}

int GLContext_CreateClone(GLCONTEXT* pThis, GLCONTEXTHANDLE clone,
	GLDRAWABLEHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{
	Bool x11Direct = True; /* can return indirect regardless of x11Direct */
	const int* contextattriblist = (!contextattribs
			|| GLContextAttributes_GetCount(contextattribs) == 0) ?
				NULL : GLContextAttributes_Get(contextattribs);
	int* pixelattriblist = (!pixelattribs
			|| GLPixelAttributes_GetCount(pixelattribs) == 0) ?
				NULL : GLPixelAttributes_Get(pixelattribs);
	int renderType = GLX_RGBA_TYPE;
	int needsARB = GLContextAttributes_GetNeedsARB(contextattribs);
	PFNGLXCREATECONTEXTATTRIBSARBPROC pfnGLXCreateContextAttribsARB;
	int (*oldX11ErrorHandler)(Display*, XErrorEvent*);
	const char* exts;

	/* Get the X11 Display from GTK/QT or create it */
	pThis->mhDisplay = GLGetXDisplay();
	if(!pThis->mhDisplay)
	{
		pThis->mhDisplay = XOpenDisplay(NULL);
		if(!pThis->mhDisplay)
			return 0;
		pThis->mWillCloseDisplay = 1;
	}
	else
		pThis->mWillCloseDisplay = 0;

	/* Create the pixel format equivelent */
	if (GLGetGLXVersionWithDisplay(pThis->mhDisplay) >= 13) /* GLX >= 1.3 */
	{
		int returned;
		GLPIXELATTRIBUTES temppa;
		if(!pixelattriblist)
		{ /* glXChooseFBConfig doesn't segfaults on NULL but
			glXGetVisualFromFBConfig will return NULL if attrs were NULL */
			GLPixelAttributes_Create(&temppa);
			GLPixelAttributes_PlatformDefaults(&temppa);
			GLPixelAttributes_EndList(&temppa);
			pixelattriblist = GLPixelAttributes_Get(&temppa);
		}
		pThis->mhSurface = glXChooseFBConfig(pThis->mhDisplay,
			DefaultScreen(pThis->mhDisplay), pixelattriblist, &returned);
		if (pThis->mhSurface)
		{
			XWindowAttributes windowattributes;
			XVisualInfo* candidate;
			GLXFBConfig first;
			int index;
			int matched;
			if (drawable && XGetWindowAttributes(pThis->mhDisplay,
				(Window)drawable, &windowattributes))
			{ /* Match visual to the window to prevent BadMatch on MakeCurrent */
				matched = 0;
				for (index = 0; index < returned && !matched; ++index)
				{
					candidate = glXGetVisualFromFBConfig(pThis->mhDisplay,
						pThis->mhSurface[index]);
					if (candidate)
					{
						matched = candidate->visualid ==
							XVisualIDFromVisual(windowattributes.visual);
						XFree(candidate);
						if (matched)
						{
							first = pThis->mhSurface[0];
							pThis->mhSurface[0] = pThis->mhSurface[index];
							pThis->mhSurface[index] = first;
						}
					}
				}
			}
			GLContext_PixelFormat(pThis) =
				glXGetVisualFromFBConfig(pThis->mhDisplay, *pThis->mhSurface);
			if (!GLContext_PixelFormat(pThis))
			{
				XFree(pThis->mhSurface);
				pThis->mhSurface = NULL;
				if(pThis->mWillCloseDisplay)
					XCloseDisplay(pThis->mhDisplay);
				return 0;
			}
		}
		else
		{
			if(pThis->mWillCloseDisplay)
				XCloseDisplay(pThis->mhDisplay);
			return 0;
		}
	}
	else /* GLX < 1.3 */
	{
		GLPIXELATTRIBUTES temppa;
		if(!pixelattriblist)
		{ /* glXChooseVisual just segfaults on NULL unlike EGL/WGL */
			GLPixelAttributes_Create(&temppa);
			GLPixelAttributes_PlatformDefaults(&temppa);
			GLPixelAttributes_EndList(&temppa);
			pixelattriblist = GLPixelAttributes_Get(&temppa);
		}
		pThis->mhSurface = NULL;
		GLContext_PixelFormat(pThis) = glXChooseVisual(pThis->mhDisplay,
			DefaultScreen(pThis->mhDisplay), pixelattriblist);
		if (!GLContext_PixelFormat(pThis))
		{
			if(pThis->mWillCloseDisplay)
				XCloseDisplay(pThis->mhDisplay);
			return 0;
		}
	}

	/* Create the context */
	if (pThis->mhSurface)
	{ /* If pThis->mhSurface then GLX is >= 1.3 */
		GLXContext tempContext = glXCreateContext(pThis->mhDisplay,
			GLContext_PixelFormat(pThis), NULL, x11Direct);
		if(!tempContext)
		{
			#if 0
				printf("GLContext: Couldn't create temporary context.\n");
			#endif
			XFree(pThis->mhSurface);
			XFree(GLContext_PixelFormat(pThis));
			if(pThis->mWillCloseDisplay)
				XCloseDisplay(pThis->mhDisplay);
			return 0;
		}
		pfnGLXCreateContextAttribsARB = (PFNGLXCREATECONTEXTATTRIBSARBPROC)
			glXGetProcAddress((const GLubyte*)("glXCreateContextAttribsARB"));
		glXDestroyContext( pThis->mhDisplay, tempContext );
	}
	else
		pfnGLXCreateContextAttribsARB = NULL;

	if (!pfnGLXCreateContextAttribsARB && needsARB)
	{
		#if 0
			printf(("Requested attributes require OpenGL 3.0.\n"));
		#endif
		XFree(pThis->mhSurface);
		XFree(GLContext_PixelFormat(pThis));
		if(pThis->mWillCloseDisplay)
			XCloseDisplay(pThis->mhDisplay);
		return 0;
	}

	oldX11ErrorHandler = XSetErrorHandler(&CTXErrorHandler);
	if (pfnGLXCreateContextAttribsARB)
	{ /* > 2.1; GLXCreateContextAttribsARB is OK with NULL attrs*/
		pThis->mhContext = (*pfnGLXCreateContextAttribsARB)(pThis->mhDisplay,
			pThis->mhSurface[0], clone, x11Direct,
			contextattriblist);
		/* ensure queued X11 errors are processed */
		XSync(pThis->mhDisplay, False);
	}
	else
		pThis->mhContext = NULL;
	if (!pThis->mhContext)
	{
		if (pThis->mhSurface)
		{ /* > 1.2 && <= 2.1 */
			pThis->mhContext = glXCreateNewContext(pThis->mhDisplay,
				pThis->mhSurface[0],
				renderType, clone, x11Direct);
		}
		else
		{ /* <= 1.2 */
			pThis->mhContext = glXCreateContext(pThis->mhDisplay,
				GLContext_PixelFormat(pThis),
				None /* shared context */, x11Direct );
		}
		/* ensure queued X11 errors are processed */
		XSync(pThis->mhDisplay, False);
	}
	XSetErrorHandler(oldX11ErrorHandler);
	if(!pThis->mhContext)
	{
		if (pThis->mhSurface)
			XFree(pThis->mhSurface);
		XFree(GLContext_PixelFormat(pThis));
		if(pThis->mWillCloseDisplay)
			XCloseDisplay(pThis->mhDisplay);
		return 0;
	}

	/* Finally, create the fullscreen window if needed */
	pThis->mIsFullscreen = 0;
	if(!drawable)
	{
		if(!GLContext_CreateFullscreenWindow(pThis, pThis->mhDisplay))
		{
			GLContext_Destroy(pThis);
			return 0;
		}
	}
	else
		pThis->mhWindow = drawable;

	/* Obtain Swap Interval get/set routines/capabilities. There are 3 exts:
		XXXEXT, XXXMESA, XXXSGI
		Note that Wayland (XWayland) blocks for a second if the
		drawable becomes completely occluded. Should it be explicitly set
		to 0? By default it should be 0 in GLX. */
	exts = glXQueryExtensionsString(pThis->mhDisplay,
		DefaultScreen(pThis->mhDisplay));
	if(GLContextFindExtension(exts, "GLX_EXT_swap_control"))
	{
		pThis->mSwapIntervalType = GLCONTEXT_SWAPINTERVALTYPE_EXT;
		pThis->mSI.EXT.mpfnglXSwapIntervalEXT = (glXSwapIntervalEXTProc)
			glXGetProcAddress((const GLubyte*)"glXSwapIntervalEXT");
		pThis->mSI.EXT.mSwapInterval = 0;
		pThis->mSI.EXT.mHasGLX_EXT_swap_control_tear =
			GLContextFindExtension(exts, "GLX_EXT_swap_control_tear");
	}
	else if(GLContextFindExtension(exts, "GLX_MESA_swap_control"))
	{
		pThis->mSwapIntervalType = GLCONTEXT_SWAPINTERVALTYPE_MESA;
		pThis->mSI.MESA.mpfnglXSwapIntervalMESA = (glXSwapIntervalMESAProc)
			glXGetProcAddress((const GLubyte*)"glXSwapIntervalMESA");
		pThis->mSI.MESA.mpfnglXGetSwapIntervalMESA = (glXGetSwapIntervalMESAProc)
			glXGetProcAddress((const GLubyte*)"glXGetSwapIntervalMESA");
	}
	else if(GLContextFindExtension(exts, "GLX_SGI_swap_control"))
	{
		pThis->mSwapIntervalType = GLCONTEXT_SWAPINTERVALTYPE_SGI;
		pThis->mSI.SGI.mpfnglXSwapIntervalSGI = (glXSwapIntervalSGIProc)
			glXGetProcAddress((const GLubyte*)"glXSwapIntervalSGI");
		pThis->mSI.SGI.mSwapInterval = 0;
	}
	else
		pThis->mSwapIntervalType = GLCONTEXT_SWAPINTERVALTYPE_NONE;

	/* done */
	return 1;
}
int GLContext_Create(GLCONTEXT* pThis, GLDRAWABLEHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return GLContext_CreateClone(pThis, NULL, drawable,
	contextattribs, pixelattribs); }

void GLContext_Destroy(GLCONTEXT* pThis)
{
	if (pThis->mhContext == glXGetCurrentContext())
		GLSetCurrentContext(NULL, None, pThis->mhDisplay);
	glXDestroyContext(pThis->mhDisplay, pThis->mhContext);
	if (pThis->mhSurface)
		XFree(pThis->mhSurface);
	XFree(GLContext_PixelFormat(pThis));
	if (pThis->mIsFullscreen)
		GLContext_DestroyFullscreenWindow(pThis, pThis->mhDisplay);
}

int GLContext_SwapBuffers(GLCONTEXT* pThis)
{
	glXSwapBuffers(pThis->mhDisplay, pThis->mhWindow);
	return 1;
}

int GLContext_SetCurrent(GLCONTEXT* pThis)
{
	return GLSetCurrentContext(pThis->mhContext, pThis->mhWindow,
		pThis->mhDisplay);
}
int GLContext_SetCurrentHandle(GLCONTEXT* pThis, GLCONTEXTHANDLE handle)
{ return GLSetCurrentContext(handle, pThis->mhWindow,
		pThis->mhDisplay); }

int GLContext_SetDrawable(GLCONTEXT* pThis, GLDRAWABLEHANDLE drawable)
{
	if (!drawable && pThis->mIsFullscreen)
		return 1; /* wants fullscreen and is already fullscreen */
	if (pThis->mhContext == glXGetCurrentContext())
		GLSetCurrentContext(NULL, None, pThis->mhDisplay);
	if (pThis->mIsFullscreen)
		GLContext_DestroyFullscreenWindow(pThis, pThis->mhDisplay);
	pThis->mIsFullscreen = 0;
	if(!drawable)
	{
		if(!GLContext_CreateFullscreenWindow(pThis, pThis->mhDisplay))
			return 0;
	}
	else
		pThis->mhWindow = drawable;
	return 1;
}
GLWINDOWHANDLE GLContext_GetWindowHandle(GLCONTEXT* pThis)
{ return pThis->mhWindow; }
GLCONTEXTHANDLE GLContext_GetCurrent(GLCONTEXT* pThis)
{
	return glXGetCurrentContext();
}

int GLContext_SetSwapInterval(GLCONTEXT* pThis, int interval)
{
	switch(pThis->mSwapIntervalType)
	{
	case GLCONTEXT_SWAPINTERVALTYPE_EXT:
	{
		if(!pThis->mSI.EXT.mpfnglXSwapIntervalEXT
				|| ((interval < 0)
						&& !pThis->mSI.EXT.mHasGLX_EXT_swap_control_tear))
			return 0;
		/* NVIDIA drivers have a bug where glXSwapIntervalEXT caches the wrong
			value so you have to do a double update to force the cache purge.
			LINUX users to use really old NVIDIA drivers so do this always. */
		(*pThis->mSI.EXT.mpfnglXSwapIntervalEXT)(pThis->mhDisplay,
			pThis->mhWindow, GLContext_GetSwapInterval(pThis));
		/* Actually set the interval */
		(*pThis->mSI.EXT.mpfnglXSwapIntervalEXT)(pThis->mhDisplay,
			pThis->mhWindow, interval);
		return 1;
	}
	case GLCONTEXT_SWAPINTERVALTYPE_MESA:
	{
		return (*pThis->mSI.MESA.mpfnglXSwapIntervalMESA)(interval) == 0;
	}
	case GLCONTEXT_SWAPINTERVALTYPE_SGI:
	{
		if((*pThis->mSI.SGI.mpfnglXSwapIntervalSGI)(interval) != 0)
			return 0;
		pThis->mSI.SGI.mSwapInterval = interval;
		return 1;
	}
	default:
		return 0;
	}
}
int GLContext_GetSwapInterval(GLCONTEXT* pThis)
{
	switch(pThis->mSwapIntervalType)
	{
	case GLCONTEXT_SWAPINTERVALTYPE_EXT:
	{
		/* The main swap interval value is
			glXQueryDrawable(GLX_SWAP_INTERVAL_EXT)
			However if negative values are allowed things get tricky
			as different drivers (NVIDIA/MESA) has varying behaviours */
		unsigned int interval;
		int realinterval;
		glXQueryDrawable(pThis->mhDisplay, pThis->mhWindow,
			GLX_SWAP_INTERVAL_EXT, &interval);
		if(pThis->mSI.EXT.mHasGLX_EXT_swap_control_tear)
		{
			unsigned int lateswaptearing = 22; /* invalid value */
			unsigned int moddedlateswaptearing = 22; /* invalid value */
			int isMESA = 0;
			int originterval = (int)interval;
			glXQueryDrawable(pThis->mhDisplay, pThis->mhWindow,
				GLX_LATE_SWAPS_TEAR_EXT, &lateswaptearing);
			(*pThis->mSI.EXT.mpfnglXSwapIntervalEXT)(pThis->mhDisplay,
				pThis->mhWindow, interval); /* clear NVIDIA cache */
			(*pThis->mSI.EXT.mpfnglXSwapIntervalEXT)(pThis->mhDisplay,
				pThis->mhWindow, 0); /* GLX_LATE_SWAPS_TEAR_EXT checking */
			glXQueryDrawable(pThis->mhDisplay, pThis->mhWindow,
				GLX_LATE_SWAPS_TEAR_EXT, &moddedlateswaptearing);
			if(moddedlateswaptearing == 0) /* NVIDIA? */
			{ /* NVIDIA GLX_LATE_SWAPS_TEAR_EXT determines whether
				late swapping is currently in use */
				if(lateswaptearing) /* Need to flip to match NVIDIA val? */
					originterval = -originterval; /* flip it */
			}
			/* In MESA GLX_LATE_SWAPS_TEAR_EXT determines if drawable can... */
			else if(moddedlateswaptearing == 1) /* use late swapping */
				isMESA = 1;
			(*pThis->mSI.EXT.mpfnglXSwapIntervalEXT)(pThis->mhDisplay,
				pThis->mhWindow, originterval); /* restore original interval */

			if(!isMESA && lateswaptearing && interval > 0)
				lateswaptearing = -((int)interval); /* NVIDIA opposite val */
			else
				realinterval = (int)interval;
		}
		else
			realinterval = (int)interval;
		return realinterval;
	}
	case GLCONTEXT_SWAPINTERVALTYPE_MESA:
	{
		return (*pThis->mSI.MESA.mpfnglXGetSwapIntervalMESA)();
	}
	case GLCONTEXT_SWAPINTERVALTYPE_SGI:
	{
		return pThis->mSI.SGI.mSwapInterval;
	}
	default:
		return 0;
	}
}
GLContextEmptyProc GLContext_GetProc(GLCONTEXT* pThis, const char* proc)
{ return glXGetProcAddress((const GLubyte*)proc); }
void GLContext_ResizeDrawable(GLCONTEXT* pThis, int width, int height)
{ (void)(pThis);(void)(width);(void)(height); }
GLDRAWABLEHANDLE GLGetWindowDrawable(GLWINDOWHANDLE window)
{
#if defined(HAVE_QT) && defined(__cplusplus)
	return (GLDRAWABLEHANDLE)((QWidget*)window)->winId();
#elif defined(GDK_CHECK_VERSION)
	#if GDK_CHECK_VERSION(4,0,0)
		if (!GLGetXDisplay())
			return window;
		return (GLDRAWABLEHANDLE)gdk_x11_surface_get_xid(
			gtk_native_get_surface(GTK_NATIVE((GtkWidget*)window)));
	#else
		GdkWindow* gdkwindow;
		GLDRAWABLEHANDLE drawable;
		if (!GLGetXDisplay())
			return window;
		g_object_get((gpointer)window, "window", &gdkwindow, NULL);
		drawable = (GLDRAWABLEHANDLE)GDK_WINDOW_XID(gdkwindow);
		g_object_unref(gdkwindow);
		return drawable;
	#endif
#else
	return window;
#endif
}
int GLContext_CreateCloneWithWindow(GLCONTEXT* pThis, GLCONTEXTHANDLE clone,
	GLWINDOWHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return GLContext_CreateClone(pThis, clone, GLGetWindowDrawable(drawable),
		contextattribs, pixelattribs); }
int GLContext_CreateWithWindow(GLCONTEXT* pThis, GLWINDOWHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return GLContext_Create(pThis, GLGetWindowDrawable(drawable),
	contextattribs, pixelattribs); }

#if defined(HAVE_QT) /* Clear up global namespace for later QT includes */
	#undef CursorShape
	#undef None
	#undef Unsorted
#endif /* HAVE_QT */
#ifdef __cplusplus
	} /* end extern "C" */
#endif
/*=============================================================================


						N/A Not Implemented OSes


=============================================================================*/
#else
#ifdef __cplusplus
	extern "C" {
#endif
/*------------------ GLCONTEXTATTRIBUTES ------------------------------------*/
void GLContextAttributes_CoreProfile(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_MajorVersion(GLCONTEXTATTRIBUTES* pThis, int val)
{ }
void GLContextAttributes_MinorVersion(GLCONTEXTATTRIBUTES* pThis, int val)
{ }
void GLContextAttributes_CompatibilityProfile(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_ForwardCompatible(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_ES2(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_DebugCtx(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_Robust(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_NoResetNotify(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_LoseOnReset(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_ResetIsolation(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_ReleaseFlush(GLCONTEXTATTRIBUTES* pThis, int val)
{ }
void GLContextAttributes_PlatformDefaults(GLCONTEXTATTRIBUTES* pThis)
{ }
void GLContextAttributes_EndList(GLCONTEXTATTRIBUTES* pThis)
{ }
/*------------------ GLPIXELATTRIBUTES --------------------------------------*/
void GLPixelAttributes_AddOrAlter(GLPIXELATTRIBUTES* pThis,
	attrib_t key, attrib_t val)
{
}

void GLPixelAttributes_RGBA(GLPIXELATTRIBUTES* pThis)
{
}
void GLPixelAttributes_BufferSize(GLPIXELATTRIBUTES* pThis, int val)
{
}
void GLPixelAttributes_Level(GLPIXELATTRIBUTES* pThis, int val)
{
}
void GLPixelAttributes_DoubleBuffer(GLPIXELATTRIBUTES* pThis)
{
}

void GLPixelAttributes_Stereo(GLPIXELATTRIBUTES* pThis)
{
}
void GLPixelAttributes_AuxBuffers(GLPIXELATTRIBUTES* pThis, int val)
{
}
void GLPixelAttributes_MinRGBA(GLPIXELATTRIBUTES* pThis,
	int red, int green, int blue, int alpha)
{
}
void GLPixelAttributes_Depth(GLPIXELATTRIBUTES* pThis, int val)
{
	GLPixelAttributes_AddOrAlter(pThis, AMA_NoDepth, FALSE);
}
void GLPixelAttributes_Stencil(GLPIXELATTRIBUTES* pThis, int val)
{
	GLPixelAttributes_AddOrAlter(pThis, AMA_NoStencil, FALSE);
}
void GLPixelAttributes_MinAcumRGBA(GLPIXELATTRIBUTES* pThis,
	int red, int green, int blue, int alpha)
{
}
void GLPixelAttributes_SampleBuffers(GLPIXELATTRIBUTES* pThis, int val)
{ }
void GLPixelAttributes_Samplers(GLPIXELATTRIBUTES* pThis, int val)
{ }

void GLPixelAttributes_FrameBuffersRGB(GLPIXELATTRIBUTES* pThis)
{ }

void GLPixelAttributes_EndList(GLPIXELATTRIBUTES* pThis)
{ }

void GLPixelAttributes_PlatformDefaults(GLPIXELATTRIBUTES* pThis)
{
}
/*-----------------------------------------------------------------------------
						GLCONTEXT
-----------------------------------------------------------------------------*/
int GLContext_CreateClone(GLCONTEXT* pThis, GLCONTEXTHANDLE clone,
	GLDRAWABLEHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return FALSE; }
int GLContext_Create(GLCONTEXT* pThis, GLDRAWABLEHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return FALSE; }
void GLContext_Destroy(GLCONTEXT* pThis)
{ }
int GLContext_SwapBuffers(GLCONTEXT* pThis)
{ return FALSE; }
int GLContext_SetCurrent(GLCONTEXT* pThis)
{ return FALSE; }
int GLContext_SetCurrentHandle(GLCONTEXT* pThis, GLCONTEXTHANDLE handle)
{ return FALSE; }
int GLContext_SetDrawable(GLCONTEXT* pThis, GLDRAWABLEHANDLE drawable)
{ return FALSE; }
GLWINDOWHANDLE GLContext_GetWindowHandle(GLCONTEXT* pThis)
{ return NULL; }
GLCONTEXTHANDLE GLContext_GetCurrent(GLCONTEXT* pThis)
{ return NULL; }
int GLContext_SetSwapInterval(GLCONTEXT* pThis, int interval)
{ return 0; }
int GLContext_GetSwapInterval(GLCONTEXT* pThis)
{ return 0; }
GLContextEmptyProc GLContext_GetProc(GLCONTEXT* pThis, const char* proc)
{ return (GLContextEmptyProc)NULL; }
void GLContext_ResizeDrawable(GLCONTEXT* pThis, int width, int height)
{ (void)(pThis);(void)(width);(void)(height); }
int GLContext_CreateCloneWithWindow(GLCONTEXT* pThis, GLCONTEXTHANDLE clone,
	GLWINDOWHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return GLContext_CreateClone(pThis, clone, drawable,
		contextattribs, pixelattribs); }
int GLContext_CreateWithWindow(GLCONTEXT* pThis, GLWINDOWHANDLE drawable,
	GLCONTEXTATTRIBUTES* contextattribs, GLPIXELATTRIBUTES* pixelattribs)
{ return GLContext_Create(pThis, drawable, contextattribs, pixelattribs); }

#ifdef __cplusplus
	} /* end extern "C" */
#endif
#endif /* OS if/else */
#endif /* GLCONTEXT_IMPLEMENTATION */
