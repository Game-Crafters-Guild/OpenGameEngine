#include "Inspectors/SkySunPathCaptions.h"

#include "Rendering/Sky/SolarPath.h"
#include "Sky/SkySunDayKind.h"

namespace GameEngine
{

std::string LatitudeCaption(float degrees)
{
    if (degrees == 0.0f)
        return "(equator)";
    return degrees > 0.0f ? "(north)" : "(south)";
}

std::string HeadingCaption(float degrees)
{
    // One unit on a wrapped line: a no-break space (U+00A0) after "along" and a non-breaking hyphen
    // (U+2011) for the minus, as the lux figures keep their units.
    constexpr struct
    {
        float Degrees;
        const char* Text;
    } kAxes[] = {{0.0f, "(along\xC2\xA0+Z)"},
                 {90.0f, "(along\xC2\xA0+X)"},
                 {180.0f, "(along\xC2\xA0\xE2\x80\x91" "Z)"},
                 {270.0f, "(along\xC2\xA0\xE2\x80\x91" "X)"}};
    for (const auto& axis : kAxes)
    {
        if (degrees == axis.Degrees)
            return axis.Text;
    }
    return {};
}

std::string AxisAltitudeCaption(float degrees)
{
    if (degrees == 0.0f)
        return "(level)";
    if (degrees == Rendering::kMaximumLatitudeDegrees)
        return "(straight up)";
    if (degrees == -Rendering::kMaximumLatitudeDegrees)
        return "(straight down)";
    return {};
}

std::string NoonHeightCaption(float degrees, float axisAltitudeDegrees)
{
    if (degrees == Rendering::kOverheadNoonHeightDegrees)
        return "(overhead)";
    // A vertical axis has no sides to name.
    const bool vertical = Editor::IsVerticalAxis(axisAltitudeDegrees);
    if (degrees < 0.0f)
        return vertical ? "(below)" : "(below, far side)";
    if (degrees > Rendering::kAxisSideHorizonNoonHeightDegrees)
        return vertical ? "(below)" : "(below, axis side)";
    if (vertical)
        return {};
    if (degrees == 0.0f)
        return "(horizon, far side)";
    if (degrees == Rendering::kAxisSideHorizonNoonHeightDegrees)
        return "(horizon, axis side)";
    return degrees < Rendering::kOverheadNoonHeightDegrees ? "(far side)" : "(axis side)";
}

} // namespace GameEngine
