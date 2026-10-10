set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

if(DEFINED ENV{GE_LINUX_CROSS_COMPILE_X64} AND "$ENV{GE_LINUX_CROSS_COMPILE_X64}")
    set(VCPKG_CMAKE_SYSTEM_PROCESSOR x86_64)
    set(VCPKG_CMAKE_C_COMPILER /usr/bin/x86_64-linux-gnu-gcc)
    set(VCPKG_CMAKE_CXX_COMPILER /usr/bin/x86_64-linux-gnu-g++)
endif()

# Steam Deck packages do not need OpenSSL's x64 assembly fast paths, and those
# paths are brittle under Mac-hosted Docker/cross-build setups.
set(GE_VCPKG_OPENSSL_NO_ASM ON)
