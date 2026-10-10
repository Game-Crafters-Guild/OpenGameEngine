// GPU parity test for the fused retarget_full.comp pipeline.
//
// For each scenario, builds a CPU reference (RetargetNode::Evaluate +
// BuildPoseToSkinMatrices), then dispatches retarget_full.comp on a real
// Vulkan device and reads back the SkinPaletteAtlas. Compares the resulting
// mat4s component-wise within tolerance.
//
// Tolerance: 1e-4 per matrix element. fp32 normalized quaternion chains
// over 7-deep humanoid hold this comfortably; failures here indicate a
// real math bug.
//
// Skips at runtime when no Vulkan device is available (headless CI bots) or
// when retarget_full.comp.spv is missing from the staged shader cache.

#include <gtest/gtest.h>

#include "Animation/AnimationClip.h"
#include "Animation/AnimationPose.h"
#include "Animation/QuaternionMath.h"
#include "Animation/EvaluationContext.h"
#include "Animation/HumanBone.h"
#include "Animation/HumanoidRig.h"
#include "Animation/Nodes/RetargetNode.h"
#include "Animation/PoseStack.h"
#include "Animation/PoseToSkinMatrices.h"
#include "Animation/RetargetMap.h"
#include "Animation/SkeletonData.h"

#include "AssetCore/GUID.h"
#include "Engine/Rendering/BonePaletteLayout.h"
#include "Engine/Rendering/RetargetGPUDataStore.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;
using namespace GameEngine::Engine::Renderer;
namespace AAnim = GameEngine::Animation;

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class RetargetGPUParityTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        DeviceDesc dd{};
        dd.applicationName = "RetargetGPUParityTest";
        dd.preferredAPI = GraphicsAPI::Vulkan;
        dd.enableDebugLayer = true;

        m_Device = DeviceFactory::CreateDevice(dd);
        if (!m_Device || !m_Device->Initialize(dd))
        {
            m_Device.reset();
            return;
        }

        m_ShaderSpirv = LoadShader("retarget_full.comp.spv");
    }

    void TearDown() override
    {
        m_DataStore.Shutdown();
        if (m_Device) m_Device->Shutdown();
        m_Device.reset();
    }

    bool HasDevice() const { return m_Device != nullptr; }
    bool HasShader() const { return !m_ShaderSpirv.empty(); }

    // -- Skeleton + clip + rig fixtures ----------------------------------

    // Build a 3-bone chain with identity rest. Returns owning SkeletonData.
    static AAnim::SkeletonData MakeChainSkeleton(uint32_t boneCount)
    {
        AAnim::SkeletonData s{};
        s.BoneCount = boneCount;
        s.SkinJointCount = boneCount;
        s.JointNodes.resize(boneCount);
        s.Parent.resize(boneCount, -1);
        s.RestTranslation.assign(boneCount * 3, 0.0f);
        s.RestRotation.assign(boneCount * 4, 0.0f);
        s.RestScale.assign(boneCount * 3, 1.0f);
        s.RestLocalMatrix.assign(boneCount * 16, 0.0f);
        s.BindPose.assign(boneCount * 16, 0.0f);
        s.InverseBind.assign(boneCount * 16, 0.0f);
        for (uint32_t b = 0; b < boneCount; ++b)
        {
            s.JointNodes[b] = b;
            s.Parent[b] = (b == 0) ? -1 : static_cast<int32_t>(b - 1);
            // Bone offset Y=1 from parent.
            if (b > 0) s.RestTranslation[b * 3 + 1] = 1.0f;
            // Identity rest rotation (xyzw).
            s.RestRotation[b * 4 + 3] = 1.0f;
            // Identity rest matrix + bind pose + inverse-bind.
            for (int i = 0; i < 16; ++i)
            {
                s.RestLocalMatrix[b * 16 + i] = (i % 5 == 0) ? 1.0f : 0.0f;
                s.BindPose[b * 16 + i]        = (i % 5 == 0) ? 1.0f : 0.0f;
                s.InverseBind[b * 16 + i]     = (i % 5 == 0) ? 1.0f : 0.0f;
            }
        }
        // MeshRoot/SkeletonRoot identity.
        for (int i = 0; i < 16; ++i)
        {
            s.MeshRootWorld[i]      = (i % 5 == 0) ? 1.0f : 0.0f;
            s.SkeletonRootWorld[i]  = (i % 5 == 0) ? 1.0f : 0.0f;
        }
        s.ComputeTopologicalSort();
        return s;
    }

    // Set bone b's rest rotation (xyzw).
    static void SetRestRotation(AAnim::SkeletonData& s, uint32_t b,
                                  const Mathematics::Quaternion& q)
    {
        const auto& g = q.GetGLM();
        s.RestRotation[b * 4 + 0] = g.x;
        s.RestRotation[b * 4 + 1] = g.y;
        s.RestRotation[b * 4 + 2] = g.z;
        s.RestRotation[b * 4 + 3] = g.w;
        // Update the rest local matrix to match.
        glm::mat4 m = glm::mat4_cast(g);
        if (b > 0) m[3].y = 1.0f;
        std::memcpy(&s.RestLocalMatrix[b * 16], glm::value_ptr(m), 64);
    }

    // Build a single-channel rotation animation clip on bone `boneIdx`.
    // AnimationClip is non-copyable; return by unique_ptr.
    static std::unique_ptr<AAnim::AnimationClip> MakeRotationClip(uint32_t boneIdx,
                                                                    const Mathematics::Quaternion& q0,
                                                                    const Mathematics::Quaternion& q1,
                                                                    float duration)
    {
        auto clip = std::make_unique<AAnim::AnimationClip>(
            GUID::Generate(), std::filesystem::path("Synthetic://ParityTest"));
        AAnim::AnimChannel ch{};
        ch.boneIndex = boneIdx;
        ch.path = AAnim::AnimPath::Rotation;
        ch.interp = AAnim::AnimInterp::Linear;

        AAnim::AnimKeyframe k0{};
        k0.time = 0.0f;
        const auto& g0 = q0.GetGLM();
        k0.rotation[0] = g0.x; k0.rotation[1] = g0.y; k0.rotation[2] = g0.z; k0.rotation[3] = g0.w;
        k0.scale[0] = k0.scale[1] = k0.scale[2] = 1.0f;

        AAnim::AnimKeyframe k1{};
        k1.time = duration;
        const auto& g1 = q1.GetGLM();
        k1.rotation[0] = g1.x; k1.rotation[1] = g1.y; k1.rotation[2] = g1.z; k1.rotation[3] = g1.w;
        k1.scale[0] = k1.scale[1] = k1.scale[2] = 1.0f;

        ch.keys = {k0, k1};
        clip->SetChannelsAndDurationForTest({ch}, duration);
        return clip;
    }

    // MakeRotationClip plus a hip (bone 0) translation channel from hip0 to hip1.
    static std::unique_ptr<AAnim::AnimationClip> MakeRotationAndHipClip(uint32_t boneIdx,
                                                                        const Mathematics::Quaternion& q0,
                                                                        const Mathematics::Quaternion& q1,
                                                                        const glm::vec3& hip0,
                                                                        const glm::vec3& hip1,
                                                                        float duration)
    {
        auto clip = MakeRotationClip(boneIdx, q0, q1, duration);
        std::vector<AAnim::AnimChannel> channels = clip->GetChannels();
        AAnim::AnimChannel hip{};
        hip.boneIndex = 0;
        hip.path = AAnim::AnimPath::Translation;
        hip.interp = AAnim::AnimInterp::Linear;
        AAnim::AnimKeyframe k0{};
        k0.time = 0.0f;
        k0.translation[0] = hip0.x; k0.translation[1] = hip0.y; k0.translation[2] = hip0.z;
        k0.rotation[3] = 1.0f;
        k0.scale[0] = k0.scale[1] = k0.scale[2] = 1.0f;
        AAnim::AnimKeyframe k1 = k0;
        k1.time = duration;
        k1.translation[0] = hip1.x; k1.translation[1] = hip1.y; k1.translation[2] = hip1.z;
        hip.keys = {k0, k1};
        channels.push_back(hip);
        clip->SetChannelsAndDurationForTest(channels, duration);
        return clip;
    }

    // Build a minimal HumanoidRig that maps bones 0..N-1 to canonical
    // {Hips, Spine, Head, ...}, with retargetPose Q. canonical[i] gives the
    // HumanBone enum for bone i; Q[i] gives its RetargetPoseRotation.
    static std::unique_ptr<AAnim::HumanoidRig> MakeRig(
        const std::vector<AAnim::HumanBone>& canonical,
        const std::vector<Mathematics::Quaternion>& Q)
    {
        auto rig = std::make_unique<AAnim::HumanoidRig>(GUID::Generate(), std::filesystem::path{});
        auto& bm = rig->BoneMapMutable();
        bm.resize(canonical.size());
        for (size_t i = 0; i < canonical.size(); ++i)
        {
            bm[i].Canonical = canonical[i];
            bm[i].SourceBoneName = "";  // empty -> use CachedSourceIndex
            bm[i].CachedSourceIndex = static_cast<uint32_t>(i);
            bm[i].RetargetPoseRotation = Q[i];
        }
        AAnim::HumanoidChain spine{};
        spine.Kind = AAnim::ChainKind::Spine;
        spine.IncludeBones = {AAnim::HumanBone::Hips, AAnim::HumanBone::Spine, AAnim::HumanBone::Head};
        rig->ChainsMutable() = { spine };
        rig->ProportionsMutable().HipHeight = 1.0f;
        return rig;
    }

    static std::unique_ptr<AAnim::RetargetMap> MakeMap()
    {
        auto map = std::make_unique<AAnim::RetargetMap>(GUID::Generate(), std::filesystem::path{});
        AAnim::ChainPairing p{};
        p.Kind = AAnim::ChainKind::Spine;
        p.FK.RotationMode = AAnim::FKRotationMode::OneToOne;
        p.FK.RotationAlpha = 1.0f;
        map->ChainMapMutable().push_back(p);
        return map;
    }

    // -- CPU reference -------------------------------------------------

    // An animator crossfade: the clip blended from, its time, and the weight
    // of the current clip (AnimatorRef::PrevClipIndex, PrevTime, BlendAlpha).
    struct BlendFrom
    {
        const AAnim::AnimationClip* Clip = nullptr;
        float Time = 0.0f;
        float Alpha = 0.0f;
    };

    // Sample a clip's rotation channels onto the source skeleton's rest pose.
    static AAnim::AnimationPose SampleReferencePose(const AAnim::SkeletonData& srcSkel,
                                                    const AAnim::AnimationClip& clip,
                                                    float clipTime)
    {
        AAnim::AnimationPose srcPose;
        srcPose.Resize(srcSkel.BoneCount);
        // Default: use rest rotations.
        for (uint32_t b = 0; b < srcSkel.BoneCount; ++b)
        {
            srcPose.Rotations[b] = Mathematics::Quaternion(glm::quat(
                srcSkel.RestRotation[b * 4 + 3],
                srcSkel.RestRotation[b * 4 + 0],
                srcSkel.RestRotation[b * 4 + 1],
                srcSkel.RestRotation[b * 4 + 2]));
            srcPose.Positions[b] = Mathematics::Vector3(
                srcSkel.RestTranslation[b * 3 + 0],
                srcSkel.RestTranslation[b * 3 + 1],
                srcSkel.RestTranslation[b * 3 + 2]);
            srcPose.Scales[b] = Mathematics::Vector3(1, 1, 1);
        }
        // Override animated channels.
        for (const auto& ch : clip.GetChannels())
        {
            if (ch.boneIndex >= srcSkel.BoneCount) continue;
            if (ch.path != AAnim::AnimPath::Rotation) continue;
            // Linear slerp between bracketing keys.
            const auto& keys = ch.keys;
            if (keys.empty()) continue;
            Mathematics::Quaternion q;
            if (clipTime <= keys.front().time)
                q = Mathematics::Quaternion(glm::quat(keys.front().rotation[3], keys.front().rotation[0],
                                                       keys.front().rotation[1], keys.front().rotation[2]));
            else if (clipTime >= keys.back().time)
                q = Mathematics::Quaternion(glm::quat(keys.back().rotation[3], keys.back().rotation[0],
                                                       keys.back().rotation[1], keys.back().rotation[2]));
            else
            {
                size_t hi = 0;
                while (hi < keys.size() && keys[hi].time < clipTime) ++hi;
                size_t lo = hi - 1;
                const auto& k0 = keys[lo];
                const auto& k1 = keys[hi];
                const float dt = std::max(1e-6f, k1.time - k0.time);
                const float a = std::clamp((clipTime - k0.time) / dt, 0.0f, 1.0f);
                glm::quat q0(k0.rotation[3], k0.rotation[0], k0.rotation[1], k0.rotation[2]);
                glm::quat q1(k1.rotation[3], k1.rotation[0], k1.rotation[1], k1.rotation[2]);
                if (glm::dot(q0, q1) < 0.0f) q1 = -q1;
                q = Mathematics::Quaternion(glm::normalize(glm::mix(q0, q1, a)));
            }
            srcPose.Rotations[ch.boneIndex] = q;
        }
        // Translation channels: linear between bracketing keys, clamped at the ends.
        for (const auto& ch : clip.GetChannels())
        {
            if (ch.boneIndex >= srcSkel.BoneCount || ch.path != AAnim::AnimPath::Translation || ch.keys.empty())
                continue;
            const auto& keys = ch.keys;
            const auto at = [](const AAnim::AnimKeyframe& k)
            { return glm::vec3(k.translation[0], k.translation[1], k.translation[2]); };
            glm::vec3 t;
            if (clipTime <= keys.front().time)
                t = at(keys.front());
            else if (clipTime >= keys.back().time)
                t = at(keys.back());
            else
            {
                size_t hi = 0;
                while (hi < keys.size() && keys[hi].time < clipTime) ++hi;
                const auto& k0 = keys[hi - 1];
                const auto& k1 = keys[hi];
                const float a = std::clamp((clipTime - k0.time) / std::max(1e-6f, k1.time - k0.time), 0.0f, 1.0f);
                t = glm::mix(at(k0), at(k1), a);
            }
            srcPose.Positions[ch.boneIndex] = Mathematics::Vector3(t.x, t.y, t.z);
        }
        return srcPose;
    }

    // Run the CPU pipeline end-to-end: sample clip (blended from `blend`'s
    // clip during a crossfade, as HumanoidRetargetSystem blends with
    // BlendLocalPoses), evaluate RetargetNode, build skin matrices. Returns
    // the atlas as a flat vector of floats (16 per joint, column-major mat4).
    std::vector<float> ComputeCPUReference(
        AAnim::SkeletonData& srcSkel,
        AAnim::SkeletonData& tgtSkel,
        const AAnim::HumanoidRig& srcRig,
        const AAnim::HumanoidRig& tgtRig,
        const AAnim::RetargetMap& map,
        const AAnim::AnimationClip& clip,
        float clipTime,
        const BlendFrom* blend = nullptr)
    {
        AAnim::AnimationPose srcPose = SampleReferencePose(srcSkel, clip, clipTime);
        if (blend)
        {
            const AAnim::AnimationPose prevPose = SampleReferencePose(srcSkel, *blend->Clip, blend->Time);
            const float w = std::clamp(blend->Alpha, 0.0f, 1.0f);
            for (uint32_t b = 0; b < srcSkel.BoneCount; ++b)
            {
                srcPose.Rotations[b] = Mathematics::Quaternion(
                    glm::slerp(prevPose.Rotations[b].GetGLM(), srcPose.Rotations[b].GetGLM(), w));
                const auto& p = prevPose.Positions[b];
                const auto& c = srcPose.Positions[b];
                const glm::vec3 t = glm::mix(glm::vec3(p.x, p.y, p.z), glm::vec3(c.x, c.y, c.z), w);
                srcPose.Positions[b] = Mathematics::Vector3(t.x, t.y, t.z);
            }
        }

        // RetargetNode evaluate.
        struct StaticPose : public AAnim::AnimGraphNode {
            AAnim::AnimationPose Pose;
            void Evaluate(AAnim::EvaluationContext&, AAnim::AnimationPose& out) override { out = Pose; }
        };
        StaticPose source;
        source.Pose = srcPose;

        AAnim::RetargetNode node;
        node.Configure(&source, &srcRig, &tgtRig, &map);
        EXPECT_TRUE(node.Build(srcSkel, tgtSkel));

        AAnim::PoseStack stack;
        stack.Reserve(8);
        AAnim::EvaluationContext ctx{};
        ctx.SourceSkeleton = &srcSkel;
        ctx.TargetSkeleton = &tgtSkel;
        ctx.ScratchStack = &stack;
        AAnim::AnimationPose tgtLocalPose;
        node.Evaluate(ctx, tgtLocalPose);

        // Build skin matrices.
        std::vector<float> outAtlas;
        AAnim::PoseSampleWorkspace ws;
        ws.Pose.assign(tgtSkel.BoneCount, AAnim::PoseSampleWorkspace::TRS{});
        for (uint32_t b = 0; b < tgtSkel.BoneCount && b < tgtLocalPose.BoneCount; ++b)
        {
            ws.Pose[b].t = glm::vec3(tgtLocalPose.Positions[b].x,
                                      tgtLocalPose.Positions[b].y,
                                      tgtLocalPose.Positions[b].z);
            ws.Pose[b].r = tgtLocalPose.Rotations[b].GetGLM();
            ws.Pose[b].s = glm::vec3(1, 1, 1);
        }
        ws.ChangedTranslation.assign(tgtSkel.BoneCount, 1u);
        ws.ChangedRotation.assign(tgtSkel.BoneCount, 1u);
        ws.ChangedScale.assign(tgtSkel.BoneCount, 1u);
        AAnim::BuildPoseToSkinMatrices(tgtSkel, nullptr, &outAtlas, ws);
        return outAtlas;
    }

    // -- GPU dispatch ---------------------------------------------------

    std::vector<float> DispatchGPU(
        AAnim::SkeletonData& srcSkel,
        AAnim::SkeletonData& tgtSkel,
        const AAnim::HumanoidRig& srcRig,
        const AAnim::HumanoidRig& tgtRig,
        const AAnim::RetargetMap& map,
        const AAnim::AnimationClip& clip,
        float clipTime,
        uint32_t skinJointCount,
        const BlendFrom* blend = nullptr)
    {
        m_DataStore.Initialize(m_Device.get());
        m_DataStore.BeginFrame(0);

        // Build a CPU RetargetNode to source the GPU export from.
        struct StaticPose : public AAnim::AnimGraphNode {
            AAnim::AnimationPose Pose;
            void Evaluate(AAnim::EvaluationContext&, AAnim::AnimationPose& out) override { out = Pose; }
        };
        StaticPose source;
        source.Pose.Resize(srcSkel.BoneCount);
        for (uint32_t b = 0; b < srcSkel.BoneCount; ++b)
            source.Pose.Rotations[b] = Mathematics::Quaternion::Identity();

        AAnim::RetargetNode node;
        node.Configure(&source, &srcRig, &tgtRig, &map);
        EXPECT_TRUE(node.Build(srcSkel, tgtSkel));

        // Asset-load uploads.
        const GUID rigGuid = map.GetGUID();
        const GUID clipGuid = clip.GetGUID();
        uint32_t rigPairIdx = m_DataStore.EnsureRigPairUploaded(rigGuid, node, srcSkel, tgtSkel, skinJointCount);
        uint32_t clipIdx    = m_DataStore.EnsureClipUploaded(clipGuid, clip);
        EXPECT_NE(rigPairIdx, kRetargetInvalidIndex);
        EXPECT_NE(clipIdx,    kRetargetInvalidIndex);

        // One character at palette offset 0.
        std::vector<GPURetargetCharacterParams> chars(1);
        chars[0].rigPairHeaderIdx = rigPairIdx;
        chars[0].clipHeaderIdx = clipIdx;
        chars[0].clipTime = clipTime;
        chars[0].paletteOffset = 0;
        if (blend)
        {
            chars[0].prevClipHeaderIdx = m_DataStore.EnsureClipUploaded(blend->Clip->GetGUID(), *blend->Clip);
            EXPECT_NE(chars[0].prevClipHeaderIdx, kRetargetInvalidIndex);
            chars[0].prevClipTime = blend->Time;
            chars[0].blendAlpha = blend->Alpha;
        }
        m_DataStore.AppendCharacterParams(chars);
        m_DataStore.FlushIfDirty();

        // Atlas readback buffer. Storage layout is mat3x4 (3 vec4 rows per
        // bone = 48 bytes), see Engine/Include/Engine/Rendering/
        // BonePaletteLayout.h.
        const size_t atlasBytes = static_cast<size_t>(skinJointCount)
                                * GameEngine::Engine::Renderer::BonePaletteLayout::kFloatsPerBone
                                * sizeof(float);
        BufferDesc atlasDesc{};
        atlasDesc.size = atlasBytes;
        atlasDesc.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferSrc | BufferUsage::TransferDst);
        atlasDesc.memoryUsage = BufferMemoryUsage::Readback;
        atlasDesc.flags = BufferCreateFlags::PersistentlyMapped;
        atlasDesc.debugName = "RetargetParityAtlas";
        BufferHandle atlasBuf = m_Device->CreateBuffer(atlasDesc);
        EXPECT_TRUE(atlasBuf.IsValid());
        std::vector<uint8_t> zeros(atlasBytes, 0);
        m_Device->UpdateBuffer(atlasBuf, 0, atlasBytes, zeros.data());

        // Pipeline.
        DescriptorSetLayoutDesc set0{};
        set0.debugName = "RetargetParity_Set0";
        for (uint32_t i = 0; i < 4; ++i)
        {
            DescriptorBinding b{};
            b.binding = i;
            b.type = DescriptorType::StorageBuffer;
            b.count = 1;
            b.shaderStages = kShaderStageCompute;
            set0.bindings.push_back(b);
        }
        PipelineDesc pd{};
        pd.type = PipelineType::Compute;
        pd.debugName = "RetargetParityPipeline";
        pd.computeShader = m_ShaderSpirv;
        pd.descriptorSetLayouts.push_back(set0);
        pd.pushConstantSize = sizeof(uint32_t);
        pd.pushConstantStagesMask = kShaderStageCompute;

        PipelineHandle pipe = m_Device->CreatePipeline(pd);
        EXPECT_NE(pipe, PipelineHandle{});

        DescriptorSetDesc dsDesc{};
        dsDesc.layout = set0;
        dsDesc.debugName = "RetargetParity_DS0";
        DescriptorSetHandle ds = m_Device->CreateDescriptorSet(dsDesc);

        auto bindSSBO = [&](uint32_t binding, BufferHandle buf, size_t offset, size_t size) {
            DescriptorSetUpdate upd{};
            upd.binding = binding;
            upd.type = DescriptorType::StorageBuffer;
            upd.buffers = {buf};
            upd.bufferOffsets = {offset};
            upd.bufferRanges = {size};
            m_Device->UpdateDescriptorSet(ds, upd);
        };
        bindSSBO(0, m_DataStore.GetClipBuffer(),  0, m_DataStore.GetClipUsedBytes());
        bindSSBO(1, m_DataStore.GetRigBuffer(),   0, m_DataStore.GetRigUsedBytes());
        bindSSBO(2, m_DataStore.GetCharsBuffer(), 0, m_DataStore.GetCharsUsedBytes());
        bindSSBO(3, atlasBuf, 0, atlasBytes);

        auto cl = m_Device->CreateCommandList(IDevice::QueueType::Compute);
        cl->Begin();
        cl->SetPipeline(pipe);
        cl->BindDescriptorSet(0, ds, pipe);
        uint32_t characterCount = 1;
        cl->SetPushConstants(characterCount);
        cl->Dispatch(1, 1, 1);
        cl->End();

        std::vector<CommandList*> lists{cl.get()};
        m_Device->ExecuteCommandLists(lists);
        m_Device->WaitForIdle();

        // Read back the packed mat3x4 form (12 floats/bone) and unpack to
        // the canonical mat4 layout (16 floats/bone) so the parity
        // comparison stays expressed in mat4 terms — keeps the existing
        // CPU reference path unchanged.
        using namespace GameEngine::Engine::Renderer::BonePaletteLayout;
        std::vector<float> packed(skinJointCount * kFloatsPerBone);
        void* mapped = m_Device->MapBuffer(atlasBuf);
        if (mapped) std::memcpy(packed.data(), mapped, atlasBytes);
        m_Device->UnmapBuffer(atlasBuf);

        std::vector<float> result(skinJointCount * 16);
        for (uint32_t j = 0; j < skinJointCount; ++j)
        {
            UnpackRowsToMat4(packed.data() + j * kFloatsPerBone,
                             result.data() + j * 16);
        }

        m_Device->DestroyBuffer(atlasBuf);
        m_Device->DestroyPipeline(pipe);
        return result;
    }

    // One crossfade dispatch against the CPU reference `expected`. The data store keeps the
    // first upload under a GUID, so each test dispatches once.
    void ExpectCrossfade(const AAnim::AnimationClip& current, float currentTime, const BlendFrom& blend,
                         const std::vector<float>* expected = nullptr)
    {
        auto srcSkel = MakeChainSkeleton(3);
        auto tgtSkel = MakeChainSkeleton(3);
        const std::vector<AAnim::HumanBone> canon = {
            AAnim::HumanBone::Hips, AAnim::HumanBone::Spine, AAnim::HumanBone::Head };
        const std::vector<Mathematics::Quaternion> Q(3, Mathematics::Quaternion::Identity());
        auto srcRig = MakeRig(canon, Q);
        auto tgtRig = MakeRig(canon, Q);
        auto map = MakeMap();
        const auto cpu = expected ? *expected
                                  : ComputeCPUReference(srcSkel, tgtSkel, *srcRig, *tgtRig, *map, current,
                                                        currentTime, &blend);
        const auto gpu = DispatchGPU(srcSkel, tgtSkel, *srcRig, *tgtRig, *map, current, currentTime, 3, &blend);
        ExpectAtlasMatches(cpu, gpu);
    }

    // The CPU atlas for one clip alone, on the same chain ExpectCrossfade uses.
    std::vector<float> SingleClipReference(const AAnim::AnimationClip& clip, float clipTime)
    {
        auto srcSkel = MakeChainSkeleton(3);
        auto tgtSkel = MakeChainSkeleton(3);
        const std::vector<AAnim::HumanBone> canon = {
            AAnim::HumanBone::Hips, AAnim::HumanBone::Spine, AAnim::HumanBone::Head };
        const std::vector<Mathematics::Quaternion> Q(3, Mathematics::Quaternion::Identity());
        auto srcRig = MakeRig(canon, Q);
        auto tgtRig = MakeRig(canon, Q);
        auto map = MakeMap();
        return ComputeCPUReference(srcSkel, tgtSkel, *srcRig, *tgtRig, *map, clip, clipTime);
    }

    // -- Comparison ----------------------------------------------------

    void ExpectAtlasMatches(const std::vector<float>& cpu,
                             const std::vector<float>& gpu,
                             float tol = 1e-4f)
    {
        ASSERT_EQ(cpu.size(), gpu.size()) << "atlas float-count mismatch";
        for (size_t i = 0; i < cpu.size(); ++i)
        {
            ASSERT_TRUE(std::isfinite(gpu[i])) << "gpu[" << i << "] not finite";
            ASSERT_TRUE(std::isfinite(cpu[i])) << "cpu[" << i << "] not finite";
            EXPECT_NEAR(gpu[i], cpu[i], tol)
                << "atlas[" << i << "] cpu=" << cpu[i] << " gpu=" << gpu[i];
        }
    }

    std::unique_ptr<IDevice> m_Device;
    std::vector<uint8_t> m_ShaderSpirv;
    RetargetGPUDataStore m_DataStore;

