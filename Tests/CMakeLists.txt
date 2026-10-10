# Top-level Tests harness: gathers all test subdirectories when BUILD_TESTING is ON.
# This file is included from the root CMakeLists via add_subdirectory(Tests).

# Benchmark / stress reference scenes (Tests/Scenes/*). Each subproject defines
# a Fetch* custom target that downloads its source assets from the Khronos
# sample repo on demand. The scene files themselves live in source control;
# the .gltf / .glb files do not (per each scene's .gitignore).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Scenes/CMakeLists.txt)
    add_subdirectory(Scenes)
endif()


# Note: handle-system and resource-manager validation lives in the Rendering
# module's ResourceHandleManagerTests target (Engine/Modules/Rendering/Tests).

# Many native tests assume `ApplicationConfig.assetDirectory = "Assets"` and run
# with WORKING_DIRECTORY = ${CMAKE_BINARY_DIR}. Stage the repo Assets into the
# per-preset build root so these tests remain hermetic (and we avoid global
# configure-time copies in the top-level CMakeLists).
#
# Test FIXTURES are staged the same way, mirroring their repo-relative paths
# under the build root; suites resolve them exe-anchored via
# Engine/Tests/StagedTestPaths.h (StagedRoot()) instead of climbing from the
# CWD or __FILE__ into the source tree. Fixture roots staged here:
#   Assets/                            asset-loading suites (also carries the
#                                      optional, gitignored Models/FBXTest .fbx
#                                      payload when a dev machine has it)
#   Engine/Tests/Fixtures/             authored-LOD glTF/FBX fixtures
#   Engine/Modules/Rendering/Shaders/  real-GLSL material compile tests
#   Tests/Projects/Biggles/Assets/Scenes/  SceneIO real-scene fixture
#   Tools/Web/smoke-project/Assets/Scenes/ web seed-project scenes
#   Tools/Web/smoke-project/Assets/Particles/ the particle stack they reference
#   Tests/Assets/TestData/BigglesCaseShadow.assetdb  frozen pre-migration journal
if(EXISTS ${CMAKE_SOURCE_DIR}/Assets)
  # Each mirror is copy + orphan prune: copy_directory_if_different never
  # deletes, so without the prune a staged mirror keeps files whose sources
  # were deleted from the repo and suites keep passing against assets a fresh
  # tree does not have. The prune removes only files the mirror itself staged
  # (cmake/PruneStagedMirrorOrphans.cmake), so runtime state tests write into
  # the staged root — script assemblies, derived cache, logs — is never touched.
  add_custom_target(StageTestAssets
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${CMAKE_SOURCE_DIR}/Assets
            ${CMAKE_BINARY_DIR}/Assets
    COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_SOURCE_DIR}/Assets
            -DDST=${CMAKE_BINARY_DIR}/Assets
            -P ${CMAKE_SOURCE_DIR}/cmake/PruneStagedMirrorOrphans.cmake
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${CMAKE_SOURCE_DIR}/Engine/Tests/Fixtures
            ${CMAKE_BINARY_DIR}/Engine/Tests/Fixtures
    COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_SOURCE_DIR}/Engine/Tests/Fixtures
            -DDST=${CMAKE_BINARY_DIR}/Engine/Tests/Fixtures
            -P ${CMAKE_SOURCE_DIR}/cmake/PruneStagedMirrorOrphans.cmake
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders
            ${CMAKE_BINARY_DIR}/Engine/Modules/Rendering/Shaders
    COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders
            -DDST=${CMAKE_BINARY_DIR}/Engine/Modules/Rendering/Shaders
            -P ${CMAKE_SOURCE_DIR}/cmake/PruneStagedMirrorOrphans.cmake
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${CMAKE_SOURCE_DIR}/Engine/Modules/GPUFogParticles/Shaders
            ${CMAKE_BINARY_DIR}/Engine/Modules/GPUFogParticles/Shaders
    COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_SOURCE_DIR}/Engine/Modules/GPUFogParticles/Shaders
            -DDST=${CMAKE_BINARY_DIR}/Engine/Modules/GPUFogParticles/Shaders
            -P ${CMAKE_SOURCE_DIR}/cmake/PruneStagedMirrorOrphans.cmake
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${CMAKE_SOURCE_DIR}/Tests/Projects/Biggles/Assets/Scenes
            ${CMAKE_BINARY_DIR}/Tests/Projects/Biggles/Assets/Scenes
    COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_SOURCE_DIR}/Tests/Projects/Biggles/Assets/Scenes
            -DDST=${CMAKE_BINARY_DIR}/Tests/Projects/Biggles/Assets/Scenes
            -P ${CMAKE_SOURCE_DIR}/cmake/PruneStagedMirrorOrphans.cmake
    # The web seed project's scenes: ShippedSceneExposureTests reads the
    # shipped bytes, and only the scenes — the rest of the seed project is
    # payload the suite never opens.
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${CMAKE_SOURCE_DIR}/Tools/Web/smoke-project/Assets/Scenes
            ${CMAKE_BINARY_DIR}/Tools/Web/smoke-project/Assets/Scenes
    COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_SOURCE_DIR}/Tools/Web/smoke-project/Assets/Scenes
            -DDST=${CMAKE_BINARY_DIR}/Tools/Web/smoke-project/Assets/Scenes
            -P ${CMAKE_SOURCE_DIR}/cmake/PruneStagedMirrorOrphans.cmake
    # ...and the particle stack those scenes reference, which SceneIOTests reads.
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${CMAKE_SOURCE_DIR}/Tools/Web/smoke-project/Assets/Particles
            ${CMAKE_BINARY_DIR}/Tools/Web/smoke-project/Assets/Particles
    COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_SOURCE_DIR}/Tools/Web/smoke-project/Assets/Particles
            -DDST=${CMAKE_BINARY_DIR}/Tools/Web/smoke-project/Assets/Particles
            -P ${CMAKE_SOURCE_DIR}/cmake/PruneStagedMirrorOrphans.cmake
    # The Biggles journal as it stood BEFORE the case-shadow fix, frozen
    # byte-identically: the live journal is migrated, so the migration suite
    # needs this pre-migration copy to have anything to prove. One file, not
    # the project: the model payload is 5 MB the suite never opens.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/Tests/Assets/TestData/BigglesCaseShadow.assetdb
            ${CMAKE_BINARY_DIR}/Tests/Assets/TestData/BigglesCaseShadow.assetdb
    # One file, not the directory: the VideoTextureSystem lifetime suite needs a
    # real decodable source, and the .mov sibling is 8 MB of nothing it uses.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/Sample/logo.mp4
            ${CMAKE_BINARY_DIR}/Apps/Editor/Assets/Sample/logo.mp4
    COMMENT "Staging repo Assets + test fixtures into the test build root"
  )
  # Optional, gitignored dev fixtures (hand-extracted .blend samples). Guarded
  # at configure time: extract them, reconfigure, and the next build stages
  # them; the blend suites GTEST_SKIP when the staged copy is absent.
  if(EXISTS ${CMAKE_SOURCE_DIR}/Tests/BlendSamples)
    add_custom_command(TARGET StageTestAssets POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
              ${CMAKE_SOURCE_DIR}/Tests/BlendSamples
              ${CMAKE_BINARY_DIR}/Tests/BlendSamples
      COMMAND ${CMAKE_COMMAND}
              -DSRC=${CMAKE_SOURCE_DIR}/Tests/BlendSamples
              -DDST=${CMAKE_BINARY_DIR}/Tests/BlendSamples
              -P ${CMAKE_SOURCE_DIR}/cmake/PruneStagedMirrorOrphans.cmake
      COMMENT "Staging dev .blend samples (present on this machine)")
  endif()

  # Fixture-consuming suites defined in earlier subdirectories (Engine/Tests,
  # Engine/Modules/Graph/Tests, Engine/Modules/Rendering/Tests). Wired centrally
  # here: this file is processed after them and only under BUILD_TESTING, where
  # StageTestAssets exists. Suites in Tests/* subdirectories are added after this
  # point, so those wire the dependency in their own CMakeLists.
  foreach(_ge_fixture_test
      MeshLODGeneratorTests
      EngineSceneIOTests
      EngineMaterialTests
      EngineTextureTests
      EngineRenderServicesTests
      EngineWorldDrawTests
      RenderPipelineDeclareTests
      MaterialGraphCompilerTests
      BlendSmokeParseTests
      BlendModelLoadTests
      BlendActionExtractTests
      BlenderToFbxRetargetTest
      ModelAssetHumanoidPostLoadTests
      HumanoidRetargetRealAssetTests
      FbxAxisConversionTests
      GltfImportTests
      EditorSkinnedSpawnTests
      ShaderVariantKeyTests)
    if(TARGET ${_ge_fixture_test})
      add_dependencies(${_ge_fixture_test} StageTestAssets)
    endif()
  endforeach()
endif()

# The engine version is read from the root VERSION file; the configured string and the
# generated header must be what the file holds, and a malformed file is refused.
add_test(NAME EngineVersion.ConfiguredFromTheVersionFile
  COMMAND ${CMAKE_COMMAND}
          -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/EngineVersion.cmake
          -DVERSION_FILE=${CMAKE_SOURCE_DIR}/VERSION
          -DCONFIGURED=${GE_ENGINE_VERSION}
          -DCONFIGURED_NUMERIC=${GE_ENGINE_VERSION_NUMERIC}
          -DHEADER=${CMAKE_BINARY_DIR}/generated/Core/EngineVersion.h
          -DWORK=${CMAKE_BINARY_DIR}/Tests/EngineVersionScratch
          -P ${CMAKE_SOURCE_DIR}/cmake/Tests/EngineVersionTest.cmake)

