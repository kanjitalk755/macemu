# SheepShaver target (included from root CMakeLists.txt, or standalone)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.16)
  project(SheepShaver VERSION 2.3 LANGUAGES C CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_C_STANDARD 99)
  option(ENABLE_JIT "Enable PPC JIT (precompiled dyngen)" OFF)
  option(ENABLE_GTK "GTK prefs UI (Unix)" OFF)
  option(ENABLE_BINCUE "BIN/CUE support" OFF)
  option(ENABLE_VOSF "Video on SEGV" OFF)
  option(USE_SDL3 "Use SDL3" OFF)
  set(MACEMU_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
  list(APPEND CMAKE_MODULE_PATH "${MACEMU_CMAKE_DIR}")
  include(MacEmuCommon)
  set(B2_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../BasiliskII/src")
  set(SS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  macemu_detect_host()
  macemu_find_sdl()
endif()

if(NOT DEFINED SS_SRC)
  set(SS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
if(NOT DEFINED B2_SRC)
  set(B2_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../BasiliskII/src")
endif()

if(NOT EXISTS "${B2_SRC}/main.cpp")
  message(FATAL_ERROR "Basilisk II sources not found at ${B2_SRC}")
endif()

set(KPX "${SS_SRC}/kpx_cpu")
set(SS_GEN "${CMAKE_CURRENT_BINARY_DIR}/ss_gen")
file(MAKE_DIRECTORY "${SS_GEN}")

if(ENABLE_JIT)
  set(ENABLE_DYNGEN 1)
endif()

macemu_write_config(
  "${CMAKE_CURRENT_BINARY_DIR}"
  "SheepShaver" "SheepShaver" "2.3" "")

# ---------------------------------------------------------------------------
# Include path: SheepShaver/src/include has text "symlink" stubs on Windows
# (tiny files whose content is a relative path). Those must not be visible
# to the compiler. Stage only real SS-specific headers into the build tree;
# shared headers come from BasiliskII.
# ---------------------------------------------------------------------------
set(SS_INC_STAGE "${CMAKE_CURRENT_BINARY_DIR}/ss_include")
file(MAKE_DIRECTORY "${SS_INC_STAGE}")
file(GLOB _ss_hdrs "${SS_SRC}/include/*")
set(_ss_staged 0)
foreach(_h IN LISTS _ss_hdrs)
  if(IS_DIRECTORY "${_h}")
    continue()
  endif()
  get_filename_component(_name "${_h}" NAME)
  file(SIZE "${_h}" _sz)
  set(_is_stub FALSE)
  if(_sz LESS 200)
    file(READ "${_h}" _content LIMIT 200)
    string(STRIP "${_content}" _content)
    if(_content MATCHES "\\.\\./")
      set(_is_stub TRUE)
    endif()
  endif()
  if(NOT _is_stub)
    configure_file("${_h}" "${SS_INC_STAGE}/${_name}" COPYONLY)
    math(EXPR _ss_staged "${_ss_staged}+1")
  endif()
endforeach()
message(STATUS "SheepShaver: staged ${_ss_staged} real headers (skipped text-stubs)")

# ---------------------------------------------------------------------------
# Shared sources (prefer BasiliskII tree; resolve text-symlink stubs)
# ---------------------------------------------------------------------------
set(SS_SHARED)
foreach(f adb.cpp audio.cpp cdrom.cpp disk.cpp extfs.cpp prefs.cpp scsi.cpp
          sony.cpp timer.cpp xpram.cpp)
  macemu_resolve_path("${SS_SRC}/${f}" "${B2_SRC}/${f}" _p)
  list(APPEND SS_SHARED "${_p}")
endforeach()
if(ENABLE_BINCUE)
  macemu_resolve_path("${SS_SRC}/bincue.cpp" "${B2_SRC}/bincue.cpp" _p)
  list(APPEND SS_SHARED "${_p}")
endif()

macemu_slirp_sources("${B2_SRC}" SS_SLIRP)
macemu_sdl_sources("${B2_SRC}" SS_SDL)
macemu_xplat_sources("${B2_SRC}" SS_XPLAT)

set(SS_CORE
  "${SS_SRC}/main.cpp"
  "${SS_SRC}/prefs_items.cpp"
  "${SS_SRC}/rom_patches.cpp"
  "${SS_SRC}/rsrc_patches.cpp"
  "${SS_SRC}/emul_op.cpp"
  "${SS_SRC}/name_registry.cpp"
  "${SS_SRC}/macos_util.cpp"
  "${SS_SRC}/gfxaccel.cpp"
  "${SS_SRC}/video.cpp"
  "${SS_SRC}/ether.cpp"
  "${SS_SRC}/thunks.cpp"
  "${SS_SRC}/serial.cpp"
  "${SS_SRC}/user_strings.cpp"
  ${SS_SHARED}
  ${SS_XPLAT}
  ${SS_SDL}
  ${SS_SLIRP}
)

# PowerPC emulator
set(SS_CPU
  "${KPX}/src/mathlib/ieeefp.cpp"
  "${KPX}/src/mathlib/mathlib.cpp"
  "${KPX}/src/cpu/ppc/ppc-cpu.cpp"
  "${KPX}/src/cpu/ppc/ppc-decode.cpp"
  "${KPX}/src/cpu/ppc/ppc-execute.cpp"
  "${KPX}/src/cpu/ppc/ppc-translate.cpp"
  "${KPX}/src/utils/utils-cpuinfo.cpp"
  "${KPX}/sheepshaver_glue.cpp"
  "${KPX}/ppc-dis.c"
)

set(PRE_PPC_IMPL "${SS_SRC}/Unix/dyngen_precompiled/ppc-execute-impl.cpp")
if(EXISTS "${PRE_PPC_IMPL}")
  configure_file("${PRE_PPC_IMPL}" "${SS_GEN}/ppc-execute-impl.cpp" COPYONLY)
else()
  message(FATAL_ERROR "Missing ${PRE_PPC_IMPL}")
endif()

if(ENABLE_JIT)
  list(APPEND SS_CPU
    "${KPX}/src/cpu/jit/jit-cache.cpp"
    "${KPX}/src/cpu/jit/basic-dyngen.cpp"
    "${KPX}/src/cpu/ppc/ppc-dyngen.cpp"
    "${KPX}/src/cpu/ppc/ppc-jit.cpp")
  if(WIN32 AND EXISTS "${SS_SRC}/Windows/cygwin_precompiled_dyngen/basic-dyngen-ops.hpp")
    set(_ops "${SS_SRC}/Windows/cygwin_precompiled_dyngen")
  else()
    set(_ops "${SS_SRC}/Unix/dyngen_precompiled")
  endif()
  configure_file("${_ops}/basic-dyngen-ops.hpp" "${SS_GEN}/basic-dyngen-ops.hpp" COPYONLY)
  configure_file("${_ops}/ppc-dyngen-ops.hpp" "${SS_GEN}/ppc-dyngen-ops.hpp" COPYONLY)
endif()

if(WIN32)
  macemu_windows_net_sources("${B2_SRC}" SS_NET)

  # Stage Windows headers: real SS ones first; shared headers from B2
  set(SS_WIN_STAGE "${CMAKE_CURRENT_BINARY_DIR}/ss_windows")
  file(MAKE_DIRECTORY "${SS_WIN_STAGE}")
  file(GLOB _ss_win_hdrs "${SS_SRC}/Windows/*.h")
  foreach(_h IN LISTS _ss_win_hdrs)
    get_filename_component(_name "${_h}" NAME)
    file(SIZE "${_h}" _sz)
    set(_is_stub FALSE)
    if(_sz LESS 200)
      file(READ "${_h}" _content LIMIT 200)
      string(STRIP "${_content}" _content)
      if(_content MATCHES "\\.\\./")
        set(_is_stub TRUE)
      endif()
    endif()
    if(NOT _is_stub)
      configure_file("${_h}" "${SS_WIN_STAGE}/${_name}" COPYONLY)
    endif()
  endforeach()

  # Stage Windows .cpp into the build tree so MSVC #include "sysdeps.h" does not
  # pick BasiliskII/src/Windows/sysdeps.h next to the source file.
  set(SS_WIN_SRC_STAGE "${CMAKE_CURRENT_BINARY_DIR}/ss_win_src")
  file(MAKE_DIRECTORY "${SS_WIN_SRC_STAGE}")
  set(SS_PLATFORM)
  foreach(f
      main_windows.cpp prefs_windows.cpp sys_windows.cpp xpram_windows.cpp
      timer_windows.cpp serial_windows.cpp ether_windows.cpp clip_windows.cpp
      extfs_windows.cpp user_strings_windows.cpp util_windows.cpp posix_emu.cpp
      about_window_windows.cpp)
    macemu_resolve_path("${SS_SRC}/Windows/${f}" "${B2_SRC}/Windows/${f}" _p)
    configure_file("${_p}" "${SS_WIN_SRC_STAGE}/${f}" COPYONLY)
    list(APPEND SS_PLATFORM "${SS_WIN_SRC_STAGE}/${f}")
  endforeach()

  foreach(f scsi_dummy.cpp prefs_editor_dummy.cpp prefs_dummy.cpp)
    macemu_resolve_path("${SS_SRC}/dummy/${f}" "${B2_SRC}/dummy/${f}" _p)
    list(APPEND SS_PLATFORM "${_p}")
  endforeach()
  list(APPEND SS_PLATFORM ${SS_NET})

  if(EXISTS "${SS_SRC}/Windows/SheepShaver.rc")
    file(SIZE "${SS_SRC}/Windows/SheepShaver.rc" _rcsz)
    if(_rcsz GREATER 100)
      list(APPEND SS_PLATFORM "${SS_SRC}/Windows/SheepShaver.rc")
    endif()
  endif()

  set(SS_PLATFORM_INC
    "${SS_WIN_STAGE}"
    "${B2_SRC}/Windows"
    "${B2_SRC}/Windows/router"
    "${B2_SRC}/Windows/b2ether")
else()
  set(SS_PLATFORM
    "${SS_SRC}/Unix/main_unix.cpp"
    "${SS_SRC}/Unix/about_window_unix.cpp"
    "${SS_SRC}/dummy/scsi_dummy.cpp"
  )
  foreach(f prefs_unix.cpp sys_unix.cpp xpram_unix.cpp timer_unix.cpp
            serial_unix.cpp ether_unix.cpp extfs_unix.cpp
            user_strings_unix.cpp clip_unix.cpp rpc_unix.cpp
            sshpty.c strlcpy.c tinyxml2.cpp disk_sparsebundle.cpp posix_sem.cpp)
    macemu_resolve_path("${SS_SRC}/Unix/${f}" "${B2_SRC}/Unix/${f}" _p)
    list(APPEND SS_PLATFORM "${_p}")
  endforeach()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    macemu_resolve_path(
      "${SS_SRC}/Unix/Linux/scsi_linux.cpp"
      "${B2_SRC}/Unix/Linux/scsi_linux.cpp" _p)
    list(APPEND SS_PLATFORM "${_p}")
  endif()
  if(ENABLE_GTK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    list(APPEND SS_PLATFORM "${SS_SRC}/Unix/prefs_editor_gtk3.cpp")
  else()
    list(APPEND SS_PLATFORM "${SS_SRC}/dummy/prefs_editor_dummy.cpp")
  endif()
  set(SS_PLATFORM_INC "${SS_SRC}/Unix")
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
# ppc-execute-impl.cpp is #included by ppc-execute.cpp (not a separate TU)
add_executable(SheepShaver
  ${SS_CORE} ${SS_CPU} ${SS_PLATFORM})
# Ensure generated impl is present before compiling ppc-execute.cpp
set_source_files_properties(
  "${KPX}/src/cpu/ppc/ppc-execute.cpp"
  PROPERTIES OBJECT_DEPENDS "${SS_GEN}/ppc-execute-impl.cpp")

target_include_directories(SheepShaver PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${SS_GEN}"
  # Platform sysdeps (Windows/ or Unix/) must come before shared includes
  ${SS_PLATFORM_INC}
  # SS-specific headers only (stubs filtered out into this staged dir)
  "${SS_INC_STAGE}"
  # Shared headers from Basilisk II
  "${B2_SRC}/include"
  "${B2_SRC}/CrossPlatform"
  "${B2_SRC}/slirp"
  "${KPX}/include"
  "${KPX}/src"
)

target_compile_definitions(SheepShaver PRIVATE
  SHEEPSHAVER=1
  POWERPC_ROM=1
  EMULATED_PPC=1
)
if(WIN32)
  target_compile_definitions(SheepShaver PRIVATE CONFIG_WIN32=1 NATMEM_OFFSET=0x11000000)
endif()
if(ENABLE_JIT)
  target_compile_definitions(SheepShaver PRIVATE USE_JIT ENABLE_DYNGEN)
endif()

macemu_apply_common(SheepShaver)

if(ENABLE_GTK AND NOT WIN32)
  target_include_directories(SheepShaver PRIVATE ${GTK3_INCLUDE_DIRS})
  target_link_libraries(SheepShaver PRIVATE ${GTK3_LIBRARIES})
endif()

install(TARGETS SheepShaver RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

message(STATUS "SheepShaver: JIT=${ENABLE_JIT}")
