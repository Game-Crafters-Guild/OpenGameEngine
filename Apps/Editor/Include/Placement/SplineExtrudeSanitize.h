#pragma once

#include "Components/Spline/SplineExtrude.h"
#include "ECS/Entity.h"

#include <string>

namespace GameEngine::ECS { class World; }

namespace GameEngine::Editor
{

// A NaN in an authored float makes the recipe unequal to ITSELF under the
// defaulted operator==, which would re-arm the settle rebuild every frame for
// as long as the field stayed poisoned. The controller stores the sanitized
// value at candidate collection, so the value it compares can never be
// self-unequal.
//
// Non-finite floats fall back to the authored default (an infinity carries no
// intent to clamp); domain checks stay with the profile builder, which already
// clamps a negative dimension and rejects an unusable cross-section.
Components::SplineExtrude SanitizeExtrudeRecipe(const Components::SplineExtrude& authored);

// Whether `entity`'s SplineExtrude was saved with the retired Rectangle profile
// and its Profile has not been written since. The scene load keeps that line's
// text (it names no current profile) and leaves Profile at its default, a path;
// building or grading that path would put a strip where the author drew a wall.
// The extrude and ground-authority controllers leave such an extrude alone, and
// the extrude controller tells the author to use a Spline Wall
// (RetiredWallProfileValidation). Writing Profile supersedes the kept text, as
// it does for the next save, and the run builds as the profile chosen. The one
// write this cannot see is choosing the path profile the field fell back to;
// discarding the kept text in the inspector says that instead.
[[nodiscard]] bool CarriesRetiredWallProfile(const ECS::World& world, ECS::EntityHandle entity);

// The line an extrude carrying the retired profile is reported with, naming the
// fix.
[[nodiscard]] std::string RetiredWallProfileValidation();

} // namespace GameEngine::Editor
