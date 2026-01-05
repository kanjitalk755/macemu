# HTTP Server and API Handlers

http_sources = [
  'http_server.cpp',
  'api_handlers.cpp',
  'static_files.cpp',
]

http_inc = include_directories(
  '.',
  '../../common/include',
  '../../platform',
  '../../config',
  '../encoders',   # For codec.h
  '../storage',    # For file_scanner.h
  '../../../external/json/include',  # nlohmann/json
)

http_lib = static_library('http',
  http_sources,
  include_directories: http_inc,
)

http_dep = declare_dependency(
  link_with: http_lib,
  include_directories: http_inc,
)
