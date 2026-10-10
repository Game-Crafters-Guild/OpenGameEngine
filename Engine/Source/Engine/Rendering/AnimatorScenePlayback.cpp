#include "Engine/Rendering/AnimatorScenePlayback.h"

#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "AnimatorSourceResolution.h"
#include "Engine/Rendering/TimelinePlayback.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <chrono>
#include <string>

namespace GameEngine::Engine::Renderer
{
namespace
{
using Components::Animator;
using Components::AnimatorPlaybackCommand;
using Components::AnimatorPlaybackSource;
using Components::AnimatorRef;
using Components::MeshRenderer;
using Components::SkeletonRef;

SharedPtr<Asset> TryAcquirePlaybackModel(AssetManager& assetManager, const GUID& guid, bool* sourcePending)
{
    if (guid.IsNull())
        return nullptr;
    if (SharedPtr<Asset> cached = assetManager.GetAsset(guid))
    {
        if (cached->IsLoaded())
            return cached;
        if (cached->GetState() == AssetState::Loading)
        {
            if (sourcePending) *sourcePending = true;
            return nullptr;
        }
    }
    AssetFuture future = assetManager.LoadAssetAsync(guid, AssetLoadPriority::High);
    if (!future.Valid())
        return nullptr;
    // Model completion may need a main-thread upload. Neither Editor entry
    // nor the runtime command owner may block waiting for that same thread.
    if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
    {
        if (sourcePending) *sourcePending = true;
        return nullptr;
    }
    return future.get();
}

const char* DescribeSourceKind(AnimatorPlaybackSource source)
{
    switch (source)
    {
        case AnimatorPlaybackSource::Library: return "animation library";
        case AnimatorPlaybackSource::Controller: return "animation controller";
        case AnimatorPlaybackSource::Timeline: return "timeline";
        case AnimatorPlaybackSource::Graph: return "animation graph";
        case AnimatorPlaybackSource::Clip:
        default: return "clip";
    }
}

// The one warning for a library or controller start that has nothing to play: the source, the
// library entry it used and the clip that resolved to nothing, so the log names what to fix.
void WarnNothingToPlay(const Animator& animator, const ResolvedAnimatorPlayback& playback)
{
    const GUID sourceGuid = animator.source == AnimatorPlaybackSource::Library ? animator.libraryGuid.ToGuid()
                                                                               : animator.controllerGuid.ToGuid();
    std::string detail;
    if (!playback.LibraryEntry.empty())
        detail = "entry '" + playback.LibraryEntry + "'";
    if (!playback.UnresolvedClip.IsNull())
        detail += (detail.empty() ? "it names clip " : " names clip ") + playback.UnresolvedClip.ToString() +
                  ", which no loaded or indexed model contains";
    Logger::Log::Warning("Animator: no clip to play from {} {}{}; it stays at rest", DescribeSourceKind(animator.source),
                         sourceGuid.ToString(), detail.empty() ? std::string() : " (" + detail + ")");
}

// Diagnostic form of the durable source: an unresolved clip is almost always a
// stale or absent model/index pair, and the warning has to name it.
std::string DescribeClipSource(const Animator& animator)
{
    const GUID model = animator.clipSourceModelGuid.ToGuid();
    if (model.IsNull() || animator.clipSourceAnimationIndex == UINT32_MAX)
        return "none recorded";
    return "model " + model.ToString() + " animation "
        + std::to_string(animator.clipSourceAnimationIndex);
}

GUID FindModelGuidInSubtree(ECS::World& world, const std::vector<ECS::EntityHandle>& subtree)
{
    for (ECS::EntityHandle entity : subtree)
    {
        const auto* meshRenderer = world.GetComponent<MeshRenderer>(entity);
        if (!meshRenderer)
            continue;
        const GUID modelGuid = meshRenderer->modelAssetGuid.ToGuid();
        if (!modelGuid.IsNull())
            return modelGuid;
    }
    return GUID::Null();
}

uint32_t ResolveEmbeddedClipIndex(const Animator& animator,
                                  const GUID& modelGuid,
                                  AssetManager& assetManager, bool* sourcePending)
{
    SharedPtr<Asset> asset = TryAcquirePlaybackModel(assetManager, modelGuid, sourcePending);
    auto* modelAsset = dynamic_cast<ModelAsset*>(asset.get());
    if (!modelAsset || !modelAsset->IsLoaded())
        return 0;

    const auto& names = modelAsset->GetAnimationNames();
    const auto& embedGuids = modelAsset->GetEmbeddedClipGuids();
    if (names.empty() || names.size() != embedGuids.size())
        return 0;

    const std::string_view animationName = animator.AssignedAnimation();
    if (!animationName.empty())
    {
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (names[i] != animationName)
                continue;
            if (uint32_t clipIndex = ResolveClipIndex(embedGuids[i], assetManager); clipIndex != 0)
                return clipIndex;
        }
    }

