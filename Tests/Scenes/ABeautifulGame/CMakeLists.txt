# Tests/Scenes/ABeautifulGame — Khronos ABeautifulGame stress / reference scene.
#
# Single-file glTF-Binary (~43 MB). Fetched via CMake file(DOWNLOAD) with
# SHA256 verification against a Khronos-pinned commit URL. Re-runs of the
# fetch target are idempotent — the helper script skips the download when the
# destination file exists and its hash matches.
#
# Opt-in. To fetch + use:
#   cmake --build --preset <preset> --target FetchABeautifulGameTestScene
#   Editor.exe --project <repo>/Tests/Scenes/ABeautifulGame

set(AGAME_KHRONOS_COMMIT "2bac6f8c57bf471df0d2a1e8a8ec023c7801dddf")
set(AGAME_URL "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/${AGAME_KHRONOS_COMMIT}/Models/ABeautifulGame/glTF-Binary/ABeautifulGame.glb")
set(AGAME_SHA256 "bd7133b4b322aae97c589b8839dae8155ad2546acb35ae32a127e722a959d007")
set(AGAME_DEST "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Models/ABeautifulGame.glb")

add_custom_target(FetchABeautifulGameTestScene
    COMMAND ${CMAKE_COMMAND}
            -DAGAME_URL=${AGAME_URL}
            -DAGAME_SHA256=${AGAME_SHA256}
            -DAGAME_DEST=${AGAME_DEST}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/FetchABeautifulGame.cmake"
    COMMENT "Fetching Khronos ABeautifulGame (~43MB) into ${AGAME_DEST}"
    VERBATIM
)
set_target_properties(FetchABeautifulGameTestScene PROPERTIES FOLDER "Tests/Scenes")