# The mirror prune is destructive build logic with an ownership contract; this
# exercises it against a scratch mirror, without a build.
add_test(NAME PruneStagedMirrorOrphans.MirrorOwnership
  COMMAND ${CMAKE_COMMAND}
          -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/PruneStagedMirrorOrphans.cmake
          -DWORK=${CMAKE_BINARY_DIR}/Tests/PruneStagedMirrorOrphansScratch
          -P ${CMAKE_SOURCE_DIR}/cmake/Tests/PruneStagedMirrorOrphansTest.cmake)

# Engine package staging is a mirror of Packages/: a package removed from
# the source leaves the staged tree, and what the staging did not take from the
# source survives. Exercised against scratch packages, without a build.
add_test(NAME StagePackages.RemovedPackageLeavesTheStagedTree
  COMMAND ${CMAKE_COMMAND}
          -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/StagePackages.cmake
          -DWORK=${CMAKE_BINARY_DIR}/Tests/StagePackagesScratch
          -P ${CMAKE_SOURCE_DIR}/cmake/Tests/StagePackagesTest.cmake)

# Complete notice texts survive staging for transitive ports and vendored components.
add_test(NAME StageThirdPartyNotices.CompleteTexts
  COMMAND ${CMAKE_COMMAND}
          -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/StageThirdPartyNotices.cmake
          -DSOURCE=${CMAKE_SOURCE_DIR}
          -DWORK=${CMAKE_BINARY_DIR}/Tests/StageThirdPartyNoticesScratch
          -P ${CMAKE_SOURCE_DIR}/cmake/Tests/StageThirdPartyNoticesTest.cmake)

# Managed notice files, including RTF and source records, retain their exact bytes.
add_test(NAME StageManagedNotices.CompleteTexts
  COMMAND ${CMAKE_COMMAND}
          -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/StageThirdPartyNotices.cmake
          -DSOURCE=${CMAKE_SOURCE_DIR}
          -DWORK=${CMAKE_BINARY_DIR}/Tests/StageManagedNoticesScratch
          -P ${CMAKE_SOURCE_DIR}/cmake/Tests/StageManagedNoticesTest.cmake)

# The prebuilt overlay copies module binaries onto the staged packages; a file
# outside a fingerprint directory never replaces the package's authored file.
add_test(NAME OverlayEnginePackagePrebuilts.OnlyFingerprintDirectoriesOverlay
  COMMAND ${CMAKE_COMMAND}
          -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/OverlayEnginePackagePrebuilts.cmake
          -DWORK=${CMAKE_BINARY_DIR}/Tests/OverlayEnginePackagePrebuiltsScratch
          -P ${CMAKE_SOURCE_DIR}/cmake/Tests/OverlayEnginePackagePrebuiltsTest.cmake)

# The prebuilt staging names the module file with the loader's rule, whatever
# name this platform's CMake gives a MODULE library. Exercised against a scratch
# module file and the built GePrebuiltStamp tool.
if(TARGET GePrebuiltStamp)
  add_test(NAME StageEnginePackagePrebuilt.StagesTheLoaderModuleFileName
    COMMAND ${CMAKE_COMMAND}
            -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/StageEnginePackagePrebuilt.cmake
            -DWORK=${CMAKE_BINARY_DIR}/Tests/StageEnginePackagePrebuiltScratch
            -DSTAMP_TOOL=$<TARGET_FILE:GePrebuiltStamp>
            -DMODULE_PREFIX=${CMAKE_SHARED_MODULE_PREFIX}
            -DMODULE_SUFFIX=${CMAKE_SHARED_MODULE_SUFFIX}
            -P ${CMAKE_SOURCE_DIR}/cmake/Tests/StageEnginePackagePrebuiltTest.cmake)
endif()

# The engine_abi markers beside the Editor's prebuilt engine-package modules
# match the digest of its SDK as the build left it, so the editor loads the
# prebuilts instead of rebuilding them from source; on macOS the signed bundle
# still verifies with the markers in it. Needs the Editor built.
get_property(_ge_markers_modules_file GLOBAL PROPERTY GE_ENGINE_PACKAGE_MARKERS_MODULES_FILE)
if(TARGET Editor AND TARGET GePrebuiltStamp AND _ge_markers_modules_file)
  set(_ge_markers_bundle_arg "")
  if(APPLE)
    set(_ge_markers_bundle_arg -DAPP_BUNDLE=$<TARGET_BUNDLE_DIR:Editor>)
  endif()
  add_test(NAME EnginePackageMarkers.MatchTheEditorsFinalSdk
    COMMAND ${CMAKE_COMMAND}
            -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/StageEnginePackageMarkers.cmake
            -DWORK=${CMAKE_BINARY_DIR}/Tests/EnginePackageMarkersScratch
            -DSTAMP_TOOL=$<TARGET_FILE:GePrebuiltStamp>
            -DSDK_DIR=$<TARGET_FILE_DIR:Editor>/SDK
            -DPACKAGES_DIR=$<TARGET_FILE_DIR:Editor>/Packages
            -DMODULES_FILE=${_ge_markers_modules_file}
            ${_ge_markers_bundle_arg}
            -P ${CMAKE_SOURCE_DIR}/cmake/Tests/EnginePackageMarkersTest.cmake)
endif()

# The vcpkg install guard is the only thing that ties a build directory to the
# dependency set its objects were compiled against; this exercises all three
# refusals against a scratch install root, without a build.
add_test(NAME VcpkgInstallGuard.RefusesSharedForeignAndChangedInstalls
  COMMAND ${CMAKE_COMMAND}
          -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/VcpkgInstallGuard.cmake
          -DWORK=${CMAKE_BINARY_DIR}/Tests/VcpkgInstallGuardScratch
          -P ${CMAKE_SOURCE_DIR}/cmake/Tests/VcpkgInstallGuardTest.cmake)

# The clone-path guard stops a configure from a path the dependency builds
# cannot take before vcpkg runs; this configures scratch projects from a path
# with a space and one without, and checks Program Files paths by string.
add_test(NAME ClonePathGuard.RefusesSpacesAndProgramFiles
  COMMAND ${CMAKE_COMMAND}
          -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/ClonePathGuard.cmake
          -DWORK=${CMAKE_BINARY_DIR}/Tests/ClonePathGuardScratch
          -P ${CMAKE_SOURCE_DIR}/cmake/Tests/ClonePathGuardTest.cmake)

# The overlay-port freshness guard reads the same status database and decides
# whether a build links a patched port or the library that preceded it; this
# exercises it against a scratch install root, without a build.
add_test(NAME OverlayPortGuard.RefusesStaleOverlayPorts
  COMMAND ${CMAKE_COMMAND}
          -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/OverlayPortGuard.cmake
          -DWORK=${CMAKE_BINARY_DIR}/Tests/OverlayPortGuardScratch
          -P ${CMAKE_SOURCE_DIR}/cmake/Tests/OverlayPortGuardTest.cmake)

# The vcpkg pin decides which checkout a newly provisioned tree lands on, and so
# whether that tree reads the machine's binary cache at all; this exercises the
# checkout and the report against scratch git repositories, without a network.
add_test(NAME VcpkgPin.PinsAndReportsTheVcpkgCheckout
  COMMAND ${CMAKE_COMMAND}
          -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/VcpkgPin.cmake
          -DWORK=${CMAKE_BINARY_DIR}/Tests/VcpkgPinScratch
          -P ${CMAKE_SOURCE_DIR}/cmake/Tests/VcpkgPinTest.cmake)

# The nethost and hostfxr the build stages come from the newest installed .NET;
# this exercises the version ordering against scratch pack paths, without a build.
add_test(NAME DotnetHostVersions.NewestVersionFirst
  COMMAND ${CMAKE_COMMAND}
          -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/DotnetHostVersions.cmake
          -P ${CMAKE_SOURCE_DIR}/cmake/Tests/DotnetHostVersionsTest.cmake)

# The compiler vcpkg resolves is part of every package's ABI, and a configure
# the build system re-runs inherits the build system's PATH; this re-configures
# a scratch project with another compiler first on PATH and checks what a
# vcpkg-style compiler detection finds. vcpkg resolves MSVC without PATH, so
# there is nothing to hold on a Windows host.
if(NOT CMAKE_HOST_WIN32)
  add_test(NAME VcpkgHostCompiler.HoldsTheFirstConfiguresCompilers
    COMMAND ${CMAKE_COMMAND}
            -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/VcpkgHostCompiler.cmake
            -DWORK=${CMAKE_BINARY_DIR}/Tests/VcpkgHostCompilerScratch
            -P ${CMAKE_SOURCE_DIR}/cmake/Tests/VcpkgHostCompilerTest.cmake)
endif()

# The managed builds' source globs are re-checked before every build, and one
# that sees dotnet's obj/ or bin/ outputs re-runs CMake after every managed
# build; this builds a scratch project with this tree's generator against a
# scratch managed project.
add_test(NAME ManagedProjectSources.BuildOutputsDoNotRerunCMake
  COMMAND ${CMAKE_COMMAND}
          -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/ManagedProjectSources.cmake
          -DWORK=${CMAKE_BINARY_DIR}/Tests/ManagedProjectSourcesScratch
          "-DGENERATOR=${CMAKE_GENERATOR}"
          "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
          -P ${CMAKE_SOURCE_DIR}/cmake/Tests/ManagedProjectSourcesTest.cmake)

