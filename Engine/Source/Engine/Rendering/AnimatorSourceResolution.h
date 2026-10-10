#pragma once

#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <cstdint>
#include <string>

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

// What an Animator's source resolves to at the start of playback.
struct ResolvedAnimatorPlayback
{
    // A clip's container is still loading (ResolvePlaybackClipIndex); the start is retried.
    bool SourcePending = false;
    uint32_t ClipIndex = 0;
    float32 TimeSeconds = 0.0f;
    float32 SpeedScale = 1.0f;
    bool Loop = true;
    // For the warning when nothing plays: the entry of the first library the source reached, and
    // the first clip it named that resolved to nothing with no container loading.
    std::string LibraryEntry;
    GUID UnresolvedClip;
};

// The clip, time, speed and looping a Clip-, library- or controller-sourced Animator starts with
// (a library entry or a controller's entry state, followed through nested libraries, controllers
// and timelines). Never waits on a clip's container. `subtreeModel` is the first model in the
// Animator's subtree.
ResolvedAnimatorPlayback ResolveAnimatorPlayback(const Components::Animator& animator, const GUID& subtreeModel,
                                                 AssetManager& assetManager);

} // namespace GameEngine::Engine::Renderer
