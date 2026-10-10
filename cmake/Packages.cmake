# Engine packages — the first-party packages in the repo's Packages/<name>
# (package.json + Assets/ + native module sources). The repo tree is the
# authoring side; every runtime consumes a STAGED copy:
#   - apps: <exe dir>/Packages/<name> (ge_stage_packages), the
#     root PackageResolver's implicit engine-package resolution scans
#     (GameEngine::kStagedEnginePackagesDirName). It serves runs from the
#     build tree only: a game carries the packages it uses under its content
#     root's Packages/, staged by the build pipeline, and the macOS template
#     scrub leaves this copy out of the Player.app every export starts from;
#   - tests: exe-anchored copies staged by the suites that need them (e.g.
#     AssetSystemTests' TestData/Packages), so package fixtures never
#     reach back into the source tree.
#
# A developer build also stages a file naming the repo tree it copied from, so
# the editor can write a package's own asset metadata back to where it is
# committed (see ge_stage_packages below); a shipped tree has none.
#
# The eztree package additionally needs the EZ-Tree upstream preset JSONs from
# the ez-tree-upstream vcpkg port. They are a build INPUT staged exe-anchored
# under Assets/EZTree/Presets by consumers (LoadPreset resolves them there);
# GE_EZTREE_PRESETS_SOURCE_DIR points at the vcpkg source of truth.

