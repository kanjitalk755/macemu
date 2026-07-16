# Shared CMake helpers for Basilisk II and SheepShaver.
# Included from the root CMakeLists.txt (or from a standalone subproject).

include(CheckIncludeFile)
include(CheckTypeSize)
include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Host / feature probes
# ---------------------------------------------------------------------------
function(macemu_detect_host)
  check_type_size("short" SIZEOF_SHORT)
  check_type_size("int" SIZEOF_INT)
  check_type_size("long" SIZEOF_LONG)
  check_type_size("long long" SIZEOF_LONG_LONG)
  check_type_size("void *" SIZEOF_VOID_P)
  check_type_size("float" SIZEOF_FLOAT)
  check_type_size("double" SIZEOF_DOUBLE)
  check_type_size("long double" SIZEOF_LONG_DOUBLE)

  if(NOT SIZEOF_SHORT)
    set(SIZEOF_SHORT 2)
  endif()
  if(NOT SIZEOF_INT)
    set(SIZEOF_INT 4)
  endif()
  if(NOT SIZEOF_LONG)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT WIN32)
      set(SIZEOF_LONG 8)
    else()
      set(SIZEOF_LONG 4)
    endif()
  endif()
  if(NOT SIZEOF_LONG_LONG)
    set(SIZEOF_LONG_LONG 8)
  endif()
  if(NOT SIZEOF_VOID_P)
    set(SIZEOF_VOID_P ${CMAKE_SIZEOF_VOID_P})
  endif()
  if(NOT SIZEOF_FLOAT)
    set(SIZEOF_FLOAT 4)
  endif()
  if(NOT SIZEOF_DOUBLE)
    set(SIZEOF_DOUBLE 8)
  endif()
  if(NOT SIZEOF_LONG_DOUBLE)
    set(SIZEOF_LONG_DOUBLE 8)
  endif()

  check_include_file(unistd.h HAVE_UNISTD_H)
  check_include_file(strings.h HAVE_STRINGS_H)
  check_include_file(fenv.h HAVE_FENV_H)

  set(USE_SDL 1)
  set(USE_SDL_VIDEO 1)
  set(USE_SDL_AUDIO 1)
  set(HAVE_SLIRP 1)

  if(WIN32)
    set(HAVE_WIN32_VM 1)
    set(HAVE_WIN32_EXCEPTIONS 1)
    set(HAVE_SIGSEGV_SKIP_INSTRUCTION 1)
  endif()
  if(ENABLE_VOSF)
    set(ENABLE_VOSF 1)
  endif()
  if(ENABLE_BINCUE)
    set(BINCUE 1)
  endif()

  # Export to parent scope
  foreach(v
      SIZEOF_SHORT SIZEOF_INT SIZEOF_LONG SIZEOF_LONG_LONG SIZEOF_VOID_P
      SIZEOF_FLOAT SIZEOF_DOUBLE SIZEOF_LONG_DOUBLE
      HAVE_UNISTD_H HAVE_STRINGS_H HAVE_FENV_H
      USE_SDL USE_SDL_VIDEO USE_SDL_AUDIO HAVE_SLIRP
      HAVE_WIN32_VM HAVE_WIN32_EXCEPTIONS HAVE_SIGSEGV_SKIP_INSTRUCTION
      ENABLE_VOSF BINCUE)
    if(DEFINED ${v})
      set(${v} "${${v}}" PARENT_SCOPE)
    endif()
  endforeach()
endfunction()

