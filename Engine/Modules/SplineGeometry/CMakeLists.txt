cmake_minimum_required(VERSION 3.20)

project(SplineGeometry LANGUAGES CXX)

# Procedural geometry along a spline's station stream: a swept cross-section for
# authored-width runs, and a flooded region for water. Deliberately depends on
# neither Spline nor the asset/rendering headers: it takes stations and a plain
# ground lattice as input, which is what keeps the terrain and raycast
# dependencies (the editor-bound pieces of the pipeline) on the caller's side of
# the seam and keeps the geometry itself unit testable without a device.
add_library(SplineGeometry OBJECT
    Source/SplineCorner.cpp
    Source/SplineProfile.cpp
    Source/SplineStripBuilder.cpp
    Source/SplineStep.cpp
    Source/SplineEndTaper.cpp
    Source/SplineFillField.cpp
    Source/SplineFillMesher.cpp
    Source/MeshPlaneCut.cpp
)

target_compile_features(SplineGeometry PUBLIC cxx_std_20)

target_include_directories(SplineGeometry
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(SplineGeometry
    PUBLIC
        Types
        Mathematics
)

# Self-find GTest (UI/Input/Spline pattern): gating on root-scope GTest state
# instead would silently drop the test target whenever the root skips its find.
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(SplineGeometryTests
        Tests/SplineProfileTests.cpp
        Tests/SplineStripBuilderTests.cpp
        Tests/SplineCornerTests.cpp
        Tests/SplineStepTests.cpp
        Tests/SplineEndTaperTests.cpp
        Tests/SplineFillFieldTests.cpp
        Tests/SplineFillMesherTests.cpp
        Tests/MeshPlaneCutTests.cpp
    )
    target_link_libraries(SplineGeometryTests PRIVATE SplineGeometry GTest::gtest GTest::gtest_main)
    set_target_properties(SplineGeometryTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests/SplineGeometry"
    )
    ge_set_output_tests(SplineGeometryTests)
    ge_stage_runtime_dependencies(SplineGeometryTests)
    add_test(NAME SplineGeometryTests COMMAND SplineGeometryTests)
endif()
