cmake_minimum_required(VERSION 3.20)
project(Animation LANGUAGES CXX)

# Public headers
file(GLOB_RECURSE ANIMATION_PUBLIC_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/Include/Animation/*.h
)

set(ANIMATION_SOURCES
    Source/AnimationEvent.cpp
    Source/AnimationEventCollectorStore.cpp
    Source/AnimationController.cpp
    Source/AnimationGraphSerializer.cpp
    Source/AnimationGraphStore.cpp
    Source/AnimationLibrary.cpp
    Source/AnimationMontage.cpp
    Source/AnimationPlayer.cpp
    Source/AnimationPose.cpp
    Source/AnimationTimeline.cpp
    Source/BoneMask.cpp
    Source/HumanBone.cpp
    Source/HumanoidImport.cpp
    Source/HumanoidNameMatcher.cpp
    Source/HumanoidRig.cpp
    Source/HumanoidRigEdit.cpp
    Source/KHRHumanoidImporter.cpp
    Source/KHRHumanoidShared.cpp
    Source/MontageInstance.cpp
    Source/OpStackNode.cpp
    Source/OpStackRegistration.cpp
    Source/PoseToSkinMatrices.cpp
    Source/PoseUtilities.cpp
    Source/RetargetAssetWatcher.cpp
    Source/RetargetMap.cpp
    Source/RootMotion.cpp
    Source/RootMotionExtractor.cpp
    Source/SkeletonData.cpp
    Source/SkeletonModifierStack.cpp
    Source/SkeletonProfile.cpp
    Source/SpriteFrames.cpp
    Source/TweenService.cpp
    Source/VRM1Importer.cpp
    Source/Nodes/AdditiveBlendNode.cpp
    Source/Nodes/Blend2Node.cpp
    Source/Nodes/BlendSpace1DNode.cpp
    Source/Nodes/BlendSpace2DNode.cpp
    Source/Nodes/ClipPlayerNode.cpp
    Source/Nodes/FABRIKNode.cpp
    Source/Nodes/FootIKNode.cpp
    Source/Nodes/LayeredBlendNode.cpp
    Source/Nodes/LookAtNode.cpp
    Source/Nodes/MontageSlotNode.cpp
    Source/Nodes/RetargetNode.cpp
    Source/Nodes/StateMachineNode.cpp
    Source/Nodes/TwoBoneIKNode.cpp
    Source/Ops/AttachmentPassthroughOp.cpp
    Source/Ops/FootLockOp.cpp
    Source/Ops/LookAtOp.cpp
)

add_library(Animation OBJECT ${ANIMATION_PUBLIC_HEADERS} ${ANIMATION_SOURCES})

set_target_properties(Animation PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "Animation"
)

target_include_directories(Animation
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Include
)

find_package(nlohmann_json CONFIG REQUIRED)

target_link_libraries(Animation
    PUBLIC
        Mathematics
        Types
        AssetCore
        GenerationalVector
        nlohmann_json::nlohmann_json
    PRIVATE
        Logger
)

# cgltf is header-only via vcpkg. Animation only INCLUDES cgltf.h (no
# CGLTF_IMPLEMENTATION); the symbols are provided by Engine's
# `Source/ThirdParty/cgltf_impl.cpp` for runtime linkage. Animation tests
# link a per-test `cgltf_impl_test.cpp` for hermeticity (see
# Tests/CMakeLists.txt).
find_path(CGLTF_INCLUDE_DIRS "cgltf.h")
if(CGLTF_INCLUDE_DIRS)
    target_include_directories(Animation PRIVATE ${CGLTF_INCLUDE_DIRS})
    target_compile_definitions(Animation PRIVATE GE_HAVE_CGLTF)
endif()

if(BUILD_TESTING)
    add_subdirectory(Tests)
endif()
