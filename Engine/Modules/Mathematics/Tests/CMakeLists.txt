find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(STATUS "GTest not found, skipping Mathematics tests")
    return()
endif()

add_executable(MathematicsTests
    CurveEvaluationTests.cpp
    GeometryTests.cpp
    TriangulationTests.cpp
    Matrix4x4Tests.cpp
    Vector3Tests.cpp
    VectorComponentTests.cpp
)

target_link_libraries(MathematicsTests
    PRIVATE
        Mathematics
        GTest::gtest
        GTest::gtest_main
)

set_target_properties(MathematicsTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "Mathematics/Tests"
)

ge_set_output_tests(MathematicsTests)
ge_stage_runtime_dependencies(MathematicsTests)

add_test(NAME MathematicsTests COMMAND MathematicsTests)
set_tests_properties(MathematicsTests PROPERTIES
    TIMEOUT 30
    LABELS "Mathematics;Unit"
)
