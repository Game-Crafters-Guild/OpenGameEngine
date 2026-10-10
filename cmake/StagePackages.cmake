# Stages the repository's Packages tree next to one executable
# (ge_stage_packages, cmake/Packages.cmake) as a mirror:
#   1. copy_directory_if_different SRC into DST (unchanged files cost nothing);
#   2. the orphan prune (cmake/PruneStagedMirrorOrphans.cmake), so a package
#      that left SRC (a branch switch, a removed package) leaves DST too
#      instead of failing its native build at every editor start;
#   3. the authoring-root marker (EnginePackageAuthoringRoot.txt);
#   4. the prebuilt module overlay (cmake/OverlayEnginePackagePrebuilts.cmake),
#      which prunes its own stale fingerprint directories;
#   5. the removal of the mirror staged under the folder's former name.
# Steps 3 and 4 stage files that are not in SRC, so the mirror's record never
# names them and the prune never touches them.
# Args: -DSRC=<repo Packages> -DDST=<exe dir>/Packages
#       -DMANIFEST_DIR=<build-tree dir for the mirror's record>
#       -DAUTHORING_ROOT_FILE=<EnginePackageAuthoringRoot.txt to stage>
#       -DPREBUILT_DIR=<PackagesPrebuilt/<CONFIG>>
foreach(_arg SRC DST MANIFEST_DIR AUTHORING_ROOT_FILE PREBUILT_DIR)
    if(NOT ${_arg})
        message(FATAL_ERROR "StagePackages.cmake: ${_arg} is required and must not be empty")
    endif()
endforeach()
# The former mirror is located as DST's sibling, so DST must be an absolute path
# to a folder named Packages: anything else would name an unrelated directory.
get_filename_component(DST "${DST}" ABSOLUTE)
get_filename_component(_dst_name "${DST}" NAME)
if(NOT _dst_name STREQUAL "Packages")
    message(FATAL_ERROR "StagePackages.cmake: DST must be a folder named Packages, got '${DST}'")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${SRC}" "${DST}"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DSRC=${SRC}" "-DDST=${DST}" "-DMANIFEST_DIR=${MANIFEST_DIR}"
            -P "${CMAKE_CURRENT_LIST_DIR}/PruneStagedMirrorOrphans.cmake"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${AUTHORING_ROOT_FILE}"
            "${DST}/EnginePackageAuthoringRoot.txt"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DSRC=${PREBUILT_DIR}" "-DDST=${DST}"
            -P "${CMAKE_CURRENT_LIST_DIR}/OverlayEnginePackagePrebuilts.cmake"
    COMMAND_ERROR_IS_FATAL ANY)

# A build tree staged while the repository folder was named EnginePackages/
# keeps that mirror beside DST: nothing reads it, but it holds every package's
# sources and prebuilt modules, and its mirror record is keyed by the old
# (SRC, DST) pair, so the prune never sees it. It is removed with its records
# here, and a removal that fails (a running editor holds a module) is retried
# at the next staging. This step is removed after the first public release
# (#2913).
get_filename_component(_former_dst "${DST}" DIRECTORY)
set(_former_dst "${_former_dst}/EnginePackages")
if(IS_DIRECTORY "${_former_dst}")
    file(REMOVE_RECURSE "${_former_dst}")
    if(EXISTS "${_former_dst}")
        message(WARNING "StagePackages: could not remove ${_former_dst}; retrying at the next staging")
    endif()
endif()
file(GLOB _former_records "${MANIFEST_DIR}/EnginePackages.*.mirror-manifest")
if(_former_records)
    file(REMOVE ${_former_records})
endif()
