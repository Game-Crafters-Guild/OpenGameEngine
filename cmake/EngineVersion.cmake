# The engine's one version source: the repository root's VERSION file, one line
# `<year>.<month>.<patch>` with an optional prerelease suffix `-alpha.<n>` or
# `-beta.<n>` and no leading `v` (a release tag is `v` plus this string). The
# root CMakeLists reads it before project(); the web package build
# (Tools/Web/build_web_package.py) and Managed/Directory.Build.props read the
# same file.

# ge_read_engine_version(<file> <out full> <out numeric>)
# Sets <out full> to the file's version string and <out numeric> to its
# `<year>.<month>.<patch>` part (what project() and the macOS bundle keys take).
# Refuses a file that does not hold exactly one such version.
function(ge_read_engine_version file outFull outNumeric)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "The engine version file ${file} is missing. It holds one line such as "
                            "2026.10.0-alpha.4.")
    endif()
    file(READ "${file}" _raw)
    string(STRIP "${_raw}" _version)
    if(NOT _version MATCHES "^([0-9][0-9][0-9][0-9]\\.(1[0-2]|[1-9])\\.(0|[1-9][0-9]*))(-(alpha|beta)\\.(0|[1-9][0-9]*))?$")
        message(FATAL_ERROR "${file} holds '${_version}', which is not "
                            "<year>.<month>.<patch>[-alpha.<n>|-beta.<n>] with no leading zeros "
                            "(for example 2026.10.0-alpha.4).")
    endif()
    set(${outFull} "${_version}" PARENT_SCOPE)
    set(${outNumeric} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()
