#pragma once

#include "ECS/Entity.h"

#include <functional>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

// How a feature that owns a spline as its shape (a mark-up region's outline) claims it from the
// spline tools: a claimed spline draws no centerline (its owner draws it), offers its knots only
// while it is the selected spline (never through "show all controls", and never while its owner
// is hidden), keeps its Closed flag, and a claimed spline's edit offers no other spline's knots.
// The spline gizmo, the spline tool and the spline inspector ask through here and never name the
// owner.
struct SplineOwnerClaim
{
    bool Claimed = false;
    bool Hidden = false; // the owner is hidden from view: the knots never show
    // Why the spline's Closed row cannot change, in the owner's words ("A region's outline is
    // always closed"); a claim leaves it null only when nothing is claimed.
    const char* ClosedRowTooltip = nullptr;
};

using SplineOwnerQuery = std::function<SplineOwnerClaim(const ECS::World&, ECS::EntityHandle)>;

// Registers the one owner query (the mark-ups module's); an empty query claims nothing.
void SetSplineOwnerQuery(SplineOwnerQuery query);
// The claim on the spline of `entity`; nothing claimed without a registered query.
SplineOwnerClaim QuerySplineOwner(const ECS::World& world, ECS::EntityHandle entity);

} // namespace GameEngine::Editor
