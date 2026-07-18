dnl Process this file with autoconf to produce a configure script.
dnl Written in 2002 by Christian Bauer

AC_INIT([SheepShaver], 2.3, [Christian.Bauer@uni-mainz.de], SheepShaver)
AC_CONFIG_SRCDIR(main_windows.cpp)
AC_CONFIG_AUX_DIR(../Unix)
AC_PREREQ(2.52)
AC_CONFIG_HEADER(config.h)

dnl Canonical system information.
AC_CANONICAL_HOST
AC_CANONICAL_TARGET

dnl Options.
AC_ARG_ENABLE(jit,          [  --enable-jit            enable JIT compiler [default=yes]], [WANT_JIT=$enableval], [WANT_JIT=yes])
AC_ARG_WITH(gtk,          [  --with-gtk              use GTK user interface [default=yes]], [WANT_GTK=$withval], [WANT_GTK=yes])
AC_ARG_ENABLE(vosf,         [  --enable-vosf           enable video on SEGV signals [default=no]], [WANT_VOSF=$enableval], [WANT_VOSF=no])
AC_ARG_WITH(sdl3,            [  --with-sdl3             use SDL 3.x, rather than SDL 2.x [default=no]], [WANT_SDL_VERSION_MAJOR=3], [WANT_SDL_VERSION_MAJOR=2])
AC_ARG_ENABLE(gfxaccel,     [  --enable-gfxaccel       SheepShaver graphics accel (NQD/RAVE/GL/DSp) [default=yes]], [WANT_GFXACCEL=$enableval], [WANT_GFXACCEL=yes])
AC_ARG_ENABLE(native-cinepak-patch, [  --enable-native-cinepak-patch  patch QuickTime to use native Cinepak decoder [default=no]], [WANT_NATIVE_CINEPAK=$enableval], [WANT_NATIVE_CINEPAK=no])
AC_ARG_ENABLE(qd3d-init-logging,     [  --enable-qd3d-init-logging      compile QuickDraw 3D/RAVE init diagnostics [default=no]], [WANT_QD3D_INIT_LOGGING=$enableval], [WANT_QD3D_INIT_LOGGING=no])
AC_ARG_ENABLE(qd3d-graphics-logging, [  --enable-qd3d-graphics-logging  compile high-volume QD3D graphics diagnostics [default=no]], [WANT_QD3D_GRAPHICS_LOGGING=$enableval], [WANT_QD3D_GRAPHICS_LOGGING=no])
AC_ARG_ENABLE(qd3d-audio-logging,    [  --enable-qd3d-audio-logging     compile sound component and SDL audio diagnostics [default=no]], [WANT_QD3D_AUDIO_LOGGING=$enableval], [WANT_QD3D_AUDIO_LOGGING=no])
AC_ARG_ENABLE(qd3d-media-logging,    [  --enable-qd3d-media-logging     compile CD/media I/O diagnostics [default=no]], [WANT_QD3D_MEDIA_LOGGING=$enableval], [WANT_QD3D_MEDIA_LOGGING=no])
AC_ARG_ENABLE(qd3d-wait-logging,     [  --enable-qd3d-wait-logging      compile high-volume timing/wait diagnostics [default=no]], [WANT_QD3D_WAIT_LOGGING=$enableval], [WANT_QD3D_WAIT_LOGGING=no])

AC_ARG_WITH(bincue,
  AS_HELP_STRING([--with-bincue], [Allow cdrom image files in bin/cue mode]))

dnl Checks for programs.
AC_PROG_CC
AC_PROG_CPP
AC_PROG_CXX
AC_PROG_MAKE_SET
AC_PROG_EGREP
AC_PROG_LN_S
AC_PATH_PROG(PERL, [perl])
AC_CHECK_TOOL(WINDRES, windres)

dnl We use GTK+ if possible.
if [[ "x$WANT_GTK" = "xyes" ]]; then
  AM_PATH_GTK_2_0(1.3.15, [], [
    AC_MSG_WARN([Could not find GTK+ 2.0, disabling user interface.])
    WANT_GTK=no
  ])
fi
AC_SUBST(WANT_GTK)

dnl We use 64-bit file size support if possible.
AC_SYS_LARGEFILE

dnl Checks for header files.
AC_HEADER_STDC

