cmake_minimum_required(VERSION 3.20)

project(Mathematics LANGUAGES CXX)

# Header-only math utilities shared between Engine, Editor, and other modules.
add_library(Mathematics INTERFACE)

# Public includes: exposes headers under "Mathematics/..." to dependents.
target_include_directories(Mathematics INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Include)

# glm is used in Vector3.h and other headers. Propagate to dependents.
find_package(glm CONFIG QUIET)
if(TARGET glm::glm)
    target_link_libraries(Mathematics INTERFACE glm::glm)
endif()

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMakeLists.txt")
    add_subdirectory(Tests)
endif()