# ez-tree-upstream is an unconditional vcpkg manifest dependency: the preset
# JSONs are a build input of the always-shipped eztree package.
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(_GE_EZTREE_UPSTREAM_DIR
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/ez-tree-upstream")
elseif(DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(_GE_EZTREE_UPSTREAM_DIR
        "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/ez-tree-upstream")
else()
    message(FATAL_ERROR "The eztree package requires the ez-tree-upstream vcpkg package")
endif()

if(NOT EXISTS "${_GE_EZTREE_UPSTREAM_DIR}/src/lib/presets/oak_large.json")
    message(FATAL_ERROR "EZ-Tree vcpkg upstream presets were not installed: ${_GE_EZTREE_UPSTREAM_DIR}")
endif()

set(GE_EZTREE_PRESETS_SOURCE_DIR "${_GE_EZTREE_UPSTREAM_DIR}/src/lib/presets"
    CACHE INTERNAL "EZ-Tree upstream preset JSON source dir (staged next to consumers)")

# Names the repository tree the staged packages were copied FROM, staged beside
# them so a developer build can write a package's own asset metadata back to
# where it is committed. Without it, an inspector edit would land in the staged
# copy that copy_directory_if_different below overwrites on the next build of
# any target, and the repository would never learn. A shipped or packaged tree
# carries no such file and mounts its packages read-only — the file name is
# GameEngine::kEnginePackageAuthoringRootFile (Assets/Packages/PackageResolver.h),
# read by the resolver.
set(GE_ENGINE_PACKAGE_AUTHORING_ROOT_FILE
    "${CMAKE_BINARY_DIR}/cmake/EnginePackageAuthoringRoot.txt"
    CACHE INTERNAL "Staged marker naming the repo's Packages tree")
file(WRITE "${GE_ENGINE_PACKAGE_AUTHORING_ROOT_FILE}" "${CMAKE_SOURCE_DIR}/Packages\n")

# Stage the whole Packages tree next to a target's executable (apps AND
# test executables that consume staged packages — tests must never reach back
# into the source tree) as a mirror (cmake/StagePackages.cmake):
# copy_directory_if_different keeps incremental builds cheap, and the orphan
# prune removes a package that left the source tree (a branch switch, a removed
# package), whose native module would otherwise fail to build at every editor
# start. The prune's record lives under the build tree. Native module build
# outputs never land here (engine-package module builds go to the global
# package cache's .derived sibling). Prebuilt module binaries
# (ge_add_engine_package_module) are overlaid from the build-tree intermediate
# when present — repo Packages/ holds no module binaries (its one
# committed binary is the vendored unity-import/Tools/UnityConverter.dll), so
# the prune never names them — and staged fingerprint dirs the intermediate no
# longer produces are pruned by the overlay script (they can never load and only
# accumulate).
function(ge_stage_packages target_name)
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DSRC=${CMAKE_SOURCE_DIR}/Packages
                -DDST=$<TARGET_FILE_DIR:${target_name}>/Packages
                -DMANIFEST_DIR=${CMAKE_BINARY_DIR}/stage-stamps/$<CONFIG>/mirror-manifests
                -DAUTHORING_ROOT_FILE=${GE_ENGINE_PACKAGE_AUTHORING_ROOT_FILE}
                -DPREBUILT_DIR=${CMAKE_BINARY_DIR}/PackagesPrebuilt/$<CONFIG>
                -P ${CMAKE_SOURCE_DIR}/cmake/StagePackages.cmake
        COMMENT "Staging Packages next to ${target_name}")
endfunction()

include(${CMAKE_CURRENT_LIST_DIR}/EnginePackageDefines.cmake)

# ---------------------------------------------------------------------------
# Prebuilt engine-package native modules.
#
# Engine packages ship WITH the engine build, so the engine build prebuilds
# their native modules and stages the DLLs beside each package in the P3
# prebuilt layout the loader already probes:
#
#   Packages/<pkg>/<prebuilt-subdir>/<platform>-<arch>-<fp8>/<Module>.dll   (.dylib, .so)
#                                                                 /engine_abi.txt
#
# so the first project open loads every engine-package module fingerprint-
# checked from the staged binaries instead of paying a cmake+MSVC source
# compile per package (the pre-prebuilt behavior, which remains the loud
# fallback whenever the fingerprint or engine_abi marker does not match).
#
# Each module compiles as a MODULE library against the same interface the
# NativeScriptManager source build uses — the Engine (+ EditorSDK for
# Editor-kind) import libs, the engine's public headers and interface defines,
# the SDK globaldefs, and the UserModuleEntry ABI-exports TU — with the
# engine's global toolchain flags, so its exported toolchain fingerprint
# matches the editor's (the LoadModule handshake stays the final gate).
#
# The fingerprint dir name, the staged module file name and the engine_abi
# digest are computed at BUILD time by the GePrebuiltStamp tool (same TUs, same
# flags, same build), never re-derived in CMake. The module binaries land in
# the ${CMAKE_BINARY_DIR}/PackagesPrebuilt/ per-config intermediate;
# ge_stage_packages overlays it onto every
# consumer's staged tree, and ge_stage_engine_package_markers writes the
# engine_abi markers after the SDK is staged (the digest hashes the STAGED
# import libs' identity).
#
# The module's EFFECTIVE compile defines come from its package manifest through
# ge_engine_package_module_defines (cmake/EnginePackageDefines.cmake), the same
# derivation the resolver runs — they are never written out per call, because a
# hand-written list is free to drift from the one the editor compiles the module
# with, and the digest chains the defines one after another, so the order counts
# as much as the set. A list that differs leaves the engine_abi marker
# mismatched and the shipped prebuilt unusable (a loud source-build fallback).
# ---------------------------------------------------------------------------
function(ge_add_engine_package_module target_name)
    cmake_parse_arguments(GEPKG "" "PACKAGE;MODULE_NAME;SOURCE_ROOT;KIND;PREBUILT_SUBDIR" "EXTRA_INCLUDE_DIRS" ${ARGN})
    if(NOT GEPKG_PACKAGE OR NOT GEPKG_MODULE_NAME OR NOT GEPKG_SOURCE_ROOT OR NOT GEPKG_KIND)
        message(FATAL_ERROR "ge_add_engine_package_module: PACKAGE, MODULE_NAME, SOURCE_ROOT and KIND are required")
    endif()
    if(GEPKG_UNPARSED_ARGUMENTS)
        # A silently swallowed argument reads as applied. This catches a
        # hand-written DEFINES in particular: the defines are derived below.
        message(FATAL_ERROR "ge_add_engine_package_module: unrecognized argument(s) '${GEPKG_UNPARSED_ARGUMENTS}'")
    endif()
    if(NOT GEPKG_PREBUILT_SUBDIR)
        set(GEPKG_PREBUILT_SUBDIR "Binaries")
    endif()
    # The manifests are configure inputs of this target: an edited `name` or
    # `dependencies` changes the list the module is compiled and stamped with,
    # and must re-derive it rather than wait for an unrelated reconfigure. The
    # whole set is listed because a dependency walk can read any manifest.
    file(GLOB _gepkg_manifests CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/Packages/*/package.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_gepkg_manifests})
    ge_engine_package_module_defines("${CMAKE_SOURCE_DIR}/Packages" "${GEPKG_PACKAGE}"
        _gepkg_defines)

    file(GLOB_RECURSE _gepkg_sources CONFIGURE_DEPENDS
        "${GEPKG_SOURCE_ROOT}/*.cpp" "${GEPKG_SOURCE_ROOT}/*.cc"
        "${GEPKG_SOURCE_ROOT}/*.cxx" "${GEPKG_SOURCE_ROOT}/*.c")
    if(NOT _gepkg_sources)
        message(FATAL_ERROR "ge_add_engine_package_module: no sources under ${GEPKG_SOURCE_ROOT}")
    endif()

    add_library(${target_name} MODULE
        ${_gepkg_sources}
        "${CMAKE_SOURCE_DIR}/Engine/Modules/NativeScripting/GameSDK/Source/UserModuleEntry.cpp")
    set_target_properties(${target_name} PROPERTIES
        OUTPUT_NAME "${GEPKG_MODULE_NAME}"
        UNITY_BUILD OFF
        FOLDER "Packages")
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    if(MSVC)
        # Same exposure as the generated user-module project, on the path that matters more:
        # this target builds the SHIPPED prebuilt, so a TU crossing COFF's 65,279-section cap
        # breaks the engine's own build for everyone rather than one runtime plugin rebuild.
        # Measured on eztree's editor plugin, the worst TU in this class: 56,142 sections in
        # Debug (86% of cap), 53,273 in Release. Release sits ABOVE DebugFast, so there is no
        # config that is safe and no scoping that would help.
        target_compile_options(${target_name} PRIVATE /bigobj)
    endif()
    # The SDK manifest's globaldefs — the source build compiles with them too.
    target_compile_definitions(${target_name} PRIVATE
        GLM_FORCE_DEPTH_ZERO_TO_ONE NOMINMAX WIN32_LEAN_AND_MEAN ${_gepkg_defines})
    # vcpkg headers (nlohmann via SettingsStore.h, glm, …): the staged-SDK
    # source build gets them from the SDK's vcpkg-include stage; the in-build
    # compile needs the vcpkg installed tree directly (the editor headers pull
    # them, and Engine's INTERFACE does not re-export the vcpkg include dir).
    set(_gepkg_vcpkg_include "")
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET AND
       IS_DIRECTORY "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
        set(_gepkg_vcpkg_include "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
    elseif(DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET AND
       IS_DIRECTORY "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
        set(_gepkg_vcpkg_include "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
    endif()
    target_include_directories(${target_name} PRIVATE
        "${GEPKG_SOURCE_ROOT}"
        "${CMAKE_SOURCE_DIR}/Engine/Modules/NativeScripting/Include"
        "${CMAKE_SOURCE_DIR}/Engine/Modules/NativeScripting/GameSDK/Include"
        ${_gepkg_vcpkg_include}
        ${GEPKG_EXTRA_INCLUDE_DIRS})
    target_link_libraries(${target_name} PRIVATE Engine)
    if(GEPKG_KIND STREQUAL "editor")
        target_link_libraries(${target_name} PRIVATE EditorSDK)
    elseif(NOT GEPKG_KIND STREQUAL "runtime")
        message(FATAL_ERROR "ge_add_engine_package_module: KIND must be editor|runtime")
    endif()

    # Stage DLL (+PDB on MSVC) into the per-config intermediate under the
    # platform dir the stamp tool names for THIS toolchain.
    set(_gepkg_pdb_arg "")
    if(MSVC)
        set(_gepkg_pdb_arg -DMODULE_PDB=$<TARGET_PDB_FILE:${target_name}>)
    endif()
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DSTAMP_TOOL=$<TARGET_FILE:GePrebuiltStamp>
            -DMODULE_FILE=$<TARGET_FILE:${target_name}>
            -DMODULE_NAME=${GEPKG_MODULE_NAME}
            ${_gepkg_pdb_arg}
            -DDEST_ROOT=${CMAKE_BINARY_DIR}/PackagesPrebuilt/$<CONFIG>/${GEPKG_PACKAGE}/${GEPKG_PREBUILT_SUBDIR}
            -P ${CMAKE_SOURCE_DIR}/cmake/StageEnginePackagePrebuilt.cmake
        COMMENT "Staging prebuilt ${GEPKG_MODULE_NAME} into PackagesPrebuilt ($<CONFIG>)")
    add_dependencies(${target_name} GePrebuiltStamp)

    if(NOT TARGET EnginePackagePrebuilts)
        add_custom_target(EnginePackagePrebuilts)
    endif()
    add_dependencies(EnginePackagePrebuilts ${target_name})

    # Record for the marker pass (pkg-relative prebuilt subdir, module kind,
    # comma-joined defines — commas survive the list file round-trip).
    string(REPLACE ";" "," _gepkg_defines_joined "${_gepkg_defines}")
    set_property(GLOBAL APPEND PROPERTY GE_ENGINE_PACKAGE_PREBUILT_MODULES
        "${GEPKG_PACKAGE}/${GEPKG_PREBUILT_SUBDIR}|${GEPKG_KIND}|${_gepkg_defines_joined}")
endfunction()

# Writes the engine_abi markers into <exe dir>/Packages/... AFTER the
# SDK and the packages are staged (POST_BUILD order = call order): the digest
# hashes the STAGED SDK import libs, so it must run against their final
# identity. A marker mismatch at load time is a loud source-build fallback,
# never a wrong load. The module list rides a configure-time file — embedding
# a ;-list in a COMMAND argument does not survive MSBuild quoting.
function(ge_stage_engine_package_markers host_target)
    get_property(_gepkg_modules GLOBAL PROPERTY GE_ENGINE_PACKAGE_PREBUILT_MODULES)
    if(NOT _gepkg_modules)
        return()
    endif()
    set(_gepkg_modules_file "${CMAKE_BINARY_DIR}/cmake/EnginePackagePrebuiltModules.txt")
    list(JOIN _gepkg_modules "\n" _gepkg_modules_content)
    file(WRITE "${_gepkg_modules_file}" "${_gepkg_modules_content}\n")
    # The marker contract test (Tests/CMakeLists.txt) recomputes from the same list.
    set_property(GLOBAL PROPERTY GE_ENGINE_PACKAGE_MARKERS_MODULES_FILE "${_gepkg_modules_file}")
    add_custom_command(TARGET ${host_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DSTAMP_TOOL=$<TARGET_FILE:GePrebuiltStamp>
            -DSDK_DIR=$<TARGET_FILE_DIR:${host_target}>/SDK
            -DPACKAGES_DIR=$<TARGET_FILE_DIR:${host_target}>/Packages
            -DMODULES_FILE=${_gepkg_modules_file}
            -P ${CMAKE_SOURCE_DIR}/cmake/StageEnginePackageMarkers.cmake
        COMMENT "Writing engine_abi markers for prebuilt engine-package modules")
endfunction()