dnl Checks for typedefs, structures, and compiler characteristics.
AC_C_BIGENDIAN
AC_C_CONST
AC_C_INLINE
AC_CHECK_SIZEOF(short, 2)
AC_CHECK_SIZEOF(int, 4)
AC_CHECK_SIZEOF(long, 4)
AC_CHECK_SIZEOF(long long, 8)
AC_CHECK_SIZEOF(float, 4)
AC_CHECK_SIZEOF(double, 8)
AC_CHECK_SIZEOF(void *, 4)
AC_TYPE_OFF_T
AC_CHECK_TYPES(loff_t)
AC_TYPE_SIZE_T

dnl Checks for library functions.
AC_CHECK_FUNCS(strdup strerror)
AC_CHECK_FUNCS(exp2f log2f exp2 log2)
AC_CHECK_FUNCS(floorf roundf ceilf truncf)
AC_CHECK_FUNCS(floor round ceil trunc)

dnl Define a macro that translates a yesno-variable into a C macro definition
dnl to be put into the config.h file
dnl $1 -- the macro to define
dnl $2 -- the value to translate
dnl $3 -- template name
AC_DEFUN([AC_TRANSLATE_DEFINE], [
    if [[ "x$2" = "xyes" -o "x$2" = "xguessing yes" ]]; then
        AC_DEFINE($1, 1, $3)
    fi
])

dnl Check that VirtualAlloc(), VirtualProtect() work
AC_CACHE_CHECK([whether VirtualProtect works],
  ac_cv_VirtualProtect_works, [
  AC_LANG_SAVE
  AC_LANG_CPLUSPLUS
  ac_cv_VirtualProtect_works=yes
  dnl First the tests that should segfault
  for test_def in NONE_READ NONE_WRITE READ_WRITE; do
    AC_TRY_RUN([
      #define HAVE_WIN32_VM 1
      #define CONFIGURE_TEST_VM_MAP
      #define TEST_VM_PROT_$test_def
      #include "../CrossPlatform/vm_alloc.cpp"
    ], ac_cv_VirtualProtect_works=no, rm -f core,
    dnl When cross-compiling, assume it works
    ac_cv_VirtualProtect_works="yes"
    )
  done
  AC_TRY_RUN([
    #define HAVE_WIN32_VM 1
    #define CONFIGURE_TEST_VM_MAP
    #define TEST_VM_PROT_RDWR_WRITE
    #include "../CrossPlatform/vm_alloc.cpp"
  ], , ac_cv_VirtualProtect_works=no,
  dnl When cross-compiling, assume it works
  ac_cv_VirtualProtect_works="yes"
  )
  AC_LANG_RESTORE
  ]
)
if [[ "x$ac_cv_VirtualProtect_works" = "xyes" ]]; then
  AC_DEFINE(HAVE_WIN32_VM, 1, [Define if your system has a working Win32-based memory allocator.])
