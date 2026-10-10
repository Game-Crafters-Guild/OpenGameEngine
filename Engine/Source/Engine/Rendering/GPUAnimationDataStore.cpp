#include "Engine/Rendering/GPUAnimationDataStore.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "Assets/AnimationClip.h"
#include "Rendering/Core/Device.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>

namespace GameEngine { namespace Engine { namespace Renderer {

using BufferHandle = GameEngine::Rendering::BufferHandle;
using BufferDesc   = GameEngine::Rendering::BufferDesc;
using BufferUsage  = GameEngine::Rendering::BufferUsage;
using BufferMemoryUsage = GameEngine::Rendering::BufferMemoryUsage;
using BufferCreateFlags = GameEngine::Rendering::BufferCreateFlags;

// Initial buffer capacities — start small, grow on demand.
// Grow-on-demand is implemented in CreateOrGrowBuffer (doubles capacity).
// Keeping initial small avoids committing ~20 MB of host-visible memory on startup.
static constexpr size_t kSkelHeaderInitial = 8 * sizeof(GPUSkeletonHeader);    // 1 KB (8 skeletons)
static constexpr size_t kSkelDataInitial   = 64u << 10;                        // 64 KB
static constexpr size_t kClipHeaderInitial = 16 * sizeof(GPUClipHeader);       // 1 KB (16 clips)
static constexpr size_t kClipDataInitial   = 256u << 10;                       // 256 KB
static constexpr size_t kInstanceRingSize  = 32u << 10;                        // 32 KB (1024 instances)

// A joint that names no skeleton node, or a node outside the joint closure.
static constexpr uint32 kNoBone = UINT32_MAX;

// ----------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------

bool GPUAnimationDataStore::Initialize(GameEngine::Rendering::IDevice* device)
{
    if (!device) return false;
    m_Device = device;

    m_SkelHeaderBuf = CreateOrGrowBuffer({}, m_SkelHeaderCapacity, kSkelHeaderInitial, "GPUAnimSkelHeaders");
    m_SkelDataBuf   = CreateOrGrowBuffer({}, m_SkelDataCapacity,   kSkelDataInitial,   "GPUAnimSkelData");
    m_ClipHeaderBuf = CreateOrGrowBuffer({}, m_ClipHeaderCapacity, kClipHeaderInitial,  "GPUAnimClipHeaders");
    m_ClipDataBuf   = CreateOrGrowBuffer({}, m_ClipDataCapacity,   kClipDataInitial,    "GPUAnimClipData");

    // One ring slot per device frame-in-flight: the ring is rotated by the
    // device frame index (RenderingLoop BeginFrame), so fewer slots than the
    // device paces would hand a slot back to the CPU while the GPU still
    // reads it — a hardcoded 2 under 3-frame pacing put consecutive frames
    // on the same slot (0,1,0|0,1,0), tearing GPUAnimInstance records under
    // GPU-bound load.
    if (!m_InstanceAllocator.Initialize(device, kInstanceRingSize, BufferUsage::Storage,
                                        device->GetFramesInFlight(), 32, "GPUAnimInstances"))
        return false;

    return m_SkelHeaderBuf.IsValid() && m_SkelDataBuf.IsValid() &&
           m_ClipHeaderBuf.IsValid() && m_ClipDataBuf.IsValid();
}

void GPUAnimationDataStore::Shutdown()
{
    if (!m_Device) return;
    m_InstanceAllocator.Shutdown();
    if (m_SkelHeaderBuf.IsValid()) m_Device->DestroyBuffer(m_SkelHeaderBuf);
    if (m_SkelDataBuf.IsValid())   m_Device->DestroyBuffer(m_SkelDataBuf);
    if (m_ClipHeaderBuf.IsValid()) m_Device->DestroyBuffer(m_ClipHeaderBuf);
    if (m_ClipDataBuf.IsValid())   m_Device->DestroyBuffer(m_ClipDataBuf);
    m_Device = nullptr;
}

void GPUAnimationDataStore::ReprovisionAfterDeviceRebuild()
{
    if (!m_Device)
        return;

    // Zero the dead persistent SSBO handles + capacities. Skipping DestroyBuffer here
    // is not to dodge a double-free (a stale Destroy* is a generational no-op) — it is
    // that CreateOrGrowBuffer's grow path calls DestroyBuffer on `existing` when it
    // IsValid(), and its capacity-check early-return would otherwise reuse the dead
    // handle; zeroing forces a clean recreate. The CPU mirrors + id->GPU-offset caches
    // survive, so re-flushing restores byte-identical content and ids still resolve.
    // Empty-scene asymmetry (intentional): with no skeletons/clips loaded the mirrors
    // are empty, so the dirty flags stay false and the SSBOs are NOT recreated here —
    // they lazily re-allocate on the first EnsureSkeletonUploaded after resume. The
    // instance ring below is always re-created because it is written every frame.
    m_SkelHeaderBuf = m_SkelDataBuf = m_ClipHeaderBuf = m_ClipDataBuf = BufferHandle{};
    m_SkelHeaderCapacity = m_SkelDataCapacity = m_ClipHeaderCapacity = m_ClipDataCapacity = 0;
    m_SkelDirty = !m_SkelHeaders.empty();
    m_ClipDirty = !m_ClipHeaders.empty();
    FlushSkeletonBuffers();
    FlushClipBuffers();

    // The per-frame instance ring's backing buffers are dead too; re-create them so
    // the next BeginFrame/Allocate writes into live mapped memory.
    m_InstanceAllocator.ReprovisionAfterDeviceRebuild();
}

void GPUAnimationDataStore::BeginFrame(uint32_t frameIndex)
{
    m_FrameInstances.clear();
    m_GPUHandledRuntimeIds.clear();
    m_InstanceAllocator.BeginFrame(frameIndex);
    m_InstanceBufferOffset = 0;
    m_DispatchScheduledThisFrame = false;
}

// ----------------------------------------------------------------
// Buffer management
// ----------------------------------------------------------------

BufferHandle GPUAnimationDataStore::CreateOrGrowBuffer(
    BufferHandle existing, size_t& currentCapacity, size_t requiredSize, const char* debugName)
{
    if (existing.IsValid() && currentCapacity >= requiredSize)
        return existing;

    if (existing.IsValid())
        m_Device->DestroyBuffer(existing);

    size_t newCapacity = (currentCapacity == 0) ? requiredSize : currentCapacity;
    while (newCapacity < requiredSize)
        newCapacity *= 2;

    BufferDesc desc{};
    desc.size = newCapacity;
    desc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
    desc.memoryUsage = BufferMemoryUsage::Upload;
    desc.flags = BufferCreateFlags::PersistentlyMapped;
    desc.persistent = true;
    desc.debugName = debugName;

    currentCapacity = newCapacity;
    return m_Device->CreateBuffer(desc);
}

void GPUAnimationDataStore::FlushSkeletonBuffers()
{
    if (!m_SkelDirty || !m_Device) return;

    const size_t headerBytes = m_SkelHeaders.size() * sizeof(GPUSkeletonHeader);
    const size_t dataBytes   = m_SkelData.size() * sizeof(uint32);

    m_SkelHeaderBuf = CreateOrGrowBuffer(m_SkelHeaderBuf, m_SkelHeaderCapacity, headerBytes, "GPUAnimSkelHeaders");
    m_SkelDataBuf   = CreateOrGrowBuffer(m_SkelDataBuf,   m_SkelDataCapacity,   dataBytes,   "GPUAnimSkelData");

    if (headerBytes > 0)
        m_Device->UpdateBuffer(m_SkelHeaderBuf, 0, headerBytes, m_SkelHeaders.data());
    if (dataBytes > 0)
        m_Device->UpdateBuffer(m_SkelDataBuf, 0, dataBytes, m_SkelData.data());

    m_SkelDirty = false;
}

void GPUAnimationDataStore::FlushClipBuffers()
{
    if (!m_ClipDirty || !m_Device) return;

    const size_t headerBytes = m_ClipHeaders.size() * sizeof(GPUClipHeader);
    const size_t dataBytes   = m_ClipData.size() * sizeof(uint32);

    m_ClipHeaderBuf = CreateOrGrowBuffer(m_ClipHeaderBuf, m_ClipHeaderCapacity, headerBytes, "GPUAnimClipHeaders");
    m_ClipDataBuf   = CreateOrGrowBuffer(m_ClipDataBuf,   m_ClipDataCapacity,   dataBytes,   "GPUAnimClipData");

    if (headerBytes > 0)
        m_Device->UpdateBuffer(m_ClipHeaderBuf, 0, headerBytes, m_ClipHeaders.data());
    if (dataBytes > 0)
        m_Device->UpdateBuffer(m_ClipDataBuf, 0, dataBytes, m_ClipData.data());

    m_ClipDirty = false;
}

// ----------------------------------------------------------------
// Skeleton upload
// ----------------------------------------------------------------

static void PackFloat(std::vector<uint32>& buf, float v)
{
    uint32 bits;
    std::memcpy(&bits, &v, sizeof(float));
    buf.push_back(bits);
}

void GPUAnimationDataStore::InvalidateSkeleton(uint32 skeletonId)
{
    if (skeletonId == 0)
        return;
    m_SkelIdToGPU.erase(skeletonId);
}

void GPUAnimationDataStore::InvalidateAllClips()
{
    m_ClipIdToGPU.clear();
}

bool GPUAnimationDataStore::FitsKernelLimits(const SkeletonData& skeleton)
{
    return skeleton.JointClosure.size() <= kMaxBones
        && skeleton.JointClosureLevelCount() <= kMaxHierarchyLevels;
}

uint32 GPUAnimationDataStore::EnsureSkeletonUploaded(uint32 skeletonId)
{
    auto it = m_SkelIdToGPU.find(skeletonId);
    if (it != m_SkelIdToGPU.end())
        return it->second;

    auto& skStore = SkeletonStore::Instance();
    const SkeletonData* skel = skStore.Get(skeletonId);
    if (!skel || skel->JointClosure.empty() || !FitsKernelLimits(*skel))
        return kNotUploaded;

    const uint32 nodes = skel->BoneCount;
    const uint32 bones = static_cast<uint32>(skel->JointClosure.size());
    // FBX-imported skeletons commonly leave SkinJointCount==0 and JointNodes
    // empty; mirror the CPU BuildPoseToSkinMatrices fallback (joint j == bone j
    // identity, BoneCount palette entries) so the GPU shader writes a full
    // atlas instead of zero matrices. Same behavior as the
    // RetargetGPUDataStore fallback in commit 5a03eac6. The predicate matches
    // PoseToSkinMatrices.cpp:84 — fall back when the populated joint table
    // doesn't match its declared count, not just when both are zero.
    const bool jointTableValid = (skel->SkinJointCount > 0)
                              && (skel->JointNodes.size() == skel->SkinJointCount);
    const bool synthesizeJoints = !jointTableValid;
    const uint32 joints = synthesizeJoints ? nodes : skel->SkinJointCount;

    // The closure bone of each skeleton node; nodes outside the closure have none.
    std::vector<uint32> boneOfNode(nodes, kNoBone);
    for (uint32 bone = 0; bone < bones; ++bone)
        boneOfNode[skel->JointClosure[bone]] = bone;

    GPUSkeletonHeader hdr{};
    hdr.boneCount = bones;
    hdr.skinJointCount = joints;
    hdr.levelCount = skel->JointClosureLevelCount();

    // Pre-compute meshRootInverse (column-major).
    {
        const glm::mat4 meshRoot = glm::make_mat4(skel->MeshRootWorld);
        const glm::mat4 inv = glm::inverse(meshRoot);
        std::memcpy(hdr.meshRootInverse, glm::value_ptr(inv), 16 * sizeof(float));
    }

    // Pack data in closure order: parents, restTRS, restLocalMat, boneNodes;
    // then per joint: jointBones, inverseBind; then levelOffsets.

    // Parents (int32 as uint32 reinterpret). An ancestor of a closure bone is
    // itself in the closure, so every parent maps to a bone.
    hdr.parentsOffset = static_cast<uint32>(m_SkelData.size());
    for (uint32 bone = 0; bone < bones; ++bone)
    {
        const uint32 node = skel->JointClosure[bone];
        const int32 parentNode = (node < skel->Parent.size()) ? skel->Parent[node] : -1;
        const int32 parent = (parentNode >= 0 && static_cast<uint32>(parentNode) < nodes)
            ? static_cast<int32>(boneOfNode[static_cast<uint32>(parentNode)]) : -1;
        uint32 bits;
        std::memcpy(&bits, &parent, sizeof(int32));
        m_SkelData.push_back(bits);
    }

    // Rest TRS: 10 floats per bone (t3 + r4 + s3).
    hdr.restTRSOffset = static_cast<uint32>(m_SkelData.size());
    for (uint32 bone = 0; bone < bones; ++bone)
    {
        const size_t i = skel->JointClosure[bone];
        // Translation (3 floats).
        for (int c = 0; c < 3; ++c)
            PackFloat(m_SkelData, (i * 3 + c < skel->RestTranslation.size()) ? skel->RestTranslation[i * 3 + c] : 0.0f);
        // Rotation (4 floats: x,y,z,w).
        for (int c = 0; c < 4; ++c)
            PackFloat(m_SkelData, (i * 4 + c < skel->RestRotation.size()) ? skel->RestRotation[i * 4 + c] : (c == 3 ? 1.0f : 0.0f));
        // Scale (3 floats).
        for (int c = 0; c < 3; ++c)
            PackFloat(m_SkelData, (i * 3 + c < skel->RestScale.size()) ? skel->RestScale[i * 3 + c] : 1.0f);
    }

    // Rest local matrices (16 floats per bone, column-major).
    hdr.restLocalMatOffset = static_cast<uint32>(m_SkelData.size());
    for (uint32 bone = 0; bone < bones; ++bone)
    {
        for (int c = 0; c < 16; ++c)
        {
            size_t idx = static_cast<size_t>(skel->JointClosure[bone]) * 16 + c;
            PackFloat(m_SkelData, (idx < skel->RestLocalMatrix.size()) ? skel->RestLocalMatrix[idx] : ((c % 5 == 0) ? 1.0f : 0.0f));
        }
    }

    // Skeleton node per bone (uint32): the clip's channel table is per node.
    hdr.boneNodesOffset = static_cast<uint32>(m_SkelData.size());
    for (uint32 bone = 0; bone < bones; ++bone)
        m_SkelData.push_back(skel->JointClosure[bone]);

    // Joint nodes, in skin order. When synthesizing the joint mapping
    // (FBX fallback, see above), JointNodes is empty so the lookup falls
    // through to the identity j → j default.
    std::vector<uint32> jointNodes(joints);
    for (uint32 j = 0; j < joints; ++j)
        jointNodes[j] = (j < skel->JointNodes.size()) ? skel->JointNodes[j] : j;

    // Bone per joint (uint32), kNoBone for a joint that names no node.
    hdr.jointBonesOffset = static_cast<uint32>(m_SkelData.size());
    for (uint32 j = 0; j < joints; ++j)
        m_SkelData.push_back((jointNodes[j] < nodes) ? boneOfNode[jointNodes[j]] : kNoBone);

    // Inverse bind matrices (16 floats per joint, column-major).
    hdr.inverseBindOffset = static_cast<uint32>(m_SkelData.size());
    for (uint32 j = 0; j < joints; ++j)
    {
        for (int c = 0; c < 16; ++c)
        {
            size_t idx = static_cast<size_t>(jointNodes[j]) * 16 + c;
            PackFloat(m_SkelData, (idx < skel->InverseBind.size()) ? skel->InverseBind[idx] : ((c % 5 == 0) ? 1.0f : 0.0f));
        }
    }

    // Level offsets (uint32, levelCount + 1 entries).
    hdr.levelOffsetsOffset = static_cast<uint32>(m_SkelData.size());
    for (size_t i = 0; i < skel->JointClosureLevelOffsets.size(); ++i)
        m_SkelData.push_back(skel->JointClosureLevelOffsets[i]);

    const uint32 gpuIndex = static_cast<uint32>(m_SkelHeaders.size());
    m_SkelHeaders.push_back(hdr);
    m_SkelIdToGPU[skeletonId] = gpuIndex;
    m_SkelDirty = true;

    FlushSkeletonBuffers();
    return gpuIndex;
}

// ----------------------------------------------------------------
// Clip upload
// ----------------------------------------------------------------

uint32 GPUAnimationDataStore::EnsureClipUploaded(uint32 clipIndex)
{
    auto it = m_ClipIdToGPU.find(clipIndex);
    if (it != m_ClipIdToGPU.end())
        return it->second;

    auto& clipStore = ClipStore::Instance();
    auto clip = clipStore.Get(clipIndex);
    if (!clip) return 0;

    const auto& channels = clip->GetChannels();
    std::vector<AnimChannel> transformChannels;
    transformChannels.reserve(channels.size());
    for (const auto& ch : channels)
    {
        if (ch.path == AnimPath::Translation || ch.path == AnimPath::Rotation || ch.path == AnimPath::Scale)
            transformChannels.push_back(ch);
    }

    // Determine boneCount from channels (max boneIndex + 1).
    uint32 boneCount = 0;
    for (const auto& ch : transformChannels)
        boneCount = std::max(boneCount, ch.boneIndex + 1);

    // Sort channels by boneIndex, then pack per-bone lookup indices.
    struct ChannelRef { uint32 channelIdx; uint32 boneIndex; };
    std::vector<ChannelRef> sorted;
    sorted.reserve(transformChannels.size());
    for (uint32 i = 0; i < transformChannels.size(); ++i)
        sorted.push_back({i, transformChannels[i].boneIndex});
    std::sort(sorted.begin(), sorted.end(), [](const ChannelRef& a, const ChannelRef& b) {
        return a.boneIndex < b.boneIndex;
    });

    // Build per-bone channel start/count.
    std::vector<uint32> boneChannelStart(boneCount, 0);
    std::vector<uint32> boneChannelCount(boneCount, 0);
    for (uint32 i = 0; i < sorted.size(); ++i)
    {
        uint32 bone = sorted[i].boneIndex;
        if (bone < boneCount)
        {
            if (boneChannelCount[bone] == 0)
                boneChannelStart[bone] = i;
            boneChannelCount[bone]++;
        }
    }

    GPUClipHeader hdr{};
    hdr.duration = clip->GetDuration();
    hdr.channelCount = static_cast<uint32>(transformChannels.size());
    hdr.boneCount = boneCount;

    // Pack channel descriptors (sorted by bone).
    hdr.channelDescOffset = static_cast<uint32>(m_ClipData.size());
    std::vector<uint32> channelKeyframeOffsets(transformChannels.size());
    uint32 keyframeAccum = 0;
    for (uint32 si = 0; si < sorted.size(); ++si)
    {
        const auto& ch = transformChannels[sorted[si].channelIdx];
        GPUChannelDesc desc{};
        desc.boneIndex = ch.boneIndex;
        uint32 pathBits = 0;
        if (ch.path == AnimPath::Translation) pathBits = 0;
        else if (ch.path == AnimPath::Rotation) pathBits = 1;
        else pathBits = 2; // Scale
        uint32 interpBits = 0;
        if (ch.interp == AnimInterp::Step) interpBits = 0;
        else if (ch.interp == AnimInterp::Linear) interpBits = 1;
        else interpBits = 2; // CubicSpline
        desc.pathAndInterp = pathBits | (interpBits << 2);
        desc.keyframeOffset = keyframeAccum;
        desc.keyframeCount = static_cast<uint32>(ch.keys.size());
        channelKeyframeOffsets[sorted[si].channelIdx] = keyframeAccum;
        keyframeAccum += desc.keyframeCount * 28; // 28 floats per keyframe (padded)

        // Pack as 4 uint32.
        m_ClipData.push_back(desc.boneIndex);
        m_ClipData.push_back(desc.pathAndInterp);
        m_ClipData.push_back(desc.keyframeOffset);
        m_ClipData.push_back(desc.keyframeCount);
    }

    // Pack per-bone channel start/count.
    hdr.boneChannelStartOffset = static_cast<uint32>(m_ClipData.size());
    for (uint32 b = 0; b < boneCount; ++b)
        m_ClipData.push_back(boneChannelStart[b]);

    hdr.boneChannelCountOffset = static_cast<uint32>(m_ClipData.size());
    for (uint32 b = 0; b < boneCount; ++b)
        m_ClipData.push_back(boneChannelCount[b]);

    // Pack keyframe data (sorted by bone, 28 floats per keyframe).
    hdr.keyframeDataOffset = static_cast<uint32>(m_ClipData.size());
    for (uint32 si = 0; si < sorted.size(); ++si)
    {
        const auto& ch = transformChannels[sorted[si].channelIdx];
        for (const auto& key : ch.keys)
        {
            PackFloat(m_ClipData, key.time);
            PackFloat(m_ClipData, key.translation[0]);
            PackFloat(m_ClipData, key.translation[1]);
            PackFloat(m_ClipData, key.translation[2]);
            PackFloat(m_ClipData, key.rotation[0]);
            PackFloat(m_ClipData, key.rotation[1]);
            PackFloat(m_ClipData, key.rotation[2]);
            PackFloat(m_ClipData, key.rotation[3]);
            PackFloat(m_ClipData, key.scale[0]);
            PackFloat(m_ClipData, key.scale[1]);
            PackFloat(m_ClipData, key.scale[2]);
            PackFloat(m_ClipData, key.inTangent[0]);
            PackFloat(m_ClipData, key.inTangent[1]);
            PackFloat(m_ClipData, key.inTangent[2]);
            PackFloat(m_ClipData, key.inTangent[3]);
            PackFloat(m_ClipData, key.outTangent[0]);
            PackFloat(m_ClipData, key.outTangent[1]);
            PackFloat(m_ClipData, key.outTangent[2]);
            PackFloat(m_ClipData, key.outTangent[3]);
            PackFloat(m_ClipData, key.inWeight[0]);
            PackFloat(m_ClipData, key.inWeight[1]);
            PackFloat(m_ClipData, key.inWeight[2]);
            PackFloat(m_ClipData, key.inWeight[3]);
            PackFloat(m_ClipData, key.outWeight[0]);
            PackFloat(m_ClipData, key.outWeight[1]);
            PackFloat(m_ClipData, key.outWeight[2]);
            PackFloat(m_ClipData, key.outWeight[3]);
            m_ClipData.push_back(0); // pad to 28 floats (7 x vec4)
        }
    }

    const uint32 gpuIndex = static_cast<uint32>(m_ClipHeaders.size());
    m_ClipHeaders.push_back(hdr);
    m_ClipIdToGPU[clipIndex] = gpuIndex;
    m_ClipDirty = true;

    FlushClipBuffers();
    return gpuIndex;
}

// ----------------------------------------------------------------
// Per-frame instance management
// ----------------------------------------------------------------

void GPUAnimationDataStore::AddInstance(const GPUAnimInstance& inst)
{
    m_FrameInstances.push_back(inst);
}

void GPUAnimationDataStore::FlushInstances()
{
    if (m_FrameInstances.empty()) return;

    const size_t bytes = m_FrameInstances.size() * sizeof(GPUAnimInstance);
    auto alloc = m_InstanceAllocator.Allocate(bytes, 32);
    if (alloc.ptr)
    {
        std::memcpy(alloc.ptr, m_FrameInstances.data(), bytes);
        m_InstanceBufferOffset = alloc.offset;
    }
}

void GPUAnimationDataStore::MarkGPUHandled(uint32 runtimeId)
{
    m_GPUHandledRuntimeIds.insert(runtimeId);
}

bool GPUAnimationDataStore::IsGPUHandled(uint32 runtimeId) const
{
    return m_GPUHandledRuntimeIds.count(runtimeId) > 0;
}

// ----------------------------------------------------------------
// Buffer accessors
// ----------------------------------------------------------------

BufferHandle GPUAnimationDataStore::GetSkeletonHeaderBuffer() const { return m_SkelHeaderBuf; }
BufferHandle GPUAnimationDataStore::GetSkeletonDataBuffer() const   { return m_SkelDataBuf; }
BufferHandle GPUAnimationDataStore::GetClipHeaderBuffer() const     { return m_ClipHeaderBuf; }
BufferHandle GPUAnimationDataStore::GetClipDataBuffer() const       { return m_ClipDataBuf; }
BufferHandle GPUAnimationDataStore::GetInstanceBuffer() const       { return m_InstanceAllocator.GetCurrentBuffer(); }

}}} // namespace GameEngine::Engine::Renderer
