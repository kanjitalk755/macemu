# Audio Encoders

audio_encoder_sources = [
  'opus_encoder.cpp',
]

audio_encoder_inc = include_directories(
  '.',
  '../../../common/include',
)

audio_encoder_lib = static_library('audio_encoders',
  audio_encoder_sources,
  include_directories: audio_encoder_inc,
)

audio_encoder_dep = declare_dependency(
  link_with: audio_encoder_lib,
  include_directories: audio_encoder_inc,
)
