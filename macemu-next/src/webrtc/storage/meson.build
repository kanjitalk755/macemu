# File Scanner for ROMs and Disks

storage_sources = [
  'file_scanner.cpp',
]

storage_inc = include_directories(
  '.',
  '../../common/include',
)

storage_lib = static_library('storage',
  storage_sources,
  include_directories: storage_inc,
)

storage_dep = declare_dependency(
  link_with: storage_lib,
  include_directories: storage_inc,
)
