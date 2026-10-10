#include "Engine/Rendering/PlanetCameraFraming.h"

#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <algorithm>

namespace GameEngine::Engine::Renderer
{

float ExpandFarClipForSphericalTerrain(ECS::World& world, float userFarClip)
{
    // Frame the whole globe from a near-orbit vantage: a sphere of radius R viewed from
    // ~2-3 R out needs the far plane past ~4 R. Relief adds to the silhouette.
    constexpr float kPlanetFarClipRadiusMultiple = 4.0f;

    float requiredFar = userFarClip;
    world.Query<ECS::Read<Components::Terrain>>().Each(
        [&](ECS::EntityHandle entity, const Components::Terrain& terrain)
        {
            if (terrain.Domain != Components::TerrainDomain::Spherical)
                return;
            // Relief adds to the silhouette; it lives in the companion TerrainPlanetRelief
            // component now (default amplitude when absent).
            float reliefAmplitude = Components::TerrainPlanetRelief{}.Amplitude;
            if (const auto* relief = world.GetComponent<Components::TerrainPlanetRelief>(entity))
                reliefAmplitude = relief->Amplitude;
            const float bound =
                (terrain.PlanetRadius + reliefAmplitude) * kPlanetFarClipRadiusMultiple;
            requiredFar = std::max(requiredFar, bound);
        });
    return requiredFar;
}

} // namespace GameEngine::Engine::Renderer