    return ResolveClipIndex(embedGuids.front(), assetManager);
}

uint32_t ResolveAnimatorClipIndexImpl(const Animator& animator,
                                  const GUID& fallbackModel,
                                  AssetManager& assetManager, bool* sourcePending)
{
    if (sourcePending) *sourcePending = false;
    const auto& registry = assetManager.GetRegistry();
    const GUID selected = registry.ResolveGuid(animator.clipGuid.ToGuid());
    if (!animator.clipSourceModelGuid.IsNull() && animator.clipSourceAnimationIndex != UINT32_MAX)
    {
        const GUID originalModel = animator.clipSourceModelGuid.ToGuid();
        const GUID modelGuid = registry.ResolveGuid(originalModel);
        const uint32 index = animator.clipSourceAnimationIndex;
        const GUID embedded = ModelAsset::DeriveEmbeddedClipGuid(modelGuid, index);
        if (selected.IsNull() || selected == embedded ||
            animator.clipGuid.ToGuid() == ModelAsset::DeriveEmbeddedClipGuid(originalModel, index))
            return ResolvePlaybackClipIndex(embedded, modelGuid, GUID::Null(), assetManager, sourcePending);
    }
    // Explicit clips, including standalone .anim and live embedded GUIDs, must
    // never be replaced by the target mesh's first animation.
    if (!selected.IsNull())
    {
        auto asset = assetManager.GetAsset(selected);
        AssetMetadata metadata{};
        const bool registered = asset || registry.TryGetAssetMetadata(selected, metadata);
        const bool model = asset ? asset->GetType() == AssetType::Model
                                 : registered && metadata.Type == AssetType::Model;
        if (model)
            return ResolveEmbeddedClipIndex(animator, selected, assetManager, sourcePending);
        // A scene authored without the source pair holds only the clip's GUID; it still selects
        // the exact authored clip.
        return ResolvePlaybackClipIndex(selected, GUID::Null(), fallbackModel, assetManager, sourcePending);
    }
    if (!animator.clipSourceModelGuid.IsNull() || animator.clipSourceAnimationIndex != UINT32_MAX)
        return 0;
    const GUID modelGuid = fallbackModel;
    if (!modelGuid.IsNull())
    {
        if (uint32_t clipIndex = ResolveEmbeddedClipIndex(animator, modelGuid, assetManager, sourcePending); clipIndex != 0)
            return clipIndex;
    }

    return ResolveClipIndex(animator.clipGuid.ToGuid(), assetManager);
}

void ApplyPlayStateToSubtree(ECS::World& world,
                             const std::vector<ECS::EntityHandle>& subtree,
                             uint32_t clipIndex,
                             float timeSeconds,
                             float speedScale,
                             bool loop,
                             float blendSeconds,
                             bool section,
                             float sectionStartSeconds,
                             float sectionEndSeconds)
{
    if (clipIndex == 0)
        return;

    uint32_t applied = 0;
    for (ECS::EntityHandle entity : subtree)
    {
        auto* anim = world.GetComponent<AnimatorRef>(entity);
        auto* skel = world.GetComponent<SkeletonRef>(entity);
        if (!anim || !skel)
            continue;
        ++applied;

        AnimatorRef updated = *anim;
        updated.SetAnimation(clipIndex, blendSeconds);
        updated.Time = timeSeconds;
        updated.Speed = speedScale;
        updated.SectionStart = sectionStartSeconds;
        updated.SectionEnd = sectionEndSeconds;
        updated.Flags &= ~(AnimatorRef::kFlag_Paused | AnimatorRef::kFlag_Loop | AnimatorRef::kFlag_Section);
        if (loop)
            updated.Flags |= AnimatorRef::kFlag_Loop;
        if (section)
            updated.Flags |= AnimatorRef::kFlag_Section;
        world.AddComponentImmediate(entity, updated);
    }

    // A resolved clip with nothing to drive is the silent-bind-pose bug class
    // (the model loaded as a static mesh — no SkinnedMeshRenderer, so the
    // resolve step never created AnimatorRef/SkeletonRef). Say so.
    if (applied == 0)
    {
        Logger::Log::Warning(
            "Animator: clip {} resolved but no entity in the subtree carries "
            "AnimatorRef+SkeletonRef — the model likely loaded unskinned (bind pose)",
            clipIndex);
    }
}

void ApplyRestToSubtree(ECS::World& world, const std::vector<ECS::EntityHandle>& subtree)
{
    for (ECS::EntityHandle entity : subtree)
    {
        auto* anim = world.GetComponent<AnimatorRef>(entity);
        auto* skel = world.GetComponent<SkeletonRef>(entity);
        if (!anim || !skel)
            continue;
        AnimatorRef updated = *anim;
        updated.SetAnimation(0);
        updated.Speed = 1.0f;
        updated.SectionStart = 0.0f;
        updated.SectionEnd = 0.0f;
        updated.Flags |= AnimatorRef::kFlag_Paused;
        updated.Flags &= ~AnimatorRef::kFlag_Section;
        world.AddComponentImmediate(entity, updated);
    }
}

uint32_t ApplyClipCommandToSubtree(ECS::World& world,
                                   const Animator& animator,
                                   const std::vector<ECS::EntityHandle>& subtree,
                                   uint32_t clipIndex)
{
    const AnimatorPlaybackCommand command = animator.pendingCommand;
    const bool autoplay = command == AnimatorPlaybackCommand::Autoplay;
    const bool section = (autoplay || command == AnimatorPlaybackCommand::PlaySection)
        && animator.sectionEndSeconds > animator.sectionStartSeconds;
    const float32 blendSeconds =
        autoplay || command == AnimatorPlaybackCommand::PlayWithBlend ? animator.blendSeconds : 0.0f;

    uint32_t applied = 0;
    for (ECS::EntityHandle entity : subtree)
    {
        auto* anim = world.GetComponent<AnimatorRef>(entity);
        auto* skel = world.GetComponent<SkeletonRef>(entity);
        if (!anim || !skel)
            continue;
        ++applied;

        AnimatorRef updated = *anim;
        switch (command)
        {
        case AnimatorPlaybackCommand::Pause:
            updated.Flags |= AnimatorRef::kFlag_Paused;
            break;
        case AnimatorPlaybackCommand::Seek:
            updated.Time = std::max(0.0f, animator.seekTimeSeconds);
            updated.Flags &= ~AnimatorRef::kFlag_Paused;
            break;
        case AnimatorPlaybackCommand::Play:
        case AnimatorPlaybackCommand::PlayWithBlend:
        case AnimatorPlaybackCommand::PlaySection:
        case AnimatorPlaybackCommand::Autoplay:
            updated.SetAnimation(clipIndex, blendSeconds);
            if (section)
                updated.Time = animator.sectionStartSeconds;
            else if (autoplay)
                updated.Time = std::max(0.0f, animator.seekTimeSeconds);
            updated.Speed = std::max(0.0f, animator.speedScale);
            updated.SectionStart = animator.sectionStartSeconds;
            updated.SectionEnd = animator.sectionEndSeconds;
            updated.Flags &= ~(AnimatorRef::kFlag_Paused | AnimatorRef::kFlag_Loop | AnimatorRef::kFlag_Section);
            if (animator.loop)
                updated.Flags |= AnimatorRef::kFlag_Loop;
            if (section)
                updated.Flags |= AnimatorRef::kFlag_Section;
            break;
        default:
            break;
        }
        world.AddComponentImmediate(entity, updated);
    }
    return applied;
}
} // namespace

