#pragma once

#include "Animation/AnimGraphNode.h"
#include "Animation/HumanBone.h"
#include "Animation/HumanoidRig.h"
#include "Animation/RetargetMap.h"

#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"

#include <array>
#include <cstdint>
#include <vector>

namespace GameEngine
{
namespace Animation
{

class SkeletonProfile;
struct SkeletonData;
struct AnimationPose;
struct EvaluationContext;

// CPU-first implementation of Phase 3 of the Humanoid Retargeting plan.
//
// RetargetNode runs Stages 1-3 of the 6-stage pipeline:
//   * Stage 1 — Encode (Formulation A bake-with-parent-shift, baked once at
//     session-build time; runtime is one quat copy + 2 quat muls per bone).
//   * Stage 2 — FK transport (Mode A canonical-axis-frame; Mode B
//     transport-then-slerp; Mode C none; Mode D world-rotation-slerp-along-arc).
//   * Stage 3 — Translation retargeting on TranslationBones with hip-height
//     proportional scale; non-translation bones forced to retarget pose.
//
// Stages 0 (clip sample), 4 (IK goals), 5 (IK), 6 (op stack) live elsewhere
// or are deferred to later phases.
//
// Cross-profile is supported but axis frames are treated as identity at this
// stage (M_src == M_tgt == identity). Same-profile is the dominant Synty +
// Mixamo + MetaHuman case and is the fast path.
class RetargetNode : public AnimGraphNode
{
public:
    RetargetNode() = default;

    // Configure pre-Evaluate. None of these are owning. Pointers must outlive
    // the RetargetNode. Build() must be called after Configure() and before
    // the first Evaluate(); subsequent rig/map mutations (asset reload) need
    // a fresh Build() call.
    void Configure(AnimGraphNode* sourceProvider,
                   const HumanoidRig* sourceRig,
                   const HumanoidRig* targetRig,
                   const RetargetMap* map);

    // Update only the source pose provider without invalidating the cached
    // build (rig + map dispatch tables stay valid). The full Configure()
    // resets m_Built = false; callers re-binding the per-frame provider
    // pointer should use this seam instead so Evaluate() doesn't fall
    // through its !m_Built early-return path with an uninitialized output
    // pose. Pointer must outlive the next Evaluate() call.
    void SetSourceProvider(AnimGraphNode* sourceProvider) {
        m_SourceProvider = sourceProvider;
    }

    // (Re)compute the cached bake-rewrite quaternion per source bone and the
    // chain dispatch tables. Idempotent. Returns false if any of the
    // configured pointers are null or the source/target skeletons are
    // missing required bones.
    bool Build(const SkeletonData& sourceSkeleton,
               const SkeletonData& targetSkeleton);

    bool IsBuilt() const { return m_Built; }

    // AnimGraphNode contract. ctx.SourceSkeleton must be set; outPose is
    // resized to ctx.TargetSkeleton->BoneCount.
    void Evaluate(EvaluationContext& ctx, AnimationPose& outPose) override;

    // Number of chain dispatches built (one per RetargetMap pairing whose
    // src + tgt rigs both define the chain and have at least one bone
    // resolving on each side). 0 means TransportStage2 is a no-op and the
    // output pose stays at FillTargetRestPose values (target bind pose).
    size_t ChainCount() const { return m_Chains.size(); }

    // Per-chain bone counts, indexed parallel to ChainCount(). Used by
    // diagnostic surfaces to surface chains that are present but empty
    // (rig has the chain but no canonical bone resolved).
    size_t ChainSourceBoneCount(size_t chainIdx) const {
        return chainIdx < m_Chains.size() ? m_Chains[chainIdx].SourceBones.size() : 0;
    }
    size_t ChainTargetBoneCount(size_t chainIdx) const {
        return chainIdx < m_Chains.size() ? m_Chains[chainIdx].TargetBones.size() : 0;
    }

