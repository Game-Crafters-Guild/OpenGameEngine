#pragma once

#include "Mathematics/Vector3.h"
#include "SplineLayout/PieceBasis.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::SplineLayout
{

// Signed yaw from a to b about +Y after projecting both onto the ground
// plane, in radians; positive = b turns right of a (this LH +Y-up engine: +X
// is the right of +Z travel). Near-vertical forwards carry no yaw demand, and
// non-finite ones carry none either: this is the last place that can reject
// them, because atan2 maps NaN to NaN, std::clamp passes NaN straight through,
// and atan2 of two infinities is a perfectly finite 45 degrees that reads
// downstream as a genuine turn.
float32 SignedGroundYaw(const Mathematics::Vector3& a, const Mathematics::Vector3& b);

// Per-tile parallelogram shear factors that soften rigid spline-placement
// seams: at each joint both neighbouring tiles shear by tan(yaw/2) toward the
// joint bisector, where yaw is the signed ground-plane angle between the
// tiles' forward directions. A tile sums the demands of its two joints and
// the result is clamped so the visible texture skew never exceeds
// `capDegrees`. This redistributes a bend's wedge across the flanking seams —
// it does not remove it (an affine shear cannot; per-turn wedge area is
// spacing-invariant).
//
// The returned factor s maps tile-local (x, z) to (x, z + s * x): apply it in
// the pose basis as Right' = Right + s * Forward.
// capDegrees <= 0 disables (all factors zero).
std::vector<float32> ComputeSeamShearFactors(const std::vector<Mathematics::Vector3>& forwards,
                                             float32 capDegrees);

// Applies those factors to the poses in place, with the coupled correction to
// the footprint-centering offset the layout already wrote.
//
// The bisector a seam closes on is WORLD geometry, so the sheared edge is
// `Right + s * Forward` no matter which of the piece's local axes happens to
// lie along it — a piece laid along its local X reaches that same world edge
// through its local +Z, which maps to -Right. Signing the shear by the axis
// instead would preserve the LOCAL lean and open the wedge it exists to close.
//
// `meshBoundsCenter` must be the SAME footprint the poses were laid out with,
// not the picked mesh's own: the correction cancels a term the layout wrote.
void ApplySeamShear(std::vector<TilePose>& poses,
                    const std::vector<float32>& shearFactors,
                    const Mathematics::Vector3& meshBoundsCenter,
                    PieceAxis axis);

} // namespace GameEngine::SplineLayout
