#pragma once

#include <cstdint>

namespace GameEngine
{
namespace Engine::Renderer
{

// Derived, GPU-bound ambient irradiance floor resolved from the AmbientLight component
// (AmbientLightSystem::ResolveActiveFloor). Colors are scene-linear, ALREADY premultiplied by
// Intensity / kReferenceWhiteNits, so the shader adds them verbatim. Uploaded into the EnvData UBO
// (ibl.glsl GE_AmbientFloor). The default (Mode == 0, colors 0) makes the shader floor exactly
// vec3(0.0) — a scene with no AmbientLight is byte-identical to a build without the feature.
struct AmbientFloorData
{
    uint32_t Mode = 0;           // 0 = off, 1 = flat (Sky slot only), 2 = gradient (Sky/Equator/Ground)
    uint32_t AffectSpecular = 0; // 0 = diffuse-only, 1 = also fill the primary specular lobe
    float Sky[3] = {0.0f, 0.0f, 0.0f};
    float Equator[3] = {0.0f, 0.0f, 0.0f};
    float Ground[3] = {0.0f, 0.0f, 0.0f};
};

} // namespace Engine::Renderer
} // namespace GameEngine
