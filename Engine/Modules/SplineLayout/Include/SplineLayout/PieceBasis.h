#pragma once

#include "SplineLayout/PieceAxis.h"
#include "SplineLayout/TileLayout.h"

namespace GameEngine::SplineLayout
{

// Where a piece's OWN local axes point once it is laid on a path pose: its
// along-path axis on Forward, its other horizontal axis across travel. Every
// consumer that places mesh-local geometry in the world — footprint centering,
// the emitted transform — reads the piece's axes from here rather than assuming
// which one runs along the path.
//
// The X case is a yaw, never a mirror: the piece's +X runs downstream and its
// +Z points to the LEFT of travel, which keeps the basis determinant positive
// so asymmetric kit pieces (wall tenons, one-sided faces) are not flipped.
struct PieceBasis
{
    Mathematics::Vector3 LocalX;
    Mathematics::Vector3 LocalY;
    Mathematics::Vector3 LocalZ;
};

inline PieceBasis MakePieceBasis(const TilePose& pose, PieceAxis axis)
{
    if (axis == PieceAxis::X)
        return {pose.Forward, pose.Up, pose.Right * -1.0f};
    return {pose.Right, pose.Up, pose.Forward};
}

// The bounds-centre coordinate along the pose's RIGHT: the c for which the
// across-travel half of the layout's footprint centering is `-Right * c`.
//
// It is negated for an X-laid piece, and the negation is the `-1.0f` two lines
// above: such a piece meets the world Right through its local +Z, so a centre
// offset of +z sits at -c along Right. Anything that shears or measures across
// travel in WORLD terms needs this, not the raw local component.
inline float32 PieceCenterOnPoseRight(const Mathematics::Vector3& center, PieceAxis axis)
{
    return axis == PieceAxis::X ? -center.z : center.x;
}

} // namespace GameEngine::SplineLayout
