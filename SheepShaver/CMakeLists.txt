# SheepShaver target (included from root CMakeLists.txt, or standalone)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	cmake_minimum_required(VERSION 3.16)
	project(SheepShaver VERSION 2.3 LANGUAGES C CXX)
	set(CMAKE_CXX_STANDARD 20)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
	set(CMAKE_C_STANDARD 99)
	option(ENABLE_JIT "Legacy default for the PPC JIT" OFF)
	option(ENABLE_SHEEPSHAVER_JIT "Enable PPC JIT (precompiled dyngen)" ${ENABLE_JIT})
	option(ENABLE_GTK "GTK prefs UI (Unix)" OFF)
	option(ENABLE_BINCUE "BIN/CUE support" ON)
	option(ENABLE_VOSF "Video on SEGV" OFF)
	option(ENABLE_QD3D_INIT_LOGGING
	"Compile focused QuickDraw 3D/RAVE initialization diagnostics" OFF)
	option(ENABLE_QD3D_GRAPHICS_LOGGING
	"Compile high-volume QD3D graphics diagnostics" OFF)
	option(ENABLE_QD3D_AUDIO_LOGGING
	"Compile sound component and SDL audio diagnostics" OFF)
	option(ENABLE_QD3D_MEDIA_LOGGING
	"Compile CD/media I/O diagnostics" OFF)
	option(ENABLE_QD3D_WAIT_LOGGING
	"Compile high-volume timing/wait diagnostics" OFF)
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

if(ENABLE_SHEEPSHAVER_JIT)
	set(ENABLE_DYNGEN 1)
endif()


if(WIN32)
	set(SS_SYSDEPS "${SS_SRC}/Windows")
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin|OSX")
	set(SS_SYSDEPS "${SS_SRC}/MacOSX")
else()
	set(SS_SYSDEPS "${SS_SRC}/Unix")
endif()
macemu_write_config(
	"${SS_SYSDEPS}"
	"SheepShaver" "SheepShaver" "2.3" "")
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	target_include_directories(SheepShaver PRIVATE "${SS_SYSDEPS}")
endif()




# ---------------------------------------------------------------------------
# Staging helper
#
# Some SS paths are text "symlink" stubs on Windows; we stage real files into
# the build tree. configure_file(COPYONLY) only runs at *cmake configure*
# time, so MSVC/Ninja incremental builds keep using stale staged copies.
# Always stage with add_custom_command(DEPENDS <source>) so a source edit
# re-copies and recompiles. SheepShaver_stage is an explicit dependency of
# the executable so VS/CMake IDE builds cannot skip the copy step.
# ---------------------------------------------------------------------------
set(SS_STAGE_OUTPUTS)
function(ss_stage_file SRC DST)
	if(NOT EXISTS "${SRC}")
	message(FATAL_ERROR "ss_stage_file: missing source ${SRC}")
	endif()
	get_filename_component(_dst_dir "${DST}" DIRECTORY)
	file(MAKE_DIRECTORY "${_dst_dir}")
	# Seed at configure so the IDE can open the path before the first build.
	if(NOT EXISTS "${DST}")
	file(COPY "${SRC}" DESTINATION "${_dst_dir}")
	endif()
	add_custom_command(
	OUTPUT "${DST}"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC}" "${DST}"
	DEPENDS "${SRC}"
	COMMENT "Stage ${DST}"
	VERBATIM)
	# Propagate the growing list back to the caller (function scope isolation).
	set(_ss_stage_acc ${SS_STAGE_OUTPUTS})
	list(APPEND _ss_stage_acc "${DST}")
	set(SS_STAGE_OUTPUTS "${_ss_stage_acc}" PARENT_SCOPE)
endfunction()

