#pragma once
#include "Components/Rendering/Ocean.h"
#include "Types/Types.h"
#include <cstddef>
namespace GameEngine::Ocean
{
// A surface preset supplies material values; world/simulation fields are not overridden.
// std430 mirror in ocean_material_profiles.glsl. Eight-point footprints match water bodies.
struct alignas(16) OceanWaterMaterialGPU
{
    float Bounds[4]{};
    float Points[8][4]{};
    float PointCount = 0.0f;
    float _MetaPad[3]{}; // keeps the next vec4 on its 16-byte std430 boundary
    float Deep[4]{}, Diffuse[4]{}, Grazing[4]{}, Shadow[4]{}, Shallow[4]{}, Subsurface[4]{}, Foam[4]{},
        Fog[4]{};
    float Surface[4]{};                    // normal strength, normal scale, roughness, specular
    float FoamDetail[4]{};                 // amount, scale, feather, relief
    float Bubbles[4]{};                    // coverage, parallax, foam roughness, fresnel power
    float Optics[4]{};                     // reflection strength, IOR air, IOR water, subsurface strength
    uint32 FoamTexture = ~0u;   // bindless foam texture index; ~0 = procedural foam
    uint32 NormalTexture = ~0u; // bindless detail normal index; ~0 = none
    uint32 EntityId = 0u;       // owning entity, the stable tiebreak between equal priorities
    int32 Priority = 0;         // higher applies later, over lower priorities
};
static_assert(sizeof(OceanWaterMaterialGPU) == 368);
// std430 offsets the GLSL mirror relies on: the point count lane pads to a vec4.
static_assert(offsetof(OceanWaterMaterialGPU, Deep) == 160);
static_assert(offsetof(OceanWaterMaterialGPU, FoamTexture) == 352);
OceanWaterMaterialGPU BuildOceanWaterMaterial(const Components::OceanSurface &surface);
} // namespace GameEngine::Ocean
