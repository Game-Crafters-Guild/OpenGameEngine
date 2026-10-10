// GPU compute skinning parity: the production AnimationComputePass, declared on a
// real render-graph frame and fed by GPUAnimationDataStore, must write the same
// skin matrices into the palette atlas as the CPU SampleAnimationPose reference,
// from a three-bone chain up to the kernel's 256-bone and 64-level limits, for
// rigs whose nodes outside the joint closure the kernel never evaluates, on the
// desktop kernel and on its compat-profile arm (the one the web cook translates).
// The store's refusal of what the kernel cannot hold needs no device.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Common/Utils.h"
#include "Engine/Rendering/AnimationComputePass.h"
#include "Engine/Rendering/BonePaletteLayout.h"
#include "Engine/Rendering/GPUAnimationDataStore.h"
#include "Engine/Rendering/AnimationSampling.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "Assets/AnimationClip.h"
#include "StagedTestPaths.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <ostream>
#include <string>
#include <tuple>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
namespace RG = GameEngine::Rendering::RenderGraph;

namespace
{
constexpr uint32 kChainJointCount = 3;
constexpr float kMatrixTolerance = 1e-4f;
constexpr uint32 kUploadRingFramesInFlight = 2;
constexpr uint64_t kUploadRingSlotBytes = 64 * 1024;

struct FramePools
{
    RG::RGResourcePool Persistent;
    RG::RGTransientPool Transient;
    RG::RGUploadRing Upload;
    explicit FramePools(IDevice* device)
        : Persistent(device), Transient(device),
          Upload(device, kUploadRingFramesInFlight, kUploadRingSlotBytes) {}
};

// Three-bone chain 0 -> 1 -> 2; bones 1 and 2 sit one unit up +Y from their parent.
// Bind pose equals rest pose, so every inverse bind is identity.
uint32 CreateTestSkeleton()
{
    auto& store = SkeletonStore::Instance();
    const uint32 skeletonId = store.CreateSkeleton(kChainJointCount);
    auto* skeleton = store.Get(skeletonId);
    skeleton->BoneCount = kChainJointCount;
    skeleton->SkinJointCount = kChainJointCount;
    skeleton->Parent = {-1, 0, 1};
    skeleton->JointNodes = {0, 1, 2};
    skeleton->RestTranslation = {0, 0, 0, 0, 1, 0, 0, 1, 0};
    skeleton->RestRotation = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    skeleton->RestScale = {1, 1, 1, 1, 1, 1, 1, 1, 1};

    const glm::mat4 identity(1.0f);
    skeleton->RestLocalMatrix.resize(kChainJointCount * 16);
    skeleton->InverseBind.resize(kChainJointCount * 16);
    for (uint32 i = 0; i < kChainJointCount; ++i)
    {
        glm::mat4 local(1.0f);
        if (i > 0)
            local = glm::translate(local, glm::vec3(0.0f, 1.0f, 0.0f));
        std::memcpy(&skeleton->RestLocalMatrix[i * 16], glm::value_ptr(local), sizeof(glm::mat4));
        std::memcpy(&skeleton->InverseBind[i * 16], glm::value_ptr(identity), sizeof(glm::mat4));
    }
    std::memcpy(skeleton->MeshRootWorld, glm::value_ptr(identity), sizeof(glm::mat4));
    std::memcpy(skeleton->SkeletonRootWorld, glm::value_ptr(identity), sizeof(glm::mat4));
    skeleton->ComputeTopologicalSort();
    return skeletonId;
}

AnimKeyframe TranslationKey(float time, float x)
{
    AnimKeyframe key{};
    key.time = time;
    key.translation[0] = x;
    key.rotation[3] = 1.0f;
    key.scale[0] = key.scale[1] = key.scale[2] = 1.0f;
    return key;
}

AnimKeyframe RotationKey(float time, const glm::quat& rotation)
{
    AnimKeyframe key{};
    key.time = time;
    key.rotation[0] = rotation.x;
    key.rotation[1] = rotation.y;
    key.rotation[2] = rotation.z;
    key.rotation[3] = rotation.w;
    key.scale[0] = key.scale[1] = key.scale[2] = 1.0f;
    return key;
}

// `node` translates linearly from X = 0 at t = 0 to X = endX at t = 1.
AnimChannel TranslationChannel(uint32 node, float endX)
{
    AnimChannel channel{};
    channel.boneIndex = node;
    channel.path = AnimPath::Translation;
    channel.interp = AnimInterp::Linear;
    channel.keys = {TranslationKey(0.0f, 0.0f), TranslationKey(1.0f, endX)};
    return channel;
}

// `node` turns from identity at t = 0 to 0.6 rad at t = 1, about an axis that changes with the node.
AnimChannel RotationChannel(uint32 node)
{
    const glm::vec3 axis(static_cast<float>(node % 2), 1.0f, static_cast<float>(node % 5) * 0.2f);
    AnimChannel channel{};
    channel.boneIndex = node;
    channel.path = AnimPath::Rotation;
    channel.interp = AnimInterp::Linear;
    channel.keys = {RotationKey(0.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)),
                    RotationKey(1.0f, glm::angleAxis(0.6f, glm::normalize(axis)))};
    return channel;
}

