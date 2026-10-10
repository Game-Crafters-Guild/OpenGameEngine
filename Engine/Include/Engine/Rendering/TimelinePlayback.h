#pragma once

#include "Animation/AnimationTimeline.h"
#include "AssetCore/GUID.h"
#include "Audio/AudioHandles.h"
#include "Components/Animation/TimelinePlaybackState.h"
#include "ECS/Entity.h"
#include "Types/Types.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
class Asset;
class AssetManager;
class TimelineAsset;
namespace Audio
{
class AudioSystem;
}
namespace ECS
{
class World;
}
} // namespace GameEngine

namespace GameEngine::Engine::Renderer
{

uint32_t ResolveClipIndex(const GUID& clipGuid, AssetManager& assetManager);

SharedPtr<Asset> TryAcquireTimelineAsset(AssetManager& assetManager, const GUID& timelineGuid);

bool LoadTimelineFromAsset(const TimelineAsset& timelineAsset, Animation::Timeline& outTimeline, std::string* outError = nullptr);

void CollectEntitiesInSubtree(ECS::World& world, ECS::EntityHandle root, std::vector<ECS::EntityHandle>& outSubtree);

ECS::EntityHandle ResolveEntityByTargetPath(ECS::World& world,
                                            ECS::EntityHandle rootEntity,
                                            std::string_view targetPath,
                                            const std::vector<ECS::EntityHandle>& subtree,
                                            std::string_view rootNodeOverride = {});

struct ResolvedTimelineAnimationClip
{
    uint32_t clipIndex = 0;
    float32 localTimeSeconds = 0.0f;
    float32 weight = 1.0f;
};

bool PickDominantAnimationClip(const Animation::TimelineEvaluationResult& evaluation,
                               AssetManager& assetManager,
                               ResolvedTimelineAnimationClip& outClip);

struct TimelinePlaybackApplyContext
{
    ECS::World* world = nullptr;
    ECS::EntityHandle rootEntity;
    AssetManager* assetManager = nullptr;
    Audio::AudioSystem* audioSystem = nullptr;
    const std::vector<ECS::EntityHandle>* subtree = nullptr;
    std::string_view rootNodePath;
    float32 speedScale = 1.0f;
    bool loop = true;
    uint32_t audioMaxPolyphony = 32;
    std::vector<Audio::AudioEmitterHandle>* activeVoices = nullptr;
};

void ApplyTimelineEvaluation(const Animation::TimelineEvaluationResult& evaluation,
                             const TimelinePlaybackApplyContext& context);

using TimelineMethodCallback = std::function<void(ECS::World& world,
                                                  ECS::EntityHandle rootEntity,
                                                  const Animation::TimelineMethodEvent& event)>;

void RegisterTimelineMethodCallback(std::string methodName, TimelineMethodCallback callback);
void UnregisterTimelineMethodCallback(std::string_view methodName);
void ClearTimelineMethodCallbacks();
void DispatchTimelineMethodEvents(ECS::World& world,
                                  ECS::EntityHandle rootEntity,
                                  const std::vector<Animation::TimelineMethodEvent>& events);

using ManagedTimelineMethodListener = void (*)(uint64_t entityId,
                                                const char* methodNameUtf8,
                                                const char* argumentsUtf8);

void SetManagedTimelineMethodListener(ManagedTimelineMethodListener listener);

void EnsureTimelinePlaybackState(ECS::World& world, ECS::EntityHandle entity, const Components::TimelinePlaybackState& initial);

} // namespace GameEngine::Engine::Renderer
