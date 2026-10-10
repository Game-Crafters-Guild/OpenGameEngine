# Contract test for cmake/OverlayEnginePackagePrebuilts.cmake, run by ctest as
#   cmake -DSCRIPT=<overlay script> -DWORK=<scratch dir> -P <this file>
# The overlay copies the prebuilt intermediate's fingerprint directories onto
# the staged packages and nothing else: a file elsewhere in the intermediate
# never replaces the package's authored file at the same path.
if(NOT DEFINED SCRIPT OR NOT DEFINED WORK)
    message(FATAL_ERROR "OverlayEnginePackagePrebuiltsTest.cmake: SCRIPT and WORK are required")
endif()

set(_dst "${WORK}/bin/Packages")
set(_prebuilt "${WORK}/PackagesPrebuilt")
set(_module_dll "tool/Binaries/windows-x64-0123abcd/Tool.Editor.dll")
set(_authored_dll "tool/Tools/Vendored.dll")

file(REMOVE_RECURSE "${WORK}")
file(WRITE "${_dst}/${_authored_dll}" "authored")
file(WRITE "${_prebuilt}/${_module_dll}" "module")
file(WRITE "${_prebuilt}/${_authored_dll}" "stale")

execute_process(
    COMMAND ${CMAKE_COMMAND} -DSRC=${_prebuilt} -DDST=${_dst} -P ${SCRIPT}
    COMMAND_ERROR_IS_FATAL ANY)

if(NOT EXISTS "${_dst}/${_module_dll}")
    message(FATAL_ERROR "the prebuilt module was not overlaid: ${_dst}/${_module_dll}")
endif()
file(READ "${_dst}/${_authored_dll}" _authored)
if(NOT _authored STREQUAL "authored")
    message(FATAL_ERROR "a file outside the fingerprint directories replaced the authored "
                        "${_authored_dll} (now '${_authored}')")
endif()

message(STATUS "OverlayEnginePackagePrebuiltsTest: passed")
