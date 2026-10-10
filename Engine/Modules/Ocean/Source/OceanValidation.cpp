#include "Ocean/OceanValidation.h"

#include "Components/Rendering/Ocean.h"
#include "Ocean/OceanSettingsAsset.h"
#include "Ocean/OceanTypes.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::Ocean
{
namespace
{
void Add(std::vector<OceanValidationIssue>& issues, OceanValidationSeverity severity,
         const char* field, const char* message, bool fix = true)
{
    issues.push_back({severity, field, message, fix});
}
}

std::vector<OceanValidationIssue> OceanSetupValidator::Validate(
    const Components::OceanRenderer& renderer, const Components::OceanSurface& surface,
    const OceanSettingsAsset* collisionSettings,
    const OceanSettingsAsset* dynamicSettings,
    const OceanSettingsAsset* foamSettings,
    const OceanSettingsAsset* shadowSettings,
    const OceanInputFrameStats* inputStats)
{
    std::vector<OceanValidationIssue> issues;
    if (renderer.QualityOverride > 2u)
        Add(issues, OceanValidationSeverity::Error, "Renderer.QualityOverride",
            "Quality must be Low, Medium, or High.");
    if (renderer.LodCount == 0u || renderer.LodCount > kMaxOceanLodCascades)
        Add(issues, OceanValidationSeverity::Error, "Renderer.LodCount",
            "LOD count is outside the allocated ocean cascade range.");
    if (renderer.LodDataResolution < 32u || renderer.LodDataResolution > 2048u)
        Add(issues, OceanValidationSeverity::Warning, "Renderer.LodDataResolution",
            "LOD data resolution should be between 32 and 2048 texels.");
    if (!std::isfinite(renderer.MinScale) || renderer.MinScale <= 0.0f)
        Add(issues, OceanValidationSeverity::Error, "Renderer.MinScale",
            "Minimum cascade scale must be finite and positive.");
    if (!std::isfinite(renderer.MaxScale) || renderer.MaxScale < renderer.MinScale)
        Add(issues, OceanValidationSeverity::Error, "Renderer.MaxScale",
            "Maximum scale must not be smaller than minimum scale.");
    if (renderer.GeometryUpSampleFactor == 0u || renderer.GeometryDownSampleFactor == 0u)
        Add(issues, OceanValidationSeverity::Error, "Renderer.GeometrySampling",
            "Geometry sampling factors must be non-zero.");
    if (renderer.CollisionProvider > 4u || renderer.MaxCollisionQueryCount == 0u)
        Add(issues, OceanValidationSeverity::Error, "Renderer.Collision",
            "Collision provider mode or capacity is invalid.");
    if (!std::isfinite(renderer.GravityMultiplier) || renderer.GravityMultiplier <= 0.0f)
        Add(issues, OceanValidationSeverity::Error, "Renderer.GravityMultiplier",
            "Gravity multiplier must be finite and positive.");

    if (surface.IorAir <= 0.0f || surface.IorWater <= 0.0f)
        Add(issues, OceanValidationSeverity::Error, "Surface.IOR",
            "Air and water indices of refraction must be positive.");
    if (surface.DepthFogEndDistance > 0.0f &&
        surface.DepthFogEndDistance < surface.DepthFogStartDistance)
        Add(issues, OceanValidationSeverity::Error, "Surface.DepthFogEndDistance",
            "Fog end distance must be zero (automatic) or after its start distance.");
    if (surface.ShorelineFoamMaxDepth < 0.0f || surface.IntersectionFoamDepth < 0.0f)
        Add(issues, OceanValidationSeverity::Error, "Surface.FoamDepth",
            "Foam depth ranges cannot be negative.");

    const OceanSettingsAsset* settings[] = {
        collisionSettings, dynamicSettings, foamSettings, shadowSettings};
    for (const OceanSettingsAsset* asset : settings)
    {
        if (!asset)
            continue;
        std::string error;
        if (!asset->Validate(&error))
            issues.push_back({OceanValidationSeverity::Error, "Settings", error, false});
    }
    if (inputStats && inputStats->HasDroppedInputs())
    {
        issues.push_back({OceanValidationSeverity::Warning, "Inputs.Capacity",
                          "Ocean input capacity was exceeded; inspect per-family drop counters.",
                          false});
    }
    return issues;
}

uint32 OceanSetupValidator::ApplyAutomaticFixes(Components::OceanRenderer& renderer,
                                                 Components::OceanSurface& surface)
{
    uint32 fixes = 0u;
    const auto assign = [&](auto& target, const auto& value) {
        if (target == value)
            return;
        target = value;
        ++fixes;
    };
    assign(renderer.QualityOverride, std::min(renderer.QualityOverride, 2u));
    assign(renderer.LodCount, std::clamp(renderer.LodCount, 1u, kMaxOceanLodCascades));
    assign(renderer.LodDataResolution,
           std::clamp(renderer.LodDataResolution, 32u, 2048u));
    if (!std::isfinite(renderer.MinScale) || renderer.MinScale <= 0.0f)
        assign(renderer.MinScale, 64.0f);
    if (!std::isfinite(renderer.MaxScale) || renderer.MaxScale < renderer.MinScale)
        assign(renderer.MaxScale, renderer.MinScale);
    assign(renderer.GeometryUpSampleFactor,
           std::max(renderer.GeometryUpSampleFactor, 1u));
    assign(renderer.GeometryDownSampleFactor,
           std::max(renderer.GeometryDownSampleFactor, 1u));
    assign(renderer.CollisionProvider, std::min(renderer.CollisionProvider, 4u));
    assign(renderer.MaxCollisionQueryCount,
           std::max(renderer.MaxCollisionQueryCount, 1u));
    if (!std::isfinite(renderer.GravityMultiplier) || renderer.GravityMultiplier <= 0.0f)
        assign(renderer.GravityMultiplier, 1.0f);
    if (surface.IorAir <= 0.0f)
        assign(surface.IorAir, 1.0f);
    if (surface.IorWater <= 0.0f)
        assign(surface.IorWater, 1.333f);
    assign(surface.DepthFogStartDistance, std::max(surface.DepthFogStartDistance, 0.0f));
    if (surface.DepthFogEndDistance > 0.0f &&
        surface.DepthFogEndDistance < surface.DepthFogStartDistance)
        assign(surface.DepthFogEndDistance, surface.DepthFogStartDistance);
    assign(surface.ShorelineFoamMaxDepth, std::max(surface.ShorelineFoamMaxDepth, 0.0f));
    assign(surface.IntersectionFoamDepth, std::max(surface.IntersectionFoamDepth, 0.0f));
    return fixes;
}

} // namespace GameEngine::Ocean
