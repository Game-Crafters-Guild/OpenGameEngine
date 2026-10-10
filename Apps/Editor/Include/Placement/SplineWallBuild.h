#pragma once

#include "Components/Spline/SplineWall.h"
#include "Mathematics/Matrix4x4.h"
#include "Placement/SplineSweepStations.h"
#include "SplineGeometry/SplineProfile.h"
#include "SplineGeometry/SplineStripBuilder.h"
#include "Types/Types.h"

namespace GameEngine::Editor
{

// The pieces a swept wall is built from, between the drape and the chunks. The
// controller (Placement/SplineWallController.h) calls them in this order, and
// the tests call them the same way on an analytic ground.

// The wall's cross-section: a rectangle Thickness across and Height tall, its
// base points grounded. The wall reads no width channel, so the profile is
// swept at its own width (SplineProfileScale::None).
[[nodiscard]] SplineGeometry::SplineProfile BuildWallProfile(const Components::SplineWall& recipe);

// What the wall's sweep builds beyond its sampled centreline: the recipe's
// corner at every corner, a smooth spline's curves followed at the sagitta, a
// welded loop, a base sunk into a cross slope, U along each face, and a station
// on every authored point, where a stepped top steps.
[[nodiscard]] SweepShape SweepShapeForWall(const Components::SplineWall& recipe);

// Step the top of a Stepped wall at its authored points
// (SplineGeometry::StepTopsAtPoints) and bring the stream's WorldDistance up to
// date with the Crease copies that adds. A Racked wall is left as it is.
void StepWallTop(SweepStationStream& stream, const Components::SplineWall& recipe,
                 const Mathematics::Matrix4x4& invPlacerWorld, bool closedLoop);

// The strip parameters every chunk of one wall is built with, so the chunks meet
// on one U axis and one V datum:
//   - U in world metres along each face, from the run's first station; a welded
//     loop snaps each face to whole metres over its own length, the top over the
//     loop's length plus the arc its round corners sweep;
//   - V on the side faces, caps and risers is the height above the wall's lowest
//     base point, so courses stay level on a slope and meet across every corner,
//     step and chunk; the top takes V across its width;
//   - caps on an open run, none on a welded loop.
// `metresPerLocalUnit` is the placer's scale: the stations are local, U and V
// are world metres.
[[nodiscard]] SplineGeometry::SplineStripParams WallStripParams(const SweepStationStream& stream,
                                                                bool closedLoop,
                                                                float32 metresPerLocalUnit);

} // namespace GameEngine::Editor
