#pragma once

#include "Components/Name.h"
#include "Mathematics/Matrix4x4.h"
#include "SplineLayout/PieceAxis.h"
#include "Types/Types.h"

namespace GameEngine::Components
{
struct Transform;
}

namespace GameEngine::SplineLayout
{

struct TilePose; // SplineLayout/TileLayout.h

// The shape shared by every generated placement piece: it is a CHILD of the
// placer entity that owns the spline and the recipe, so its Transform is
// parent-local and its label says which slot it fills.

// Placed pieces are children of the placer, so their Transform column is
// PARENT-LOCAL, while layout produces WORLD poses. TransformHierarchySystem
// computes World = Parent.World * Local, so Local = inverse(Parent.World) * Pose.
//
// Inverting once per rebuild keeps the per-piece cost at one 4x4 multiply.
// A placer whose world matrix is not invertible (a zeroed scale) inverts to
// non-finite values; identity is returned instead, so the pieces collapse with
// their parent rather than writing NaNs into the transform graph and on into
// bounds and culling.
Mathematics::Matrix4x4 InvertPlacerWorld(const float32* placerWorldMatrix);

// `axis` names which of the piece's own local horizontal axes runs along the
// path; it lands on the pose's Forward and the other lands across travel. Both
// recipes emit through here, so the axis a piece is LAID along is the axis its
// length was MEASURED on (SplineLayout/PieceAxis.h) and the two cannot drift.
//
// lengthScale stretches the piece along that axis (fence spans stretch to
// fill); 1.0f leaves the basis as laid out.
void WriteParentLocalPose(Components::Transform& out,
                          const Mathematics::Matrix4x4& invPlacerWorld,
                          const TilePose& pose,
                          PieceAxis axis,
                          float32 lengthScale);

// Hierarchy label for one generated piece, e.g. "Tile 12" / "Post 3" / "Span 3".
// A DISPLAY label only: it is ordinal, so a spacing change renumbers it. The
// override keys a later phase needs must be parameter-addressed instead (the
// composition design's slot roles), and nothing here is that.
Components::Name MakePieceLabel(const char* role, uint32 index);

} // namespace GameEngine::SplineLayout
