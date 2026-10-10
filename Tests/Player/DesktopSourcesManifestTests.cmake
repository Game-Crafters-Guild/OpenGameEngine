cmake_minimum_required(VERSION 3.25)

include("${GE_SOURCE_ROOT}/cmake/DesktopPlayerSources.cmake")
if(DEFINED GE_CASE_ROOT)
    ge_read_desktop_player_sources("${GE_CASE_ROOT}" sources)
    return()
endif()

# Validate the real desktop list used by both the repo target and SDK staging.
ge_read_desktop_player_sources("${GE_SOURCE_ROOT}/Apps/Player" actual_sources)
if(NOT "main.cpp" IN_LIST actual_sources OR "WebMain.cpp" IN_LIST actual_sources)
    message(FATAL_ERROR "Desktop entry point ownership is incorrect")
endif()

file(MAKE_DIRECTORY "${GE_TEST_ROOT}/Source/Companions")
file(WRITE "${GE_TEST_ROOT}/Source/main.cpp" "int main() { return 0; }\n")
file(WRITE "${GE_TEST_ROOT}/Source/Companions/Options.h" "// companion\n")
file(WRITE "${GE_TEST_ROOT}/DesktopSources.txt"
    " \n # comment\n main.cpp \nmain.cpp\nCompanions/Options.h\n")
ge_read_desktop_player_sources("${GE_TEST_ROOT}" sources)
if(NOT sources STREQUAL "main.cpp;Companions/Options.h")
    message(FATAL_ERROR "Manifest ordering, trimming or deduplication changed: ${sources}")
endif()

# Each case names the guard that must fire, written "<message>|<entry>". A
# nonzero exit alone proves nothing here: the existence check also rejects most
# traversal spellings, so a run with no path-safety guard at all would still
# refuse them and the case would read green.
foreach(case IN ITEMS "Invalid desktop Player source|../escape.cpp"
                      "Invalid desktop Player source|/escape.cpp"
                      "Invalid desktop Player source|C:/escape.cpp"
                      "Invalid desktop Player source|folder/../escape.cpp"
                      "Invalid desktop Player source|folder\\escape.cpp"
                      "Invalid desktop Player source|./main.cpp"
                      "Invalid desktop Player source|main.cpp\;other.cpp"
                      "template source not found|Missing.h"
                      "Empty desktop Player source list|# empty")
    string(REGEX REPLACE "\\|.*$" "" reason "${case}")
    string(REGEX REPLACE "^[^|]*\\|" "" bad "${case}")
    file(WRITE "${GE_TEST_ROOT}/DesktopSources.txt" "${bad}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DGE_SOURCE_ROOT=${GE_SOURCE_ROOT}" "-DGE_CASE_ROOT=${GE_TEST_ROOT}"
        -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(status EQUAL 0)
        message(FATAL_ERROR "Accepted invalid manifest: ${bad}")
    endif()
    if(NOT error MATCHES "${reason}")
        message(FATAL_ERROR "Refused '${bad}' for the wrong reason: ${error}")
    endif()
endforeach()

file(REMOVE "${GE_TEST_ROOT}/DesktopSources.txt")
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DGE_SOURCE_ROOT=${GE_SOURCE_ROOT}" "-DGE_CASE_ROOT=${GE_TEST_ROOT}"
    -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT error MATCHES "manifest not found")
    message(FATAL_ERROR "Missing manifest did not produce an actionable error: ${error}")
endif()
