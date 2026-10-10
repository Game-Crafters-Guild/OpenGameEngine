# Engine wasm triplet: browser/wasm32 via the pinned Emscripten SDK
# (Tools/emsdk/setup.sh), threads enabled.
#
# All objects in a shared-memory wasm link must be compiled with -pthread
# (atomics + bulk-memory), so the flag is triplet-wide: every port matches the
# engine's threaded link. The single-threaded fallback config uses
# wasm32-emscripten-nothreads instead — the two ABIs cannot mix in one binary.
#
# Flags live in the chainload wrapper toolchain, NOT in VCPKG_CXX_FLAGS:
# vcpkg does not apply VCPKG_*_FLAGS when a chainload toolchain is set.

set(VCPKG_TARGET_ARCHITECTURE wasm32)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Emscripten)
set(VCPKG_ENV_PASSTHROUGH_UNTRACKED EMSDK PATH)

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/toolchains/wasm32-emscripten.toolchain.cmake")
