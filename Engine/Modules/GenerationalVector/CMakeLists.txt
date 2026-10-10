# GenerationalVector Library
# A high-performance generational handle system for safe resource management

cmake_minimum_required(VERSION 3.20)
project(GenerationalVector LANGUAGES CXX)

# Set C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Library sources
set(GENERATIONAL_VECTOR_SOURCES
    Source/GenerationalManager.cpp
)

# Create the library
add_library(GenerationalVector OBJECT ${GENERATIONAL_VECTOR_SOURCES})

# Include directories
target_include_directories(GenerationalVector
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
        $<INSTALL_INTERFACE:Include>
)

# Compiler-specific options
if(MSVC)
    target_compile_options(GenerationalVector PRIVATE /W4)
else()
    target_compile_options(GenerationalVector PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Export the library for use by other projects
set_target_properties(GenerationalVector PROPERTIES
    EXPORT_NAME GenerationalVector
    VERSION ${GE_ENGINE_VERSION_NUMERIC}
    SOVERSION 1
)

# Install rules
install(TARGETS GenerationalVector
    EXPORT GenerationalVectorTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION Include
)

install(DIRECTORY Include/
    DESTINATION Include
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

install(EXPORT GenerationalVectorTargets
    FILE GenerationalVectorTargets.cmake
    NAMESPACE GenerationalVector::
    DESTINATION lib/cmake/GenerationalVector
)

# Create config file
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    GenerationalVectorConfigVersion.cmake
    VERSION ${GE_ENGINE_VERSION_NUMERIC}
    COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerationalVectorConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/GenerationalVectorConfig.cmake
    INSTALL_DESTINATION lib/cmake/GenerationalVector
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/GenerationalVectorConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/GenerationalVectorConfigVersion.cmake
    DESTINATION lib/cmake/GenerationalVector
)

# Tests follow the repo-wide switch, like every other module.
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMakeLists.txt")
    add_subdirectory(Tests)
endif()

# Examples (optional)
option(GENERATIONAL_VECTOR_BUILD_EXAMPLES "Build GenerationalVector examples" OFF)
if(GENERATIONAL_VECTOR_BUILD_EXAMPLES)
    add_subdirectory(Examples)
endif()
