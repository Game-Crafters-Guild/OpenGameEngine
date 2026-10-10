# Events module: header-only multicast event (C#-delegate-style subscriber
# list with self-detaching handles). See Include/Events/Event.h.

cmake_minimum_required(VERSION 3.20)

project(Events LANGUAGES CXX)

# Header-only target to propagate include path transitively
add_library(Events INTERFACE)

# Public include: exposes "Events/Event.h" to dependents
target_include_directories(Events INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Include)

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMakeLists.txt")
    add_subdirectory(Tests)
endif()
