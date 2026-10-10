cmake_minimum_required(VERSION 3.20)
project(SplineLayout LANGUAGES CXX)

# Placement of authored pieces along caller-sampled centerlines. Spline remains
# the low-level curve evaluator; this module uses component recipes and writes
# component values, without an Engine, Editor, renderer or device dependency.
add_library(SplineLayout OBJECT
    Source/CenterlineSampling.cpp
    Source/FenceLayout.cpp
    Source/PieceEntity.cpp
    Source/SeamShear.cpp
    Source/SpanMitre.cpp
    Source/TileLayout.cpp
)
target_compile_features(SplineLayout PUBLIC cxx_std_20)
target_include_directories(SplineLayout PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Include)
target_link_libraries(SplineLayout PUBLIC Types Mathematics AssetCore ECSComponents SplineGeometry)
set_target_properties(SplineLayout PROPERTIES FOLDER "Engine/Modules")

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(SplineLayoutTests
        Tests/CenterlineSamplingTests.cpp
        Tests/FenceLayoutTests.cpp
        Tests/SeamShearTests.cpp
        Tests/SpanMitreTests.cpp
        Tests/SplineStationJitterTests.cpp
        Tests/TileLayoutTests.cpp
    )
    # OBJECT dependencies propagate headers, not object files: the recipe's
    # asset references need AssetCore's GUID constructor, and the span mitre
    # SplineGeometry's plane cut, in this standalone link.
    target_link_libraries(SplineLayoutTests PRIVATE SplineLayout SplineGeometry AssetCore GTest::gtest GTest::gtest_main)
    set_target_properties(SplineLayoutTests PROPERTIES FOLDER "Engine/Tests/SplineLayout")
    ge_set_output_tests(SplineLayoutTests)
    ge_stage_runtime_dependencies(SplineLayoutTests)
    add_test(NAME SplineLayoutTests COMMAND SplineLayoutTests)
endif()
