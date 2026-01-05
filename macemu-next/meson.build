project('macemu-next', ['c', 'cpp'],
  version: '2.0.0',
  default_options: [
    'c_std=c11',
    'cpp_std=c++17',
    'warning_level=2',
    'buildtype=debugoptimized'
  ]
)

# Options
build_basilisk = get_option('basilisk')
build_sheepshaver = get_option('sheepshaver')

# Dependencies
cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

# Unicorn dependency (always required - all CPUs compiled)
unicorn_inc = include_directories('external/unicorn/include')
unicorn_lib = cc.find_library('unicorn',
  dirs: [meson.current_source_dir() / 'external/unicorn/build'],
  static: true,
  required: true
)
unicorn_dep = declare_dependency(
  include_directories: unicorn_inc,
  dependencies: [unicorn_lib]
)

# Other dependencies
threads_dep = dependency('threads', required: true)
m_dep = cc.find_library('m', required: true)  # Math library for Unicorn

# WebRTC streaming dependencies (optional for now)
enable_webrtc = get_option('webrtc')
if enable_webrtc
  # JSON configuration
  nlohmann_json_dep = dependency('nlohmann_json', required: false, fallback: ['nlohmann_json', 'nlohmann_json_dep'])
  if not nlohmann_json_dep.found()
    nlohmann_json_inc = include_directories('external/json/include')
    nlohmann_json_dep = declare_dependency(include_directories: nlohmann_json_inc)
  endif

  # Video encoders
  openh264_dep = dependency('openh264', required: false)
  libvpx_dep = dependency('vpx', required: false)
  libwebp_dep = dependency('libwebp', required: false)

  # Audio encoder
  opus_dep = dependency('opus', required: false)

  # HTTP server
  cpp_httplib_dep = dependency('cpp-httplib', required: false)

  # WebRTC library
  libdatachannel_dep = dependency('libdatachannel', required: false)

  # libyuv for color conversion (Ubuntu package lacks .pc file, use find_library)
  libyuv_lib = cc.find_library('yuv', required: false)
  if libyuv_lib.found()
    libyuv_dep = declare_dependency(
      dependencies: [libyuv_lib],
      include_directories: include_directories('/usr/include')
    )
  else
    libyuv_dep = dependency('', required: false)  # Dummy for .found() check
  endif

  message('WebRTC dependencies:')
  message('  nlohmann_json: ' + nlohmann_json_dep.found().to_string())
  message('  openh264: ' + openh264_dep.found().to_string())
  message('  libvpx (VP9): ' + libvpx_dep.found().to_string())
  message('  libwebp: ' + libwebp_dep.found().to_string())
  message('  opus: ' + opus_dep.found().to_string())
  message('  cpp-httplib: ' + cpp_httplib_dep.found().to_string())
  message('  libdatachannel: ' + libdatachannel_dep.found().to_string())
  message('  libyuv: ' + libyuv_dep.found().to_string())
endif

# Build info
message('Build configuration:')
message('  All CPU backends compiled (runtime selection via CPU_BACKEND env var)')
message('  Build BasiliskII: ' + build_basilisk.to_string())
message('  Build SheepShaver: ' + build_sheepshaver.to_string())

# Subdirectories
subdir('src/cpu')
subdir('src/core')
subdir('src/drivers')

# Optional WebRTC streaming
if enable_webrtc
  subdir('src/config')
  subdir('src/webserver')
  subdir('src/drivers/video/encoders')
  subdir('src/drivers/audio/encoders')
endif

# Main executable
macemu_next_sources = ['src/main.cpp']
macemu_next_link_with = [core_lib, driver_lib, cpu_lib]
macemu_next_deps = [unicorn_dep, threads_dep, m_dep]
macemu_next_inc = [
  include_directories('src/common/include'),
  include_directories('src/core'),
  include_directories('src/cpu'),
  include_directories('src/cpu/uae_cpu'),
  include_directories('src/drivers'),
  include_directories('src/platform'),
]
macemu_next_cpp_args = []

# Add WebRTC support if enabled
if enable_webrtc
  macemu_next_cpp_args += ['-DENABLE_WEBRTC']
  macemu_next_link_with += [config_lib, webserver_lib, video_encoder_lib, audio_encoder_lib]
  macemu_next_deps += [nlohmann_json_dep]

  # Add codec dependencies if found
  if openh264_dep.found()
    macemu_next_deps += [openh264_dep]
  endif
  if libvpx_dep.found()
    macemu_next_deps += [libvpx_dep]
  endif
  if libwebp_dep.found()
    macemu_next_deps += [libwebp_dep]
  endif
  if opus_dep.found()
    macemu_next_deps += [opus_dep]
  endif
  if libyuv_dep.found()
    macemu_next_deps += [libyuv_dep]
  endif

  macemu_next_inc += [
    include_directories('src/config'),
    include_directories('src/webserver'),
    include_directories('src/drivers/video'),
    include_directories('src/drivers/audio'),
  ]
endif

macemu_next = executable('macemu-next',
  macemu_next_sources,
  link_with: macemu_next_link_with,
  dependencies: macemu_next_deps,
  include_directories: macemu_next_inc,
  cpp_args: macemu_next_cpp_args
)

# Tests
if get_option('tests')
  subdir('tests')
endif
