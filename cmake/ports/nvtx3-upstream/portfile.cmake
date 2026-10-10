# NVTX v3 is header-only: the headers dynamically resolve an injection library
# (Nsight Systems, nvtx-injection) at runtime, so there is nothing to build and
# nothing to link. Only the C headers are installed.
#
# There is no NVTX port in the upstream vcpkg registry, so this is a new port
# rather than an override of one.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO NVIDIA/NVTX
    REF v3.6.0
    SHA512 03892ea32a5867923301f871fecd5dae9e5cbbc506e28b2bf1a436ff5a15fb6c73f8639f9cf1bb4723dd6279f6fe8593da5a06630bae1eef290233bed2063e20
)

# Installs the nvtx3 directory itself (not its contents), so consumers include
# <nvtx3/nvToolsExt.h> and the nvtxDetail/ implementation headers resolve
# relative to it exactly as upstream expects.
file(INSTALL "${SOURCE_PATH}/c/include/nvtx3"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include")

file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/NVTX3_UPSTREAM.json" [=[
{
  "name": "NVIDIA Tools Extension (NVTX) v3",
  "repository": "https://github.com/NVIDIA/NVTX.git",
  "tag": "v3.6.0",
  "license": "Apache-2.0 WITH LLVM-exception",
  "files": [
    "c/include/nvtx3/",
    "LICENSE.txt"
  ]
}
]=])

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
