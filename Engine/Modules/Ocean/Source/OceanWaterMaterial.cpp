#include "Ocean/OceanWaterMaterial.h"
#include <algorithm>
namespace GameEngine::Ocean
{
OceanWaterMaterialGPU BuildOceanWaterMaterial(const Components::OceanSurface &s)
{
    OceanWaterMaterialGPU m{};
    const auto color = [](float *out, const auto &c) {
        out[0] = c.r;
        out[1] = c.g;
        out[2] = c.b;
        out[3] = c.a;
    };
    color(m.Deep, s.DeepColor);
    color(m.Diffuse, s.Diffuse);
    color(m.Grazing, s.DiffuseGrazing);
    color(m.Shadow, s.DiffuseShadow);
    color(m.Shallow, s.SubSurfaceShallowCol);
    color(m.Subsurface, s.SubSurfaceColour);
    color(m.Foam, s.FoamColor);
    color(m.Fog, s.DepthFogDensity);
    m.Fog[3] = s.RefractionStrength;
    m.Surface[0] = std::max(s.NormalsStrength, 0.0f);
    m.Surface[1] = std::max(s.NormalsScale, 0.001f);
    m.Surface[2] = std::clamp(s.Roughness, 0.0f, 1.0f);
    m.Surface[3] = std::max(s.Specular, 0.0f);
    m.FoamDetail[0] = std::max(s.FoamAmount, 0.0f);
    m.FoamDetail[1] = std::max(s.FoamScale, 0.01f);
    m.FoamDetail[2] = std::max(s.FoamFeather, 0.001f);
    m.FoamDetail[3] = std::clamp(s.FoamNormalStrength, 0.0f, 2.0f);
    m.Bubbles[0] = std::clamp(s.FoamBubbleCoverage, 0.0f, 1.0f);
    m.Bubbles[1] = std::clamp(s.FoamBubbleParallax, 0.0f, 0.5f);
    m.Bubbles[2] = std::clamp(s.FoamRoughness, 0.04f, 1.0f);
    m.Bubbles[3] = s.FresnelPower;
    m.Optics[0] = s.ReflectionStrength;
    m.Optics[1] = s.IorAir;
    m.Optics[2] = s.IorWater;
    m.Optics[3] = s.SubsurfaceStrength;
    return m;
}
} // namespace GameEngine::Ocean
