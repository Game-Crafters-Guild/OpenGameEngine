# FixupMacAppBundle.cmake
#
# Post-build fixups for macOS .app bundles:
# - Ensure the main executable has an rpath to Contents/Frameworks
# - Rewrite absolute/unstable dylib dependency paths to @rpath/<basename> when the dylib is staged in Frameworks
# - Ensure each staged dylib has id=@rpath/<basename> and has an @loader_path rpath
#
# Intended to run on macOS only (guarded by APPLE).
#
# Inputs (via -D on cmake -P):
# - GE_APP_EXE: Full path to the executable inside the app bundle (Contents/MacOS/<name>)
#
# Optional:
# - GE_FIXUP_VERBOSE: ON/OFF (default OFF)
#

if(NOT APPLE)
  message(STATUS "FixupMacAppBundle: non-Apple platform; skipping")
  return()
endif()

if(DEFINED ENV{GE_SKIP_MAC_BUNDLE_FIXUP})
  if(NOT "$ENV{GE_SKIP_MAC_BUNDLE_FIXUP}" STREQUAL "" AND NOT "$ENV{GE_SKIP_MAC_BUNDLE_FIXUP}" STREQUAL "0")
    message(STATUS "FixupMacAppBundle: skipped (GE_SKIP_MAC_BUNDLE_FIXUP is set)")
    return()
  endif()
endif()

set(_verbose OFF)
if(DEFINED GE_FIXUP_VERBOSE)
  set(_verbose "${GE_FIXUP_VERBOSE}")
endif()

if(NOT DEFINED GE_APP_EXE OR GE_APP_EXE STREQUAL "")
  message(FATAL_ERROR "FixupMacAppBundle: GE_APP_EXE must be set (path to Contents/MacOS/<exe>)")
endif()

set(_exe "${GE_APP_EXE}")
if(NOT EXISTS "${_exe}")
  message(FATAL_ERROR "FixupMacAppBundle: executable not found: ${_exe}")
endif()

find_program(_otool otool)
find_program(_install_name_tool install_name_tool)
if(NOT _otool)
  message(FATAL_ERROR "FixupMacAppBundle: 'otool' not found in PATH")
endif()
if(NOT _install_name_tool)
  message(FATAL_ERROR "FixupMacAppBundle: 'install_name_tool' not found in PATH")
endif()

get_filename_component(_macos_dir "${_exe}" DIRECTORY)          # .../Contents/MacOS
get_filename_component(_contents_dir "${_macos_dir}" DIRECTORY) # .../Contents
get_filename_component(_app_dir "${_contents_dir}" DIRECTORY)   # .../*.app
set(_frameworks_dir "${_contents_dir}/Frameworks")

if(_verbose)
  message(STATUS "FixupMacAppBundle: exe=${_exe}")
  message(STATUS "FixupMacAppBundle: frameworks=${_frameworks_dir}")
endif()

if(NOT EXISTS "${_frameworks_dir}")
  message(FATAL_ERROR "FixupMacAppBundle: missing Contents/Frameworks directory: ${_frameworks_dir}")
endif()

file(GLOB _dylibs "${_frameworks_dir}/*.dylib")

function(_ge_exec _cmd)
  execute_process(
    COMMAND ${_cmd}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
  )
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "FixupMacAppBundle: command failed (rc=${_rc}): ${_cmd}\n${_err}")
  endif()
endfunction()

function(_ge_otool_L _bin _outVar)
  execute_process(
    COMMAND "${_otool}" -L "${_bin}"
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    RESULT_VARIABLE _rc
  )
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "FixupMacAppBundle: otool -L failed for '${_bin}': ${_err}")
  endif()
  set(${_outVar} "${_out}" PARENT_SCOPE)
endfunction()

function(_ge_otool_l _bin _outVar)
  execute_process(
    COMMAND "${_otool}" -l "${_bin}"
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    RESULT_VARIABLE _rc
  )
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "FixupMacAppBundle: otool -l failed for '${_bin}': ${_err}")
  endif()
  set(${_outVar} "${_out}" PARENT_SCOPE)
endfunction()

function(_ge_has_rpath _bin _needle _outVar)
  _ge_otool_l("${_bin}" _lout)
  string(FIND "${_lout}" "${_needle}" _idx)
  if(_idx EQUAL -1)
    set(${_outVar} FALSE PARENT_SCOPE)
  else()
    set(${_outVar} TRUE PARENT_SCOPE)
  endif()