uint32_t ResolvePlaybackClipIndex(const GUID& clipGuid,
                                  const GUID& sourceModel,
                                  const GUID& subtreeModel,
                                  AssetManager& assetManager,
                                  bool* sourcePending)
{
    if (sourcePending) *sourcePending = false;
    if (clipGuid.IsNull())
        return 0;
    auto& clipStore = ClipStore::Instance();
    if (const uint32_t clipIndex = clipStore.GetIndexIfPresent(clipGuid); clipIndex != 0)
        return clipIndex;

    const auto& registry = assetManager.GetRegistry();
    AssetMetadata metadata{};
    if (assetManager.GetAsset(clipGuid) || registry.TryGetAssetMetadata(clipGuid, metadata))
        return ResolveClipIndex(clipGuid, assetManager);

    GUID container = sourceModel;
    if (container.IsNull())
        container = registry.FindSubassetContainer(clipGuid);
    if (container.IsNull())
        container = subtreeModel;
    if (container.IsNull())
        return 0;
    container = registry.ResolveGuid(container);

    bool pending = false;
    SharedPtr<Asset> asset = TryAcquirePlaybackModel(assetManager, container, &pending);
    if (sourcePending) *sourcePending = pending;
    if (pending)
        return 0;
    auto* model = dynamic_cast<ModelAsset*>(asset.get());
    if (!model || !model->IsLoaded())
        return 0;
    return clipStore.GetIndexIfPresent(clipGuid);
}

