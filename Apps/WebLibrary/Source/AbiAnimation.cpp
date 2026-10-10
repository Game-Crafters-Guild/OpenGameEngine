// A loaded model's animation clips as a page plays them: ge_animation_clips, ge_animation_play
// and ge_animation_pause (Apps/WebLibrary/ts/src/abi.ts). The Animator component is not in the
// reflected registry (its asset references are templated fields), so these calls stamp its
// playback command for the page; the next tick's animation systems apply it
// (ProcessClipAnimatorCommands).

#include "AbiEntities.h"
#include "AbiErrors.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Animation/Animator.h"
#include "Components/HierarchyQueries.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include <emscripten/emscripten.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::WebLibrary
{

namespace
{

// The model whose clips the entity's Animator plays: the one the first MeshRenderer at or under
// the entity draws, as the Animator's own clip resolution finds it.
const ModelAsset* FindAnimatedModel(ECS::World& world, ECS::EntityHandle entity)
{
    std::vector<ECS::EntityHandle> subtree = Components::DescendantsOf(world, entity);
    subtree.insert(subtree.begin(), entity);
    AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
    for (const ECS::EntityHandle e : subtree)
    {
        const auto* renderer = world.GetComponent<Components::MeshRenderer>(e);
        if (!renderer || renderer->modelAssetGuid.IsNull())
            continue;
        const SharedPtr<Asset> asset = assets.GetAsset(renderer->modelAssetGuid.ToGuid());
        if (asset && asset->GetType() == AssetType::Model && asset->IsLoaded())
            return static_cast<const ModelAsset*>(asset.get());
    }
    return nullptr;
}

// The clip names of the model the entity's Animator plays, with the entity and its world; null,
// with the last error naming `call`, when the entity is not an animated model.
const Vector<String>* AnimatedModelClips(std::string_view call, uint32_t entityId, ECS::World*& outWorld,
                                         ECS::EntityHandle& outEntity)
{
    outWorld = WorldForCall(call);
    if (!outWorld || !ResolveEntity(*outWorld, entityId, call, outEntity))
        return nullptr;
    const bool hasAnimator = outWorld->HasComponent<Components::Animator>(outEntity);
    const ModelAsset* model = hasAnimator ? FindAnimatedModel(*outWorld, outEntity) : nullptr;
    if (!model || model->GetAnimationNames().empty())
    {
        SetLastError("{}: entity {} is not an animated model; call it on the entity scene.load returned for a "
                     "skinned model with animation clips.", call, entityId);
        return nullptr;
    }
    return &model->GetAnimationNames();
}

std::string JoinClipNames(const Vector<String>& clips)
{
    std::string joined;
    for (const String& clip : clips)
        joined += (joined.empty() ? "'" : ", '") + std::string(clip) + "'";
    return joined;
}

} // namespace

} // namespace GameEngine::WebLibrary

using namespace GameEngine;
using namespace GameEngine::WebLibrary;

extern "C"
{

/// The names of the clips the animated model `entityId` plays, as a JSON array of strings in the
/// model's order; null on failure. The text stays valid until the next call that returns a string.
EMSCRIPTEN_KEEPALIVE const char* ge_animation_clips(uint32_t entityId)
{
    const AbiCallScope scope("ge_animation_clips");
    if (scope.Refused())
        return nullptr;
    ECS::World* world = nullptr;
    ECS::EntityHandle entity;
    const Vector<String>* clips = AnimatedModelClips("ge_animation_clips", entityId, world, entity);
    if (!clips)
        return nullptr;
    nlohmann::json names = nlohmann::json::array();
    for (const String& clip : *clips)
        names.push_back(std::string(clip));
    return ReturnString(names.dump());
}

/// Plays the clip `clipName` (UTF-8) on the animated model `entityId`, looping, at `speed` times its
/// authored rate. The clip already playing keeps its time, so a call for it changes the speed or
/// resumes it after ge_animation_pause; another clip starts from its beginning. The next tick
/// applies it.
EMSCRIPTEN_KEEPALIVE int32_t ge_animation_play(uint32_t entityId, const char* clipName, float speed)
{
    const AbiCallScope scope("ge_animation_play");
    if (scope.Refused())
        return kFailed;
    ECS::World* world = nullptr;
    ECS::EntityHandle entity;
    const Vector<String>* clips = AnimatedModelClips("ge_animation_play", entityId, world, entity);
    if (!clips)
        return kFailed;
    const std::string_view name = clipName ? clipName : "";
    if (std::find(clips->begin(), clips->end(), name) == clips->end())
    {
        SetLastError("ge_animation_play: the model has no clip '{}'; its clips are {}.", name, JoinClipNames(*clips));
        return kFailed;
    }
    if (!std::isfinite(speed) || speed < 0.0f)
    {
        SetLastError("ge_animation_play: speed {} is not a rate; pass 0 or more (1 plays the clip as authored).", speed);
        return kFailed;
    }
    auto* animator = world->GetComponentForWrite<Components::Animator>(entity);
    animator->PlayState(name);
    animator->speedScale = speed;
    animator->loop = true;
    return kOk;
}

/// Holds the animated model `entityId` at its current pose; ge_animation_play resumes it. The next
/// tick applies it.
EMSCRIPTEN_KEEPALIVE int32_t ge_animation_pause(uint32_t entityId)
{
    const AbiCallScope scope("ge_animation_pause");
    if (scope.Refused())
        return kFailed;
    ECS::World* world = nullptr;
    ECS::EntityHandle entity;
    if (!AnimatedModelClips("ge_animation_pause", entityId, world, entity))
        return kFailed;
    world->GetComponentForWrite<Components::Animator>(entity)->Pause();
    return kOk;
}

} // extern "C"