private:
    static std::vector<uint8_t> LoadShader(const char* name)
    {
        namespace fs = std::filesystem;
        std::vector<std::string> searchPaths = {
            "Assets/Shaders/" + std::string(name),
            "../Assets/Shaders/" + std::string(name),
        };
#ifdef RENDERING_SOURCE_DIR
        searchPaths.push_back(std::string(RENDERING_SOURCE_DIR) + "/Shaders/" + name);
#endif
#ifdef GE_BINARY_DIR
        // Compiled .spv lives in the build dir's Shaders/ subdir (preset-agnostic).
        searchPaths.push_back(std::string(GE_BINARY_DIR) + "/Shaders/" + name);
#endif
        for (const auto& p : searchPaths)
        {
            auto bytes = Utils::ReadFile(p);
            if (!bytes.empty()) return bytes;
        }
        return {};
    }
};

// ---------------------------------------------------------------------------
// Scenario 1 — same-rig animated.
// Source skel == target skel (3-bone chain identity-rest), clip animates
// bone 1 from identity to a 30° X-axis rotation. CPU and GPU should produce
// identical skin matrices.
// ---------------------------------------------------------------------------

TEST_F(RetargetGPUParityTest, SameRigAnimated)
{
    if (!HasDevice()) GTEST_SKIP() << "No Vulkan device available";
    if (!HasShader()) GTEST_SKIP() << "retarget_full.comp.spv not found";

    auto srcSkel = MakeChainSkeleton(3);
    auto tgtSkel = MakeChainSkeleton(3);

    const std::vector<AAnim::HumanBone> canon = {
        AAnim::HumanBone::Hips, AAnim::HumanBone::Spine, AAnim::HumanBone::Head };
    const std::vector<Mathematics::Quaternion> Q(3, Mathematics::Quaternion::Identity());
    auto srcRig = MakeRig(canon, Q);
    auto tgtRig = MakeRig(canon, Q);
    auto map = MakeMap();

    // 30° rotation around X for bone 1 at t=duration.
    const glm::quat q0 = glm::quat(1, 0, 0, 0);
    const glm::quat q1 = glm::angleAxis(glm::radians(30.0f), glm::vec3(1, 0, 0));
    auto clip = MakeRotationClip(1, Mathematics::Quaternion(q0), Mathematics::Quaternion(q1), 1.0f);

    const float t = 0.5f;
    auto cpu = ComputeCPUReference(srcSkel, tgtSkel, *srcRig, *tgtRig, *map, *clip, t);
    auto gpu = DispatchGPU(srcSkel, tgtSkel, *srcRig, *tgtRig, *map, *clip, t, /*skinJointCount=*/3);

    ExpectAtlasMatches(cpu, gpu);
}