endfunction()

function(_ge_ensure_rpath _bin _rpath)
  _ge_has_rpath("${_bin}" "${_rpath}" _has)
  if(NOT _has)
    if(_verbose)
      message(STATUS "FixupMacAppBundle: adding rpath '${_rpath}' to ${_bin}")
    endif()
    execute_process(
      COMMAND "${_install_name_tool}" -add_rpath "${_rpath}" "${_bin}"
      RESULT_VARIABLE _rc
      OUTPUT_VARIABLE _out
      ERROR_VARIABLE _err
    )
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "FixupMacAppBundle: install_name_tool -add_rpath failed for '${_bin}': ${_err}")
    endif()
  endif()
endfunction()

function(_ge_collect_framework_basenames _outVar)
  set(_names "")
  file(GLOB _local_dylibs "${_frameworks_dir}/*.dylib")
  foreach(_d IN LISTS _local_dylibs)
    get_filename_component(_name "${_d}" NAME)
    list(APPEND _names "${_name}")
  endforeach()
  list(REMOVE_DUPLICATES _names)
  set(${_outVar} "${_names}" PARENT_SCOPE)
endfunction()

function(_ge_patch_binary_deps _bin)
  _ge_collect_framework_basenames(_names)
  if(_names STREQUAL "")
    return()
  endif()

  _ge_otool_L("${_bin}" _deps)
  string(REPLACE "\r\n" "\n" _deps "${_deps}")
  string(REPLACE "\r" "\n" _deps "${_deps}")
  string(REPLACE "\n" ";" _lines "${_deps}")
  list(LENGTH _lines _n)
  if(_n GREATER 0)
    list(REMOVE_AT _lines 0) # skip '<bin>:'
  endif()

  foreach(_line IN LISTS _lines)
    string(STRIP "${_line}" _s)
    if(_s STREQUAL "")
      continue()
    endif()

    string(REGEX MATCH "^([^ ]+)" _m "${_s}")
    set(_dep "${CMAKE_MATCH_1}")
    if(_dep STREQUAL "")
      continue()
    endif()

    # Ignore system libraries
    if(_dep MATCHES "^/usr/lib/" OR _dep MATCHES "^/System/Library/")
      continue()
    endif()

    # Extract basename and see if it is staged in Frameworks.
    get_filename_component(_base "${_dep}" NAME)
    list(FIND _names "${_base}" _idx)
    if(_idx EQUAL -1)
      continue()
    endif()

    set(_desired "@rpath/${_base}")
    if(_dep STREQUAL _desired)
      continue()
    endif()

    if(_verbose)
      message(STATUS "FixupMacAppBundle: ${_bin}: ${_dep} -> ${_desired}")
    endif()

    execute_process(
      COMMAND "${_install_name_tool}" -change "${_dep}" "${_desired}" "${_bin}"
      RESULT_VARIABLE _rc
      OUTPUT_VARIABLE _out
      ERROR_VARIABLE _err
    )
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "FixupMacAppBundle: install_name_tool -change failed for '${_bin}': ${_err}")
    endif()
  endforeach()
endfunction()

# Ensure executable can find Frameworks.
_ge_ensure_rpath("${_exe}" "@executable_path/../Frameworks")

# Fixup dylib IDs and rpaths first.
foreach(_d IN LISTS _dylibs)
  get_filename_component(_name "${_d}" NAME)
  if(_name STREQUAL "")
    continue()
  endif()

  # Set dylib install name to @rpath/<basename>
  execute_process(
    COMMAND "${_install_name_tool}" -id "@rpath/${_name}" "${_d}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
  )
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "FixupMacAppBundle: install_name_tool -id failed for '${_d}': ${_err}")
  endif()

  # Ensure each dylib can resolve @rpath dependencies relative to its own directory.
  _ge_ensure_rpath("${_d}" "@loader_path")
endforeach()

# Patch dependency paths in the executable and the dylibs.
_ge_patch_binary_deps("${_exe}")
foreach(_d IN LISTS _dylibs)
  _ge_patch_binary_deps("${_d}")
endforeach()

message(STATUS "FixupMacAppBundle: OK (${_app_dir})")