uint32 RegisterClip(std::vector<AnimChannel> channels)
{
    const GUID clipGuid = GUID::Generate();
    auto clip = MakeShared<AnimationClip>(clipGuid, std::filesystem::path("Synthetic://GPUTest"));
    clip->SetChannelsAndDurationForTest(std::move(channels), 1.0f);
    return ClipStore::Instance().RegisterRuntimeClip(clipGuid, std::move(clip));
}

// A skeleton and a clip registered in the stores, and the number of skin joints the skeleton writes.
struct Rig
{
    uint32 SkeletonId = 0;
    uint32 ClipIndex = 0;
    uint32 JointCount = 0;
};

// The three-bone chain; bone 0 translates from X = 0 at t = 0 to X = 2 at t = 1.
Rig CreateChainRig()
{
    return {CreateTestSkeleton(), RegisterClip({TranslationChannel(0, 2.0f)}), kChainJointCount};
}

// Every node sits 0.1 up +Y from its parent, turned 0.05 rad about an axis that changes with the
// node, so the hierarchy composes rotations as well as offsets. Inverse binds are distinct
// translations, so a joint that reads another joint's inverse bind shows in its palette entry.
void FillRigSkeleton(SkeletonData* skeleton, const std::vector<int32>& parent, const std::vector<uint32>& jointNodes)
{
    const uint32 nodeCount = static_cast<uint32>(parent.size());
    skeleton->BoneCount = nodeCount;
    skeleton->Parent = parent;
    skeleton->SkinJointCount = static_cast<uint32>(jointNodes.size());
    skeleton->JointNodes = jointNodes;
    skeleton->RestTranslation.assign(nodeCount * 3, 0.0f);
    skeleton->RestRotation.assign(nodeCount * 4, 0.0f);
    skeleton->RestScale.assign(nodeCount * 3, 1.0f);
    skeleton->RestLocalMatrix.resize(nodeCount * 16);
    skeleton->InverseBind.resize(nodeCount * 16);
    for (uint32 node = 0; node < nodeCount; ++node)
    {
        const glm::vec3 translation(0.0f, 0.1f, 0.0f);
        const glm::vec3 axis(1.0f, static_cast<float>(node % 3), static_cast<float>(node % 7) * 0.1f + 0.1f);
        const glm::quat rotation = glm::angleAxis(0.05f, glm::normalize(axis));
        skeleton->RestTranslation[node * 3 + 1] = translation.y;
        skeleton->RestRotation[node * 4 + 0] = rotation.x;
        skeleton->RestRotation[node * 4 + 1] = rotation.y;
        skeleton->RestRotation[node * 4 + 2] = rotation.z;
        skeleton->RestRotation[node * 4 + 3] = rotation.w;
        const glm::mat4 local = glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation);
        const glm::vec3 inverseBindOffset(-0.01f * static_cast<float>(node), -0.02f * static_cast<float>(node % 5), 0.03f);
        const glm::mat4 inverseBind = glm::translate(glm::mat4(1.0f), inverseBindOffset);
        std::memcpy(&skeleton->RestLocalMatrix[node * 16], glm::value_ptr(local), sizeof(glm::mat4));
        std::memcpy(&skeleton->InverseBind[node * 16], glm::value_ptr(inverseBind), sizeof(glm::mat4));
    }
    skeleton->ComputeTopologicalSort();
}

