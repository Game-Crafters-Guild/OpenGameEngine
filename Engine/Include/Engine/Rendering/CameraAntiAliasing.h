#pragma once

// The camera -> view anti-aliasing policy, shared by every host that renders a
// scene Camera: the editor Game View and the Player. Both resolve the same
// per-camera overrides against the same engine defaults and publish the same
// per-view state, so the policy lives here rather than being restated (and
// drifting) in each host.

#include "Engine/Rendering/AntiAliasing.h"
#include "Rendering/CameraTypes.h"

namespace GameEngine::Engine::Renderer
{

class RenderServices;
struct Camera;
struct CameraAspectResolution;

// Resolve `camera`'s AA overrides against the engine defaults, publish the
// resulting per-view AA state onto `viewId`, and return the effective mode +
// sample count (the count the view's color target must be created with).
//
// Letterboxed views are excluded from the jittered/gated modes and fall back to
// unjittered MSAA/off semantics: the resolve nodes assume a full-extent raster
// and pass a letterboxed view through unresolved, so jittering one would ship
// permanent shimmer.
ResolvedAntiAliasing ApplyCameraAntiAliasing(RenderServices& rs,
                                             ::GameEngine::Rendering::ViewId viewId,
                                             const Camera& camera,
                                             const CameraAspectResolution& aspect);

} // namespace GameEngine::Engine::Renderer
