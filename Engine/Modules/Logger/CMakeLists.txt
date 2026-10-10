cmake_minimum_required(VERSION 3.20)

# Logger Library - Unified logging system for all modules
project(Logger LANGUAGES CXX)

# Core Logger source files
set(LOGGER_SOURCES
    Source/Logger.cpp
    Source/ConsoleSink.cpp
    Source/CallbackSink.cpp
    Source/DeferredSink.cpp
    Source/RingBufferSink.cpp
    Source/Backtrace.cpp
)

# Core Logger header files
set(LOGGER_HEADERS
    Include/Logger/Logger.h
    Include/Logger/LogLevel.h
    Include/Logger/LogSink.h
    Include/Logger/CallbackSink.h
    Include/Logger/DeferredSink.h
    Include/Logger/RingBufferSink.h
    Include/Logger/Backtrace.h
    Include/Logger/Types.h
)

# Optional features (can be excluded for minimal builds)
set(LOGGER_OPTIONAL_SOURCES
    Source/FileSink.cpp
    Source/NetworkSink.cpp
)

set(LOGGER_OPTIONAL_HEADERS
    Include/Logger/FileSink.h
    Include/Logger/NetworkSink.h
)

# Option to include advanced logging features
option(LOGGER_ENABLE_FILE_LOGGING "Enable file logging functionality" ON)
option(LOGGER_ENABLE_NETWORK_LOGGING "Enable network logging functionality" OFF)

if(LOGGER_ENABLE_FILE_LOGGING)
    list(APPEND LOGGER_SOURCES Source/FileSink.cpp)
    list(APPEND LOGGER_HEADERS Include/Logger/FileSink.h)
    message(STATUS "Logger: File logging enabled")
else()
    message(STATUS "Logger: File logging disabled")
endif()

if(LOGGER_ENABLE_NETWORK_LOGGING)
    list(APPEND LOGGER_SOURCES Source/NetworkSink.cpp)
    list(APPEND LOGGER_HEADERS Include/Logger/NetworkSink.h)
    message(STATUS "Logger: Network logging enabled")
else()
    message(STATUS "Logger: Network logging disabled")
endif()

# Create static library
add_library(Logger OBJECT ${LOGGER_SOURCES} ${LOGGER_HEADERS})

# Configure feature defines so headers can see which sinks are available
if(LOGGER_ENABLE_FILE_LOGGING)
    target_compile_definitions(Logger PUBLIC LOGGER_ENABLE_FILE_LOGGING=1)
else()
    target_compile_definitions(Logger PUBLIC LOGGER_ENABLE_FILE_LOGGING=0)
endif()

# Set C++20 standard
target_compile_features(Logger PUBLIC cxx_std_20)

# Include directories
target_include_directories(Logger PUBLIC Include)
target_include_directories(Logger PRIVATE Source)

# Logger is standalone - no external dependencies except standard library
# This makes it the foundation for all other modules

# Platform-specific definitions
if(WIN32)
    target_compile_definitions(Logger PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    # Add Windows-specific libraries if needed for file operations
    if(LOGGER_ENABLE_FILE_LOGGING)
        # Windows file system libraries are included by default
    endif()
endif()

# Compiler-specific options
if(MSVC)
    target_compile_options(Logger PRIVATE /W4)
    # Disable specific warnings for Logger module
    target_compile_options(Logger PRIVATE /wd4100) # Unreferenced formal parameter
else()
    target_compile_options(Logger PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Export compile commands for IDEs
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Installation
install(TARGETS Logger
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY Include/Logger
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# Debug information
message(STATUS "Logger: Configured as static library")
message(STATUS "Logger: Include directory: ${CMAKE_CURRENT_SOURCE_DIR}/Include")
message(STATUS "Logger: Source files: ${LOGGER_SOURCES}")
message(STATUS "Logger: Header files: ${LOGGER_HEADERS}")

# Performance and debugging options
option(LOGGER_ENABLE_THREAD_SAFETY "Enable thread-safe logging (slight performance cost)" ON)
option(LOGGER_ENABLE_TIMESTAMPS "Enable timestamp logging" ON)
option(LOGGER_ENABLE_SOURCE_LOCATION "Enable source file/line logging (debug builds only)" ON)

if(LOGGER_ENABLE_THREAD_SAFETY)
    target_compile_definitions(Logger PUBLIC LOGGER_THREAD_SAFE=1)
    message(STATUS "Logger: Thread safety enabled")
else()
    target_compile_definitions(Logger PUBLIC LOGGER_THREAD_SAFE=0)
    message(STATUS "Logger: Thread safety disabled (better performance)")
endif()

if(LOGGER_ENABLE_TIMESTAMPS)
    target_compile_definitions(Logger PUBLIC LOGGER_ENABLE_TIMESTAMPS=1)
    message(STATUS "Logger: Timestamps enabled")
else()
    target_compile_definitions(Logger PUBLIC LOGGER_ENABLE_TIMESTAMPS=0)
    message(STATUS "Logger: Timestamps disabled")
endif()

if(LOGGER_ENABLE_SOURCE_LOCATION AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(Logger PUBLIC LOGGER_ENABLE_SOURCE_LOCATION=1)
    message(STATUS "Logger: Source location enabled (Debug build)")
else()
    target_compile_definitions(Logger PUBLIC LOGGER_ENABLE_SOURCE_LOCATION=0)
    message(STATUS "Logger: Source location disabled")
endif()

# Tests: built with the global testing switch (or forced on via LOGGER_BUILD_TESTS
# for a standalone module configure).
option(LOGGER_BUILD_TESTS "Build Logger tests" OFF)
if(LOGGER_BUILD_TESTS OR BUILD_TESTING)
    enable_testing()
    add_subdirectory(Tests)
endif()
