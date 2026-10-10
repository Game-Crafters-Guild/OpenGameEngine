set(GE_RENDERDOC_COMMIT "050034a0faa37d606ce1b8cf677dba4bc36984ea")

vcpkg_download_distfile(GE_RENDERDOC_LICENSE
    URLS "https://raw.githubusercontent.com/baldurk/renderdoc/${GE_RENDERDOC_COMMIT}/LICENSE.md"
    FILENAME "renderdoc-${GE_RENDERDOC_COMMIT}-LICENSE.md"
    SHA512 e12121d071bd4098fa577d9d097b9e37e82916f3856d4efdb0052def0c2911e1b1780176df9f12fea24138da10eb33f3994da1a88efacaab17737f538cbeef9e
)

vcpkg_download_distfile(GE_RENDERDOC_APP_HEADER
    URLS "https://raw.githubusercontent.com/baldurk/renderdoc/${GE_RENDERDOC_COMMIT}/renderdoc/api/app/renderdoc_app.h"
    FILENAME "renderdoc-${GE_RENDERDOC_COMMIT}-renderdoc_app.h"
    SHA512 61e9ef550875c9d8fdbadfd8220eb86c91ea4d2c0060fd4ceb89e045b3724e8d76513ea73c6a2ca3bcca2b0471c1a3b2d102e72be3ca5b906d8e49c79b53daac
)

file(INSTALL "${GE_RENDERDOC_APP_HEADER}"
    DESTINATION "${CURRENT_PACKAGES_DIR}/include"
    RENAME "renderdoc_app.h")

file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/RENDERDOC_APP_API_UPSTREAM.json" [=[
{
  "name": "RenderDoc in-application API",
  "repository": "https://github.com/baldurk/renderdoc.git",
  "tag": "v1.44",
  "commit": "050034a0faa37d606ce1b8cf677dba4bc36984ea",
  "license": "MIT",
  "files": [
    "renderdoc/api/app/renderdoc_app.h",
    "LICENSE.md"
  ]
}
]=])

vcpkg_install_copyright(FILE_LIST "${GE_RENDERDOC_LICENSE}")
