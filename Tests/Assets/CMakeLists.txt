# Asset System Tests
cmake_minimum_required(VERSION 3.20)

# Find Google Test
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    # Download and build Google Test if not found
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/03597a01ee50ed33e9fd7188a1e9cd6a8e8b4bc3.zip
    )
    FetchContent_MakeAvailable(googletest)
endif()

# Asset System Test Sources (Google Test based - optional)
set(ASSET_TEST_SOURCES
    AssetIOServiceTests.cpp
    AssetDecodeGateTests.cpp
    AssetDecodeCancellationTests.cpp
    AsyncAssetReloadTests.cpp
    InlineReloadDeferralTests.cpp
    ParserRegistryExtensionDispatchTests.cpp
    AssetLoadPipelineTests.cpp
    AssetTypeLoadingTests.cpp
    AssetErrorHandlingTests.cpp
    Complete4StagePipelineIntegrationTest.cpp
    AudioAssetPolicyTests.cpp
    MetaFileRegistryTests.cpp
    AssetMetaMaintenanceTests.cpp
    FilePatternTests.cpp
    AssetRegistryConcurrencyTests.cpp
    AssetRootSeparationTests.cpp
    FoldedPathIndexTests.cpp
    AssetDbLegacyPathNormalizationTests.cpp
    AssetDbIgnoresMetaSidecarsTests.cpp
    AssetDbTypeInferencePersistenceTests.cpp
    TerrainMaterialLibraryAssetTests.cpp
    AssetDbHardeningTests.cpp
    AssetDbDerivedReconcileTests.cpp
    AssetDbDerivedRedirectTests.cpp
    AssetDbGhostRetentionTests.cpp
    AssetDbSubassetCascadeTests.cpp
    ImportedMaterialCacheTests.cpp
    EmbeddedImageContentTests.cpp
    AssetDbCaseIdentityTests.cpp
    AssetDbJournalGrowthTests.cpp
    AssetDbSharedStoreTests.cpp
    AssetDbPublishTests.cpp
    AssetTransientResidencyTests.cpp
    AssetDbCacheFactoryTestEnv.cpp
    ${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp
    RenderPipelineAssetParsingTests.cpp
    ShaderMaterialWorkflowE2ETests.cpp
    MaterialAssetExtensionRegistrationTests.cpp
    MaterialAssetParsingTests.cpp
    FileWatcherExclusionTests.cpp
    SafeSaveRenameTests.cpp
    PathNormalizationTests.cpp
    OverlappingSourceRemapTests.cpp
    PackageSystemTests.cpp
    PackageDistributionTests.cpp
    PackageManagementActionsTests.cpp
    PackageStoredIdentityTests.cpp
    BuildShippableContentTests.cpp
    BuildRenderPipelineCollectionTests.cpp
    BuildSceneCollectionTests.cpp
    BuildDefaultUIFontCollectionTests.cpp
    FbxLoaderOptionsTests.cpp
)

# Create Asset System Tests executable
add_executable(AssetSystemTests ${ASSET_TEST_SOURCES})

# AssetSystemTests compiles many .cpp files that often share helper names in anonymous namespaces.
# Unity builds merge translation units and can cause redefinition errors. Keep this target non-unity.
set_target_properties(AssetSystemTests PROPERTIES UNITY_BUILD OFF)

# Enable C++20 features
target_compile_features(AssetSystemTests PUBLIC cxx_std_20)

# Link with required libraries.
# AssetDatabaseSqlite: AssetDbHardeningTests instantiates AssetDbCache_Sqlite
# directly, and registry-backed tests inject the cache factory — Engine.dll no
# longer links the SQLite implementation, so test targets must link it.
# AssetCore is deliberately NOT linked directly: it is an OBJECT module
# already spliced into Engine.dll, and linking it here again would give the
# exe a second copy of its file-scope state (issue #356, same disease as
# #340). Headers propagate via Engine's re-exported INTERFACE.
target_link_libraries(AssetSystemTests PRIVATE
    Engine
    AssetDatabaseSqlite
    GameEngine.Native
    GTest::GTest
    GTest::Main
)

# Include directories
target_include_directories(AssetSystemTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Tests
    # StagedTestPaths.h — exe-anchored fixture resolution, same as the Graph
    # module's test target uses.
    ${CMAKE_SOURCE_DIR}/Engine/Tests
)

# Output layout: bin/<Config>/Tests
ge_set_output_tests(AssetSystemTests)
ge_stage_runtime_dependencies(AssetSystemTests)
# AssetDbCaseIdentityTests migrates the frozen Biggles journal, which
# StageTestAssets mirrors into the build root. Wired here rather than in the
# central fixture list in Tests/CMakeLists.txt: that list is evaluated before
# this subdirectory defines the target.
if(TARGET StageTestAssets)
    add_dependencies(AssetSystemTests StageTestAssets)
endif()
# The eztree engine-package acceptance tests consume a STAGED copy of the
# package, never the source tree. It stages under TestData/ — NOT the exe-dir
# Packages/ the resolver's implicit default scans, because every test
# executable shares bin/<Config>/Tests and the default would leak the real
# package into every resolver test's implicit set.
add_custom_command(TARGET AssetSystemTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_SOURCE_DIR}/Packages"
            "$<TARGET_FILE_DIR:AssetSystemTests>/TestData/Packages"
    COMMENT "Staging Packages test copy for AssetSystemTests")

