# Video Encoders

video_encoder_sources = [
  'h264_encoder.cpp',
  'vp9_encoder.cpp',
  'webp_encoder.cpp',
  'png_encoder.cpp',
  'fpng.cpp',
]

video_encoder_inc = include_directories(
  '.',
  '../../../common/include',
)

# Special compiler flags for fpng (requires SSE4.1 and PCLMUL)
fpng_args = []
if host_machine.cpu_family() == 'x86_64' or host_machine.cpu_family() == 'x86'
  fpng_args = ['-msse4.1', '-mpclmul']
endif

video_encoder_lib = static_library('video_encoders',
  video_encoder_sources,
  include_directories: video_encoder_inc,
  cpp_args: fpng_args,
)

video_encoder_dep = declare_dependency(
  link_with: video_encoder_lib,
  include_directories: video_encoder_inc,
)
