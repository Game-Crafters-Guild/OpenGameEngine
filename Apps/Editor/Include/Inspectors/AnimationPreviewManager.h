#pragma once

#include "AssetCore/GUID.h"
#include "ECS/Entity.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::ECS { class World; }

namespace GameEngine::Editor
{

// Tracks per-entity animation preview state and manages the AnimationSystem
// enable/disable lifecycle for edit-mode preview.
class AnimationPreviewManager
{
public:
    struct PreviewState
    {
        uint32_t clipIndex = 0;
        GUID clipGuid;
        float duration = 0.0f;
        float speed = 1.0f;
        std::vector<ECS::EntityHandle> subtree;
    };

    // Enable preview for an entity using the given clip. Resolves the GUID
    // through the ClipStore (loading the asset on demand) and assigns the
    // resulting runtime clip to AnimatorRef components in the subtree.
    static bool EnablePreview(ECS::World& world, ECS::EntityHandle rootEntity,
                              const GUID& clipGuid);

    // Disable preview for an entity. Resets AnimatorRef state on the subtree.
    static void DisablePreview(ECS::World& world, ECS::EntityHandle rootEntity);

    // Switch the active clip while preview is already on.
    static bool SwitchClip(ECS::World& world, ECS::EntityHandle rootEntity,
                           const GUID& clipGuid);

    // Apply playback changes to the cached subtree.
    static void SetPaused(ECS::World& world, ECS::EntityHandle rootEntity, bool paused);
    static void SetSpeed(ECS::World& world, ECS::EntityHandle rootEntity, float speed);
    static void SetTime(ECS::World& world, ECS::EntityHandle rootEntity, float time);

    static bool IsPreviewActive(ECS::EntityHandle rootEntity);
    static const PreviewState* GetState(ECS::EntityHandle rootEntity);

    // Disable all active previews (called when entering play mode).
    static void DisableAllPreviews(ECS::World& world);

    // True if any preview is active (for SimulationRefresh gating).
    static bool AnyPreviewActive();

    // Retry previews whose model wasn't resident when EnablePreview ran. Loads
    // are non-blocking, so the first EnablePreview call may register a "pending"
    // preview; this drives the per-frame retry until the model loads. Cheap
    // (early-outs) when nothing is pending. Call once per frame for the active world.
    static void TickPending(ECS::World& world);

private:
    enum class EnableResult
    {
        Enabled, // Clip resolved and preview applied.
        Pending, // Model isn't resident yet; load was kicked, retry later.
        Failed,  // No model/clip available — genuine failure.
    };

    // Resolve the clip for an entity and apply the preview on success. Shared by
    // EnablePreview and TickPending. logOnFailure controls whether the genuine
    // "could not resolve" warning is emitted (suppressed on the first attempt so
    // a still-loading model doesn't spam the log).
    static EnableResult TryResolveAndEnable(ECS::World& world, ECS::EntityHandle rootEntity,
                                            const GUID& clipGuid, bool logOnFailure);
};

} // namespace GameEngine::Editor