# The engine-package define derivation decides what every shipped prebuilt native
# module is compiled with and stamped as; a list that differs from the one the
# editor derives makes all of them unusable. This exercises the rule and its
# dependency ordering against scratch manifests, without a build.
add_test(NAME EnginePackageDefines.DerivesTheResolverDefineList
  COMMAND ${CMAKE_COMMAND}
          -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/EnginePackageDefines.cmake
          -DWORK=${CMAKE_BINARY_DIR}/Tests/EnginePackageDefinesScratch
          -P ${CMAKE_SOURCE_DIR}/cmake/Tests/EnginePackageDefinesTest.cmake)

# Engine.dll's export definition is regenerated before every MSVC link of it,
# and a rewritten definition relinks the 70 MB DLL; this exercises the rewrite
# rule against a scratch object set, without a build.
if(MSVC)
  add_test(NAME GenerateEngineExportsDef.RewritesOnlyAChangedDefinition
    COMMAND ${CMAKE_COMMAND}
            -DSCRIPT=${CMAKE_SOURCE_DIR}/cmake/GenerateEngineExportsDef.cmake
            -DWORK=${CMAKE_BINARY_DIR}/Tests/GenerateEngineExportsDefScratch
            -P ${CMAKE_SOURCE_DIR}/cmake/Tests/GenerateEngineExportsDefTest.cmake)
endif()

# An export compiles the Player against the Editor's staged SDK. An editor that
# runs outside its build tree has no vcpkg tree to name, so the Player must
# configure and link without one. Needs the Editor built (its SDK staged); runs
# a real configure and link of the SDK's Player template, about a minute.
if(TARGET Editor AND WIN32)
  add_test(NAME EngineSdk.PlayerLinksWithoutAVcpkgTree
    COMMAND ${CMAKE_COMMAND}
            -DSDK=$<TARGET_FILE_DIR:Editor>/SDK
            "-DGENERATOR=${CMAKE_GENERATOR}"
            -DWORK=${CMAKE_BINARY_DIR}/Tests/EngineSdkPlayerLinkScratch
            -P ${CMAKE_SOURCE_DIR}/cmake/Tests/EngineSdkPlayerLinkTest.cmake)
endif()

# Opt into a shader parity review against a pinned Git baseline. This is not a
# permanent no-change rule: main must remain free to improve native rendering,
# and an explicit baseline keeps squash merges from invalidating the control.
# Reports SKIPPED when the toolchain or the revision is absent.
find_package(Python3 COMPONENTS Interpreter QUIET)
set(GE_DESKTOP_SHADER_PARITY_BASELINE "" CACHE STRING
    "Git revision for optional desktop shader compatibility validation")
if(GE_DESKTOP_SHADER_PARITY_BASELINE AND Python3_Interpreter_FOUND AND GLSLC_EXECUTABLE)
  add_test(NAME DesktopSpirvParity.MatchesMain
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/Web/desktop_spirv_parity.py
            --repo ${CMAKE_SOURCE_DIR}
            --glslc ${GLSLC_EXECUTABLE}
            --baseline ${GE_DESKTOP_SHADER_PARITY_BASELINE})
  set_tests_properties(DesktopSpirvParity.MatchesMain PROPERTIES SKIP_RETURN_CODE 125)
endif()

# The host cook reads and writes packages only through ShaderReflect.
if(Python3_Interpreter_FOUND AND TARGET ShaderReflect AND NOT EMSCRIPTEN)
  add_test(NAME WebShaderPackageBounds
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/Web/test_shader_package_bounds.py
            --shaderpkg-dir ${CMAKE_BINARY_DIR}/Shaders
            --shader-reflect $<TARGET_FILE:ShaderReflect>)
  set_tests_properties(WebShaderPackageBounds PROPERTIES TIMEOUT 60)
endif()

# The host cook must pair WGSL with reflection from the same source profile.
if(Python3_Interpreter_FOUND AND TARGET ShaderReflect AND NOT EMSCRIPTEN)
  add_test(NAME WebBuiltinShaderMetadata
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/Web/test_builtin_shader_metadata.py
            --shaderpkg-dir ${CMAKE_BINARY_DIR}/Shaders
            --shader-reflect $<TARGET_FILE:ShaderReflect>)
  set_tests_properties(WebBuiltinShaderMetadata PROPERTIES SKIP_RETURN_CODE 125 TIMEOUT 300)
endif()

# A package a browser gets carries no SPIR-V for a stage it has WGSL for.
if(Python3_Interpreter_FOUND AND TARGET ShaderReflect AND NOT EMSCRIPTEN)
  add_test(NAME WebShaderPackageStrip
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/Web/test_shaderpkg_strip.py
            --shaderpkg-dir ${CMAKE_BINARY_DIR}/Shaders
            --shader-reflect $<TARGET_FILE:ShaderReflect>)
  set_tests_properties(WebShaderPackageStrip PROPERTIES SKIP_RETURN_CODE 125 TIMEOUT 120)
endif()

# On-demand fixture downloads for the skip-gated animation/import suites.
# Opt-in custom targets mirroring the Tests/Scenes Fetch* shape: hash-pinned
# archives, idempotent re-runs, extracted into the exact layout the suites
# resolve through the StageTestAssets sweep (and mirrored straight into the
# build root so no reconfigure is needed after fetching).
#
# Blender bundles (public, canonical blender.org / studio.blender.org URLs;
# CC0 / CC-BY / CC-BY-SA — attribution staged as blend-samples/LICENSES.md):
#   cmake --build --preset <preset> --target FetchBlendTestSamples
# unlocks BlendSmokeParseTests, BlendModelLoadTests, BlendActionExtractTests
# and the .blend half of BlenderToFbxRetargetTest.
#
# Store-licensed FBX test files (NOT redistributable — kept LOCAL, no
# hosted archive exists). License holders configure their own copy:
#   -DGE_FBXTEST_URL=file:///C:/path/to/fbxtest-fixtures-v1.zip
#   cmake --build --preset <preset> --target FetchFbxTestFixtures
# (or just place the five .fbx files in Assets/Models/FBXTest/). Unlocks
# ModelAssetHumanoidPostLoadTests, HumanoidRetargetRealAssetTests,
# EditorSkinnedSpawnTests and the .fbx half of BlenderToFbxRetargetTest;
# without them those suites skip gracefully. Two optional creatures in the same
# folder, Sheep.fbx and Wolf.fbx, unlock the four-legged cases of
# FbxAxisConversionTests and ModelAssetHumanoidPostLoadTests.
add_custom_target(FetchBlendTestSamples
  COMMAND ${CMAKE_COMMAND}
          -DBLEND_SAMPLES_ROOT=${CMAKE_SOURCE_DIR}/Tests/BlendSamples
          -DSTAGED_ROOT=${CMAKE_BINARY_DIR}/Tests/BlendSamples
          -DDOWNLOAD_DIR=${CMAKE_BINARY_DIR}/_fixture_downloads
          -DLICENSES_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/BlendSamplesLicenses.md
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchBlendSamples.cmake
  COMMENT "Fetching Blender-sample test fixtures (~94 MB on first run)"
  VERBATIM)
set_target_properties(FetchBlendTestSamples PROPERTIES FOLDER "Tests/Fixtures")

set(_ge_fbxtest_overrides "")
if(GE_FBXTEST_URL)
  list(APPEND _ge_fbxtest_overrides -DFBXTEST_URL=${GE_FBXTEST_URL})
endif()
if(GE_FBXTEST_SHA256)
  list(APPEND _ge_fbxtest_overrides -DFBXTEST_SHA256=${GE_FBXTEST_SHA256})
endif()
add_custom_target(FetchFbxTestFixtures
  COMMAND ${CMAKE_COMMAND}
          -DFBXTEST_ROOT=${CMAKE_SOURCE_DIR}/Assets/Models/FBXTest
          -DSTAGED_ROOT=${CMAKE_BINARY_DIR}/Assets/Models/FBXTest
          -DDOWNLOAD_DIR=${CMAKE_BINARY_DIR}/_fixture_downloads
          ${_ge_fbxtest_overrides}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchFbxTestFixtures.cmake
  COMMENT "Fetching store-licensed FBX test files (local archive, ~0.7 MB)"
  VERBATIM)
set_target_properties(FetchFbxTestFixtures PROPERTIES FOLDER "Tests/Fixtures")

# Gate scripting-related tests behind ENABLE_SCRIPTING
if(ENABLE_SCRIPTING)

# The managed TEST FIXTURES (DomainRoutingTest, GameEngine.Scripts) are plain
# managed assemblies with no native ABI coupling, so they are built once, in one
# configuration, whatever configuration the engine is built in. This is the
# single source of truth for that choice: every fixture build, every staging
# path and — via GE_MANAGED_FIXTURE_CONFIG below — every test-side path derives
# from it, so the suites resolve their fixtures in DebugFast and Release too.
#
# The engine's own managed assemblies (CoreBridge, HotReload, Scripting.Runtime)
# are NOT fixtures: ge_add_managed_assembly builds them with
# `--configuration $<CONFIG>`, so paths into their output must use $<CONFIG> to
# name the configuration that actually produced the file.
set(GE_MANAGED_FIXTURE_CONFIG Debug)