    // Snapshot of post-Build internals shaped for the GPU data store. Filled
    // by ExportForGPU(). All vectors sized by srcBoneCount or tgtBoneCount.
    // The GPU pipeline reads this to flatten per-rig-pair state into a
    // single SSBO; CPU evaluation continues to use the private members
    // directly. Only the data the fused retarget_full.comp consumes is
    // surfaced — IK / op-stack state is deferred to Phase B.
    struct GPUExport {
        uint32_t SourceBoneCount = 0;
        uint32_t TargetBoneCount = 0;
        std::vector<int32>                   SourceParent;        // -1 = root
        std::vector<uint32>                  SourceTopo;          // parent-before-child
        std::vector<Mathematics::Quaternion> SourceRestRot;       // local rest rotation per src bone
        std::vector<int32>                   TargetParent;
        std::vector<uint32>                  TargetTopo;
        std::vector<Mathematics::Quaternion> TargetBindLocal;     // authored local bind per tgt bone
        std::vector<Mathematics::Vector3>    TargetRestPos;       // rest local translation per tgt bone
        // Per-target-bone canonical routing (sized = TargetBoneCount).
        // SourceIdx == ~0u for non-canonical bones; BindCorrection is identity.
        std::vector<uint32>                  TargetSourceIdx;
        std::vector<Mathematics::Quaternion> TargetBindCorrection;
        // Stage 3 translation.
        uint32_t SourceHipBoneIndex = ~0u;
        uint32_t TargetHipBoneIndex = ~0u;
        float    HipScale = 1.0f;
        std::vector<uint32>                  TranslationTargetBones;
    };
    void ExportForGPU(GPUExport& out) const;

private:
    // Per-canonical-bone routing built once at Build() time.
    struct CanonicalRouting
    {
        bool   HasSource = false;
        bool   HasTarget = false;
        uint32_t SourceBoneIndex = ~0u;
        uint32_t TargetBoneIndex = ~0u;
        Mathematics::Quaternion SourceRetargetPose;  // identity if missing
        Mathematics::Quaternion TargetRetargetPose;  // identity if missing
        // Composite world-space transport: inv(src_canonical_world) *
        // tgt_canonical_world. Multiplied into the per-frame source clip
        // world to produce target world directly:
        //     tgtW = srcW * BindCorrection
        // (= srcW * inv(srcWB) * tgtWB, by associativity). Saves one
        // quaternion inverse and one multiply per canonical bone per frame
        // vs. the naive `(srcW * inv(srcWB)) * tgtWB`. Identity when either
        // side of the canonical pair is missing.
        Mathematics::Quaternion BindCorrection;
    };

    // Per-chain dispatch table. Built once at Build() time.
    struct ChainDispatch
    {
        ChainKind     Kind = ChainKind::Other;
        FKRotationMode    RotationMode = FKRotationMode::OneToOne;
        float            RotationAlpha = 1.0f;
        FKTranslationMode TranslationMode = FKTranslationMode::None;
        // Source-chain bones in the source rig (root-to-tip order, by
        // source-skeleton bone index). Pulled from src rig's HumanoidChain.
        std::vector<uint32_t> SourceBones;
        // Target-chain bones in the target rig (root-to-tip order, by
        // target-skeleton bone index). Pulled from tgt rig's HumanoidChain.
        std::vector<uint32_t> TargetBones;
        // Parallel canonical-bone arrays (so we can look up retarget pose +
        // tgtRetargetPose without a second indirection).
        std::vector<HumanBone> SourceCanonical;
        std::vector<HumanBone> TargetCanonical;
    };

    // Stage 2 — World-space FK retarget. Single topological walk over the
    // target skeleton. For canonical bones with a mapped source: applies
    // the world-space delta `target_world = (src_world * inv(src_world_bind))
    // * target_world_bind`, then converts to local via the running parent
    // world. For non-canonical bones: keeps the bind local and just
    // accumulates parent world for downstream children. This sidesteps the
    // per-bone-axis-frame mismatch that the legacy local-frame formula
    // (`qTgtLocal = TargetRetargetPose * inv(srcBind) * key`) cannot bridge
    // across rigs whose local axes differ.
    //
    // Source clip world rotations are computed once from `sourcePose` into
    // m_SourceWorldClipScratch before the target walk. Mutates the per-frame
    // scratch buffers; non-const.
    void TransportStage2(const AnimationPose& sourcePose,
                         AnimationPose& outPose);

    // Stage 3 — translation retargeting.
    void TranslateStage3(const AnimationPose& sourcePose,
                         AnimationPose& outPose) const;

    // Look up canonical routing by HumanBone enum. Returns nullptr if the
    // bone is not part of the canonical mapping.
    const CanonicalRouting* FindCanonical(HumanBone bone) const;

    // Per-bone identity-vs-retarget map filler used by Stage 2 default path.
    // Non-chain bones get target's retarget pose written through.
    void FillTargetRestPose(AnimationPose& outPose) const;

