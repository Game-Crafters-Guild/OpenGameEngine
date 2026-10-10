#pragma once

#include "AssetCore/AssetTypes.h"
#include "Components/AssetRef.h"
#include "Types/Color.h"

namespace GameEngine {
namespace Components {

// Attaches a lens flare to an entity (typically a light or the sun). Positional
// flares use the entity's world position; SunMode projects a directional light
// as an infinite source. The referenced LensFlareDefinition supplies the
// elements, atlas, fades, and dynamic boost.
struct LensFlareSource {
    AssetRef<AssetType::LensFlareDefinition> Flare;

    // When attached to (or placed directly on) a directional light, project
    // the flare from the light direction instead of its world position.
    bool SunMode = false;

    // Per-instance multipliers on top of the definition's globals.
    float Intensity = 1.0f;
    float Scale     = 1.0f;

    // Per-instance tint multiplied into every element's color.
    ColorLinear Tint{1.0f, 1.0f, 1.0f, 1.0f};

    // Fade the flare when scene geometry occludes the source (depth-probe).
    bool Occlude = true;

    // When > 0, replaces the definition's max distance for this source (the
    // range the distance fade/scale/cull works over).
    float MaxDistanceOverride = 0.0f;
};

} // namespace Components
} // namespace GameEngine
