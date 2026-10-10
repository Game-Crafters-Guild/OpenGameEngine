# Engine wasm triplet, single-threaded ABI (no atomics/shared memory).
# Pairs with GE_WASM_SINGLE_THREAD engine builds — the no-COOP/COEP fallback
# config. See wasm32-emscripten.cmake for why the two ABIs need two triplets.
#
# Flags live in the chainload wrapper toolchain, NOT in VCPKG_CXX_FLAGS:
# vcpkg does not apply VCPKG_*_FLAGS when a chainload toolchain is set.

set(VCPKG_TARGET_ARCHITECTURE wasm32)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Emscripten)
set(VCPKG_ENV_PASSTHROUGH_UNTRACKED EMSDK PATH)

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/toolchains/wasm32-emscripten-nothreads.toolchain.cmake")

# Exposes "nothreads" to manifest platform expressions. harfbuzz is excluded
# on this ABI: its meson build hands -pthread to some subtargets and not
# others once meson's threads dependency resolves, and the mixed objects
# cannot link. Text shapes through its GE_HAVE_HARFBUZZ=0 fallback here.
set(VCPKG_DEP_INFO_OVERRIDE_VARS nothreads)