uint32 CreateRigSkeleton(const std::vector<int32>& parent, const std::vector<uint32>& jointNodes)
{
    auto& store = SkeletonStore::Instance();
    const uint32 skeletonId = store.CreateSkeleton(static_cast<uint32>(parent.size()));
    FillRigSkeleton(store.Get(skeletonId), parent, jointNodes);
    return skeletonId;
}

// Node 0 is the root of every other node.
std::vector<int32> FlatParents(uint32 nodeCount)
{
    std::vector<int32> parent(nodeCount, 0);
    parent[0] = -1;
    return parent;
}

// Node n hangs off node n - 1: one hierarchy level per node.
std::vector<int32> ChainParents(uint32 nodeCount)
{
    std::vector<int32> parent(nodeCount);
    for (uint32 node = 0; node < nodeCount; ++node)
        parent[node] = static_cast<int32>(node) - 1;
    return parent;
}

// A skin whose joint j is node j.
std::vector<uint32> EveryNodeAJoint(uint32 nodeCount)
{
    std::vector<uint32> jointNodes(nodeCount);
    for (uint32 node = 0; node < nodeCount; ++node)
        jointNodes[node] = node;
    return jointNodes;
}

// `jointCount` joints in a hand-like tree: joints 0-15 form a spine chain and every further joint
// belongs to a five-joint finger whose first joint hangs off a spine joint, so 256 joints stand
// 21 levels deep. Joint 0 translates and every fourth joint turns.
Rig CreateFingerRig(uint32 jointCount)
{
    constexpr uint32 kSpineJoints = 16;
    constexpr uint32 kFingerJoints = 5;
    std::vector<int32> parent(jointCount, -1);
    std::vector<uint32> jointNodes(jointCount);
    std::vector<AnimChannel> channels = {TranslationChannel(0, 1.0f)};
    for (uint32 joint = 1; joint < jointCount; ++joint)
    {
        const bool fingerRoot = joint >= kSpineJoints && (joint - kSpineJoints) % kFingerJoints == 0;
        parent[joint] = fingerRoot ? static_cast<int32>(((joint - kSpineJoints) / kFingerJoints) % kSpineJoints)
                                   : static_cast<int32>(joint - 1);
        if (joint % 4 == 3)
            channels.push_back(RotationChannel(joint));
    }
    for (uint32 joint = 0; joint < jointCount; ++joint)
        jointNodes[joint] = joint;
    return {CreateRigSkeleton(parent, jointNodes), RegisterClip(std::move(channels)), jointCount};
}

Rig CreateTwoHundredJointRig() { return CreateFingerRig(200); }

Rig CreateTwoHundredFiftySixJointRig() { return CreateFingerRig(256); }

// 300 nodes, 100 of them joints. Node 0 is the armature: no joint, every joint's ancestor, and it
// translates. Joint k is node 1 + 3k; joint 0 hangs off the armature and the other 99 off joint 0,
// one hierarchy level wider than a workgroup. Node 2 + 3k is a socket under joint k and node 3 + 3k
// a root of its own; neither is in the joint closure, and the clip turns socket 2 and moves root 3.
// The skin lists the joints in reverse node order.
Rig CreateDecoratedRig()
{
    constexpr uint32 kNodeCount = 300;
    constexpr uint32 kJointCount = 100;
    std::vector<int32> parent(kNodeCount, -1);
    std::vector<uint32> jointNodes(kJointCount);
    std::vector<AnimChannel> channels = {TranslationChannel(0, 1.0f), RotationChannel(2), TranslationChannel(3, 2.0f)};
    for (uint32 joint = 0; joint < kJointCount; ++joint)
    {
        const uint32 node = 1 + 3 * joint;
        parent[node] = (joint == 0) ? 0 : 1;
        parent[node + 1] = static_cast<int32>(node);
        jointNodes[kJointCount - 1 - joint] = node;
        if (joint % 4 == 0)
            channels.push_back(RotationChannel(node));
    }
    return {CreateRigSkeleton(parent, jointNodes), RegisterClip(std::move(channels)), kJointCount};
}

