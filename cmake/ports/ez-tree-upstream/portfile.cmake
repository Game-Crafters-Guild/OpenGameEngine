set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO dgreenheck/ez-tree
    REF 28c16503da2a8a6f2ccb6c070f47ff8ca13ad4f6
    SHA512 f57d805914e07f07bce40e81332516af71d3c8b13117641da97858f11581ded9f18358c7bb15e7b69980574aa3d1dc100eb8f943b1741df91c533ac512b924f1
    HEAD_REF main
)

file(INSTALL "${SOURCE_PATH}/LICENSE"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
    RENAME copyright)

file(INSTALL "${SOURCE_PATH}/src/lib/presets/"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}/src/lib/presets"
    FILES_MATCHING PATTERN "*.json")

file(INSTALL "${SOURCE_PATH}/src/lib/assets/bark/"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}/src/lib/assets/bark"
    FILES_MATCHING
        PATTERN "*.jpg"
        PATTERN "README.md")

file(INSTALL "${SOURCE_PATH}/src/lib/assets/leaves/"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}/src/lib/assets/leaves"
    FILES_MATCHING PATTERN "*.png")

file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/EZTREE_UPSTREAM.json" [=[
{
  "name": "ez-tree",
  "package": "@dgreenheck/ez-tree",
  "packageVersion": "1.1.0",
  "repository": "https://github.com/dgreenheck/ez-tree.git",
  "upstreamCommit": "28c16503da2a8a6f2ccb6c070f47ff8ca13ad4f6",
  "importDate": "2026-06-19",
  "snapshotPolicy": "vcpkg overlay package: installs the MIT license, provenance metadata, JSON presets used by the native C++ port, and upstream texture assets used by GameEngine defaults.",
  "license": {
    "type": "MIT",
    "file": "copyright",
    "copyright": "Copyright (c) 2024 Daniel Greenheck"
  },
  "installedFiles": [
    "copyright",
    "EZTREE_UPSTREAM.json",
    "src/lib/presets/*.json",
    "src/lib/assets/bark/*.jpg",
    "src/lib/assets/bark/README.md",
    "src/lib/assets/leaves/*.png"
  ],
  "engineAssetCopies": [
    {
      "source": "src/lib/assets/bark",
      "destination": "Packages/eztree/Assets/Textures/EZTree/bark"
    },
    {
      "source": "src/lib/assets/leaves",
      "destination": "Packages/eztree/Assets/Textures/EZTree/leaves"
    }
  ],
  "nativePortTouchpoints": [
    "Packages/eztree/Native/EZTree",
    "Packages/eztree/Native/EZTreeECS",
    "Packages/eztree/Editor"
  ]
}
]=])
