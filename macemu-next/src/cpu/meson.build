# Always compile ALL CPU backends (runtime selection via platform API)
cpu_sources = [
  'cpu_uae.c',                # UAE backend for platform API
  'cpu_unicorn.cpp',          # Unicorn backend for platform API (C++ for EmulOp linkage)
  'cpu_dualcpu.c',            # DualCPU backend for platform API
  'cpu_trace.c',              # CPU execution tracing infrastructure
  'uae_wrapper.cpp',          # UAE CPU wrapper
  'unicorn_wrapper.c',        # Unicorn wrapper
  'unicorn_exception.c',      # Exception simulation for Unicorn
  'unicorn_validation.cpp',   # Validation module (used by dualcpu)
]

# Build UAE CPU (always)
subdir('uae_cpu')

cpu_deps = [unicorn_dep, uae_dep]

# Build CPU library
cpu_lib = static_library('cpu',
  cpu_sources,
  dependencies: cpu_deps,
  include_directories: [
    include_directories('.'),
    include_directories('../common/include'),
    include_directories('../platform'),
    include_directories('../drivers/platform'),
  ]
)

cpu_dep = declare_dependency(
  link_with: cpu_lib,
  include_directories: include_directories('.')
)
