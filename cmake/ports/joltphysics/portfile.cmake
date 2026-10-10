# GameEngine overlay of vcpkg's joltphysics port.
# Sole modification: -DFLOATING_POINT_EXCEPTIONS_ENABLED=OFF in the Release build only.
# Jolt's upstream defaults this ON for MSVC Debug+Release, which installs SSE FP exception
# traps inside Jolt's .cpp scope guards (baked into the compiled .lib). That costs per-frame
# time in physics-active scenes even in optimized builds. Only meaningful on MSVC; no-op on
# Linux/macOS where Jolt's FPE path is compiler-gated to MSVC anyway (Jolt.cmake:521).
#
# Scope: Release variant only (OPTIONS_RELEASE). Debug variant keeps FPE to preserve the
# strict-safety behavior of the Debug config. GameEngine's DebugFast/RelWithDebInfo/Release
# configs consume the Release variant, so they pick up FPE-free Jolt.
#
# When upstream vcpkg bumps joltphysics version, resync this portfile and vcpkg.json.

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO jrouwe/JoltPhysics
    REF "v${VERSION}"
    SHA512 bc6f2436bef91a6ffd09eee98186be645f6e9a9b3f65a7a645abf00432ac40ada03320c1fe946661817a94eda372cf8d88e4889991cdd25f1c16ecf9a4486677
    HEAD_REF master
)

string(COMPARE EQUAL "${VCPKG_CRT_LINKAGE}" "static" USE_STATIC_CRT)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        debugrenderer       DEBUG_RENDERER_IN_DEBUG_AND_RELEASE
        profiler            PROFILER_IN_DEBUG_AND_RELEASE
        rtti                CPP_RTTI_ENABLED
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/Build"
    OPTIONS
        -DTARGET_UNIT_TESTS=OFF
        -DTARGET_HELLO_WORLD=OFF
        -DTARGET_PERFORMANCE_TEST=OFF
        -DTARGET_SAMPLES=OFF
        -DTARGET_VIEWER=OFF
        -DCROSS_PLATFORM_DETERMINISTIC=OFF
        -DINTERPROCEDURAL_OPTIMIZATION=OFF
        -DUSE_STATIC_MSVC_RUNTIME_LIBRARY=${USE_STATIC_CRT}
        -DENABLE_ALL_WARNINGS=OFF
        -DOVERRIDE_CXX_FLAGS=OFF
        -DJPH_USE_DX12=OFF
        -DJPH_USE_VK=OFF
        -DJPH_USE_MTL=OFF
        ${FEATURE_OPTIONS}
    OPTIONS_RELEASE
        -DGENERATE_DEBUG_SYMBOLS=OFF
        -DFLOATING_POINT_EXCEPTIONS_ENABLED=OFF
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_cmake_config_fixup(PACKAGE_NAME Jolt CONFIG_PATH "lib/cmake/Jolt")

# Re-add JPH_FLOATING_POINT_EXCEPTIONS_ENABLED for MSVC Debug consumers only.
# vcpkg_cmake_config_fixup canonicalizes the exported JoltConfig.cmake from the
# Release configure output. Because we passed FLOATING_POINT_EXCEPTIONS_ENABLED=OFF
# in OPTIONS_RELEASE, the exported config has no FPE define for any consumer, but on
# MSVC the Debug variant Jolt.lib was still built with FPE on (we only changed the
# Release variant). That mismatch trips Jolt's runtime version_id check at
# JPH::RegisterTypes. Gate the injected define on CXX_COMPILER_ID:MSVC because Jolt's
# own lib-side FPE is gated on MSVC at Jolt.cmake:521 — on clang/gcc, neither variant
# of Jolt.a defines FPE internally, so we must not add it consumer-side either.
file(APPEND "${CURRENT_PACKAGES_DIR}/share/Jolt/JoltConfig.cmake"
"\n# Overlay-injected: per-config FPE define to match per-variant Jolt.lib FPE state.\n# MSVC-only: Jolt's lib-side FPE is gated on MSVC at Jolt.cmake:521.\nset_property(TARGET Jolt::Jolt APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS\n    \"$<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:MSVC>>:JPH_FLOATING_POINT_EXCEPTIONS_ENABLED>\")\n")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