// 64 joints in one chain, the kernel's level limit. Joint 0 translates and every third joint turns.
Rig CreateSixtyFourLevelChain()
{
    constexpr uint32 kLevels = 64;
    std::vector<AnimChannel> channels = {TranslationChannel(0, 1.0f)};
    for (uint32 node = 2; node < kLevels; node += 3)
        channels.push_back(RotationChannel(node));
    return {CreateRigSkeleton(ChainParents(kLevels), EveryNodeAJoint(kLevels)), RegisterClip(std::move(channels)),
            kLevels};
}

// 180 nodes in which every parent has a higher node index than its child: node n hangs off node
// n + 4 + n % 7 (clamped to the root, node 179). The skin lists its joints in node order, so children
// come first, and leaves out every fifth node, which the joints below it pull into the closure. The
// root translates and every fourth node turns.
Rig CreateReversedNodeOrderRig()
{
    constexpr uint32 kNodeCount = 180;
    std::vector<int32> parent(kNodeCount, -1);
    std::vector<uint32> jointNodes;
    std::vector<AnimChannel> channels = {TranslationChannel(kNodeCount - 1, 1.0f)};
    for (uint32 node = 0; node + 1 < kNodeCount; ++node)
        parent[node] = static_cast<int32>(std::min(kNodeCount - 1, node + 4 + node % 7));
    for (uint32 node = 0; node < kNodeCount; ++node)
    {
        if (node % 5 != 2)
            jointNodes.push_back(node);
        if (node % 4 == 3)
            channels.push_back(RotationChannel(node));
    }
    const uint32 jointCount = static_cast<uint32>(jointNodes.size());
    return {CreateRigSkeleton(parent, jointNodes), RegisterClip(std::move(channels)), jointCount};
}

std::vector<float> SampleCpuReference(uint32 skeletonId, uint32 clipIndex, float time)
{
    const auto* skeleton = SkeletonStore::Instance().Get(skeletonId);
    const auto clip = ClipStore::Instance().Get(clipIndex);
    std::vector<float> skinMatrices;
    PoseSampleWorkspace workspace;
    SampleAnimationPose(*skeleton, clip.get(), time, nullptr, &skinMatrices, workspace);
    return skinMatrices;
}

// Mirrors AnimationSystem's GPU instance for a runtime whose palette starts at atlas offset 0.
GPUAnimInstance MakeInstance(uint32 skeletonGpuIndex, uint32 clipGpuIndex, float time)
{
    GPUAnimInstance instance{};
    instance.skeletonGPUIndex = skeletonGpuIndex;
    instance.clipGPUIndex = clipGpuIndex;
    instance.time = time;
    instance.atlasOutputOffset = 0;
    return instance;
}

BufferHandle CreateReadbackAtlas(IDevice& device, size_t sizeBytes)
{
    BufferDesc desc{};
    desc.size = sizeBytes;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferSrc | BufferUsage::TransferDst);
    desc.memoryUsage = BufferMemoryUsage::Readback;
    desc.flags = BufferCreateFlags::PersistentlyMapped;
    desc.debugName = "GPUAnimationComputeTest.Atlas";
    return device.CreateBuffer(desc);
}

// The atlas stores each bone as three packed rows; the CPU reference is column-major mat4.
std::vector<float> UnpackAtlas(const std::vector<float>& packedRows, uint32 jointCount)
{
    using namespace BonePaletteLayout;
    std::vector<float> matrices(jointCount * 16);
    for (uint32 joint = 0; joint < jointCount; ++joint)
        UnpackRowsToMat4(packedRows.data() + joint * kFloatsPerBone, matrices.data() + joint * 16);
    return matrices;
}

struct SampleCase
{
    const char* Name;
    Rig (*CreateRig)();
    float Time;
};

// A build of animation_skinning.comp staged under <build>/Shaders.
struct Kernel
{
    const char* Name;
    const char* File;
};

