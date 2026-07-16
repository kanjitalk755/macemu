# Basilisk II target (included from root CMakeLists.txt, or standalone)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.16)
  project(BasiliskII VERSION 1.0 LANGUAGES C CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_C_STANDARD 99)
  option(ENABLE_JIT "Enable UAE JIT compiler" ON)
  option(ENABLE_GTK "GTK prefs UI (Unix)" OFF)
  option(ENABLE_BINCUE "BIN/CUE support" ON)
  option(ENABLE_VOSF "Video on SEGV" OFF)
  option(USE_SDL3 "Use SDL3" OFF)
  set(MACEMU_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
  list(APPEND CMAKE_MODULE_PATH "${MACEMU_CMAKE_DIR}")
  include(MacEmuCommon)
  set(B2_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  macemu_detect_host()
  macemu_find_sdl()
endif()

if(NOT DEFINED B2_SRC)
  set(B2_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|ARM|aarch64|AARCH64)")
  set(UAE_DIR "${B2_SRC}/uae_cpu_2021")
else()
  set(UAE_DIR "${B2_SRC}/uae_cpu")
endif()

set(B2_GEN "${CMAKE_CURRENT_BINARY_DIR}/uae_gen")
file(MAKE_DIRECTORY "${B2_GEN}")

macemu_write_config(
  "${CMAKE_CURRENT_BINARY_DIR}"
  "Basilisk II" "BasiliskII" "1.0" "Christian.Bauer@uni-mainz.de")

if(WIN32)
  set(B2_SYSDEPS "${B2_SRC}/Windows")
else()
  set(B2_SYSDEPS "${B2_SRC}/Unix")
endif()

# ---------------------------------------------------------------------------
# UAE codegen host tools
# ---------------------------------------------------------------------------
add_executable(build68k "${UAE_DIR}/build68k.c")
target_include_directories(build68k PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}" "${B2_SYSDEPS}" "${B2_SRC}/include" "${UAE_DIR}")
target_compile_definitions(build68k PRIVATE HAVE_CONFIG_H)
if(MSVC)
  target_compile_definitions(build68k PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_custom_command(
  OUTPUT "${B2_GEN}/cpudefs.cpp"
  COMMAND ${CMAKE_COMMAND}
    -DTOOL=$<TARGET_FILE:build68k>
    -DARGS=${UAE_DIR}/table68k
    -DOUT=${B2_GEN}/cpudefs.cpp
    -P ${MACEMU_CMAKE_DIR}/run_redirect.cmake
  DEPENDS build68k "${UAE_DIR}/table68k"
  COMMENT "Generating cpudefs.cpp"
  VERBATIM
)

add_library(uae_readcpu STATIC "${UAE_DIR}/readcpu.cpp" "${B2_GEN}/cpudefs.cpp")
target_include_directories(uae_readcpu PUBLIC
  "${CMAKE_CURRENT_BINARY_DIR}" "${B2_SYSDEPS}" "${B2_SRC}/include" "${UAE_DIR}" "${B2_GEN}")
target_compile_definitions(uae_readcpu PUBLIC HAVE_CONFIG_H)
if(MSVC)
  target_compile_definitions(uae_readcpu PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(gencpu "${UAE_DIR}/gencpu.c")
target_link_libraries(gencpu PRIVATE uae_readcpu)
target_compile_definitions(gencpu PRIVATE HAVE_CONFIG_H)
if(MSVC)
  target_compile_definitions(gencpu PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set(UAE_GEN_SRCS
  "${B2_GEN}/cpuemu.cpp" "${B2_GEN}/cpustbl.cpp" "${B2_GEN}/cputbl.h"
  "${B2_GEN}/cpuemu_nf.cpp" "${B2_GEN}/cpustbl_nf.cpp")
add_custom_command(
  OUTPUT ${UAE_GEN_SRCS}
  COMMAND gencpu
  DEPENDS gencpu "${B2_GEN}/cpudefs.cpp"
  WORKING_DIRECTORY "${B2_GEN}"
  COMMENT "Running gencpu"
  VERBATIM
)

set(JIT_GEN_SRCS)
if(ENABLE_JIT)
  add_executable(gencomp "${UAE_DIR}/compiler/gencomp.c")
  target_link_libraries(gencomp PRIVATE uae_readcpu)
  target_include_directories(gencomp PRIVATE "${UAE_DIR}/compiler" "${UAE_DIR}")
  target_compile_definitions(gencomp PRIVATE HAVE_CONFIG_H)
  if(MSVC)
    target_compile_definitions(gencomp PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  set(JIT_GEN_SRCS
    "${B2_GEN}/compemu.cpp" "${B2_GEN}/compstbl.cpp" "${B2_GEN}/comptbl.h")
  add_custom_command(
    OUTPUT ${JIT_GEN_SRCS}
    COMMAND gencomp
    DEPENDS gencomp "${B2_GEN}/cpudefs.cpp"
    WORKING_DIRECTORY "${B2_GEN}"
    COMMENT "Running gencomp"
    VERBATIM
  )
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
macemu_slirp_sources("${B2_SRC}" B2_SLIRP)
macemu_sdl_sources("${B2_SRC}" B2_SDL)
macemu_xplat_sources("${B2_SRC}" B2_XPLAT)

set(B2_CORE
  "${B2_SRC}/main.cpp"
  "${B2_SRC}/prefs.cpp"
  "${B2_SRC}/prefs_items.cpp"
  "${B2_SRC}/rom_patches.cpp"
  "${B2_SRC}/slot_rom.cpp"
  "${B2_SRC}/rsrc_patches.cpp"
  "${B2_SRC}/emul_op.cpp"
  "${B2_SRC}/macos_util.cpp"
  "${B2_SRC}/xpram.cpp"
  "${B2_SRC}/timer.cpp"
  "${B2_SRC}/adb.cpp"
  "${B2_SRC}/serial.cpp"
  "${B2_SRC}/ether.cpp"
  "${B2_SRC}/sony.cpp"
  "${B2_SRC}/disk.cpp"
  "${B2_SRC}/cdrom.cpp"
  "${B2_SRC}/scsi.cpp"
  "${B2_SRC}/video.cpp"
  "${B2_SRC}/audio.cpp"
  "${B2_SRC}/extfs.cpp"
  "${B2_SRC}/user_strings.cpp"
  ${B2_XPLAT}
  ${B2_SDL}
)

set(B2_UAE
  "${UAE_DIR}/basilisk_glue.cpp"
  "${UAE_DIR}/memory.cpp"
  "${UAE_DIR}/newcpu.cpp"
  "${UAE_DIR}/readcpu.cpp"
  "${UAE_DIR}/fpu/fpu_ieee.cpp"
  "${B2_GEN}/cpudefs.cpp"
  "${B2_GEN}/cpuemu.cpp"
  "${B2_GEN}/cpustbl.cpp"
)
if(ENABLE_JIT)
  list(APPEND B2_UAE
    "${B2_GEN}/cpuemu_nf.cpp" "${B2_GEN}/cpustbl_nf.cpp"
    "${B2_GEN}/compemu.cpp" "${B2_GEN}/compstbl.cpp"
    "${UAE_DIR}/compiler/compemu_support.cpp"
    "${UAE_DIR}/compiler/compemu_fpp.cpp")
endif()

if(WIN32)
  macemu_windows_net_sources("${B2_SRC}" B2_NET)
  set(B2_PLATFORM
    "${B2_SRC}/Windows/main_windows.cpp"
    "${B2_SRC}/Windows/prefs_windows.cpp"
    "${B2_SRC}/Windows/sys_windows.cpp"
    "${B2_SRC}/Windows/xpram_windows.cpp"
    "${B2_SRC}/Windows/timer_windows.cpp"
    "${B2_SRC}/Windows/serial_windows.cpp"
    "${B2_SRC}/Windows/ether_windows.cpp"
    "${B2_SRC}/Windows/clip_windows.cpp"
    "${B2_SRC}/Windows/extfs_windows.cpp"
    "${B2_SRC}/Windows/user_strings_windows.cpp"
    "${B2_SRC}/Windows/util_windows.cpp"
    "${B2_SRC}/Windows/posix_emu.cpp"
    "${B2_SRC}/dummy/scsi_dummy.cpp"
    "${B2_SRC}/dummy/prefs_editor_dummy.cpp"
    ${B2_NET}
    "${B2_SRC}/Windows/BasiliskII.rc"
  )
  set(B2_PLATFORM_INC
    "${B2_SRC}/Windows" "${B2_SRC}/Windows/router" "${B2_SRC}/Windows/b2ether")
else()
  set(B2_PLATFORM
    "${B2_SRC}/Unix/main_unix.cpp"
    "${B2_SRC}/Unix/prefs_unix.cpp"
    "${B2_SRC}/Unix/sys_unix.cpp"
    "${B2_SRC}/Unix/xpram_unix.cpp"
    "${B2_SRC}/Unix/timer_unix.cpp"
    "${B2_SRC}/Unix/serial_unix.cpp"
    "${B2_SRC}/Unix/ether_unix.cpp"
    "${B2_SRC}/Unix/extfs_unix.cpp"
    "${B2_SRC}/Unix/user_strings_unix.cpp"
    "${B2_SRC}/Unix/clip_unix.cpp"
    "${B2_SRC}/Unix/rpc_unix.cpp"
    "${B2_SRC}/Unix/sshpty.c"
    "${B2_SRC}/Unix/strlcpy.c"
    "${B2_SRC}/Unix/tinyxml2.cpp"
    "${B2_SRC}/Unix/disk_sparsebundle.cpp"
    "${B2_SRC}/Unix/posix_sem.cpp"
    "${B2_SRC}/dummy/scsi_dummy.cpp"
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND B2_PLATFORM "${B2_SRC}/Unix/Linux/scsi_linux.cpp")
  endif()
  if(ENABLE_GTK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
    list(APPEND B2_PLATFORM "${B2_SRC}/Unix/prefs_editor_gtk3.cpp")
  else()
    list(APPEND B2_PLATFORM "${B2_SRC}/dummy/prefs_editor_dummy.cpp")
  endif()
  set(B2_PLATFORM_INC "${B2_SRC}/Unix")
endif()

if(ENABLE_BINCUE)
  list(APPEND B2_CORE "${B2_SRC}/bincue.cpp")
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(BasiliskII
  ${B2_CORE} ${B2_SLIRP} ${B2_UAE} ${B2_PLATFORM} ${UAE_GEN_SRCS} ${JIT_GEN_SRCS})

target_include_directories(BasiliskII PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${B2_SRC}/include"
  "${B2_SRC}/CrossPlatform"
  "${B2_SRC}/slirp"
  "${UAE_DIR}"
  "${B2_GEN}"
  ${B2_PLATFORM_INC}
)

target_compile_definitions(BasiliskII PRIVATE FPU_IEEE UNALIGNED_PROFITABLE)
if(ENABLE_JIT)
  target_compile_definitions(BasiliskII PRIVATE USE_JIT USE_JIT_FPU)
endif()

macemu_apply_common(BasiliskII)

if(ENABLE_GTK AND NOT WIN32)
  target_include_directories(BasiliskII PRIVATE ${GTK3_INCLUDE_DIRS})
  target_link_libraries(BasiliskII PRIVATE ${GTK3_LIBRARIES})
  target_compile_options(BasiliskII PRIVATE ${GTK3_CFLAGS_OTHER})
endif()

install(TARGETS BasiliskII RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(EXISTS "${B2_SRC}/SDL/keycodes")
  install(FILES "${B2_SRC}/SDL/keycodes" DESTINATION ${CMAKE_INSTALL_DATADIR}/BasiliskII)
endif()

message(STATUS "Basilisk II: UAE=${UAE_DIR} JIT=${ENABLE_JIT}")
