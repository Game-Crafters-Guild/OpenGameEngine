# Prune orphans from a staged mirror of a source directory. A mirror is a
# copy_directory_if_different of SRC into DST; that copy never deletes, so a
# file deleted from SRC lives on in DST and suites keep passing against assets
# a fresh tree does not have (the dead-IBL class: >250 MB of deleted assets
# living on in staged trees).
#
# The mirror owns exactly what it staged. DST is often a composite: the
# exe-anchored Assets/ next to a test executable is shared by every test in
# bin/<Config>/Tests, the Editor's asset root merges a dozen source trees with
# build products, and other targets stage their own files into both. Deleting
# whatever DST has that SRC lacks would delete those files whenever this mirror
# runs after them, so the prune instead records SRC's file list at each staging
# in a manifest and removes only files that were in the previous record and are
# no longer in SRC. Files it never staged — sibling content, build products,
# runtime state written into the tree — are never candidates, and several
# mirrors into one DST each prune only their own record.
#
# The manifest names the (SRC, DST) pair, so mirrors that share a DST, and
# mirrors that share a manifest directory, never share a record. It lives
# beside DST as <DST>.<key>.mirror-manifest, or — when DST is what ships or is
# mirrored onward — under MANIFEST_DIR as <DST name>.<key>.mirror-manifest.
# Args: -DSRC=<dir> -DDST=<dir> [-DMANIFEST_DIR=<dir>]
if(NOT SRC OR NOT DST)
    message(FATAL_ERROR "PruneStagedMirrorOrphans.cmake: SRC and DST are required and must not be empty")
endif()
if(DEFINED MANIFEST_DIR AND NOT MANIFEST_DIR)
    message(FATAL_ERROR "PruneStagedMirrorOrphans.cmake: MANIFEST_DIR must not be empty when given")
endif()

get_filename_component(SRC "${SRC}" ABSOLUTE)
get_filename_component(DST "${DST}" ABSOLUTE)
string(SHA1 _key "${SRC}\n${DST}")
string(SUBSTRING "${_key}" 0 8 _key)
if(DEFINED MANIFEST_DIR)
    get_filename_component(MANIFEST_DIR "${MANIFEST_DIR}" ABSOLUTE)
    get_filename_component(_dst_name "${DST}" NAME)
    set(_manifest "${MANIFEST_DIR}/${_dst_name}.${_key}.mirror-manifest")
else()
    set(_manifest "${DST}.${_key}.mirror-manifest")
endif()

set(_current "")
if(IS_DIRECTORY "${SRC}")
    file(GLOB_RECURSE _current LIST_DIRECTORIES false RELATIVE "${SRC}" "${SRC}/*")
    list(SORT _current)
endif()

set(_previous "")
if(EXISTS "${_manifest}")
    file(STRINGS "${_manifest}" _previous ENCODING UTF-8)
endif()

set(_orphans ${_previous})
if(_orphans AND _current)
    list(REMOVE_ITEM _orphans ${_current})
endif()

# Orphans that could not be removed stay recorded so the next staging retries
# them; forgetting one would leave it staged for good.
set(_retained "")
foreach(_rel IN LISTS _orphans)
    # A case-only rename on a case-insensitive filesystem lists under the new
    # spelling while the old one still resolves; that is a rename, not an orphan.
    if(EXISTS "${SRC}/${_rel}")
        continue()
    endif()
    file(REMOVE "${DST}/${_rel}")
    # file(REMOVE) fails silently on a file the process may not delete (a handle
    # the running editor holds, a read-only directory).
    if(EXISTS "${DST}/${_rel}")
        message(WARNING "PruneStagedMirrorOrphans: could not remove ${DST}/${_rel}; retrying at the next staging")
        list(APPEND _retained "${_rel}")
        continue()
    endif()
    # Sweep the directories this removal emptied, innermost first, stopping at
    # the first one that still has content and never above DST itself.
    get_filename_component(_dir "${DST}/${_rel}" DIRECTORY)
    while(NOT _dir STREQUAL DST AND IS_DIRECTORY "${_dir}")
        file(GLOB _remaining "${_dir}/*")
        if(_remaining)
            break()
        endif()
        file(REMOVE_RECURSE "${_dir}")
        get_filename_component(_dir "${_dir}" DIRECTORY)
    endwhile()
endforeach()

set(_record ${_current} ${_retained})
list(SORT _record)
if(NOT "${_previous}" STREQUAL "${_record}")
    string(JOIN "\n" _content ${_record})
    file(WRITE "${_manifest}" "${_content}\n")
endif()