# Build-tree include overlays must never be independent copies of editable
# source headers. Prefer an NTFS hard link so following an include in Visual
# Studio edits the tracked source itself. If the build and source directories
# are on different volumes, use a forwarding header; the compiler then tracks
# the real source header as a dependency and changes are still immediate.
set(SS_IDE_HEADERS)
function(ss_overlay_file SRC DST)
	if(NOT EXISTS "${SRC}")
	message(FATAL_ERROR "ss_overlay_file: missing source ${SRC}")
	endif()
	get_filename_component(_dst_dir "${DST}" DIRECTORY)
	file(MAKE_DIRECTORY "${_dst_dir}")
	if(EXISTS "${DST}" OR IS_SYMLINK "${DST}")
	file(REMOVE "${DST}")
	endif()
	file(CREATE_LINK "${SRC}" "${DST}" RESULT _link_result)
	if(NOT _link_result STREQUAL "0")
	file(TO_CMAKE_PATH "${SRC}" _source_include)
	file(WRITE "${DST}"
		"/* Generated include overlay; edit ${_source_include}, not this file. */\n"
		"#include \"${_source_include}\"\n")
	endif()
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SRC}")
	set(_ide_headers ${SS_IDE_HEADERS})
	list(APPEND _ide_headers "${SRC}")
	set(SS_IDE_HEADERS "${_ide_headers}" PARENT_SCOPE)
endfunction()

function(ss_is_text_stub FPATH OUT_VAR)
	set(_stub FALSE)
	if(EXISTS "${FPATH}" AND NOT IS_DIRECTORY "${FPATH}")
	file(SIZE "${FPATH}" _sz)
	if(_sz LESS 200)
		file(READ "${FPATH}" _content LIMIT 200)
		string(STRIP "${_content}" _content)
		if(_content MATCHES "\\.\\./")
		set(_stub TRUE)
		endif()
	endif()
	endif()
	set(${OUT_VAR} "${_stub}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Include overlay: expose real SS-specific headers (skip text symlinks)
# ---------------------------------------------------------------------------
set(SS_INC_STAGE "${CMAKE_CURRENT_BINARY_DIR}/ss_include")
file(MAKE_DIRECTORY "${SS_INC_STAGE}")
file(GLOB _ss_hdrs "${SS_SRC}/include/*")
# Re-run configure if headers appear/disappear
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_ss_hdrs})
set(_ss_staged 0)
foreach(_h IN LISTS _ss_hdrs)
	if(IS_DIRECTORY "${_h}")
	continue()
	endif()
	ss_is_text_stub("${_h}" _is_stub)
	get_filename_component(_name "${_h}" NAME)
	if(_is_stub)
	list(APPEND SS_IDE_HEADERS "${B2_SRC}/include/${_name}")
	else()
	ss_overlay_file("${_h}" "${SS_INC_STAGE}/${_name}")
	math(EXPR _ss_staged "${_ss_staged}+1")
	endif()
endforeach()
message(STATUS "SheepShaver: overlaying ${_ss_staged} real headers (shared headers use BasiliskII sources)")

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
# Resolve CrossPlatform sources (sigsegv/vm_alloc/video_blit) via text stubs or
# real SS overrides — never hardcode only BasiliskII while a full SS copy sits
# in the tree for the IDE to open (that made "Build" look like it ignored edits).
macemu_xplat_sources_resolved("${SS_SRC}" "${B2_SRC}" SS_XPLAT)
macemu_check_shared_source_traps("${SS_SRC}" "${B2_SRC}")
# Log the exact paths that will be compiled (visible in VS CMake output).
foreach(_xp IN LISTS SS_XPLAT)
	message(STATUS "SheepShaver xplat: ${_xp}")
endforeach()

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
	ss_stage_file("${PRE_PPC_IMPL}" "${SS_GEN}/ppc-execute-impl.cpp")
else()
	message(FATAL_ERROR "Missing ${PRE_PPC_IMPL}")
endif()

if(ENABLE_SHEEPSHAVER_JIT)
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
	ss_stage_file("${_ops}/basic-dyngen-ops.hpp" "${SS_GEN}/basic-dyngen-ops.hpp")
	ss_stage_file("${_ops}/ppc-dyngen-ops.hpp" "${SS_GEN}/ppc-dyngen-ops.hpp")
endif()

