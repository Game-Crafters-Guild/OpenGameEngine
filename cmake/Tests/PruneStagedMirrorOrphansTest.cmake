# Contract test for cmake/PruneStagedMirrorOrphans.cmake, run by ctest as
#   cmake -DSCRIPT=<prune script> -DWORK=<scratch dir> -P <this file>
# Stages scratch mirrors the way the build does (copy_directory_if_different,
# then the prune) and checks the ownership contract: a file deleted from SRC is
# pruned from DST and the directories that emptied are swept, while files the
# mirror never staged — a sibling target's content, runtime state written into
# the tree, a second mirror's files — survive every prune, whatever order the
# writers ran in. A case-only rename is a rename, not a deletion. With
# MANIFEST_DIR the records live outside the staged tree, one per (SRC, DST)
# pair, so mirrors sharing a source or a destination never share a record.
# An orphan that cannot be removed stays recorded and is removed by a later
# prune once it can be; an empty SRC or DST is refused before anything runs.
if(NOT DEFINED SCRIPT OR NOT DEFINED WORK)
    message(FATAL_ERROR "PruneStagedMirrorOrphansTest.cmake: SCRIPT and WORK are required")
endif()

set(_src "${WORK}/src")
set(_src2 "${WORK}/src2")
set(_dst "${WORK}/dst")

# stage_mirror(<src> [<dst> [<manifest dir>]])
function(stage_mirror src)
    set(_to "${_dst}")
    set(_prune_args "")
    if(ARGC GREATER 1)
        set(_to "${ARGV1}")
    endif()
    if(ARGC GREATER 2)
        set(_prune_args "-DMANIFEST_DIR=${ARGV2}")
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${src}" "${_to}"
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -DSRC=${src} -DDST=${_to} ${_prune_args} -P ${SCRIPT}
        COMMAND_ERROR_IS_FATAL ANY)
endfunction()

# expect_present(<root> <rel>...) / expect_absent(<root> <rel>...)
function(expect_present root)
    foreach(_rel IN LISTS ARGN)
        if(NOT EXISTS "${root}/${_rel}")
            message(FATAL_ERROR "expected ${_rel} to survive the prune under ${root}, it is gone")
        endif()
    endforeach()
endfunction()

function(expect_absent root)
    foreach(_rel IN LISTS ARGN)
        if(EXISTS "${root}/${_rel}")
            message(FATAL_ERROR "expected ${_rel} to be pruned under ${root}, it is still staged")
        endif()
    endforeach()
endfunction()

function(expect_manifest_count pattern expected)
    file(GLOB _manifests "${pattern}")
    list(LENGTH _manifests _count)
    if(NOT _count EQUAL expected)
        message(FATAL_ERROR "expected ${expected} manifest(s) matching ${pattern}, found ${_count}")
    endif()
endfunction()

file(REMOVE_RECURSE "${WORK}")

