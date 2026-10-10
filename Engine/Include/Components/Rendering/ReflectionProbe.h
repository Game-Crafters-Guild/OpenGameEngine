#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

enum class ReflectionProbeUpdateMode : uint32 {
    Once = 0,
    Realtime = 1,
};

struct ReflectionProbe {
    // Local influence volume. Like PostProcessVolume, the entity transform owns
    // the probe range: a unit box/sphere is scaled and oriented by WorldTransform.
    float32 BlendDistance = 2.0f;
    uint32 Priority = 0;

    // Godot-style local probe box. When disabled, the same transform range is
    // treated as a sphere/ellipsoid influence without parallax box projection.
    bool BoxProjection = true;
    float32 OriginOffsetX = 0.0f;
    float32 OriginOffsetY = 0.0f;
    float32 OriginOffsetZ = 0.0f;

    // Dynamic multiplier applied through EnvData, so changing it does not force a
    // cubemap rebake.
    float32 Intensity = 1.0f;
    float32 ExposureEV = 0.0f;
    float32 RotationDegrees = 0.0f;
    float32 IblLowerHemisphereDarkness = 0.3f;

    // When true the six-face capture also gathers environment reflections
    // (the sky/HDRI and any lower-priority probe). Default false: the capture
    // sees direct light, emission and the diffuse GI field only. A probe that
    // sampled its OWN environment cube while baking would feed its output back
    // into its next bake; in a closed, high-albedo room with no dominant fixed
    // sky that loop diverges to Inf and the whole view tonemaps to black, so
    // the safe default breaks the recursion. Turn it on only for an open scene
    // where the environment is dominated by a fixed sky.
    bool CaptureEnvironment = false;

    // Scene capture behavior. Probes use the active sky/HDRI as background
    // when present, then capture nearby scene content.
    uint32 CaptureResolution = 256;
    ReflectionProbeUpdateMode UpdateMode = ReflectionProbeUpdateMode::Realtime;
    // Shortest time between realtime captures while the probe's world
    // changes (SceneProbeWorldEpochs: lights, renderables, dynamic depth,
    // material edits; not shader-time animation, video textures or
    // particles); a world at rest is not recaptured. 0 captures every frame:
    // a full six-face bake per frame, no time slicing and no rebake throttle.
    float32 RealtimeUpdateInterval = 0.5f; // @ge-range 0
    float32 MaxDistance = 0.0f;
    uint32 CullMask = 0xFFFFFFFFu;
};

} // namespace Components
} // namespace GameEngine