if(WIN32)
	macemu_windows_net_sources("${B2_SRC}" SS_NET)

	# Stage Windows headers (sysdeps isolation for staged .cpp)
	set(SS_WIN_STAGE "${CMAKE_CURRENT_BINARY_DIR}/ss_windows")
	file(MAKE_DIRECTORY "${SS_WIN_STAGE}")
	file(GLOB _ss_win_hdrs "${SS_SRC}/Windows/*.h")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_ss_win_hdrs})
	foreach(_h IN LISTS _ss_win_hdrs)
	ss_is_text_stub("${_h}" _is_stub)
	get_filename_component(_name "${_h}" NAME)
	if(_is_stub)
		list(APPEND SS_IDE_HEADERS "${B2_SRC}/Windows/${_name}")
	else()
		ss_overlay_file("${_h}" "${SS_WIN_STAGE}/${_name}")
	endif()
	endforeach()

	# Compile Windows .cpp from their *real* source paths (not a staged copy).
	# Staging .cpp broke MSVC/VS incremental rebuilds: the IDE tracked the
	# staged copy while you edited the original, so "Build" often did nothing.
	#
	# Shared BasiliskII Windows sources (clip_windows.cpp, posix_emu.cpp, …)
	# still need SheepShaver's sysdeps.h: MSVC searches the .cpp's directory
	# first for #include "sysdeps.h". Force-include SS sysdeps first (/FI);
	# both headers use the SYSDEPS_H guard so the later include is a no-op.
	# SheepShaver-owned Windows sources already sit next to the right sysdeps.
	set(SS_PLATFORM)
	set(_ss_sysdeps_fi "${SS_WIN_STAGE}/sysdeps.h")
	# Normalize for string(FIND) (forward slashes)
	file(TO_CMAKE_PATH "${B2_SRC}" _b2_norm)
	foreach(f
		main_windows.cpp prefs_windows.cpp sys_windows.cpp xpram_windows.cpp
		timer_windows.cpp serial_windows.cpp ether_windows.cpp clip_windows.cpp
		extfs_windows.cpp user_strings_windows.cpp util_windows.cpp posix_emu.cpp
		about_window_windows.cpp)
	macemu_resolve_path("${SS_SRC}/Windows/${f}" "${B2_SRC}/Windows/${f}" _p)
	list(APPEND SS_PLATFORM "${_p}")
	file(TO_CMAKE_PATH "${_p}" _p_norm)
	string(FIND "${_p_norm}" "${_b2_norm}/Windows" _from_b2)
	if(NOT _from_b2 EQUAL -1)
		if(MSVC)
		set_source_files_properties("${_p}" PROPERTIES
			COMPILE_OPTIONS "/FI${_ss_sysdeps_fi}")
		else()
		set_source_files_properties("${_p}" PROPERTIES
			COMPILE_OPTIONS "-include;${_ss_sysdeps_fi}")
		endif()
		set_source_files_properties("${_p}" PROPERTIES
		OBJECT_DEPENDS "${_ss_sysdeps_fi}")
	endif()
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
# Graphics acceleration (PocketShaver engines: NQD / RAVE / GL / DSp)
# ---------------------------------------------------------------------------
if(NOT DEFINED ENABLE_GFXACCEL)
	option(ENABLE_GFXACCEL "SheepShaver graphics accel (NQD/RAVE/GL/DSp)" ON)
endif()
if(NOT DEFINED GFXACCEL_BACKEND)
	set(GFXACCEL_BACKEND "auto" CACHE STRING "gfxaccel GPU backend (auto|opengl|metal)")
endif()

set(SS_GFXACCEL)
set(SS_GFXACCEL_INC)
set(SS_GFXACCEL_LIBS)
set(SS_GFXACCEL_DEFS)

