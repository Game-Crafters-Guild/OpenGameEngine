#pragma once

#include "Mathematics/Vector3.h"
#include "SplineLayout/FenceLayout.h"
#include "SplineGeometry/MeshPlaneCut.h"
#include "SplineGeometry/PieceMesh.h"
#include "SplineLayout/TileLayout.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::SplineLayout
{

// Past this deflection a bare join keeps square ends and is reported: the
// extrusion design's mitre limit of 2.0 (a mitre twice the wall's thickness)
// stated as the turn that produces it.
inline constexpr float32 kMitreLimitDegrees = 120.0f;

// One span's side of a join: its frame, the point where its base line meets
// the station, and the extent of what is laid on it, in the frame's metres.
struct SpanJoinSide
{
    TilePose Pose;
    // The span's base line at the station: the station base for a racked or
    // sheared span; for a stepped span the point level with its lower
    // station, straight below or above the station base.
    Mathematics::Vector3 BaseLineEnd{};
    // Half the piece's extent across travel.
    float32 HalfThickness = 0.0f;
    // The piece's lowest and highest point along the frame's Up from the base
    // line.
    float32 Bottom = 0.0f;
    float32 Top = 0.0f;
    // The same for a crest registered to the span, which carries the span's
    // end planes and so must reach them across its own thickness. A zero
    // half-thickness is no crest.
    float32 CrestHalfThickness = 0.0f;
    float32 CrestBottom = 0.0f;
    float32 CrestTop = 0.0f;
};

// The two end planes of one join where span `before` closes and span `after`
// opens, measured from the two spans' frames.
//
// The plane is vertical, through the station, perpendicular to the ground
// bisector of the two spans' forwards. For two equally thick pieces whose side
// faces stand plumb (racked, stepped and sheared frames keep Right level where
// the stations stand plumb) it is exactly the plane holding the lines where
// their outer faces and their inner faces cross, so cut on it the outer faces
// meet on one edge and the inner faces on another. For unequal thicknesses
// those two crossings lie on no common plane, and the plane through them runs
// off along the spans as the turn straightens ((h1 - h2) / sin(turn)); the
// bisector keeps every reach within the thicker piece's
// halfThickness * tan(turn / 2) over its lean, and the thinner piece's faces
// meet the thicker one's with the step their thicknesses make.
struct SpanJoinMeasure
{
    // Signed ground turn from before to after, positive to the right.
    float32 TurnRadians = 0.0f;
    // Gap the two outer faces leave when both ends stay square: each face
    // stops half its span's thickness times tan(|turn| / 2) short.
    float32 NotchMetres = 0.0f;
    // The planes in each span's own frame. StationInset holds the distance,
    // in the frame's metres, from the span's extended end back to where the
    // plane crosses its base line; the caller divides it by the final
    // LengthScale and sets StationLocalY from the planting.
    FenceEndPlane BeforeEnd;
    FenceEndPlane AfterStart;
    // How far past its base line's end each span is laid so that its end
    // crosses the plane at every corner of its cross-section, top as well as
    // bottom (SpanEndReach).
    float32 BeforeOverhangMetres = 0.0f;
    float32 AfterOverhangMetres = 0.0f;
};

// `station` is the join's station base. False when either span has no ground
// direction, the two run back on each other, or either frame has no usable
// forward along the plane (a span standing on end).
bool MeasureSpanJoin(const SpanJoinSide& before, const SpanJoinSide& after,
                     const Mathematics::Vector3& station, SpanJoinMeasure& out);

// One end plane rounded to the steps at which two joins share one mesh: 0.1
// degree of turn, 0.1 mm of inset at the base line and at the piece's top, and
// 0.1 mm of the base line's height. A square end is all zeros, and a plane
// that does not lean (the two insets equal) ignores the base line's height.
//
// The lean is carried as the two insets, not as an angle: the rebuilt plane
// passes within 0.05 mm of the true one at the base line and at the top, so
// it is within 0.05 mm everywhere between them, whatever the piece's height;
// the turn's 0.05 degree adds at most halfThickness * tan(0.05 degrees)
// (0.22 mm across a 0.5 m wall). An angle step would grow with the height:
// 0.05 degrees is 4.4 mm at the top of a 5 m wall.
struct MitreEndKey
{
    int32 TurnDeciDegrees = 0;
    int32 InsetTenthMillimetres = 0;
    int32 TopInsetTenthMillimetres = 0;
    int32 StationLocalYTenthMillimetres = 0;
    bool Mitred = false;

    bool operator==(const MitreEndKey&) const = default;
};

// Everything a piece's cut variant is a function of besides the piece
// itself. Its length scale is rounded to 1e-3, so the equal spans of a
// uniform arc share one variant and a knot drag that holds these values
// re-uses it.
struct MitreShape
{
    int32 LengthScaleThousandths = 1000;
    MitreEndKey Start;
    MitreEndKey End;

    bool IsMitred() const { return Start.Mitred || End.Mitred; }
    bool operator==(const MitreShape&) const = default;
};

MitreShape QuantizeMitreShape(float32 lengthScale, const FenceEndPlane& start,
                              const FenceEndPlane& end);

// The plane one end of a shape cuts the piece on, in the piece's local frame:
// its positive side is past the join.
SplineGeometry::CutPlane MitreEndCutPlane(const FencePieceBounds& piece, const MitreShape& shape, bool atStart);

struct MitreVariant
{
    SplineGeometry::PieceMesh Mesh;
    // Cut chains that did not close; see SplineGeometry::MeshCutResult.
    uint32 OpenLoops = 0;
};

// The piece cut for its shape: what lies past each mitred end's plane is
// removed and the cut is capped (SplineGeometry/MeshPlaneCut.h).
MitreVariant BuildMitreVariant(const SplineGeometry::PieceMesh& piece, const FencePieceBounds& bounds,
                               const MitreShape& shape);

inline constexpr uint32 kNoMitreVariant = UINT32_MAX;

// Which pool a variant's piece is drawn from; with the pool slot it names the
// mesh within one recipe.
enum class MitrePieceRole : uint8
{
    Span,
    Crest,
    Gate,
};

struct MitreVariantKey
{
    MitrePieceRole Role = MitrePieceRole::Span;
    uint32 PoolSlot = 0;
    MitreShape Shape;

    bool operator==(const MitreVariantKey&) const = default;
};

struct MitreVariantPlan
{
    // Distinct variants in first-use order: the spans in emission order, then
    // the crests.
    std::vector<MitreVariantKey> Variants;
    // Per emitted span and crest: its index in Variants, or kNoMitreVariant to
    // draw the piece as modelled.
    std::vector<uint32> SpanVariants;
    std::vector<uint32> CrestVariants;
};

// Which variant every mitred span and registered crest draws. Equal shapes of
// one piece share a variant, so the equal spans of a uniform arc stay
// instanced.
MitreVariantPlan PlanMitreVariants(const FenceLayoutResult& layout);

} // namespace GameEngine::SplineLayout
