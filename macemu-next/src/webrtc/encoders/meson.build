# Video and Audio Encoders

encoder_sources = [
  'h264_encoder.cpp',
  'vp9_encoder.cpp',
  'webp_encoder.cpp',
  'png_encoder.cpp',
  'opus_encoder.cpp',
  'fpng.cpp',
]

encoder_inc = include_directories(
  '.',
  '../../common/include',
  '../../platform',  # For VideoOutput
)

# fpng needs SSE4.1 and PCLMUL support
fpng_args = []
if host_machine.cpu_family() == 'x86_64' or host_machine.cpu_family() == 'x86'
  fpng_args = ['-msse4.1', '-mpclmul']
endif

encoder_lib = static_library('encoders',
  encoder_sources,
  include_directories: encoder_inc,
  cpp_args: fpng_args,
  # Dependencies added by parent
)

encoder_dep = declare_dependency(
  link_with: encoder_lib,
  include_directories: encoder_inc,
)
