# JSON Configuration System

config_sources = [
  'config_manager.cpp',
  'json_utils.cpp',
  'prefs_manager.cpp',
]

config_inc = include_directories(
  '.',
  '../common/include',
  '../../external/json/include',  # nlohmann/json
  '../webrtc/encoders',           # For codec.h (used by prefs_manager)
)

config_lib = static_library('config',
  config_sources,
  include_directories: config_inc,
)

config_dep = declare_dependency(
  link_with: config_lib,
  include_directories: config_inc,
)
