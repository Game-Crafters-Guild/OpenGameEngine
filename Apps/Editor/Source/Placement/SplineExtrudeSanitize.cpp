#include "Placement/SplineExtrudeSanitize.h"

#include "ECS/Entity.h"
#include "ECS/UnresolvedComponentStore.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>

namespace GameEngine::Editor
{
namespace
{

float32 SanitizedFloat(float32 value, float32 fallback)
{
    return std::isfinite(value) ? value : fallback;
}

bool EqualsIgnoringCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
                      { return std::tolower(static_cast<unsigned char>(x)) ==
                               std::tolower(static_cast<unsigned char>(y)); });
}

} // namespace

Components::SplineExtrude SanitizeExtrudeRecipe(const Components::SplineExtrude& authored)
{
    const Components::SplineExtrude defaults{};
    Components::SplineExtrude clean = authored;
    clean.Width = SanitizedFloat(authored.Width, defaults.Width);
    clean.EdgeDrop = SanitizedFloat(authored.EdgeDrop, defaults.EdgeDrop);
    clean.EdgeInset = SanitizedFloat(authored.EdgeInset, defaults.EdgeInset);
    clean.CrownRise = SanitizedFloat(authored.CrownRise, defaults.CrownRise);
    clean.ShoulderWidth = SanitizedFloat(authored.ShoulderWidth, defaults.ShoulderWidth);
    clean.ShoulderDrop = SanitizedFloat(authored.ShoulderDrop, defaults.ShoulderDrop);
    clean.MaxHalfWidth = SanitizedFloat(authored.MaxHalfWidth, defaults.MaxHalfWidth);
    // A non-finite floor falls back to the DISABLED value, which is finite, so a
    // poisoned field can never leave a NaN in a recipe compared against itself.
    clean.SeaLevelFloor = SanitizedFloat(authored.SeaLevelFloor, defaults.SeaLevelFloor);
    clean.EndTaperMetres = SanitizedFloat(authored.EndTaperMetres, defaults.EndTaperMetres);
    clean.LateralOffset = SanitizedFloat(authored.LateralOffset, defaults.LateralOffset);
    clean.VerticalOffset = SanitizedFloat(authored.VerticalOffset, defaults.VerticalOffset);
    clean.TilesPerMetreU = SanitizedFloat(authored.TilesPerMetreU, defaults.TilesPerMetreU);
    clean.TilesPerMetreV = SanitizedFloat(authored.TilesPerMetreV, defaults.TilesPerMetreV);
    return clean;
}

bool CarriesRetiredWallProfile(const ECS::World& world, ECS::EntityHandle entity)
{
    const ECS::UnresolvedComponentStore* store = world.TryGetUnresolvedComponents();
    const std::vector<ECS::PreservedField>* fields = store ? store->FieldsFor(entity) : nullptr;
    if (!fields)
        return false;
    return std::any_of(fields->begin(), fields->end(), [&](const ECS::PreservedField& field)
                       {
                           return EqualsIgnoringCase(field.Component, "SplineExtrude") &&
                                  EqualsIgnoringCase(field.Field, "Profile") &&
                                  EqualsIgnoringCase(field.RawText, "Rectangle") &&
                                  !ECS::IsPreservedFieldSuperseded(world, entity, field);
                       });
}

std::string RetiredWallProfileValidation()
{
    return "SplineExtrude: this run was saved with the Rectangle profile, which is now the Spline "
           "Wall component, so nothing is built from it. Replace it with a Spline Wall: add a Spline "
           "Wall to this spline and remove the Spline Extrude.";
}

} // namespace GameEngine::Editor
