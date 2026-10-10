# Lays a fetched Sponza checkout down beside the test scene: the model's glTF
# files and the upstream Models/Sponza/LICENSE.md, both byte for byte. Run by the
# FetchSponzaTestScene target after its sparse checkout; a cone-mode checkout of
# Models/Sponza/glTF also checks out the files directly under Models/Sponza.
#
#   cmake -DCLONE_ROOT=<checkout root> -DDEST=<model directory> -P CopySponzaModel.cmake
#
# The license is checked before anything is copied, so the model never lands
# without it.

foreach(_required CLONE_ROOT DEST)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "CopySponzaModel.cmake: missing -D${_required}=...")
    endif()
endforeach()

set(_model "${CLONE_ROOT}/Models/Sponza")
if(NOT EXISTS "${_model}/LICENSE.md")
    message(FATAL_ERROR
        "CopySponzaModel.cmake: ${_model}/LICENSE.md is missing; the model is not copied without "
        "its license. Point SPONZA_KHRONOS_COMMIT in Tests/Scenes/Sponza/CMakeLists.txt at a "
        "glTF-Sample-Assets revision that has Models/Sponza/LICENSE.md, then rebuild FetchSponzaTestScene.")
endif()
if(NOT IS_DIRECTORY "${_model}/glTF")
    message(FATAL_ERROR
        "CopySponzaModel.cmake: ${_model}/glTF is missing; the checkout did not include the model. "
        "Delete ${DEST} and rebuild FetchSponzaTestScene.")
endif()

file(COPY "${_model}/glTF/" DESTINATION "${DEST}")
file(COPY "${_model}/LICENSE.md" DESTINATION "${DEST}")
