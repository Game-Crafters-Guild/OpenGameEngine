# Contract test for the engine_abi markers the Editor build writes beside its
# prebuilt engine-package modules, run by ctest against the built Editor as
#   cmake -DSCRIPT=<StageEnginePackageMarkers.cmake> -DWORK=<scratch dir>
#         -DSTAMP_TOOL=<GePrebuiltStamp> -DSDK_DIR=<editor dir>/SDK
#         -DPACKAGES_DIR=<editor dir>/Packages -DMODULES_FILE=<module list>
#         [-DAPP_BUNDLE=<Editor.app>] -P <this file>
# Every module's marker equals the digest of the SDK as the build left it, the
# digest the editor computes at project open: a build step that changes an SDK
# library after the markers are written (signing one changes its size) leaves
# every prebuilt refused and rebuilt from source. The digests are recomputed by
# the production marker script into a scratch copy of the marker directories.
# On macOS the bundle's signature also verifies with the markers in it.
if(NOT DEFINED SCRIPT OR NOT DEFINED WORK OR NOT DEFINED STAMP_TOOL OR NOT DEFINED SDK_DIR
   OR NOT DEFINED PACKAGES_DIR OR NOT DEFINED MODULES_FILE)
    message(FATAL_ERROR "EnginePackageMarkersTest.cmake: SCRIPT, WORK, STAMP_TOOL, SDK_DIR, PACKAGES_DIR and MODULES_FILE are required")
endif()

set(_work_packages "${WORK}/Packages")
file(REMOVE_RECURSE "${WORK}")

file(STRINGS "${MODULES_FILE}" _entries)
set(_subdirs "")
foreach(_entry IN LISTS _entries)
    if(NOT _entry)
        continue()
    endif()
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _subdir)
    list(APPEND _subdirs "${_subdir}")
    file(GLOB _markers "${PACKAGES_DIR}/${_subdir}/*/engine_abi.txt")
    foreach(_marker IN LISTS _markers)
        file(RELATIVE_PATH _marker_relative "${PACKAGES_DIR}" "${_marker}")
        get_filename_component(_platform_relative "${_marker_relative}" DIRECTORY)
        file(MAKE_DIRECTORY "${_work_packages}/${_platform_relative}")
    endforeach()
endforeach()
list(LENGTH _subdirs _module_count)
if(_module_count EQUAL 0)
    message(FATAL_ERROR "no prebuilt engine-package modules listed in '${MODULES_FILE}'")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -DSTAMP_TOOL=${STAMP_TOOL} -DSDK_DIR=${SDK_DIR}
            -DPACKAGES_DIR=${_work_packages} -DMODULES_FILE=${MODULES_FILE} -P ${SCRIPT}
    COMMAND_ERROR_IS_FATAL ANY)

set(_failures "")
foreach(_subdir IN LISTS _subdirs)
    file(GLOB _recomputed "${_work_packages}/${_subdir}/*/engine_abi.txt")
    list(LENGTH _recomputed _recomputed_count)
    if(NOT _recomputed_count EQUAL 1)
        string(APPEND _failures "\n  ${_subdir}: ${_recomputed_count} marker(s) for this toolchain, expected 1")
        continue()
    endif()
    file(RELATIVE_PATH _marker_relative "${_work_packages}" "${_recomputed}")
    file(STRINGS "${_recomputed}" _expected LIMIT_COUNT 1)
    file(STRINGS "${PACKAGES_DIR}/${_marker_relative}" _staged LIMIT_COUNT 1)
    if(NOT _staged STREQUAL _expected)
        string(APPEND _failures "\n  ${_marker_relative}: staged '${_staged}', the SDK as built digests to '${_expected}'")
    endif()
endforeach()
if(_failures)
    message(FATAL_ERROR "engine_abi markers do not match the Editor's final SDK; the editor refuses these prebuilts:${_failures}")
endif()

if(DEFINED APP_BUNDLE)
    execute_process(
        COMMAND codesign --verify --deep --strict "${APP_BUNDLE}"
        RESULT_VARIABLE _verify_result
        ERROR_VARIABLE _verify_error)
    if(NOT _verify_result EQUAL 0)
        message(FATAL_ERROR "'${APP_BUNDLE}' does not verify with its engine_abi markers in it:\n${_verify_error}")
    endif()
endif()

message(STATUS "EnginePackageMarkersTest: ${_module_count} marker(s) match the Editor's final SDK")