else
  AC_MSG_ERROR([Sorry, Windows VM functions don't work as expected on your system.])
fi

dnl Check if Windows exceptions are supported.
AC_CACHE_CHECK([whether your system supports Windows exceptions],
  ac_cv_have_win32_exceptions, [
  AC_LANG_SAVE
  AC_LANG_CPLUSPLUS
  AC_TRY_RUN([
    #define HAVE_WIN32_EXCEPTIONS 1
    #define CONFIGURE_TEST_SIGSEGV_RECOVERY
    #include "../CrossPlatform/vm_alloc.cpp"
    #include "../CrossPlatform/sigsegv.cpp"
  ],
  ac_cv_have_win32_exceptions=yes,
  ac_cv_have_win32_exceptions=no,
  dnl When cross-compiling, assume it works
  ac_cv_have_win32_exceptions="yes"
  )
  AC_LANG_RESTORE
  ]
)
if [[ "x$ac_cv_have_win32_exceptions" = "xyes" ]]; then
  AC_DEFINE(HAVE_WIN32_EXCEPTIONS, 1, [Define if your system supports Windows exceptions.])
else
  AC_MSG_ERROR([Sorry, Windows exceptions don't work as expected on your system.])
fi

dnl Check if we can ignore the fault (instruction skipping in SIGSEGV handler)
AC_CACHE_CHECK([whether we can skip instruction in SIGSEGV handler],
  ac_cv_have_skip_instruction, [
  AC_LANG_SAVE
  AC_LANG_CPLUSPLUS
  AC_TRY_RUN([
    #define HAVE_SIGSEGV_SKIP_INSTRUCTION 1
    #define CONFIGURE_TEST_SIGSEGV_RECOVERY
    #include "../CrossPlatform/vm_alloc.cpp"
    #include "../CrossPlatform/sigsegv.cpp"
  ], ac_cv_have_skip_instruction=yes, ac_cv_have_skip_instruction=no,
  dnl When cross-compiling, assume it works
  ac_cv_have_skip_instruction="yes"
  )
  AC_LANG_RESTORE
  ]
)
AC_TRANSLATE_DEFINE(HAVE_SIGSEGV_SKIP_INSTRUCTION, "$ac_cv_have_skip_instruction",
  [Define if we can ignore the fault (instruction skipping in SIGSEGV handler).])

dnl Enable VOSF screen updates with this feature is requested
if [[ "x$WANT_VOSF" = "xyes" ]]; then
    AC_DEFINE(ENABLE_VOSF, 1, [Define if using video enabled on SEGV signals.])
else
    WANT_VOSF=no
fi

dnl Check for GCC 2.7 or higher.
HAVE_GCC27=no
AC_MSG_CHECKING(for GCC 2.7 or higher)
AC_EGREP_CPP(xyes,
[#if __GNUC__ - 1 > 1 || __GNUC_MINOR__ - 1 > 5
  xyes
#endif
], [AC_MSG_RESULT(yes); HAVE_GCC27=yes], AC_MSG_RESULT(no))

dnl Check for GCC 3.0 or higher.
HAVE_GCC30=no
AC_MSG_CHECKING(for GCC 3.0 or higher)
AC_EGREP_CPP(xyes,
[#if __GNUC__ >= 3
  xyes
#endif
], [AC_MSG_RESULT(yes); HAVE_GCC30=yes], AC_MSG_RESULT(no))

dnl Add -fno-strict-aliasing for slirp sources
if [[ "x$HAVE_GCC30" = "xyes" ]]; then
  SAVED_CFLAGS="$CFLAGS"
  CFLAGS="$CFLAGS -fno-strict-aliasing"
  AC_CACHE_CHECK([whether the compiler supports -fno-strict-aliasing],
    ac_cv_gcc_no_strict_aliasing, [
    AC_TRY_COMPILE([],[],
      [ac_cv_gcc_no_strict_aliasing=yes],
      [ac_cv_gcc_no_strict_aliasing=no])
  ])
  CFLAGS="$SAVED_CFLAGS"
  if test "x$ac_cv_gcc_no_strict_aliasing" = xyes; then
       AC_SUBST(SLIRP_CFLAGS, "-fno-strict-aliasing")
  fi
fi

case $host_os in
cygwin)
	CFLAGS="$CFLAGS -mwin32"
	CXXFLAGS="$CXXFLAGS -mwin32"
	;;
esac

dnl BINCUE
AS_IF([test  "x$with_bincue" = "xyes" ], [have_bincue=yes], [have_bincue=no])
AS_IF([test  "x$have_bincue" = "xyes" ], [
   DEFINES="$DEFINES -DBINCUE"
   CPPFLAGS="$CPPFLAGS -DBINCUE"
   AC_SUBST(USE_BINCUE, yes)
], [AC_SUBST(USE_BINCUE, no)])

dnl BINCUE overrides
if [[ "x$have_bincue" = "xyes" ]]; then
  EXTRASRCS="$EXTRASRCS bincue.cpp"
fi

dnl CPU emulator sources
CPUSRCS="\
    ../kpx_cpu/src/mathlib/ieeefp.cpp \
    ../kpx_cpu/src/mathlib/mathlib.cpp \
    ../kpx_cpu/src/cpu/ppc/ppc-cpu.cpp \
    ../kpx_cpu/src/cpu/ppc/ppc-decode.cpp \
    ../kpx_cpu/src/cpu/ppc/ppc-execute.cpp \
    ../kpx_cpu/src/cpu/ppc/ppc-translate.cpp \
    ../kpx_cpu/src/utils/utils-cpuinfo.cpp"
CPPFLAGS="$CPPFLAGS -I../kpx_cpu/include -I../kpx_cpu/src"


case "$target_cpu" in
x86_64* )
  CXXFLAGS="$CXXFLAGS -DMEM_BULK -fpermissive"
  WANT_JIT="no"
  ;;
esac

dnl Enable JIT compiler, if possible
USE_DYNGEN="no"
USE_PREGENERATED_DYNGEN="no"
if [[ "x$WANT_JIT" = "xyes" ]]; then
  case $host_cpu in
  i?86)
    DYNGEN_OP_FLAGS="-fomit-frame-pointer -mpreferred-stack-boundary=2"
    if [[ "x$HAVE_GCC30" = "xyes" ]]; then
      DYNGEN_OP_FLAGS="$DYNGEN_OP_FLAGS -falign-functions=0"
    else
      DYNGEN_OP_FLAGS="$DYNGEN_OP_FLAGS -malign-functions=0"
    fi
    ;;
  esac

  case $host_os in
  cygwin)
	USE_PREGENERATED_DYNGEN="no"
	;;
  *)
	USE_PREGENERATED_DYNGEN="yes"
	;;
  esac

  USE_DYNGEN="yes"
  DYNGEN_OP_FLAGS="$DYNGEN_OP_FLAGS -finline-limit=10000 -g0"
  if [[ "x$HAVE_GCC30" = "xyes" ]]; then
    DYNGEN_OP_FLAGS="$DYNGEN_OP_FLAGS -fno-reorder-blocks -fno-optimize-sibling-calls"
  fi
  AC_DEFINE(ENABLE_DYNGEN, 1, [Define to enable dyngen engine])
  DYNGENSRCS="\
    ../kpx_cpu/src/cpu/jit/dyngen.c \
    ../kpx_cpu/src/cpu/jit/cxxdemangle.cpp"
  CPUSRCS="\
    ../kpx_cpu/src/cpu/jit/jit-cache.cpp \
    ../kpx_cpu/src/cpu/jit/basic-dyngen.cpp \
    ../kpx_cpu/src/cpu/ppc/ppc-dyngen.cpp \
    ../kpx_cpu/src/cpu/ppc/ppc-jit.cpp $CPUSRCS"
  CPPFLAGS="$CPPFLAGS -DUSE_JIT"
