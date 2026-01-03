dnl Process this file with autoconf to produce a configure script.
dnl Written in 2025

AC_INIT([macemu-webrtc], [1.0], [])
AC_CONFIG_SRCDIR([server/server.cpp])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([.])
AC_CONFIG_MACRO_DIRS([m4])
AC_PREREQ([2.69])

dnl Canonicalize system
AC_CANONICAL_HOST
AC_CANONICAL_TARGET

dnl Checks for programs.
AC_PROG_CXX
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_MAKE_SET

dnl Require C++17
AC_LANG([C++])
AX_CXX_COMPILE_STDCXX([17], [noext], [mandatory])

dnl Checks for header files.
AC_CHECK_HEADERS([pthread.h])

dnl Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_SIZE_T

dnl Checks for library functions.
AC_CHECK_FUNCS([mmap munmap])

dnl Check for pkg-config
AC_CHECK_PROG([PKG_CONFIG], [pkg-config], [pkg-config])
if test "x$PKG_CONFIG" = "x"; then
  AC_MSG_ERROR([pkg-config is required. Please install pkg-config.])
fi

dnl Check for OpenSSL
AC_MSG_CHECKING([for OpenSSL])
OPENSSL_CFLAGS=$($PKG_CONFIG --cflags openssl 2>/dev/null)
OPENSSL_LIBS=$($PKG_CONFIG --libs openssl 2>/dev/null)
if test "x$OPENSSL_LIBS" = "x"; then
  OPENSSL_LIBS="-lssl -lcrypto"
fi
if $PKG_CONFIG --exists openssl 2>/dev/null; then
  AC_MSG_RESULT([yes])
  AC_DEFINE([HAVE_OPENSSL], [1], [Define if OpenSSL is available])
else
  AC_MSG_ERROR([openssl not found. Install with: apt install libssl-dev (provides pkg-config openssl)])
fi
AC_SUBST([OPENSSL_CFLAGS])
AC_SUBST([OPENSSL_LIBS])

dnl Check for OpenH264
AC_MSG_CHECKING([for OpenH264])
OPENH264_CFLAGS=$($PKG_CONFIG --cflags openh264 2>/dev/null)
OPENH264_LIBS=$($PKG_CONFIG --libs openh264 2>/dev/null)
if test "x$OPENH264_LIBS" = "x"; then
  OPENH264_LIBS="-lopenh264"
fi
if $PKG_CONFIG --exists openh264 2>/dev/null; then
  AC_MSG_RESULT([yes])
  AC_DEFINE([HAVE_OPENH264], [1], [Define if OpenH264 is available])
else
  AC_MSG_ERROR([openh264 not found. Install with: apt install libopenh264-dev (provides pkg-config openh264)])
fi
AC_SUBST([OPENH264_CFLAGS])
AC_SUBST([OPENH264_LIBS])

dnl Check for libyuv
AC_CHECK_HEADER([libyuv.h], [], [
  AC_MSG_ERROR([libyuv.h not found. Please install libyuv-dev.])
])
AC_CHECK_LIB([yuv], [I420Copy], [], [
  AC_MSG_ERROR([libyuv library not found. Please install libyuv-dev.])
])

dnl Check for Opus
AC_MSG_CHECKING([for Opus])
OPUS_CFLAGS=$($PKG_CONFIG --cflags opus 2>/dev/null)
OPUS_LIBS=$($PKG_CONFIG --libs opus 2>/dev/null)
if test "x$OPUS_LIBS" = "x"; then
  OPUS_LIBS="-lopus"
fi
if $PKG_CONFIG --exists opus 2>/dev/null; then
  AC_MSG_RESULT([yes])
  AC_DEFINE([HAVE_OPUS], [1], [Define if Opus is available])
else
  AC_MSG_ERROR([opus not found. Install with: apt install libopus-dev])
fi
AC_SUBST([OPUS_CFLAGS])
AC_SUBST([OPUS_LIBS])

dnl Check for SVT-AV1
AC_MSG_CHECKING([for SVT-AV1])
SVTAV1_CFLAGS=$($PKG_CONFIG --cflags SvtAv1Enc 2>/dev/null)
SVTAV1_LIBS=$($PKG_CONFIG --libs SvtAv1Enc 2>/dev/null)
if test "x$SVTAV1_LIBS" = "x"; then
  SVTAV1_CFLAGS="-I/usr/include/svt-av1 -DEB_DLL"
  SVTAV1_LIBS="-lSvtAv1Enc"
fi
if test -f "/usr/include/svt-av1/EbSvtAv1Enc.h" || $PKG_CONFIG --exists SvtAv1Enc 2>/dev/null; then
  AC_MSG_RESULT([yes])
  AC_DEFINE([HAVE_SVTAV1], [1], [Define if SVT-AV1 is available])
else
  AC_MSG_ERROR([SVT-AV1 not found. Install with: apt install libsvtav1-dev libsvtav1enc-dev])
fi
AC_SUBST([SVTAV1_CFLAGS])
AC_SUBST([SVTAV1_LIBS])

dnl Check for libvpx (VP9 encoder)
AC_MSG_CHECKING([for libvpx])
VPX_CFLAGS=$($PKG_CONFIG --cflags vpx 2>/dev/null)
VPX_LIBS=$($PKG_CONFIG --libs vpx 2>/dev/null)
if test "x$VPX_LIBS" = "x"; then
  VPX_LIBS="-lvpx"