// ---------------------------------------------------------------------------
// Scenario 2 — cross-rig synthetic 5° X-tilt.
// Source rig bones tilted 5° on X at bind; target rig identity bind.
// Clip plays the source's authored bind. Phase 25.3 semantics:
// target's authored visual matches the source's tilted bind chain (tilt5
// per bone in the chain). Verifies BindCorrection is correctly applied
// on GPU.
// ---------------------------------------------------------------------------

TEST_F(RetargetGPUParityTest, CrossRigSyntheticTilt)
{
    if (!HasDevice()) GTEST_SKIP() << "No Vulkan device available";
    if (!HasShader()) GTEST_SKIP() << "retarget_full.comp.spv not found";

    const Mathematics::Quaternion tilt5(glm::angleAxis(glm::radians(5.0f), glm::vec3(1, 0, 0)));

    auto srcSkel = MakeChainSkeleton(3);
    auto tgtSkel = MakeChainSkeleton(3);
    // Source bones: 5° X-tilt rest.
    for (uint32_t b = 0; b < 3; ++b) SetRestRotation(srcSkel, b, tilt5);

    const std::vector<AAnim::HumanBone> canon = {
        AAnim::HumanBone::Hips, AAnim::HumanBone::Spine, AAnim::HumanBone::Head };
    // Source RetargetPoseRotation = inv(tilt5) so canonical bind = identity.
    const std::vector<Mathematics::Quaternion> srcQ(3, AAnim::Inverse(tilt5));
    const std::vector<Mathematics::Quaternion> tgtQ(3, Mathematics::Quaternion::Identity());
    auto srcRig = MakeRig(canon, srcQ);
    auto tgtRig = MakeRig(canon, tgtQ);
    auto map = MakeMap();

    // Clip plays source's authored bind (tilt5) on every bone.
    auto clip = MakeRotationClip(1, tilt5, tilt5, 1.0f);

    auto cpu = ComputeCPUReference(srcSkel, tgtSkel, *srcRig, *tgtRig, *map, *clip, 0.5f);
    auto gpu = DispatchGPU(srcSkel, tgtSkel, *srcRig, *tgtRig, *map, *clip, 0.5f, 3);

    ExpectAtlasMatches(cpu, gpu);
}

