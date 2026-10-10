#pragma once

#include "Components/Spline/SplineWall.h"

#include <string>
#include <vector>

namespace GameEngine::Editor
{

// The one gate a wall recipe passes before the controller compares or builds it.
// A NaN in an authored float makes the recipe unequal to ITSELF under the
// defaulted operator==, which would re-arm the settle rebuild every frame, so a
// non-finite float falls back to its default. A thickness or height outside
// [kSplineWallMinExtentMetres, kSplineWallMaxExtentMetres] is brought to the
// nearer bound. An enumerator outside its enum falls back to its default.
Components::SplineWall SanitizeWallRecipe(const Components::SplineWall& authored);

// What the author is told about an authored recipe the sanitizer had to change:
// one line per dimension brought to a bound, naming the fix. Lines carry no
// measurement, so a drag below the minimum reports once (ReportRecipeValidation).
std::vector<std::string> WallRecipeValidation(const Components::SplineWall& authored);

} // namespace GameEngine::Editor
