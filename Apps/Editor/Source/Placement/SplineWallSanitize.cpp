#include "Placement/SplineWallSanitize.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace GameEngine::Editor
{
namespace
{

using Components::kSplineWallMaxExtentMetres;
using Components::kSplineWallMinExtentMetres;
using Components::SplineWall;

[[nodiscard]] float32 SanitizedExtent(float32 value, float32 fallback)
{
    if (!std::isfinite(value))
        return fallback;
    return std::clamp(value, kSplineWallMinExtentMetres, kSplineWallMaxExtentMetres);
}

[[nodiscard]] bool BelowMinimum(float32 value)
{
    return std::isfinite(value) && value < kSplineWallMinExtentMetres;
}

[[nodiscard]] bool AboveMaximum(float32 value)
{
    return std::isfinite(value) && value > kSplineWallMaxExtentMetres;
}

} // namespace

SplineWall SanitizeWallRecipe(const SplineWall& authored)
{
    const SplineWall defaults{};
    SplineWall clean = authored;
    clean.Thickness = SanitizedExtent(authored.Thickness, defaults.Thickness);
    clean.Height = SanitizedExtent(authored.Height, defaults.Height);
    if (authored.Grade != Components::SplineWallGrade::Racked &&
        authored.Grade != Components::SplineWallGrade::Stepped)
        clean.Grade = defaults.Grade;
    if (authored.Corner != Components::SplineWallCorner::Mitre &&
        authored.Corner != Components::SplineWallCorner::Round)
        clean.Corner = defaults.Corner;
    return clean;
}

std::vector<std::string> WallRecipeValidation(const SplineWall& authored)
{
    std::vector<std::string> validation;
    if (BelowMinimum(authored.Thickness))
    {
        validation.push_back(std::format(
            "SplineWall: Thickness is below {:.2f} m, so the wall is built {:.2f} m thick. Set a "
            "thickness of at least {:.2f} m.",
            kSplineWallMinExtentMetres, kSplineWallMinExtentMetres, kSplineWallMinExtentMetres));
    }
    if (AboveMaximum(authored.Thickness))
    {
        validation.push_back(std::format(
            "SplineWall: Thickness is above {:.0f} m, so the wall is built {:.0f} m thick. Set a "
            "thickness of at most {:.0f} m.",
            kSplineWallMaxExtentMetres, kSplineWallMaxExtentMetres, kSplineWallMaxExtentMetres));
    }
    if (BelowMinimum(authored.Height))
    {
        validation.push_back(std::format(
            "SplineWall: Height is below {:.2f} m, so the wall is built {:.2f} m tall. Set a height "
            "of at least {:.2f} m.",
            kSplineWallMinExtentMetres, kSplineWallMinExtentMetres, kSplineWallMinExtentMetres));
    }
    if (AboveMaximum(authored.Height))
    {
        validation.push_back(std::format(
            "SplineWall: Height is above {:.0f} m, so the wall is built {:.0f} m tall. Set a height "
            "of at most {:.0f} m.",
            kSplineWallMaxExtentMetres, kSplineWallMaxExtentMetres, kSplineWallMaxExtentMetres));
    }
    return validation;
}

} // namespace GameEngine::Editor
