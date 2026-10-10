# Idempotent fetch helper invoked by the FetchBlendTestSamples target
# (Tests/CMakeLists.txt). Downloads the three public Blender asset bundles the
# Blend* test suites were authored against, verifies each archive's SHA256,
# and extracts them into the exact layout the suites resolve:
#
#   <BLEND_SAMPLES_ROOT>/
#     human-base/human-base-meshes-bundle-v1.4.1/human_base_meshes_bundle.blend
#     ellie/asset-demo-bundle-4.0-ellie-animation/ellie_animation/ellie_animation.blend
#     anim-fund-rigs/animation_fundamental_rigs_release_01/pendulum.blend (+7 more rigs)
#     LICENSES.md
#
# All three archives are byte-pinned (SHA256) to the exact bundle versions the
# test assertions encode (action counts, mesh counts, bone counts), so a
# silent upstream re-release fails loud on hash mismatch instead of producing
# confusing assertion failures.
#
# Expects via -D:
#   BLEND_SAMPLES_ROOT  source-tree fixture root (gitignored)
#   STAGED_ROOT         build-root mirror the tests actually read (StagedTestPaths)
#   DOWNLOAD_DIR        archive cache directory
#   LICENSES_FILE       tracked attribution file copied next to the fixtures
#
# A bundle is skipped when its marker .blend already exists; delete the
# bundle's subdirectory under BLEND_SAMPLES_ROOT to force a re-fetch.

foreach(_required BLEND_SAMPLES_ROOT STAGED_ROOT DOWNLOAD_DIR LICENSES_FILE)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "FetchBlendSamples.cmake: missing -D${_required}=...")
    endif()
endforeach()

function(fetch_blend_bundle NAME URL SHA256 ZIP_NAME SUBDIR MARKER)
    set(_marker "${BLEND_SAMPLES_ROOT}/${MARKER}")
    if(EXISTS "${_marker}")
        message(STATUS "blend-samples/${NAME}: already hydrated (${MARKER}); skipping.")
        return()
    endif()

    set(_zip "${DOWNLOAD_DIR}/${ZIP_NAME}")
    set(_have_zip FALSE)
    if(EXISTS "${_zip}")
        file(SHA256 "${_zip}" _current)
        if(_current STREQUAL SHA256)
            set(_have_zip TRUE)
            message(STATUS "blend-samples/${NAME}: cached archive hash matches; reusing ${ZIP_NAME}.")
        else()
            message(STATUS "blend-samples/${NAME}: cached archive hash mismatched; re-downloading.")
            file(REMOVE "${_zip}")
        endif()
    endif()

    if(NOT _have_zip)
        message(STATUS "blend-samples/${NAME}: downloading ${URL}")
        # Hash is verified explicitly below rather than via EXPECTED_HASH:
        # EXPECTED_HASH turns transport errors into a hard error inside
        # file(), which would skip the actionable guidance here.
        file(DOWNLOAD "${URL}" "${_zip}"
             SHOW_PROGRESS
             STATUS _status
             TLS_VERIFY ON)
        list(GET _status 0 _rc)
        if(NOT _rc EQUAL 0)
            list(GET _status 1 _err)
            file(REMOVE "${_zip}")
            message(FATAL_ERROR
                "blend-samples/${NAME}: download failed: ${_err}\n"
                "URL: ${URL}\n"
                "If the URL has rotted, re-locate the bundle on blender.org and "
                "update Tests/cmake/FetchBlendSamples.cmake (URL + SHA256 pair).")
        endif()
        file(SHA256 "${_zip}" _actual)
        if(NOT _actual STREQUAL SHA256)
            file(REMOVE "${_zip}")
            message(FATAL_ERROR
                "blend-samples/${NAME}: SHA256 mismatch for ${ZIP_NAME}\n"
                "  expected ${SHA256}\n"
                "  actual   ${_actual}\n"
                "Upstream re-released the bundle. The suites' assertions are "
                "pinned to the expected bytes — verify the new archive against "
                "the tests before updating the hash.")
        endif()
    endif()

    file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${BLEND_SAMPLES_ROOT}/${SUBDIR}")
    if(NOT EXISTS "${_marker}")
        message(FATAL_ERROR
            "blend-samples/${NAME}: archive extracted but expected marker is absent: ${_marker}\n"
            "The upstream archive layout changed; update SUBDIR/MARKER in "
            "Tests/cmake/FetchBlendSamples.cmake.")
    endif()
    message(STATUS "blend-samples/${NAME}: extracted into ${BLEND_SAMPLES_ROOT}/${SUBDIR}.")
endfunction()

file(MAKE_DIRECTORY "${DOWNLOAD_DIR}")
file(MAKE_DIRECTORY "${BLEND_SAMPLES_ROOT}")

# Human Base Meshes bundle v1.4.1 — Blender Studio, CC0.
# 227 mesh datablocks / no armature; the ~50 MB parse-budget fixture.
fetch_blend_bundle(human-base
    "https://download.blender.org/demo/asset-bundles/human-base-meshes/human-base-meshes-bundle-v1.4.1.zip"
    "811f43accbb31a88266d932f8f5563b2d13586fca0ba2693aad1f5fe582b3515"
    "human-base-meshes-bundle-v1.4.1.zip"
    "human-base"
    "human-base/human-base-meshes-bundle-v1.4.1/human_base_meshes_bundle.blend")

# Ellie Pose Library v2.0.0 — Blender Studio, CC-BY. The archive's inner
# folder is named asset-demo-bundle-4.0-ellie-animation (Blender 4.0 asset
# demo); BlenRig humanoid armature + 58 Actions.
fetch_blend_bundle(ellie
    "https://download.blender.org/demo/asset-bundles/ellie-pose-library/ellie-pose-library-v2.0.0.zip"
    "547a997488b91683df68eb2925423fcfbbb1b6e3e7bedd0d4cd4f67d23090231"
    "ellie-pose-library-v2.0.0.zip"
    "ellie"
    "ellie/asset-demo-bundle-4.0-ellie-animation/ellie_animation/ellie_animation.blend")

# Animation Fundamentals rigs release 01 — Blender Studio training rigs,
# CC-BY-SA 4.0 (LICENSE.txt ships inside the archive). Blender Studio serves
# files content-addressed; the stable /download-source/ URL 302s to a
# short-lived signed URL that file(DOWNLOAD) follows transparently.
fetch_blend_bundle(anim-fund-rigs
    "https://studio.blender.org/download-source/files/a1/a16ee9c081f64a058d948758d67904d0/a16ee9c081f64a058d948758d67904d0.zip"
    "f0f57b1292c147b76d45c179c01e4ee2397576348a194dd62c24c022dd00fbee"
    "animation_fundamental_rigs_release_01.zip"
    "anim-fund-rigs"
    "anim-fund-rigs/animation_fundamental_rigs_release_01/pendulum.blend")

# Attribution: required by the CC-BY / CC-BY-SA bundles, staged next to the
# fixtures so the obligation travels with the files.
configure_file("${LICENSES_FILE}" "${BLEND_SAMPLES_ROOT}/LICENSES.md" COPYONLY)

# Mirror into the build root the suites actually read (StagedTestPaths.h).
# StageTestAssets keeps this in sync on later builds (its blend-samples hook
# is gated on the source dir existing at configure time); this direct copy
# makes the fixtures live immediately, no reconfigure required.
file(COPY "${BLEND_SAMPLES_ROOT}/" DESTINATION "${STAGED_ROOT}")
message(STATUS "blend-samples: staged mirror refreshed at ${STAGED_ROOT}.")
