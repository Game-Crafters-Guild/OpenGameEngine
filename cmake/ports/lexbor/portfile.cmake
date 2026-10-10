# GameEngine overlay of vcpkg's lexbor port.
# Modifications vs upstream (both patch lxb_css_selectors_state_pseudo_class in
# source/lexbor/css/selectors/state.c; applied in listed order, after upstream's own
# fix-install-dirs.patch):
#   0001 — removes :focus-within / :focus-visible from lexbor's "not supported"
#          pseudo-class branch so those selectors survive parsing into the AST.
#   0002 — preserves ALL other unknown pseudo-classes (e.g. :loading, :my-state)
#          as LXB_CSS_SELECTOR_PSEUDO_CLASS__UNDEF and deletes the remaining
#          "not supported" branch (:visited, :valid, :scope, ...), so every
#          argument-less pseudo-class reaches the AST instead of failing (and
#          discarding) the whole rule.
# GameEngine matches selectors with its own cascade engine (Engine/Modules/UI), not
# lexbor's DOM matcher, so the upstream rejections only served to silently drop rules
# the engine can match. Functional pseudo-classes :foo(...) are still rejected.
#
# The source download (REF/SHA512) and fix-install-dirs.patch are identical to upstream;
# only the two GameEngine patches appended to PATCHES differ. When upstream vcpkg bumps the
# lexbor version, resync this portfile, vcpkg.json and upstream's own patches, and re-cut
# 0001/0002 against the new source (cut 0002 against the 0001-patched tree, since vcpkg
# applies patches in order).

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO lexbor/lexbor
    REF v${VERSION}
    SHA512 d65906504f490a03579eb737fa85007f1e948c4e82f986454d763728e4d71be40fe953a5a7cfda84b322cb99837f482295ee89bc3f07f2ef89ccaeeab39a4acf
    PATCHES
        fix-install-dirs.patch # https://github.com/lexbor/lexbor/pull/406
        0001-parse-focus-within-and-focus-visible.patch
        0002-preserve-unknown-pseudo-classes.patch
)

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        perf  LEXBOR_WITH_PERF
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static" BUILD_STATIC)
string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" BUILD_SHARED)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
    ${FEATURE_OPTIONS}
    -DLEXBOR_BUILD_SHARED=${BUILD_SHARED}
    -DLEXBOR_BUILD_STATIC=${BUILD_STATIC}
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/lexbor)
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/include/lexbor/html/tree/insertion_mode"
    "${CURRENT_PACKAGES_DIR}/debug/include/lexbor/html/tree/insertion_mode"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
