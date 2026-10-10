# Idempotent fetch helper invoked by FetchABeautifulGameTestScene.
# Expects AGAME_URL / AGAME_SHA256 / AGAME_DEST to be passed via -D.

if(EXISTS "${AGAME_DEST}")
    file(SHA256 "${AGAME_DEST}" CURRENT_HASH)
    if(CURRENT_HASH STREQUAL AGAME_SHA256)
        message(STATUS "ABeautifulGame.glb already present (hash matches); skipping download.")
        return()
    endif()
    message(STATUS "ABeautifulGame.glb present but hash mismatched; re-downloading.")
    file(REMOVE "${AGAME_DEST}")
endif()

get_filename_component(AGAME_DIR "${AGAME_DEST}" DIRECTORY)
file(MAKE_DIRECTORY "${AGAME_DIR}")

message(STATUS "Downloading ABeautifulGame.glb from Khronos...")
file(DOWNLOAD "${AGAME_URL}" "${AGAME_DEST}"
     EXPECTED_HASH SHA256=${AGAME_SHA256}
     SHOW_PROGRESS
     STATUS DOWNLOAD_STATUS
     TLS_VERIFY ON)

list(GET DOWNLOAD_STATUS 0 DOWNLOAD_RC)
if(NOT DOWNLOAD_RC EQUAL 0)
    list(GET DOWNLOAD_STATUS 1 DOWNLOAD_ERR)
    file(REMOVE "${AGAME_DEST}")
    message(FATAL_ERROR "ABeautifulGame.glb download failed: ${DOWNLOAD_ERR}")
endif()
message(STATUS "ABeautifulGame.glb fetched successfully.")
