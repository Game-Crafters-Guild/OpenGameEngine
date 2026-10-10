# Tests/Scenes/Sponza — Khronos Sponza stress / reference scene.
#
# Sponza ships as a multi-file glTF (gltf + bin + ~30 textures, ~25 MB). The
# Khronos repo doesn't expose a single .glb for it, so we fetch via a git
# sparse-checkout pinned to a specific commit. This keeps the download
# reproducible (no risk of Khronos rewriting history under us) and avoids
# wiring a 30-file URL manifest.
#
# Opt-in. To fetch + use:
#   cmake --build --preset <preset> --target FetchSponzaTestScene
#   Editor.exe --project <repo>/Tests/Scenes/Sponza
#
# Re-runs of FetchSponzaTestScene are idempotent — the target re-clones the
# shallow tree each time but the final copy step (CopySponzaModel.cmake) lays the
# same files down: the model's glTF files and, beside them, the upstream
# LICENSE.md. To force a re-fetch, delete Assets/Models/Sponza/.

set(SPONZA_KHRONOS_COMMIT "2bac6f8c57bf471df0d2a1e8a8ec023c7801dddf")
set(SPONZA_REPO_URL "https://github.com/KhronosGroup/glTF-Sample-Assets.git")
set(SPONZA_DEST "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Models/Sponza")
set(SPONZA_TMP_CLONE "${CMAKE_CURRENT_BINARY_DIR}/_sponza_fetch_clone")

foreach(_case copies-license refuses-without-license)
    add_test(
        NAME FetchSponzaTestScene.${_case}
        COMMAND ${CMAKE_COMMAND}
            -DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/CopySponzaModel.cmake
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/_copy_sponza_model_test/${_case}
            -DCASE=${_case}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/TestCopySponzaModel.cmake
    )
endforeach()

find_program(GIT_EXECUTABLE git)
if(NOT GIT_EXECUTABLE)
    message(STATUS "Tests/Scenes/Sponza: git not found in PATH; FetchSponzaTestScene unavailable.")
    return()
endif()

add_custom_target(FetchSponzaTestScene
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${SPONZA_TMP_CLONE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SPONZA_DEST}"
    COMMAND ${GIT_EXECUTABLE} clone
            --depth 1
            --filter=blob:none
            --no-checkout
            --quiet
            "${SPONZA_REPO_URL}"
            "${SPONZA_TMP_CLONE}"
    COMMAND ${GIT_EXECUTABLE} -C "${SPONZA_TMP_CLONE}"
            fetch --depth 1 --quiet origin ${SPONZA_KHRONOS_COMMIT}
    COMMAND ${GIT_EXECUTABLE} -C "${SPONZA_TMP_CLONE}"
            sparse-checkout init --cone
    COMMAND ${GIT_EXECUTABLE} -C "${SPONZA_TMP_CLONE}"
            sparse-checkout set "Models/Sponza/glTF"
    COMMAND ${GIT_EXECUTABLE} -C "${SPONZA_TMP_CLONE}"
            checkout --quiet ${SPONZA_KHRONOS_COMMIT}
    COMMAND ${CMAKE_COMMAND}
            "-DCLONE_ROOT=${SPONZA_TMP_CLONE}"
            "-DDEST=${SPONZA_DEST}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/CopySponzaModel.cmake"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${SPONZA_TMP_CLONE}"
    COMMENT "Fetching Khronos Sponza (~25MB) into ${SPONZA_DEST}"
    VERBATIM
)
set_target_properties(FetchSponzaTestScene PROPERTIES FOLDER "Tests/Scenes")