# Copy test assets
add_custom_command(TARGET AssetSystemTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:AssetSystemTests>/TestAssets
    COMMENT "Creating test assets directory"
)

# Add test discovery for CTest
include(GoogleTest)
gtest_discover_tests(AssetSystemTests)

# Performance Benchmark Tests (separate executable for performance testing)
add_executable(AssetPerformanceBenchmarks
    AssetLoadPipelineTests.cpp
    AssetPerformanceRegressionTests.cpp
    AssetPerformanceRegressionFramework.cpp
    AssetDbLargeProjectPerformanceTests.cpp
    SnapshotWarmStartBenchmark.cpp
    ${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp
)

target_compile_features(AssetPerformanceBenchmarks PUBLIC cxx_std_20)

# Engine only (plus exe-local AssetDatabaseSqlite) — same single-link rule as
# AssetSystemTests above.
target_link_libraries(AssetPerformanceBenchmarks PRIVATE
    Engine
    AssetDatabaseSqlite
    GameEngine.Native
    GTest::GTest
    GTest::Main
)

target_include_directories(AssetPerformanceBenchmarks PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Tests
)

# Output layout: bin/<Config>/Tests
ge_set_output_tests(AssetPerformanceBenchmarks)
ge_stage_runtime_dependencies(AssetPerformanceBenchmarks)

# Add performance test discovery
gtest_discover_tests(AssetPerformanceBenchmarks)

# Custom test targets for different test categories
add_custom_target(run_asset_tests
    COMMAND $<TARGET_FILE:AssetSystemTests>
    DEPENDS AssetSystemTests
    WORKING_DIRECTORY $<TARGET_FILE_DIR:AssetSystemTests>
    COMMENT "Running Asset System Tests"
)

add_custom_target(run_asset_performance_benchmarks
    COMMAND $<TARGET_FILE:AssetPerformanceBenchmarks>
    DEPENDS AssetPerformanceBenchmarks
    WORKING_DIRECTORY $<TARGET_FILE_DIR:AssetPerformanceBenchmarks>
    COMMENT "Running Asset Performance Benchmarks"
)

# Combined target for all asset tests
add_custom_target(run_all_asset_tests
    DEPENDS run_asset_tests run_asset_performance_benchmarks
    COMMENT "Running All Asset System Tests and Benchmarks"
)

# Opt-in: the asset database's manual scale and reconcile benchmarks (tens of thousands of files,
# minutes of wall time, every test DISABLED_). Not registered with CTest and run by no gate: build
# AssetDbBenchmarks and run an arm by name with --gtest_also_run_disabled_tests.
add_executable(AssetDbBenchmarks
    AssetDbScaleBenchmark.cpp
    AssetDbReconcilePerfBenchmark.cpp
    AssetDbCacheFactoryTestEnv.cpp
)
set_target_properties(AssetDbBenchmarks PROPERTIES UNITY_BUILD OFF)
target_compile_features(AssetDbBenchmarks PUBLIC cxx_std_20)
target_link_libraries(AssetDbBenchmarks PRIVATE
    Engine
    AssetDatabaseSqlite
    GameEngine.Native
    GTest::GTest
    GTest::Main
)
target_include_directories(AssetDbBenchmarks PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Tests
    ${CMAKE_SOURCE_DIR}/Engine/Tests
)
ge_set_output_tests(AssetDbBenchmarks)
ge_stage_runtime_dependencies(AssetDbBenchmarks)

# FileWatcher exclusion logic tests
add_executable(FileWatcherExclusionTests FileWatcherExclusionTests.cpp)
target_compile_features(FileWatcherExclusionTests PUBLIC cxx_std_20)
target_link_libraries(FileWatcherExclusionTests PRIVATE Engine GTest::GTest GTest::Main)
target_include_directories(FileWatcherExclusionTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Tests
)
ge_set_output_tests(FileWatcherExclusionTests)
ge_stage_runtime_dependencies(FileWatcherExclusionTests)
gtest_discover_tests(FileWatcherExclusionTests)

# FilePattern + FileWatchingService subscription tests. Standalone executable rather
# than part of AssetSystemTests, so these keep building and running when something
# unrelated breaks that target.
add_executable(FilePatternTests FilePatternTests.cpp)
target_compile_features(FilePatternTests PUBLIC cxx_std_20)
# Needs GameEngine.Native for DLL-exported hot-reload / compile-cache symbols
# pulled in transitively via AssetRuntime (FileWatchingService's lib).
target_link_libraries(FilePatternTests PRIVATE Engine GameEngine.Native GTest::GTest GTest::Main)
target_include_directories(FilePatternTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Tests
)
ge_set_output_tests(FilePatternTests)
ge_stage_runtime_dependencies(FilePatternTests)
gtest_discover_tests(FilePatternTests)
