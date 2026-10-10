# Job system tests.
#
# JobSystemTests is the PURE module suite (links JobSystem + Logger only —
# never Engine, no scripting, no shim) and builds on every platform. The
# scripting/hot-reload suites below the platform gate rely on the legacy
# GameEngine.NativeShim and a Windows-only layout for managed test projects;
# Linux/macOS scripting smoke coverage is provided by EngineCoreClrSmokeTests
# and EngineCompileServerSmokeTests.
find_package(GTest REQUIRED)

# Test sources are split by link home (issue #340). JobSystemTests is the
# pure module suite: it links the JobSystem OBJECT library directly and never
# loads Engine.dll. A test exe must never link both JobSystem and Engine: two
# copies of the module mean two m_NextTaskId counters, and any test mixing
# exe-side TaskHandles with Engine-side tasks in one pool draws colliding
# TaskIds into that pool's registry, corrupting F12 cancel arbitration.
set(JOB_SYSTEM_TEST_SOURCES
    JobSystemTestsMain.cpp
    WorkStealingThreadPoolTests.cpp
    WakeProtocolAndSpinTests.cpp
    TaskDependencyGraphTests.cpp
    SubmitWithDependencyTests.cpp
    CancellationAndShutdownTests.cpp
    CompletionWaiterGateTests.cpp
    CompletionFencePinTests.cpp
    BatchEnqueueTests.cpp
    InlineModeAndExternalWaitTests.cpp
    JobChannelTests.cpp
    JobCounterTests.cpp
    JobPriorityTests.cpp
    JobSystemStatisticsTests.cpp
    QueueFairnessTests.cpp
    TaskSlabPoolTests.cpp
    TopologyCensusBench.cpp
    PerformanceRegressionTests.cpp
    ../Performance/WorkStealingTest.cpp
)

add_executable(JobSystemTests ${JOB_SYSTEM_TEST_SOURCES})
# The blocking-capability contract uses Platform's header-only thread helper.
target_include_directories(JobSystemTests PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Include)

# C6326 fires on EXPECT_EQ(constant, constant) — gtest expands the macro into
# a helper whose both operands SAL-analyses as constant when the test uses
# literal values (common in WorkStealingThreadPoolTests). C4702 (unreachable
# code) fires on `throw; return x;` patterns in gtest EXPECT_THROW lambdas.
if(MSVC)
    target_compile_options(JobSystemTests PRIVATE /wd6326 /wd4702 /W4)
else()
    target_compile_options(JobSystemTests PRIVATE -Wall -Wextra -Wpedantic)
endif()

# No GTest::Main: JobSystemTestsMain.cpp provides main() so Debug CRT asserts
# report to stderr instead of a modal dialog (which wedges unattended runs
# silently — see the PR #342 participating-wait triage).
# The alloc tripwires count through the allocation counter's hook, which every
# test executable carries (cmake/AllocationHook.cmake); the module under test is
# the exe-local copy, so the counted code is this executable's in every
# configuration. Logger is listed directly because OBJECT-library objects
# propagate only one level (same pattern as Tests/ECS).
target_link_libraries(JobSystemTests
    JobSystem
    Logger
    GTest::GTest
)

