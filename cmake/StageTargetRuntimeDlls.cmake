# StageTargetRuntimeDlls.cmake
#
# Script-mode helper used by POST_BUILD steps to copy a list of runtime DLLs next
# to an executable so it can run from its output directory.
#
# Inputs:
# - GE_STAGE_RUNTIME_DLLS_DEST_DIR: destination directory
# - GE_STAGE_RUNTIME_DLLS: CMake list of DLL paths (may be empty)

if(NOT DEFINED GE_STAGE_RUNTIME_DLLS_DEST_DIR OR GE_STAGE_RUNTIME_DLLS_DEST_DIR STREQUAL "")
  message(FATAL_ERROR "StageTargetRuntimeDlls: GE_STAGE_RUNTIME_DLLS_DEST_DIR must be set")
endif()

# MSBuild / some generators may escape quotes into the argument value when passing
# `-DVAR="..."`. Strip quotes so we always operate on a real path/list.
string(REPLACE "\"" "" GE_STAGE_RUNTIME_DLLS_DEST_DIR "${GE_STAGE_RUNTIME_DLLS_DEST_DIR}")
if(DEFINED GE_STAGE_RUNTIME_DLLS)
  string(REPLACE "\"" "" GE_STAGE_RUNTIME_DLLS "${GE_STAGE_RUNTIME_DLLS}")
endif()

file(MAKE_DIRECTORY "${GE_STAGE_RUNTIME_DLLS_DEST_DIR}")

include("${CMAKE_CURRENT_LIST_DIR}/GeStagedCopy.cmake")
ge_staged_copy_cleanup("${GE_STAGE_RUNTIME_DLLS_DEST_DIR}")

if(NOT DEFINED GE_STAGE_RUNTIME_DLLS OR GE_STAGE_RUNTIME_DLLS STREQUAL "")
  # Nothing to copy.
  return()
endif()

foreach(_ge_dll IN LISTS GE_STAGE_RUNTIME_DLLS)
  get_filename_component(_ge_name "${_ge_dll}" NAME)
  # NOTE: mimalloc-redirect can be sensitive to load order (must be initialized very early).
  # When staged next to test executables it can produce noisy warnings and can even crash
  # during shutdown in some configurations. Do not stage it automatically.
  if(_ge_name STREQUAL "mimalloc-redirect.dll")
    continue()
  endif()
  if(EXISTS "${_ge_dll}")
    ge_staged_copy("${_ge_dll}" "${GE_STAGE_RUNTIME_DLLS_DEST_DIR}" "StageTargetRuntimeDlls")
  endif()
endforeach()


