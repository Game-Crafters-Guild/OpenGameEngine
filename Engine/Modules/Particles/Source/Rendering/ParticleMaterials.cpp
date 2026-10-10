#include "Particles/Rendering/ParticleMaterials.h"

#include "AssetCore/GUID.h"
#include "Components/Rendering/ParticleRenderer.h"
#include "GPUFogParticles/GPUFogParticlesMaterial.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>

namespace GameEngine::Particles
{
namespace
{
// The procedural smoke particles draw without a material asset: the GPU fog layers, denser and
// softer-edged than the fog material's own defaults.
MaterialDocument ProceduralSmokeMaterial()
{
    constexpr float kSimpleNoiseScale = 24.0f;
    constexpr float kVoronoiScale = 6.0f;
    constexpr float kSimpleNoiseAmount = 0.75f;
    constexpr float kSimplexNoiseAmount = 0.15f;
    constexpr float kVoronoiNoiseAmount = 0.55f;
    constexpr float kRadialMaskPower = 1.35f;
    constexpr float kEdgeSoftness = 0.22f;
    MaterialDocument doc = GPUFogParticles::CreateDefaultMaterial("Particle Material");
    doc.properties[std::string(GPUFogParticles::kSimpleNoiseScale)] = kSimpleNoiseScale;
    doc.properties[std::string(GPUFogParticles::kVoronoiScale)] = kVoronoiScale;
    doc.properties[std::string(GPUFogParticles::kSimpleNoiseAmount)] = kSimpleNoiseAmount;
    doc.properties[std::string(GPUFogParticles::kSimplexNoiseAmount)] = kSimplexNoiseAmount;
    doc.properties[std::string(GPUFogParticles::kVoronoiNoiseAmount)] = kVoronoiNoiseAmount;
    doc.properties[std::string(GPUFogParticles::kRadialMaskPower)] = kRadialMaskPower;
    doc.properties[std::string(GPUFogParticles::kEdgeSoftness)] = kEdgeSoftness;
    return doc;
}
// The power of the near fade a particle's colour and emission take beyond what the alpha already gives
// them (GE_ParticleNearFadeColorScale). The alpha always carries the fade once.
// - Add, destination OneMinusSrcAlpha: the sprite hides what is behind it by its faded alpha, so its
//   colour fades exactly as much. Source SrcAlpha (straight alpha) gets that from the alpha: 0. Source
//   One (premultiplied, the hybrid blend) does not: 1.
// - Add, destination One (additive): nothing hides behind the sprite and the colour fades as the square
//   of the fade, which spreads the dimming of a bright glow over a bright scene across the band. Source
//   SrcAlpha takes one fade from the alpha: 1. Source One takes both here: 2.
// - Every other blend (multiply DstColor / Zero and Zero / SrcColor, replace with destination Zero, a
//   destination of SrcAlpha, Min, Max, the subtractions): 0. Fading such a colour toward 0 does not
//   fade the sprite, it darkens or blackens what is behind it; left alone, a multiply sprite's tint
//   still collapses with the sprite at the end of the fade.
float NearFadeColorExponent(const std::optional<MaterialBlendState>& authored)
{
    const MaterialBlendState blend = authored.value_or(MaterialBlendState{});
    if (blend.ColorOp != MaterialBlendOp::Add)
        return 0.0f;
    const bool sourceOne = blend.SrcColorFactor == MaterialBlendFactor::One;
    const bool sourceAlpha = blend.SrcColorFactor == MaterialBlendFactor::SrcAlpha;
    if (blend.DstColorFactor == MaterialBlendFactor::OneMinusSrcAlpha)
        return sourceOne ? 1.0f : 0.0f;
    if (blend.DstColorFactor == MaterialBlendFactor::One)
        return sourceOne ? 2.0f : sourceAlpha ? 1.0f : 0.0f;
    return 0.0f;
}
} // namespace

MaterialDocument BuildParticleRenderMaterialDocument(const Components::ParticleRenderer& renderer,
                                                     const MaterialDocument* source)
{
    using Components::ParticleLightingMode;
    using Components::ParticleSixWayLayout;
    MaterialDocument doc = source ? *source : ProceduralSmokeMaterial();
    doc.surfaceShaderGuid.clear();
    doc.vertexModifierGuid.clear();
    doc.surfaceGraphGuid.clear();
    doc.surfaceGraph.clear();
    doc.customVertexShader = false;
    doc.zWrite = false;
    doc.vertexModifier = "Particles/particle_vertex.glsl";
    doc.surfaceShader = "Surfaces/particle_surface.glsl";
    const bool lit = renderer.Lighting != ParticleLightingMode::Unlit;
    doc.lightingModel = lit ? "StandardPBR" : "Unlit";
    doc.alphaMode = MaterialAlphaMode::Blend;
    doc.doubleSided = true;
    doc.keywords = {"PARTICLE_BUFFER"};
    if (!renderer.Texture.IsNull())
        doc.textures["albedoMap"] = renderer.Texture.ToGuid().ToString();
    if (doc.textures.contains("albedoMap"))
        doc.keywords.push_back("PARTICLE_TEXTURE");
    if (lit)
        doc.keywords.push_back("PARTICLE_LIT");
    const bool sixWay = renderer.Lighting == ParticleLightingMode::SixWay && !renderer.SixWayMapA.IsNull() &&
                        !renderer.SixWayMapB.IsNull();
    if (sixWay)
    {
        doc.keywords.push_back("SIX_WAY");
        if (renderer.SixWayLayout == ParticleSixWayLayout::RightTopBackRgba)
            doc.keywords.push_back("SIX_WAY_RGBA");
        else if (renderer.SixWayLayout == ParticleSixWayLayout::TopLeftRightBottomBackFront)
            doc.keywords.push_back("SIX_WAY_TOP_LEFT_RIGHT_BOTTOM_BACK_FRONT");
        doc.textures["positiveAxesMap"] = renderer.SixWayMapA.ToGuid().ToString();
        doc.textures["negativeAxesMap"] = renderer.SixWayMapB.ToGuid().ToString();
    }
    if (!renderer.EmissionTexture.IsNull())
    {
        doc.keywords.push_back("PARTICLE_EMISSION_TEXTURE");
        doc.textures["emissionMap"] = renderer.EmissionTexture.ToGuid().ToString();
    }
    doc.properties["emissionColor"] = std::vector<float>{renderer.EmissionColor.r, renderer.EmissionColor.g,
                                                         renderer.EmissionColor.b, renderer.EmissionColor.a};
    doc.properties["emissionIntensity"] = std::max(renderer.EmissionIntensity, 0.0f);
    doc.properties["trailTextureTile"] = renderer.TrailTextureTile ? 1.0f : 0.0f;
    doc.properties["sixWayStrength"] = renderer.SixWayContrast;
    const uint32 columns = std::clamp(renderer.Columns, 1u, 256u);
    const uint32 rows = std::clamp(renderer.Rows, 1u, 256u);
    const uint32 cells = columns * rows;
    const uint32 frames = renderer.FrameCount ? std::min(cells, renderer.FrameCount) : cells;
    doc.properties["sheetColumns"] = static_cast<float>(columns);
    doc.properties["sheetRows"] = static_cast<float>(rows);
    doc.properties["sheetFrames"] = static_cast<float>(frames);
    // The surface takes its second, frame-blending fetch only when this is 1: a sheet of one frame has
    // nothing to blend.
    doc.properties["sheetBlend"] = renderer.BlendFrames && frames > 1 ? 1.0f : 0.0f;
    doc.properties["sheetLoop"] = renderer.Loop && renderer.FrameRate > 0 ? 1.0f : 0.0f;
    // Under the default straight-alpha blend a fragment of opacity 0 changes nothing, so the surface
    // discards it unshaded; an authored blend (additive, premultiplied) can draw at opacity 0.
    doc.properties["straightAlphaBlend"] = !doc.blend || *doc.blend == MaterialBlendState{} ? 1.0f : 0.0f;
    doc.properties["nearFadeColorExponent"] = NearFadeColorExponent(doc.blend);
    return doc;
}

std::vector<MaterialDocument> ParticleRenderMaterialShapes()
{
    using Components::ParticleLightingMode;
    using Components::ParticleSixWayLayout;
    // A program depends on which maps are assigned, never on which asset they name.
    const GUID placeholder = GUID::Derive(GUID::Null(), "engine/particles/material-shape-placeholder");
    struct Lighting
    {
        ParticleLightingMode Mode;
        ParticleSixWayLayout Layout;
    };
    constexpr std::array<Lighting, 5> kLightings = {{
        {ParticleLightingMode::Unlit, ParticleSixWayLayout::SignedAxes},
        {ParticleLightingMode::Lit, ParticleSixWayLayout::SignedAxes},
        {ParticleLightingMode::SixWay, ParticleSixWayLayout::SignedAxes},
        {ParticleLightingMode::SixWay, ParticleSixWayLayout::RightTopBackRgba},
        {ParticleLightingMode::SixWay, ParticleSixWayLayout::TopLeftRightBottomBackFront},
    }};
    std::vector<MaterialDocument> shapes;
    for (const Lighting& lighting : kLightings)
        for (const bool texture : {false, true})
            for (const bool emission : {false, true})
            {
                Components::ParticleRenderer renderer;
                renderer.Lighting = lighting.Mode;
                renderer.SixWayLayout = lighting.Layout;
                if (lighting.Mode == ParticleLightingMode::SixWay)
                {
                    renderer.SixWayMapA.Set(placeholder);
                    renderer.SixWayMapB.Set(placeholder);
                }
                if (texture)
                    renderer.Texture.Set(placeholder);
                if (emission)
                    renderer.EmissionTexture.Set(placeholder);
                shapes.push_back(BuildParticleRenderMaterialDocument(renderer, nullptr));
            }
    return shapes;
}

std::span<const ParticleRenderVariant> ParticleRenderVariants()
{
    using Rendering::MaterialKeyword;
    using Rendering::VertexAttributeFlags;
    constexpr MaterialKeyword kForward = kParticleDrawKeywords | MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows;
    constexpr MaterialKeyword kForwardIbl = kForward | MaterialKeyword::IBL;
    static constexpr std::array<ParticleRenderVariant, 5> kVariants = {{
        {"base", MaterialKeyword::None, VertexAttributeFlags::StandardMesh},
        {"forward-color", kForward, VertexAttributeFlags::StandardMesh},
        {"forward-color+tangent", kForward, VertexAttributeFlags::StandardMeshWithTangent},
        {"forward-color-ibl", kForwardIbl, VertexAttributeFlags::StandardMesh},
        {"forward-color-ibl+tangent", kForwardIbl, VertexAttributeFlags::StandardMeshWithTangent},
    }};
    return kVariants;
}

} // namespace GameEngine::Particles
