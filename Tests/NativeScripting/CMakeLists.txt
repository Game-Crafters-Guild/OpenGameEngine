# Cross-DLL registration smoke test — native-scripting (Phase 2) prerequisite.
#
# Proves the engine's component reflection registries (ComponentFieldRegistry,
# ComponentFactory) are a single process-wide instance shared across the host
# EXE, Engine.dll, and a separately-linked user DLL. This is the documented #1
# risk for the native C++ scripting DLL loader: if the function-local-static
# registries were cloned per linking unit, a user DLL's registrations would be
# invisible to the engine. See CrossDllRegistrationSmokeTest.cpp.

find_package(GTest REQUIRED)

add_executable(NativePostSimulationTests NativePostSimulationTests.cpp)
target_link_libraries(NativePostSimulationTests PRIVATE Engine Mathematics Types GTest::gtest GTest::gtest_main)
target_include_directories(NativePostSimulationTests PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/Include
    ${CMAKE_SOURCE_DIR}/Engine/Modules/NativeScripting/GameSDK/Include)
ge_set_output_tests(NativePostSimulationTests)
ge_stage_runtime_dependencies(NativePostSimulationTests)
add_test(NAME NativePostSimulationTests COMMAND $<TARGET_FILE:NativePostSimulationTests>)

# dotnet + the ComponentScanner DLL feed the C10 scan-path test (a macro-free
# fixture auto-detected by the scanner). DOTNET_EXECUTABLE is already cached by the
# engine build (the scanner is mandatory there); re-find defensively in case this
# subdir is configured standalone. The scanner DLL is built transitively when the
# test links Engine (Engine's generated reflection TU depends on the scanner).
find_program(DOTNET_EXECUTABLE dotnet)

# ---------------------------------------------------------------------------
# Probe DLL — stand-in for a Phase-2 user module.
#
# Links PRIVATE the Engine IMPORT LIB (Engine.lib / Engine.dll), NOT the ECS
# object code. That is the invariant under test: registry statics stay
# single-instance only because the registry methods resolve to Engine.dll's
# exports rather than being recompiled (and re-instantiated) into this DLL.
#
# Mathematics + Types are header-only INTERFACE libs that Engine deliberately
# does NOT re-export through its INTERFACE. They supply the include dirs that
# Components/ComponentRegistration.h needs transitively
# (MathReflectionTraits.h -> Mathematics/*, Reflection.h -> Types/Color.h).
# Linking INTERFACE libs adds include dirs only — no object code, no clone.
# ---------------------------------------------------------------------------
add_library(NativeScriptingProbe SHARED ProbeComponent.cpp)
set_target_properties(NativeScriptingProbe PROPERTIES UNITY_BUILD OFF)
target_link_libraries(NativeScriptingProbe PRIVATE Engine Mathematics Types)
target_include_directories(NativeScriptingProbe PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Include)

# ---------------------------------------------------------------------------
# gtest host — LoadLibrary's the probe at runtime and asserts shared-registry.
#
# Links PRIVATE Engine (the SAME import lib) like the other native smoke tests.
# Types supplies Reflection.h's Types/Color.h include (pulled by the registry
# headers); the host needs no Mathematics (it never includes the math traits).
# ---------------------------------------------------------------------------
add_executable(CrossDllRegistrationSmokeTest CrossDllRegistrationSmokeTest.cpp)
set_target_properties(CrossDllRegistrationSmokeTest PROPERTIES UNITY_BUILD OFF)
target_link_libraries(CrossDllRegistrationSmokeTest PRIVATE
    Engine Types GTest::gtest GTest::gtest_main)
target_include_directories(CrossDllRegistrationSmokeTest PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/Include)

ge_set_output_tests(CrossDllRegistrationSmokeTest)
ge_stage_runtime_dependencies(CrossDllRegistrationSmokeTest)

# The host resolves the probe by a path next to its OWN exe (not the cwd), so the
# probe must be staged there. It is loaded dynamically, not linked, so it is not
# picked up by ge_stage_runtime_dependencies — copy it explicitly.
add_dependencies(CrossDllRegistrationSmokeTest NativeScriptingProbe)
add_custom_command(TARGET CrossDllRegistrationSmokeTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:NativeScriptingProbe>
        $<TARGET_FILE_DIR:CrossDllRegistrationSmokeTest>/$<TARGET_FILE_NAME:NativeScriptingProbe>
    COMMENT "Staging NativeScriptingProbe next to the cross-DLL smoke test")

