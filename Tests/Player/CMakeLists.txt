# Standalone Player tests.
#
# PlayerHdrOptions and PlayerLog are leaf TUs — header-only mode parsing plus Logger,
# and the log file's sinks — so the production sources compile in directly: no Player
# executable, no window, no device.

if(BUILD_TESTING)
    # PrepareBundle invokes /usr/bin/ditto and preserves bundle symlinks, and
    # Codesign runs codesign; these integration tests require the macOS tools,
    # not just filesystem APIs. MacPackageCodeExportTests runs a whole macOS export
    # (BuildPipeline::Execute) whose native modules are copies of a real dylib,
    # since the export edits their load commands with install_name_tool.
    if(APPLE)
        add_executable(MacBundleAssemblerTests MacBundleAssemblerTests.cpp MacPackageCodeExportTests.cpp)
        target_include_directories(MacBundleAssemblerTests PRIVATE ${CMAKE_SOURCE_DIR}/Tests)
        target_compile_features(MacBundleAssemblerTests PRIVATE cxx_std_20)
        target_link_libraries(MacBundleAssemblerTests PRIVATE Engine GTest::gtest_main)
        add_dependencies(MacBundleAssemblerTests MacBundleFixtureDylib MacBundleUniversalFixtureDylib)
        target_compile_definitions(MacBundleAssemblerTests PRIVATE
            GE_MAC_BUNDLE_FIXTURE_DYLIB="$<TARGET_FILE:MacBundleFixtureDylib>"
            GE_MAC_BUNDLE_UNIVERSAL_FIXTURE_DYLIB="$<TARGET_FILE:MacBundleUniversalFixtureDylib>")
        ge_set_output_tests(MacBundleAssemblerTests)
        ge_stage_runtime_dependencies(MacBundleAssemblerTests)
        add_test(NAME MacBundleAssemblerTests COMMAND $<TARGET_FILE:MacBundleAssemblerTests>)
    endif()

    # Pure filesystem preparation: exercise the exact production owner without
    # booting Engine, loading a user DLL or creating a graphics device.
    add_executable(PlayerSourcePreparationTests
        PlayerSourcePreparationTests.cpp
        ${CMAKE_SOURCE_DIR}/Engine/Source/Engine/Build/PlayerSourcePreparation.cpp
    )
    target_include_directories(PlayerSourcePreparationTests PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Include)
    target_compile_features(PlayerSourcePreparationTests PRIVATE cxx_std_20)
    target_link_libraries(PlayerSourcePreparationTests PRIVATE GTest::gtest GTest::gtest_main)
    ge_set_output_tests(PlayerSourcePreparationTests)
    ge_stage_runtime_dependencies(PlayerSourcePreparationTests)
    add_test(NAME PlayerSourcePreparationTests COMMAND $<TARGET_FILE:PlayerSourcePreparationTests>)
    add_test(NAME DesktopPlayerSourcesManifestTests
        COMMAND "${CMAKE_COMMAND}"
            "-DGE_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
            "-DGE_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/ManifestTests"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/DesktopSourcesManifestTests.cmake"
    )

    add_executable(PlayerTests
        PlayerHdrOptionsTests.cpp
        PlayerLogTests.cpp
        ${CMAKE_SOURCE_DIR}/Apps/Player/Source/PlayerHdrOptions.cpp
        ${CMAKE_SOURCE_DIR}/Apps/Player/Source/PlayerLog.cpp
    )
    target_include_directories(PlayerTests PRIVATE
        ${CMAKE_SOURCE_DIR}/Apps/Player/Source
        ${CMAKE_SOURCE_DIR}/Engine/Tests
        ${CMAKE_SOURCE_DIR}/Tests
    )
    target_link_libraries(PlayerTests PRIVATE
        Engine
        GTest::gtest
        GTest::gtest_main
    )
    set_target_properties(PlayerTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        VCPKG_APPLOCAL_DEPS OFF
    )
    ge_set_output_tests(PlayerTests)
    ge_stage_runtime_dependencies(PlayerTests)
    add_test(NAME PlayerTests
        COMMAND "${CMAKE_COMMAND}"
            -DTEST_EXE=$<TARGET_FILE:PlayerTests>
            -DTEST_WORKING_DIR=$<TARGET_FILE_DIR:PlayerTests>
            -P "${CMAKE_SOURCE_DIR}/cmake/RunTestExecutable.cmake"
    )
endif()
