#include "ECSModules/Rendering/Systems/AmbientLightSystem.h"

#include "Components/Rendering/AmbientLight.h"
#include "Components/Rendering/LightPhotometry.h" // kReferenceWhiteNits (the shared 203-nit anchor)
#include "ECS/Components.h"
#include "ECS/Query.h"
#include "Engine/Rendering/IEnvironmentSource.h" // complete type for EnsureFeature<ImageBasedLightingFeature>'s make_unique
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cmath>

namespace GameEngine { namespace Engine::Renderer {

AmbientFloorData AmbientLightSystem::ResolveActiveFloor(ECS::World& world)
{
    Components::AmbientLight comp{};
    bool found = false;
    int count = 0;

    world.Query<ECS::Read<Components::AmbientLight>>()
        .Each([&](ECS::EntityHandle /*e*/, const Components::AmbientLight& a) {
            ++count;
            if (found)
                return;
            comp = a;
            found = true;
        });

    if (count > 1)
    {
        static bool warnedMultiple = false;
        if (!warnedMultiple)
        {
            LOG_WARNING("Multiple AmbientLight components found ({}). Only the first will be used.", count);
            warnedMultiple = true;
        }
    }

    AmbientFloorData floor{}; // default: off (zero floor) -> byte-identical with no component
    if (!found)
        return floor;

    // Color x Intensity / kReferenceWhiteNits puts the floor on the SAME scene-linear anchor (203
    // nits) as the physical/gradient sky, the physical-unit lights, and camera auto-exposure. The
    // CPU premultiplies so the shader adds the resolved colors verbatim. Serialized data is not
    // trusted on the way to the UBO: a non-finite or negative intensity collapses the scale to 0
    // and channels clamp at 0, so a hand-edited scene can never upload NaN/negative irradiance
    // (0 x inf = NaN would otherwise reach every lit pixel).
    float scale = comp.Intensity / Components::kReferenceWhiteNits;
    if (!std::isfinite(scale) || scale < 0.0f)
        scale = 0.0f;
    floor.AffectSpecular = comp.AffectSpecular ? 1u : 0u;
    if (comp.Mode == Components::AmbientLightMode::Flat)
    {
        floor.Mode = 1u;
        for (int i = 0; i < 3; ++i)
        {
            floor.Sky[i] = std::max(0.0f, comp.Color[i] * scale);
            // Pack the flat color into every slot so the value is defined regardless of which slot a
            // future consumer reads; the shader's flat branch reads Sky only.
            floor.Equator[i] = floor.Sky[i];
            floor.Ground[i] = floor.Sky[i];
        }
    }
    else
    {
        floor.Mode = 2u;
        for (int i = 0; i < 3; ++i)
        {
            floor.Sky[i] = std::max(0.0f, comp.SkyColor[i] * scale);
            floor.Equator[i] = std::max(0.0f, comp.EquatorColor[i] * scale);
            floor.Ground[i] = std::max(0.0f, comp.GroundColor[i] * scale);
        }
    }
    return floor;
}

void AmbientLightSystem::Update(ECS::World& world, float32 /*deltaTime*/)
{
    RenderServices* rs = m_RenderServices;
    if (!rs)
        return;

    // EnsureFeature mirrors SkyEnvironmentSystem: the IBL feature is engine-shared and idempotently
    // created here or by IBLGenNode (whichever runs first). Setting the floor is pure CPU state read
    // back by UploadEnvData during world-pass resource resolution, so it needs no initialized device.
    auto& ibl = rs->EnsureFeature<ImageBasedLightingFeature>();
    ibl.SetAmbientFloor(ResolveActiveFloor(world));
}

} } // namespace GameEngine::Engine::Renderer
