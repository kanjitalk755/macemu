# WebRTC Streaming System

# Dependencies (to be added to main meson.build)
# These will be defined in the parent and passed here

# Subdirectories
subdir('encoders')
subdir('http')
subdir('storage')
subdir('utils')

# Main WebRTC coordinator sources
webrtc_sources = [
  'globals.cpp',
  'video_encoder_thread.cpp',
  'audio_encoder_thread.cpp',
    'webrtc_server.cpp',
]

webrtc_inc = include_directories(
  '.',
  '../common/include',
  '../platform',
  '../config',
  'encoders',
  'http',
  'storage',
  'utils',
)

# Combine all WebRTC components
webrtc_lib = static_library('webrtc',
  webrtc_sources,
  include_directories: webrtc_inc,
  link_with: [encoder_lib, http_lib, storage_lib, utils_lib],
)

webrtc_dep = declare_dependency(
  link_with: [webrtc_lib, encoder_lib, http_lib, storage_lib, utils_lib],
  include_directories: webrtc_inc,
)