fi
CPUSRCS="$CPUSRCS ../kpx_cpu/sheepshaver_glue.cpp ../kpx_cpu/ppc-dis.c"

dnl Use the dummy prefs file.
CPUSRCS="$CPUSRCS ../dummy/prefs_dummy.cpp"

dnl We really want SDL for now
if [[ "x$WANT_SDL_VERSION_MAJOR" = "x3" ]]; then
  SDL_CFLAGS=`pkg-config sdl3 --cflags`
  SDL_LIBS=`pkg-config sdl3 --libs | sed -e 's/-lSDL3/-lSDL3.dll/'`
  AC_DEFINE([USE_SDL3], [1], [Use SDL3])
else
  SDL_CFLAGS=`sdl2-config --cflags`
  SDL_LIBS=`sdl2-config --static-libs`
  AC_DEFINE([USE_SDL2], [1], [Use SDL2])
fi
AC_SUBST(SDL_CFLAGS)
AC_SUBST(SDL_LIBS)
AC_DEFINE(USE_SDL, 1, [Define to enable SDL support])
AC_DEFINE(USE_SDL_VIDEO, 1, [Define to enable SDL video graphics support])
AC_DEFINE(USE_SDL_AUDIO, 1, [Define to enable SDL audio support])

dnl SheepShaver graphics acceleration (NQD/RAVE/GL/DSp).
dnl Mirrors SheepShaver/CMakeLists.txt (OpenGL backend; Metal is Apple-only and
dnl not built here). Keep the source list and defines in sync with CMake.
if [[ "x$WANT_GFXACCEL" = "xyes" ]]; then
  AC_DEFINE(ENABLE_GFXACCEL, 1, [Define to enable SheepShaver graphics acceleration.])
  AC_DEFINE(GFXACCEL_USE_OPENGL, 1, [Define to select the OpenGL gfxaccel backend.])
  CPPFLAGS="$CPPFLAGS -I../gfxaccel -I../gfxaccel/include -I../gfxaccel/gl"
  dnl Portable engine / policy / dispatch sources.
  GFXACCEL_SRCS="../gfxaccel/display_mode_controller.cpp ../gfxaccel/dsp_dispatch.cpp \
../gfxaccel/dsp_engine.cpp ../gfxaccel/dsp_install_hooks.cpp ../gfxaccel/dsp_mode_enumerate.cpp \
../gfxaccel/dsp_thunks.cpp ../gfxaccel/gfxaccel_resources.cpp ../gfxaccel/gfxaccel_resources_heap.cpp \
../gfxaccel/gl_defer.cpp ../gfxaccel/gl_dispatch.cpp ../gfxaccel/gl_engine.cpp ../gfxaccel/gl_state.cpp \
../gfxaccel/gl_thunks.cpp ../gfxaccel/rave_dispatch.cpp ../gfxaccel/rave_draw_context.cpp \
../gfxaccel/rave_engine.cpp ../gfxaccel/rave_thunks.cpp"
  dnl OpenGL backend renderers / device / compositor.
  GFXACCEL_SRCS="$GFXACCEL_SRCS ../gfxaccel/gl/gl_device.cpp ../gfxaccel/gl/gl_compositor.cpp \
../gfxaccel/gl/vbl_source_sdl.cpp ../gfxaccel/gl/gfxaccel_resources_gl.cpp \
../gfxaccel/gl/gfxaccel_resources_heap_gl.cpp ../gfxaccel/gl/gfxaccel_arc_shim.cpp \
../gfxaccel/gl/pso_archive_gl.cpp ../gfxaccel/gl/nqd_gl_renderer.cpp ../gfxaccel/gl/rave_gl_renderer.cpp \
../gfxaccel/gl/gl_ffp_renderer.cpp ../gfxaccel/gl/dsp_gl_renderer.cpp ../gfxaccel/gl/dsp_draw_context_gl.cpp \
../gfxaccel/gl/gfxaccel_link_stubs.cpp"
  if [[ "x$WANT_NATIVE_CINEPAK" = "xyes" ]]; then
    GFXACCEL_SRCS="$GFXACCEL_SRCS ../gfxaccel/cinepak_decoder.cpp ../gfxaccel/cinepak_hooks.cpp"
  fi
  EXTRASRCS="$EXTRASRCS $GFXACCEL_SRCS"
  LIBS="$LIBS -lopengl32"
