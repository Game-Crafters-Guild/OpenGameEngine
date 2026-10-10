# Overlay the per-config PackagesPrebuilt intermediate onto a staged
# Packages tree, then prune staged fingerprint dirs the intermediate no
# longer produces. Platform dirs are named <platform>-<arch>-<fp8> by the
# GePrebuiltStamp tool; a staged fingerprint dir with no counterpart in the
# intermediate can never pass the loader's engine_abi gate against this tree's
# binaries, so copy_directory_if_different alone would let dead DLL+PDB sets
# accumulate in every consumer's staged tree across ABI changes.
#
# Only fingerprint-shaped directories are copied: every prebuilt module output
# lives in one. A file anywhere else in the intermediate is no module output,
# and copying it would overwrite the package's authored file at the same path
# (such as the vendored unity-import/Tools/UnityConverter.dll) in every build.
#
# When SRC does not exist (config never built prebuilts) this is a silent
# no-op, including the prune: without an intermediate there is no basis to
# call a staged dir stale.
# Args: -DSRC=<PackagesPrebuilt/<CONFIG>> -DDST=<exe dir>/Packages
if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "OverlayEnginePackagePrebuilts.cmake: SRC and DST are required")
endif()

if(NOT IS_DIRECTORY "${SRC}")
    return()
endif()

# Copy every fingerprint-shaped dir in SRC to the same place under DST, then
# prune the DST-side fingerprint dirs under its parent (a prebuilt root) that
# SRC does not produce. Only fingerprint-shaped names are candidates — authored
# package content never uses that shape.
file(GLOB_RECURSE _src_entries LIST_DIRECTORIES true "${SRC}/*")
foreach(_entry IN LISTS _src_entries)
    if(NOT IS_DIRECTORY "${_entry}")
        continue()
    endif()
    get_filename_component(_name "${_entry}" NAME)
    if(NOT _name MATCHES "^[A-Za-z0-9_]+-[A-Za-z0-9_]+-[0-9a-f]+$")
        continue()
    endif()
    get_filename_component(_src_root "${_entry}" DIRECTORY)
    file(RELATIVE_PATH _rel_root "${SRC}" "${_src_root}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${_entry}" "${DST}/${_rel_root}/${_name}"
        RESULT_VARIABLE _copy_result)
    if(NOT _copy_result EQUAL 0)
        message(FATAL_ERROR "OverlayEnginePackagePrebuilts.cmake: copy '${_entry}' -> '${DST}/${_rel_root}/${_name}' failed")
    endif()
    file(GLOB _dst_platform_dirs LIST_DIRECTORIES true "${DST}/${_rel_root}/*")
    foreach(_dst_dir IN LISTS _dst_platform_dirs)
        if(NOT IS_DIRECTORY "${_dst_dir}")
            continue()
        endif()
        get_filename_component(_dst_name "${_dst_dir}" NAME)
        if(_dst_name MATCHES "^[A-Za-z0-9_]+-[A-Za-z0-9_]+-[0-9a-f]+$"
           AND NOT IS_DIRECTORY "${_src_root}/${_dst_name}")
            file(REMOVE_RECURSE "${_dst_dir}")
        endif()
    endforeach()
endforeach()