set_target_properties(JobSystemTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME JobSystemUnitTests COMMAND $<TARGET_FILE:JobSystemTests>)
# The wake-protocol stress + bench suites (WakeProtocolAndSpinTests.cpp) add
# ~90s of deliberate wall time.
set_tests_properties(JobSystemUnitTests PROPERTIES
    TIMEOUT 600  # 10 minutes timeout
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── Scripting / hot-reload suites (Windows-only, shim-dependent) ──────────
if(NOT ENABLE_SCRIPTING OR NOT WIN32)
  return()
endif()

# Build and deploy a tiny native shim DLL to satisfy CoreBridge's ABI probe in tests
if(WIN32)
  # GameEngineNativeShimUi.cpp is a TEST DOUBLE for the game-UI element ABI, so the managed
  # binding's own bookkeeping (wrapper identity, one native subscription per event, listener
  # isolation, ALC revocation) can be driven from a dotnet-test host that has no engine. The
  # engine-side contract stays pinned in UITests and ScriptingAbiIntegrationTests.
  add_library(GameEngine.NativeShim SHARED
      ../NativeShim/GameEngineNativeShim.cpp
      ../NativeShim/GameEngineNativeShimUi.cpp)
  # OUTPUT_NAME collides with the real GameEngine.Native target, so the shim
  # must build into its own directory or it clobbers the real DLL/.lib in the
  # shared output dirs (consumers copy it via $<TARGET_FILE:...>, which follows).
  set_target_properties(GameEngine.NativeShim PROPERTIES OUTPUT_NAME "GameEngine.Native")
  # Per-config variants required: the top-level CMAKE_*_OUTPUT_DIRECTORY_<CONFIG>
  # globals initialize per-config target properties, which override the generic ones.
  foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${cfg}" cfgUpper)
    set_target_properties(GameEngine.NativeShim PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_${cfgUpper} "${CMAKE_CURRENT_BINARY_DIR}/NativeShim/${cfg}"
      LIBRARY_OUTPUT_DIRECTORY_${cfgUpper} "${CMAKE_CURRENT_BINARY_DIR}/NativeShim/${cfg}"
      ARCHIVE_OUTPUT_DIRECTORY_${cfgUpper} "${CMAKE_CURRENT_BINARY_DIR}/NativeShim/${cfg}"
      PDB_OUTPUT_DIRECTORY_${cfgUpper} "${CMAKE_CURRENT_BINARY_DIR}/NativeShim/${cfg}")
  endforeach()
  # Include Engine public headers so the shim can include ScriptingABI.h
  target_include_directories(GameEngine.NativeShim PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Include)

endif()

# Create test executable for job system (defined later) and only then wire copy

# Ensure we can invoke dotnet to build managed components
find_program(DOTNET_EXECUTABLE dotnet REQUIRED)

# ONE managed configuration for every dotnet build, staging copy and test command in this file.
#
# It has to be one mapping, and DebugFast has to land on Debug. Two separate bugs came from
# getting that wrong:
#   * The test commands said a literal `-c Debug` while the build and staging steps mapped every
#     non-Debug config to Release, so a DebugFast tree ran `dotnet test -c Debug` against a fresh
#     bin/Debug output nothing had been staged into: the binding fell back to the real
#     GameEngine.Native.dll, which has no vcpkg runtime dependencies beside it, and 44 of 75
#     tests failed with DllNotFoundException. A false red indistinguishable from a broken change.
#   * Mapping DebugFast to Release is wrong even when it IS consistent: the suite drives
#     accessors that only exist in a Debug managed build (HotReloadManager's
#     GetPreloadedAssemblyForTests and GetCurrentAssembly are `#if DEBUG`), so three
#     InitializeOnLoad arms fail on a missing method. DebugFast is a debug config — same /Od and
#     PDBs as Debug, differing only in CRT and vcpkg variant — so it belongs on Debug here.
set(GE_MANAGED_TEST_CFG "$<IF:$<OR:$<CONFIG:Debug>,$<CONFIG:DebugFast>>,Debug,Release>")




# HotReloadPipelineTests hosts every test that exercises Engine-side code
# (HotReloadPipeline, compile server, scripting ABI) and links Engine ONLY,
# resolving JobSystem symbols against the copy spliced into Engine.dll (issue
# #340 — see the pure-suite comment above for why the two never mix).
set(HOTRELOAD_PIPELINE_TEST_SOURCES
    JobSystemTestsMain.cpp
    HotReloadPipelineTests.cpp
    HotReloadPipelineCompileServerIntegrationTest.cpp
    HotReloadPipelineTestHooksTests.cpp
    HotReloadPipelineMultiInstanceIsolationTests.cpp
    ../HotReloadPerformanceRegressionTests.cpp
    CompileServerClientFakeTransportTests.cpp
    CompileServerEndToEndTests.cpp
    CompileServerHostLifetimeTests.cpp
    CompileServerTestHost.cpp
    ScriptManagerCompileServerIntegrationTest.cpp
    ScriptsProjectGenerationTests.cpp
    ScriptingAbiHandshakeTests.cpp
    PackageScriptCompileTests.cpp
)

add_executable(HotReloadPipelineTests ${HOTRELOAD_PIPELINE_TEST_SOURCES})

# See the pure-suite block above for the C6326/C4702 rationale.
if(MSVC)
    target_compile_options(HotReloadPipelineTests PRIVATE /wd6326 /wd4702)
endif()

# Ensure the shim is built before the pipeline tests (CoreBridge ABI probe)
add_dependencies(HotReloadPipelineTests GameEngine.NativeShim)

# HotReloadPipelineTests deliberately does NOT link JobSystem: exe-side
# Submit handles and the pipeline's Engine-side stage tasks must draw their
# TaskIds from the single counter inside Engine.dll (issue #340; pinned by
# HotReloadPipelineSupersede.QueuedRunIsSupersededWithoutEverCompiling).
target_link_libraries(HotReloadPipelineTests
    Engine
    GTest::GTest
    GameEngine.NativeShim
)
target_include_directories(HotReloadPipelineTests PRIVATE
    ${CMAKE_SOURCE_DIR}/Tests          # TestTempDir.h
    ${CMAKE_SOURCE_DIR}/Engine/Tests)  # EngineLogCapture.h


# Build managed projects before copying artifacts to avoid stale DLLs
if(BUILD_MANAGED_HOTRELOAD)

if (NOT TARGET BuildManagedHotReload)
  add_custom_target(BuildManagedHotReload ALL
      COMMAND ${DOTNET_EXECUTABLE} build ${CMAKE_SOURCE_DIR}/Managed/HotReload/GameEngine.HotReload.csproj -c ${GE_MANAGED_TEST_CFG}
      COMMAND ${DOTNET_EXECUTABLE} build ${CMAKE_SOURCE_DIR}/Managed/CoreBridge/GameEngine.CoreBridge.csproj -c ${GE_MANAGED_TEST_CFG}
	      COMMAND ${DOTNET_EXECUTABLE} build ${CMAKE_SOURCE_DIR}/Managed/CompileServerHost/GameEngine.CompileServerHost.csproj -c ${GE_MANAGED_TEST_CFG}

      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Building managed HotReload and CoreBridge projects")
endif()
endif()


if(BUILD_MANAGED_HOTRELOAD)

add_dependencies(HotReloadPipelineTests BuildManagedHotReload)
else()
# CompileServerEndToEnd / ScriptManager integration tests launch the resident
# compile server from Managed/CompileServerHost/bin/<Config>/net10.0, preferring
# the native configuration (CompileServerClient::FindHostPath). Without this
# dependency a clean tree has no host DLL (StartServerForPipeName fails in every
# end-to-end test) and a dev tree silently runs whatever stale host was built
# last. CompileServerHost builds in $<CONFIG>, and only when its inputs changed.
# BuildManagedHotReload already builds the host when enabled.
add_dependencies(HotReloadPipelineTests CompileServerHost)
endif()

# Optional managed interop test project (NUnit)
add_custom_target(BuildManagedInteropTests ALL
    COMMAND ${DOTNET_EXECUTABLE} build ${CMAKE_SOURCE_DIR}/Managed/InteropTests/InteropTests.csproj -c ${GE_MANAGED_TEST_CFG}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building managed interop tests")
add_dependencies(HotReloadPipelineTests BuildManagedInteropTests)
# Managed CoreBridge tests (NUnit)
add_custom_target(BuildManagedCoreBridgeTests ALL
    COMMAND ${DOTNET_EXECUTABLE} build ${CMAKE_SOURCE_DIR}/Managed/CoreBridgeTests/CoreBridgeTests.csproj -c ${GE_MANAGED_TEST_CFG}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building managed CoreBridge tests")
add_dependencies(HotReloadPipelineTests BuildManagedCoreBridgeTests)
add_dependencies(BuildManagedCoreBridgeTests GameEngine.NativeShim)
# Serialize against BuildManagedInteropTests: both dotnet-build project graphs
# share csprojs (CoreBridge and friends), and concurrent MSBuild invocations
# race on the shared outputs (CS2012 file locks).
add_dependencies(BuildManagedCoreBridgeTests BuildManagedInteropTests)
# Same serialization: the CoreBridgeTests build also builds DomainRoutingTest
# through its project reference, into the output BuildDomainRoutingTest writes.
if(TARGET BuildDomainRoutingTest)
  add_dependencies(BuildManagedCoreBridgeTests BuildDomainRoutingTest)
endif()
if(BUILD_MANAGED_HOTRELOAD)

add_dependencies(BuildManagedCoreBridgeTests BuildManagedHotReload)
endif()

add_custom_command(TARGET BuildManagedCoreBridgeTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        ${CMAKE_SOURCE_DIR}/Managed/CoreBridgeTests/bin/${GE_MANAGED_TEST_CFG}/net10.0
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:GameEngine.NativeShim>
        ${CMAKE_SOURCE_DIR}/Managed/CoreBridgeTests/bin/${GE_MANAGED_TEST_CFG}/net10.0/$<TARGET_FILE_NAME:GameEngine.Native>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/Managed/CoreBridge/bin/${GE_MANAGED_TEST_CFG}/net10.0/GameEngine.CoreBridge.dll
        ${CMAKE_SOURCE_DIR}/Managed/CoreBridgeTests/bin/${GE_MANAGED_TEST_CFG}/net10.0/GameEngine.CoreBridge.dll
    COMMENT "Copying GameEngine.Native and CoreBridge to Managed CoreBridgeTests output")

# Same config-mapping rule as ManagedInteropTests below: the -c must match the genex the build
# and staging steps use, or the run tests an output nothing was staged into.
add_test(NAME ManagedCoreBridgeTests
    COMMAND ${DOTNET_EXECUTABLE} test ${CMAKE_SOURCE_DIR}/Managed/CoreBridgeTests/CoreBridgeTests.csproj
            -c ${GE_MANAGED_TEST_CFG})
set_tests_properties(ManagedCoreBridgeTests PROPERTIES
    TIMEOUT 120
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    ENVIRONMENT "DOTNET_ROLL_FORWARD=LatestPatch;DOTNET_ROOT=${DOTNET_DIR};GE_NATIVE_DIR=${CMAKE_BINARY_DIR}/bin/$<CONFIG>;PATH=${CMAKE_BINARY_DIR}/bin/$<CONFIG>;${DOTNET_DIR};$ENV{PATH}")

# Ensure native shim is available next to managed interop tests
add_dependencies(BuildManagedInteropTests GameEngine.NativeShim)
add_custom_command(TARGET BuildManagedInteropTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        ${CMAKE_SOURCE_DIR}/Managed/InteropTests/bin/${GE_MANAGED_TEST_CFG}/net10.0
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:GameEngine.NativeShim>
        ${CMAKE_SOURCE_DIR}/Managed/InteropTests/bin/${GE_MANAGED_TEST_CFG}/net10.0/$<TARGET_FILE_NAME:GameEngine.Native>
    COMMENT "Copying GameEngine.Native to Managed InteropTests output")

add_custom_command(TARGET BuildManagedInteropTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove -f
        ${CMAKE_SOURCE_DIR}/Managed/InteropTests/bin/${GE_MANAGED_TEST_CFG}/net10.0/hostfxr.dll
    COMMAND ${CMAKE_COMMAND} -E remove -f
        ${CMAKE_SOURCE_DIR}/Managed/InteropTests/bin/${GE_MANAGED_TEST_CFG}/net10.0/nethost.dll
    COMMENT "Preparing Managed InteropTests output")


# Derive the dotnet installation directory for PATH augmentation
get_filename_component(DOTNET_DIR "${DOTNET_EXECUTABLE}" DIRECTORY)


# Add managed interop tests to CTest (no-build because we build above)
# Add managed interop tests to CTest (no-build because we build above)
# The -c MUST be the same generator expression the build and staging steps above use. It was a
# literal `Debug` while they mapped every non-Debug config (DebugFast, MinSizeRel,
# RelWithDebInfo) to Release: `dotnet test -c Debug` then built a FRESH bin/Debug output with no
# shim staged into it, the binding fell back to GE_NATIVE_DIR — the real GameEngine.Native.dll,
# which has no vcpkg runtime dependencies staged beside it — and the suite reported 44 of 75
# tests failing with DllNotFoundException. A false red that looks exactly like a broken change.
add_test(NAME ManagedInteropTests
    COMMAND ${DOTNET_EXECUTABLE} test ${CMAKE_SOURCE_DIR}/Managed/InteropTests/InteropTests.csproj
            -c ${GE_MANAGED_TEST_CFG})
# Run from repo root and set env so native shim is discoverable and correct dotnet is used
set_tests_properties(ManagedInteropTests PROPERTIES
    TIMEOUT 120
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    ENVIRONMENT "DOTNET_ROLL_FORWARD=LatestPatch;DOTNET_ROOT=${DOTNET_DIR};GE_NATIVE_DIR=${CMAKE_BINARY_DIR}/bin/$<CONFIG>;PATH=${CMAKE_BINARY_DIR}/bin/$<CONFIG>;${DOTNET_DIR};$ENV{PATH}")


# Build and run a tiny managed smoke to validate DllImport resolution
add_custom_target(ManagedInteropSmoke ALL
    COMMAND ${DOTNET_EXECUTABLE} build ${CMAKE_SOURCE_DIR}/Managed/InteropSmoke/InteropSmoke.csproj -c Debug
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building managed interop smoke")

add_dependencies(ManagedInteropSmoke GameEngine.NativeShim)
# Same CS2012 serialization: InteropSmoke's dotnet build also rebuilds
# CoreBridge through its ProjectReference graph.
add_dependencies(ManagedInteropSmoke BuildManagedCoreBridgeTests)

add_test(NAME ManagedInteropSmoke
    COMMAND ${DOTNET_EXECUTABLE} run --project ${CMAKE_SOURCE_DIR}/Managed/InteropSmoke/InteropSmoke.csproj -c Debug --no-build
)
set_tests_properties(ManagedInteropSmoke PROPERTIES
    TIMEOUT 60
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    ENVIRONMENT "GE_NATIVE_DIR=${CMAKE_BINARY_DIR}/bin/$<CONFIG>;PATH=${CMAKE_BINARY_DIR}/bin/$<CONFIG>;${DOTNET_DIR};$ENV{PATH}")

# Ensure native shim and CoreBridge are available next to InteropSmoke output
add_custom_command(TARGET ManagedInteropSmoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        ${CMAKE_SOURCE_DIR}/Managed/InteropSmoke/bin/${GE_MANAGED_TEST_CFG}/net10.0
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:GameEngine.NativeShim>
        ${CMAKE_SOURCE_DIR}/Managed/InteropSmoke/bin/${GE_MANAGED_TEST_CFG}/net10.0/$<TARGET_FILE_NAME:GameEngine.Native>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/Managed/CoreBridge/bin/${GE_MANAGED_TEST_CFG}/net10.0/GameEngine.CoreBridge.dll
        ${CMAKE_SOURCE_DIR}/Managed/InteropSmoke/bin/${GE_MANAGED_TEST_CFG}/net10.0/GameEngine.CoreBridge.dll
    COMMENT "Copying GameEngine.Native and CoreBridge to InteropSmoke output")




# Stage the managed set HotReloadPipelineTests resolves from its executable
# directory: PathResolver anchors CoreBridge/HotReload/runtimeconfig probing
# there, and the compile-server integration tests reference the ABI assemblies
# through the generated project's EngineBinDir (= this exe's directory).
# BuildManagedInteropTests (a hard dependency above) builds every one of these
# outputs via InteropTests' ProjectReferences, so the sources are guaranteed to
# exist at POST_BUILD time — no configure-time EXISTS guards, which silently
# skip staging in fresh worktrees.
if(WIN32)
    set(_ge_managed_cfg "${GE_MANAGED_TEST_CFG}")
    add_custom_command(TARGET HotReloadPipelineTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/Managed/CoreBridge/bin/${_ge_managed_cfg}/net10.0/GameEngine.CoreBridge.dll
            ${CMAKE_SOURCE_DIR}/Managed/HotReload/bin/${_ge_managed_cfg}/net10.0/GameEngine.HotReload.dll
            ${CMAKE_SOURCE_DIR}/Managed/HotReload/bin/${_ge_managed_cfg}/net10.0/GameEngine.HotReload.runtimeconfig.json
            ${CMAKE_SOURCE_DIR}/Managed/HotReload/bin/${_ge_managed_cfg}/net10.0/GameEngine.HotReload.deps.json
            ${CMAKE_SOURCE_DIR}/Managed/Scripting.Runtime/bin/${_ge_managed_cfg}/GameEngine.Scripting.Runtime.dll
            ${CMAKE_SOURCE_DIR}/Managed/Scripting.Runtime/bin/${_ge_managed_cfg}/GameEngine.Scripting.ABI.dll
            ${CMAKE_SOURCE_DIR}/Managed/Scripting.Runtime/bin/${_ge_managed_cfg}/GameEngine.ECS.ABI.dll
            $<TARGET_FILE_DIR:HotReloadPipelineTests>
        COMMENT "Staging managed CoreBridge/HotReload/ABI set to HotReloadPipelineTests output")

    # CompileServerHost probes <EngineBinDir>/SourceGenerators for source generators.
    # CompileServerEndToEnd.GeneratorErrorFailsCompile and
    # GeneratorWarningKeepsCompileSuccessful point EngineBinDir at this exe's directory,
    # so the generator has to be staged there or those tests would silently exercise a
    # generator-less server and pass for the wrong reason. The generator comes from
    # EntitySystemGeneratorAssembly, which builds it in the native configuration
    # ($<CONFIG>), not in the managed test configuration the set above uses.
    add_dependencies(HotReloadPipelineTests EntitySystemGeneratorAssembly)
    add_custom_command(TARGET HotReloadPipelineTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:HotReloadPipelineTests>/SourceGenerators
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/Managed/SourceGenerators/EntitySystemGenerator/bin/$<CONFIG>/netstandard2.0/EntitySystemGenerator.dll
            $<TARGET_FILE_DIR:HotReloadPipelineTests>/SourceGenerators/EntitySystemGenerator.dll
        COMMENT "Staging EntitySystemGenerator to HotReloadPipelineTests output")
endif()


# Set C++ standard
set_target_properties(HotReloadPipelineTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# Add compiler flags for testing
if(MSVC)
    target_compile_options(HotReloadPipelineTests PRIVATE /W4)
else()
    target_compile_options(HotReloadPipelineTests PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Add tests to CTest
add_test(NAME HotReloadPipelineTests COMMAND $<TARGET_FILE:HotReloadPipelineTests>)

set_tests_properties(HotReloadPipelineTests PROPERTIES
    TIMEOUT 600  # 10 minutes timeout (compile-server end-to-end runs)
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Create async hot-reload integration test
add_executable(AsyncHotReloadTests ../AsyncHotReloadTestSuite.cpp ../HotReloadBenchmark.cpp
    ../EngineShutdownTestEnvironment.cpp)



target_link_libraries(AsyncHotReloadTests
    Engine
    GameEngine.Native
    GTest::GTest
    GTest::Main
)

# The CLR-hosting set (CoreBridge, HotReload and its runtimeconfig) comes from the
# one staging target every executable in bin/<config>/Tests shares, so no two
# targets copy those files into that directory concurrently; GameEngine.Native.dll
# is staged by ge_stage_runtime_dependencies like any other imported library.
add_dependencies(AsyncHotReloadTests StageCoreBridgeAndHotReload)
set_target_properties(AsyncHotReloadTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# Add integration test to CTest
add_test(NAME AsyncHotReloadIntegrationTests COMMAND $<TARGET_FILE:AsyncHotReloadTests>)

set_tests_properties(AsyncHotReloadIntegrationTests PROPERTIES
    TIMEOUT 600  # 10 minutes timeout for integration tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Create performance benchmark executable
add_executable(HotReloadBenchmark ../HotReloadBenchmarkMain.cpp ../HotReloadBenchmark.cpp)

target_link_libraries(HotReloadBenchmark
    Engine
)

set_target_properties(HotReloadBenchmark PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# FileChangeTracker Tests
add_executable(FileChangeTrackerTests
    ../FileChangeTrackerTest.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/HotReloadNativeStubs.cpp
)
set_source_files_properties(${CMAKE_SOURCE_DIR}/Engine/Source/HotReloadNativeStubs.cpp
    TARGET_DIRECTORY FileChangeTrackerTests
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/wd4273>")

target_link_libraries(FileChangeTrackerTests
    Engine
    GTest::GTest
    GTest::Main
)

set_target_properties(FileChangeTrackerTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME FileChangeTrackerTest COMMAND $<TARGET_FILE:FileChangeTrackerTests>)

set_tests_properties(FileChangeTrackerTest PROPERTIES
    TIMEOUT 300  # 5 minutes timeout
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# DependencyGraphAnalyzer Tests
add_executable(DependencyGraphAnalyzerTests
    ../DependencyGraphAnalyzerTest.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/HotReloadNativeStubs.cpp
)
set_source_files_properties(${CMAKE_SOURCE_DIR}/Engine/Source/HotReloadNativeStubs.cpp
    TARGET_DIRECTORY DependencyGraphAnalyzerTests
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/wd4273>")

target_link_libraries(DependencyGraphAnalyzerTests
    Engine
    GTest::GTest
    GTest::Main
)

set_target_properties(DependencyGraphAnalyzerTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME DependencyGraphAnalyzerTest COMMAND $<TARGET_FILE:DependencyGraphAnalyzerTests>)

set_tests_properties(DependencyGraphAnalyzerTest PROPERTIES
    TIMEOUT 300  # 5 minutes timeout
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# IncrementalCompilation Tests
add_executable(IncrementalCompilationTests
    IncrementalCompilationTest.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/HotReloadNativeStubs.cpp
)
set_source_files_properties(${CMAKE_SOURCE_DIR}/Engine/Source/HotReloadNativeStubs.cpp
    TARGET_DIRECTORY IncrementalCompilationTests
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/wd4273>")

target_link_libraries(IncrementalCompilationTests
    Engine
    GTest::GTest
    GTest::Main
)

set_target_properties(IncrementalCompilationTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME IncrementalCompilationTest COMMAND $<TARGET_FILE:IncrementalCompilationTests>)

set_tests_properties(IncrementalCompilationTest PROPERTIES
    TIMEOUT 300  # 5 minutes timeout
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Add benchmark as a test (but don't fail build if it's slow)
add_test(NAME HotReloadPerformanceBenchmark COMMAND $<TARGET_FILE:HotReloadBenchmark>)

set_tests_properties(HotReloadPerformanceBenchmark PROPERTIES
    TIMEOUT 1200  # 20 minutes timeout for benchmarks
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# HybridCompilationIntegration Tests
add_executable(HybridCompilationIntegrationTests
    HybridCompilationIntegrationTest.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/HotReloadNativeStubs.cpp
)
set_source_files_properties(${CMAKE_SOURCE_DIR}/Engine/Source/HotReloadNativeStubs.cpp
    TARGET_DIRECTORY HybridCompilationIntegrationTests
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/wd4273>")

target_link_libraries(HybridCompilationIntegrationTests
    Engine
    GTest::GTest
    GTest::Main
)

set_target_properties(HybridCompilationIntegrationTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME HybridCompilationIntegrationTest COMMAND $<TARGET_FILE:HybridCompilationIntegrationTests>)

set_tests_properties(HybridCompilationIntegrationTest PROPERTIES
    TIMEOUT 600  # 10 minutes timeout for integration tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# HybridCompilationBenchmark Tests
add_executable(HybridCompilationBenchmarkTests
    HybridCompilationBenchmark.cpp
    ${CMAKE_SOURCE_DIR}/Engine/Source/HotReloadNativeStubs.cpp
)
set_source_files_properties(${CMAKE_SOURCE_DIR}/Engine/Source/HotReloadNativeStubs.cpp
    TARGET_DIRECTORY HybridCompilationBenchmarkTests
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/wd4273>")

target_link_libraries(HybridCompilationBenchmarkTests
    Engine
    GTest::GTest
    GTest::Main
)

set_target_properties(HybridCompilationBenchmarkTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME HybridCompilationBenchmarkTest COMMAND $<TARGET_FILE:HybridCompilationBenchmarkTests>)

set_tests_properties(HybridCompilationBenchmarkTest PROPERTIES
    TIMEOUT 900  # 15 minutes timeout for benchmark tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Custom target to run all async hot-reload tests
add_custom_target(run_async_hotreload_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R "JobSystem|HotReloadPipeline|AsyncHotReload|HotReloadPerformance|FileChangeTracker|DependencyGraphAnalyzer|IncrementalCompilation|HybridCompilationIntegration|HybridCompilationBenchmark"
    DEPENDS JobSystemTests HotReloadPipelineTests AsyncHotReloadTests HotReloadBenchmark FileChangeTrackerTests DependencyGraphAnalyzerTests IncrementalCompilationTests HybridCompilationIntegrationTests HybridCompilationBenchmarkTests
    COMMENT "Running all async hot-reload tests"
)

# Custom target to run only compilation performance benchmarks
add_custom_target(run_compilation_benchmarks
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R "HotReloadPerformance|HybridCompilationBenchmark"
    DEPENDS HotReloadBenchmark HybridCompilationBenchmarkTests
    COMMENT "Running compilation performance benchmarks"
)

# Custom target for quick unit tests only
add_custom_target(run_job_system_tests
    COMMAND JobSystemTests
    DEPENDS JobSystemTests
    COMMENT "Running job system unit tests"
)

# Custom target for integration tests only
add_custom_target(run_integration_tests
    COMMAND AsyncHotReloadTests
    DEPENDS AsyncHotReloadTests
    COMMENT "Running async hot-reload integration tests"
)

# Custom target for performance benchmarks only
add_custom_target(run_benchmarks
    COMMAND HotReloadBenchmark
    DEPENDS HotReloadBenchmark
    COMMENT "Running hot-reload performance benchmarks"
)

# Install test executables (optional)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    install(TARGETS JobSystemTests AsyncHotReloadTests HotReloadBenchmark
        RUNTIME DESTINATION bin/tests
    )
endif()

# Add memory leak detection for Debug builds
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND WIN32)
    target_compile_definitions(JobSystemTests PRIVATE _CRTDBG_MAP_ALLOC)
    target_compile_definitions(HotReloadPipelineTests PRIVATE _CRTDBG_MAP_ALLOC)
    target_compile_definitions(AsyncHotReloadTests PRIVATE _CRTDBG_MAP_ALLOC)
endif()

# Add thread sanitizer for supported compilers
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    option(ENABLE_THREAD_SANITIZER "Enable thread sanitizer" OFF)
    if(ENABLE_THREAD_SANITIZER)
        target_compile_options(JobSystemTests PRIVATE -fsanitize=thread)
        target_link_options(JobSystemTests PRIVATE -fsanitize=thread)

        target_compile_options(AsyncHotReloadTests PRIVATE -fsanitize=thread)
        target_link_options(AsyncHotReloadTests PRIVATE -fsanitize=thread)
    endif()
endif()

# Add address sanitizer for supported compilers
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    option(ENABLE_ADDRESS_SANITIZER "Enable address sanitizer" OFF)
    if(ENABLE_ADDRESS_SANITIZER)
        target_compile_options(JobSystemTests PRIVATE -fsanitize=address)
        target_link_options(JobSystemTests PRIVATE -fsanitize=address)

        target_compile_options(AsyncHotReloadTests PRIVATE -fsanitize=address)
        target_link_options(AsyncHotReloadTests PRIVATE -fsanitize=address)
    endif()
endif()

# Coverage reporting (for GCC/Clang)
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    option(ENABLE_COVERAGE "Enable coverage reporting" OFF)
    if(ENABLE_COVERAGE)
        target_compile_options(JobSystemTests PRIVATE --coverage)
        target_link_options(JobSystemTests PRIVATE --coverage)

        target_compile_options(AsyncHotReloadTests PRIVATE --coverage)
        target_link_options(AsyncHotReloadTests PRIVATE --coverage)
    endif()
endif()

# Print test configuration
message(STATUS "Async Hot-Reload Tests Configuration:")
message(STATUS "  Job System Tests: JobSystemTests")
message(STATUS "  Hot-Reload Pipeline Tests: HotReloadPipelineTests")

# Output layout: bin/<Config>/Tests for all Jobs executables, and stage runtime DLL deps.
set(_GE_JOBS_TEST_TARGETS
    JobSystemTests
    HotReloadPipelineTests
    AsyncHotReloadTests
    HotReloadBenchmark
    FileChangeTrackerTests
    DependencyGraphAnalyzerTests
    IncrementalCompilationTests
    HybridCompilationIntegrationTests
    HybridCompilationBenchmarkTests
)

foreach(_ge_t IN LISTS _GE_JOBS_TEST_TARGETS)
    if(TARGET ${_ge_t})
        if(_ge_t STREQUAL "HotReloadPipelineTests")
            # This exe needs the no-op GameEngine.NativeShim staged beside it
            # under the real DLL's name (CoreBridge probes GameEngine.Native.dll
            # from the executable directory). In the shared Tests dir that copy
            # shadows the real GameEngine.Native.dll for every other test exe
            # importing newer ABI exports (0xc0000139 at process start), so this
            # one target gets an isolated directory.
            ge_set_target_runtime_output_dir(${_ge_t} "Tests/HotReloadPipeline")
        else()
            ge_set_output_tests(${_ge_t})
        endif()
        ge_stage_runtime_dependencies(${_ge_t})
        if(TARGET StageTestAssets)
            add_dependencies(${_ge_t} StageTestAssets)
        endif()
    endif()
endforeach()

# Registered after ge_stage_runtime_dependencies on purpose: POST_BUILD steps
# run in registration order, and the dependency stager can resolve the exe's
# GameEngine.Native import to the real DLL in bin/<config>. The shim copy must
# have the last word inside this exe's isolated directory.
add_custom_command(TARGET HotReloadPipelineTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:GameEngine.NativeShim>
        $<TARGET_FILE_DIR:HotReloadPipelineTests>/GameEngine.Native.dll
    COMMENT "Copying GameEngine.Native shim to HotReloadPipelineTests output")
message(STATUS "  Integration Tests: AsyncHotReloadTests")
message(STATUS "  Performance Benchmarks: HotReloadBenchmark")
message(STATUS "  Run all tests: make run_async_hotreload_tests")
message(STATUS "  Thread Sanitizer: ${ENABLE_THREAD_SANITIZER}")
message(STATUS "  Address Sanitizer: ${ENABLE_ADDRESS_SANITIZER}")
message(STATUS "  Coverage: ${ENABLE_COVERAGE}")