# An empty argument must be refused, not resolved against the working directory.
function(expect_refused)
    execute_process(
        COMMAND ${CMAKE_COMMAND} ${ARGN} -P ${SCRIPT}
        RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
    if(_rc EQUAL 0)
        message(FATAL_ERROR "expected the prune to refuse '${ARGN}', it ran")
    endif()
endfunction()
expect_refused(-DSRC=${_src} -DDST=)
expect_refused(-DSRC= -DDST=${_dst})
expect_refused(-DSRC=${_src} -DDST=${_dst} -DMANIFEST_DIR=)
expect_refused(-DDST=${_dst})

file(WRITE "${_src}/keep.txt" "keep")
file(WRITE "${_src}/Textures/gone.png" "gone")
file(WRITE "${_src}/UI/demo.css" "demo")
file(WRITE "${_src}/Case/Foo.png" "case")

# A sibling target staged its content into the composite DST before the mirror
# ran, and a test wrote runtime state into it. The mirror owns neither.
file(WRITE "${_dst}/Fonts/Roboto.ttf" "font")
file(WRITE "${_dst}/UI/theme/dark.css" "theme")
file(WRITE "${_dst}/Logs/run.log" "log")

# First staging: nothing is recorded yet, so nothing may be pruned.
stage_mirror("${_src}")
expect_present("${_dst}" keep.txt Textures/gone.png UI/demo.css Case/Foo.png
               Fonts/Roboto.ttf UI/theme/dark.css Logs/run.log)
expect_manifest_count("${_dst}.*.mirror-manifest" 1)

# Deleting from SRC prunes the staged copy and sweeps the directory it emptied;
# a directory the sibling still populates stays.
file(REMOVE "${_src}/Textures/gone.png" "${_src}/UI/demo.css")
stage_mirror("${_src}")
expect_absent("${_dst}" Textures/gone.png Textures UI/demo.css)
expect_present("${_dst}" keep.txt UI/theme/dark.css Fonts/Roboto.ttf Logs/run.log)

# A case-only rename lists under the new spelling. On a case-insensitive
# filesystem the old spelling still resolves in SRC (and in DST names the same
# file), so it must not be treated as an orphan; on a case-sensitive one it is
# a delete plus an add, and the new spelling is staged either way.
file(RENAME "${_src}/Case/Foo.png" "${_src}/Case/foo.png")
stage_mirror("${_src}")
expect_present("${_dst}" Case/foo.png)
file(REMOVE "${_src}/Case/foo.png")
stage_mirror("${_src}")
expect_absent("${_dst}" Case/foo.png Case)

# The record follows SRC: a file that comes back and leaves again is pruned again.
file(WRITE "${_src}/Textures/back.png" "back")
stage_mirror("${_src}")
expect_present("${_dst}" Textures/back.png)
file(REMOVE "${_src}/Textures/back.png")
stage_mirror("${_src}")
expect_absent("${_dst}" Textures/back.png Textures)
expect_present("${_dst}" keep.txt Fonts/Roboto.ttf UI/theme/dark.css Logs/run.log)

# A second mirror into the same DST keeps its own record: neither mirror's
# prune touches the other's files, in either order, and each still prunes its
# own deletions.
file(WRITE "${_src2}/Second/only.txt" "second")
stage_mirror("${_src2}")
stage_mirror("${_src}")
expect_present("${_dst}" keep.txt Second/only.txt Fonts/Roboto.ttf)
stage_mirror("${_src2}")
expect_present("${_dst}" keep.txt Second/only.txt)
file(REMOVE "${_src2}/Second/only.txt")
stage_mirror("${_src2}")
expect_absent("${_dst}" Second/only.txt Second)
expect_present("${_dst}" keep.txt Fonts/Roboto.ttf UI/theme/dark.css Logs/run.log)

# MANIFEST_DIR keeps the records out of the staged tree: nothing lands beside
# or inside DST, and the prune still follows SRC.
set(_manifests "${WORK}/manifests")
set(_shipped "${WORK}/shipped/Assets")
file(WRITE "${_src}/Icons/old.png" "old")
file(WRITE "${_src2}/Second/only.txt" "second")
stage_mirror("${_src}" "${_shipped}" "${_manifests}")
stage_mirror("${_src2}" "${_shipped}" "${_manifests}")
expect_present("${_shipped}" keep.txt Icons/old.png Second/only.txt)
expect_manifest_count("${_shipped}.*.mirror-manifest" 0)
expect_manifest_count("${_shipped}/*.mirror-manifest" 0)
expect_manifest_count("${_manifests}/Assets.*.mirror-manifest" 2)
file(REMOVE "${_src}/Icons/old.png")
stage_mirror("${_src}" "${_shipped}" "${_manifests}")
expect_absent("${_shipped}" Icons/old.png Icons)
expect_present("${_shipped}" keep.txt Second/only.txt)

# One source mirrored into two destinations with a shared MANIFEST_DIR: the
# records are per (SRC, DST) pair, so pruning the first destination does not
# consume the deletion before the second one has pruned its own copy.
set(_shipped2 "${WORK}/shipped/Tests/Assets")
file(WRITE "${_src}/Icons/twice.png" "twice")
stage_mirror("${_src}" "${_shipped}" "${_manifests}")
stage_mirror("${_src}" "${_shipped2}" "${_manifests}")
expect_present("${_shipped}" Icons/twice.png)
expect_present("${_shipped2}" Icons/twice.png)
expect_manifest_count("${_manifests}/Assets.*.mirror-manifest" 3)
file(REMOVE "${_src}/Icons/twice.png")
stage_mirror("${_src}" "${_shipped}" "${_manifests}")
stage_mirror("${_src}" "${_shipped2}" "${_manifests}")
expect_absent("${_shipped}" Icons/twice.png)
expect_absent("${_shipped2}" Icons/twice.png)

# An orphan the process cannot delete is kept in the record and removed by a
# later prune once it can be. The lock is a read-only parent directory, which
# only blocks deletion on a POSIX filesystem for a non-root user; when it does
# not hold here the case is skipped, stated, rather than passed vacuously.
if(NOT CMAKE_HOST_WIN32)
    file(WRITE "${_src}/Locked/held.txt" "held")
    stage_mirror("${_src}" "${_shipped}" "${_manifests}")
    expect_present("${_shipped}" Locked/held.txt)
    file(REMOVE "${_src}/Locked/held.txt")
    file(CHMOD "${_shipped}/Locked" PERMISSIONS OWNER_READ OWNER_EXECUTE)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -DSRC=${_src} -DDST=${_shipped} -DMANIFEST_DIR=${_manifests} -P ${SCRIPT}
        RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "a retained orphan must not fail the prune (rc ${_rc})")
    endif()
    if(EXISTS "${_shipped}/Locked/held.txt")
        file(GLOB _shipped_manifests "${_manifests}/Assets.*.mirror-manifest")
        set(_recorded FALSE)
        foreach(_m IN LISTS _shipped_manifests)
            file(STRINGS "${_m}" _lines)
            if("Locked/held.txt" IN_LIST _lines)
                set(_recorded TRUE)
            endif()
        endforeach()
        if(NOT _recorded)
            message(FATAL_ERROR "a retained orphan must stay recorded for the next prune")
        endif()
        file(CHMOD "${_shipped}/Locked" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
        stage_mirror("${_src}" "${_shipped}" "${_manifests}")
        expect_absent("${_shipped}" Locked/held.txt Locked)
    else()
        message(STATUS "read-only directory did not block deletion here (root?); retained-orphan case skipped")
        file(CHMOD "${_shipped}/Locked" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    endif()
endif()

file(REMOVE_RECURSE "${WORK}")