if(ENABLE_GFXACCEL)
	set(GA "${SS_SRC}/gfxaccel")
	set(SS_GFXACCEL_INC "${GA}" "${GA}/include" "${GA}/gl")

	# Portable engine / policy / dispatch sources
	list(APPEND SS_GFXACCEL
	"${GA}/display_mode_controller.cpp"
	"${GA}/dsp_dispatch.cpp"
	"${GA}/dsp_engine.cpp"
	"${GA}/dsp_install_hooks.cpp"
	"${GA}/dsp_mode_enumerate.cpp"
	"${GA}/dsp_thunks.cpp"
	"${GA}/gfxaccel_resources.cpp"
	"${GA}/gfxaccel_resources_heap.cpp"
	"${GA}/gl_defer.cpp"
	"${GA}/gl_dispatch.cpp"
	"${GA}/gl_engine.cpp"
	"${GA}/gl_state.cpp"
	"${GA}/gl_thunks.cpp"
	"${GA}/rave_dispatch.cpp"
	"${GA}/rave_draw_context.cpp"
	"${GA}/rave_engine.cpp"
	"${GA}/rave_thunks.cpp"
	)

	if(ENABLE_NATIVE_CINEPAK_PATCH)
		list(APPEND SS_GFXACCEL
		"${GA}/cinepak_decoder.cpp"
		"${GA}/cinepak_hooks.cpp"
		)
	endif()
	# Resolve backend
	set(_gfx_backend "${GFXACCEL_BACKEND}")
	if(_gfx_backend STREQUAL "auto")
	if(APPLE)
		set(_gfx_backend "metal")
	else()
		set(_gfx_backend "opengl")
	endif()
	endif()

	if(_gfx_backend STREQUAL "metal")
	if(NOT APPLE)
		message(FATAL_ERROR "GFXACCEL_BACKEND=metal requires Apple platforms")
	endif()
	list(APPEND SS_GFXACCEL_DEFS ENABLE_GFXACCEL=1 GFXACCEL_USE_METAL=1)
	enable_language(OBJCXX)
	list(APPEND SS_GFXACCEL
		"${GA}/metal_device_shared.mm"
		"${GA}/metal_compositor.mm"
		"${GA}/metal_compositor_submitframe.mm"
		"${GA}/nqd_metal_renderer.mm"
		"${GA}/rave_metal_renderer.mm"
		"${GA}/gl_metal_renderer.mm"
		"${GA}/dsp_metal_renderer.mm"
		"${GA}/dsp_draw_context.mm"
		"${GA}/dsp_alt_buffer.mm"
		"${GA}/dsp_flatten_restore.mm"
		"${GA}/dsp_clut_gamma.mm"
		"${GA}/dsp_host_bridge.mm"
		"${GA}/gfxaccel_resources.mm"
		"${GA}/gfxaccel_resources_heap.mm"
		"${GA}/gfxaccel_arc_shim.mm"
		"${GA}/vbl_source.mm"
		"${GA}/pso_archive.mm"
	)
	find_library(METAL_LIB Metal)
	find_library(QUARTZCORE_LIB QuartzCore)
	find_library(FOUNDATION_LIB Foundation)
	list(APPEND SS_GFXACCEL_LIBS ${METAL_LIB} ${QUARTZCORE_LIB} ${FOUNDATION_LIB})
	else()
	# OpenGL + SDL backend (default on Windows/Linux)
	list(APPEND SS_GFXACCEL_DEFS ENABLE_GFXACCEL=1 GFXACCEL_USE_OPENGL=1)
	list(APPEND SS_GFXACCEL
		"${GA}/gl/gl_device.cpp"
		"${GA}/gl/gl_compositor.cpp"
		"${GA}/gl/vbl_source_sdl.cpp"
		"${GA}/gl/gfxaccel_resources_gl.cpp"
		"${GA}/gl/gfxaccel_resources_heap_gl.cpp"
		"${GA}/gl/gfxaccel_arc_shim.cpp"
		"${GA}/gl/pso_archive_gl.cpp"
		"${GA}/gl/nqd_gl_renderer.cpp"
		"${GA}/gl/rave_gl_renderer.cpp"
		"${GA}/gl/gl_ffp_renderer.cpp"
		"${GA}/gl/dsp_gl_renderer.cpp"
		"${GA}/gl/dsp_draw_context_gl.cpp"
		"${GA}/gl/gfxaccel_link_stubs.cpp"
	)
	if(WIN32)
		list(APPEND SS_GFXACCEL_LIBS opengl32)
	elseif(APPLE)
		find_library(OPENGL_LIB OpenGL)
		list(APPEND SS_GFXACCEL_LIBS ${OPENGL_LIB})
	else()
		find_package(OpenGL REQUIRED)
		list(APPEND SS_GFXACCEL_LIBS OpenGL::GL)
	endif()
	endif()

	message(STATUS "SheepShaver gfxaccel: backend=${_gfx_backend}")
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
# ppc-execute-impl.cpp is #included by ppc-execute.cpp (not a separate TU)
add_executable(SheepShaver
	${SS_CORE} ${SS_CPU} ${SS_PLATFORM} ${SS_GFXACCEL})