// The kernel every desktop profile loads, and its GE_COMPAT_PROFILE arm: the constant-bound, masked
// level loop the web cook translates to WGSL, built for Vulkan so this suite runs it.
constexpr Kernel kDesktopKernel{"Desktop", "animation_skinning.comp.spv"};
constexpr Kernel kCompatKernel{"Compat", "Tests/animation_skinning_compat.comp.spv"};

using KernelCase = std::tuple<SampleCase, Kernel>;

void PrintTo(const SampleCase& sampleCase, std::ostream* os) { *os << sampleCase.Name; }

void PrintTo(const Kernel& kernel, std::ostream* os) { *os << kernel.Name; }

std::string KernelCaseName(const ::testing::TestParamInfo<KernelCase>& info)
{
    return std::string(std::get<0>(info.param).Name) + "_" + std::get<1>(info.param).Name;
}

constexpr uint32 kUnknownSkeletonId = 0x7FFFFFF0u;

} // namespace

class GPUAnimationComputeTest : public ::testing::TestWithParam<KernelCase>
{
protected:
    void SetUp() override
    {
        DeviceDesc desc{};
        desc.applicationName = "GPUAnimationComputeTest";
        desc.preferredAPI = GraphicsAPI::Vulkan;
        desc.enableDebugLayer = true;
        m_Device = DeviceFactory::CreateDevice(desc);
        if (!m_Device || !m_Device->Initialize(desc))
            m_Device.reset();
    }

    void TearDown() override
    {
        m_DataStore.Shutdown();
        if (m_Device)
            m_Device->Shutdown();
        m_Device.reset();
    }

    // One production dispatch: the data store uploads skeleton, clip and instance,
    // AnimationComputePass declares its pass on a render-graph frame, the frame executes.
    std::vector<float> DispatchProductionPass(const Kernel& kernel, const Rig& rig, float time)
    {
        const std::vector<uint8_t> shader = Utils::ReadFile(
            (TestPaths::StagedRoot() / "Shaders" / kernel.File).string());
        EXPECT_FALSE(shader.empty()) << kernel.File << " is not staged under <build>/Shaders";
        if (shader.empty())
            return {};

        EXPECT_TRUE(m_DataStore.Initialize(m_Device.get()));
        m_DataStore.BeginFrame(0);
        const uint32 skeletonGpuIndex = m_DataStore.EnsureSkeletonUploaded(rig.SkeletonId);
        const uint32 clipGpuIndex = m_DataStore.EnsureClipUploaded(rig.ClipIndex);
        m_DataStore.AddInstance(MakeInstance(skeletonGpuIndex, clipGpuIndex, time));

        std::vector<float> packedRows(rig.JointCount * BonePaletteLayout::kFloatsPerBone);
        const size_t atlasBytes = packedRows.size() * sizeof(float);
        const BufferHandle atlas = CreateReadbackAtlas(*m_Device, atlasBytes);
        EXPECT_TRUE(atlas.IsValid());
        if (!atlas.IsValid())
            return {};

        AnimationComputePass pass;
        pass.SetComputeShader(shader);
        pass.SetDataStore(&m_DataStore);
        {
            FramePools pools(m_Device.get());
            RG::RGFrame frame(m_Device.get(), &pools.Persistent, &pools.Transient, &pools.Upload);
            frame.BeginFrame(0);
            const RG::RGBuffer atlasResource = frame.ImportExternalBuffer("SkinPaletteAtlas", atlas, atlasBytes);
            pass.Declare(frame, atlasResource, RG::RGBuffer{});
            EXPECT_EQ(frame.Graph().PassCount(), 1u) << "AnimationComputePass declared no pass";
            frame.Execute();
            m_Device->WaitForIdle();
        }

        if (const void* mapped = m_Device->MapBuffer(atlas))
            std::memcpy(packedRows.data(), mapped, atlasBytes);
        m_Device->UnmapBuffer(atlas);
        m_Device->DestroyBuffer(atlas);
        return UnpackAtlas(packedRows, rig.JointCount);
    }

    std::unique_ptr<IDevice> m_Device;
    GPUAnimationDataStore m_DataStore;
};

