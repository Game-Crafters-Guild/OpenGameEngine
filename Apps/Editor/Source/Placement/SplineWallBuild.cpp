#include "Placement/SplineWallBuild.h"

#include "Placement/TileLayout.h"
#include "SplineGeometry/SplineStep.h"

#include <algorithm>
#include <limits>

namespace GameEngine::Editor
{

namespace SG = GameEngine::SplineGeometry;
using Mathematics::Vector3;

SG::SplineProfile BuildWallProfile(const Components::SplineWall& recipe)
{
    SG::SplineProfileParams params;
    params.Shape = SG::SplineProfileShape::Rectangle;
    params.Width = recipe.Thickness;
    params.Height = recipe.Height;
    return SG::BuildProfile(params);
}

SweepShape SweepShapeForWall(const Components::SplineWall& recipe)
{
    SweepShape shape;
    shape.Corners = recipe.Corner == Components::SplineWallCorner::Round ? SG::SplineCornerStyle::Round
                                                                         : SG::SplineCornerStyle::Mitre;
    shape.FollowCurve = true;
    shape.CloseLoop = true;
    shape.GroundBase = true;
    shape.FaceU = true;
    shape.StationAtPoints = true;
    return shape;
}

void StepWallTop(SweepStationStream& stream, const Components::SplineWall& recipe,
                 const Mathematics::Matrix4x4& invPlacerWorld, bool closedLoop)
{
    if (recipe.Grade != Components::SplineWallGrade::Stepped)
        return;
    const Vector3 localUp = NormalizedOrFallback(invPlacerWorld.TransformPoint(Vector3(0.0f, 1.0f, 0.0f)) -
                                                     invPlacerWorld.TransformPoint(Vector3(0.0f, 0.0f, 0.0f)),
                                                 Vector3(0.0f, 1.0f, 0.0f));
    SG::StepTopsAtPoints(stream.Local, stream.PointDistances, localUp, closedLoop);
    stream.WorldDistance.resize(stream.Local.size());
    for (size_t i = 0; i < stream.Local.size(); ++i)
        stream.WorldDistance[i] = stream.Local[i].Distance;
}

SG::SplineStripParams WallStripParams(const SweepStationStream& stream, bool closedLoop,
                                      float32 metresPerLocalUnit)
{
    SG::SplineStripParams params;
    params.WidthScale = SG::SplineProfileScale::None;
    params.TilesPerMetreU = 1.0f;
    // V and a face's lateral offset are measured on the LOCAL cross-section;
    // the placer's scale makes them world metres like U.
    params.TilesPerMetreV = metresPerLocalUnit;
    params.LateralMetresPerUnit = metresPerLocalUnit;
    params.GenerateCaps = !closedLoop;
    if (stream.Local.empty())
        return params;

    params.UOriginMetres = stream.WorldDistance.front();
    // One datum for the whole wall, its lowest base point, so the courses meet
    // across every corner, step and chunk.
    float32 lowestBase = std::numeric_limits<float32>::max();
    for (const SG::SplineStripStation& station : stream.Local)
        lowestBase = std::min(lowestBase, station.Position.y + station.Up.y * station.GroundOffset);
    params.HeightVDatumMetres = lowestBase;

    if (closedLoop)
    {
        const SG::SplineStripStation& last = stream.Local.back();
        params.LoopLengthMetres = stream.WorldDistance.back() - stream.WorldDistance.front();
        params.LoopFaceTurnLeft = last.FaceTurnLeft;
        params.LoopFaceTurnRight = last.FaceTurnRight;
        params.LoopTopArcAllowance = last.TopArcAllowance;
    }
    return params;
}

} // namespace GameEngine::Editor
