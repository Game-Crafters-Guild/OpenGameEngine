# GameEngine overlay of vcpkg's yoga port.
# Modifications vs upstream (applied in listed order):
#   disable_tests — upstream vcpkg's own patch, unchanged.
#   0001 — replaces Yoga's two-pass flexible-length resolver with the CSS
#          Flexbox 9.7 freeze loop, so an item clamped by min-width/min-height
#          stops at its clamp instead of flexing on top of it, and a line
#          shrinking around a max-clamped item stops overflowing its container.
#          The patch header carries the failing numbers, the Chrome arm they
#          were measured against, and the one related defect it does NOT fix.
#   0002 — keeps RTTI enabled on non-MSVC. Yoga's -fno-rtti emits private
#          copies of std typeinfo; linked into libEngine.dylib they break
#          libc++abi's pointer-identity catch matching, killing every
#          catch(const std::exception&) in the dylib (first throw =
#          std::terminate). MSVC /GR- stays: it matches by type name.
#   0003 — exports <prefix>/include as the public include root instead of
#          <prefix>/include/yoga. The latter makes yoga's own subdirectory
#          names (event/, config/, node/, style/, enums/, algorithm/, debug/,
#          numeric/) top-level include prefixes for every consumer, so on a
#          case-insensitive filesystem an engine module's "Event/Event.h"
#          resolves to yoga/event/event.h. Consumers include yoga as
#          <yoga/...>, which the remaining root serves.
#
# The source download (REF/SHA512) is identical to upstream; only PATCHES
# differs. When upstream vcpkg bumps the yoga version, resync this portfile and
# vcpkg.json and re-cut 0001 against the new source (cut it against the
# disable_tests-patched tree, since vcpkg applies patches in order).

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO facebook/yoga
    REF "v${VERSION}"
    SHA512 41ca044dcc7e404d5d3b052a85a650713bd31950a010a14658e25b1d065fffa16239cb93d2b00845d4e8443169ae50a91ad36080305f1be93e53ed481603a78b
    HEAD_REF master
    PATCHES
        disable_tests.patch
        0001-css-flexbox-9.7-resolve-flexible-lengths.patch
        0002-keep-std-rtti-identity.patch
        0003-scope-public-include-root.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/${PORT})

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
