# Stage one prebuilt engine-package module DLL (+PDB when present) into the
# per-config intermediate under the toolchain's platform dir:
#     <DEST_ROOT>/<platform>-<arch>-<fp8>/<Module>.dll     (.dylib on macOS, .so on Linux)
# The platform dir name and the module file name come from the GePrebuiltStamp
# tool — compiled in the same build with the same flags, and naming the file
# with the loader's own rule (PrebuiltModuleFileName) — never from CMake-side
# guesses. The module is renamed to that file name: CMake names a MODULE
# library lib<Module>.so on macOS and Linux, which the loader never probes.
# Args: -DSTAMP_TOOL= -DMODULE_FILE= -DMODULE_NAME= [-DMODULE_PDB=] -DDEST_ROOT=
if(NOT DEFINED STAMP_TOOL OR NOT DEFINED MODULE_FILE OR NOT DEFINED MODULE_NAME OR NOT DEFINED DEST_ROOT)
    message(FATAL_ERROR "StageEnginePackagePrebuilt.cmake: STAMP_TOOL, MODULE_FILE, MODULE_NAME and DEST_ROOT are required")
endif()

set(_stamp_out "${MODULE_FILE}.platformdir.txt")
execute_process(
    COMMAND "${STAMP_TOOL}" platformdir "${_stamp_out}"
    RESULT_VARIABLE _stamp_result)
if(NOT _stamp_result EQUAL 0)
    message(FATAL_ERROR "StageEnginePackagePrebuilt.cmake: GePrebuiltStamp platformdir failed (${_stamp_result})")
endif()
file(STRINGS "${_stamp_out}" _platform_lines LIMIT_COUNT 1)
list(GET _platform_lines 0 _platform_dir)
if(NOT _platform_dir)
    message(FATAL_ERROR "StageEnginePackagePrebuilt.cmake: empty platform dir from GePrebuiltStamp")
endif()

set(_module_name_out "${MODULE_FILE}.modulefile.txt")
execute_process(
    COMMAND "${STAMP_TOOL}" modulefile "${MODULE_NAME}" "${_module_name_out}"
    RESULT_VARIABLE _module_name_result)
if(NOT _module_name_result EQUAL 0)
    message(FATAL_ERROR "StageEnginePackagePrebuilt.cmake: GePrebuiltStamp modulefile failed (${_module_name_result})")
endif()
file(STRINGS "${_module_name_out}" _module_name_lines LIMIT_COUNT 1)
list(GET _module_name_lines 0 _module_file_name)
if(NOT _module_file_name)
    message(FATAL_ERROR "StageEnginePackagePrebuilt.cmake: empty module file name from GePrebuiltStamp")
endif()

set(_dest "${DEST_ROOT}/${_platform_dir}")

# One platform dir per config per toolchain: siblings that share this build's
# <platform>-<arch>- prefix under a different fingerprint can never pass the
# loader's engine_abi gate against this tree's binaries — they are residue of
# ABI changes and only accumulate dead DLL+PDB sets (observed >200 MB per
# config). Dirs for other platform prefixes are left alone.
string(REGEX MATCH "^(.+-)[0-9a-f]+$" _unused_match "${_platform_dir}")
if(CMAKE_MATCH_1)
    file(GLOB _sibling_dirs LIST_DIRECTORIES true "${DEST_ROOT}/${CMAKE_MATCH_1}*")
    foreach(_sibling IN LISTS _sibling_dirs)
        get_filename_component(_sibling_name "${_sibling}" NAME)
        if(IS_DIRECTORY "${_sibling}" AND NOT _sibling_name STREQUAL "${_platform_dir}")
            file(REMOVE_RECURSE "${_sibling}")
        endif()
    endforeach()
endif()

file(MAKE_DIRECTORY "${_dest}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MODULE_FILE}" "${_dest}/${_module_file_name}"
    RESULT_VARIABLE _copy_result)
if(NOT _copy_result EQUAL 0)
    message(FATAL_ERROR "StageEnginePackagePrebuilt.cmake: copy '${MODULE_FILE}' failed")
endif()
if(DEFINED MODULE_PDB AND EXISTS "${MODULE_PDB}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MODULE_PDB}" "${_dest}/")
endif()
