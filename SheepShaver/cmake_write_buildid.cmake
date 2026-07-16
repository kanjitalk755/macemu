# Written by SheepShaver POST_BUILD. Args: -Dexe=... -Dstamp=...
if(NOT DEFINED exe OR NOT DEFINED stamp)
  message(FATAL_ERROR "cmake_write_buildid.cmake needs -Dexe= and -Dstamp=")
endif()
string(TIMESTAMP _now "%Y-%m-%d %H:%M:%S")
if(EXISTS "${exe}")
  file(TIMESTAMP "${exe}" _exe_ts "%Y-%m-%d %H:%M:%S")
  file(SIZE "${exe}" _exe_sz)
else()
  set(_exe_ts "missing")
  set(_exe_sz 0)
endif()
file(WRITE "${stamp}"
  "exe=${exe}\n"
  "size=${_exe_sz}\n"
  "exe_mtime=${_exe_ts}\n"
  "built_at=${_now}\n"
  "note=If VS is debugging an older path, check this file next to the running exe.\n"
)
message(STATUS "SheepShaver.buildid -> ${stamp}")