// ---------------------------------------------------------------------------
// Scenario 3 — short-clip fallback.
// Skeleton has 3 bones, clip animates bone 0 only. Bones 1 and 2 should
// fall back to rest rotation. Verifies the GPU shader's "rest fallback for
// non-animated bones" matches the CPU's behavior.
// ---------------------------------------------------------------------------

TEST_F(RetargetGPUParityTest, ShortClipRestFallback)
{
    if (!HasDevice()) GTEST_SKIP() << "No Vulkan device available";
    if (!HasShader()) GTEST_SKIP() << "retarget_full.comp.spv not found";

    auto srcSkel = MakeChainSkeleton(3);
    auto tgtSkel = MakeChainSkeleton(3);

    const std::vector<AAnim::HumanBone> canon = {
        AAnim::HumanBone::Hips, AAnim::HumanBone::Spine, AAnim::HumanBone::Head };
    const std::vector<Mathematics::Quaternion> Q(3, Mathematics::Quaternion::Identity());
    auto srcRig = MakeRig(canon, Q);
    auto tgtRig = MakeRig(canon, Q);
    auto map = MakeMap();

    // Clip animates only bone 0.
    auto clip = MakeRotationClip(0,
        Mathematics::Quaternion::Identity(),
        Mathematics::Quaternion(glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 1, 0))),
        1.0f);

    auto cpu = ComputeCPUReference(srcSkel, tgtSkel, *srcRig, *tgtRig, *map, *clip, 1.0f);
    auto gpu = DispatchGPU(srcSkel, tgtSkel, *srcRig, *tgtRig, *map, *clip, 1.0f, 3);

    ExpectAtlasMatches(cpu, gpu);
}