# Show tracked headers, rather than generated overlay paths, in IDE projects.
if(SS_IDE_HEADERS)
	list(REMOVE_DUPLICATES SS_IDE_HEADERS)
	set_source_files_properties(${SS_IDE_HEADERS} PROPERTIES HEADER_FILE_ONLY TRUE)
	target_sources(SheepShaver PRIVATE ${SS_IDE_HEADERS})
endif()

# Force staging before any compile/link (required for VS/CMake IDE + Ninja).
# Without this, staged headers can lag source edits forever after configure.
if(SS_STAGE_OUTPUTS)
	add_custom_target(SheepShaver_stage DEPENDS ${SS_STAGE_OUTPUTS})
	add_dependencies(SheepShaver SheepShaver_stage)
endif()

# NOTE: Do not use cmake -E echo with ">" in the message under cmd.exe —
# the shell treats it as a redirect and can overwrite the .exe (we hit this).
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
	${SS_GFXACCEL_INC}
)

target_compile_definitions(SheepShaver PRIVATE
	SHEEPSHAVER=1
	POWERPC_ROM=1
	EMULATED_PPC=1
	${SS_GFXACCEL_DEFS}
)
if(WIN32)
	target_compile_definitions(SheepShaver PRIVATE CONFIG_WIN32=1 NATMEM_OFFSET=0x11000000)
endif()
if(MSVC AND ENABLE_GFXACCEL)
	# M_PI and other math macros from <cmath>
	target_compile_definitions(SheepShaver PRIVATE _USE_MATH_DEFINES=1)
endif()
if(ENABLE_SHEEPSHAVER_JIT)
	target_compile_definitions(SheepShaver PRIVATE USE_JIT ENABLE_DYNGEN)
endif()
if(ENABLE_QD3D_INIT_LOGGING)
	target_compile_definitions(SheepShaver PRIVATE QD3D_INIT_LOGGING_ENABLED=1)
endif()
if(ENABLE_QD3D_GRAPHICS_LOGGING)
	target_compile_definitions(SheepShaver PRIVATE QD3D_GRAPHICS_LOGGING_ENABLED=1)
endif()
if(ENABLE_QD3D_AUDIO_LOGGING)
	target_compile_definitions(SheepShaver PRIVATE QD3D_AUDIO_LOGGING_ENABLED=1)
endif()
if(ENABLE_QD3D_MEDIA_LOGGING)
	target_compile_definitions(SheepShaver PRIVATE QD3D_MEDIA_LOGGING_ENABLED=1)
endif()
if(ENABLE_QD3D_WAIT_LOGGING)
	target_compile_definitions(SheepShaver PRIVATE QD3D_WAIT_LOGGING_ENABLED=1)
endif()
if(ENABLE_NATIVE_CINEPAK_PATCH)
	target_compile_definitions(SheepShaver PRIVATE
		ENABLE_NATIVE_CINEPAK_PATCH=1)
endif()

macemu_apply_common(SheepShaver)

if(SS_GFXACCEL_LIBS)
	target_link_libraries(SheepShaver PRIVATE ${SS_GFXACCEL_LIBS})
endif()

if(ENABLE_GTK AND NOT WIN32)
	target_include_directories(SheepShaver PRIVATE ${GTK3_INCLUDE_DIRS})
	target_link_libraries(SheepShaver PRIVATE ${GTK3_LIBRARIES})
endif()

install(TARGETS SheepShaver RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

message(STATUS "SheepShaver: JIT=${ENABLE_SHEEPSHAVER_JIT}")
message(STATUS "SheepShaver: QD3D init logging=${ENABLE_QD3D_INIT_LOGGING}")
message(STATUS "SheepShaver: QD3D graphics logging=${ENABLE_QD3D_GRAPHICS_LOGGING}")
message(STATUS "SheepShaver: QD3D audio logging=${ENABLE_QD3D_AUDIO_LOGGING}")
message(STATUS "SheepShaver: QD3D media logging=${ENABLE_QD3D_MEDIA_LOGGING}")
message(STATUS "SheepShaver: QD3D wait logging=${ENABLE_QD3D_WAIT_LOGGING}")
