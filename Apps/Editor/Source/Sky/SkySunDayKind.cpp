#include "Sky/SkySunDayKind.h"

#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunPath.h"
#include "Rendering/Sky/SolarPath.h"

#include <cmath>

namespace GameEngine::Editor
{
namespace
{
constexpr double kDayKindDegreesPerRadian = 57.295779513082320876;
constexpr float kDayKindFullDayHours = 24.0f;
// The sun sits on the axis, or opposite it, when its circle about the axis is narrower than the
// readout's one decimal can show.
constexpr double kStillSunDeclinationDegrees = 89.95;
constexpr float kVerticalWithinDegrees = 1.0f;
} // namespace

SkySunDayKind ClassifySkySunDay(const Components::SkyEnvironment& sky)
{
    const Rendering::SolarPathAngles angles = Components::SkySunPath::PathAngles(sky);
    if (sky.SunPath == Components::SkySunPathKind::Custom)
    {
        if (std::abs(angles.DeclinationRadians * kDayKindDegreesPerRadian) >= kStillSunDeclinationDegrees)
            return SkySunDayKind::StandsStill;
        if (IsVerticalAxis(sky.CustomAxisAltitude))
            return SkySunDayKind::CirclesAtOneHeight;
    }
    const float dayLength = Rendering::SolarDayLengthHours(angles);
    if (dayLength <= 0.0f)
        return SkySunDayKind::NeverRises;
    return dayLength >= kDayKindFullDayHours ? SkySunDayKind::NeverSets : SkySunDayKind::RisesAndSets;
}

bool IsVerticalAxis(float axisAltitudeDegrees)
{
    return std::abs(axisAltitudeDegrees) >= Rendering::kMaximumLatitudeDegrees - kVerticalWithinDegrees;
}

} // namespace GameEngine::Editor