// ---------------------------------------------------------------------------
// Scenario 4 — animator crossfade.
// Mid-blend, the CPU path slerps the outgoing clip's pose into the current
// one before retargeting; the GPU path must produce the same palette. The two
// clips animate different bones, so each side of the blend falls back to rest
// for the other's bone.
// ---------------------------------------------------------------------------

TEST_F(RetargetGPUParityTest, CrossfadeBlendsThePreviousClip)
{
    if (!HasDevice()) GTEST_SKIP() << "No Vulkan device available";
    if (!HasShader()) GTEST_SKIP() << "retarget_full.comp.spv not found";

    auto srcSkel = MakeChainSkeleton(3);
    auto tgtSkel = MakeChainSkeleton(3);

    const std::vector<AAnim::HumanBone> canon = {
        AAnim::HumanBone::Hips, AAnim::HumanBone::Spine, AAnim::HumanBone::Head };
    const std::vector<Mathematics::Quaternion> Q(3, Mathematics::Quaternion::Identity());
    auto srcRig = MakeRig(canon, Q);
    auto tgtRig = MakeRig(canon, Q);
    auto map = MakeMap();

    auto current = MakeRotationClip(1, Mathematics::Quaternion::Identity(),
        Mathematics::Quaternion(glm::angleAxis(glm::radians(60.0f), glm::vec3(1, 0, 0))), 1.0f);
    auto previous = MakeRotationClip(2, Mathematics::Quaternion::Identity(),
        Mathematics::Quaternion(glm::angleAxis(glm::radians(80.0f), glm::vec3(0, 1, 0))), 1.0f);
    // Both clips are sampled at their midpoint, where this reference's key nlerp equals the
    // upload's key slerp; the blend weight is off-center so the blend's own slerp is tested.
    const BlendFrom blend{previous.get(), 0.5f, 0.3f};

    auto cpu = ComputeCPUReference(srcSkel, tgtSkel, *srcRig, *tgtRig, *map, *current, 0.5f, &blend);
    auto gpu = DispatchGPU(srcSkel, tgtSkel, *srcRig, *tgtRig, *map, *current, 0.5f, 3, &blend);

    ExpectAtlasMatches(cpu, gpu);
}

