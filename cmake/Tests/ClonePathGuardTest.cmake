# Contract test for cmake/ClonePathGuard.cmake, run by ctest as
#   cmake -DSCRIPT=<guard script> -DWORK=<scratch dir> -P <this file>
# A refusal is a message(FATAL_ERROR), so every case runs in a child process
# this driver outlives to check the text.
#
# Covered: a configure from a source directory whose path contains a space is
# refused before project() with the fix in the message, and the same project
# from a path without one configures. On Windows, paths under Program Files and
# Program Files (x86) in any letter case are refused, and a path with "program"
# in another word is not.
if(NOT DEFINED SCRIPT OR NOT DEFINED WORK)
    message(FATAL_ERROR "ClonePathGuardTest.cmake: SCRIPT and WORK are required")
endif()

set(_fix "Clone the repository to a short path with no spaces outside Program Files")

file(REMOVE_RECURSE "${WORK}")

# configure_from(<source dir> <out rc> <out normalized output>)
function(configure_from sourceDir rcVar textVar)
    file(MAKE_DIRECTORY "${sourceDir}")
    file(WRITE "${sourceDir}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.25)\n"
        "include(\"${SCRIPT}\")\n"
        "ge_check_clone_path(\"\${CMAKE_SOURCE_DIR}\")\n"
        "project(ClonePathGuardProbe LANGUAGES NONE)\n")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -S "${sourceDir}" -B "${sourceDir}/build"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    # CMake indents and breaks the lines of a message(); collapse all
    # whitespace so an assertion on the text cannot fail on formatting.
    string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out}${_err}")
    set(${rcVar} "${_rc}" PARENT_SCOPE)
    set(${textVar} "${_text}" PARENT_SCOPE)
endfunction()

# check_path(<path> <out rc> <out normalized output>)
function(check_path path rcVar textVar)
    execute_process(
        COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${path}" -P "${SCRIPT}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out}${_err}")
    set(${rcVar} "${_rc}" PARENT_SCOPE)
    set(${textVar} "${_text}" PARENT_SCOPE)
endfunction()

# expect_refused(<case> <rc> <text> <reason>)
function(expect_refused case rc text reason)
    if(rc EQUAL 0)
        message(FATAL_ERROR "${case}: the guard accepted the path: ${text}")
    endif()
    foreach(_needle IN ITEMS "${reason}" "${_fix}")
        string(FIND "${text}" "${_needle}" _at)
        if(_at LESS 0)
            message(FATAL_ERROR "${case}: the refusal lacks \"${_needle}\": ${text}")
        endif()
    endforeach()
endfunction()

function(expect_accepted case rc text)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${case}: the guard refused the path (exit ${rc}): ${text}")
    endif()
endfunction()

configure_from("${WORK}/with space/GameEngine" _rc _text)
expect_refused("space" "${_rc}" "${_text}" "contains a space")

configure_from("${WORK}/no_space/GameEngine" _rc _text)
expect_accepted("no space" "${_rc}" "${_text}")

if(CMAKE_HOST_WIN32)
    foreach(_path IN ITEMS
            "C:/Program Files/Microsoft Visual Studio/18/Community/GameEngine"
            [[c:\PROGRAM FILES (X86)\GameEngine]])
        check_path("${_path}" _rc _text)
        expect_refused("${_path}" "${_rc}" "${_text}" "lies under Program Files")
    endforeach()

    check_path("C:/Dev/Programs/GameEngine" _rc _text)
    expect_accepted("program in another word" "${_rc}" "${_text}")
endif()
