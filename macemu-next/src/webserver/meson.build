# Web Server - HTTP API and browser UI

webserver_sources = [
  'http_server.cpp',
  'api_handlers.cpp',
  'static_files.cpp',
  'file_scanner.cpp',
  'keyboard_map.cpp',
]

webserver_inc = include_directories(
  '.',
  '../common/include',
  '../config',
  '../../external/json/include',
)

webserver_lib = static_library('webserver',
  webserver_sources,
  include_directories: webserver_inc,
)

webserver_dep = declare_dependency(
  link_with: webserver_lib,
  include_directories: webserver_inc,
)
