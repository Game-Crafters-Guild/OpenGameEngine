#include "Ocean/ocean_material_profiles.glsl"
#include "Includes/bindless_textures.glsl"
OceanWaterMaterial oceanMaterial;
bool oceanHasMaterial = false;
void OceanResolveSurfaceMaterial(vec2 worldXZ)
{
    oceanMaterial.deep = uDeepColor;
    oceanMaterial.diffuse = uDiffuse;
    oceanMaterial.grazing = uDiffuseGrazing;
    oceanMaterial.shadow = uDiffuseShadow;
    oceanMaterial.shallow = uSubSurfaceShallowCol;
    oceanMaterial.subsurface = uSubSurfaceColour;
    oceanMaterial.foam = uFoamColor;
    oceanMaterial.fog = uDepthFogRefraction;
    oceanMaterial.surface = vec4(uNormalsStrength, uNormalsScale, uRoughness, uSpecular);
    oceanMaterial.foamDetail = vec4(uFoamAmount, uFoamScale, uFoamFeather, uFoamNormalStrength);
    oceanMaterial.bubbles = vec4(uFoamBubbleCoverage, uFoamBubbleParallax, uFoamRoughness, uFresnelPower);
    oceanMaterial.optics = vec4(uReflectionStrength, uIorAir, uIorWater, uSubsurfaceStrength);
    // Materials arrive sorted by priority; each fades over the result so far
    // across its footprint edge. Textures cannot blend: a material's own
    // textures take over where its weight passes one half.
    oceanMaterial.foamTexture = 0xffffffffu;
    oceanMaterial.normalTexture = 0xffffffffu;
#if !defined(GE_COMPAT_PROFILE)
    for (uint i = 0u; i < oceanMaterialMeta.x; ++i)
    {
        float weight = OceanMaterialWeight(oceanMaterials[i], worldXZ);
        if (weight <= 0.0)
            continue;
        OceanWaterMaterial m = oceanMaterials[i];
        oceanMaterial.deep = mix(oceanMaterial.deep, m.deep, weight);
        oceanMaterial.diffuse = mix(oceanMaterial.diffuse, m.diffuse, weight);
        oceanMaterial.grazing = mix(oceanMaterial.grazing, m.grazing, weight);
        oceanMaterial.shadow = mix(oceanMaterial.shadow, m.shadow, weight);
        oceanMaterial.shallow = mix(oceanMaterial.shallow, m.shallow, weight);
        oceanMaterial.subsurface = mix(oceanMaterial.subsurface, m.subsurface, weight);
        oceanMaterial.foam = mix(oceanMaterial.foam, m.foam, weight);
        oceanMaterial.fog = mix(oceanMaterial.fog, m.fog, weight);
        oceanMaterial.surface = mix(oceanMaterial.surface, m.surface, weight);
        oceanMaterial.foamDetail = mix(oceanMaterial.foamDetail, m.foamDetail, weight);
        oceanMaterial.bubbles = mix(oceanMaterial.bubbles, m.bubbles, weight);
        oceanMaterial.optics = mix(oceanMaterial.optics, m.optics, weight);
        if (weight >= 0.5)
        {
            oceanMaterial.foamTexture = m.foamTexture;
            oceanMaterial.normalTexture = m.normalTexture;
            oceanHasMaterial = true;
        }
    }
#endif
}
uint OceanMaterialNormalAvailable()
{
#if defined(GE_COMPAT_PROFILE)
    return 0u;
#else
    return oceanHasMaterial ? (oceanMaterial.normalTexture != 0xffffffffu ? 1u : 0u) : uNormalTextureAvailable;
#endif
}
vec2 OceanMaterialNormalSample(vec2 uv, vec2 dx, vec2 dy)
{
#if defined(GE_COMPAT_PROFILE)
    return vec2(0.5);
#else
    if (oceanHasMaterial)
        return textureGrad(GE_BTEX(oceanMaterial.normalTexture, GE_TS_REPEAT), uv, dx, dy).rg;
    return textureGrad(uOceanDetailNormal, uv, dx, dy).rg;
#endif
}
#define uDeepColor oceanMaterial.deep
#define uDiffuse oceanMaterial.diffuse
#define uDiffuseGrazing oceanMaterial.grazing
#define uDiffuseShadow oceanMaterial.shadow
#define uSubSurfaceShallowCol oceanMaterial.shallow
#define uSubSurfaceColour oceanMaterial.subsurface
#define uFoamColor oceanMaterial.foam
#define uDepthFogRefraction oceanMaterial.fog
#define uNormalsStrength oceanMaterial.surface.x
#define uNormalsScale oceanMaterial.surface.y
#define uRoughness oceanMaterial.surface.z
#define uSpecular oceanMaterial.surface.w
#define uFoamAmount oceanMaterial.foamDetail.x
#define uFoamScale oceanMaterial.foamDetail.y
#define uFoamFeather oceanMaterial.foamDetail.z
#define uFoamNormalStrength oceanMaterial.foamDetail.w
#define uFoamBubbleCoverage oceanMaterial.bubbles.x
#define uFoamBubbleParallax oceanMaterial.bubbles.y
#define uFoamRoughness oceanMaterial.bubbles.z
#define uFresnelPower oceanMaterial.bubbles.w
#define uReflectionStrength oceanMaterial.optics.x
#define uIorAir oceanMaterial.optics.y
#define uIorWater oceanMaterial.optics.z
#define uSubsurfaceStrength oceanMaterial.optics.w
