# Prebuilt wgpu-native release archives (no Rust toolchain in the build).
# The archive layout is include/webgpu/{webgpu.h,wgpu.h} + lib/ with a static
# and a shared library; the engine links the STATIC library so the backend
# needs no runtime sidecar staging. Version-pinned: naga (the WGSL cook
# translator vendored in Tools/ShaderCook) must match this release's wgpu.
set(GE_WGPU_VERSION "v29.0.1.1")

if(VCPKG_TARGET_IS_OSX)
    if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
        set(GE_WGPU_PLATFORM "macos-aarch64")
        set(GE_WGPU_SHA512 c66d94b51a1bb36c2199ffd4c10e60b497714c13100946499f24ab7eb7ca65f04460aee0496af842d8904ad883bd492c1c30bf002d8ff5539c8ec8e11b84f104)
    else()
        set(GE_WGPU_PLATFORM "macos-x86_64")
        set(GE_WGPU_SHA512 16d36a9349647091e240606fdbcb34078ee29184636d15bdbcdeb6e3537a5b5a3e6896bbd36600ef69b0d9d6bd6ad06832c4e292245a1c0b86454483cd45e4bb)
    endif()
elseif(VCPKG_TARGET_IS_WINDOWS)
    set(GE_WGPU_PLATFORM "windows-x86_64-msvc")
    set(GE_WGPU_SHA512 84f44adeb55001b74d97fee5770a2973a74118f5e54b8fd63619ac9a3b28879f28dbfcc34d0edc29f99fe216f316b19d9635ff4f80a13369f10f002904e06c63)
else()
    set(GE_WGPU_PLATFORM "linux-x86_64")
    set(GE_WGPU_SHA512 5ebd1bbb86b4a007741bab4577367920fb8ffe6d398f32057b614d6d4e04cf6a92a25e008c389cc9c57bce47c20474c19643a41d5c97b902cb36d40437ebbb79)
endif()

vcpkg_download_distfile(GE_WGPU_ARCHIVE
    URLS "https://github.com/gfx-rs/wgpu-native/releases/download/${GE_WGPU_VERSION}/wgpu-${GE_WGPU_PLATFORM}-release.zip"
    FILENAME "wgpu-native-${GE_WGPU_VERSION}-${GE_WGPU_PLATFORM}.zip"
    SHA512 ${GE_WGPU_SHA512}
)

vcpkg_extract_source_archive(GE_WGPU_SOURCE
    ARCHIVE "${GE_WGPU_ARCHIVE}"
    NO_REMOVE_ONE_LEVEL
)

file(INSTALL "${GE_WGPU_SOURCE}/include/webgpu/webgpu.h"
             "${GE_WGPU_SOURCE}/include/webgpu/wgpu.h"
     DESTINATION "${CURRENT_PACKAGES_DIR}/include/webgpu")

if(VCPKG_TARGET_IS_WINDOWS)
    file(INSTALL "${GE_WGPU_SOURCE}/lib/wgpu_native.lib"
         DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
    file(INSTALL "${GE_WGPU_SOURCE}/lib/wgpu_native.lib"
         DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
else()
    file(INSTALL "${GE_WGPU_SOURCE}/lib/libwgpu_native.a"
         DESTINATION "${CURRENT_PACKAGES_DIR}/lib")
    file(INSTALL "${GE_WGPU_SOURCE}/lib/libwgpu_native.a"
         DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
endif()

file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage" [=[
wgpu-native provides the static library and standard webgpu.h header:
    find_path(WGPU_INCLUDE_DIR webgpu/webgpu.h)
    find_library(WGPU_NATIVE_LIB wgpu_native)
]=])

file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/copyright" [=[
wgpu-native is dual-licensed MIT OR Apache-2.0.
https://github.com/gfx-rs/wgpu-native/blob/trunk/LICENSE.MIT
https://github.com/gfx-rs/wgpu-native/blob/trunk/LICENSE.APACHE
]=])