// ---------------------------------------------------------------------------
// Crossfade rules, one dispatch per test. Each pins one rule of the kernel's
// blend against the CPU reference, which blends as BlendLocalPoses does.
// Sample times sit on exact keys or clip midpoints, where the reference's key
// nlerp equals the upload's key slerp.
// ---------------------------------------------------------------------------

namespace
{
Mathematics::Quaternion AboutX(float degrees)
{
    return Mathematics::Quaternion(glm::angleAxis(glm::radians(degrees), glm::vec3(1, 0, 0)));
}

} // namespace

TEST_F(RetargetGPUParityTest, CrossfadeSamplesThePreviousClipAtItsOwnTime)
{
    if (!HasDevice()) GTEST_SKIP() << "No Vulkan device available";
    if (!HasShader()) GTEST_SKIP() << "retarget_full.comp.spv not found";
    auto current = MakeRotationClip(1, Mathematics::Quaternion::Identity(), AboutX(60.0f), 1.0f);
    auto previous = MakeRotationClip(2, Mathematics::Quaternion::Identity(), AboutX(-70.0f), 1.0f);
    ExpectCrossfade(*current, 0.5f, BlendFrom{previous.get(), 1.0f, 0.3f});
}

TEST_F(RetargetGPUParityTest, CrossfadeTakesTheShortArc)
{
    // 220 and 30 degrees about X on the same bone: the two quaternions' dot is negative.
    if (!HasDevice()) GTEST_SKIP() << "No Vulkan device available";
    if (!HasShader()) GTEST_SKIP() << "retarget_full.comp.spv not found";
    ASSERT_LT(glm::dot(AboutX(30.0f).GetGLM(), AboutX(220.0f).GetGLM()), 0.0f);
    auto current = MakeRotationClip(1, AboutX(30.0f), AboutX(30.0f), 1.0f);
    auto previous = MakeRotationClip(1, AboutX(220.0f), AboutX(220.0f), 1.0f);
    ExpectCrossfade(*current, 0.5f, BlendFrom{previous.get(), 0.5f, 0.4f});
}

