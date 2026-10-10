#pragma once

#include "ECS/Entity.h"
#include "ECS/Systems.h"
#include "Engine/Rendering/AmbientFloorData.h"
#include "Types/Types.h"

namespace GameEngine { namespace Engine::Renderer {

class RenderServices;

// Extracts the (0 or 1) opt-in AmbientLight component each frame and pushes the resolved,
// scene-linear irradiance floor into ImageBasedLightingFeature, which writes it into the EnvData
// UBO (uploaded during world-pass resource resolution, mirroring the multiplicative AmbientTint).
// A sibling of SkyEnvironmentSystem: same Camera phase, but independent of the sky source — the
// floor lights a scene with no sky at all. With no AmbientLight present the floor is cleared to
// zero, so the frame is byte-identical to a build without this feature.
class AmbientLightSystem : public ECS::ISystem {
public:
    explicit AmbientLightSystem(RenderServices* renderServices)
        : m_RenderServices(renderServices)
    {
    }

    const char* GetName() const override { return "AmbientLightSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

    // Pure extraction: scan the world for the first enabled AmbientLight (first-wins) and resolve
    // it into the scene-linear GPU floor (Color x Intensity / kReferenceWhiteNits). Returns the
    // default (Mode 0, zero) when none is present. Split out so it is testable without a device.
    static AmbientFloorData ResolveActiveFloor(ECS::World& world);

private:
    RenderServices* m_RenderServices = nullptr;
};

} } // namespace GameEngine::Engine::Renderer
