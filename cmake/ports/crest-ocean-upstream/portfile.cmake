set(VCPKG_POLICY_EMPTY_PACKAGE enabled)

set(GE_CREST_COMMIT "db0658ff0b2e93e4a9e28cc2867509658b0ecc00")

vcpkg_download_distfile(GE_CREST_LICENSE
    URLS "https://raw.githubusercontent.com/wave-harmonic/crest/${GE_CREST_COMMIT}/LICENSE"
    FILENAME "crest-${GE_CREST_COMMIT}-LICENSE"
    SHA512 d82742e253b5dbd42d0e2accd7f88fecd3d9ecaa96b10eaa5602d24f4d2f9f3d6af9755076dec47e7e720cd26f47408dfcc57fe8ee5b60ffd9d3a87b761658ae
)

vcpkg_install_copyright(FILE_LIST "${GE_CREST_LICENSE}")
