# Platform drivers - now includes encoders and null implementations

# Video drivers
video_driver_sources = [
  'video/video_null.cpp',
  'video/video_adapter.cpp',
  'video/video_output.cpp',
  'video/video_encoder_thread.cpp',
]

# Audio drivers
audio_driver_sources = [
  'audio/audio_null.cpp',
  'audio/audio_adapter.cpp',
  'audio/audio_output.cpp',
  'audio/audio_encoder_thread.cpp',
]

# Other drivers
other_driver_sources = [
  'disk/disk_null.cpp',
  'disk/disk_adapter.cpp',
  'scsi/scsi_null.cpp',
  'scsi/scsi_adapter.cpp',
  'serial/serial_null.cpp',
  'serial/serial_adapter.cpp',
  'ether/ether_null.cpp',
  'ether/ether_adapter.cpp',
  'platform/platform_null.cpp',
  'platform/timer_interrupt.cpp',
]

# Consolidated null drivers (was dummy/)
null_driver_sources = [
  'null_drivers.cpp',
]

driver_sources = video_driver_sources + audio_driver_sources + other_driver_sources + null_driver_sources

driver_inc = include_directories(
  '.',
  'video',
  'audio',
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
