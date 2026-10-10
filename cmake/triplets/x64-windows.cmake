set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# Release-only ports: DLL-only ports with pure C APIs whose debug variants
# nothing needs. Debug builds link the release import lib via CMake's
# imported-config fallback, runtime staging (cmake/OutputLayout.cmake)
# resolves the release DLL, and a release C-ABI DLL is safe inside a /MDd
# process because each DLL owns its CRT and every allocation crossing the
# boundary is freed through the library's own API.
#
# Keep the debug variant for everything else:
#  - Static C++ libs linked into Debug targets (joltphysics, yoga, glm,
#    recastnavigation, spirv-reflect): /MD objects in a /MDd link fail
#    with LNK2038.
#  - C++-interface DLLs (gtest, benchmark, pugixml, thorvg): their headers
#    bake the consumer's _ITERATOR_DEBUG_LEVEL into types that cross the
#    DLL boundary.
#  - C DLLs whose API hands the caller a buffer to free with the CRT
#    (ktx: TextureCook frees ktxTexture_WriteToMemory's buffer, the only
#    form libktx offers - it exports no paired free): a release DLL's
#    ucrtbase allocation freed via ucrtbased aborts in RtlValidateHeap.
#  - utf8proc: kept pending a drop audit. Its sole consumer
#    (PathNormalization) uses the caller-allocated decompose/reencode
#    form, so no allocation crosses the DLL boundary today.
if(PORT MATCHES "^(ffmpeg|freetype|harfbuzz|curl|sqlite3|zlib|libpng|bzip2|brotli|zstd)$")
    set(VCPKG_BUILD_TYPE release)
endif()

# Shaderc stack: shaderc is a C-ABI DLL (cmake/ports/shaderc builds
# shaderc_shared.dll; glslang and SPIRV-Tools are static libs linked INTO
# that DLL, invisible to consumers), so the release-only rationale above
# applies to the whole stack — glslang and spirv-tools have no consumer
# on this triplet other than shaderc's own port build. Their debug
# variants exist solely to step-debug into shaderc/glslang/SPIRV-Tools
# internals and cost ~1.3 GB per install tree (debug SPIRV-Tools-opt.lib
# alone is ~775 MB): skipped unless GE_SHADERC_DEBUG_PORTS is defined in
# the environment at configure time.
# VCPKG_ENV_PASSTHROUGH makes the variable TRACKED: its value is hashed
# into these three ports' ABIs (only these — the passthrough is scoped to
# this block), so opted-in installs occupy distinct binary-cache entries
# and flipping the variable reinstalls just the stack.
if(PORT MATCHES "^(shaderc|glslang|spirv-tools)$")
    set(VCPKG_ENV_PASSTHROUGH GE_SHADERC_DEBUG_PORTS)
    if(NOT DEFINED ENV{GE_SHADERC_DEBUG_PORTS})
        set(VCPKG_BUILD_TYPE release)
    endif()
endif()
