#include "SplineLayout/SpanMitre.h"

#include "SplineLayout/SeamShear.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace GameEngine::SplineLayout
{
namespace
{

using V3 = Mathematics::Vector3;

constexpr float32 kRadiansPerDeciDegree = Mathematics::Pi / 1800.0f;
constexpr float32 kMetresPerTenthMillimetre = 1.0e-4f;
constexpr float32 kLengthScaleStep = 1.0e-3f;
// Below this share of the plane's normal along travel the plane runs nearly
// parallel to the span, and no overhang reaches it.
constexpr float32 kMinAlongShare = 1.0e-3f;

int32 RoundToStep(float32 value, float32 step)
{
    return static_cast<int32>(std::lround(value / step));
}

// The plane in one span's frame, and how far that span's end must reach.
//
// A point laid at (x, y, a) in the frame — x along Right, y along Up from the
// base line, a along Forward from the base line's end — is past the plane when
// c.x*x + c.y*y + c.z*a > d, with c the plane's normal in the frame and d its
// offset there. Along travel the plane therefore stands at
//     a(x, y) = (d - c.x*x - c.y*y) / c.z.
// The end is laid at the furthest of a(x, y) over the four corners of the
// cross-section (x = +-halfThickness, y = Bottom or Top), and of a registered
// crest's on top of it, so it crosses the plane everywhere and the cut, not a
// short end, makes the face. On a racked
// span the frame is pitched and the plane plumb, so the top corners sit
// Top*tan(pitch) further along or back than the bottom ones: that is the reach
// a base-line overhang alone leaves short.
bool EndPlaneInFrame(const SpanJoinSide& span, const V3& outward, const V3& planePoint,
                     bool atStart, FenceEndPlane& outPlane, float32& outOverhangMetres)
{
    const TilePose& pose = span.Pose;
    V3 c(V3::Dot(outward, pose.Right), V3::Dot(outward, pose.Up), V3::Dot(outward, pose.Forward));
    float32 d = V3::Dot(outward, planePoint - span.BaseLineEnd);
    const float32 length = std::sqrt(V3::Dot(c, c));
    if (!(length > 0.0f) || !std::isfinite(length))
        return false;
    c = c * (1.0f / length);
    d /= length;
    const float32 along = atStart ? -c.z : c.z;
    if (!(along > kMinAlongShare))
        return false;
    outPlane.TurnRadians = 2.0f * std::asin(std::clamp(c.x, -1.0f, 1.0f));
    outPlane.LeanRadians = atStart ? std::atan2(c.y, -c.z) : std::atan2(-c.y, c.z);

    // Signed reach past the base line's end, positive away from the span.
    const float32 sign = atStart ? -1.0f : 1.0f;
    const auto reach = [&](float32 x, float32 y) { return sign * (d - c.x * x - c.y * y) / c.z; };
    float32 furthest = 0.0f;
    for (const float32 x : {-span.HalfThickness, span.HalfThickness})
    {
        for (const float32 y : {span.Bottom, span.Top})
            furthest = std::max(furthest, reach(x, y));
    }
    if (span.CrestHalfThickness > 0.0f)
    {
        for (const float32 x : {-span.CrestHalfThickness, span.CrestHalfThickness})
        {
            for (const float32 y : {span.CrestBottom, span.CrestTop})
                furthest = std::max(furthest, reach(x, y));
        }
    }
    outOverhangMetres = furthest;
    // From the extended end back to where the plane crosses the base line.
    outPlane.StationInset = furthest - reach(0.0f, 0.0f);
    return true;
}

// The plane's coefficients in the frame, rebuilt from a quantised end: the
// turn gives its share across travel, and the two points where it crosses the
// piece's centre line (at the base line and at the top, `alongRise` metres
// further along travel over `height`) give its tilt about Right. A variant is
// then a function of its key alone.
V3 EndPlaneCoefficients(const MitreEndKey& end, bool atStart, float32 height, float32 alongRise)
{
    const float32 halfTurn = 0.5f * static_cast<float32>(end.TurnDeciDegrees) * kRadiansPerDeciDegree;
    const float32 square = std::cos(halfTurn);
    const float32 side = std::sin(halfTurn);
    const float32 sign = atStart ? -1.0f : 1.0f;
    const float32 run = std::sqrt(height * height + alongRise * alongRise);
    if (!(height > kMinAlongShare) || !(run > 0.0f))
        return V3(side, 0.0f, sign * square);
    // Normal to the rise (0, height, alongRise) within the Up-Forward plane.
    const float32 forward = sign * square * height / run;
    return V3(side, -forward * alongRise / height, forward);
}

float32 LengthScaleOf(const MitreShape& shape)
{
    return static_cast<float32>(shape.LengthScaleThousandths) * kLengthScaleStep;
}

// A piece's own axes as the pose lays them: which local component runs along
// travel, which across it, and the sign that turns the across component into
// metres to the RIGHT of travel (PieceBasis.h: an X-laid piece meets Right
// through its local -Z).
struct PieceAxes
{
    int32 Along = 2;
    int32 Across = 0;
    float32 RightSign = 1.0f;
};

PieceAxes AxesOf(const FencePieceBounds& piece)
{
    if (piece.Axis() == PieceAxis::X)
        return {0, 2, -1.0f};
    return {2, 0, 1.0f};
}

MitreEndKey QuantizeEnd(const FenceEndPlane& plane)
{
    MitreEndKey key;
    if (!plane.Mitred)
        return key;
    key.Mitred = true;
    constexpr float32 kDegreesPerRadian = 180.0f / Mathematics::Pi;
    key.TurnDeciDegrees = RoundToStep(plane.TurnRadians * kDegreesPerRadian, 0.1f);
    key.InsetTenthMillimetres = RoundToStep(plane.StationInset, kMetresPerTenthMillimetre);
    key.TopInsetTenthMillimetres = RoundToStep(plane.TopInset, kMetresPerTenthMillimetre);
    if (key.TopInsetTenthMillimetres != key.InsetTenthMillimetres)
        key.StationLocalYTenthMillimetres =
            RoundToStep(plane.StationLocalY, kMetresPerTenthMillimetre);
    return key;
}

struct MitreVariantKeyHash
{
    size_t operator()(const MitreVariantKey& key) const noexcept
    {
        uint64 h = 0xCBF29CE484222325ull;
        const auto mix = [&h](int64 value)
        {
            h ^= static_cast<uint64>(value);
            h *= 0x100000001B3ull;
        };
        mix(static_cast<int64>(key.Role));
        mix(key.PoolSlot);
        mix(key.Shape.LengthScaleThousandths);
        for (const MitreEndKey* end : {&key.Shape.Start, &key.Shape.End})
        {
            mix(end->Mitred ? 1 : 0);
            mix(end->TurnDeciDegrees);
            mix(end->InsetTenthMillimetres);
            mix(end->TopInsetTenthMillimetres);
            mix(end->StationLocalYTenthMillimetres);
        }
        return static_cast<size_t>(h);
    }
};

// Assigns one piece its variant, adding the variant the first time its shape
// is met.
class VariantAssigner
{
public:
    explicit VariantAssigner(MitreVariantPlan& plan) : m_Plan(plan) {}

    uint32 Assign(MitrePieceRole role, uint32 poolSlot, float32 lengthScale,
                  const FenceEndPlane& start, const FenceEndPlane& end)
    {
        if (!start.Mitred && !end.Mitred)
            return kNoMitreVariant;
        const MitreVariantKey key{role, poolSlot, QuantizeMitreShape(lengthScale, start, end)};
        const auto found = m_Index.find(key);
        if (found != m_Index.end())
            return found->second;
        const uint32 index = static_cast<uint32>(m_Plan.Variants.size());
        m_Plan.Variants.push_back(key);
        m_Index.emplace(key, index);
        return index;
    }

private:
    MitreVariantPlan& m_Plan;
    std::unordered_map<MitreVariantKey, uint32, MitreVariantKeyHash> m_Index;
};

} // namespace

bool MeasureSpanJoin(const SpanJoinSide& before, const SpanJoinSide& after, const V3& station,
                     SpanJoinMeasure& out)
{
    const V3 none(0.0f, 0.0f, 0.0f);
    const V3 groundBefore =
        NormalizedOrFallback(V3(before.Pose.Forward.x, 0.0f, before.Pose.Forward.z), none);
    const V3 groundAfter =
        NormalizedOrFallback(V3(after.Pose.Forward.x, 0.0f, after.Pose.Forward.z), none);
    // Facing past the closing span.
    const V3 normal = NormalizedOrFallback(groundBefore + groundAfter, none);
    if (V3::Dot(normal, normal) < 0.5f)
        return false;

    SpanJoinMeasure measure;
    measure.TurnRadians = SignedGroundYaw(before.Pose.Forward, after.Pose.Forward);
    measure.NotchMetres = (before.HalfThickness + after.HalfThickness) *
                          std::tan(0.5f * std::abs(measure.TurnRadians));
    if (!EndPlaneInFrame(before, normal, station, false, measure.BeforeEnd,
                         measure.BeforeOverhangMetres) ||
        !EndPlaneInFrame(after, normal * -1.0f, station, true, measure.AfterStart,
                         measure.AfterOverhangMetres))
        return false;
    measure.BeforeEnd.Mitred = true;
    measure.AfterStart.Mitred = true;
    out = measure;
    return true;
}

MitreShape QuantizeMitreShape(float32 lengthScale, const FenceEndPlane& start,
                              const FenceEndPlane& end)
{
    MitreShape shape;
    shape.LengthScaleThousandths = RoundToStep(lengthScale, kLengthScaleStep);
    shape.Start = QuantizeEnd(start);
    shape.End = QuantizeEnd(end);
    return shape;
}

SplineGeometry::CutPlane MitreEndCutPlane(const FencePieceBounds& piece, const MitreShape& shape,
                                          bool atStart)
{
    const MitreEndKey& end = atStart ? shape.Start : shape.End;
    const PieceAxes axes = AxesOf(piece);
    const float32 scale = LengthScaleOf(shape);
    const float32 inset = static_cast<float32>(end.InsetTenthMillimetres) * kMetresPerTenthMillimetre;
    const float32 topInset =
        static_cast<float32>(end.TopInsetTenthMillimetres) * kMetresPerTenthMillimetre;
    const float32 stationY =
        static_cast<float32>(end.StationLocalYTenthMillimetres) * kMetresPerTenthMillimetre;
    const float32 top = piece.Center.y + piece.HalfExtents.y;
    // From the base line to the top the crossing moves along travel by the
    // difference of the insets, outward at a closing end for a smaller inset.
    const float32 alongRise = (atStart ? -1.0f : 1.0f) * scale * (inset - topInset);
    const V3 c = EndPlaneCoefficients(end, atStart, top - stationY, alongRise);
    // The station's position along travel from the piece's centre, in metres.
    const float32 stationAlong =
        (atStart ? -1.0f : 1.0f) * scale * (0.5f * piece.Length() - inset);
    const float32 centreAlong = (&piece.Center.x)[axes.Along];
    const float32 centreAcross = (&piece.Center.x)[axes.Across];

    // c.x * right + c.y * (y - stationY) + c.z * (along - stationAlong) = 0 in
    // the pose's metres, with right = RightSign * (across - centreAcross) and
    // along = scale * (localAlong - centreAlong), rewritten on local
    // coordinates.
    SplineGeometry::CutPlane plane;
    float32* normal = &plane.Normal.x;
    normal[axes.Along] = c.z * scale;
    normal[axes.Across] = c.x * axes.RightSign;
    normal[1] = c.y;
    plane.Offset = c.x * axes.RightSign * centreAcross + c.y * stationY +
                   c.z * (scale * centreAlong + stationAlong);
    return plane;
}

MitreVariant BuildMitreVariant(const SplineGeometry::PieceMesh& piece, const FencePieceBounds& bounds,
                               const MitreShape& shape)
{
    MitreVariant variant;
    variant.Mesh = piece;
    const bool ends[2] = {shape.Start.Mitred, shape.End.Mitred};
    for (int i = 0; i < 2; ++i)
    {
        if (!ends[i])
            continue;
        SplineGeometry::MeshCutResult cut = SplineGeometry::CutMeshByPlane(variant.Mesh, MitreEndCutPlane(bounds, shape, i == 0));
        variant.Mesh = std::move(cut.Mesh);
        variant.OpenLoops += cut.OpenLoops;
    }
    return variant;
}

MitreVariantPlan PlanMitreVariants(const FenceLayoutResult& layout)
{
    MitreVariantPlan plan;
    VariantAssigner assigner(plan);
    plan.SpanVariants.reserve(layout.Spans.size());
    for (const FenceSpan& span : layout.Spans)
    {
        plan.SpanVariants.push_back(assigner.Assign(span.IsGate ? MitrePieceRole::Gate
                                                                : MitrePieceRole::Span,
                                                    span.PoolSlot, span.LengthScale, span.Start,
                                                    span.End));
    }
    plan.CrestVariants.reserve(layout.Crests.size());
    for (const FenceCrest& crest : layout.Crests)
    {
        plan.CrestVariants.push_back(assigner.Assign(MitrePieceRole::Crest, crest.PoolSlot,
                                                     crest.LengthScale, crest.Start, crest.End));
    }
    return plan;
}

} // namespace GameEngine::SplineLayout
