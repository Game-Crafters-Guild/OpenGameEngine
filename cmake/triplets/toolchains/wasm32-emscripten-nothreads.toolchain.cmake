# Chainload toolchain for the single-threaded wasm32-emscripten-nothreads
# triplet. See wasm32-emscripten.toolchain.cmake for why flags live here
# instead of VCPKG_CXX_FLAGS. No -pthread anywhere: this is the no-COOP/COEP
# fallback ABI and must not emit shared-memory objects.

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

string(APPEND CMAKE_CXX_FLAGS_INIT " -DHB_NO_PRAGMA_GCC_DIAGNOSTIC_ERROR")
