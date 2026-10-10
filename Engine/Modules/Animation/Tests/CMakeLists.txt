find_package(GTest REQUIRED)
include(GoogleTest)

# Animation, AssetCore, Logger and GenerationalVector are OBJECT libraries. CMake
# only splices an object library's objects into targets that link it DIRECTLY —
# Animation's PUBLIC links to AssetCore and GenerationalVector propagate include
# paths but not their objects (GUID/SharedFileRead/AssetEvents, the generational
# manager), and its PRIVATE Logger link propagates nothing. Every test
# executable must therefore link all four explicitly or it fails with
# unresolved externals on a fresh tree.

add_executable(AnimationIKTests
    IKTests.cpp
)
target_compile_features(AnimationIKTests PUBLIC cxx_std_20)
target_link_libraries(AnimationIKTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(AnimationIKTests)
ge_stage_runtime_dependencies(AnimationIKTests)
gtest_discover_tests(AnimationIKTests)

add_executable(AnimationMontageTests
    MontageTests.cpp
)
target_compile_features(AnimationMontageTests PUBLIC cxx_std_20)
target_link_libraries(AnimationMontageTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(AnimationMontageTests)
ge_stage_runtime_dependencies(AnimationMontageTests)
gtest_discover_tests(AnimationMontageTests)

add_executable(AnimationCoreTests
    AnimationCoreTests.cpp
)
target_compile_features(AnimationCoreTests PUBLIC cxx_std_20)
target_link_libraries(AnimationCoreTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(AnimationCoreTests)
ge_stage_runtime_dependencies(AnimationCoreTests)
gtest_discover_tests(AnimationCoreTests)

add_executable(AnimationTimelineTests
    AnimationTimelineTests.cpp
)
target_compile_features(AnimationTimelineTests PUBLIC cxx_std_20)
target_link_libraries(AnimationTimelineTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(AnimationTimelineTests)
ge_stage_runtime_dependencies(AnimationTimelineTests)
gtest_add_tests(TARGET AnimationTimelineTests)

add_executable(AnimationLibraryTests
    AnimationLibraryTests.cpp
)
target_compile_features(AnimationLibraryTests PUBLIC cxx_std_20)
target_link_libraries(AnimationLibraryTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(AnimationLibraryTests)
ge_stage_runtime_dependencies(AnimationLibraryTests)
gtest_add_tests(TARGET AnimationLibraryTests)

add_executable(AnimationControllerTests
    AnimationControllerTests.cpp
)
target_compile_features(AnimationControllerTests PUBLIC cxx_std_20)
target_link_libraries(AnimationControllerTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(AnimationControllerTests)
ge_stage_runtime_dependencies(AnimationControllerTests)
gtest_add_tests(TARGET AnimationControllerTests)

add_executable(AnimationPlayerTests
    AnimationPlayerTests.cpp
)
target_compile_features(AnimationPlayerTests PUBLIC cxx_std_20)
target_link_libraries(AnimationPlayerTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(AnimationPlayerTests)
ge_stage_runtime_dependencies(AnimationPlayerTests)
gtest_add_tests(TARGET AnimationPlayerTests)

add_executable(TweenServiceTests
    TweenServiceTests.cpp
)
target_compile_features(TweenServiceTests PUBLIC cxx_std_20)
target_link_libraries(TweenServiceTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(TweenServiceTests)
ge_stage_runtime_dependencies(TweenServiceTests)
gtest_add_tests(TARGET TweenServiceTests)

add_executable(SkeletonModifierStackTests
    SkeletonModifierStackTests.cpp
)
target_compile_features(SkeletonModifierStackTests PUBLIC cxx_std_20)
target_link_libraries(SkeletonModifierStackTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(SkeletonModifierStackTests)
ge_stage_runtime_dependencies(SkeletonModifierStackTests)
gtest_add_tests(TARGET SkeletonModifierStackTests)

add_executable(SpriteFramesTests
    SpriteFramesTests.cpp
)
target_compile_features(SpriteFramesTests PUBLIC cxx_std_20)
target_link_libraries(SpriteFramesTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(SpriteFramesTests)
ge_stage_runtime_dependencies(SpriteFramesTests)
gtest_add_tests(TARGET SpriteFramesTests)

add_executable(FootIKNodeTests
    FootIKNodeTests.cpp
)
target_compile_features(FootIKNodeTests PUBLIC cxx_std_20)
target_link_libraries(FootIKNodeTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(FootIKNodeTests)
ge_stage_runtime_dependencies(FootIKNodeTests)
gtest_discover_tests(FootIKNodeTests)

add_executable(IKAllocationTests
    IKAllocationTests.cpp
)
target_compile_features(IKAllocationTests PUBLIC cxx_std_20)
target_link_libraries(IKAllocationTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(IKAllocationTests)
ge_stage_runtime_dependencies(IKAllocationTests)
gtest_discover_tests(IKAllocationTests)

add_executable(SkeletonProfileTests
    SkeletonProfileTests.cpp
)
target_compile_features(SkeletonProfileTests PUBLIC cxx_std_20)
target_link_libraries(SkeletonProfileTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(SkeletonProfileTests)
ge_stage_runtime_dependencies(SkeletonProfileTests)
gtest_discover_tests(SkeletonProfileTests)

add_executable(HumanoidRigTests
    HumanoidRigTests.cpp
)
target_compile_features(HumanoidRigTests PUBLIC cxx_std_20)
target_link_libraries(HumanoidRigTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(HumanoidRigTests)
ge_stage_runtime_dependencies(HumanoidRigTests)
gtest_discover_tests(HumanoidRigTests)

add_executable(RetargetMapTests
    RetargetMapTests.cpp
)
target_compile_features(RetargetMapTests PUBLIC cxx_std_20)
target_link_libraries(RetargetMapTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(RetargetMapTests)
ge_stage_runtime_dependencies(RetargetMapTests)
gtest_discover_tests(RetargetMapTests)

add_executable(RetargetOpStackTests
    RetargetOpStackTests.cpp
)
target_compile_features(RetargetOpStackTests PUBLIC cxx_std_20)
target_link_libraries(RetargetOpStackTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(RetargetOpStackTests)
ge_stage_runtime_dependencies(RetargetOpStackTests)
gtest_discover_tests(RetargetOpStackTests)

add_executable(RetargetAssetWatcherTests
    RetargetAssetWatcherTests.cpp
)
target_compile_features(RetargetAssetWatcherTests PUBLIC cxx_std_20)
target_link_libraries(RetargetAssetWatcherTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(RetargetAssetWatcherTests)
ge_stage_runtime_dependencies(RetargetAssetWatcherTests)
gtest_discover_tests(RetargetAssetWatcherTests)

add_executable(HumanoidNameMatcherTests
    HumanoidNameMatcherTests.cpp
)
target_compile_features(HumanoidNameMatcherTests PUBLIC cxx_std_20)
target_link_libraries(HumanoidNameMatcherTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(HumanoidNameMatcherTests)
ge_stage_runtime_dependencies(HumanoidNameMatcherTests)
gtest_discover_tests(HumanoidNameMatcherTests)

add_executable(AutoImportTests
    AutoImportTests.cpp
)
target_compile_features(AutoImportTests PUBLIC cxx_std_20)
target_link_libraries(AutoImportTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(AutoImportTests)
ge_stage_runtime_dependencies(AutoImportTests)
gtest_discover_tests(AutoImportTests)

add_executable(AutoCreateRetargetMapTests
    AutoCreateRetargetMapTests.cpp
)
target_compile_features(AutoCreateRetargetMapTests PUBLIC cxx_std_20)
target_link_libraries(AutoCreateRetargetMapTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(AutoCreateRetargetMapTests)
ge_stage_runtime_dependencies(AutoCreateRetargetMapTests)
gtest_discover_tests(AutoCreateRetargetMapTests)

add_executable(RetargetNodeTests
    RetargetNodeTests.cpp
)
target_compile_features(RetargetNodeTests PUBLIC cxx_std_20)
target_link_libraries(RetargetNodeTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(RetargetNodeTests)
ge_stage_runtime_dependencies(RetargetNodeTests)
gtest_discover_tests(RetargetNodeTests)

add_executable(HumanoidRigEditTests
    HumanoidRigEditTests.cpp
)
target_compile_features(HumanoidRigEditTests PUBLIC cxx_std_20)
target_link_libraries(HumanoidRigEditTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(HumanoidRigEditTests)
ge_stage_runtime_dependencies(HumanoidRigEditTests)
gtest_discover_tests(HumanoidRigEditTests)

add_executable(HumanoidStandardProfileTests
    HumanoidStandardProfileTests.cpp
)
target_compile_features(HumanoidStandardProfileTests PUBLIC cxx_std_20)
target_link_libraries(HumanoidStandardProfileTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
ge_set_output_tests(HumanoidStandardProfileTests)
ge_stage_runtime_dependencies(HumanoidStandardProfileTests)

# Phase 10 — KHR_humanoid + VRM 1.0 + auto-import dispatch tests. These three
# test executables include cgltf.h and need the cgltf implementation symbols.
# Each links a test-local cgltf_impl_test.cpp that defines CGLTF_IMPLEMENTATION;
# this keeps the tests hermetic (no Engine link dependency).
find_path(CGLTF_INCLUDE_DIRS_TESTS "cgltf.h")

add_executable(KHRHumanoidImporterTests
    KHRHumanoidImporterTests.cpp
    cgltf_impl_test.cpp
)
target_compile_features(KHRHumanoidImporterTests PUBLIC cxx_std_20)
target_link_libraries(KHRHumanoidImporterTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
if(CGLTF_INCLUDE_DIRS_TESTS)
    target_include_directories(KHRHumanoidImporterTests PRIVATE ${CGLTF_INCLUDE_DIRS_TESTS})
endif()
ge_set_output_tests(KHRHumanoidImporterTests)
ge_stage_runtime_dependencies(KHRHumanoidImporterTests)
gtest_discover_tests(KHRHumanoidImporterTests)

add_executable(VRM1ImporterTests
    VRM1ImporterTests.cpp
    cgltf_impl_test.cpp
)
target_compile_features(VRM1ImporterTests PUBLIC cxx_std_20)
target_link_libraries(VRM1ImporterTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
if(CGLTF_INCLUDE_DIRS_TESTS)
    target_include_directories(VRM1ImporterTests PRIVATE ${CGLTF_INCLUDE_DIRS_TESTS})
endif()
ge_set_output_tests(VRM1ImporterTests)
ge_stage_runtime_dependencies(VRM1ImporterTests)
gtest_discover_tests(VRM1ImporterTests)

add_executable(AutoImportDispatchTests
    AutoImportDispatchTests.cpp
    cgltf_impl_test.cpp
)
target_compile_features(AutoImportDispatchTests PUBLIC cxx_std_20)
target_link_libraries(AutoImportDispatchTests PRIVATE GTest::gtest GTest::gtest_main Animation AssetCore Logger GenerationalVector)
if(CGLTF_INCLUDE_DIRS_TESTS)
    target_include_directories(AutoImportDispatchTests PRIVATE ${CGLTF_INCLUDE_DIRS_TESTS})
endif()
ge_set_output_tests(AutoImportDispatchTests)
ge_stage_runtime_dependencies(AutoImportDispatchTests)
gtest_discover_tests(AutoImportDispatchTests)

# Stage the engine-shipped HumanoidStandard.profile.json next to the test
# exe so the test is hermetic — no cwd assumptions, no candidate fallbacks,
# no dependence on the Editor target's POST_BUILD step. The test reads it
# via a path relative to its own exe location.
add_custom_command(TARGET HumanoidStandardProfileTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/Assets/SkeletonProfiles/HumanoidStandard.profile.json
        $<TARGET_FILE_DIR:HumanoidStandardProfileTests>/TestAssets/SkeletonProfiles/HumanoidStandard.profile.json
    COMMENT "Staging HumanoidStandard.profile.json for HumanoidStandardProfileTests"
    VERBATIM
)

gtest_discover_tests(HumanoidStandardProfileTests
    WORKING_DIRECTORY $<TARGET_FILE_DIR:HumanoidStandardProfileTests>
)
