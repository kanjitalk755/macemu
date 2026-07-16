# Run a program and capture stdout to a file (portable; no shell redirect).
if(NOT DEFINED TOOL OR NOT DEFINED OUT)
  message(FATAL_ERROR "run_redirect.cmake requires TOOL and OUT")
endif()
if(DEFINED ARGS)
  separate_arguments(ARG_LIST NATIVE_COMMAND "${ARGS}")
else()
  set(ARG_LIST)
endif()
if(DEFINED WORKDIR)
  set(_wd WORKING_DIRECTORY "${WORKDIR}")
else()
  set(_wd)
endif()
execute_process(
  COMMAND "${TOOL}" ${ARG_LIST}
  OUTPUT_FILE "${OUT}"
  RESULT_VARIABLE _rc
  ${_wd}
)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "${TOOL} failed with exit code ${_rc}")
endif()
