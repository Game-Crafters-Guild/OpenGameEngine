cmake_minimum_required(VERSION 3.20)

find_package(GTest REQUIRED)

add_executable(FontAtlasCoreTests
    FontAtlasCoreTests.cpp
)

add_executable(FontAtlasShapingTests
    FontAtlasShapingTests.cpp
)

target_link_libraries(FontAtlasCoreTests
    PRIVATE
    Text
    GTest::gtest
    GTest::gtest_main
)

target_link_libraries(FontAtlasShapingTests
    PRIVATE
    Text
    GTest::gtest
    GTest::gtest_main
)

set_target_properties(FontAtlasShapingTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

set_target_properties(FontAtlasCoreTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# Output layout: bin/<Config>/Tests, with the runtime DLL closure staged
# beside each exe so the binaries run standalone. See cmake/OutputLayout.cmake.
foreach(_ge_t IN ITEMS FontAtlasCoreTests FontAtlasShapingTests)
    ge_set_output_tests(${_ge_t})
    ge_stage_runtime_dependencies(${_ge_t})
    # Platform::GetExecutablePath anchors the staged fixtures. Linking Platform
    # alone fails on Windows: its MenuIconBitmap_Win32.cpp calls the SVG
    # rasteriser and image decoder that live in Engine, so Engine is linked.
    target_link_libraries(${_ge_t} PRIVATE Engine)
    add_custom_command(TARGET ${_ge_t} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_ge_t}>/TestData/Text"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/Apps/Editor/Assets/Fonts/Roboto-Regular.ttf"
            "${PROJECT_SOURCE_DIR}/Apps/Editor/Assets/Fonts/Roboto-Italic.ttf"
            "${CMAKE_CURRENT_SOURCE_DIR}/Fonts/NotoSansArabic-Regular.ttf"
            "${CMAKE_CURRENT_SOURCE_DIR}/Fonts/Noto-OFL.txt"
            "$<TARGET_FILE_DIR:${_ge_t}>/TestData/Text"
        VERBATIM)
endforeach()

include(GoogleTest)
add_test(NAME FontAtlasCoreTests COMMAND FontAtlasCoreTests)
add_test(NAME FontAtlasShapingTests COMMAND FontAtlasShapingTests)

