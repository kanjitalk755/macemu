# Platform API - Video/Audio output buffers

platform_sources = [
  'timer_interrupt.cpp',
  'video_output.cpp',
  'audio_output.cpp',
]

platform_inc = include_directories(
  '.',
  '../common/include',
  '../cpu',          # For uae_wrapper.h
  '../core',         # For main.h
)

platform_lib = static_library('platform',
  platform_sources,
  include_directories: platform_inc,
  dependencies: [threads_dep],
)

platform_dep = declare_dependency(
  link_with: platform_lib,
  include_directories: platform_inc,
)