fi
if [[ "x$WANT_NATIVE_CINEPAK" = "xyes" ]]; then
  AC_DEFINE(ENABLE_NATIVE_CINEPAK_PATCH, 1, [Define to patch QuickTime to use the native Cinepak decoder.])
fi
if [[ "x$WANT_QD3D_INIT_LOGGING" = "xyes" ]]; then
  AC_DEFINE(QD3D_INIT_LOGGING_ENABLED, 1, [Define to compile QuickDraw 3D/RAVE init diagnostics.])
fi
if [[ "x$WANT_QD3D_GRAPHICS_LOGGING" = "xyes" ]]; then
  AC_DEFINE(QD3D_GRAPHICS_LOGGING_ENABLED, 1, [Define to compile high-volume QD3D graphics diagnostics.])
fi
if [[ "x$WANT_QD3D_AUDIO_LOGGING" = "xyes" ]]; then
  AC_DEFINE(QD3D_AUDIO_LOGGING_ENABLED, 1, [Define to compile sound component and SDL audio diagnostics.])
fi
if [[ "x$WANT_QD3D_MEDIA_LOGGING" = "xyes" ]]; then
  AC_DEFINE(QD3D_MEDIA_LOGGING_ENABLED, 1, [Define to compile CD/media I/O diagnostics.])
fi
if [[ "x$WANT_QD3D_WAIT_LOGGING" = "xyes" ]]; then
  AC_DEFINE(QD3D_WAIT_LOGGING_ENABLED, 1, [Define to compile high-volume timing/wait diagnostics.])
fi

dnl Remove the "-g" option if set for GCC.
if [[ "x$HAVE_GCC27" = "xyes" ]]; then
  CFLAGS=`echo $CFLAGS | sed -e 's/-g\b//g'`
  CXXFLAGS=`echo $CXXFLAGS | sed -e 's/-g\b//g'`
fi

dnl Generate Makefile.
AC_SUBST(PERL)
AC_SUBST(USE_DYNGEN)
AC_SUBST(USE_PREGENERATED_DYNGEN)
AC_SUBST(DYNGENSRCS)
AC_SUBST(DYNGEN_OP_FLAGS)
AC_SUBST(CPUSRCS)
AC_SUBST(EXTRASRCS)
AC_OUTPUT([Makefile])

dnl Print summary.
echo
echo SheepShaver configuration summary:
echo
echo SDL major-version ................ : $WANT_SDL_VERSION_MAJOR
echo Enable JIT compiler .............. : $WANT_JIT
echo GTK user interface ............... : $WANT_GTK
echo BINCUE support ................... : $have_bincue
echo Enable VOSF ...................... : $WANT_VOSF
echo Graphics acceleration ............ : $WANT_GFXACCEL
echo Native Cinepak patch ............. : $WANT_NATIVE_CINEPAK
echo QD3D logging (init/gfx/audio/media/wait) : $WANT_QD3D_INIT_LOGGING/$WANT_QD3D_GRAPHICS_LOGGING/$WANT_QD3D_AUDIO_LOGGING/$WANT_QD3D_MEDIA_LOGGING/$WANT_QD3D_WAIT_LOGGING
echo
echo "Configuration done. Now type \"make\"."
