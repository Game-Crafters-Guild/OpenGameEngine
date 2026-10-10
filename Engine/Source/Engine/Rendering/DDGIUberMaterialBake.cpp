#include "Engine/Rendering/DDGIUberMaterialBake.h"

#include "Engine/Rendering/DDGIEmissive.h"
#include "Engine/Rendering/DDGIMaterialMapAtlas.h"
#include "Engine/Rendering/Material.h"
#include "Rendering/Materials/LegacyMaterialLanes.h"
#include "Rendering/Materials/MaterialParamsLayout.h"
#include "Rendering/Materials/ShaderPropertyTable.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace GameEngine::Engine::Renderer
{
namespace
{

const float* BlockAt(const MaterialGpuParams& params, uint32_t byteOffset)
{
    return reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(&params) + byteOffset);
}

// `count` floats of `name` as the surface sees them, found by NAME: a surface
// that declares its properties packs them in declaration order, so nothing sits
// at a fixed lane — the shipped waterfall_fx puts panSpeed on lane 0 and
// waterColor on lane 1, exactly where a fixed reader would look for base colour
// and metallic. A declared surface answers from its own table: the block where
// the name has a lane, the declaration's default where the compiler folded it
// to a constant (an adapter read no surface redeclares). Every other surface
// answers from the shared legacy map. A name the material does not author at
// all reads `fallback` in every component.
void ReadParam(const Material& material, const MaterialGpuParams& params, std::string_view name,
               float* out, uint32_t count, float fallback)
{
    std::fill_n(out, count, fallback);
    const float* source = nullptr;
    uint32_t components = 0;
    if (const Rendering::ShaderPropertyTable* declared = material.GetDeclaredProperties())
    {
        const Rendering::ShaderProperty* prop = declared->Find(name);
        if (!prop)
            return;
        source = prop->HasLane ? BlockAt(params, prop->ByteOffset) : prop->Default.data();
        components = prop->ComponentCount();
    }
    else if (const Rendering::LegacyMaterialLane* lane = Rendering::FindLegacyMaterialLane(name))
    {
        source = BlockAt(params, Rendering::LegacyLaneByteOffset(*lane));
        components = lane->Components;
    }
    else
    {
        return;
    }
    std::copy_n(source, std::min(count, components), out);
}

} // namespace

// The material record the software kernel shades with. Reads the same PARAMETERS
// ddgi_trace_hw.comp reads out of the MaterialParams SSBO (base colour,
// metallic/roughness, emission) so the two lanes light a surface identically on
// every surface that addresses the block by lane name. On a surface that
// declares its properties the two disagree: this side follows the declaration,
// the hardware kernel still indexes fixed lanes because the GLSL has no
// per-material lane map to follow (design doc §10 owns closing that);
// the emission luminance is folded into EmissiveR/G/B here because
// the reference's record has no separate intensity field; a bound emissive map
// multiplies that colour, as on the raster surface. It is scene-linear: the
// authored nits are divided by the reference-white anchor (ComputeDDGIEmissive),
// matching the raster surface and the hardware lane.
// The map slots (12 albedo, 15 metallic-roughness, 16 emissive, 18..21 the one
// shared uv transform)
// come from the shared DDGIMaterialMapAtlas, which the hardware lane reads
// through its own table — same atlas, same layers, same uv transform, so
// textured surfaces agree across lanes too.
SceneBvh::UberMaterial BakeUberMaterial(const Material* material,
                                        const DDGIMaterialMapAtlas* atlas)
{
    SceneBvh::UberMaterial baked{};
    if (!material)
        return baked;  // white, non-metal, non-emissive — never fail the sweep over a material

    if (atlas)
    {
        const DDGIMaterialMapAtlas::MaterialRecord& maps =
            atlas->GetRecordForMaterialSlot(material->GetGpuSceneMaterialIndex());
        baked.AlbedoMapLayer    = maps.AlbedoLayer;
        baked.MetalnessMapLayer = maps.MetallicLayer;
        baked.EmissiveMapLayer  = maps.EmissiveLayer;
        baked.UvRepeatX        = maps.ScaleX;
        baked.UvRepeatY        = maps.ScaleY;
        baked.UvOffsetX        = maps.OffsetX;
        baked.UvOffsetY        = maps.OffsetY;
    }

    MaterialGpuParams params{};
    const uint32_t copyBytes = std::min<uint32_t>(material->GetCacheSize(), sizeof(params));
    if (copyBytes == 0)
        return baked;
    std::memcpy(&params, material->GetCacheData(), copyBytes);

    // Defaults are the raster surface's own: an opaque white non-metal at the
    // mid roughness the standard adapter assumes, so a material that authors
    // none of these bounces neutral light rather than black.
    float baseColor[4] = {};
    ReadParam(*material, params, "baseColor", baseColor, 4, 1.0f);
    float metallic = 0.0f;
    ReadParam(*material, params, "metallic", &metallic, 1, 0.0f);
    float roughness = 0.5f;
    ReadParam(*material, params, "roughness", &roughness, 1, 0.5f);
    float emission[4] = {};
    ReadParam(*material, params, "emissive", emission, 3, 0.0f);
    ReadParam(*material, params, "emissionLuminance", &emission[3], 1, 0.0f);

    baked.BaseColorR        = baseColor[0];
    baked.BaseColorG        = baseColor[1];
    baked.BaseColorB        = baseColor[2];
    baked.Opacity           = baseColor[3];
    baked.Metalness         = metallic;
    baked.Roughness         = roughness;
    // Authored emission (tint x nits) folded to scene-linear by the reference-
    // white anchor, so the software lane emits exactly what the raster surface
    // shows rather than 203x hotter.
    const DDGIEmissive emissive =
        ComputeDDGIEmissive(emission[0], emission[1], emission[2], emission[3]);
    baked.EmissiveR         = emissive.Color[0];
    baked.EmissiveG         = emissive.Color[1];
    baked.EmissiveB         = emissive.Color[2];
    return baked;
}

} // namespace GameEngine::Engine::Renderer
