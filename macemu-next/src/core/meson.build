# Core platform-independent managers
core_sources = [
  'adb.cpp',
  'audio.cpp',
  'cdrom.cpp',
  'disk.cpp',
  'emul_op.cpp',
  'ether.cpp',
  'extfs.cpp',
  'json_config.cpp',
  'macos_util.cpp',
  'main.cpp',
  'prefs.cpp',
  'prefs_items.cpp',
  'rom_patches.cpp',
  'rsrc_patches.cpp',
  'scsi.cpp',
  'serial.cpp',
  'slot_rom.cpp',
  'sony.cpp',
  'timer.cpp',
  'user_strings.cpp',
  'video.cpp',
  'xpram.cpp',
  '../common/platform.cpp',  # Platform initialization
]

core_inc = include_directories(
  '.',
  '../common/include',
  '../cpu',
  '../cpu/uae_cpu',
  '../../external/json/include',  # nlohmann/json
)

core_lib = static_library('core',
  core_sources,
  include_directories: core_inc,
  dependencies: [threads_dep],
)

core_dep = declare_dependency(
  link_with: core_lib,
  include_directories: core_inc,
)
