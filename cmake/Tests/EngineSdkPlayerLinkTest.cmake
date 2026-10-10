# Contract test for the SDK's generated GameEngineConfig.cmake, run by ctest as
#   cmake -DSDK=<staged SDK dir> -DGENERATOR=<generator> -DWORK=<scratch> -P <this file>
# Covered: the Player an export compiles (the SDK's own template and sources)
# configures and links against the staged SDK with GAMEENGINE_VCPKG_INSTALLED
# unset, as an editor outside its build tree exports it. The vcpkg headers come
# from the SDK's staged vcpkg-include; Engine.dll carries every vcpkg dependency.
foreach(_required SDK GENERATOR WORK)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "EngineSdkPlayerLinkTest.cmake: SDK, GENERATOR and WORK are required")
    endif()
endforeach()

set(_template_dir "${SDK}/templates/Player")
if(NOT EXISTS "${_template_dir}/GeneratedCMakeLists.txt.in")
    message(FATAL_ERROR "EngineSdkPlayerLinkTest: no Player template in '${_template_dir}'; build the Editor first")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/src")
file(GLOB _template_sources "${_template_dir}/*.cpp" "${_template_dir}/*.h")
file(COPY ${_template_sources} DESTINATION "${WORK}/src")

set(GE_GAME_NAME "SdkLinkProbe")
set(GE_SDK_CMAKE_DIR "${SDK}/cmake")
file(GLOB _player_sources "${WORK}/src/*.cpp")
list(JOIN _player_sources "\n    " GE_DISCOVERED_SOURCES)
set(GE_USER_INCLUDE_DIRS "")
set(GE_APP_ICON_SECTION "")
configure_file("${_template_dir}/GeneratedCMakeLists.txt.in" "${WORK}/CMakeLists.txt" @ONLY)

# Release, the configuration a shipped game builds in; the SDK picks the staged
# libs of the matching CRT flavor.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${WORK}" -B "${WORK}/build" -G "${GENERATOR}"
            "-DGameEngine_DIR=${SDK}/cmake" -DGAMEENGINE_BUILD_CONFIG=Release
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_output)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR "EngineSdkPlayerLinkTest: the Player did not configure without "
                        "GAMEENGINE_VCPKG_INSTALLED:\n${_configure_output}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${WORK}/build" --config Release
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_output)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR "EngineSdkPlayerLinkTest: the Player did not link without "
                        "GAMEENGINE_VCPKG_INSTALLED:\n${_build_output}")
endif()

file(REMOVE_RECURSE "${WORK}")
message(STATUS "EngineSdkPlayerLinkTest: passed")
