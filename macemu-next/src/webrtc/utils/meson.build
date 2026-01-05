# WebRTC Utilities

utils_sources = [
  'keyboard_map.cpp',
]

utils_inc = include_directories(
  '.',
  '../../common/include',
)

utils_lib = static_library('webrtc_utils',
  utils_sources,
  include_directories: utils_inc,
)

utils_dep = declare_dependency(
  link_with: utils_lib,
  include_directories: utils_inc,
)
