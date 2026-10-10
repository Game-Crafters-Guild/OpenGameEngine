# Runs CopySponzaModel.cmake on a stand-in checkout under WORK_DIR.
#
#   cmake -DSCRIPT=<CopySponzaModel.cmake> -DWORK_DIR=<scratch> -DCASE=<case> -P TestCopySponzaModel.cmake
#
# CASE copies-license: the checkout has Models/Sponza/LICENSE.md; the copy succeeds and
#   lays the license beside the model byte for byte.
# CASE refuses-without-license: the checkout has no LICENSE.md; the copy fails, names
#   the missing file and copies nothing.

foreach(_required SCRIPT WORK_DIR CASE)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "TestCopySponzaModel.cmake: missing -D${_required}=...")
    endif()
endforeach()

set(_clone "${WORK_DIR}/clone")
set(_dest "${WORK_DIR}/dest")
file(REMOVE_RECURSE "${WORK_DIR}")
file(WRITE "${_clone}/Models/Sponza/glTF/Sponza.gltf" "{\"asset\":{\"version\":\"2.0\"}}\n")
if(CASE STREQUAL "copies-license")
    file(WRITE "${_clone}/Models/Sponza/LICENSE.md" "# LICENSE file for the model: Sponza\n")
elseif(NOT CASE STREQUAL "refuses-without-license")
    message(FATAL_ERROR "TestCopySponzaModel.cmake: unknown CASE '${CASE}'")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DCLONE_ROOT=${_clone}" "-DDEST=${_dest}" -P "${SCRIPT}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _output)

if(CASE STREQUAL "copies-license")
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "The copy failed with a license present:\n${_output}")
    endif()
    if(NOT EXISTS "${_dest}/Sponza.gltf")
        message(FATAL_ERROR "The model was not copied to ${_dest}")
    endif()
    if(NOT EXISTS "${_dest}/LICENSE.md")
        message(FATAL_ERROR "The license was not copied beside the model to ${_dest}")
    endif()
    file(SHA256 "${_clone}/Models/Sponza/LICENSE.md" _expected)
    file(SHA256 "${_dest}/LICENSE.md" _actual)
    if(NOT _expected STREQUAL _actual)
        message(FATAL_ERROR "The copied license differs from the upstream file")
    endif()
else()
    if(_result EQUAL 0)
        message(FATAL_ERROR "The copy succeeded without a license")
    endif()
    # CMake wraps a message to the console width; match with the wrapping removed.
    string(REGEX REPLACE "[ \t\r\n]+" " " _flat "${_output}")
    if(NOT _flat MATCHES "Models/Sponza/LICENSE.md is missing")
        message(FATAL_ERROR "The failure does not name the missing license:\n${_output}")
    endif()
    if(EXISTS "${_dest}/Sponza.gltf")
        message(FATAL_ERROR "The model was copied without its license")
    endif()
endif()