# ---------------------------------------------------------------------------
# SDL
# ---------------------------------------------------------------------------
function(macemu_find_sdl)
  if(USE_SDL3)
    find_package(SDL3 CONFIG REQUIRED)
    set(MACEMU_SDL_TARGET SDL3::SDL3 PARENT_SCOPE)
    set(USE_SDL3 1 PARENT_SCOPE)
    return()
  endif()

  find_package(SDL2 CONFIG QUIET)
  if(NOT SDL2_FOUND AND DEFINED ENV{SDL2DIR})
    list(APPEND CMAKE_PREFIX_PATH "$ENV{SDL2DIR}")
    find_package(SDL2 CONFIG QUIET)
  endif()

  if(SDL2_FOUND)
    if(TARGET SDL2::SDL2)
      set(MACEMU_SDL_TARGET SDL2::SDL2 PARENT_SCOPE)
    elseif(TARGET SDL2::SDL2-static)
      set(MACEMU_SDL_TARGET SDL2::SDL2-static PARENT_SCOPE)
    endif()
    if(TARGET SDL2::SDL2main)
      set(MACEMU_SDL_MAIN SDL2::SDL2main PARENT_SCOPE)
    endif()
  else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(SDL2 REQUIRED sdl2)
      set(MACEMU_SDL_PKG 1 PARENT_SCOPE)
      set(SDL2_INCLUDE_DIRS "${SDL2_INCLUDE_DIRS}" PARENT_SCOPE)
      set(SDL2_LIBRARIES "${SDL2_LIBRARIES}" PARENT_SCOPE)
    else()
      find_path(SDL2_INCLUDE_DIR SDL.h
        PATHS ENV SDL2DIR
        PATH_SUFFIXES include include/SDL2)
      find_library(SDL2_LIBRARY NAMES SDL2
        PATHS ENV SDL2DIR
        PATH_SUFFIXES lib lib/x64 lib/x86)
      find_library(SDL2MAIN_LIBRARY NAMES SDL2main
        PATHS ENV SDL2DIR
        PATH_SUFFIXES lib lib/x64 lib/x86)
      if(NOT SDL2_INCLUDE_DIR OR NOT SDL2_LIBRARY)
        message(FATAL_ERROR
          "SDL2 not found. Install SDL2 dev files or pass -DSDL2_DIR=... / set SDL2DIR")
      endif()
      set(SDL2_INCLUDE_DIR "${SDL2_INCLUDE_DIR}" PARENT_SCOPE)
      set(SDL2_LIBRARY "${SDL2_LIBRARY}" PARENT_SCOPE)
      set(SDL2MAIN_LIBRARY "${SDL2MAIN_LIBRARY}" PARENT_SCOPE)
    endif()
  endif()

  # <SDL2/SDL.h> (Linux) vs <SDL.h> (official VC zip)
  find_path(_SDL2_NESTED_INC SDL2/SDL.h
    HINTS ${SDL2_INCLUDE_DIRS} ${SDL2_INCLUDE_DIR}
    PATHS ENV SDL2DIR
    PATH_SUFFIXES include)
  if(_SDL2_NESTED_INC)
    set(USE_SDL2 1 PARENT_SCOPE)
  endif()
endfunction()

