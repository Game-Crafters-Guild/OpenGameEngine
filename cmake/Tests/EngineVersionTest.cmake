# Contract test for cmake/EngineVersion.cmake, run by ctest as
#   cmake -DSCRIPT=<reader> -DVERSION_FILE=<root VERSION> -DCONFIGURED=<GE_ENGINE_VERSION>
#         -DCONFIGURED_NUMERIC=<GE_ENGINE_VERSION_NUMERIC> -DHEADER=<generated EngineVersion.h>
#         -DWORK=<scratch dir> -P <this file>
#
# Covered: the configured GE_ENGINE_VERSION and its numeric part are what the root VERSION file
# holds, and the generated C++ header carries the same string; a release version's numeric
# part is the whole string; a malformed or missing file is refused, naming the file. A refusal
# is a message(FATAL_ERROR), so each refusal case runs the reader in a child process.
foreach(_required SCRIPT VERSION_FILE CONFIGURED CONFIGURED_NUMERIC HEADER WORK)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "EngineVersionTest.cmake: ${_required} is required")
    endif()
endforeach()

include("${SCRIPT}")

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

ge_read_engine_version("${VERSION_FILE}" _full _numeric)
if(NOT _full STREQUAL CONFIGURED)
    message(FATAL_ERROR "GE_ENGINE_VERSION is '${CONFIGURED}' but ${VERSION_FILE} holds '${_full}'")
endif()
if(NOT _numeric STREQUAL CONFIGURED_NUMERIC)
    message(FATAL_ERROR "GE_ENGINE_VERSION_NUMERIC is '${CONFIGURED_NUMERIC}', expected '${_numeric}'")
endif()
file(READ "${HEADER}" _header)
string(FIND "${_header}" "kEngineVersion = \"${_full}\";" _at)
if(_at EQUAL -1)
    message(FATAL_ERROR "${HEADER} does not carry kEngineVersion \"${_full}\":\n${_header}")
endif()

file(WRITE "${WORK}/release/VERSION" "2027.1.3\n")
ge_read_engine_version("${WORK}/release/VERSION" _full _numeric)
if(NOT _full STREQUAL "2027.1.3" OR NOT _numeric STREQUAL "2027.1.3")
    message(FATAL_ERROR "a release version read as '${_full}' / '${_numeric}'")
endif()

set(_driver "${WORK}/drive.cmake")
file(WRITE "${_driver}" "include(\"${SCRIPT}\")\nge_read_engine_version(\"\${FILE}\" _full _numeric)\n")
function(expect_refused name text)
    set(_file "${WORK}/${name}/VERSION")
    if(NOT text STREQUAL "<missing>")
        file(WRITE "${_file}" "${text}\n")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DFILE=${_file}" -P "${_driver}"
                    RESULT_VARIABLE _result ERROR_VARIABLE _error OUTPUT_QUIET)
    if(_result EQUAL 0)
        message(FATAL_ERROR "'${text}' (${name}) was accepted")
    endif()
    string(FIND "${_error}" "${_file}" _named)
    if(_named EQUAL -1)
        message(FATAL_ERROR "the refusal of '${text}' (${name}) does not name the file:\n${_error}")
    endif()
endfunction()

expect_refused(one-oh "1.0")
expect_refused(leading-v "v2026.10.0")
expect_refused(release-candidate "2026.10.0-rc.1")
expect_refused(leading-zero-month "2026.01.0")
expect_refused(two-lines "2026.10.0\n2026.11.0")
expect_refused(missing "<missing>")