add_test(NAME CrossDllRegistrationSmokeTest
    COMMAND $<TARGET_FILE:CrossDllRegistrationSmokeTest>)
set_tests_properties(CrossDllRegistrationSmokeTest PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ---------------------------------------------------------------------------
# Cross-image ownership of UI event-handler callables.
#
# Reuses the probe DLL above as the stand-in for a native user module: the point
# is that it is a SEPARATELY LINKED image, so the header-inline
# RegisterEventHandler it calls is compiled into IT, and the std::function that
# produces is that image's code. Proves an engine-side resolver can tell a
# module-owned callable from an engine-owned one — the mechanism the unload-time
# handler revocation rests on.
#
# Windows-only: the proof is a PE one. It reads an image's span out of the
# mapped IMAGE_NT_HEADERS (SizeOfImage), resolves Engine.dll by module name to
# dodge the import-thunk aliasing trap, and treats an image as ONE contiguous
# address range. A Mach-O/ELF image is a set of segments, and a dylib exports
# real symbols rather than thunks, so the equivalent proof is a different
# construction, not a dladdr substitution. The fixture's SetUpTestSuite is a
# no-op off Windows, so building it elsewhere yields a binary whose every test
# passes against zeroed ranges.
# ---------------------------------------------------------------------------
if(WIN32)
    # Real DLL rejection gates; fixtures have no engine registrations so this
    # suite isolates diagnostic and image-lifecycle ordering from rollback.
    foreach(probe IN ITEMS RejectedModuleProbe RejectedModuleMissingAbi RejectedModuleMissingFingerprint)
        add_library(${probe} SHARED RejectedModuleProbe.cpp)
        set_target_properties(${probe} PROPERTIES UNITY_BUILD OFF)
        target_include_directories(${probe} PRIVATE
            ${CMAKE_SOURCE_DIR}/Engine/Modules/NativeScripting/Include)
    endforeach()
    target_compile_definitions(RejectedModuleMissingAbi PRIVATE GE_PROBE_MISSING_ABI)
    target_compile_definitions(RejectedModuleMissingFingerprint PRIVATE GE_PROBE_MISSING_FINGERPRINT)

    add_executable(NativeModuleRejectionTests NativeModuleRejectionTests.cpp)
    set_target_properties(NativeModuleRejectionTests PROPERTIES UNITY_BUILD OFF)
    target_link_libraries(NativeModuleRejectionTests PRIVATE Engine Types GTest::gtest GTest::gtest_main)
    target_include_directories(NativeModuleRejectionTests PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Include)
    ge_set_output_tests(NativeModuleRejectionTests)
    ge_stage_runtime_dependencies(NativeModuleRejectionTests)
    foreach(probe IN ITEMS RejectedModuleProbe RejectedModuleMissingAbi RejectedModuleMissingFingerprint)
        add_dependencies(NativeModuleRejectionTests ${probe})
        add_custom_command(TARGET NativeModuleRejectionTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${probe}>
                $<TARGET_FILE_DIR:NativeModuleRejectionTests>/$<TARGET_FILE_NAME:${probe}>)
    endforeach()
    add_test(NAME NativeModuleRejectionTests COMMAND $<TARGET_FILE:NativeModuleRejectionTests>)
    set_tests_properties(NativeModuleRejectionTests PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

    add_executable(NativeModuleImageOwnershipTests NativeModuleImageOwnershipTests.cpp)
    set_target_properties(NativeModuleImageOwnershipTests PROPERTIES UNITY_BUILD OFF)
    target_link_libraries(NativeModuleImageOwnershipTests PRIVATE
        Engine Types Mathematics GTest::gtest GTest::gtest_main)
    target_include_directories(NativeModuleImageOwnershipTests PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Include)

    ge_set_output_tests(NativeModuleImageOwnershipTests)
    ge_stage_runtime_dependencies(NativeModuleImageOwnershipTests)

    # Loaded dynamically by path next to this exe, so staging is explicit (the same
    # reason CrossDllRegistrationSmokeTest copies it).
    add_dependencies(NativeModuleImageOwnershipTests NativeScriptingProbe)
    add_custom_command(TARGET NativeModuleImageOwnershipTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:NativeScriptingProbe>
            $<TARGET_FILE_DIR:NativeModuleImageOwnershipTests>/$<TARGET_FILE_NAME:NativeScriptingProbe>
        COMMENT "Staging NativeScriptingProbe next to the image-ownership tests")

    add_test(NAME NativeModuleImageOwnershipTests
        COMMAND $<TARGET_FILE:NativeModuleImageOwnershipTests>)
    set_tests_properties(NativeModuleImageOwnershipTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

    # Queued log records outliving their producer image, through the real loader:
    # the probe DLL logs, NativeScriptManager rejects and unmaps it, the drain
    # then dispatches. The probe source is shared with the Logger module's own
    # fixture, which proves the same thing without an Engine dependency.
    add_library(NativeLoggerModuleProbe SHARED
        ${CMAKE_SOURCE_DIR}/Engine/Modules/Logger/Tests/LoggerModuleProbe.cpp)
    set_target_properties(NativeLoggerModuleProbe PROPERTIES UNITY_BUILD OFF)
    target_link_libraries(NativeLoggerModuleProbe PRIVATE Engine)

    add_executable(NativeModuleLoggerTests NativeModuleLoggerTests.cpp)
    set_target_properties(NativeModuleLoggerTests PROPERTIES UNITY_BUILD OFF)
    target_link_libraries(NativeModuleLoggerTests PRIVATE Engine Types GTest::gtest_main)
    target_include_directories(NativeModuleLoggerTests PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Include
        ${CMAKE_SOURCE_DIR}/Engine/Modules/Logger/Tests)
    ge_set_output_tests(NativeModuleLoggerTests)
    ge_stage_runtime_dependencies(NativeModuleLoggerTests)
    add_dependencies(NativeModuleLoggerTests NativeLoggerModuleProbe)
    add_custom_command(TARGET NativeModuleLoggerTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:NativeLoggerModuleProbe>
            $<TARGET_FILE_DIR:NativeModuleLoggerTests>/$<TARGET_FILE_NAME:NativeLoggerModuleProbe>)
    add_test(NAME NativeModuleLoggerTests COMMAND $<TARGET_FILE:NativeModuleLoggerTests>)
    set_tests_properties(NativeModuleLoggerTests PROPERTIES TIMEOUT 60 LABELS "Logger;NativeScripting;DLL")
endif()

# ---------------------------------------------------------------------------
# NativeScriptManager unit tests (C9) — deterministic lifecycle / classification.
# Links PRIVATE Engine; NativeScriptManager lives in Engine.dll.
# ---------------------------------------------------------------------------
add_executable(NativeScriptManagerTests
    NativeScriptManagerTests.cpp
    # The module build and the toolchain detection run on their job channels: parked, they
    # hold no compute worker.
    NativeBuildChannelTests.cpp
    # Ship-safety C1: packaged-game staging of the native user module (BuildPipeline side).
    NativeScriptStagingTests.cpp
    # P2 package build staging: per-package native/managed staging, packages.index,
    # and the loud build-validation matrix — against fake staged dirs.
    PackageBuildStagingTests.cpp
    # BuildPipeline::Execute against an in-flight editor module build (InFlightModuleBuild.h):
    # the step-0 wait, a failed module build, and a cancel requested before Execute.
    BuildPipelineNativeModuleWaitTests.cpp
    # P2.2 packaged-mode detection (game.config + Assets/.assetmanifest) and the
    # Player ScriptsConfig it produces.
    PackagedGameLayoutTests.cpp
    # Ship-safety C5: curated runtime dependency staging (replaces the ship-everything glob).
    RuntimeDependencyStagingTests.cpp
    # Packaged-C# dotnet preflight: SDK-manifest-first discovery, never a bare PATH spawn.
    DotnetPreflightTests.cpp
    # Persisted user-project CMakeLists shape (#370): SDK paths through GAMEENGINE_SDK_DIR,
    # refreshed last-known-good default, dead-SDK regeneration.
    UserProjectGeneratorTests.cpp
    # Reflection-driven scene serialization (user/native components round-trip via GE_REFLECT
    # with no hand-written schema) — thematically a native-scripting concern; lives here because
    # this target links the Engine import lib cleanly.
    ReflectionSceneSerializationTests.cpp
    # Package pilot: late-registered engine plugins (package/user DLLs loaded at
    # project open) get their missed startup hooks replayed via the registry's
    # late-registration handler.
    EnginePluginLateRegistrationTests.cpp
    # Player build-configuration contract: one table behind the settings UI, the
    # build request, the CRT-flavour check and the NativeAOT gate.
    PlayerBuildConfigTests.cpp
    # The generated NativeAOT project's language settings match the live compile's.
    NativeAotProjectTests.cpp
    # Which targets each editor host builds, and the refusal naming the fix per host.
    BuildHostTests.cpp
    # Linux runtime template checks: the x64 ELF Player and the one list of game files
    # a template export leaves behind.
    LinuxRuntimeTemplateTests.cpp)
set_target_properties(NativeScriptManagerTests PROPERTIES UNITY_BUILD OFF)
# PlayerBuildConfigTests asserts per-configuration truths, so it needs the one
# source that can name this binary's configuration. Preprocessor state cannot:
# DebugFast defines neither _DEBUG nor NDEBUG. Same definition the Editor gets.
target_compile_definitions(NativeScriptManagerTests PRIVATE GE_BUILD_CONFIG="$<CONFIG>")
# Mathematics + Types are header-only INTERFACE libs Engine does not re-export; the reflection
# test needs their headers (Vector3/Quaternion/ColorLinear field types via MathReflectionTraits).
target_link_libraries(NativeScriptManagerTests PRIVATE
    Engine Mathematics Types GTest::gtest GTest::gtest_main)
target_include_directories(NativeScriptManagerTests PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/Include
    # GltfTestFiles.h: in-memory glTF clips for AnimatorEventsAbiTests.
    ${CMAKE_SOURCE_DIR}/Engine/Tests)

# D1: blob (C#) components registered with a field schema through the scripting ABI —
# scene round-trip, hot-reload layout migration, snapshot round-trip. The ABI entry points
# are exported by GameEngine.Native (not Engine), so this piece needs the scripting shim;
# both DLLs link Engine SHARED, so they operate on the same process-wide registries.
# EnableStateAbiTests drives the component switch the managed EntityCommands.SetEnabled uses,
# through the same exports; AnimatorEventsAbiTests the event poll AnimatorApi.PollEvents uses.
if(TARGET GameEngine.Native)
    target_sources(NativeScriptManagerTests PRIVATE BlobComponentSchemaAbiTests.cpp EnableStateAbiTests.cpp
        AnimatorEventsAbiTests.cpp AnimatorInputAbiTests.cpp)
    target_link_libraries(NativeScriptManagerTests PRIVATE GameEngine.Native)
endif()

# A dependency-free Mach-O dylib for MacExportNativeModuleIsWhereThePlayerLoadsIt and
# MacBundleAssemblerTests: the macOS exporter runs install_name_tool on every native module it
# ships, which needs a real image with header room (CMake links Apple shared libraries with
# -headerpad_max_install_names).
if(APPLE)
    add_library(MacBundleFixtureDylib SHARED MacBundleFixtureDylib.cpp)
    add_dependencies(NativeScriptManagerTests MacBundleFixtureDylib)
    target_compile_definitions(NativeScriptManagerTests PRIVATE
        GE_MAC_BUNDLE_FIXTURE_DYLIB="$<TARGET_FILE:MacBundleFixtureDylib>")

    # The same image built universal (arm64 and x86_64) for MacBundleAssemblerTests: otool lists
    # the load commands of every slice, so a universal module repeats each rpath.
    add_library(MacBundleUniversalFixtureDylib SHARED MacBundleFixtureDylib.cpp)
    set_target_properties(MacBundleUniversalFixtureDylib PROPERTIES OSX_ARCHITECTURES "arm64;x86_64")
endif()

ge_set_output_tests(NativeScriptManagerTests)
ge_stage_runtime_dependencies(NativeScriptManagerTests)

add_test(NAME NativeScriptManagerTests
    COMMAND $<TARGET_FILE:NativeScriptManagerTests>)
set_tests_properties(NativeScriptManagerTests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ---------------------------------------------------------------------------
# C10 end-to-end: generate + cmake-compile + load a real user component DLL and
# assert it registers in the engine's shared registries. Heavyweight (shells out
# to cmake + MSVC), so a longer timeout. The user DLL builds in its OWN build dir
# (workdir below), never the engine build dir, so it doesn't contend with it.
# ---------------------------------------------------------------------------
add_executable(UserModuleBuildLoadTest UserModuleBuildLoadTest.cpp)
set_target_properties(UserModuleBuildLoadTest PROPERTIES UNITY_BUILD OFF)
target_link_libraries(UserModuleBuildLoadTest PRIVATE
    Engine Types GTest::gtest GTest::gtest_main)
target_include_directories(UserModuleBuildLoadTest PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/Include)

ge_set_output_tests(UserModuleBuildLoadTest)
ge_stage_runtime_dependencies(UserModuleBuildLoadTest)

# Hand the engine build interface to the test via a generated key=value file placed
# next to the test exe (a generated file avoids string-literal escaping of Windows
# paths, and per-config exe dirs keep each config's paths separate). The genex
# values capture EXACTLY the engine's include/link/define interface a consumer needs.
set(_c10_sdk_inc "${CMAKE_SOURCE_DIR}/Engine/Modules/NativeScripting/GameSDK/Include")
set(_c10_sdk_entry "${CMAKE_SOURCE_DIR}/Engine/Modules/NativeScripting/GameSDK/Source/UserModuleEntry.cpp")
file(GENERATE
    OUTPUT "$<TARGET_FILE_DIR:UserModuleBuildLoadTest>/c10_config.txt"
    CONTENT
"cmake=${CMAKE_COMMAND}
importlib=$<TARGET_LINKER_FILE:Engine>
config=$<CONFIG>
sdkinc=${_c10_sdk_inc}
sdkentry=${_c10_sdk_entry}
workdir=${CMAKE_BINARY_DIR}/Tests/NativeScripting/c10work
includes=$<TARGET_PROPERTY:Engine,INTERFACE_INCLUDE_DIRECTORIES>
defs=$<TARGET_PROPERTY:Engine,INTERFACE_COMPILE_DEFINITIONS>
globaldefs=GLM_FORCE_DEPTH_ZERO_TO_ONE;NOMINMAX;WIN32_LEAN_AND_MEAN;GE_DEBUG_INSTRUMENTATION=${GE_DEBUG_INSTRUMENTATION_VALUE}
dotnet=${DOTNET_EXECUTABLE}
scannerdll=${CMAKE_SOURCE_DIR}/Managed/ComponentScanner/bin/$<CONFIG>/net10.0/ComponentScanner.dll
")

add_test(NAME UserModuleBuildLoadTest
    COMMAND $<TARGET_FILE:UserModuleBuildLoadTest>)
set_tests_properties(UserModuleBuildLoadTest PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    TIMEOUT 480)

# ---------------------------------------------------------------------------
# NativeAOT script publish: a real `dotnet publish /p:NativeLib=Shared` of a
# project named like BuildPipeline's generated one, checked for the library
# name the pipeline copies. Heavyweight (ILCompiler plus the platform linker; a
# cold run restores the ILCompiler packages), so a test of its own with a longer
# timeout. It publishes into a temp directory, never the build tree.
# ---------------------------------------------------------------------------
add_executable(NativeAotPublishTest NativeAotPublishTest.cpp)
set_target_properties(NativeAotPublishTest PROPERTIES UNITY_BUILD OFF)
target_link_libraries(NativeAotPublishTest PRIVATE Engine GTest::gtest GTest::gtest_main)
target_include_directories(NativeAotPublishTest PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Include)
ge_set_output_tests(NativeAotPublishTest)
ge_stage_runtime_dependencies(NativeAotPublishTest)
add_test(NAME NativeAotPublishTest COMMAND $<TARGET_FILE:NativeAotPublishTest>)
set_tests_properties(NativeAotPublishTest PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    TIMEOUT 600)
