#include "ECSModules/Rendering/Systems/AnimationGraphSystem.h"
#include "ECSModules/Rendering/Systems/AnimationGraphCompileFromModel.h"

#include "Animation/AnimationEventCollectorStore.h"
#include "Animation/AnimationGraphPlayer.h"
#include "Animation/AnimationGraphSerializer.h"
#include "Animation/AnimationGraphStore.h"
#include "Animation/EvaluationContext.h"
#include "Animation/Nodes/AdditiveBlendNode.h"
#include "Animation/Nodes/Blend2Node.h"
#include "Animation/Nodes/BlendSpace1DNode.h"
#include "Animation/Nodes/BlendSpace2DNode.h"
#include "Animation/Nodes/ClipPlayerNode.h"
#include "Animation/Nodes/FABRIKNode.h"
#include "Animation/Nodes/LayeredBlendNode.h"
#include "Animation/Nodes/LookAtNode.h"
#include "Animation/Nodes/MontageSlotNode.h"
#include "Animation/Nodes/StateMachineNode.h"
#include "Animation/Nodes/TwoBoneIKNode.h"
#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"
#include "Assets/AnimationGraphAsset.h"
#include "Assets/AssetManager.h"
#include "Animation/RootMotion.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "PhysicsECS/Components/CharacterController.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TimelinePlayback.h"
#include "Logger/Logger.h"
#include "Types/StringId.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace GameEngine { namespace Engine::Renderer {

