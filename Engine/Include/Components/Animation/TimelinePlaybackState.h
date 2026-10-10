#pragma once

#include "Types/Types.h"

namespace GameEngine::Components
{

// Runtime clock for Animator timeline playback. Added on the same entity as
// Animator when timeline source is active. Authoring lives on Animator
// (timelineGuid, loop, speedScale, commands); this component holds per-play
// state consumed by TimelinePlaybackSystem.
// @ge-no-add  data helper, not user-addable in the editor
// [DoNotSerialize] — per-play runtime clock created by TimelinePlaybackSystem; authoring lives on Animator.
struct TimelinePlaybackState
{
    float32 timeSeconds = 0.0f;
    float32 previousTimeSeconds = 0.0f;
    bool playing = false;
    bool paused = false;
};

} // namespace GameEngine::Components
