# Platform drivers - adapters + null implementations

adapter_sources = [
  'adapter/scsi_adapter.cpp',
  'adapter/video_adapter.cpp',
  'adapter/audio_adapter.cpp',
  'adapter/disk_adapter.cpp',
  'adapter/serial_adapter.cpp',
  'adapter/ether_adapter.cpp',
]

null_driver_sources = [
  'scsi/scsi_null.cpp',
  'video/video_null.cpp',
  'audio/audio_null.cpp',
  'disk/disk_null.cpp',
  'serial/serial_null.cpp',
  'ether/ether_null.cpp',
  'platform/platform_null.cpp',  # Host platform stubs
]

# Platform infrastructure
platform_sources = [
  '../platform/timer_interrupt.cpp',
]

# Keep some dummy drivers that aren't adapted yet
remaining_dummy_sources = [
  'dummy/clip_dummy.cpp',
  'dummy/prefs_dummy.cpp',
  'dummy/prefs_editor_dummy.cpp',
  'dummy/user_strings_dummy.cpp',
  'dummy/xpram_dummy.cpp',
]

driver_sources = adapter_sources + null_driver_sources + remaining_dummy_sources + platform_sources

driver_inc = include_directories(
  '.',
  'adapter',
  '../common/include',
  '../core',
  '../cpu',
  '../cpu/uae_cpu',
  '../platform',
)

driver_lib = static_library('drivers',
  driver_sources,
  include_directories: driver_inc,
)

driver_dep = declare_dependency(
  link_with: driver_lib,
  include_directories: driver_inc,
)