namespace
{

using Components::Animator;
using Components::AnimatorGraphParamKind;
using Components::AnimatorGraphParamWrite;
using Components::AnimatorPlaybackCommand;
using Components::AnimatorPlaybackSource;
using Components::AnimatorRef;
using Components::CharacterController;
using Components::SkeletonRef;
using Components::Transform;
using Animation::AdditiveBlendNode;
using Animation::AnimGraphNode;
using Animation::AnimationGraphPlayer;
using Animation::AnimationGraphSerializer;
using Animation::AnimationGraphStore;
using Animation::Blend2Node;
using Animation::BlendSpace1DNode;
using Animation::BlendSpace2DNode;
using Animation::ClipPlayerNode;
using Animation::EvaluationContext;
using Animation::FABRIKNode;
using Animation::LayeredBlendNode;
using Animation::LookAtNode;
using Animation::MontageSlotNode;
using Animation::RootMotionDelta;
using Animation::StateMachineNode;
using Animation::TwoBoneIKNode;

constexpr float32 kMaxRootMotionSpeed = 20.0f;
constexpr float32 kMinRootMotionDiscontinuityMeters = 2.0f;

void ApplyRootMotion(ECS::World& world, ECS::EntityHandle rootEntity, const RootMotionDelta& delta)
{
    auto* transform = world.GetComponentForWrite<Transform>(rootEntity);
    if (!transform)
        return;

    Mathematics::Vector3 disp = transform->GetRotation().Rotate(delta.Translation);
    if (auto* character = world.GetComponentForWrite<CharacterController>(rootEntity);
        character && ECS::Entity(&world, rootEntity).IsEnabled<CharacterController>())
    {
        character->animationDisplacementX = disp.x;
        character->animationDisplacementY = disp.y;
        character->animationDisplacementZ = disp.z;
        character->hasAnimationDisplacement = true;
        return;
    }
    transform->Translate(disp.x, disp.y, disp.z);
}

void BindClipPlayers(AnimGraphNode* node, AssetManager& assetManager)
{
    if (!node)
        return;

    if (auto* clip = dynamic_cast<ClipPlayerNode*>(node))
    {
        if (clip->GetClip() || clip->GetClipGuid().IsNull())
            return;
        const uint32 index =
            ClipStore::Instance().GetOrLoadClipIndex(clip->GetClipGuid(), assetManager);
        if (index == 0)
            return;
        auto resolved = ClipStore::Instance().Get(index);
        if (resolved)
            clip->SetClip(resolved);
        return;
    }

    if (auto* blend = dynamic_cast<Blend2Node*>(node))
    {
        BindClipPlayers(const_cast<AnimGraphNode*>(blend->GetInputA()), assetManager);
        BindClipPlayers(const_cast<AnimGraphNode*>(blend->GetInputB()), assetManager);
        return;
    }

    if (auto* sm = dynamic_cast<StateMachineNode*>(node))
    {
        for (const auto& state : sm->GetStates())
            BindClipPlayers(state.Node.get(), assetManager);
        return;
    }

    if (auto* bs = dynamic_cast<BlendSpace1DNode*>(node))
    {
        for (const auto& sample : bs->GetSamples())
            BindClipPlayers(sample.Node.get(), assetManager);
        return;
    }

    if (auto* bs2 = dynamic_cast<BlendSpace2DNode*>(node))
    {
        for (const auto& sample : bs2->GetSamples())
            BindClipPlayers(sample.Node.get(), assetManager);
        return;
    }

    if (auto* layered = dynamic_cast<LayeredBlendNode*>(node))
    {
        BindClipPlayers(const_cast<AnimGraphNode*>(layered->GetBase()), assetManager);
        BindClipPlayers(const_cast<AnimGraphNode*>(layered->GetOverlay()), assetManager);
        return;
    }

    if (auto* additive = dynamic_cast<AdditiveBlendNode*>(node))
    {
        BindClipPlayers(const_cast<AnimGraphNode*>(additive->GetBase()), assetManager);
        BindClipPlayers(const_cast<AnimGraphNode*>(additive->GetAdditive()), assetManager);
        return;
    }

    if (auto* ik = dynamic_cast<TwoBoneIKNode*>(node))
    {
        BindClipPlayers(const_cast<AnimGraphNode*>(ik->GetSource()), assetManager);
        return;
    }

    if (auto* fabrik = dynamic_cast<FABRIKNode*>(node))
    {
        BindClipPlayers(const_cast<AnimGraphNode*>(fabrik->GetSource()), assetManager);
        return;
    }

    if (auto* lookAt = dynamic_cast<LookAtNode*>(node))
    {
        BindClipPlayers(const_cast<AnimGraphNode*>(lookAt->GetSource()), assetManager);
        return;
    }

    if (auto* slot = dynamic_cast<MontageSlotNode*>(node))
        BindClipPlayers(const_cast<AnimGraphNode*>(slot->GetSource()), assetManager);
}

uint64 InstantiateGraph(const GUID& graphGuid, AssetManager& assetManager)
{
    SharedPtr<Asset> base = assetManager.GetAsset(graphGuid);
    if (!base)
    {
        AssetFuture future = assetManager.LoadAssetAsync(graphGuid, AssetLoadPriority::High);
        if (!future.Valid())
            return 0;
        base = future.get();
    }
    auto* graphAsset = dynamic_cast<::GameEngine::AnimationGraphAsset*>(base.get());
    if (!graphAsset)
    {
        Logger::Log::Error("AnimationGraphSystem: GUID {} is not an AnimationGraph",
                           graphGuid.ToCompactString());
        return 0;
    }
    if (!graphAsset->IsLoaded() && !graphAsset->Load())
        return 0;

    auto player = AnimationGraphSerializer::Deserialize(graphAsset->GetDocument());
    if ((!player || !player->RootNode) && LooksLikeAuthoringModel(graphAsset->GetDocument()))
        player = CompileAuthoringModel(graphAsset->GetDocument());
    if (!player || !player->RootNode)
        return 0;
    BindClipPlayers(player->RootNode.get(), assetManager);
    return AnimationGraphStore::Instance().Create(std::move(player));
}

void ReleaseGraph(Animator& animator)
{
    if (animator.graphRuntimeId != 0)
        AnimationGraphStore::Instance().Destroy(animator.graphRuntimeId);
    animator.graphRuntimeId = 0;
    animator.graphInstanceGuid = GUID{};
    animator.graphPaused = false;
    animator.pendingGraphParamCount = 0;
}

uint32 ApplyPendingParams(Animator& animator, AnimationGraphPlayer& player, StringId* outTriggers)
{
    uint32 triggerCount = 0;
    for (uint32 i = 0; i < animator.pendingGraphParamCount; ++i)
    {
        const AnimatorGraphParamWrite& stamp = animator.pendingGraphParams[i];
        if (stamp.id == 0)
            continue;
        switch (stamp.kind)
        {
        case AnimatorGraphParamKind::Float:
            player.SetFloat(stamp.id, stamp.floatValue);
            break;
        case AnimatorGraphParamKind::Bool:
            player.SetBool(stamp.id, stamp.boolValue != 0);
            break;
        case AnimatorGraphParamKind::Trigger:
            player.SetTrigger(stamp.id);
            if (outTriggers && triggerCount < Animator::kPendingGraphParamCapacity)
                outTriggers[triggerCount++] = stamp.id;
            break;
        }
    }
    animator.pendingGraphParamCount = 0;
    return triggerCount;
}

void ConsumeGraphCommand(Animator& animator)
{
    switch (animator.pendingCommand)
    {
    case AnimatorPlaybackCommand::Pause:
        animator.graphPaused = true;
        break;
    case AnimatorPlaybackCommand::Play:
    case AnimatorPlaybackCommand::PlayWithBlend:
    case AnimatorPlaybackCommand::PlaySection:
        animator.graphPaused = false;
        if (animator.graphRuntimeId == 0)
            animator.graphInstanceGuid = GUID{};
        break;
    case AnimatorPlaybackCommand::Stop:
        ReleaseGraph(animator);
        animator.graphInstanceGuid = animator.graphGuid.ToGuid();
        animator.graphPaused = true;
        break;
    case AnimatorPlaybackCommand::Seek:
    case AnimatorPlaybackCommand::None:
    default:
        break;
    }
    animator.pendingCommand = AnimatorPlaybackCommand::None;
}

// Bit-exact: a paused graph re-evaluates the identical pose, and no epsilon
// may hide a real palette change.
template <typename Element>
bool SameBits(const std::vector<Element>& a, const std::vector<Element>& b)
{
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(Element)) == 0);
}