uint32_t ResolveAnimatorClipIndex(const Components::Animator& animator,
                                  const GUID& fallbackModel, AssetManager& assetManager, bool* sourcePending)
{
    return ResolveAnimatorClipIndexImpl(animator, fallbackModel, assetManager, sourcePending);
}

void BootstrapSceneAnimators(ECS::World& world)
{
    auto& assetManager = EngineCore::GetInstance().GetAssetManager();
    BootstrapSceneAnimators(world, assetManager);
}

void StartAnimatorFromSource(ECS::World& world, AssetManager& assetManager, Animator& animator,
                             const std::vector<ECS::EntityHandle>& subtree)
{
    const ResolvedAnimatorPlayback playback =
        ResolveAnimatorPlayback(animator, FindModelGuidInSubtree(world, subtree), assetManager);
    if (playback.ClipIndex == 0)
    {
        ApplyRestToSubtree(world, subtree);
        // A source still loading starts once it lands: ProcessClipAnimatorCommands retries it
        // every frame. A container that never leaves loading (a stalled decode) is retried for as
        // long as playback lasts: each frame, each such Animator collects its subtree, resolves
        // its source again (asset-manager and registry lookups) and rewrites the subtree's rest
        // pose.
        if (playback.SourcePending)
        {
            animator.pendingCommand = AnimatorPlaybackCommand::Autoplay;
            return;
        }
        // Nothing to wait for: the start ends here, so it is said once.
        if (animator.source == AnimatorPlaybackSource::Library ||
            animator.source == AnimatorPlaybackSource::Controller)
            WarnNothingToPlay(animator, playback);
        else
            Logger::Log::Warning("Animator: could not resolve clip (clipGuid={}, animation='{}', source: {})",
                                 animator.clipGuid.ToGuid().ToString(), animator.AssignedAnimation(),
                                 DescribeClipSource(animator));
        return;
    }

    const auto clip = ClipStore::Instance().Get(playback.ClipIndex);
    if (!clip || clip->GetDuration() <= 0.0f)
    {
        Logger::Log::Warning("Animator: resolved clip invalid (idx={} duration={})", playback.ClipIndex,
                             clip ? clip->GetDuration() : -1.0f);
        ApplyRestToSubtree(world, subtree);
        return;
    }

    Logger::Log::Info("Animator: playing clip index {} ({:.2f} s) from its {} source", playback.ClipIndex,
                      clip->GetDuration(), DescribeSourceKind(animator.source));
    const bool section = animator.sectionEndSeconds > animator.sectionStartSeconds;
    ApplyPlayStateToSubtree(world, subtree, playback.ClipIndex, playback.TimeSeconds, playback.SpeedScale,
                            playback.Loop, animator.blendSeconds, section, animator.sectionStartSeconds,
                            animator.sectionEndSeconds);
}

