vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO thorvg/thorvg
    REF "v${VERSION}"
    SHA512 0b4489bf589196b23f04396929ffe0523a013f42a799c8c2018ea9784bda4044d6b843b8bb4c677f997e99b52f44e1fb2847672ec6659edb9ae89132396f57b2
    HEAD_REF master
)

# ThorVG replaces the global operator new/delete with malloc/free forwarders, and as a
# static library those replacements become the allocation functions of every image that
# links it (Engine). The engine owns that choice: drop them, and the image's own
# operator new (the default library's, also malloc/free, or a replacement the engine
# defines) serves ThorVG instead. Behaviour is unchanged except on allocation failure:
# ThorVG's operator new returned null, the standard one throws std::bad_alloc.
#
# vcpkg_replace_string only warns when nothing matches, so the block is checked first: a
# ThorVG update that changes it fails the port build instead of silently keeping the
# four definitions in Engine.
set(THORVG_INITIALIZER "${SOURCE_PATH}/src/renderer/tvgInitializer.cpp")
set(THORVG_GLOBAL_ALLOCATION_FUNCTIONS [=[
void* operator new(std::size_t size)
{
    return tvg::malloc(size);
}


void operator delete(void* ptr) noexcept
{
    tvg::free(ptr);
}


void* operator new[](std::size_t size)
{
    return tvg::malloc(size);
}


void operator delete[](void* ptr) noexcept
{
    tvg::free(ptr);
}]=])
file(READ "${THORVG_INITIALIZER}" THORVG_INITIALIZER_SOURCE)
string(FIND "${THORVG_INITIALIZER_SOURCE}" "${THORVG_GLOBAL_ALLOCATION_FUNCTIONS}" THORVG_BLOCK_OFFSET)
if(THORVG_BLOCK_OFFSET EQUAL -1)
    message(FATAL_ERROR
        "ThorVG ${VERSION}: ${THORVG_INITIALIZER} no longer holds the global operator new/delete "
        "block this overlay removes. Find the global allocation functions in the new source, "
        "update the block in cmake/ports/thorvg/portfile.cmake to match them (or delete the "
        "overlay if ThorVG no longer defines them), and bump its port-version.")
endif()
vcpkg_replace_string("${THORVG_INITIALIZER}" "${THORVG_GLOBAL_ALLOCATION_FUNCTIONS}" "")

if ("tools" IN_LIST FEATURES)
    list(APPEND BUILD_OPTIONS -Dtools=all)
endif()

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${BUILD_OPTIONS}
        # see ${SOURCE_PATH}/meson_options.txt
        -Dstatic=true # Use static modules
        -Dengines=['cpu']
        -Dloaders=all
        -Dsavers=all
        -Dsimd=true
        -Dbindings=capi
        -Dtests=false
        -Dstrip=false
        -Dextra=['']
    OPTIONS_DEBUG
        -Dlog=true
        -Dbindir=${CURRENT_PACKAGES_DIR}/debug/bin
    OPTIONS_RELEASE
        -Dbindir=${CURRENT_PACKAGES_DIR}/bin
)
vcpkg_install_meson()
vcpkg_fixup_pkgconfig()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/thorvg-1/thorvg.h" "#ifndef TVG_STATIC" "#if 0")
else()
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/thorvg-1/thorvg.h" "#ifndef TVG_STATIC" "#if 1")
endif()

if ("tools" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES tvg-svg2png tvg-lottie2gif AUTO_CLEAN)
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