TEST_F(RetargetGPUParityTest, CrossfadeAtFullWeightIsTheCurrentClipAlone)
{
    if (!HasDevice()) GTEST_SKIP() << "No Vulkan device available";
    if (!HasShader()) GTEST_SKIP() << "retarget_full.comp.spv not found";
    auto current = MakeRotationClip(1, Mathematics::Quaternion::Identity(), AboutX(60.0f), 1.0f);
    auto previous = MakeRotationClip(2, Mathematics::Quaternion::Identity(), AboutX(-70.0f), 1.0f);
    const auto currentAlone = SingleClipReference(*current, 0.5f);
    ExpectCrossfade(*current, 0.5f, BlendFrom{previous.get(), 1.0f, 1.0f}, &currentAlone);
}

TEST_F(RetargetGPUParityTest, CrossfadeAtZeroWeightIsThePreviousClipAlone)
{
    if (!HasDevice()) GTEST_SKIP() << "No Vulkan device available";
    if (!HasShader()) GTEST_SKIP() << "retarget_full.comp.spv not found";
    auto current = MakeRotationClip(1, Mathematics::Quaternion::Identity(), AboutX(60.0f), 1.0f);
    auto previous = MakeRotationClip(2, Mathematics::Quaternion::Identity(), AboutX(-70.0f), 1.0f);
    const auto previousAlone = SingleClipReference(*previous, 1.0f);
    ExpectCrossfade(*current, 0.5f, BlendFrom{previous.get(), 1.0f, 0.0f}, &previousAlone);
}