    // Configuration (non-owning).
    AnimGraphNode*       m_SourceProvider = nullptr;
    const HumanoidRig*   m_SourceRig      = nullptr;
    const HumanoidRig*   m_TargetRig      = nullptr;
    const RetargetMap*   m_Map            = nullptr;

    // Built tables.
    bool                 m_Built = false;
    uint32_t             m_SourceBoneCount = 0;
    uint32_t             m_TargetBoneCount = 0;
    // Per-source-bone caches indexed by source-skeleton bone index.
    std::vector<Mathematics::Quaternion> m_SourceRestRot;   // bind_src[b] (rest local rotation; fallback when clip is shorter than skeleton)
    std::vector<HumanBone> m_SourceBoneCanonical;           // canonical slot for source bone, or HumanBone::None
    std::vector<int32>     m_SourceParent;                  // parent bone index per source bone (-1 = root)
    std::vector<uint32>    m_SourceTopo;                    // topological order, parent-before-child (or 0..N-1 fallback)
    std::vector<uint32>    m_TargetTopo;                    // ditto for target

    // Per-target-bone retarget pose lookup (target bind composed with
    // RetargetPoseRotation; identity for non-mapped bones). Stage 3 writes
    // this through for non-translation, non-chain-touched bones.
    std::vector<Mathematics::Quaternion> m_TargetRetargetPoseLocal; // sized = m_TargetBoneCount
    std::vector<Mathematics::Vector3>    m_TargetRestPos;           // sized = m_TargetBoneCount
    std::vector<Mathematics::Vector3>    m_TargetRestScale;         // sized = m_TargetBoneCount
    std::vector<int32>                   m_TargetParent;            // sized = m_TargetBoneCount

    // Per-bone world rest rotations cached at Build() time. Used by the
    // world-space retarget transport: delta_world = src_world_clip *
    // inverse(src_world_bind); tgt_world = delta_world * tgt_world_bind. This
    // sidesteps the per-bone-axis-frame mismatch that the legacy local-frame
    // formula (qTgtLocal = TargetRetargetPose * inv(srcBind) * key) cannot
    // bridge across rigs whose local axes differ.
    //
    // Phase 25.1: The "world bind" here is the *canonical* T-pose world
    // rotation, not the rig's authored bind. BakeRetargetPoseFromAPose
    // computes a per-bone Q (RetargetPoseRotation in BoneMap) that aligns
    // each rig's authored bind direction to canonical LocalForward; we
    // compose those Q-corrected locals here so both rigs land at a shared
    // canonical at retarget bind, making delta transport valid even when
    // authored binds disagree.
    std::vector<Mathematics::Quaternion> m_SourceWorldBind;         // sized = m_SourceBoneCount
    std::vector<Mathematics::Quaternion> m_TargetWorldBind;         // sized = m_TargetBoneCount

    // Per-bone Q (RetargetPoseRotation in BoneMap, mirrored here for hot-
    // path access). Identity for non-canonical bones. Applied as bone-local
    // pre-rotation during the source clip world walk so the clip is read
    // in canonical frame: canonical_local_clip = Q * authored_clip_local.
    std::vector<Mathematics::Quaternion> m_SourceCanonicalQ;        // sized = m_SourceBoneCount
    std::vector<Mathematics::Quaternion> m_TargetCanonicalQ;        // sized = m_TargetBoneCount

    // Per-frame scratch for the world-space retarget walk. Reused across
    // Evaluate() calls; sized in Build() to match each skeleton's bone count.
    std::vector<Mathematics::Quaternion> m_SourceWorldClipScratch;
    std::vector<Mathematics::Quaternion> m_TargetWorldClipScratch;

    // Canonical -> routing.
    std::array<CanonicalRouting, static_cast<size_t>(HumanBone::Count)> m_Canonical{};

    // Per-target-bone canonical lookup. Indexed by target-skeleton bone
    // index; HumanBone::None for non-canonical bones. Used by the world-
    // space retarget walk to decide per-bone whether to apply the
    // delta-from-source or fall back to bind local.
    std::vector<HumanBone> m_TargetBoneCanonical;

    // Per-chain dispatch (one entry per ChainPairing in the map that maps to
    // a chain on both rigs).
    std::vector<ChainDispatch> m_Chains;

    // Body proportions cached for Stage 3 hip scale.
    float m_SourceHipHeight = 0.0f;
    float m_TargetHipHeight = 0.0f;
};

} // namespace Animation
} // namespace GameEngine
