#include "GPUFogParticles/GPUFogParticlesMaterial.h"

#include "Rendering/Materials/MaterialAlphaMode.h"

#include <string>
#include <vector>

namespace GameEngine::GPUFogParticles
{
namespace
{

void SetFloat(MaterialDocument& doc, std::string_view key, float value)
{
    doc.properties[std::string(key)] = value;
}

void ApplySharedDefaults(MaterialDocument& doc)
{
    doc.schemaVersion = 3;
    doc.lightingModel = "Unlit";
    doc.alphaMode = MaterialAlphaMode::Blend;
    doc.doubleSided = true;
    doc.surfaceShader = std::string(kSurfaceShaderPath);
    doc.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f};
    doc.properties["opacity"] = 1.0f;

    SetFloat(doc, kSimpleNoiseScale, 20.0f);
    SetFloat(doc, kSimplexNoiseScale, 4.0f);
    SetFloat(doc, kVoronoiScale, 5.0f);
    SetFloat(doc, kCombinedNoiseRemap, 0.0f);

    SetFloat(doc, kSimpleNoiseAmount, 0.25f);
    SetFloat(doc, kSimplexNoiseAmount, 0.25f);
    SetFloat(doc, kVoronoiNoiseAmount, 0.5f);
    SetFloat(doc, kRadialMaskPower, 1.0f);

    SetFloat(doc, kSimpleNoiseRemap, 0.0f);
    SetFloat(doc, kSimplexNoiseRemap, 0.0f);
    SetFloat(doc, kVoronoiNoiseRemap, 0.0f);
    SetFloat(doc, kEdgeSoftness, 0.0f);

    SetFloat(doc, kSimpleAnimationX, 0.0f);
    SetFloat(doc, kSimpleAnimationY, 0.0f);
    SetFloat(doc, kSimpleAnimationZ, 0.0f);
    SetFloat(doc, kSimpleAnimationW, 0.0f);
    SetFloat(doc, kSimplexAnimationX, 0.0f);
    SetFloat(doc, kSimplexAnimationY, 0.0f);
    SetFloat(doc, kSimplexAnimationZ, 0.02f);
    SetFloat(doc, kSimplexAnimationW, 0.0f);
    SetFloat(doc, kVoronoiAnimationX, 0.0f);
    SetFloat(doc, kVoronoiAnimationY, 0.0f);
    SetFloat(doc, kVoronoiAnimationZ, 0.0f);
    SetFloat(doc, kVoronoiAnimationW, 0.0f);

    SetFloat(doc, kSurfaceDepthFade, 0.66f);
    SetFloat(doc, kShapeDistortion, 0.28f);
    SetFloat(doc, kWispyNoiseAmount, 0.35f);
    SetFloat(doc, kDetailNoiseAmount, 0.25f);
    SetFloat(doc, kCameraDepthFadeRange, 1.0f);
    SetFloat(doc, kCameraDepthFadeOffset, 0.0f);
}

} // namespace

MaterialDocument CreateDefaultMaterial(std::string_view name)
{
    MaterialDocument doc{};
    doc.materialName = std::string(name);
    ApplySharedDefaults(doc);
    return doc;
}

MaterialDocument CreateLargeFogMaterial()
{
    MaterialDocument doc = CreateDefaultMaterial("GPU Fog Particles Large");
    SetFloat(doc, kSimpleNoiseAmount, 0.8f);
    SetFloat(doc, kSimpleNoiseRemap, 0.5f);
    SetFloat(doc, kSimplexNoiseAmount, 0.0f);
    SetFloat(doc, kSimplexNoiseScale, 2.0f);
    SetFloat(doc, kSimplexNoiseRemap, 0.5f);
    SetFloat(doc, kVoronoiNoiseAmount, 0.6f);
    SetFloat(doc, kVoronoiNoiseRemap, 0.2f);
    SetFloat(doc, kRadialMaskPower, 1.35f);
    SetFloat(doc, kSimpleAnimationX, -0.1f);
    SetFloat(doc, kSimpleAnimationY, 0.05f);
    SetFloat(doc, kSimplexAnimationZ, 0.01f);
    SetFloat(doc, kShapeDistortion, 0.38f);
    SetFloat(doc, kWispyNoiseAmount, 0.45f);
    SetFloat(doc, kDetailNoiseAmount, 0.2f);
    return doc;
}

MaterialDocument CreateSmallFogMaterial()
{
    MaterialDocument doc = CreateDefaultMaterial("GPU Fog Particles Small");
    SetFloat(doc, kSimpleNoiseScale, 32.0f);
    SetFloat(doc, kSimplexNoiseScale, 3.0f);
    SetFloat(doc, kVoronoiScale, 8.0f);
    SetFloat(doc, kSimpleNoiseAmount, 0.65f);
    SetFloat(doc, kSimplexNoiseAmount, 0.15f);
    SetFloat(doc, kVoronoiNoiseAmount, 0.45f);
    SetFloat(doc, kSimpleNoiseRemap, 0.45f);
    SetFloat(doc, kVoronoiNoiseRemap, 0.25f);
    SetFloat(doc, kRadialMaskPower, 1.8f);
    SetFloat(doc, kSimpleAnimationX, -0.04f);
    SetFloat(doc, kSimpleAnimationY, 0.08f);
    SetFloat(doc, kSimplexAnimationZ, 0.018f);
    SetFloat(doc, kShapeDistortion, 0.24f);
    SetFloat(doc, kWispyNoiseAmount, 0.3f);
    SetFloat(doc, kDetailNoiseAmount, 0.35f);
    return doc;
}

} // namespace GameEngine::GPUFogParticles
