# Contract test for cmake/StageEnginePackagePrebuilt.cmake, run by ctest as
#   cmake -DSCRIPT=<staging script> -DWORK=<scratch dir> -DSTAMP_TOOL=<GePrebuiltStamp>
#         -DMODULE_PREFIX=<CMAKE_SHARED_MODULE_PREFIX> -DMODULE_SUFFIX=<CMAKE_SHARED_MODULE_SUFFIX>
#         -P <this file>
# A MODULE library built under this platform's CMake naming (lib<Module>.so on
# macOS and Linux) is staged under the one file name the loader probes,
# <Module>.dll / .dylib / .so with no "lib" prefix, and under no other name.
if(NOT DEFINED SCRIPT OR NOT DEFINED WORK OR NOT DEFINED STAMP_TOOL OR NOT DEFINED MODULE_SUFFIX)
    message(FATAL_ERROR "StageEnginePackagePrebuiltTest.cmake: SCRIPT, WORK, STAMP_TOOL and MODULE_SUFFIX are required")
endif()

set(_module_name "WaterPack.Editor")
if(CMAKE_HOST_WIN32)
    set(_loader_file "${_module_name}.dll")
elseif(CMAKE_HOST_APPLE)
    set(_loader_file "${_module_name}.dylib")
else()
    set(_loader_file "${_module_name}.so")
endif()

set(_built "${WORK}/out/${MODULE_PREFIX}${_module_name}${MODULE_SUFFIX}")
set(_dest_root "${WORK}/Binaries")
file(REMOVE_RECURSE "${WORK}")
file(WRITE "${_built}" "module")

execute_process(
    COMMAND ${CMAKE_COMMAND} -DSTAMP_TOOL=${STAMP_TOOL} -DMODULE_FILE=${_built}
            -DMODULE_NAME=${_module_name} -DDEST_ROOT=${_dest_root} -P ${SCRIPT}
    COMMAND_ERROR_IS_FATAL ANY)

file(GLOB _staged RELATIVE "${_dest_root}" "${_dest_root}/*/*")
list(LENGTH _staged _staged_count)
if(NOT _staged_count EQUAL 1)
    message(FATAL_ERROR "expected one staged module file, found ${_staged_count}: '${_staged}'")
endif()
get_filename_component(_staged_file "${_staged}" NAME)
if(NOT _staged_file STREQUAL _loader_file)
    message(FATAL_ERROR "the module built as '${MODULE_PREFIX}${_module_name}${MODULE_SUFFIX}' was staged as "
                        "'${_staged_file}'; the loader probes '${_loader_file}'")
endif()

message(STATUS "StageEnginePackagePrebuiltTest: passed")
