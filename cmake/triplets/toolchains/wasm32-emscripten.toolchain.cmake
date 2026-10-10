# Chainload toolchain for the threaded wasm32-emscripten triplet.
#
# vcpkg does NOT apply VCPKG_C_FLAGS/VCPKG_CXX_FLAGS when a chainload
# toolchain is set (documented: the chainload file owns all flags), so the
# ABI-critical flags live here, where both CMake ports and — via
# vcpkg_cmake_get_vars flag detection — meson ports pick them up.
#
# -pthread: every object in a shared-memory wasm link needs atomics +
# bulk-memory; the whole triplet must match the engine's threaded ABI.
# HB_NO_PRAGMA_GCC_DIAGNOSTIC_ERROR: harfbuzz's hb.hh self-promotes -Wunused
# to an error via pragma (command-line -Wno-* cannot override a pragma), and
# emscripten's clang 21 folds -Wunused-template into -Wunused, breaking the
# 14.x build. The macro is harfbuzz's own opt-out; defining it triplet-wide
# is inert for every other port.

if(DEFINED ENV{EMSDK} AND EXISTS "$ENV{EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
    set(GE_EMSDK_ROOT "$ENV{EMSDK}")
else()
    get_filename_component(GE_EMSDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../Tools/emsdk/emsdk" ABSOLUTE)
endif()
if(NOT EXISTS "${GE_EMSDK_ROOT}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
    message(FATAL_ERROR
        "Emscripten toolchain not found (looked at '${GE_EMSDK_ROOT}'). "
        "Run Tools/emsdk/setup.sh, or export EMSDK to an activated emsdk install.")
endif()
include("${GE_EMSDK_ROOT}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")

string(APPEND CMAKE_C_FLAGS_INIT " -pthread")
string(APPEND CMAKE_CXX_FLAGS_INIT " -pthread -DHB_NO_PRAGMA_GCC_DIAGNOSTIC_ERROR")
string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " -pthread")
string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT " -pthread")
string(APPEND CMAKE_MODULE_LINKER_FLAGS_INIT " -pthread")
