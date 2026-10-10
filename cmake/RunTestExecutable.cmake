# RunTestExecutable.cmake
#
# Script-mode helper to run a test executable with some preflight cleanup.
#
# Inputs:
# - TEST_EXE: full path to executable
# - TEST_WORKING_DIR: working directory (optional)
# - TEST_ARGS: optional argument list passed to the executable (CMake list)

if(NOT DEFINED TEST_EXE OR TEST_EXE STREQUAL "")
  message(FATAL_ERROR "RunTestExecutable: TEST_EXE must be set")
endif()

set(_exe "${TEST_EXE}")
string(REPLACE "\"" "" _exe "${_exe}")

get_filename_component(_exe_dir "${_exe}" DIRECTORY)

set(_wd "")
if(DEFINED TEST_WORKING_DIR AND NOT TEST_WORKING_DIR STREQUAL "")
  set(_wd "${TEST_WORKING_DIR}")
  string(REPLACE "\"" "" _wd "${_wd}")
endif()

set(_args_list "")
if(DEFINED TEST_ARGS AND NOT TEST_ARGS STREQUAL "")
  set(_args_list ${TEST_ARGS})
endif()
set(_args_joined "")
if(_args_list)
  string(JOIN " " _args_joined ${_args_list})
endif()

#
# IMPORTANT (Windows): some DLL initialization failures surface as "abnormal termination"
# to CMake's direct process runner (RESULT_VARIABLE becomes "Exit code 0xc000...."),
# even when `cmd.exe` reports an ordinary exit code. Since CTest is Windows-first here,
# run through cmd.exe so we get the same observable behavior as developers running
# the binary from a terminal.
#
if(WIN32)
  # Normalize paths for cmd.exe (prefer backslashes).
  set(_exe_cmd "${_exe}")
  string(REPLACE "/" "\\" _exe_cmd "${_exe_cmd}")
  set(_wd_cmd "${_wd}")
  string(REPLACE "/" "\\" _wd_cmd "${_wd_cmd}")

  # Prefer running the test executable directly so we get its real exit code.
  # (Some environments report odd results when going through cmd.exe.)
  if(NOT _wd STREQUAL "")
    execute_process(
      COMMAND "${_exe_cmd}" ${_args_list}
      WORKING_DIRECTORY "${_wd_cmd}"
      RESULT_VARIABLE _rc
    )
  else()
    execute_process(COMMAND "${_exe_cmd}" ${_args_list} RESULT_VARIABLE _rc)
  endif()
else()
  if(NOT _wd STREQUAL "")
    execute_process(COMMAND "${_exe}" ${_args_list} WORKING_DIRECTORY "${_wd}" RESULT_VARIABLE _rc)
  else()
    execute_process(COMMAND "${_exe}" ${_args_list} RESULT_VARIABLE _rc)
  endif()
endif()

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "Test executable failed rc=${_rc}")
endif()