bool SamePoseBits(const Animation::AnimationPose& a, const Animation::AnimationPose& b)
{
    static_assert(sizeof(Mathematics::Vector3) == 3 * sizeof(float32), "pose comparison assumes unpadded vectors");
    static_assert(sizeof(Mathematics::Quaternion) == 4 * sizeof(float32), "pose comparison assumes unpadded quaternions");
    return a.BoneCount == b.BoneCount && SameBits(a.Positions, b.Positions) &&
           SameBits(a.Rotations, b.Rotations) && SameBits(a.Scales, b.Scales);
}

} // namespace

bool AnimationGraphSystem::PaletteInputsChanged() const
{
    if (m_FramePaletteInputs != m_LastPaletteInputs || m_FramePoseCount != m_LastPoseCount)
        return true;
    for (uint32 slot = 0; slot < m_FramePoseCount; ++slot)
    {
        if (!SamePoseBits(m_FramePoses[slot], m_LastPoses[slot]))
            return true;
    }
    return false;
}

void AnimationGraphSystem::Update(ECS::World& world, float32 deltaTime)
{
    auto& store = AnimationGraphStore::Instance();
    auto& skStore = SkeletonStore::Instance();
    m_WrittenRuntimes.clear();
    m_FramePaletteInputs.clear();
    m_FramePoseCount = 0;

    world.Query<ECS::Write<Animator>>().Each([&](ECS::EntityHandle rootEntity, Animator& animator) {
        if (!animator.active || animator.source != AnimatorPlaybackSource::Graph)
        {
            if (animator.graphRuntimeId != 0)
                ReleaseGraph(animator);
            return;
        }

        ConsumeGraphCommand(animator);

        if (animator.source != AnimatorPlaybackSource::Graph)
            return;

        const GUID requested = animator.graphGuid.ToGuid();
        if (!requested.IsNull() && requested != animator.graphInstanceGuid)
        {
            if (animator.graphRuntimeId != 0)
                store.Destroy(animator.graphRuntimeId);
            animator.graphRuntimeId = InstantiateGraph(requested, EngineCore::GetInstance().GetAssetManager());
            animator.graphInstanceGuid = requested;
        }

        CollectEntitiesInSubtree(world, rootEntity, m_Subtree);
        for (ECS::EntityHandle entity : m_Subtree)
        {
            auto* animRef = world.GetComponent<AnimatorRef>(entity);
            if (!animRef || animRef->ClipIndex == 0)
                continue;
            AnimatorRef updated = *animRef;
            updated.ClipIndex = 0;
            updated.ClearBlend();
            world.AddComponentImmediate(entity, updated);
        }

        AnimationGraphPlayer* player = store.Get(animator.graphRuntimeId);
        if (!player && animator.graphRuntimeId != 0 && !requested.IsNull())
        {
            // Stale handle (duplicate teardown, generation mismatch). Re-bind.
            animator.graphRuntimeId = InstantiateGraph(requested, EngineCore::GetInstance().GetAssetManager());
            animator.graphInstanceGuid = requested;
            player = store.Get(animator.graphRuntimeId);
        }
        if (!player)
            return;

        EvaluationContext ctx;
        ctx.DeltaTime = animator.graphPaused
            ? 0.0f
            : std::max(0.0f, deltaTime) * std::max(0.0f, animator.speedScale);
        ctx.Events = Animation::AnimationEventCollectorStore::Instance().Get(animator.eventCollectorId);
        for (ECS::EntityHandle entity : m_Subtree)
        {
            auto* sref = world.GetComponent<SkeletonRef>(entity);
            if (!sref || sref->skeletonId == 0)
                continue;
            ctx.TargetSkeleton = skStore.Get(sref->skeletonId);
            if (ctx.TargetSkeleton)
                break;
        }

        StringId triggers[Animator::kPendingGraphParamCapacity] = {};
        const uint32 triggerCount = ApplyPendingParams(animator, *player, triggers);
        player->Evaluate(ctx, m_Pose);
        for (uint32 i = 0; i < triggerCount; ++i)
            player->ResetTrigger(triggers[i]);

        if (animator.rootMotionLocal)
        {
            const bool hadPrevious = player->RootMotion.HasPrevious();
            const RootMotionDelta delta = player->RootMotion.Extract(m_Pose);
            const float32 maxLen = std::max(kMinRootMotionDiscontinuityMeters,
                                            kMaxRootMotionSpeed * ctx.DeltaTime);
            if (!animator.graphPaused && hadPrevious && delta.Translation.Length() <= maxLen)
                ApplyRootMotion(world, rootEntity, delta);
        }

        const size_t recordsBefore = m_FramePaletteInputs.size();
        for (ECS::EntityHandle entity : m_Subtree)
        {
            auto* animRef = world.GetComponent<AnimatorRef>(entity);
            auto* sref = world.GetComponent<SkeletonRef>(entity);
            if (!animRef || !sref || sref->skeletonId == 0 || sref->runtimeId == 0)
                continue;

            if (!m_WrittenRuntimes.insert(sref->runtimeId).second)
                continue;

            const auto* skel = skStore.Get(sref->skeletonId);
            auto* runtime = skStore.GetRuntime(sref->runtimeId);
            if (!skel || !runtime || skel->BoneCount == 0)
                continue;

            Animation::EmitSkinMatricesFromAnimationPose(*skel, m_Pose, runtime->CompactSkinMatrices, m_Workspace);
            m_FramePaletteInputs.push_back(PaletteInputs{sref->runtimeId, sref->skeletonId, m_FramePoseCount});
        }
        if (m_FramePaletteInputs.size() != recordsBefore)
        {
            if (m_FramePoses.size() == m_FramePoseCount)
                m_FramePoses.emplace_back();
            m_FramePoses[m_FramePoseCount++].CopyFrom(m_Pose);
        }
    });

    // Report palette content only when it can differ from the last update's
    // (m_LastPaletteInputs contract in the header).
    const bool paletteChanged = PaletteInputsChanged();
    m_LastPaletteInputs.swap(m_FramePaletteInputs);
    m_LastPoses.swap(m_FramePoses);
    m_LastPoseCount = m_FramePoseCount;
    if (paletteChanged && m_RenderServices)
        m_RenderServices->NotifySkinPaletteContentChanged(world.GetWorldId());
}

} } // namespace GameEngine::Engine::Renderer
