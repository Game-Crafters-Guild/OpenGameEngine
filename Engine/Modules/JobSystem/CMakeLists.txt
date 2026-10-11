cmake_minimum_required(VERSION 3.20)

# JobSystem Library - Task execution and dependency management
project(JobSystem LANGUAGES CXX)

# Core Job System source files
set(JOBSYSTEM_SOURCES
    Source/WorkStealingThreadPool.cpp
    Source/BlockingThreads.cpp
    Source/ParallelForCore.cpp
    Source/JobChannel.cpp
    Source/JobChannelState.cpp
    Source/JobCounter.cpp
    Source/Task.cpp
    Source/TaskHandle.cpp
    Source/TaskDependencyGraph.cpp
    Source/TaskSlabPool.cpp
)

# Core Job System header files
set(JOBSYSTEM_HEADERS
    Include/JobSystem/WorkStealingThreadPool.h
    Include/JobSystem/ParallelForOptions.h
    Include/JobSystem/JobChannel.h
    Include/JobSystem/JobSystemStatistics.h
    Include/JobSystem/JobCounter.h
    Include/JobSystem/Task.h
    Include/JobSystem/TaskEnvelopes.h
    Include/JobSystem/TaskHandle.h
    Include/JobSystem/TaskDependencyGraph.h
    Include/JobSystem/TaskSlabPool.h
    Include/JobSystem/TaskTypes.h
    Include/JobSystem/Types.h
    Include/JobSystem/ParallelAlgorithms.h
)

# Create static library
add_library(JobSystem OBJECT ${JOBSYSTEM_SOURCES} ${JOBSYSTEM_HEADERS})

# Set C++20 standard
target_compile_features(JobSystem PUBLIC cxx_std_20)

# Include directories
target_include_directories(JobSystem PUBLIC Include)
target_include_directories(JobSystem PRIVATE Source ${CMAKE_SOURCE_DIR}/Engine/Include)

# Dependencies
# The imported target owns its include requirements. Publishing guessed vcpkg
# roots here can shadow other packages for consumers and change shared type layouts.
find_package(concurrentqueue CONFIG REQUIRED)
target_link_libraries(JobSystem PUBLIC concurrentqueue::concurrentqueue)

# JobSystem module dependencies (target-based; avoid hardcoded include paths)
target_link_libraries(JobSystem PUBLIC Logger Types)

# Platform-specific definitions
if(WIN32)
    target_compile_definitions(JobSystem PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# Compiler-specific options
if(MSVC)
    target_compile_options(JobSystem PRIVATE /W4)
else()
    target_compile_options(JobSystem PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Export compile commands for IDEs
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Installation
install(TARGETS JobSystem
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY Include/JobSystem
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# Tests
if(BUILD_TESTING)
    add_subdirectory(Tests)
endif()

# Debug information
message(STATUS "JobSystem: Configured as static library")
message(STATUS "JobSystem: Include directory: ${CMAKE_CURRENT_SOURCE_DIR}/Include")
message(STATUS "JobSystem: Source files: ${JOBSYSTEM_SOURCES}")
message(STATUS "JobSystem: Header files: ${JOBSYSTEM_HEADERS}")
