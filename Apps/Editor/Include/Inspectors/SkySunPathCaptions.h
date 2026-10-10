#pragma once

#include <string>

namespace GameEngine
{

// The captions the sky's sun path fields show after their number, in the field's own caption
// suffix: what the value means in words, so "-33.9 (south)" reads without a tooltip. Empty where the
// number says it all.

// "(north)", "(south)" or "(equator)". The degree sign is in the row's label.
std::string LatitudeCaption(float degrees);

// A heading about +Y, north or the custom axis alike: "(along +X)" when it lies along a world axis,
// empty between them. The gloss stays on one line when the text wraps.
std::string HeadingCaption(float degrees);

// The custom axis's altitude: "(level)", "(straight up)" or "(straight down)" at those three, empty
// between them.
std::string AxisAltitudeCaption(float degrees);

// Where the custom path's noon sun stands, short enough to sit on one line beside its number in the
// narrowest inspector: "(overhead)" at 90, "(far side)" (away from the axis) below it and "(axis
// side)" above it, "(horizon, far side)" at 0 and "(horizon, axis side)" at 180, "(below, far side)"
// under 0 and "(below, axis side)" over 180. Under a vertical axis, which has no sides, only
// "(overhead)" and, under 0 or over 180, "(below)".
std::string NoonHeightCaption(float degrees, float axisAltitudeDegrees);

} // namespace GameEngine