TEST_F(RetargetGPUParityTest, CrossfadeBlendsTheHipTranslation)
{
    // Both clips carry a hip translation, blended by the crossfade weight.
    if (!HasDevice()) GTEST_SKIP() << "No Vulkan device available";
    if (!HasShader()) GTEST_SKIP() << "retarget_full.comp.spv not found";
    auto current = MakeRotationAndHipClip(1, Mathematics::Quaternion::Identity(), AboutX(60.0f),
                                          glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 2.0f), 1.0f);
    auto previous = MakeRotationAndHipClip(2, Mathematics::Quaternion::Identity(), AboutX(-70.0f),
                                           glm::vec3(1.0f, 0.5f, 0.0f), glm::vec3(1.0f, 0.5f, 0.0f), 1.0f);
    ExpectCrossfade(*current, 0.5f, BlendFrom{previous.get(), 1.0f, 0.3f});
}

TEST_F(RetargetGPUParityTest, CrossfadeBelowZeroWeightClampsLikeTheCpu)
{
    // AnimatorRef::BlendAlpha has no bounds (a BlendTime written directly can be negative);
    // BlendLocalPoses clamps the weight for rotations and translations alike.
    if (!HasDevice()) GTEST_SKIP() << "No Vulkan device available";
    if (!HasShader()) GTEST_SKIP() << "retarget_full.comp.spv not found";
    auto current = MakeRotationAndHipClip(1, Mathematics::Quaternion::Identity(), AboutX(60.0f),
                                          glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 2.0f), 1.0f);
    auto previous = MakeRotationAndHipClip(2, Mathematics::Quaternion::Identity(), AboutX(-70.0f),
                                           glm::vec3(1.0f, 0.5f, 0.0f), glm::vec3(1.0f, 0.5f, 0.0f), 1.0f);
    ExpectCrossfade(*current, 0.5f, BlendFrom{previous.get(), 1.0f, -0.5f});
}

TEST_F(RetargetGPUParityTest, CrossfadeAboveFullWeightClampsLikeTheCpu)
{
    // A BlendTime written past the duration gives a weight above 1; it clamps as on the CPU.
    if (!HasDevice()) GTEST_SKIP() << "No Vulkan device available";
    if (!HasShader()) GTEST_SKIP() << "retarget_full.comp.spv not found";
    auto current = MakeRotationAndHipClip(1, Mathematics::Quaternion::Identity(), AboutX(60.0f),
                                          glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 2.0f), 1.0f);
    auto previous = MakeRotationAndHipClip(2, Mathematics::Quaternion::Identity(), AboutX(-70.0f),
                                           glm::vec3(1.0f, 0.5f, 0.0f), glm::vec3(1.0f, 0.5f, 0.0f), 1.0f);
    ExpectCrossfade(*current, 0.5f, BlendFrom{previous.get(), 1.0f, 1.5f});
}