function(macemu_link_sdl target)
  if(MACEMU_SDL_MAIN)
    target_link_libraries(${target} PRIVATE ${MACEMU_SDL_MAIN})
  endif()
  if(MACEMU_SDL_TARGET)
    target_link_libraries(${target} PRIVATE ${MACEMU_SDL_TARGET})
  elseif(MACEMU_SDL_PKG)
    target_include_directories(${target} PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(${target} PRIVATE ${SDL2_LIBRARIES})
  else()
    target_include_directories(${target} PRIVATE ${SDL2_INCLUDE_DIR})
    if(SDL2MAIN_LIBRARY)
      target_link_libraries(${target} PRIVATE ${SDL2MAIN_LIBRARY})
    endif()
    target_link_libraries(${target} PRIVATE ${SDL2_LIBRARY})
  endif()
endfunction()

# ---------------------------------------------------------------------------
# config.h generation
# ---------------------------------------------------------------------------
function(macemu_write_config out_dir package tarname version bugreport)
  set(MACEMU_PACKAGE "${package}")
  set(MACEMU_TARNAME "${tarname}")
  set(MACEMU_VERSION "${version}")
  set(MACEMU_BUGREPORT "${bugreport}")
  configure_file(
    "${MACEMU_CMAKE_DIR}/config.h.in"
    "${out_dir}/config.h"
    @ONLY
  )
endfunction()

# ---------------------------------------------------------------------------
# Shared source lists (live under BasiliskII/src, reused by SheepShaver)
# ---------------------------------------------------------------------------
function(macemu_slirp_sources b2_src outvar)
  set(srcs
    "${b2_src}/slirp/bootp.c"
    "${b2_src}/slirp/cksum.c"
    "${b2_src}/slirp/debug.c"
    "${b2_src}/slirp/if.c"
    "${b2_src}/slirp/ip_icmp.c"
    "${b2_src}/slirp/ip_input.c"
    "${b2_src}/slirp/ip_output.c"
    "${b2_src}/slirp/mbuf.c"
    "${b2_src}/slirp/misc.c"
    "${b2_src}/slirp/sbuf.c"
    "${b2_src}/slirp/slirp.c"
    "${b2_src}/slirp/socket.c"
    "${b2_src}/slirp/tcp_input.c"
    "${b2_src}/slirp/tcp_output.c"
    "${b2_src}/slirp/tcp_subr.c"
    "${b2_src}/slirp/tcp_timer.c"
    "${b2_src}/slirp/tftp.c"
    "${b2_src}/slirp/udp.c"
  )
  set(${outvar} "${srcs}" PARENT_SCOPE)
endfunction()

function(macemu_sdl_sources b2_src outvar)
  set(srcs
    "${b2_src}/SDL/video_sdl.cpp"
    "${b2_src}/SDL/video_sdl2.cpp"
    "${b2_src}/SDL/video_sdl3.cpp"
    "${b2_src}/SDL/audio_sdl.cpp"
    "${b2_src}/SDL/audio_sdl3.cpp"
  )
  set(${outvar} "${srcs}" PARENT_SCOPE)
endfunction()

function(macemu_xplat_sources b2_src outvar)
  set(srcs
    "${b2_src}/CrossPlatform/vm_alloc.cpp"
    "${b2_src}/CrossPlatform/sigsegv.cpp"
    "${b2_src}/CrossPlatform/video_blit.cpp"
  )
  set(${outvar} "${srcs}" PARENT_SCOPE)
endfunction()

# Windows router / ether / cdenable (shared between both emulators)
function(macemu_windows_net_sources b2_src outvar)
  set(srcs
    "${b2_src}/Windows/cdenable/cache.cpp"
    "${b2_src}/Windows/cdenable/eject_nt.cpp"
    "${b2_src}/Windows/cdenable/ntcd.cpp"
    "${b2_src}/Windows/b2ether/packet32.cpp"
    "${b2_src}/Windows/router/arp.cpp"
    "${b2_src}/Windows/router/dump.cpp"
    "${b2_src}/Windows/router/dynsockets.cpp"
    "${b2_src}/Windows/router/ftp.cpp"
    "${b2_src}/Windows/router/icmp.cpp"
    "${b2_src}/Windows/router/iphelp.cpp"
    "${b2_src}/Windows/router/ipsocket.cpp"
    "${b2_src}/Windows/router/mib/interfaces.cpp"
    "${b2_src}/Windows/router/mib/mibaccess.cpp"
    "${b2_src}/Windows/router/router.cpp"
    "${b2_src}/Windows/router/tcp.cpp"
    "${b2_src}/Windows/router/udp.cpp"
  )
  set(${outvar} "${srcs}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Common compile / link settings for an emulator executable
# ---------------------------------------------------------------------------
function(macemu_apply_common target)
  target_compile_definitions(${target} PRIVATE
    HAVE_CONFIG_H
    _REENTRANT
    DIRECT_ADDRESSING
  )

  if(WIN32)
    target_compile_definitions(${target} PRIVATE
      WIN32
      _WINDOWS
      NOMINMAX
      _CRT_SECURE_NO_WARNINGS
      _CRT_NONSTDC_NO_WARNINGS
      _WIN32_WINNT=0x0601
      WINVER=0x0601
    )
  endif()

  # MSVC compatibility for POSIX-ish code (slirp, fcntl flags, strdup, alloca)
  if(MSVC)
    target_compile_definitions(${target} PRIVATE
      __STDC__
      _CRT_DECLARE_NONSTDC_NAMES=1
    )
    # strdup / alloca live under different names in the UCRT
    target_compile_options(${target} PRIVATE
      "/FImalloc.h"
    )
    target_compile_definitions(${target} PRIVATE
      "strdup=_strdup"
      "alloca=_alloca"
    )
  endif()

  if(ENABLE_BINCUE)
    target_compile_definitions(${target} PRIVATE BINCUE)
  endif()

  # GCC-style asm flags only on non-MSVC (MSVC uses MSVC_INTRINSICS instead)
  if(NOT MSVC)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
      target_compile_definitions(${target} PRIVATE X86_64_ASSEMBLY OPTIMIZED_FLAGS)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i[3-6]86|x86|X86")
      target_compile_definitions(${target} PRIVATE X86_ASSEMBLY OPTIMIZED_FLAGS SAHF_SETO_PROFITABLE)
    endif()
  elseif(WIN32)
    target_compile_definitions(${target} PRIVATE
      MSVC_INTRINSICS
      OPTIMIZED_FLAGS
      SAHF_SETO_PROFITABLE
      UNALIGNED_PROFITABLE
    )
  endif()

  macemu_link_sdl(${target})

  if(WIN32)
    target_link_libraries(${target} PRIVATE ws2_32 iphlpapi winmm)
    if(MSVC)
      set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)
      target_compile_options(${target} PRIVATE /bigobj /wd4102 /wd4244 /wd4267 /wd4996)
    endif()
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(${target} PRIVATE Threads::Threads)
    if(UNIX AND NOT APPLE)
      target_link_libraries(${target} PRIVATE dl m)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Resolve "text symlink" paths used by SheepShaver checkouts on Windows
# ---------------------------------------------------------------------------
function(macemu_resolve_path preferred fallback outvar)
  if(EXISTS "${preferred}" AND NOT IS_DIRECTORY "${preferred}")
    file(SIZE "${preferred}" _sz)
    if(_sz LESS 200)
      file(READ "${preferred}" _content)
      string(STRIP "${_content}" _content)
      if(_content MATCHES "\\.\\./")
        get_filename_component(_dir "${preferred}" DIRECTORY)
        get_filename_component(_resolved "${_dir}/${_content}" ABSOLUTE)
        if(EXISTS "${_resolved}")
          set(${outvar} "${_resolved}" PARENT_SCOPE)
          return()
        endif()
      endif()
    endif()
  endif()
  if(EXISTS "${preferred}")
    set(${outvar} "${preferred}" PARENT_SCOPE)
  elseif(EXISTS "${fallback}")
    set(${outvar} "${fallback}" PARENT_SCOPE)
  else()
    message(FATAL_ERROR "Missing source: ${preferred} (also tried ${fallback})")
  endif()
endfunction()