void BootstrapSceneAnimators(ECS::World& world, AssetManager& assetManager)
{
    world.Query<ECS::Write<Animator>>().Each([&](ECS::EntityHandle rootEntity, Animator& animator) {
        if (!animator.active || !animator.autoPlayOnEnterPlayMode ||
            animator.pendingCommand != AnimatorPlaybackCommand::None)
            return;
        // Timeline and graph sources start in their own systems.
        if (animator.source == AnimatorPlaybackSource::Timeline ||
            animator.source == AnimatorPlaybackSource::Graph)
            return;
        std::vector<ECS::EntityHandle> subtree;
        CollectEntitiesInSubtree(world, rootEntity, subtree);
        StartAnimatorFromSource(world, assetManager, animator, subtree);
    });
}

void ProcessClipAnimatorCommands(ECS::World& world)
{
    auto& assetManager = EngineCore::GetInstance().GetAssetManager();
    ProcessClipAnimatorCommands(world, assetManager);
}

void ProcessClipAnimatorCommands(ECS::World& world, AssetManager& assetManager)
{
    world.Query<ECS::Write<Animator>>().Each([&](ECS::EntityHandle rootEntity, Animator& animator) {
        if (!animator.active || animator.pendingCommand == AnimatorPlaybackCommand::None)
            return;

        if (animator.source == AnimatorPlaybackSource::Timeline ||
            animator.source == AnimatorPlaybackSource::Graph)
            return;

        if (animator.source != AnimatorPlaybackSource::Clip)
        {
            // A library or controller start waiting for its clip's container: try again.
            if (animator.pendingCommand == AnimatorPlaybackCommand::Autoplay &&
                (animator.source == AnimatorPlaybackSource::Library ||
                 animator.source == AnimatorPlaybackSource::Controller))
            {
                animator.pendingCommand = AnimatorPlaybackCommand::None;
                std::vector<ECS::EntityHandle> subtree;
                CollectEntitiesInSubtree(world, rootEntity, subtree);
                StartAnimatorFromSource(world, assetManager, animator, subtree);
                return;
            }
            Logger::Log::Warning(
                "ProcessClipAnimatorCommands: dropping pendingCommand on non-Clip Animator (source={})",
                static_cast<int>(animator.source));
            animator.pendingCommand = AnimatorPlaybackCommand::None;
            return;
        }

        std::vector<ECS::EntityHandle> subtree;
        CollectEntitiesInSubtree(world, rootEntity, subtree);

        const AnimatorPlaybackCommand command = animator.pendingCommand;
        if (command == AnimatorPlaybackCommand::Stop)
        {
            ApplyRestToSubtree(world, subtree);
            animator.pendingCommand = AnimatorPlaybackCommand::None;
            return;
        }

        if (command == AnimatorPlaybackCommand::Pause || command == AnimatorPlaybackCommand::Seek)
        {
            ApplyClipCommandToSubtree(world, animator, subtree, 0);
            animator.pendingCommand = AnimatorPlaybackCommand::None;
            return;
        }

        bool sourcePending = false;
        const uint32_t clipIndex = ResolveAnimatorClipIndex(animator, FindModelGuidInSubtree(world, subtree), assetManager, &sourcePending);
        if (clipIndex == 0)
        {
            if (sourcePending) return;
            Logger::Log::Warning(
                "ProcessClipAnimatorCommands: could not resolve clip (clipGuid={}, animation='{}', source: {})",
                animator.clipGuid.ToGuid().ToString(),
                animator.AssignedAnimation(),
                DescribeClipSource(animator));
            animator.pendingCommand = AnimatorPlaybackCommand::None;
            return;
        }

        if (ApplyClipCommandToSubtree(world, animator, subtree, clipIndex) == 0)
        {
            Logger::Log::Warning(
                "ProcessClipAnimatorCommands: clip {} resolved but no entity in the subtree carries "
                "AnimatorRef+SkeletonRef — the model likely loaded unskinned (bind pose)",
                clipIndex);
        }
        animator.pendingCommand = AnimatorPlaybackCommand::None;
    });
}

} // namespace GameEngine::Engine::Renderer
