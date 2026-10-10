# Idempotent fetch helper invoked by the FetchFbxTestFixtures target
# (Tests/CMakeLists.txt). Downloads the store-licensed FBX test-file archive
# and extracts it into Assets/Models/FBXTest/ — the layout the
# humanoid-retarget suites resolve via the StageTestAssets sweep:
#
#   Assets/Models/FBXTest/
#     BusinessMale.fbx            target humanoid (store humanoid-standard rig)
#     A_Walk_F_Masc.fbx           source walk clip
#     A_Idle_Standing_Masc.fbx    source idle clip
#     Knight_skinned.fbx          non-standard-rig positive case
#     Barrel_static.fbx           no-skeleton negative case
#
# These are store-licensed test files. Their license permits use by the
# license holder but NOT redistribution, so unlike the Blender bundles (see
# FetchBlendSamples.cmake) there is NO hosted default: they stay local per
# team decision (2026-07-19). License holders point GE_FBXTEST_URL at their
# own copy of the archive (any file:// or self-controlled URL), or simply
# place the five .fbx files in Assets/Models/FBXTest/. Never re-host publicly.
#
# Expects via -D:
#   FBXTEST_ROOT    source-tree fixture dir (Assets/Models/FBXTest)
#   STAGED_ROOT     build-root mirror the tests actually read (StagedTestPaths)
#   DOWNLOAD_DIR    archive cache directory
# Required for any download (no hosted default exists):
#   FBXTEST_URL     archive URL (file:///... or a URL the license holder controls)
#   FBXTEST_SHA256  archive SHA256 (defaults to the pinned team archive)
#
# Skipped when BusinessMale.fbx already exists; delete the directory's .fbx
# files to force a re-fetch.

foreach(_required FBXTEST_ROOT STAGED_ROOT DOWNLOAD_DIR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "FetchFbxTestFixtures.cmake: missing -D${_required}=...")
    endif()
endforeach()

# The pinned SHA256 identifies the team's fbxtest-fixtures-v1.zip (the five
# .fbx files at the archive root). There is deliberately NO default URL —
# the test files' store license keeps the archive off shared hosting.
if(NOT DEFINED FBXTEST_SHA256)
    set(FBXTEST_SHA256 "e4dd639f91b60585e37dfa3dde616cc50a7469bb4851a819bb5868a3cdb5f1ec")
endif()

set(_marker "${FBXTEST_ROOT}/BusinessMale.fbx")
if(EXISTS "${_marker}")
    message(STATUS "FBXTest fixtures: already hydrated (BusinessMale.fbx present); skipping download.")
elseif(NOT DEFINED FBXTEST_URL)
    message(FATAL_ERROR
        "FBXTest fixtures: no archive URL configured.\n"
        "These are store-licensed test files (license-holder use only, never "
        "re-hosted publicly), so they stay local. Either place the five .fbx files in\n"
        "  ${FBXTEST_ROOT}\n"
        "or configure with your own copy of the team archive:\n"
        "  -DGE_FBXTEST_URL=file:///C:/path/to/fbxtest-fixtures-v1.zip\n"
        "and rebuild the FetchFbxTestFixtures target. Without the fixtures the "
        "humanoid-retarget suites skip gracefully.")
else()
    set(_zip "${DOWNLOAD_DIR}/fbxtest-fixtures-v1.zip")
    set(_have_zip FALSE)
    if(EXISTS "${_zip}")
        file(SHA256 "${_zip}" _current)
        if(_current STREQUAL FBXTEST_SHA256)
            set(_have_zip TRUE)
            message(STATUS "FBXTest fixtures: cached archive hash matches; reusing.")
        else()
            message(STATUS "FBXTest fixtures: cached archive hash mismatched; re-downloading.")
            file(REMOVE "${_zip}")
        endif()
    endif()

    if(NOT _have_zip)
        message(STATUS "FBXTest fixtures: downloading ${FBXTEST_URL}")
        # Hash is verified explicitly below rather than via EXPECTED_HASH:
        # EXPECTED_HASH turns transport errors into a hard error inside
        # file(), which would skip the actionable guidance here.
        file(DOWNLOAD "${FBXTEST_URL}" "${_zip}"
             SHOW_PROGRESS
             STATUS _status
             TLS_VERIFY ON)
        list(GET _status 0 _rc)
        if(NOT _rc EQUAL 0)
            list(GET _status 1 _err)
            file(REMOVE "${_zip}")
            message(FATAL_ERROR
                "FBXTest fixtures: download failed: ${_err}\n"
                "URL: ${FBXTEST_URL}\n"
                "Check the GE_FBXTEST_URL you configured (a file:///... path to "
                "your local copy of fbxtest-fixtures-v1.zip is the usual form).")
        endif()
        file(SHA256 "${_zip}" _actual)
        if(NOT _actual STREQUAL FBXTEST_SHA256)
            file(REMOVE "${_zip}")
            message(FATAL_ERROR
                "FBXTest fixtures: SHA256 mismatch for fbxtest-fixtures-v1.zip\n"
                "  expected ${FBXTEST_SHA256}\n"
                "  actual   ${_actual}\n"
                "The archive at GE_FBXTEST_URL does not match the pinned team "
                "archive. Point at the original fbxtest-fixtures-v1.zip, or "
                "update the pinned hash if the archive was intentionally "
                "regenerated.")
        endif()
    endif()

    file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${FBXTEST_ROOT}")
    if(NOT EXISTS "${_marker}")
        message(FATAL_ERROR
            "FBXTest fixtures: archive extracted but BusinessMale.fbx is absent. "
            "The archive layout must place the .fbx files at its root.")
    endif()
    message(STATUS "FBXTest fixtures: extracted into ${FBXTEST_ROOT}.")
endif()

# Mirror into the build root the suites actually read. The Assets/ sweep in
# StageTestAssets refreshes this on later builds; the direct copy makes the
# fixtures live immediately, no rebuild of StageTestAssets required.
file(COPY "${FBXTEST_ROOT}/" DESTINATION "${STAGED_ROOT}")
message(STATUS "FBXTest fixtures: staged mirror refreshed at ${STAGED_ROOT}.")
