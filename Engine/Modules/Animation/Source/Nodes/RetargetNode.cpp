#include "Animation/Nodes/RetargetNode.h"

#include "Animation/AnimationPose.h"
#include "Animation/EvaluationContext.h"
#include "Animation/PoseStack.h"
#include "Animation/QuaternionMath.h"
#include "Animation/SkeletonData.h"
#include "Animation/SkeletonProfile.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace GameEngine
{
namespace Animation
{

namespace
{

// Threshold for Mode A vs Mode D escalation (per plan §3.2). When per-bone
// delta exceeds 30 degrees on a chain that is auto-set to OneToOne, callers
// can opt into SlerpAlongArc; the gate lives in the pairing settings.
constexpr float kSpineWorldArcEscalationDeg = 30.0f;

Mathematics::Quaternion ReadRestRotation(const SkeletonData& skel, uint32_t boneIdx)
{
    if (boneIdx >= skel.BoneCount) return Mathematics::Quaternion::Identity();
    if (skel.RestRotation.size() < static_cast<size_t>(boneIdx + 1) * 4u)
        return Mathematics::Quaternion::Identity();
    const float* rp = &skel.RestRotation[boneIdx * 4u];
    return Mathematics::Quaternion(glm::quat(rp[3], rp[0], rp[1], rp[2]));
}

Mathematics::Vector3 ReadRestTranslation(const SkeletonData& skel, uint32_t boneIdx)
{
    if (boneIdx >= skel.BoneCount) return Mathematics::Vector3(0.0f, 0.0f, 0.0f);
    if (skel.RestTranslation.size() < static_cast<size_t>(boneIdx + 1) * 3u)
        return Mathematics::Vector3(0.0f, 0.0f, 0.0f);
    const float* tp = &skel.RestTranslation[boneIdx * 3u];
    return Mathematics::Vector3(tp[0], tp[1], tp[2]);
}

Mathematics::Vector3 ReadRestScale(const SkeletonData& skel, uint32_t boneIdx)
{
    if (boneIdx >= skel.BoneCount) return Mathematics::Vector3(1.0f, 1.0f, 1.0f);
    if (skel.RestScale.size() < static_cast<size_t>(boneIdx + 1) * 3u)
        return Mathematics::Vector3(1.0f, 1.0f, 1.0f);
    const float* sp = &skel.RestScale[boneIdx * 3u];
    return Mathematics::Vector3(sp[0], sp[1], sp[2]);
}

// Walk a chain in HumanoidRig.Chains by ChainKind and return the
// canonical-bone list (root-to-tip). Returns empty if the chain isn't
// present.
const std::vector<HumanBone>& FindChainBones(const HumanoidRig& rig, ChainKind kind)
{
    static const std::vector<HumanBone> kEmpty;
    for (const auto& c : rig.Chains())
        if (c.Kind == kind) return c.IncludeBones;
    return kEmpty;
}

} // namespace

void RetargetNode::Configure(AnimGraphNode* sourceProvider,
                             const HumanoidRig* sourceRig,
                             const HumanoidRig* targetRig,
                             const RetargetMap* map)
{
    m_SourceProvider = sourceProvider;
    m_SourceRig = sourceRig;
    m_TargetRig = targetRig;
    m_Map = map;
    m_Built = false;
}

bool RetargetNode::Build(const SkeletonData& sourceSkeleton,
                         const SkeletonData& targetSkeleton)
{
    m_Built = false;
    m_SourceBoneCount = sourceSkeleton.BoneCount;
    m_TargetBoneCount = targetSkeleton.BoneCount;

    if (!m_SourceRig || !m_TargetRig || !m_Map) return false;
    if (m_SourceBoneCount == 0 || m_TargetBoneCount == 0) return false;

    // Per-source-bone caches sized to source skeleton.
    m_SourceRestRot.assign(m_SourceBoneCount, Mathematics::Quaternion::Identity());
    m_SourceBoneCanonical.assign(m_SourceBoneCount, HumanBone::None);
    m_SourceParent.assign(m_SourceBoneCount, -1);

    for (uint32_t b = 0; b < m_SourceBoneCount; ++b)
    {
        m_SourceRestRot[b] = ReadRestRotation(sourceSkeleton, b);
        m_SourceParent[b]  = (b < sourceSkeleton.Parent.size()) ? sourceSkeleton.Parent[b] : -1;
    }

    // Cache topological order (parent-before-child). FBX-imported skeletons
    // populate this via SkeletonData::ComputeTopologicalSort. When absent
    // (synthetic test fixtures), fall back to 0..N-1, which is correct for
    // any skeleton where bone indices already obey the standard FBX
    // convention (parent index < child index).
    auto cacheTopo = [](const SkeletonData& skel, std::vector<uint32>& outTopo)
    {
        if (!skel.TopologicalOrder.empty()
            && skel.TopologicalOrder.size() == skel.BoneCount)
        {
            outTopo = skel.TopologicalOrder;
            return;
        }
        outTopo.resize(skel.BoneCount);
        for (uint32 i = 0; i < skel.BoneCount; ++i) outTopo[i] = i;
    };
    cacheTopo(sourceSkeleton, m_SourceTopo);
    cacheTopo(targetSkeleton, m_TargetTopo);

    // Per-target-bone caches.
    m_TargetRetargetPoseLocal.assign(m_TargetBoneCount, Mathematics::Quaternion::Identity());
    m_TargetRestPos.assign(m_TargetBoneCount, Mathematics::Vector3(0.0f, 0.0f, 0.0f));
    m_TargetRestScale.assign(m_TargetBoneCount, Mathematics::Vector3(1.0f, 1.0f, 1.0f));
    m_TargetParent.assign(m_TargetBoneCount, -1);
    m_TargetBoneCanonical.assign(m_TargetBoneCount, HumanBone::None);
    for (uint32_t b = 0; b < m_TargetBoneCount; ++b)
    {
        const Mathematics::Quaternion bind_tgt = ReadRestRotation(targetSkeleton, b);
        m_TargetRetargetPoseLocal[b] = bind_tgt; // identity Q for unmapped bones
        m_TargetRestPos[b]   = ReadRestTranslation(targetSkeleton, b);
        m_TargetRestScale[b] = ReadRestScale(targetSkeleton, b);
        m_TargetParent[b]    = (b < targetSkeleton.Parent.size()) ? targetSkeleton.Parent[b] : -1;
    }

    // Per-frame scratch sized once at build time.
    m_SourceWorldClipScratch.assign(m_SourceBoneCount, Mathematics::Quaternion::Identity());
    m_TargetWorldClipScratch.assign(m_TargetBoneCount, Mathematics::Quaternion::Identity());

    // m_SourceWorldBind / m_TargetWorldBind are filled below AFTER the
    // canonical routing tables and per-bone retarget-pose values have been
    // populated. They store the WORLD rotation of the *retarget pose*
    // (Q * bind, where Q is the bone's RetargetPoseRotation that brings
    // source/target bind into the canonical T-pose). For canonically
    // mapped bones, src_retarget_world == tgt_retarget_world == canonical
    // T-pose world rotation, so delta_world from the clip transports
    // straight onto the target with no axis-frame fudge needed.
    m_SourceWorldBind.assign(m_SourceBoneCount, Mathematics::Quaternion::Identity());
    m_TargetWorldBind.assign(m_TargetBoneCount, Mathematics::Quaternion::Identity());

    // Phase 25.1 — Per-bone canonical Q caches. Default identity; overridden
    // below for canonical bones whose BoneMap entry carries a non-identity
    // RetargetPoseRotation.
    m_SourceCanonicalQ.assign(m_SourceBoneCount, Mathematics::Quaternion::Identity());
    m_TargetCanonicalQ.assign(m_TargetBoneCount, Mathematics::Quaternion::Identity());

    // Reset canonical routing.
    for (auto& cr : m_Canonical) cr = CanonicalRouting{};

    // Source rig: cache canonical -> source bone index + retarget rotation.
    // Per audit P0 #6: re-resolve CachedSourceIndex from SourceBoneName at
    // build time so model hot-reload (different bone count / order) doesn't
    // silently route a stale index to the wrong bone.
    for (const auto& m : m_SourceRig->BoneMap())
    {
        if (m.Canonical == HumanBone::None) continue;
        const auto canonIdx = static_cast<size_t>(m.Canonical);
        if (canonIdx >= m_Canonical.size()) continue;

        // Live re-resolve: trust the name, fall back to the cached index when
        // names aren't populated (synthetic test skeletons).
        uint32_t resolved = m.CachedSourceIndex;
        if (!m.SourceBoneName.empty() && !sourceSkeleton.BoneNameLookup.empty())
        {
            const StringId nameId = HashStringId(m.SourceBoneName);
            const uint32_t live = sourceSkeleton.ResolveBoneIndex(nameId, ~0u);
            if (live != ~0u) resolved = live;
        }
        if (resolved == ~0u || resolved >= m_SourceBoneCount) continue;

        auto& cr = m_Canonical[canonIdx];
        cr.HasSource = true;
        cr.SourceBoneIndex = resolved;
        // srcRetargetPose[b] = Q * bind_src[b]
        const Mathematics::Quaternion bind = m_SourceRestRot[resolved];
        cr.SourceRetargetPose = (m.RetargetPoseRotation * bind).Normalized();
        m_SourceBoneCanonical[resolved] = m.Canonical;
        m_SourceCanonicalQ[resolved] = m.RetargetPoseRotation;
    }

    // Target rig: cache canonical -> target bone index + retarget rotation.
    // Same hot-reload re-resolve as above.
    for (const auto& m : m_TargetRig->BoneMap())
    {
        if (m.Canonical == HumanBone::None) continue;
        const auto canonIdx = static_cast<size_t>(m.Canonical);
        if (canonIdx >= m_Canonical.size()) continue;

        uint32_t resolved = m.CachedSourceIndex;
        if (!m.SourceBoneName.empty() && !targetSkeleton.BoneNameLookup.empty())
        {
            const StringId nameId = HashStringId(m.SourceBoneName);
            const uint32_t live = targetSkeleton.ResolveBoneIndex(nameId, ~0u);
            if (live != ~0u) resolved = live;
        }
        if (resolved == ~0u || resolved >= m_TargetBoneCount) continue;

        auto& cr = m_Canonical[canonIdx];
        cr.HasTarget = true;
        cr.TargetBoneIndex = resolved;
        const Mathematics::Quaternion bind = ReadRestRotation(targetSkeleton, resolved);
        cr.TargetRetargetPose = (m.RetargetPoseRotation * bind).Normalized();
        // Override the per-target-bone default with the explicit retarget pose.
        m_TargetRetargetPoseLocal[resolved] = cr.TargetRetargetPose;
        m_TargetBoneCanonical[resolved] = m.Canonical;
        m_TargetCanonicalQ[resolved] = m.RetargetPoseRotation;
    }

    // Build chain dispatch tables. Walk RetargetMap.ChainMap; for each
    // pairing, locate src + tgt rig chains by ChainKind and resolve the
    // canonical bones to source/target skeleton indices.
    m_Chains.clear();
    m_Chains.reserve(m_Map->ChainMap().size());

    for (const auto& pairing : m_Map->ChainMap())
    {
        const auto& srcCanonical = FindChainBones(*m_SourceRig, pairing.Kind);
        const auto& tgtCanonical = FindChainBones(*m_TargetRig, pairing.Kind);
        if (srcCanonical.empty() || tgtCanonical.empty()) continue;

        ChainDispatch chain;
        chain.Kind = pairing.Kind;
        chain.RotationMode = pairing.FK.RotationMode;
        chain.RotationAlpha = pairing.FK.RotationAlpha;
        chain.TranslationMode = pairing.FK.TranslationMode;

        chain.SourceBones.reserve(srcCanonical.size());
        chain.SourceCanonical.reserve(srcCanonical.size());
        for (HumanBone bone : srcCanonical)
        {
            const auto& cr = m_Canonical[static_cast<size_t>(bone)];
            if (!cr.HasSource) continue;
            chain.SourceBones.push_back(cr.SourceBoneIndex);
            chain.SourceCanonical.push_back(bone);
        }

        chain.TargetBones.reserve(tgtCanonical.size());
        chain.TargetCanonical.reserve(tgtCanonical.size());
        for (HumanBone bone : tgtCanonical)
        {
            const auto& cr = m_Canonical[static_cast<size_t>(bone)];
            if (!cr.HasTarget) continue;
            chain.TargetBones.push_back(cr.TargetBoneIndex);
            chain.TargetCanonical.push_back(bone);
        }

        if (chain.SourceBones.empty() || chain.TargetBones.empty()) continue;
        m_Chains.push_back(std::move(chain));
    }

    // Compute world rotations of the *retarget pose* for both rigs. For
    // each bone, the local rotation we compose into the world walk is:
    //   - srcRetargetPose[b] (= Q[b] * bind_src[b]) for canonical bones
    //     mapped on the source side, where Q is the BoneMap's
    //     RetargetPoseRotation that brings source A-pose / authored bind
    //     into the canonical T-pose. Defaults to bind_src[b] otherwise.
    //   - Same shape on the target side.
    //
    // For both rigs at canonical T-pose, m_SourceWorldBind[mapped]
    // == m_TargetWorldBind[mapped]: the world rotation a bone has in the
    // shared canonical T-pose. The retarget delta (src_world_clip *
    // inv(src_retarget_world)) measures the source's deviation from
    // canonical, and applying it to the target's canonical world produces
    // the corresponding target deviation. This is the cross-rig piece that
    // the legacy local-frame formula could not bridge.
    auto composeWorldBind = [](const SkeletonData& skel,
                               const std::vector<uint32>& topo,
                               std::vector<Mathematics::Quaternion>& localOverride,
                               std::vector<Mathematics::Quaternion>& outWorld)
    {
        for (size_t i = 0; i < topo.size(); ++i)
        {
            const uint32 b = topo[i];
            const Mathematics::Quaternion local = localOverride[b];
            const int32 parent = (b < skel.Parent.size()) ? skel.Parent[b] : -1;
            outWorld[b] = (parent < 0)
                ? local.Normalized()
                : (outWorld[static_cast<size_t>(parent)] * local).Normalized();
        }
    };

    std::vector<Mathematics::Quaternion> srcLocalRetarget(m_SourceBoneCount);
    for (uint32 b = 0; b < m_SourceBoneCount; ++b)
        srcLocalRetarget[b] = m_SourceRestRot[b];
    for (size_t ci = 0; ci < m_Canonical.size(); ++ci)
    {
        const auto& cr = m_Canonical[ci];
        if (cr.HasSource && cr.SourceBoneIndex < m_SourceBoneCount)
            srcLocalRetarget[cr.SourceBoneIndex] = cr.SourceRetargetPose;
    }
    composeWorldBind(sourceSkeleton, m_SourceTopo, srcLocalRetarget, m_SourceWorldBind);

    std::vector<Mathematics::Quaternion> tgtLocalRetarget(m_TargetBoneCount);
    for (uint32 b = 0; b < m_TargetBoneCount; ++b)
        tgtLocalRetarget[b] = m_TargetRetargetPoseLocal[b];
    composeWorldBind(targetSkeleton, m_TargetTopo, tgtLocalRetarget, m_TargetWorldBind);

    // Precompute per-canonical-bone composite transport
    // BindCorrection = inv(src_canonical_world) * tgt_canonical_world. Both
    // operands are immutable post-Build, so doing the inverse + multiply
    // here drops 1 inv + 1 mul + 1 normalize from the per-frame Stage 2
    // hot path (was: tgtW = (srcW * inv(srcWB)) * tgtWB; now: tgtW = srcW *
    // BindCorrection). Identity when either side of the canonical pair is
    // unmapped — that case never enters the retarget branch in TransportStage2.
    for (auto& cr : m_Canonical)
    {
        if (cr.HasSource && cr.HasTarget &&
            cr.SourceBoneIndex < m_SourceBoneCount &&
            cr.TargetBoneIndex < m_TargetBoneCount)
        {
            const Mathematics::Quaternion& srcWB = m_SourceWorldBind[cr.SourceBoneIndex];
            const Mathematics::Quaternion& tgtWB = m_TargetWorldBind[cr.TargetBoneIndex];
            cr.BindCorrection = (Inverse(srcWB) * tgtWB).Normalized();
        }
        else
        {
            cr.BindCorrection = Mathematics::Quaternion::Identity();
        }
    }

    // Body proportions for Stage 3.
    m_SourceHipHeight = m_SourceRig->Proportions().HipHeight;
    m_TargetHipHeight = m_TargetRig->Proportions().HipHeight;

    m_Built = true;
    return true;
}

void RetargetNode::ExportForGPU(GPUExport& out) const
{
    out = GPUExport{};
    if (!m_Built) return;

    out.SourceBoneCount = m_SourceBoneCount;
    out.TargetBoneCount = m_TargetBoneCount;

    out.SourceParent  = m_SourceParent;
    out.SourceTopo    = m_SourceTopo;
    out.SourceRestRot = m_SourceRestRot;

    out.TargetParent = m_TargetParent;
    out.TargetTopo   = m_TargetTopo;
    out.TargetRestPos = m_TargetRestPos;

    // TargetBindLocal: the authored bind local rotation per tgt bone. For
    // non-canonical bones m_TargetRetargetPoseLocal already holds bind_local;
    // for canonical bones it's been overridden to (Q * bind), which the GPU
    // shader does not consume on the canonical branch. Either value is
    // correct since the canonical entries are dead reads — but we re-read
    // skeleton rest rotations for clarity, matching the field's contract.
    //
    // (Done at the call site instead — the caller passes in the skeleton.)
    out.TargetBindLocal = m_TargetRetargetPoseLocal;

    // Per-target-bone canonical routing.
    out.TargetSourceIdx.assign(m_TargetBoneCount, static_cast<uint32_t>(~0u));
    out.TargetBindCorrection.assign(m_TargetBoneCount, Mathematics::Quaternion::Identity());
    for (const auto& cr : m_Canonical)
    {
        if (cr.HasSource && cr.HasTarget &&
            cr.TargetBoneIndex < m_TargetBoneCount &&
            cr.SourceBoneIndex < m_SourceBoneCount)
        {
            out.TargetSourceIdx[cr.TargetBoneIndex]      = cr.SourceBoneIndex;
            out.TargetBindCorrection[cr.TargetBoneIndex] = cr.BindCorrection;
        }
    }

    // Stage 3 translation.
    out.HipScale = (m_SourceHipHeight > 1e-6f) ? (m_TargetHipHeight / m_SourceHipHeight) : 1.0f;
    const auto* hipsCr = FindCanonical(HumanBone::Hips);
    if (hipsCr)
    {
        if (hipsCr->HasSource) out.SourceHipBoneIndex = hipsCr->SourceBoneIndex;
        if (hipsCr->HasTarget) out.TargetHipBoneIndex = hipsCr->TargetBoneIndex;
    }
    if (m_TargetRig)
    {
        // TranslationBones are the canonical bones whose authored translation
        // should be inherited verbatim; the rig stores them as a list of
        // canonical enum values. Resolve each to its target-skeleton index.
        for (const HumanBone tb : m_TargetRig->TranslationBones())
        {
            const auto* cr = FindCanonical(tb);
            if (cr && cr->HasTarget) out.TranslationTargetBones.push_back(cr->TargetBoneIndex);
        }
    }
}

const RetargetNode::CanonicalRouting* RetargetNode::FindCanonical(HumanBone bone) const
{
    const auto idx = static_cast<size_t>(bone);
    if (idx >= m_Canonical.size()) return nullptr;
    const auto& cr = m_Canonical[idx];
    if (!cr.HasSource && !cr.HasTarget) return nullptr;
    return &cr;
}

void RetargetNode::FillTargetRestPose(AnimationPose& outPose) const
{
    outPose.Resize(m_TargetBoneCount);
    for (uint32_t b = 0; b < m_TargetBoneCount; ++b)
    {
        outPose.Rotations[b] = m_TargetRetargetPoseLocal[b];
        outPose.Positions[b] = m_TargetRestPos[b];
        outPose.Scales[b]    = m_TargetRestScale[b];
    }
}

void RetargetNode::TransportStage2(const AnimationPose& sourcePose,
                                   AnimationPose& outPose)
{
    // Stage 2 — World-space FK retarget via mesh-displacement equivalence.
    //
    // For both rigs at canonical T-pose at bind, transport the source's
    // mesh displacement (rotation from its own bind to its clip pose)
    // onto target's bind world. This produces the SAME visual deformation
    // on target as on source.
    //
    //   delta_world[b] = source_world_clip[b] * inverse(source_world_bind[b])
    //   target_world[b] = delta_world[b] * target_world_bind[b]
    //   target_local[b] = inverse(parent_world[b]) * target_world[b]
    //
    // Mesh-displacement equivalence:
    //   src mesh delta = src_world_clip * inv(src_bind_world)
    //   tgt mesh delta = tgt_world_clip * inv(tgt_bind_world)
    //                  = (delta * tgt_bind_world) * inv(tgt_bind_world)
    //                  = delta = src mesh delta ✓
    //
    // Same-rig: src_bind_world == tgt_bind_world, so target_world ==
    // source_world_clip — direct world-rotation copy. Cross-rig with
    // baked-in parent-chain transforms (Synty BusinessMale at 115° hip
    // bake vs A_Walk_F_Masc at 13°): the formula still applies because
    // each rig's own bind world cancels out of its own mesh-displacement.

    // 1. Compose source clip world rotations in the source's AUTHORED frame
    //    (no Q applied to clip locals). The clip stores absolute bone-local
    //    rotations relative to the rig's authored bind, so an FK walk over
    //    those locals yields source's authored clip world.
    //
    //    Why no Q here: applying Q would re-canonicalize the clip, which at
    //    clip ≈ source-authored-bind collapses src_clip_world to
    //    src_canonical_world. delta_world = src_clip_world *
    //    inv(src_canonical_world) then goes to identity, swallowing the
    //    A-pose-vs-T-pose bind offset between source and target. By keeping
    //    the clip in authored frame and using src_canonical_world as the
    //    reference, delta = inv(qWorld) at bind, so target inherits source's
    //    authored bind direction (e.g., arms drop from canonical T-pose to
    //    A-pose for an A-pose-authored Synty walk played on a T-pose-bind
    //    target). At later clip frames delta = (src_authored_clip *
    //    inv(src_canonical)) carries both the bind offset AND the clip's
    //    relative motion onto target's canonical T-pose.
    const uint32 srcCopy = std::min<uint32>(m_SourceBoneCount, sourcePose.BoneCount);
    for (uint32 i = 0; i < m_SourceBoneCount; ++i)
    {
        const uint32 b = m_SourceTopo[i];
        const Mathematics::Quaternion local = (b < srcCopy)
            ? sourcePose.Rotations[b]
            : m_SourceRestRot[b];
        const int32 parent = m_SourceParent[b];
        m_SourceWorldClipScratch[b] = (parent < 0)
            ? local.Normalized()
            : (m_SourceWorldClipScratch[static_cast<size_t>(parent)] * local).Normalized();
    }

    // 2. Walk target topologically. Canonical mapped bones apply the
    //    world-space mesh-displacement transport; everything else keeps
    //    bind local. Parent world rotations are accumulated for the
    //    inv(parent_world) conversion of the next chain bone.
    for (uint32 i = 0; i < m_TargetBoneCount; ++i)
    {
        const uint32 b = m_TargetTopo[i];
        const int32 parent = m_TargetParent[b];
        const Mathematics::Quaternion parentWorld = (parent < 0)
            ? Mathematics::Quaternion::Identity()
            : m_TargetWorldClipScratch[static_cast<size_t>(parent)];

        const HumanBone canon = m_TargetBoneCanonical[b];
        const auto& cr = (canon != HumanBone::None)
            ? m_Canonical[static_cast<size_t>(canon)]
            : m_Canonical[0]; // unused when canon is None
        const bool retarget =
            canon != HumanBone::None &&
            cr.HasSource &&
            cr.HasTarget &&
            cr.SourceBoneIndex < m_SourceBoneCount;

        if (retarget)
        {
            // Composite delta+target-bind multiply, precomputed at Build time.
            // tgtW = srcW * inv(srcWB) * tgtWB folds into srcW * BindCorrection.
            const Mathematics::Quaternion srcW   = m_SourceWorldClipScratch[cr.SourceBoneIndex];
            const Mathematics::Quaternion tgtW   = (srcW * cr.BindCorrection).Normalized();
            const Mathematics::Quaternion tgtLoc = (Inverse(parentWorld) * tgtW).Normalized();

            outPose.Rotations[b] = tgtLoc;
            m_TargetWorldClipScratch[b] = tgtW;
        }
        else
        {
            const Mathematics::Quaternion bindLoc = outPose.Rotations[b];
            m_TargetWorldClipScratch[b] = (parentWorld * bindLoc).Normalized();
        }
    }
}

void RetargetNode::TranslateStage3(const AnimationPose& sourcePose,
                                   AnimationPose& outPose) const
{
    // Stage 3 — Translation retargeting.
    //   For bones in HumanoidRig.TranslationBones (default: hip):
    //     tgtTranslation = (tgtHipHeight / srcHipHeight) * srcTranslation
    //   Non-TranslationBones get tgtTranslation = tgtRetargetPose.position
    //   (eliminates FBX joint-translation jitter).
    //
    // FillTargetRestPose already wrote rest translations for every target
    // bone, so non-translation bones are correct. We only need to overwrite
    // the bones in TranslationBones.

    const float scale =
        (m_SourceHipHeight > 1e-6f) ? (m_TargetHipHeight / m_SourceHipHeight) : 1.0f;

    for (HumanBone bone : m_TargetRig->TranslationBones())
    {
        const auto* cr = FindCanonical(bone);
        if (!cr || !cr->HasSource || !cr->HasTarget) continue;
        if (cr->SourceBoneIndex >= sourcePose.BoneCount) continue;

        const Mathematics::Vector3 src = sourcePose.Positions[cr->SourceBoneIndex];
        outPose.Positions[cr->TargetBoneIndex] = src * scale;
    }
}

void RetargetNode::Evaluate(EvaluationContext& ctx, AnimationPose& outPose)
{
    if (!m_Built || !m_SourceProvider || !ctx.ScratchStack)
    {
        outPose.Resize(m_TargetBoneCount);
        return;
    }

    // Source pose: evaluate child node into a stack-borrowed scratch.
    PoseStack::Frame sourceFrame(*ctx.ScratchStack);
    AnimationPose& sourcePose = sourceFrame.Pose();
    m_SourceProvider->Evaluate(ctx, sourcePose);

    // Phase 25.1+: Stage 2 (world-space transport) reads sourcePose
    // directly and applies per-bone Q during the source clip world walk —
    // no Stage 1 srcDelta encode needed at runtime.
    FillTargetRestPose(outPose);
    TransportStage2(sourcePose, outPose);
    TranslateStage3(sourcePose, outPose);
}

} // namespace Animation
} // namespace GameEngine
