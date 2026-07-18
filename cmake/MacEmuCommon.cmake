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
  check_include_file(stdint.h HAVE_STDINT_H)
  check_include_file(stdlib.h HAVE_STDLIB_H)
  check_include_file(string.h HAVE_STRING_H)
  check_include_file(memory.h HAVE_MEMORY_H)
  check_include_file(sys/types.h HAVE_SYS_TYPES_H)
  check_include_file(sys/stat.h HAVE_SYS_STAT_H)
  check_include_file(sys/ioctl.h HAVE_SYS_IOCTL_H)
  check_include_file(fcntl.h HAVE_FCNTL_H)
  check_include_file(sys/time.h HAVE_SYS_TIME_H)
  check_include_file(sys/ioctl.h HAVE_SYS_IOCTL_H)
  check_include_file(sys/socket.h HAVE_SYS_SOCKET_H)
  check_include_file(sys/mman.h HAVE_SYS_MMAN_H)
  check_include_file(sys/select.h HAVE_SYS_SELECT_H)
  check_include_file(sys/poll.h HAVE_SYS_POLL_H)
  check_include_file(sys/wait.h HAVE_SYS_WAIT_H)
  check_include_file(sys/filio.h HAVE_SYS_FILIO_H)
  check_include_file(arpa/inet.h HAVE_ARPA_INET_H)
  check_include_file(stropts.h HAVE_STROPTS_H)
  check_include_file(sys/stropts.h HAVE_SYS_STROPTS_H)
  check_include_file(pty.h HAVE_PTY_H)
  check_include_file(util.h HAVE_UTIL_H)

  include(CheckFunctionExists)
  include(CheckSymbolExists)
  check_function_exists(strdup HAVE_STRDUP)
  check_function_exists(strerror HAVE_STRERROR)
  check_function_exists(cfmakeraw HAVE_CFMAKERAW)
  check_symbol_exists(nanosleep time.h HAVE_NANOSLEEP)
  check_symbol_exists(clock_gettime time.h HAVE_CLOCK_GETTIME)
  check_symbol_exists(clock_nanosleep time.h HAVE_CLOCK_NANOSLEEP)
  check_symbol_exists(getpagesize unistd.h HAVE_GETPAGESIZE)
  check_symbol_exists(sigaction signal.h HAVE_SIGACTION)
  check_symbol_exists(mmap sys/mman.h HAVE_MMAP)
  check_symbol_exists(mprotect sys/mman.h HAVE_MPROTECT_FUNC)

  set(USE_SDL 1)
  set(USE_SDL_VIDEO 1)
  set(USE_SDL_AUDIO 1)
  set(HAVE_SLIRP 1)

  if(WIN32)
    set(HAVE_WIN32_VM 1)
    set(HAVE_WIN32_EXCEPTIONS 1)
    set(HAVE_SIGSEGV_SKIP_INSTRUCTION 1)
  else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES
       "^(i[3-6]86|x86|x86_64|amd64|AMD64|arm|ARM|aarch64|AARCH64|ppc|powerpc|ppc64|p