fi
if $PKG_CONFIG --exists vpx 2>/dev/null; then
  AC_MSG_RESULT([yes])
  AC_DEFINE([HAVE_VPX], [1], [Define if libvpx is available])
else
  AC_MSG_ERROR([libvpx not found. Install with: apt install libvpx-dev])
fi
AC_SUBST([VPX_CFLAGS])
AC_SUBST([VPX_LIBS])

dnl Check for pthread
AC_CHECK_LIB([pthread], [pthread_create], [], [
  AC_MSG_ERROR([pthread library not found.])
])

dnl Check for rt (POSIX realtime extensions)
AC_CHECK_LIB([rt], [shm_open], [], [
  AC_MSG_ERROR([librt not found.])
])

dnl Check for libdatachannel (hard requirement - user must build it)
LIBDATACHANNEL_DIR="libdatachannel"
LIBDATACHANNEL_BUILD="${LIBDATACHANNEL_DIR}/build"
LIBDATACHANNEL_LIB="${LIBDATACHANNEL_BUILD}/libdatachannel.a"
LIBDATACHANNEL_INCLUDES="-I${LIBDATACHANNEL_DIR}/include -I${LIBDATACHANNEL_DIR}/deps/json/include -Iserver"

AC_MSG_CHECKING([for libdatachannel])
if test -f "$LIBDATACHANNEL_LIB"; then
  AC_MSG_RESULT([yes])
  HAVE_LIBDATACHANNEL=yes
else
  AC_MSG_RESULT([no])
  AC_MSG_ERROR([libdatachannel not found. You must build it first.

Please run:
  git submodule update --init --recursive
  mkdir -p libdatachannel/build
  cd libdatachannel/build
  cmake .. -DCMAKE_BUILD_TYPE=Release -DNO_WEBSOCKET=OFF -DNO_MEDIA=OFF -DNO_EXAMPLES=ON -DNO_TESTS=ON -DUSE_GNUTLS=OFF -DUSE_MBEDTLS=OFF -DUSE_NICE=OFF -DBUILD_SHARED_LIBS=OFF
  make -j\$(nproc)
  cd ../..
Then re-run: ./configure
])
fi

dnl Check for fpng sources
AC_MSG_CHECKING([for fpng sources])
if test -f "server/fpng.cpp" -a -f "server/fpng.h"; then
  AC_MSG_RESULT([yes])
else
  AC_MSG_ERROR([fpng sources not found. Please ensure server/fpng.cpp and server/fpng.h exist.])
fi

dnl libdatachannel dependencies (static libraries built with libdatachannel)
LIBJUICE="${LIBDATACHANNEL_BUILD}/deps/libjuice/libjuice.a"
LIBSRTP="${LIBDATACHANNEL_BUILD}/deps/libsrtp/libsrtp2.a"
USRSCTP="${LIBDATACHANNEL_BUILD}/deps/usrsctp/usrsctplib/libusrsctp.a"

dnl Compiler flags for debugging (full debug symbols, no optimization)
CXXFLAGS="$CXXFLAGS -Wall -Wextra -g3 -O0 -fPIC -fno-omit-frame-pointer -rdynamic"

dnl fpng flags (SSE4.1 and pclmul for fast CRC32)
FPNG_CFLAGS="-msse4.1 -mpclmul"

dnl Combined flags for server
SERVER_CFLAGS="$LIBDATACHANNEL_INCLUDES $OPENH264_CFLAGS $SVTAV1_CFLAGS $OPENSSL_CFLAGS $FPNG_CFLAGS"
SERVER_LIBS="$LIBDATACHANNEL_LIB $LIBJUICE $LIBSRTP $USRSCTP $OPENH264_LIBS $OPENSSL_LIBS -lyuv -lpthread -lrt $OPUS_LIBS $SVTAV1_LIBS $VPX_LIBS"

dnl Substitutions
AC_SUBST([SERVER_CFLAGS])
AC_SUBST([SERVER_LIBS])
AC_SUBST([FPNG_CFLAGS])
AC_SUBST([LIBDATACHANNEL_DIR])
AC_SUBST([LIBDATACHANNEL_BUILD])
AC_SUBST([LIBDATACHANNEL_LIB])
AC_SUBST([HAVE_LIBDATACHANNEL])
AC_SUBST([CMAKE])

dnl Output files
AC_CONFIG_FILES([Makefile])
AC_OUTPUT

dnl Print summary
echo
echo "Configuration summary:"
echo "======================"
echo "C++ Compiler ..................... : $CXX"
echo "C++ Flags ........................ : $CXXFLAGS"
echo "OpenSSL .......................... : yes"
echo "OpenH264 ......................... : yes"
echo "SVT-AV1 .......................... : yes"
echo "libvpx (VP9) ..................... : yes"
echo "Opus ............................. : yes"
echo "libyuv ........................... : yes"
echo "libdatachannel ................... : $HAVE_LIBDATACHANNEL"
if test "x$HAVE_LIBDATACHANNEL" = "xno"; then
echo "  (will be built during make)"
fi
echo
echo "Configuration done. Now type 'make'."
