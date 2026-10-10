# GameEngine overlay of vcpkg's shaderc port (registry version 2026.2).
# Modifications vs upstream:
#   1. vcpkg_check_linkage(ONLY_STATIC_LIBRARY) removed: on dynamic-linkage
#      triplets (x64-windows) BUILD_SHARED_LIBS=ON builds shaderc_shared.dll.
#      glslang/SPIRV-Tools stay static libs linked INTO the DLL, so the upstream
#      concern behind the check (their unexported symbols) does not apply to
#      consumers, which see only shaderc's own C API (SHADERC_SHAREDLIB import).
#      Static-linkage triplets (arm64-osx, x64-linux) build the static library
#      exactly as the stock port does.
#   2. glslc-link-shared.patch: glslc links shaderc_shared when
#      BUILD_SHARED_LIBS is ON (upstream hardcodes the static target name).
#   3. -DSHADERC_ENABLE_EXAMPLES=OFF corrected to -DSHADERC_SKIP_EXAMPLES=ON
#      (the former is not a shaderc option; examples otherwise build).
# The stock cmake-config-export.patch already exports whichever library target
# was built, so consumers see unofficial::shaderc::shaderc_shared (dynamic) or
# unofficial::shaderc::shaderc (static).
# When upstream vcpkg bumps shaderc, resync all files and reapply 1-3.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO google/shaderc
    REF "v${VERSION}"
    SHA512 a8dbf46cd10b2fabd0f533fcc79975202a235d04019295f14d3b51bf9699978ebf332c63f14b60fb518fc4deb07c37f84f19f2450958ae2035cbfacc68d78dae
    HEAD_REF master
    PATCHES 
        disable-update-version.patch
        fix-build-type.patch
        cmake-config-export.patch
        glslc-link-shared.patch
)

configure_file(${CMAKE_CURRENT_LIST_DIR}/build-version.inc ${SOURCE_PATH}/glslc/src/build-version.inc)

set(OPTIONS "")
if(VCPKG_CRT_LINKAGE STREQUAL "dynamic")
    list(APPEND OPTIONS -DSHADERC_ENABLE_SHARED_CRT=ON)
endif()

# shaderc uses python to manipulate copyright information
vcpkg_find_acquire_program(PYTHON3)
get_filename_component(PYTHON3_EXE_PATH "${PYTHON3}" DIRECTORY)
vcpkg_add_to_path(PREPEND "${PYTHON3_EXE_PATH}")

# Add these libraries to the pkgconfig file since we patch the build to link against these
set(EXTRA_STATIC_PKGCONFIG_LIBS "-lglslang -lSPIRV-Tools-opt -lSPIRV-Tools")
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${OPTIONS}
        "-DCMAKE_PROJECT_INCLUDE=${CMAKE_CURRENT_LIST_DIR}/cmake-project-include.cmake"
        -DSHADERC_SKIP_EXAMPLES=ON
        -DSHADERC_SKIP_TESTS=true 
        "-DEXTRA_STATIC_PKGCONFIG_LIBS=${EXTRA_STATIC_PKGCONFIG_LIBS}"
)

vcpkg_cmake_install()
if(NOT VCPKG_BUILD_TYPE)
    if(VCPKG_TARGET_IS_WINDOWS)
        vcpkg_replace_string("${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-dbg/shaderc.pc" "-lglslang" "-lglslangd")
    endif()
    file(COPY "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-dbg/shaderc.pc" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig")
endif()

vcpkg_fixup_pkgconfig()
vcpkg_cmake_config_fixup(PACKAGE_NAME unofficial-shaderc CONFIG_PATH share/unofficial-shaderc)

vcpkg_copy_tools(TOOL_NAMES glslc AUTO_CLEAN)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