pc64le|mips|mips64|sparc|sparc64|ia64)")
      set(HAVE_SIGSEGV_SKIP_INSTRUCTION 1)
    endif()
    # SIGSEGV recovery mechanism (mirrors BasiliskII/src/Unix/configure.ac).
    # sigsegv.cpp gates its fault-handler definitions on HAVE_SIGINFO_T /
    # HAVE_SIGCONTEXT_SUBTERFUGE; without one of them the whole handler block
    # is preprocessed out (undefined SIGSEGV_FAULT_HANDLER_ARGLIST/_ADDRESS).
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles("
      #include <signal.h>
      #include <sys/types.h>
      static void handler(int, siginfo_t *sip, void *) {
        void *addr = sip->si_addr;
        (void)addr;
      }
      int main() {
        struct sigaction sa;
        sa.sa_sigaction = handler;
        sa.sa_flags = SA_SIGINFO;
        return sigaction(SIGSEGV, &sa, 0);
      }" HAVE_SIGINFO_T)
    if(HAVE_SIGINFO_T)
      set(HAVE_SIGINFO_T 1)
    else()
      # Fallback: sigcontext subterfuge (older/other platforms).
      check_cxx_source_compiles("
        #include <signal.h>
        static void handler(int, struct sigcontext scs) {
          (void)scs.cr2;
        }
        int main() {
          signal(SIGSEGV, (void (*)(int))handler);
          return 0;
        }" HAVE_SIGCONTEXT_SUBTERFUGE)
      if(HAVE_SIGCONTEXT_SUBTERFUGE)
        set(HAVE_SIGCONTEXT_SUBTERFUGE 1)
      endif()
    endif()
    find_package(Threads QUIET)
    if(CMAKE_USE_PTHREADS_INIT)
      set(HAVE_PTHREADS 1)
      set(CMAKE_REQUIRED_LIBRARIES Threads::Threads)
      check_symbol_exists(pthread_cancel pthread.h HAVE_PTHREAD_CANCEL)
      check_symbol_exists(pthread_testcancel pthread.h HAVE_PTHREAD_TESTCANCEL)
      check_symbol_exists(pthread_cond_init pthread.h HAVE_PTHREAD_COND_INIT)
      check_symbol_exists(pthread_mutexattr_setprotocol pthread.h
        HAVE_PTHREAD_MUTEXATTR_SETPROTOCOL)
      check_symbol_exists(pthread_mutexattr_settype pthread.h
        HAVE_PTHREAD_MUTEXATTR_SETTYPE)
      unset(CMAKE_REQUIRED_LIBRARIES)
    endif()
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
      HAVE_STDINT_H HAVE_STDLIB_H HAVE_STRING_H HAVE_MEMORY_H
      HAVE_SYS_TYPES_H HAVE_SYS_STAT_H
      HAVE_FCNTL_H HAVE_SYS_TIME_H HAVE_SYS_IOCTL_H HAVE_SYS_SOCKET_H
      HAVE_SYS_MMAN_H HAVE_SYS_SELECT_H HAVE_SYS_POLL_H HAVE_SYS_WAIT_H
      HAVE_SYS_FILIO_H HAVE_ARPA_INET_H HAVE_STROPTS_H HAVE_SYS_STROPTS_H
      HAVE_PTY_H HAVE_UTIL_H
      HAVE_STRDUP HAVE_STRERROR
      HAVE_PTHREADS HAVE_PTHREAD_CANCEL HAVE_PTHREAD_TESTCANCEL
      HAVE_PTHREAD_COND_INIT HAVE_PTHREAD_MUTEXATTR_SETPROTOCOL
      HAVE_PTHREAD_MUTEXATTR_SETTYPE HAVE_CFMAKERAW
      HAVE_NANOSLEEP HAVE_CLOCK_GETTIME HAVE_CLOCK_NANOSLEEP
      HAVE_GETPAGESIZE HAVE_SIGACTION
      USE_SDL USE_SDL_VIDEO USE_SDL_AUDIO HAVE_SLIRP
      HAVE_WIN32_VM HAVE_WIN32_EXCEPTIONS HAVE_SIGSEGV_SKIP_INSTRUCTION
      HAVE_SIGINFO_T HAVE_SIGCONTEXT_SUBTERFUGE
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
    if(WIN32)
      get_target_property(_macemu_sdl_type ${MACEMU_SDL_TARGET} TYPE)
      if(_macemu_sdl_type STREQUAL "SHARED_LIBRARY" OR
         _macemu_sdl_type STREQUAL "MODULE_LIBRARY")
        add_custom_command(TARGET ${target} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  "$<TARGET_FILE:${MACEMU_SDL_TARGET}>"
                  "$<TARGET_FILE_DIR:${target}>"
          COMMENT "Copy SDL runtime next to ${target}"
          VERBATIM)
      endif()
    endif()
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
  # Prefer paths under a sibling tree when callers pass them via
  # macemu_resolve_path; this helper alone always uses BasiliskII.
  set(srcs
    "${b2_src}/CrossPlatform/vm_alloc.cpp"
    "${b2_src}/CrossPlatform/sigsegv.cpp"
    "${b2_src}/CrossPlatform/video_blit.cpp"
  )
  set(${outvar} "${srcs}" PARENT_SCOPE)
endfunction()

# Resolve CrossPlatform TUs the same way as other shared SheepShaver sources:
# SheepShaver/src/CrossPlatform/<f> may be a text stub ("../../../BasiliskII/...")
# or a real override. Never compile a stale full copy the IDE shows while the
# build silently uses BasiliskII - resolve_path picks the real file.
function(macemu_xplat_sources_resolved ss_src b2_src outvar)
  set(srcs)
  foreach(f vm_alloc.cpp sigsegv.cpp video_blit.cpp)
    macemu_resolve_path("${ss_src}/CrossPlatform/${f}" "${b2_src}/CrossPlatform/${f}" _p)
    list(APPEND srcs "${_p}")
  endforeach()
  set(${outvar} "${srcs}" PARENT_SCOPE)
endfunction()

# Warn (or fail) when a SheepShaver path looks like a full source file while the
# build actually compiles the BasiliskII twin - classic "MSVC didn't pick up my
# edit" trap (text stubs are OK; large non-stub files are not).
function(macemu_check_shared_source_traps ss_src b2_src)
  set(_trap_files
    CrossPlatform/sigsegv.cpp
    CrossPlatform/vm_alloc.cpp
    CrossPlatform/video_blit.cpp
    bincue.cpp
    cdrom.cpp
    disk.cpp
    prefs.cpp
    Windows/sys_windows.cpp
    Windows/clip_windows.cpp
    Windows/posix_emu.cpp
  )
  set(_traps)
  foreach(rel ${_trap_files})
    set(_ss "${ss_src}/${rel}")
    set(_b2 "${b2_src}/${rel}")
    if(EXISTS "${_ss}" AND EXISTS "${_b2}" AND NOT IS_DIRECTORY "${_ss}")
      file(SIZE "${_ss}" _sz)
      if(_sz GREATER_EQUAL 200)
        file(READ "${_ss}" _content LIMIT 200)
        string(STRIP "${_content}" _content)
        if(NOT _content MATCHES "\\.\\./")
          # Full local copy. If it differs from B2, edits to either side confuse.
          file(SHA256 "${_ss}" _hss)
          file(SHA256 "${_b2}" _hb2)
          if(NOT _hss STREQUAL _hb2)
            list(APPEND _traps "${_ss} (differs from ${_b2})")
          endif()
        endif()
      endif()
    endif()
  endforeach()
  if(_traps)
    message(WARNING
      "Shared sources under SheepShaver differ from BasiliskII twins.\n"
      "CMake may compile one path while you edit the other in the IDE:\n"
      "  ${_traps}\n"
      "Prefer a text stub (relative path to BasiliskII) unless this is an intentional SS-only override "
      "listed explicitly in SheepShaver/CMakeLists.txt.")
  endif()
endfunction()

# Windows router / ether / cdenable (shared between both emulators)
function(macemu_windows_net_sources b2_src outvar)
  set(srcs
    "${b2_src}/Windows/cdenable/cache.cpp"
    "${b2_src}/Windows/cdenable/eject_nt.cpp"
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
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND srcs "${b2_src}/Windows/cdenable/ntcd.cpp")
  endif()
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
  if(NOT WIN32)
    target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
  endif()

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
    target_compile_options(${target} PRIVATE /D__WIN32__)
    target_link_libraries(${target} PRIVATE ws2_32 iphlpapi winmm)
    if(MSVC)
      set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)
      target_compile_options(${target} PRIVATE /bigobj /wd4102 /wd4244 /wd4267 /wd4996 /Zc:__STDC__)
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
