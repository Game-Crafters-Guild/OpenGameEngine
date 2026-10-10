#pragma once

#include "Components/Rendering/SkyEnvironment.h"

#include <cstdint>

namespace GameEngine
{
// What the curve's physical reference line is drawn from: the sky's path and day and its sun tint keys,
// for a clear sun of kClearNoonSunIlluminanceLux. Never the light the curve drives, whose Intensity the
// curve itself writes, so the reference stays put while the curve is edited. The line is redrawn only
// when these change, compared field by field.
struct SkySunCurveReferenceInputs
{
    float Latitude = 0.0f;
    int32_t DayOfYear = 0;
    Components::SkySunPathKind SunPath = Components::SkySunPathKind::Earth;
    float CustomAxisAltitude = 0.0f;
    float CustomNoonHeight = 0.0f;
    Components::SkyScalarDayKeys KeyTimes{};
    Components::SkyVec3DayKeys SunTint{};

    bool operator==(const SkySunCurveReferenceInputs& other) const;
};

SkySunCurveReferenceInputs ReadSkySunCurveReferenceInputs(const Components::SkyEnvironment& sky);

} // namespace GameEngine