TEST_P(GPUAnimationComputeTest, SkinMatricesMatchCpuReference)
{
    if (!m_Device)
        GTEST_SKIP() << "No Vulkan device available";

    const auto& [sampleCase, kernel] = GetParam();
    const Rig rig = sampleCase.CreateRig();
    const float time = sampleCase.Time;

    const std::vector<float> expected = SampleCpuReference(rig.SkeletonId, rig.ClipIndex, time);
    ASSERT_EQ(expected.size(), rig.JointCount * 16u);
    const std::vector<float> actual = DispatchProductionPass(kernel, rig, time);
    ASSERT_EQ(actual.size(), expected.size());

    for (size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_NEAR(actual[i], expected[i], kMatrixTolerance)
            << "joint " << (i / 16) << ", column " << (i % 16) / 4 << ", row " << (i % 4);
    }
}

// RestPose: at t = 0 the clip leaves bone 0 at the origin, so the output is the rest chain.
// AnimatedTranslation: at t = 0.5 bone 0 has moved +1 in X and its children follow.
// TwoHundredJoints and TwoHundredFiftySixJoints: finger rigs past 128 bones and at the kernel's
// 256-bone limit. DecorationNodesOutsideTheJointClosure: 300 nodes skinned through 101 bones.
// SixtyFourLevelChain: the kernel's level limit. ReversedNodeOrder: every parent after its child in
// node order, children first in the skin, and nodes outside the skin inside the closure.
INSTANTIATE_TEST_SUITE_P(SampleTimes, GPUAnimationComputeTest,
                         ::testing::Combine(
                             ::testing::Values(SampleCase{"RestPose", CreateChainRig, 0.0f},
                                               SampleCase{"AnimatedTranslation", CreateChainRig, 0.5f},
                                               SampleCase{"TwoHundredJoints", CreateTwoHundredJointRig, 0.5f},
                                               SampleCase{"TwoHundredFiftySixJoints", CreateTwoHundredFiftySixJointRig, 0.5f},
                                               SampleCase{"DecorationNodesOutsideTheJointClosure", CreateDecoratedRig, 0.5f},
                                               SampleCase{"SixtyFourLevelChain", CreateSixtyFourLevelChain, 0.5f},
                                               SampleCase{"ReversedNodeOrder", CreateReversedNodeOrderRig, 0.5f}),
                             ::testing::Values(kDesktopKernel, kCompatKernel)),
                         KernelCaseName);

// Without a device the store packs a skeleton and uploads nothing, so its answers need no GPU.
TEST(GPUAnimationDataStoreTest, RefusesSkeletonsTheKernelCannotHold)
{
    GPUAnimationDataStore store;
    const uint32 atBoneLimit = CreateRigSkeleton(FlatParents(256), EveryNodeAJoint(256));
    EXPECT_NE(store.EnsureSkeletonUploaded(atBoneLimit), GPUAnimationDataStore::kNotUploaded);
    EXPECT_EQ(store.EnsureSkeletonUploaded(CreateRigSkeleton(FlatParents(257), EveryNodeAJoint(257))),
              GPUAnimationDataStore::kNotUploaded);
    EXPECT_EQ(store.EnsureSkeletonUploaded(CreateRigSkeleton(ChainParents(65), EveryNodeAJoint(65))),
              GPUAnimationDataStore::kNotUploaded);
    EXPECT_EQ(store.EnsureSkeletonUploaded(kUnknownSkeletonId), GPUAnimationDataStore::kNotUploaded)
        << "a missing skeleton must not answer with another skeleton's slot";

    // A reload refills the skeleton under its id. The store answers from its cache until the
    // skeleton is invalidated, then packs it again and refuses it.
    auto& skeletons = SkeletonStore::Instance();
    skeletons.EnsureSizes(atBoneLimit, 300);
    FillRigSkeleton(skeletons.Get(atBoneLimit), FlatParents(300), EveryNodeAJoint(300));
    EXPECT_NE(store.EnsureSkeletonUploaded(atBoneLimit), GPUAnimationDataStore::kNotUploaded);
    store.InvalidateSkeleton(atBoneLimit);
    EXPECT_EQ(store.EnsureSkeletonUploaded(atBoneLimit), GPUAnimationDataStore::kNotUploaded);
}
