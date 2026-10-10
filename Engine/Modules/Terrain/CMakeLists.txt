cmake_minimum_required(VERSION 3.20)

project(Terrain LANGUAGES CXX)

add_library(Terrain OBJECT
    Source/Heightfield.cpp
    Source/CDLODQuadtree.cpp
    Source/CDLODSelection.cpp
)

target_compile_features(Terrain PUBLIC cxx_std_20)

target_include_directories(Terrain
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(Terrain
    PUBLIC
        Types
        Mathematics
        Logger
)

# stb_image for heightmap loading (PNG16)
find_path(STB_INCLUDE_DIRS "stb_image.h")
if(STB_INCLUDE_DIRS)
    target_include_directories(Terrain PRIVATE ${STB_INCLUDE_DIRS})
    # stb_image_impl.cpp is compiled by Engine — Terrain only needs the header
    target_compile_definitions(Terrain PRIVATE GE_HAVE_STB)
else()
    message(WARNING "stb_image.h not found -- heightmap PNG loading will fail to compile")
endif()

# The root CMakeLists finds GTest AFTER this module is added, so resolve it
# here — otherwise the guard below never passes and TerrainTests silently
# drops out of the build.
if(BUILD_TESTING AND NOT TARGET GTest::gtest)
    find_package(GTest CONFIG QUIET)
endif()

if(BUILD_TESTING AND (GTest_FOUND OR TARGET GTest::gtest))
    add_executable(TerrainTests
        Tests/HeightfieldTests.cpp
        Tests/CDLODSelectionTests.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/stb_image_impl.cpp
    )
    target_link_libraries(TerrainTests PRIVATE Terrain GTest::gtest GTest::gtest_main)
    if(STB_INCLUDE_DIRS)
        target_include_directories(TerrainTests PRIVATE ${STB_INCLUDE_DIRS})
        target_compile_definitions(TerrainTests PRIVATE GE_HAVE_STB)
    endif()
    set_target_properties(TerrainTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests/Terrain"
    )
    ge_set_output_tests(TerrainTests)
    ge_stage_runtime_dependencies(TerrainTests)
    add_test(NAME TerrainTests COMMAND TerrainTests)
endif()
