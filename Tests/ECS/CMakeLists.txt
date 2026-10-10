# Consolidated ECS Tests
# Reorganized from 5 separate test executables into 3 focused test suites
# for better maintainability and reduced build overhead

# Find Google Test
find_package(GTest REQUIRED)

# Destruction contract and allocation-failure injection (this executable arms
# allocation faults) stay out of the benchmark suites.
add_executable(EntityDestroyNotificationTests EntityDestroyNotificationTests.cpp EntityDestroyAllocationTests.cpp)
set_target_properties(EntityDestroyNotificationTests PROPERTIES UNITY_BUILD OFF)
target_compile_features(EntityDestroyNotificationTests PRIVATE cxx_std_20)
target_link_libraries(EntityDestroyNotificationTests PRIVATE GTest::gtest_main Logger JobSystem ECS)
ge_set_output_tests(EntityDestroyNotificationTests)
ge_stage_runtime_dependencies(EntityDestroyNotificationTests)
add_test(NAME EntityDestroyNotificationTests COMMAND EntityDestroyNotificationTests)
set_tests_properties(EntityDestroyNotificationTests PROPERTIES TIMEOUT 30)

# The query allocation contract (a process-wide allocation window, so its own executable).
add_executable(QueryAllocationTests QueryAllocationTests.cpp)
set_target_properties(QueryAllocationTests PROPERTIES UNITY_BUILD OFF)
target_compile_features(QueryAllocationTests PRIVATE cxx_std_20)
target_include_directories(QueryAllocationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(QueryAllocationTests PRIVATE GTest::gtest_main Logger JobSystem ECS)
ge_set_output_tests(QueryAllocationTests)
ge_stage_runtime_dependencies(QueryAllocationTests)
add_test(NAME QueryAllocationTests COMMAND QueryAllocationTests)
set_tests_properties(QueryAllocationTests PROPERTIES TIMEOUT 30)

# ============================================================================
# CONSOLIDATED TEST SUITE DEFINITIONS
# ============================================================================

# ECS Core Tests - Basic functionality and core operations
# Consolidates: StandaloneECSTests + ECSCoreTest
set(ECS_CORE_TEST_SOURCES
    StandaloneECSTest.cpp
    ECSCoreTest.cpp
    OversizedChunkAllocationTest.cpp
    RegisterComponentsTest.cpp
    QueryInvalidationAndOverflowTest.cpp
    CommandBufferRingTest.cpp
    ProcessCommandsBatchLimitTest.cpp
    DeferredRequiredComponentTest.cpp
    ViewAliasSmokeTest.cpp
    ViewWithoutTest.cpp
    QuerySignatureTest.cpp
    EnableStateTest.cpp
    ViewOptionallyVisibleTest.cpp
    ChangeFilterP1Test.cpp
    DirtyFeedTest.cpp
    LifecycleEventsTest.cpp
    SwapGenerationGuardTest.cpp
    ParallelPerfTest.cpp
    ParallelBatchEachTest.cpp
    BackpressureRetuneBench.cpp
    HierarchyQueriesTest.cpp
    RelationIndexTest.cpp
    QueryBenchmarks.cpp
    ComponentTypeNameTest.cpp
    ComponentReflectionTests.cpp
    ComponentMigrationTest.cpp
    ComponentLayoutReloadMigratorTests.cpp
    WorldClearStateTest.cpp
    WorldSnapshotIdentityTest.cpp
    SystemPlanTrailingWaveTest.cpp
    SystemPerformanceMaximaTest.cpp
    SystemWaveThreadPinningTest.cpp
    SystemSchedulePhaseInsertionTest.cpp
    OrderedHandleSetTest.cpp
    ModuleReloadScheduleTests.cpp
)

# ECS Advanced Tests - Advanced features and production readiness
# Consolidates: AutoRegistrationTests + EntityCreationTest + ComponentSystemTest + PublicAPITest
set(ECS_ADVANCED_TEST_SOURCES
    AutoRegistrationTests.cpp
    EntityCreationTest.cpp
    PublicAPITest.cpp
    ComponentSystemTest.cpp
    NewFeaturesTest.cpp
    RemovalHookLifetimeTests.cpp
    ComponentHookSubscriptionTests.cpp
    LiteComponentRegistrationTests.cpp
    ComparisonBenchmark.cpp
    HierarchyBenchmark.cpp
)

# ECS Integration Tests - Cross-library integration (remains separate)
set(ECS_INTEGRATION_TEST_SOURCES
    IntegrationTest.cpp
)

# ============================================================================
# ECS CORE TESTS - Basic functionality and core operations
# ============================================================================

add_executable(ECSCoreTests ${ECS_CORE_TEST_SOURCES})

# Disable unity builds for test executables to avoid ODR violations from
# third-party headers (concurrentqueue.h) that don't use include guards.
set_target_properties(ECSCoreTests PROPERTIES UNITY_BUILD OFF)

target_compile_features(ECSCoreTests PUBLIC cxx_std_20)

target_include_directories(ECSCoreTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine/Include
)

target_link_libraries(ECSCoreTests PRIVATE
    GTest::gtest
    GTest::gtest_main
    Logger
    JobSystem
    ECS
    # ComponentReflectionTests exercises the Mathematics field-type traits
    # (Components/MathReflectionTraits.h). Mathematics is INTERFACE/header-only, so
    # this propagates its include dirs to the TEST only — ECS stays Mathematics-free.
    Mathematics
)

ge_set_output_tests(ECSCoreTests)
ge_stage_runtime_dependencies(ECSCoreTests)

# ============================================================================
# ECS ADVANCED TESTS - Advanced features and production readiness
# ============================================================================

add_executable(ECSAdvancedTests ${ECS_ADVANCED_TEST_SOURCES})

target_compile_features(ECSAdvancedTests PUBLIC cxx_std_20)

# Enable AVX2+FMA for SIMD benchmarks
if(MSVC)
    target_compile_options(ECSAdvancedTests PRIVATE /arch:AVX2)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(ECSAdvancedTests PRIVATE -mavx2 -mfma)
endif()

target_include_directories(ECSAdvancedTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine/Include
)

target_link_libraries(ECSAdvancedTests PRIVATE
    GTest::gtest
    GTest::gtest_main
    Logger
    JobSystem
    ECS
    # Lightweight component registration uses Components/MathReflectionTraits.h.
    Mathematics
)

ge_set_output_tests(ECSAdvancedTests)
ge_stage_runtime_dependencies(ECSAdvancedTests)

# ============================================================================
# ECS INTEGRATION TESTS - Cross-library integration (remains separate)
# ============================================================================

add_executable(ECSIntegrationTests ${ECS_INTEGRATION_TEST_SOURCES})

target_compile_features(ECSIntegrationTests PUBLIC cxx_std_20)

target_include_directories(ECSIntegrationTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ECSIntegrationTests PRIVATE
    GTest::gtest
    GTest::gtest_main
    Logger
    JobSystem
    ECS
)

ge_set_output_tests(ECSIntegrationTests)
ge_stage_runtime_dependencies(ECSIntegrationTests)

# ============================================================================

# Optional diagnostics build flag for adapter counters
option(ECS_ADAPTER_DIAGNOSTICS "Enable adapter diagnostics counters in ChunkCallAdapter" OFF)
if(ECS_ADAPTER_DIAGNOSTICS)
    target_compile_definitions(ECSCoreTests PRIVATE ECS_ADAPTER_DIAGNOSTICS=1)
    target_compile_definitions(ECSAdvancedTests PRIVATE ECS_ADAPTER_DIAGNOSTICS=1)
    target_compile_definitions(ECSIntegrationTests PRIVATE ECS_ADAPTER_DIAGNOSTICS=1)
endif()

# ============================================================================
# CHANGE-SIGNALING ENFORCEMENT COMPILE-FAIL MATRIX (design §4.6, M4a + D-SDK)
# ============================================================================
# Each case is an EXCLUDE_FROM_ALL object target that must FAIL to compile;
# the ctest invokes the build for that target and expects failure (WILL_FAIL).
# CompilePassControl is the positive control: it must keep compiling, so the
# WILL_FAIL cases can never pass vacuously against a broken include set.

set(ECS_COMPILE_FAIL_CASES
    EachAutoUnqualified
    EachReadMutableParam
    EachComponentOnlyReadMutable
    BatchEachAutoUnqualified
    BatchEachReadMutablePtr
    ForEachChunkReadMutablePtr
    ParallelReadMutableParam
    ParallelBatchEachAutoUnqualified
    OptionalAutoDeduced
    ChangedTypeNotInQuery
    SetEnabledOnNotToggleable
)

foreach(case IN LISTS ECS_COMPILE_FAIL_CASES)
    add_library(ECSCompileFail_${case} OBJECT CompileFail/${case}.cpp)
    set_target_properties(ECSCompileFail_${case} PROPERTIES
        EXCLUDE_FROM_ALL TRUE
        EXCLUDE_FROM_DEFAULT_BUILD TRUE
        UNITY_BUILD OFF)
    target_compile_features(ECSCompileFail_${case} PUBLIC cxx_std_20)
    target_include_directories(ECSCompileFail_${case} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ECSCompileFail_${case} PRIVATE Logger JobSystem ECS)

    add_test(NAME ECSCompileFail.${case}
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                     --target ECSCompileFail_${case} --config $<CONFIG>)
    set_tests_properties(ECSCompileFail.${case} PROPERTIES
        WILL_FAIL TRUE
        LABELS "CompileFail"
        RUN_SERIAL TRUE) # msbuild contends on the shared build tree
endforeach()

# Positive control (must compile): built as part of the default build via the
# same harness path, expected to SUCCEED.
add_library(ECSCompileFail_Control OBJECT CompileFail/CompilePassControl.cpp)
set_target_properties(ECSCompileFail_Control PROPERTIES
    EXCLUDE_FROM_ALL TRUE
    EXCLUDE_FROM_DEFAULT_BUILD TRUE
    UNITY_BUILD OFF)
target_compile_features(ECSCompileFail_Control PUBLIC cxx_std_20)
target_include_directories(ECSCompileFail_Control PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ECSCompileFail_Control PRIVATE Logger JobSystem ECS)
add_test(NAME ECSCompileFail.PositiveControl
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                 --target ECSCompileFail_Control --config $<CONFIG>)
set_tests_properties(ECSCompileFail.PositiveControl PROPERTIES
    LABELS "CompileFail"
    RUN_SERIAL TRUE)

# TEST DISCOVERY FOR CTEST
# ============================================================================

include(GoogleTest)
gtest_discover_tests(ECSCoreTests)
gtest_discover_tests(ECSAdvancedTests)
gtest_discover_tests(ECSIntegrationTests)

# ============================================================================
# CONSOLIDATION SUMMARY
# ============================================================================
#
# BEFORE: 5 separate test executables
# - StandaloneECSTests (9 tests)
# - AutoRegistrationTests (18 tests)
# - ECSIntegrationTests (5 tests)
# - MinimalECSTests (2 tests)
# - AdditionalECSTests (15 tests)
# Total: 49 tests across 5 executables
#
# AFTER: 3 consolidated test suites
# - ECSCoreTests (11 tests) = StandaloneECSTests + ECSCoreTest
# - ECSAdvancedTests (33 tests) = AutoRegistrationTests + EntityCreationTest + ComponentSystemTest + PublicAPITest
# - ECSIntegrationTests (5 tests) = unchanged
# Total: 49 tests across 3 executables (40% reduction in build targets)
#
# Benefits:
# - Reduced build time and maintenance overhead
# - Logical organization by functionality level
# - Preserved 100% test coverage
# ============================================================================
