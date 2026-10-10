# Write engine_abi markers for staged prebuilt engine-package modules. Runs
# POST_BUILD on the host app AFTER its SDK and Packages staging and, on macOS,
# the signing of its SDK libraries: the digest hashes the STAGED SDK import
# libs' identity (path + size + mtime), which signing changes, so it must be
# computed against their final staged state — the exact inputs the
# NativeScriptManager loader recomputes at project open. A stale/mismatched
# marker makes the loader fall back to a source build loudly; it can never
# cause a wrong load.
#
# Args: -DSTAMP_TOOL= -DSDK_DIR= -DPACKAGES_DIR= -DMODULES_FILE=
# MODULES_FILE lines: <pkg-relative prebuilt subdir>|<editor|runtime>|<defines,comma-separated>
if(NOT DEFINED STAMP_TOOL OR NOT DEFINED SDK_DIR OR NOT DEFINED PACKAGES_DIR OR NOT DEFINED MODULES_FILE)
    message(FATAL_ERROR "StageEnginePackageMarkers.cmake: STAMP_TOOL, SDK_DIR, PACKAGES_DIR and MODULES_FILE are required")
endif()

set(_stamp_out "${PACKAGES_DIR}/.ge_prebuilt_stamp.txt")

execute_process(
    COMMAND "${STAMP_TOOL}" platformdir "${_stamp_out}"
    RESULT_VARIABLE _stamp_result)
if(NOT _stamp_result EQUAL 0)
    message(FATAL_ERROR "StageEnginePackageMarkers.cmake: GePrebuiltStamp platformdir failed")
endif()
file(STRINGS "${_stamp_out}" _platform_lines LIMIT_COUNT 1)
list(GET _platform_lines 0 _platform_dir)

file(STRINGS "${MODULES_FILE}" _modules)
foreach(_entry IN LISTS _modules)
    if(NOT _entry)
        continue()
    endif()
    string(REPLACE "|" ";" _parts "${_entry}")
    list(LENGTH _parts _parts_len)
    if(_parts_len LESS 2)
        message(FATAL_ERROR "StageEnginePackageMarkers.cmake: malformed module entry '${_entry}'")
    endif()
    list(GET _parts 0 _prebuilt_subdir)
    list(GET _parts 1 _kind)
    set(_defines "")
    if(_parts_len GREATER 2)
        list(GET _parts 2 _defines_joined)
        string(REPLACE "," ";" _defines "${_defines_joined}")
    endif()

    set(_platform_path "${PACKAGES_DIR}/${_prebuilt_subdir}/${_platform_dir}")
    if(NOT IS_DIRECTORY "${_platform_path}")
        # The module target didn't build (e.g. a consumer staged without the
        # prebuilt targets) — nothing to mark; the loader falls back loudly.
        continue()
    endif()

    execute_process(
        COMMAND "${STAMP_TOOL}" abidigest "${SDK_DIR}" "${_kind}" "${_stamp_out}" ${_defines}
        RESULT_VARIABLE _digest_result)
    if(NOT _digest_result EQUAL 0)
        message(FATAL_ERROR "StageEnginePackageMarkers.cmake: abidigest failed for '${_entry}'")
    endif()
    file(STRINGS "${_stamp_out}" _digest_lines LIMIT_COUNT 1)
    list(GET _digest_lines 0 _digest)
    file(WRITE "${_platform_path}/engine_abi.txt" "${_digest}\n")
endforeach()

file(REMOVE "${_stamp_out}")
