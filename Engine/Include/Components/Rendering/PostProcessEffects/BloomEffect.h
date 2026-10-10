#pragma once

#include "Components/AssetRef.h"
#include "Types/Types.h"

namespace GameEngine {
namespace Components {

// Per-volume bloom controls. Attach to the same entity as a PostProcessVolume.
struct BloomEffect {
    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    float32 Threshold{1.0f};
    float32 Knee{0.1f};
    // Stabilize isolated hot highlights before filtering.
    bool AntiFlicker{true};
    // Additive gain of the thresholded highlight pyramid.
    float32 Intensity{1.0f};
    // Fraction of scene light redistributed by a separate, threshold-free pyramid.
    // Zero disables its additional work. Independent of additive Intensity.
    float32 ScatteringAmount{0.0f};
    // Tint applies only to additive highlights, never to redistributed scene light.
    float32 Tint[3]{1.0f, 1.0f, 1.0f};
    // Resolution-independent extent of the bloom veil. Fractional values
    // continuously scale the reconstruction filter between pyramid levels.
    float32 Radius{2.5f};
    // Maximum number of independently weighted bloom pyramid levels, i.e. the
    // quality/perf ceiling on pyramid depth. Radius selects the active count
    // (and the fractional reconstruction scale) up to this ceiling for each
    // render height. Defaults to the full pyramid so Radius stays responsive
    // across its whole range; lower it to cap depth for perf. Levels beyond the
    // radius-requested count are never allocated, so a high ceiling costs
    // nothing until Radius asks for those levels.
    int32 Octaves{8};
    // Bias between narrow and broad pyramid levels.
    float32 Scatter{0.5f};
    // Adds a broad bloom source from scene depth so distant geometry and the
    // sky can form an atmospheric veil independently of pixel brightness.
    bool DepthVeilEnabled{false};
    float32 DepthVeilIntensity{1.0f};
    // Linear camera-space distances in world units.
    float32 DepthVeilStart{25.0f};
    float32 DepthVeilEnd{500.0f};
    float32 DepthVeilTint[3]{1.0f, 1.0f, 1.0f};
    bool LensDirtEnabled{false};
    bool LensDirtVignette{false};
    float32 LensDirtVignetteIntensity{1.0f};
    float32 LensDirtVignetteRadius{0.25f};
    float32 LensDirtVignetteSmoothness{0.2f};
    bool LensDirtVignetteRounded{false};
    float32 LensDirtVignetteColor[3]{1.0f, 1.0f, 1.0f};
    // Exponential intensity control, constrained to the supported editor/runtime range.
    float32 LensDirtIntensity{0.0f};
    float32 LensDirtScatter{0.5f};
    // Empty selects the built-in Sonic Ether dirt texture; an authored asset
    // overrides it through the renderer texture service.
    TextureRef LensDirtTexture{};
};

} // namespace Components
} // namespace GameEngine
