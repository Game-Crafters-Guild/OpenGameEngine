#pragma once

#include <cstdint>

namespace GameEngine::Components
{
struct SkyEnvironment;
}

namespace GameEngine::Editor
{

// What the sun does over the day on a sky's path. The inspector's readout words the day and the
// Scene View overlay marks it from this one reading, so the card never says the sun stays on the
// horizon over an overlay that marks a sunrise.
enum class SkySunDayKind : std::uint8_t
{
    // Crosses the horizon twice: the day has a sunrise and a sunset.
    RisesAndSets,
    // Stays above the horizon (polar day on Earth).
    NeverSets,
    // Stays below the horizon (polar night on Earth).
    NeverRises,
    // A Custom axis within about a degree of straight up or down: the sun circles the sky at one
    // height, on the horizon when that height is 0, and its noon has no side to be on.
    CirclesAtOneHeight,
    // A Custom noon height at either end of the axis's reach: the sun sits on the axis it would
    // circle, or straight opposite it, and does not move.
    StandsStill,
};

SkySunDayKind ClassifySkySunDay(const Components::SkyEnvironment& sky);

// True for a Custom axis within about a degree of straight up or down: the sun then circles the sky
// at one height, and its noon has no side of the axis to be on.
bool IsVerticalAxis(float axisAltitudeDegrees);

} // namespace GameEngine::Editor
