#pragma once

#include "Ocean/OceanInputRegistry.h"
#include "Types/Types.h"

#include <string>
#include <vector>

namespace GameEngine::Components
{
struct OceanRenderer;
struct OceanSurface;
}

namespace GameEngine::Ocean
{

class OceanSettingsAsset;

enum class OceanValidationSeverity : uint32
{
    Info = 0u,
    Warning = 1u,
    Error = 2u,
};

struct OceanValidationIssue
{
    OceanValidationSeverity Severity = OceanValidationSeverity::Info;
    std::string Field;
    std::string Message;
    bool HasAutomaticFix = false;
};

class OceanSetupValidator
{
public:
    static std::vector<OceanValidationIssue> Validate(
        const Components::OceanRenderer& renderer,
        const Components::OceanSurface& surface,
        const OceanSettingsAsset* collisionSettings = nullptr,
        const OceanSettingsAsset* dynamicSettings = nullptr,
        const OceanSettingsAsset* foamSettings = nullptr,
        const OceanSettingsAsset* shadowSettings = nullptr,
        const OceanInputFrameStats* inputStats = nullptr);

    // Applies only lossless/safety fixes: invalid enum/range values are clamped,
    // zero capacities become one, and inverted scale/depth ranges are repaired.
    // Artistic values within a valid range are never changed.
    static uint32 ApplyAutomaticFixes(Components::OceanRenderer& renderer,
                                      Components::OceanSurface& surface);
};

} // namespace GameEngine::Ocean
