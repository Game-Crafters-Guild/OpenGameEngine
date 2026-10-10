# Contract test for cmake/StagePackages.cmake, run by ctest as
#   cmake -DSCRIPT=<staging script> -DWORK=<scratch dir> -P <this file>
# Stages two scratch packages the way ge_stage_packages does, removes
# one from the source and stages again: the removed package leaves the staged
# tree with no empty directory, while what the staging did not take from the
# source survives: a file another writer put beside the packages, the
# authoring-root marker and the overlaid prebuilt module binaries. The mirror a
# build staged under the folder's former name, EnginePackages/, leaves with its
# record at the first staging. A relative DST resolves against the working
# directory, so the former mirror removed is its sibling there; a DST not named
# Packages is refused before anything is staged or removed.
if(NOT DEFINED SCRIPT OR NOT DEFINED WORK)
    message(FATAL_ERROR "StagePackagesTest.cmake: SCRIPT and WORK are required")
endif()

set(_src "${WORK}/Packages")
set(_dst "${WORK}/bin/Packages")
set(_prebuilt "${WORK}/PackagesPrebuilt")
set(_prebuilt_dll "alpha/Binaries/windows-x64-0123abcd/Alpha.dll")

function(stage)
    execute_process(
        COMMAND ${CMAKE_COMMAND}
                -DSRC=${_src} -DDST=${_dst} -DMANIFEST_DIR=${WORK}/manifests
                -DAUTHORING_ROOT_FILE=${WORK}/EnginePackageAuthoringRoot.txt
                -DPREBUILT_DIR=${_prebuilt}
                -P ${SCRIPT}
        COMMAND_ERROR_IS_FATAL ANY)
endfunction()

function(expect_present)
    foreach(_rel IN LISTS ARGN)
        if(NOT EXISTS "${_dst}/${_rel}")
            message(FATAL_ERROR "expected ${_rel} to stay staged under ${_dst}, it is gone")
        endif()
    endforeach()
endfunction()

file(REMOVE_RECURSE "${WORK}")
file(WRITE "${_src}/alpha/package.json" "{}")
file(WRITE "${_src}/alpha/Assets/alpha.txt" "alpha")
file(WRITE "${_src}/beta/package.json" "{}")
file(WRITE "${_src}/beta/Native/Beta.cpp" "beta")
file(WRITE "${WORK}/EnginePackageAuthoringRoot.txt" "${_src}\n")
file(WRITE "${_prebuilt}/${_prebuilt_dll}" "dll")
set(_former_dst "${WORK}/bin/EnginePackages")
set(_former_record "${WORK}/manifests/EnginePackages.0123abcd.mirror-manifest")
file(WRITE "${_former_dst}/alpha/package.json" "{}")
file(WRITE "${_former_dst}/${_prebuilt_dll}" "dll")
file(WRITE "${_former_record}" "alpha/package.json\n")

stage()
foreach(_former IN ITEMS "${_former_dst}" "${_former_record}")
    if(EXISTS "${_former}")
        message(FATAL_ERROR "the mirror staged under the former folder name survived the staging: ${_former}")
    endif()
endforeach()
expect_present(alpha/package.json alpha/Assets/alpha.txt beta/package.json beta/Native/Beta.cpp
               EnginePackageAuthoringRoot.txt ${_prebuilt_dll})
file(WRITE "${_dst}/build-product.txt" "written beside the packages by another step")

file(REMOVE_RECURSE "${_src}/beta")
stage()
if(EXISTS "${_dst}/beta")
    message(FATAL_ERROR "the package removed from the source is still staged: ${_dst}/beta")
endif()
expect_present(alpha/package.json alpha/Assets/alpha.txt EnginePackageAuthoringRoot.txt ${_prebuilt_dll}
               build-product.txt)

# A relative DST stages into the working directory and removes the former
# mirror beside it there, never one at the drive root.
set(_cwd "${WORK}/cwd")
file(WRITE "${_cwd}/EnginePackages/alpha/package.json" "{}")
execute_process(
    COMMAND ${CMAKE_COMMAND}
            -DSRC=${_src} -DDST=Packages -DMANIFEST_DIR=${WORK}/manifests
            -DAUTHORING_ROOT_FILE=${WORK}/EnginePackageAuthoringRoot.txt
            -DPREBUILT_DIR=${_prebuilt}
            -P ${SCRIPT}
    WORKING_DIRECTORY "${_cwd}"
    COMMAND_ERROR_IS_FATAL ANY)
if(NOT EXISTS "${_cwd}/Packages/alpha/package.json" OR EXISTS "${_cwd}/EnginePackages")
    message(FATAL_ERROR "a relative DST did not resolve against the working directory ${_cwd}")
endif()

# A DST not named Packages is refused, and the EnginePackages beside it survives.
set(_other "${WORK}/other")
file(WRITE "${_other}/EnginePackages/keep.txt" "not this staging's")
execute_process(
    COMMAND ${CMAKE_COMMAND}
            -DSRC=${_src} -DDST=${_other}/NotPackages -DMANIFEST_DIR=${WORK}/manifests
            -DAUTHORING_ROOT_FILE=${WORK}/EnginePackageAuthoringRoot.txt
            -DPREBUILT_DIR=${_prebuilt}
            -P ${SCRIPT}
    RESULT_VARIABLE _other_result OUTPUT_QUIET ERROR_QUIET)
if(_other_result EQUAL 0 OR NOT EXISTS "${_other}/EnginePackages/keep.txt"
   OR EXISTS "${_other}/NotPackages")
    message(FATAL_ERROR "a DST not named Packages was staged or removed a sibling (exit ${_other_result})")
endif()

message(STATUS "StagePackagesTest: passed")
