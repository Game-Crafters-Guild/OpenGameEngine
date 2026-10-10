# Noise module: the engine's shared CPU noise primitives.
#
# Header-only, so the sampling loops that call it keep inlining them. The module
# header lists what lives here and what deliberately stays with its own GPU
# mirror; see Include/Noise/GradientNoise2D.h.

cmake_minimum_required(VERSION 3.20)

project(Noise LANGUAGES CXX)

add_library(Noise INTERFACE)

# Public includes: exposes headers under "Noise/..." to dependents.
target_include_directories(Noise INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Include)

# Types supplies float32/uint32/int32; Mathematics the vector type of the 3D fields.
target_link_libraries(Noise INTERFACE Types Mathematics)

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMakeLists.txt")
    add_subdirectory(Tests)
endif()
