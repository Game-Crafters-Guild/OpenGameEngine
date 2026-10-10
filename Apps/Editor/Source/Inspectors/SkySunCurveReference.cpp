#include "Inspectors/SkySunCurveReference.h"

#include <algorithm>

namespace GameEngine
{

bool SkySunCurveReferenceInputs::operator==(const SkySunCurveReferenceInputs& other) const
{
    const auto sameKeys = [](const Components::SkyScalarDayKeys& a, const Components::SkyScalarDayKeys& b) {
        return a.Midnight == b.Midnight && a.Dawn == b.Dawn && a.Midday == b.Midday && a.Sunset == b.Sunset;
    };
    const auto sameColours = [](const Components::SkyVec3DayKeys& a, const Components::SkyVec3DayKeys& b) {
        return std::equal(a.Midnight, a.Midnight + 3, b.Midnight) && std::equal(a.Dawn, a.Dawn + 3, b.Dawn) &&
               std::equal(a.Midday, a.Midday + 3, b.Midday) && std::equal(a.Sunset, a.Sunset + 3, b.Sunset);
    };
    return Latitude == other.Latitude && DayOfYear == other.DayOfYear && SunPath == other.SunPath &&
           CustomAxisAltitude == other.CustomAxisAltitude && CustomNoonHeight == other.CustomNoonHeight &&
           sameKeys(KeyTimes, other.KeyTimes) && sameColours(SunTint, other.SunTint);
}

SkySunCurveReferenceInputs ReadSkySunCurveReferenceInputs(const Components::SkyEnvironment& sky)
{
    SkySunCurveReferenceInputs inputs;
    inputs.Latitude = sky.Latitude;
    inputs.DayOfYear = sky.DayOfYear;
    inputs.SunPath = sky.SunPath;
    inputs.CustomAxisAltitude = sky.CustomAxisAltitude;
    inputs.CustomNoonHeight = sky.CustomNoonHeight;
    inputs.KeyTimes = sky.DayKeyTimesHours;
    inputs.SunTint = sky.SunTintKeys;
    return inputs;
}

} // namespace GameEngine