# Hand a suite the two configuration names, plus the header that turns them into
# fixture paths (Tests/ManagedFixturePaths.h). Every scripting-ABI suite resolves
# its managed fixtures through this and never spells a configuration itself.
function(ge_managed_fixture_paths target_name)
  # Engine/Tests carries StagedTestPaths.h, the exe-anchored StagedRoot() the
  # fixture paths resolve against.
  target_include_directories(${target_name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine/Tests)
  target_compile_definitions(${target_name} PRIVATE
    GE_BUILD_CONFIG="$<CONFIG>"
    GE_MANAGED_FIXTURE_CONFIG="${GE_MANAGED_FIXTURE_CONFIG}")
endfunction()

# Add a minimal native ABI smoke test
add_executable(AbiNativeSmokeTest
    Integration/AbiNativeSmokeTest.cpp
    Integration/ScriptManagerMainThreadTaskTests.cpp
    Integration/ScriptingAbiDomainTests.cpp
    Integration/ScriptingAbiLoggerBridgeTests.cpp
    ${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp
)
find_package(GTest REQUIRED)
target_link_libraries(AbiNativeSmokeTest PRIVATE Engine GameEngine.Native GTest::gtest GTest::gtest_main)
ge_managed_fixture_paths(AbiNativeSmokeTest)
ge_set_output_tests(AbiNativeSmokeTest)
ge_stage_runtime_dependencies(AbiNativeSmokeTest)
add_test(NAME AbiNativeSmokeTest COMMAND $<TARGET_FILE:AbiNativeSmokeTest>)
set_tests_properties(AbiNativeSmokeTest PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
if(TARGET StageTestAssets)
  add_dependencies(AbiNativeSmokeTest StageTestAssets)
endif()

# Scripting ABI Integration Tests (GTest)
add_executable(ScriptingAbiIntegrationTests
    Integration/ScriptingAbiPackageTest.cpp
    Integration/ScriptingAbiDomainTests.cpp
    Integration/ScriptingAbiAdditionalTests.cpp
    Integration/ScriptingAbiCapabilityTests.cpp
    Integration/ScriptingAbiPerfTests.cpp
    Integration/ScriptingAbiPerfCountersTests.cpp
    Integration/ScriptingAbiInvokeByNamePerfTests.cpp
    Integration/ScriptingAbiMultiDomainIsolationTests.cpp
    Integration/ScriptingAbiTokenPathTests.cpp
    Integration/ScriptingAbiEditorDomainTests.cpp
    ScriptingAbi/HotReloadRobustnessTests.cpp
    ScriptingAbi/ReloadPipelineStressTests.cpp
    ScriptingAbi/NegativeCallbackTests.cpp
    Integration/ScriptingAbiNativeHotReloadFlowTests.cpp
    Integration/ScriptingAbiHotReloadMetricsTests.cpp
    Integration/ScriptingAbiAbiHandshakeTests.cpp
    Integration/ScriptingAbiTokenEnforcementTests.cpp
    Integration/ScriptingAbiNegativeQueryExportTests.cpp
    Integration/ScriptingAbiTokenCachePurgeTests.cpp
    Integration/ScriptingAbiDiagnosticsSubscriptionTests.cpp
    Integration/ScriptingAbiGuardrailTests.cpp
    Integration/ScriptingAbiDomainUnloadCallbackTests.cpp
    Integration/ScriptingAbiUiTypeOwnerTests.cpp
    Integration/ScriptingAbiInputActionTests.cpp
    Integration/ScriptingAbiPathMaskTests.cpp
    # The unity-import package's converter assembly on the engine's CoreCLR,
    # through the hosting seam the import modal calls (an EditorSDK TU, which
    # needs only Engine).
    Integration/UnityImportHostedConverterTests.cpp
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Assets/UnityImportHostedConverter.cpp
    ${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp
)

find_package(GTest REQUIRED)

target_link_libraries(ScriptingAbiIntegrationTests PRIVATE Engine GameEngine.Native GTest::gtest GTest::gtest_main)
# Several test TUs define identical file-scope helpers (ReadAllFile, FindDomainRouting,
# etc.); unity-glue collides them. Disable unity for this target. With unity off,
# pre-existing warnings (C4100 unused-param, C4505 unused-static) surface; suppress
# them for this build-only test target (not engine code).
set_target_properties(ScriptingAbiIntegrationTests PROPERTIES UNITY_BUILD OFF)
if(MSVC)
    target_compile_options(ScriptingAbiIntegrationTests PRIVATE /wd4100 /wd4505)
endif()
target_include_directories(ScriptingAbiIntegrationTests PRIVATE ${CMAKE_SOURCE_DIR}/Apps/Editor/Include)
# The converter assembly the unity-import package vendors, staged where the
# package keeps it.
add_custom_command(TARGET ScriptingAbiIntegrationTests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/Packages/unity-import/Tools/UnityConverter.dll
    $<TARGET_FILE_DIR:ScriptingAbiIntegrationTests>/TestData/Packages/unity-import/Tools/UnityConverter.dll
  COMMENT "Staging the unity-import converter assembly for ScriptingAbiIntegrationTests")
ge_managed_fixture_paths(ScriptingAbiIntegrationTests)
ge_set_output_tests(ScriptingAbiIntegrationTests)
ge_stage_runtime_dependencies(ScriptingAbiIntegrationTests)
if(TARGET StageTestAssets)
  add_dependencies(ScriptingAbiIntegrationTests StageTestAssets)
endif()

# GameEngine.Native.dll is an import of every executable here, so ge_stage_runtime_dependencies
# stages it; the managed set comes from StageCoreBridgeAndHotReload below.
find_program(DOTNET_EXECUTABLE dotnet REQUIRED)


# Option to disable managed HotReload/CoreBridge during test builds
option(BUILD_MANAGED_HOTRELOAD "Build managed HotReload/CoreBridge before tests" OFF)

if(BUILD_MANAGED_HOTRELOAD)

# Build managed HotReload/CoreBridge (and CompileServerHost) via central managed targets
find_program(DOTNET_EXECUTABLE dotnet REQUIRED)
add_custom_target(BuildManagedHotReload ALL
	COMMENT "Ensuring HotReload/CoreBridge/CompileServerHost managed projects are built")

# Reuse the top-level managed build targets instead of invoking dotnet build again
if(TARGET HotReloadAssembly)
	add_dependencies(BuildManagedHotReload HotReloadAssembly)
endif()
if(TARGET CoreBridgeAssembly)
	add_dependencies(BuildManagedHotReload CoreBridgeAssembly)
endif()
if(TARGET CompileServerHost)
	add_dependencies(BuildManagedHotReload CompileServerHost)
endif()
# Make Roslyn generator outputs visible in build tree (Debug)
if(WIN32)
  add_custom_command(TARGET BuildManagedHotReload POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_SOURCE_DIR}/Managed/CoreBridge/obj/Generated/GameEngine.InteropGenerator/GameEngine.InteropGenerator.Generator
      ${CMAKE_BINARY_DIR}/ManagedGenerated/CoreBridge
    COMMENT "Copying CoreBridge generated sources to build folder for inspection (Debug)")
endif()

add_dependencies(ScriptingAbiIntegrationTests BuildManagedHotReload)

# Copy managed artifacts next to the test exe (Debug/Release)
  # Copy entire HotReload output dir so all dependent assemblies are available next to the test exe
  add_custom_command(TARGET ScriptingAbiIntegrationTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_SOURCE_DIR}/Managed/HotReload/bin/$<CONFIG>/net10.0
      $<TARGET_FILE_DIR:ScriptingAbiIntegrationTests>)
if(WIN32)
  add_custom_command(TARGET ScriptingAbiIntegrationTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${CMAKE_SOURCE_DIR}/Managed/HotReload/bin/$<CONFIG>/net10.0/GameEngine.HotReload.dll
      $<TARGET_FILE_DIR:ScriptingAbiIntegrationTests>/GameEngine.HotReload.dll)
  add_custom_command(TARGET ScriptingAbiIntegrationTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${CMAKE_SOURCE_DIR}/Managed/HotReload/bin/$<CONFIG>/net10.0/GameEngine.HotReload.runtimeconfig.json
      $<TARGET_FILE_DIR:ScriptingAbiIntegrationTests>/GameEngine.HotReload.runtimeconfig.json)
  add_custom_command(TARGET ScriptingAbiIntegrationTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${CMAKE_SOURCE_DIR}/Managed/HotReload/bin/$<CONFIG>/net10.0/GameEngine.HotReload.deps.json
      $<TARGET_FILE_DIR:ScriptingAbiIntegrationTests>/GameEngine.HotReload.deps.json)


  add_custom_command(TARGET ScriptingAbiIntegrationTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${CMAKE_SOURCE_DIR}/Managed/CoreBridge/bin/$<CONFIG>/net10.0/GameEngine.CoreBridge.dll
      $<TARGET_FILE_DIR:ScriptingAbiIntegrationTests>/GameEngine.CoreBridge.dll)
endif()

# Build DomainRoutingTest managed assembly and copy next to test exe
endif()

# Ensure dotnet is available for DomainRoutingTest even when HotReload build is disabled
if(NOT DOTNET_EXECUTABLE)
  find_program(DOTNET_EXECUTABLE dotnet REQUIRED)
endif()

add_custom_target(BuildDomainRoutingTest ALL
  COMMAND ${DOTNET_EXECUTABLE} build ${CMAKE_SOURCE_DIR}/Tests/ManagedTestAssemblies/DomainRoutingTest/DomainRoutingTest.csproj -c ${GE_MANAGED_FIXTURE_CONFIG}
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Building DomainRoutingTest managed test assembly")
add_dependencies(ScriptingAbiIntegrationTests BuildDomainRoutingTest)
# Copy DomainRoutingTest outputs into the build tree in locations expected by tests.
# The fixture's own bin/<config> segment is the fixture configuration; the
# bin/<config> that names the staged native tree is $<CONFIG>. The tests build
# the same two-token path from GE_MANAGED_FIXTURE_CONFIG and GE_BUILD_CONFIG.
set(_ge_domain_routing_dll
  ${CMAKE_SOURCE_DIR}/Tests/ManagedTestAssemblies/DomainRoutingTest/bin/${GE_MANAGED_FIXTURE_CONFIG}/net10.0/DomainRoutingTest.dll)
set(_ge_domain_routing_reldir
  Tests/ManagedTestAssemblies/DomainRoutingTest/bin/${GE_MANAGED_FIXTURE_CONFIG}/net10.0)
add_custom_target(CopyDomainRoutingTest ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${_ge_domain_routing_dll}
    ${CMAKE_BINARY_DIR}/DomainRoutingTest.dll
  COMMAND ${CMAKE_COMMAND} -E make_directory
    ${CMAKE_BINARY_DIR}/${_ge_domain_routing_reldir}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${_ge_domain_routing_dll}
    ${CMAKE_BINARY_DIR}/${_ge_domain_routing_reldir}/DomainRoutingTest.dll
  # Also stage under bin/<config> so tests that run with CWD=bin/<config> can find it via current_path()
  COMMAND ${CMAKE_COMMAND} -E make_directory
    ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/${_ge_domain_routing_reldir}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${_ge_domain_routing_dll}
    ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/${_ge_domain_routing_reldir}/DomainRoutingTest.dll
  COMMENT "Copying DomainRoutingTest.dll to expected test path, build root, and bin/<config> path")
add_dependencies(CopyDomainRoutingTest BuildDomainRoutingTest)
add_dependencies(ScriptingAbiIntegrationTests CopyDomainRoutingTest)

# Build the default Scripts assembly via the ScriptManager auto-generated project and
# stage it where tests expect it when CWD=bin/<config>. The project is EMITTED by the
# engine at first run, so the helper script skips cleanly in fresh worktrees where it
# does not exist yet instead of failing the whole test build.
add_custom_target(BuildScriptsAssembly ALL
  COMMAND ${CMAKE_COMMAND}
    -DDOTNET_EXE=${DOTNET_EXECUTABLE}
    -DSCRIPTS_DIR=${CMAKE_BINARY_DIR}/bin/$<CONFIG>/ScriptAssemblies
    -DCONFIG=${GE_MANAGED_FIXTURE_CONFIG}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildAndStageScriptsAssembly.cmake
  COMMENT "Building + staging GameEngine.Scripts (skips when the emitted project is absent)")
add_dependencies(ScriptingAbiIntegrationTests BuildScriptsAssembly)

# Stage CoreBridge and HotReload near the test exe and ScriptAssemblies for robust resolution
add_custom_target(StageCoreBridgeAndHotReload ALL
  # Next to test exe (Editor/CoreBridge resolution)
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/Managed/CoreBridge/bin/$<CONFIG>/net10.0/GameEngine.CoreBridge.dll
    ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/GameEngine.CoreBridge.dll

  # Under ScriptAssemblies/<config>/net10.0 for ScriptAssemblies-based probing
  COMMAND ${CMAKE_COMMAND} -E make_directory
    ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/ScriptAssemblies/${GE_MANAGED_FIXTURE_CONFIG}/net10.0
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/Managed/CoreBridge/bin/$<CONFIG>/net10.0/GameEngine.CoreBridge.dll
    ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/ScriptAssemblies/${GE_MANAGED_FIXTURE_CONFIG}/net10.0/GameEngine.CoreBridge.dll
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/Managed/HotReload/bin/$<CONFIG>/net10.0/GameEngine.HotReload.dll
    ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/GameEngine.HotReload.dll

  # Into the test exe directory itself (bin/<config>/Tests): PathResolver anchors
  # CoreBridge/HotReload/runtimeconfig resolution to the EXECUTABLE directory, and
  # EnsureSharedAssembliesInDefault + the CoreBridge Default-ALC resolver probe the
  # same directory for the shared managed set. Without these copies, CLR hosting in
  # ScriptingAbiIntegrationTests fails in fresh worktrees.
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/Managed/CoreBridge/bin/$<CONFIG>/net10.0/GameEngine.CoreBridge.dll
    $<TARGET_FILE_DIR:ScriptingAbiIntegrationTests>/GameEngine.CoreBridge.dll
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/Managed/HotReload/bin/$<CONFIG>/net10.0/GameEngine.HotReload.dll
    $<TARGET_FILE_DIR:ScriptingAbiIntegrationTests>/GameEngine.HotReload.dll
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/Managed/HotReload/bin/$<CONFIG>/net10.0/GameEngine.HotReload.runtimeconfig.json
    $<TARGET_FILE_DIR:ScriptingAbiIntegrationTests>/GameEngine.HotReload.runtimeconfig.json
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/Managed/HotReload/bin/$<CONFIG>/net10.0/GameEngine.HotReload.deps.json
    $<TARGET_FILE_DIR:ScriptingAbiIntegrationTests>/GameEngine.HotReload.deps.json
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/Managed/Scripting.Runtime/bin/$<CONFIG>/GameEngine.Scripting.Runtime.dll
    $<TARGET_FILE_DIR:ScriptingAbiIntegrationTests>/GameEngine.Scripting.Runtime.dll
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/Managed/Scripting.Runtime/bin/$<CONFIG>/GameEngine.Scripting.ABI.dll
    $<TARGET_FILE_DIR:ScriptingAbiIntegrationTests>/GameEngine.Scripting.ABI.dll
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/Managed/Scripting.Runtime/bin/$<CONFIG>/GameEngine.ECS.ABI.dll
    $<TARGET_FILE_DIR:ScriptingAbiIntegrationTests>/GameEngine.ECS.ABI.dll
  COMMENT "Staging CoreBridge and HotReload near test exe, test exe dir, and under ScriptAssemblies")
# This target only copies; the assemblies it copies are produced by the managed
# targets below. Without these edges a fresh worktree fails the copy outright
# (MSB8066) because nothing has run `dotnet build` yet.
foreach(_ge_staged_assembly CoreBridgeAssembly HotReloadAssembly ScriptingRuntimeAssembly)
  if(TARGET ${_ge_staged_assembly})
    add_dependencies(StageCoreBridgeAndHotReload ${_ge_staged_assembly})
  endif()
endforeach()
# If we built managed hotreload/corebridge, stage after that; otherwise just stage from existing outputs
if (TARGET BuildManagedHotReload)
  add_dependencies(StageCoreBridgeAndHotReload BuildManagedHotReload)
endif()
add_dependencies(ScriptingAbiIntegrationTests StageCoreBridgeAndHotReload)

# AbiNativeSmokeTest compiles ScriptingAbiDomainTests.cpp too, so it needs
# the same managed fixtures and the same staged CLR-hosting set as the larger
# suite. Both executables share bin/<config>/Tests, so the staging above already
# lands next to this one; only the ordering edges were missing.
add_dependencies(AbiNativeSmokeTest
  BuildDomainRoutingTest
  CopyDomainRoutingTest
  BuildScriptsAssembly
  StageCoreBridgeAndHotReload)


# Register tests with CTest, run from build root so relative paths are consistent
add_test(NAME ScriptingAbiIntegrationTests COMMAND $<TARGET_FILE:ScriptingAbiIntegrationTests>)
set_tests_properties(ScriptingAbiIntegrationTests PROPERTIES
  TIMEOUT 300
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

endif() # ENABLE_SCRIPTING


if(WIN32 AND ENABLE_SCRIPTING)

  # Editor IoL + HotReload tests:
  # - WindowsEditorHotReload: runs Editor.exe directly and verifies that
  #   InitializeOnLoad Boot() is called on initial load and after hot reload.
  # - LinuxEditorIoL_Docker: optional Docker-backed Linux Editor IoL harness;
  #   skipped cleanly when Docker is not available or not running.
  add_executable(EditorIoLHotReloadTests
    Integration/EditorIoLHotReloadTests.cpp
  )

  target_link_libraries(EditorIoLHotReloadTests PRIVATE Engine GameEngine.Native GTest::gtest GTest::gtest_main)

  # Output layout: bin/<Config>/Tests
  ge_set_output_tests(EditorIoLHotReloadTests)
  ge_stage_runtime_dependencies(EditorIoLHotReloadTests)
	
	  # Register separate CTest entries for the Windows and Linux (Docker) IoL
	  # scenarios, while keeping a shared executable and gtest suite. Both test
	  # names share the common "EditorIoLHotReloadTests" prefix so existing
	  # workflows like `ctest -R EditorIoLHotReloadTests` continue to run both.
	  add_test(NAME EditorIoLHotReloadTests.Windows
	    COMMAND $<TARGET_FILE:EditorIoLHotReloadTests>
	            --gtest_filter=EditorIoL.WindowsEditorHotReload)
	  set_tests_properties(EditorIoLHotReloadTests.Windows PROPERTIES
	    TIMEOUT 600
	    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
	  )
	
	  add_test(NAME EditorIoLHotReloadTests.LinuxDocker
	    COMMAND $<TARGET_FILE:EditorIoLHotReloadTests>
	            --gtest_filter=EditorIoL.LinuxEditorIoL_Docker)
	  set_tests_properties(EditorIoLHotReloadTests.LinuxDocker PROPERTIES
	    TIMEOUT 600
	    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
	  )

endif()

# Hot Reload Demonstration executable
add_executable(HotReloadDemo HotReloadDemonstration.cpp)

# Find Google Test
find_package(GTest REQUIRED)

# Paths + Settings API smoke tests (gtest)
add_executable(PathsAndSettingsTests
  PathsAndSettingsTests.cpp
  ${CMAKE_SOURCE_DIR}/Engine/Source/HotReloadNativeStubs.cpp
)
# Stubs provide local no-op defs for Engine's GE_* ScriptingABI references
# (see HotReloadNativeStubs.cpp). /wd4273 silences the dllimport-vs-definition warning.
set_source_files_properties(${CMAKE_SOURCE_DIR}/Engine/Source/HotReloadNativeStubs.cpp
  TARGET_DIRECTORY PathsAndSettingsTests
  PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/wd4273>")
target_link_libraries(PathsAndSettingsTests PRIVATE Engine GTest::gtest GTest::gtest_main)
target_include_directories(PathsAndSettingsTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/Engine/Include
  # Rendering/Sky/SkyRenderer.h supplies the sky art's install-relative path constant,
  # which the moon-texture anchoring test pins. Headers only — no extra link edge.
  ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Include
)
ge_set_output_tests(PathsAndSettingsTests)
ge_stage_runtime_dependencies(PathsAndSettingsTests)
# Stage the sky art next to this test exe exactly as the Editor and Player stage it, so the
# moon-texture test asserts a file that a staging rule actually produced. Without a staged
# copy the test could only check string composition, and deleting a staging rule would leave
# it green.
ge_stage_test_asset(PathsAndSettingsTests
  "${CMAKE_SOURCE_DIR}/Assets/Textures/Sky/MoonPhases/moon_full.png"
  "Assets/Textures/Sky/MoonPhases/moon_full.png")
add_test(NAME PathsAndSettingsTests COMMAND $<TARGET_FILE:PathsAndSettingsTests>)
set_tests_properties(PathsAndSettingsTests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Editor startup asset seeding/refresh helpers (pure std::filesystem, no Engine
# dependency — Types is header-only) — compiled directly from the editor source tree.
add_executable(BuiltinAssetSyncTests
  BuiltinAssetSyncTests.cpp
  ${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Startup/BuiltinAssetSync.cpp
)
target_link_libraries(BuiltinAssetSyncTests PRIVATE GTest::gtest GTest::gtest_main)
target_include_directories(BuiltinAssetSyncTests PRIVATE
  ${CMAKE_SOURCE_DIR}/Apps/Editor/Source
  ${CMAKE_SOURCE_DIR}/Engine/Modules/Types/Include
)
ge_set_output_tests(BuiltinAssetSyncTests)
add_test(NAME BuiltinAssetSyncTests COMMAND $<TARGET_FILE:BuiltinAssetSyncTests>)
set_tests_properties(BuiltinAssetSyncTests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Game release packaging and filesystem output promotion contracts.
find_package(Python3 3.10 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME GamePublishingTests
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/Tests/GamePublishingTests.py")
endif()
add_executable(BuildOutputPromotionTests
  BuildOutputPromotionTests.cpp
  BuildDirectoryCopyTests.cpp
  ${CMAKE_SOURCE_DIR}/Engine/Source/Engine/Build/OutputPromotion.cpp
  ${CMAKE_SOURCE_DIR}/Engine/Source/Engine/Build/DirectoryCopy.cpp
)
target_include_directories(BuildOutputPromotionTests PRIVATE "${CMAKE_SOURCE_DIR}/Engine/Include")
target_compile_features(BuildOutputPromotionTests PRIVATE cxx_std_20)
target_link_libraries(BuildOutputPromotionTests PRIVATE GTest::gtest GTest::gtest_main FileSystemObjects Logger)
ge_set_output_tests(BuildOutputPromotionTests)
ge_stage_runtime_dependencies(BuildOutputPromotionTests)
add_test(NAME BuildOutputPromotionTests COMMAND $<TARGET_FILE:BuildOutputPromotionTests>)
set_tests_properties(BuildOutputPromotionTests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Physics smoke tests (Jolt backend): validates basic gravity + collisions.
add_executable(PhysicsSmokeTests
  PhysicsSmokeTests.cpp
)
# Modules are OBJECT libs aggregated into Engine.dll; link Engine to resolve
# module symbols via the import lib. See GenerationalVector/Tests for the same
# fix pattern after Phase 1's STATIC→OBJECT module conversion.
target_link_libraries(PhysicsSmokeTests PRIVATE Engine GTest::gtest GTest::gtest_main)
target_include_directories(PhysicsSmokeTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/Engine/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/TerrainECS/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Include
)
# The removal hooks must never resolve a sibling component off the dying entity
# (self-deadlock on worldMutex); that shape is asserted against the sources.
target_compile_definitions(PhysicsSmokeTests PRIVATE
  GE_REPO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
ge_set_output_tests(PhysicsSmokeTests)
ge_stage_runtime_dependencies(PhysicsSmokeTests)
add_test(NAME PhysicsSmokeTests COMMAND $<TARGET_FILE:PhysicsSmokeTests>)
set_tests_properties(PhysicsSmokeTests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

if(GE_PHYSICS_BACKEND_JOLT)
  add_executable(PhysicsBoxOverlapTests PhysicsBoxOverlapTests.cpp)
  target_link_libraries(PhysicsBoxOverlapTests PRIVATE Engine GTest::gtest)
  ge_set_output_tests(PhysicsBoxOverlapTests)
  ge_stage_runtime_dependencies(PhysicsBoxOverlapTests)
  add_test(NAME PhysicsBoxOverlapTests COMMAND $<TARGET_FILE:PhysicsBoxOverlapTests>)
  set_tests_properties(PhysicsBoxOverlapTests PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

add_executable(CharacterControllerTests
  CharacterControllerTests.cpp
)
target_link_libraries(CharacterControllerTests PRIVATE Engine GTest::gtest GTest::gtest_main)
target_include_directories(CharacterControllerTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/Engine/Include
)
ge_set_output_tests(CharacterControllerTests)
ge_stage_runtime_dependencies(CharacterControllerTests)
add_test(NAME CharacterControllerTests COMMAND $<TARGET_FILE:CharacterControllerTests>)
set_tests_properties(CharacterControllerTests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# The GPU surface-rule evaluator, lifted VERBATIM out of the shipped shader at build time and
# compiled into TerrainRegionBakeTests as C++ (through CBTTerrain's GlslShim.h), so the CPU/GPU
# parity gate executes the shader's own source rather than a copy that can drift. Three blocks,
# because what sits between the second and third is GLSL-only — the row/condition SSBO
# declarations — and the host supplies its own stand-ins there. A missing or reordered marker is a
# hard error in the extraction step: extracting nothing would leave the gate passing with no
# shader in it.
set(_GE_SURFACE_RULES_GLSL
  ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT/terrain_surface_rules.glsl)
set(_GE_EXTRACT_SHADER_BLOCK
  ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Tests/ExtractShaderBlock.cmake)
set(_GE_SURFACE_RULE_MARKERS TYPES MATH ROWS)
set(_GE_SURFACE_RULE_NAMES   Types Math Rows)
set(_GE_SURFACE_RULE_HEADERS "")
list(LENGTH _GE_SURFACE_RULE_MARKERS _GE_SURFACE_RULE_COUNT)
math(EXPR _GE_SURFACE_RULE_LAST "${_GE_SURFACE_RULE_COUNT} - 1")
foreach(_i RANGE ${_GE_SURFACE_RULE_LAST})
  list(GET _GE_SURFACE_RULE_MARKERS ${_i} _marker)
  list(GET _GE_SURFACE_RULE_NAMES ${_i} _name)
  set(_out ${CMAKE_CURRENT_BINARY_DIR}/generated/TerrainSurfaceRule${_name}Extracted.h)
  add_custom_command(
    OUTPUT ${_out}
    COMMAND ${CMAKE_COMMAND} -DSRC=${_GE_SURFACE_RULES_GLSL}
                             -DOUT=${_out}
                             "-DBEGIN_MARKER=// GE_SURFACE_RULE_${_marker}_BEGIN"
                             "-DEND_MARKER=// GE_SURFACE_RULE_${_marker}_END"
                             -P ${_GE_EXTRACT_SHADER_BLOCK}
    DEPENDS ${_GE_SURFACE_RULES_GLSL} ${_GE_EXTRACT_SHADER_BLOCK}
    COMMENT "Extracting the ${_marker} block from terrain_surface_rules.glsl"
    VERBATIM)
  list(APPEND _GE_SURFACE_RULE_HEADERS ${_out})
endforeach()

# Terrain region-bake parity: region-scoped modifier re-bakes must match a
# fresh full bake bit-exactly (heightfield, splatmap, normalmap).
# DirtyRegionLogTests: per-consumer dirty-region journal semantics (cursors,
# overflow coalescing, CreateTerrain seeding).
add_executable(TerrainRegionBakeTests
  ${_GE_SURFACE_RULE_HEADERS}
  TerrainRegionBakeTests.cpp
  # The single terrain's baked-terrain cache: hit identity, key classes, pins.
  TerrainBakeCacheTests.cpp
  # Raw .r16/.r32 heightmaps: the grid their import settings declare, and the refusal that
  # names the fix when the file cannot be read on any grid.
  RawHeightmapTests.cpp
  TerrainGrassRegionTests.cpp
  # Spline-driven modifiers: Shape::Spline resolution + bounds, the spline
  # extraction -> modifier gather schedule edge, and the spline-scene change gate.
  TerrainSplineModifierTests.cpp
  # Modifier volumes (design §2a): the migration-parity contract against the
  # pre-volume components, effect stack ordering, spline path-vs-area, the
  # inward feather / master weight, and the sphere guards.
  TerrainModifierVolumeTests.cpp
  # Pooled flattens: the per-station route heights a SplinePath volume feeds a
  # pooled flatten, the Average blend's pool, the ground claim masked PER MEMBER
  # in every blend mode, and the contracts around them (a claim writes no
  # height; claims combine by max). Emits the offline twin's parity fixture.
  TerrainFlattenPoolTests.cpp
  # Surface rules (F10 S2): condition evaluation, the AND product, the band and
  # curve shapes, the caps, the GPU splat gate, and the rules arm of the splat
  # claim end to end.
  TerrainSurfaceRulesTests.cpp
  # Default surface rules (F10 S5): the measured slope/height distribution the
  # default rows are placed against, the pin that every authored band stays
  # REACHABLE on real ground, and what the rows actually place for the look review.
  TerrainDefaultSurfaceRulesTests.cpp
  # Surface rules on the GPU (F10 S3): the shipped GLSL evaluator, executed as C++,
  # swept against the C++ evaluator it mirrors — plus the row packer's ordering.
  TerrainSurfaceRuleGpuParityTests.cpp
  DirtyRegionLogTests.cpp
  # CBTUpdateSystem::ConsumeDirtyRegion coverage (C5 live-edit): the ECS ->
  # TerrainService -> classify path, incl. the C1 slot-0 guard + m3 cursor confirm.
  CBTRegionConsumeTests.cpp
  # CBTTreeSeed: when the CBT tree restarts from its roots (a retired terrain, a lowered depth
  # cap, a domain switch).
  CBTTreeSeedTests.cpp
  # Sphere sculpt .tsculpt persistence at the service level: restore-is-an-edit,
  # saved-grid load x #632 radius remap, #630 undo-across-load, needs-save lifecycle.
  SphereSculptPersistenceTests.cpp
)
target_link_libraries(TerrainRegionBakeTests PRIVATE Engine Noise GTest::gtest GTest::gtest_main)
target_include_directories(TerrainRegionBakeTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/Engine/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrainECS/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Include
  # GlslShim.h, and the generated headers the extraction above writes.
  ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Tests
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
ge_set_output_tests(TerrainRegionBakeTests)
ge_stage_runtime_dependencies(TerrainRegionBakeTests)
add_test(NAME TerrainRegionBakeTests COMMAND $<TARGET_FILE:TerrainRegionBakeTests>)
set_tests_properties(TerrainRegionBakeTests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# PlanetColliderTests: pure-function oracles for the planet collider height source —
# per-face body frames (valid rotations), HEIGHT-MATCH (collider samples == the CBT
# render height field), the gnomonic skew budget, the sculpt-edit UV rect -> grid rect
# mapping, and the TerrainService planet-face slot lifecycle. No device / physics world.
add_executable(PlanetColliderTests
  PlanetColliderTests.cpp
)
target_link_libraries(PlanetColliderTests PRIVATE Engine GTest::gtest GTest::gtest_main)
target_include_directories(PlanetColliderTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/Engine/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/TerrainECS/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Include
)
ge_set_output_tests(PlanetColliderTests)
ge_stage_runtime_dependencies(PlanetColliderTests)
add_test(NAME PlanetColliderTests COMMAND $<TARGET_FILE:PlanetColliderTests>)
set_tests_properties(PlanetColliderTests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# TerrainAtlasTests: the resident-window atlas slice — slot-pool churn/leak/
# generation + frames-in-flight quarantine (Risk 2), atlas sampling parity +
# shared-edge continuity across disjoint slots, mixed-LOD edge ownership (Risk 1),
# and out-of-window fallback + horizon-seam continuity (Risk 3). Pure-function
# oracles, but for the paged resolve's device parity test; each risk mitigation has a
# disable-and-fail discriminator.
# The paged height resolve's GLSL (cbt_page.glsl), extracted so TerrainAtlasTests compiles the
# shipped shader code as C++ and holds it to the CPU mirror bit for bit. Two blocks: the consumer's
# taps sit between them, and the host defines its own.
set(_GE_PAGE_RESOLVE_GLSL
  ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT/cbt_page.glsl)
set(_GE_PAGE_RESOLVE_HEADERS "")
foreach(_block TYPES RESOLVE)
  if(_block STREQUAL "TYPES")
    set(_out ${CMAKE_CURRENT_BINARY_DIR}/generated/PageResolveTypesExtracted.h)
  else()
    set(_out ${CMAKE_CURRENT_BINARY_DIR}/generated/PageResolveExtracted.h)
  endif()
  add_custom_command(
    OUTPUT ${_out}
    COMMAND ${CMAKE_COMMAND} -DSRC=${_GE_PAGE_RESOLVE_GLSL}
                             -DOUT=${_out}
                             "-DBEGIN_MARKER=// GE_SHARED_PAGE_${_block}_BEGIN"
                             "-DEND_MARKER=// GE_SHARED_PAGE_${_block}_END"
                             -P ${_GE_EXTRACT_SHADER_BLOCK}
    DEPENDS ${_GE_PAGE_RESOLVE_GLSL} ${_GE_EXTRACT_SHADER_BLOCK}
    COMMENT "Extracting the ${_block} block from cbt_page.glsl"
    VERBATIM)
  list(APPEND _GE_PAGE_RESOLVE_HEADERS ${_out})
endforeach()

# The resolve's device probe: cbt_page.glsl in a compute kernel, so the device parity test runs the
# shipped resolve with the hardware's bilinear tap.
set(_GE_PAGE_RESOLVE_PROBE_SPV ${CMAKE_BINARY_DIR}/Shaders/terrain_page_resolve_probe.comp.spv)
add_custom_command(
  OUTPUT ${_GE_PAGE_RESOLVE_PROBE_SPV}
  COMMAND ${CBT_GLSLC_EXECUTABLE} --target-env=vulkan1.3
          -I ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT
          ${CMAKE_CURRENT_SOURCE_DIR}/TerrainPageResolveProbe.comp -o ${_GE_PAGE_RESOLVE_PROBE_SPV}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/TerrainPageResolveProbe.comp ${_GE_PAGE_RESOLVE_GLSL}
  COMMENT "Compiling the paged height resolve probe: TerrainPageResolveProbe.comp"
  VERBATIM)

add_executable(TerrainAtlasTests
  TerrainAtlasTests.cpp
  # The paged height source: the level rule, the page cache, the residency, the resolve mirror and
  # the shipped GLSL resolve against it, on the host and on the device.
  TerrainPageResidencyTests.cpp
  TerrainPageResolveGpuTests.cpp
  # The height source of a tiled terrain and the main-thread half of its height pages.
  TerrainHeightPagesTests.cpp
  ${_GE_PAGE_RESOLVE_HEADERS}
  ${_GE_PAGE_RESOLVE_PROBE_SPV}
)
target_compile_definitions(TerrainAtlasTests PRIVATE
  TERRAIN_PAGE_PROBE_SPV="${_GE_PAGE_RESOLVE_PROBE_SPV}")
if(MSVC)
  # Silence C6326 (/analyze) "constant compared with constant" noise from GTest
  # EXPECT_EQ/EXPECT_FLOAT_EQ on literal expectations (as CBTTerrainTests does).
  target_compile_options(TerrainAtlasTests PRIVATE /wd6326)
endif()
target_link_libraries(TerrainAtlasTests PRIVATE Engine GTest::gtest GTest::gtest_main)
target_include_directories(TerrainAtlasTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/Engine/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/TerrainECS/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/PageStreaming/Include
  # Grass atlas params builder oracle (TerrainGrassRenderFeature::BuildAtlasParams) + its deps.
  ${CMAKE_SOURCE_DIR}/Engine/Modules/TerrainGrass/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/Terrain/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/Types/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/Mathematics/Include
  # TileStreamingManager.h (TileStreamingPriority ordering oracle) pulls JobSystem/TaskHandle.h.
  ${CMAKE_SOURCE_DIR}/Engine/Modules/JobSystem/Include
  # GlslShim.h, and the extracted cbt_page.glsl blocks.
  ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Tests
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
ge_set_output_tests(TerrainAtlasTests)
ge_stage_runtime_dependencies(TerrainAtlasTests)
add_test(NAME TerrainAtlasTests COMMAND $<TARGET_FILE:TerrainAtlasTests>)
set_tests_properties(TerrainAtlasTests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# PageStoreTests: the terrain page store (Engine/Modules/PageStreaming) as pure oracles: the
# .gepage round trip, the apron's bit-equality under the store-wide height quantum, the sparse
# resolve, the incremental re-cook, the lattice-preserving tent, the generated provider's level 0
# against the terrain's tile fill (FillTiledBaseRegion), the cook key, the terrain container, the
# PNG decoder limit and the asynchronous page loader. Links Engine for the tile fill and the
# asset reader threads.
add_executable(PageStoreTests
  PageStoreTests.cpp
)
if(MSVC)
  target_compile_options(PageStoreTests PRIVATE /wd6326)
endif()
target_link_libraries(PageStoreTests PRIVATE Engine GTest::gtest GTest::gtest_main)
target_include_directories(PageStoreTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/Engine/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/PageStreaming/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/TerrainECS/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/Terrain/Include
  # The sculpt page table's entry constants (CBTLayout.h), which the page-table entry mirrors.
  ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/Types/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/Mathematics/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/JobSystem/Include
)
ge_set_output_tests(PageStoreTests)
ge_stage_runtime_dependencies(PageStoreTests)
add_test(NAME PageStoreTests COMMAND $<TARGET_FILE:PageStoreTests>)
set_tests_properties(PageStoreTests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# WorldHookOwnershipTests: "register, don't accrete" for component teardown —
# a source scan asserting every production RegisterOnRemove lives in its owning
# module's registrar and that Core registers none itself. Pure filesystem, no Engine.
add_executable(WorldHookOwnershipTests
  WorldHookOwnershipTests.cpp
)
target_link_libraries(WorldHookOwnershipTests PRIVATE GTest::gtest GTest::gtest_main)
target_compile_definitions(WorldHookOwnershipTests PRIVATE
  GE_REPO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_compile_features(WorldHookOwnershipTests PRIVATE cxx_std_20)
ge_set_output_tests(WorldHookOwnershipTests)
add_test(NAME WorldHookOwnershipTests COMMAND $<TARGET_FILE:WorldHookOwnershipTests>)
set_tests_properties(WorldHookOwnershipTests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# TerrainWorldHookTests: scene close has an owner — the Components::Terrain
# OnRemove hook releases the TerrainService slot and its GPU set from World::Clear,
# World::DestroyEntity and a component remove, and slot indices are recycled so
# reopening a scene does not ratchet.
add_executable(TerrainWorldHookTests
  TerrainWorldHookTests.cpp
)
target_link_libraries(TerrainWorldHookTests PRIVATE Engine GTest::gtest GTest::gtest_main)
target_include_directories(TerrainWorldHookTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/Engine/Include
)
ge_set_output_tests(TerrainWorldHookTests)
ge_stage_runtime_dependencies(TerrainWorldHookTests)
add_test(NAME TerrainWorldHookTests COMMAND $<TARGET_FILE:TerrainWorldHookTests>)
set_tests_properties(TerrainWorldHookTests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# TerrainUsabilityTests: the one-control-component slice — auto-derived MaxDepth
# (incl. the fp32 ceiling clamp), the internal MaxDepthOverride, and the creation
# presets yielding a rendering-ready configuration.
add_executable(TerrainUsabilityTests
  TerrainProvisioningTests.cpp
)
target_link_libraries(TerrainUsabilityTests PRIVATE Engine GTest::gtest GTest::gtest_main)
target_include_directories(TerrainUsabilityTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/Engine/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrainECS/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Include
)
ge_set_output_tests(TerrainUsabilityTests)
ge_stage_runtime_dependencies(TerrainUsabilityTests)
add_test(NAME TerrainUsabilityTests COMMAND $<TARGET_FILE:TerrainUsabilityTests>)
set_tests_properties(TerrainUsabilityTests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# TerrainMaterialTableTests: how a channel role finds its library material and
# what the GPU record carries once it has — the slot-ID identity rule, the legacy
# per-layer fallback, and the migration equivalence between the two.
add_executable(TerrainMaterialTableTests
  TerrainMaterialTableTests.cpp
)
target_link_libraries(TerrainMaterialTableTests PRIVATE Engine GTest::gtest GTest::gtest_main)
target_include_directories(TerrainMaterialTableTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/Engine/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/Terrain/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/TerrainECS/Include
)
ge_set_output_tests(TerrainMaterialTableTests)
ge_stage_runtime_dependencies(TerrainMaterialTableTests)
add_test(NAME TerrainMaterialTableTests COMMAND $<TARGET_FILE:TerrainMaterialTableTests>)
set_tests_properties(TerrainMaterialTableTests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# TerrainZoneTests: E4 zone modifiers — payload codec, zone-local UV sampling
# under rotation/scale, payload-dirty-rect region parity, paint zones, and the
# auto-grow authoring math.
add_executable(TerrainZoneTests
  TerrainZoneTests.cpp
)
target_link_libraries(TerrainZoneTests PRIVATE Engine GTest::gtest GTest::gtest_main)
target_include_directories(TerrainZoneTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/Engine/Include
)
ge_set_output_tests(TerrainZoneTests)
ge_stage_runtime_dependencies(TerrainZoneTests)
add_test(NAME TerrainZoneTests COMMAND $<TARGET_FILE:TerrainZoneTests>)
set_tests_properties(TerrainZoneTests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# TerrainModifierGizmoTests: editor terrain-modifier volume gizmo geometry —
# planar footprint outlines + spherical (planet) projection. The geometry
# (Apps/Editor/Include/SceneView/TerrainModifierGizmoGeometry.h) is header-only
# and pure, so the test compiles it directly; Engine is linked only to satisfy
# the CBTTerrain header include path the same way the other terrain tests do.
add_executable(TerrainModifierGizmoTests
  TerrainModifierGizmoTests.cpp
)
target_link_libraries(TerrainModifierGizmoTests PRIVATE Engine GTest::gtest GTest::gtest_main)
target_include_directories(TerrainModifierGizmoTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/Apps/Editor/Include
  ${CMAKE_SOURCE_DIR}/Engine/Include
  ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Include
)
ge_set_output_tests(TerrainModifierGizmoTests)
ge_stage_runtime_dependencies(TerrainModifierGizmoTests)
add_test(NAME TerrainModifierGizmoTests COMMAND $<TARGET_FILE:TerrainModifierGizmoTests>)
set_tests_properties(TerrainModifierGizmoTests PROPERTIES
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# UIReplay (Editor-driven UI automation)
# Notes:
# - Runs the Editor with scripted UI input scenarios (PowerShell).
# - Intended as a smoke/regression test for retained-mode invalidation + virtualization.
# - Uses the safety-first runner (timeouts, stdout/stderr capture, never throws).
if (WIN32 AND TARGET Editor)
  find_program(POWERSHELL_EXECUTABLE powershell)
  if (POWERSHELL_EXECUTABLE)
    add_test(NAME UIReplay.Smoke
      COMMAND ${POWERSHELL_EXECUTABLE} -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/Tests/run_ui_replay_tests_safe.ps1"
        -EditorExe $<TARGET_FILE:Editor>
        -Config $<CONFIG>
        -ProjectRoot ${CMAKE_SOURCE_DIR}
        -OutDir ${CMAKE_BINARY_DIR}/Tests/UIReplay/out
        -ExitAfterFrames 700
        -TimeoutSeconds 120
        -Scenarios
          "${CMAKE_SOURCE_DIR}/Tests/UIReplay/smoke_idle.json"
          "${CMAKE_SOURCE_DIR}/Tests/UIReplay/repro_project_picker_recent_project_sceneview_primitives_visible.json"
          "${CMAKE_SOURCE_DIR}/Tests/UIReplay/repro_assets_listview_horizontal_scrollbar_drag.json"
    )
    set_tests_properties(UIReplay.Smoke PROPERTIES
      TIMEOUT 900
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
  endif()
endif()

# Link hot reload demo with engine library
target_link_libraries(HotReloadDemo PRIVATE
    Engine
    GameEngine.Native
)

# Include directories for hot reload demo
target_include_directories(HotReloadDemo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Engine/Include
)

# Output layout: bin/<Config>/Tests
ge_set_output_tests(HotReloadDemo)
ge_stage_runtime_dependencies(HotReloadDemo)

# Add async hot-reload job system tests
add_subdirectory(Jobs)

# Host checks for the Wasm support scripts. Wasm smoke binaries are registered
# only by Emscripten builds, which also add this directory from the root.
add_subdirectory(Wasm)

# Add AssetCore architecture tests
add_subdirectory(AssetCore)

# Add Asset System Performance Optimization tests
add_subdirectory(Assets)

# UI tests (docking model unit tests in new UI module)
add_subdirectory(UI)

# Add ECS standalone tests
add_subdirectory(ECS)


# Add Text tests
add_subdirectory(Text)

# Add Rendering tests (includes AnimationSystemTests)
add_subdirectory(Rendering)

# Audio ECS tests (AudioEmitterSystem lifecycle consumer)
add_subdirectory(Audio)

# Add Editor tests
add_subdirectory(Editor)

# Standalone Player startup resolution (HDR output options).
add_subdirectory(Player)

# EditorSDK ABI tests (registries + Editor-kind module build/load); no-op
# when the Editor (and its EditorSDK target) is not built.
add_subdirectory(EditorSDK)

# Native scripting (Phase 2) — cross-DLL registration smoke test
add_subdirectory(NativeScripting)

# Cross-framework ECS benchmark harness (ours vs EnTT/flecs/gaia-ecs).
# Not a test target: EXCLUDE_FROM_ALL inside keeps it out of default builds;
# it only ever builds when requested explicitly (--target ECSBenchCompare).
add_subdirectory(${CMAKE_SOURCE_DIR}/Tools/Benchmarks/ECS
                 ${CMAKE_BINARY_DIR}/Tools/Benchmarks/ECS)

# Particle runtime timing tool, built only on request (--target ParticleRuntimeBenchmark).
add_subdirectory(${CMAKE_SOURCE_DIR}/Tools/Benchmarks/Particles
                 ${CMAKE_BINARY_DIR}/Tools/Benchmarks/Particles)
