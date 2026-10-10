#pragma once

#include "AssetCore/GUID.h"
#include "ECS/ECS.h"

#include <cstdint>
#include <vector>

namespace GameEngine::ECS
{
class World;
}
namespace GameEngine
{
class AssetManager;
}
namespace GameEngine::Components
{
struct Animator;
}

namespace GameEngine::Engine::Renderer
{

// Resolve an explicit clip/source before the optional target-model convenience
// selection. Editor and runtime entry points share this exact source policy.
// Zero with sourcePending=true means its selected source model is still loading;
// zero with sourcePending=false means resolution failed.
uint32_t ResolveAnimatorClipIndex(const Components::Animator& animator,
                                 const GUID& fallbackModel, AssetManager& assetManager,
                                 bool* sourcePending = nullptr);

// The ClipStore index of the clip `clipGuid` names, for playback that must never wait on its
// container. The one rule every playback source follows (clip, library, controller):
//   - a clip already in the ClipStore resolves at once;
//   - a clip asset of its own (a registered GUID) resolves through ClipStore (a small file, no
//     import);
//   - an embedded clip has no registry record (AssetCore/SubassetDeriveKeys.h): its container
//     registers it into the ClipStore as it loads. The container is, in order, `sourceModel`
//     (the durable source pair, when the caller has one), the model the registry's journal
//     names (AssetRegistry::FindSubassetContainer), then `subtreeModel`. While it is not
//     resident it is requested and the answer is pending: zero with *sourcePending true, and the
//     caller tries again on a later frame.
// Zero with *sourcePending false: no clip to play.
uint32_t ResolvePlaybackClipIndex(const GUID& clipGuid,
                                  const GUID& sourceModel,
                                  const GUID& subtreeModel,
                                  AssetManager& assetManager,
                                  bool* sourcePending);

// Starts a Clip-, library- or controller-sourced Animator: resolves its clip
// (ResolveAnimatorPlayback) and puts the AnimatorRef components of `subtree` (the Animator's
// entity and its descendants) into play state, or at rest. The one start every host uses: the
// editor's play mode and the runtime hosts (BootstrapSceneAnimators). It ends one of three ways:
//   - the clip plays;
//   - a clip's container is still loading: the subtree rests, Autoplay is armed, and
//     ProcessClipAnimatorCommands starts it once the container lands;
//   - nothing to play: said once in the log, and the subtree is left at rest.
void StartAnimatorFromSource(ECS::World& world, AssetManager& assetManager, Components::Animator& animator,
                             const std::vector<ECS::EntityHandle>& subtree);

// Wire scene-authored Animator components to AnimatorRef clip playback.
// Player and other runtime hosts call this after scene load so skinned meshes animate outside
// the editor; every Clip, library and controller source starts through StartAnimatorFromSource.
void BootstrapSceneAnimators(ECS::World& world);
// Tools/runtime hosts with explicit asset ownership use the same hydration path.
void BootstrapSceneAnimators(ECS::World& world, AssetManager& assetManager);

// Consume Animator.pendingCommand for Clip source and apply it to AnimatorRef
// in the entity subtree (PlayState / CrossFadeSeconds / Pause / Stop / Seek), and retry the
// pending start of a library or controller source (StartAnimatorFromSource).
// Timeline source is left for TimelinePlaybackSystem. Graph source is left
// for AnimationGraphSystem. Called each animation tick.
void ProcessClipAnimatorCommands(ECS::World& world);
void ProcessClipAnimatorCommands(ECS::World& world, AssetManager& assetManager);

} // namespace GameEngine::Engine::Renderer
