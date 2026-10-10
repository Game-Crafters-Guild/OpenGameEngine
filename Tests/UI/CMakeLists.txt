# UI Tests CMakeLists.txt

# Find required packages
find_package(GTest REQUIRED)

# New lightweight unit tests for our UI docking data model
add_executable(UIDockingModelTests
    DockingModelUnitTests.cpp
)

# Engine ONLY — UI / AssetCore / AssetRuntime / GameEngineRendering are OBJECT
# modules already spliced into Engine.dll; linking them here as well would give
# the exe a second copy of every module's file-scope state (issue #356, same
# disease as #340). Headers propagate via Engine's re-exported INTERFACE.
target_link_libraries(UIDockingModelTests PRIVATE
    GTest::gtest GTest::gtest_main
    Engine)

set_target_properties(UIDockingModelTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

ge_set_output_tests(UIDockingModelTests)
ge_stage_runtime_dependencies(UIDockingModelTests)
if(TARGET StageTestAssets)
  add_dependencies(UIDockingModelTests StageTestAssets)
endif()

add_test(NAME UIDockingModelTests COMMAND $<TARGET_FILE:UIDockingModelTests>)
set_tests_properties(UIDockingModelTests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
