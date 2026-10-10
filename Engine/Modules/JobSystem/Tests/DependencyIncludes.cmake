cmake_minimum_required(VERSION 3.20)

# Configure the real module with a selected package and several unrelated roots.
# Inspect the generated consumer usage requirements without building worker code.
file(MAKE_DIRECTORY "${TEST_BINARY_DIR}/source")
set(fixture [=[
cmake_minimum_required(VERSION 3.20)
project(JobSystemDependencyIncludes LANGUAGES CXX)
set(BUILD_TESTING OFF)
set(selected "${CMAKE_BINARY_DIR}/selected-package")
file(MAKE_DIRECTORY "${selected}/include")
file(WRITE "${selected}/concurrentqueueConfig.cmake"
    "add_library(concurrentqueue::concurrentqueue INTERFACE IMPORTED)\n"
    "set_target_properties(concurrentqueue::concurrentqueue PROPERTIES INTERFACE_INCLUDE_DIRECTORIES \"${selected}/include\")\n")
set(concurrentqueue_DIR "${selected}")

set(CONCURRENTQUEUE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/stale-cache" CACHE PATH "")
set(VCPKG_INSTALLED_DIR "${CMAKE_SOURCE_DIR}/unrelated-install")
set(VCPKG_TARGET_TRIPLET x64-windows)
file(MAKE_DIRECTORY "${CONCURRENTQUEUE_INCLUDE_DIR}"
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
    "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/include"
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/include")

add_library(Logger INTERFACE)
add_library(Types INTERFACE)
add_subdirectory("@JOBSYSTEM_SOURCE_DIR@" jobsystem)
file(WRITE "${CMAKE_BINARY_DIR}/consumer.cpp" "int HeaderConsumer;\n")
add_library(HeaderConsumer OBJECT "${CMAKE_BINARY_DIR}/consumer.cpp")
target_link_libraries(HeaderConsumer PRIVATE JobSystem)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/consumer-includes.txt"
    CONTENT "$<JOIN:$<TARGET_PROPERTY:HeaderConsumer,INCLUDE_DIRECTORIES>,\n>")
]=])
string(CONFIGURE "${fixture}" fixture @ONLY)
file(WRITE "${TEST_BINARY_DIR}/source/CMakeLists.txt" "${fixture}")

set(generator_args -G "${TEST_GENERATOR}")
if(TEST_GENERATOR_PLATFORM)
    list(APPEND generator_args -A "${TEST_GENERATOR_PLATFORM}")
endif()
if(TEST_GENERATOR_TOOLSET)
    list(APPEND generator_args -T "${TEST_GENERATOR_TOOLSET}")
endif()
if(TEST_MAKE_PROGRAM)
    list(APPEND generator_args "-DCMAKE_MAKE_PROGRAM:FILEPATH=${TEST_MAKE_PROGRAM}")
endif()
if(TEST_CXX_COMPILER)
    list(APPEND generator_args "-DCMAKE_CXX_COMPILER:FILEPATH=${TEST_CXX_COMPILER}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${TEST_BINARY_DIR}/source"
    -B "${TEST_BINARY_DIR}/build" ${generator_args}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Fixture configure failed:\n${output}\n${error}")
endif()
file(STRINGS "${TEST_BINARY_DIR}/build/consumer-includes.txt" includes)
set(expected "${TEST_BINARY_DIR}/build/selected-package/include")
if(NOT expected IN_LIST includes)
    message(FATAL_ERROR "The package target's includes did not reach the consumer:\n${includes}")
endif()
foreach(forbidden IN ITEMS
    "${TEST_BINARY_DIR}/source/stale-cache"
    "${TEST_BINARY_DIR}/source/unrelated-install/x64-windows/include"
    "${TEST_BINARY_DIR}/source/vcpkg_installed/x64-windows/include"
    "${TEST_BINARY_DIR}/build/vcpkg_installed/x64-windows/include")
    if(forbidden IN_LIST includes)
        message(FATAL_ERROR "JobSystem leaked unrelated dependency root: ${forbidden}")
    endif()
endforeach()
message(STATUS "JobSystem consumer receives the selected package without unrelated include roots")
