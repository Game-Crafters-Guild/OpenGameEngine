#include "Components/Transform.h"
#include "SplineLayout/FenceLayout.h"
#include "SplineLayout/PieceEntity.h"
#include "SplineLayout/SpanMitre.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace GameEngine;
using SplineLayout::BuildFenceLayout;
using SplineLayout::BuildMitreVariant;
using SplineLayout::CenterSample;
using SplineGeometry::CutPlane;
using SplineLayout::FenceEndPlane;
using SplineLayout::FenceLayoutParams;
using SplineLayout::FenceLayoutResult;
using SplineLayout::FencePieceBounds;
using SplineLayout::FenceSpan;
using SplineLayout::MitreEndCutPlane;
using SplineLayout::MitreShape;
using SplineLayout::MitreVariant;
using SplineGeometry::PieceMesh;
using SplineGeometry::SplineVertex;
using SplineLayout::QuantizeMitreShape;
using SplineLayout::WriteParentLocalPose;
using V2 = Mathematics::Vector2;
using V3 = Mathematics::Vector3;

namespace
{

constexpr float32 kPi = 3.14159265358979f;
// How far a cut end may land off its plane: half a millimetre, the bar the
// fence design sets for the joins (section 4c).
constexpr float32 kOnPlaneMetres = 5.0e-4f;

// The castle kit's 5 m wall: 5 x 5 x 0.5 m, laid along local X and pivoted on
// its corner (FenceLayoutTests' CastleWall()).
FencePieceBounds CastleWall()
{
    FencePieceBounds b;
    b.HalfExtents = V3(2.5f, 2.5f, 0.25f);
    b.Center = V3(-2.5f, 2.5f, 0.0f);
    return b;
}

// A 3 m panel laid along local Z, centred, 0.4 m thick.
FencePieceBounds ZPanel()
{
    FencePieceBounds b;
    b.HalfExtents = V3(0.2f, 1.0f, 1.5f);
    b.Center = V3(0.0f, 1.0f, 0.0f);
    return b;
}

void AddQuad(PieceMesh& mesh, const V3& a, const V3& b, const V3& c, const V3& d, const V3& normal)
{
    const uint32 base = static_cast<uint32>(mesh.Vertices.size());
    for (const V3& p : {a, b, c, d})
    {
        SplineVertex v;
        v.Position = p;
        v.Normal = normal;
        v.UV = V2(p.x, p.y);
        mesh.Vertices.push_back(v);
    }
    if (V3::Dot(V3::Cross(b - a, c - a), normal) > 0.0f)
        mesh.Indices.insert(mesh.Indices.end(), {base, base + 1u, base + 2u, base, base + 2u, base + 3u});
    else
        mesh.Indices.insert(mesh.Indices.end(), {base, base + 2u, base + 1u, base, base + 3u, base + 2u});
}

// The piece's bounds as a closed box mesh, subdivided along its length the way
// a modelled wall carries interior edges for a cut to cross.
PieceMesh BoxOf(const FencePieceBounds& bounds, uint32 segments = 4u)
{
    const V3 lo = bounds.Center - bounds.HalfExtents;
    const V3 hi = bounds.Center + bounds.HalfExtents;
    const int along = bounds.Axis() == SplineLayout::PieceAxis::X ? 0 : 2;
    PieceMesh mesh;
    const auto at = [&](float32 t, float32 y, float32 across)
    {
        V3 p;
        (&p.x)[along] = (&lo.x)[along] + t * ((&hi.x)[along] - (&lo.x)[along]);
        p.y = y;
        (&p.x)[2 - along] = across;
        return p;
    };
    const float32 acrossLo = (&lo.x)[2 - along];
    const float32 acrossHi = (&hi.x)[2 - along];
    V3 acrossNormal(0, 0, 0);
    (&acrossNormal.x)[2 - along] = 1.0f;
    for (uint32 s = 0; s < segments; ++s)
    {
        const float32 t0 = static_cast<float32>(s) / static_cast<float32>(segments);
        const float32 t1 = static_cast<float32>(s + 1u) / static_cast<float32>(segments);
        AddQuad(mesh, at(t0, lo.y, acrossLo), at(t1, lo.y, acrossLo), at(t1, hi.y, acrossLo),
                at(t0, hi.y, acrossLo), acrossNormal * -1.0f);
        AddQuad(mesh, at(t0, lo.y, acrossHi), at(t1, lo.y, acrossHi), at(t1, hi.y, acrossHi),
                at(t0, hi.y, acrossHi), acrossNormal);
        AddQuad(mesh, at(t0, lo.y, acrossLo), at(t1, lo.y, acrossLo), at(t1, lo.y, acrossHi),
                at(t0, lo.y, acrossHi), V3(0, -1, 0));
        AddQuad(mesh, at(t0, hi.y, acrossLo), at(t1, hi.y, acrossLo), at(t1, hi.y, acrossHi),
                at(t0, hi.y, acrossHi), V3(0, 1, 0));
    }
    V3 alongNormal(0, 0, 0);
    (&alongNormal.x)[along] = 1.0f;
    for (const float32 t : {0.0f, 1.0f})
        AddQuad(mesh, at(t, lo.y, acrossLo), at(t, lo.y, acrossHi), at(t, hi.y, acrossHi),
                at(t, hi.y, acrossLo), alongNormal * (t == 0.0f ? -1.0f : 1.0f));
    return mesh;
}

// A bare authored corner: two straight legs meeting at the origin, the first
// heading +Z into it, the second leaving it turned `degrees` to the right.
std::vector<CenterSample> Corner(float32 leg, float32 degrees, float32 step)
{
    std::vector<CenterSample> center;
    const uint32 steps = static_cast<uint32>(std::lround(leg / step));
    for (uint32 i = 0; i <= steps; ++i)
        center.push_back({V3(0.0f, 0.0f, -leg + static_cast<float32>(i) * step), V3(0, 1, 0), true});
    const float32 turn = degrees * kPi / 180.0f;
    const V3 out(std::sin(turn), 0.0f, std::cos(turn));
    for (uint32 i = 1; i <= steps; ++i)
        center.push_back({out * (static_cast<float32>(i) * step), V3(0, 1, 0), true});
    return center;
}

FenceLayoutResult BareCorner(const FencePieceBounds& piece, float32 degrees)
{
    const std::vector<CenterSample> center = Corner(piece.Length(), degrees, 0.05f);
    const float32 last = static_cast<float32>(center.size() - 1u);
    const float32 boundaries[3] = {0.0f, last * 0.5f, last};
    const FencePieceBounds spans[1] = {piece};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = spans;
    return BuildFenceLayout(center, params);
}

// Local to world through the transform the controller emits.
V3 ToWorld(const FenceSpan& span, const FencePieceBounds& piece, const V3& local)
{
    static const float32 kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Components::Transform t{};
    WriteParentLocalPose(t, SplineLayout::InvertPlacerWorld(kIdentity), span.Pose, piece.Axis(),
                         span.LengthScale);
    const float32* m = t.matrix;
    return V3(m[0] * local.x + m[4] * local.y + m[8] * local.z + m[12],
              m[1] * local.x + m[5] * local.y + m[9] * local.z + m[13],
              m[2] * local.x + m[6] * local.y + m[10] * local.z + m[14]);
}

float32 SignedDistance(const CutPlane& plane, const V3& p)
{
    return (V3::Dot(plane.Normal, p) - plane.Offset) / plane.Normal.Length();
}

// The world join plane at the corner: vertical through the station at the
// origin, perpendicular to the bisector of the two legs.
V3 BisectorOf(float32 degrees)
{
    const float32 turn = degrees * kPi / 180.0f;
    return (V3(0, 0, 1) + V3(std::sin(turn), 0, std::cos(turn))).Normalize();
}

} // namespace

// The plane rebuilt from a quantised end passes through the station, which a
// mitred span holds `StationInset` inside its modelled end — for both the
// piece axes a kit models on.
TEST(SpanMitre, TheCutPlaneOfAQuantisedEndPassesThroughItsStation)
{
    for (const FencePieceBounds& piece : {CastleWall(), ZPanel()})
    {
        const FenceLayoutResult layout = BareCorner(piece, 30.0f);
        ASSERT_EQ(layout.Spans.size(), 2u);
        const FenceSpan& before = layout.Spans[0];
        const FenceSpan& after = layout.Spans[1];
        ASSERT_TRUE(before.End.Mitred);
        ASSERT_TRUE(after.Start.Mitred);

        for (const bool atStart : {false, true})
        {
            const FenceSpan& span = atStart ? after : before;
            const MitreShape shape = QuantizeMitreShape(span.LengthScale, span.Start, span.End);
            const CutPlane plane = MitreEndCutPlane(piece, shape, atStart);
            // The station at the origin, in the piece's local frame: on its
            // centreline, `inset` inside its end, at the station's height.
            const FenceEndPlane& end = atStart ? span.Start : span.End;
            const int along = piece.Axis() == SplineLayout::PieceAxis::X ? 0 : 2;
            V3 station = piece.Center;
            station.y = end.StationLocalY;
            (&station.x)[along] += (atStart ? -1.0f : 1.0f) *
                                    (0.5f * piece.Length() - end.StationInset);
            EXPECT_NEAR(SignedDistance(plane, station), 0.0f, 1e-4f);
            EXPECT_LT((ToWorld(span, piece, station) - V3(0, 0, 0)).Length(), 1e-3f)
                << "the local station is not the world station";
        }
    }
}

// End to end at a 90 degree bare corner: the layout's planes, quantised, cut
// both spans; in the world every surviving end vertex lies on the one join
// plane, and the two spans' outer faces meet on one edge.
TEST(SpanMitre, ACutCornerMeetsOnOnePlaneAndCloses)
{
    for (const FencePieceBounds& piece : {CastleWall(), ZPanel()})
    {
        const FenceLayoutResult layout = BareCorner(piece, 90.0f);
        ASSERT_EQ(layout.Spans.size(), 2u);
        const V3 bisector = BisectorOf(90.0f);
        const PieceMesh box = BoxOf(piece);
        float32 outerCornerX[2] = {0.0f, 0.0f};
        for (int s = 0; s < 2; ++s)
        {
            const FenceSpan& span = layout.Spans[static_cast<size_t>(s)];
            const MitreShape shape = QuantizeMitreShape(span.LengthScale, span.Start, span.End);
            const MitreVariant variant = BuildMitreVariant(box, piece, shape);
            EXPECT_EQ(variant.OpenLoops, 0u);
            float32 farthest = -1e9f;
            for (const SplineVertex& v : variant.Mesh.Vertices)
            {
                const V3 world = ToWorld(span, piece, v.Position);
                // Nothing of either span crosses the join plane.
                const float32 past = s == 0 ? V3::Dot(world, bisector) : -V3::Dot(world, bisector);
                EXPECT_LE(past, kOnPlaneMetres);
                farthest = std::max(farthest, past);
                // The outer (left) corner at the plane: the most negative x.
                if (std::abs(past) < kOnPlaneMetres)
                    outerCornerX[s] = std::min(outerCornerX[s], world.x);
            }
            EXPECT_NEAR(farthest, 0.0f, kOnPlaneMetres) << "span " << s << " does not reach the plane";
        }
        // Both outer faces reach the same outer corner: no notch.
        EXPECT_NEAR(outerCornerX[0], outerCornerX[1], 1e-3f);
        // The first leg heads +Z into the corner and the second leaves along +X,
        // so the outside is -X and the outer faces meet at x = -halfThickness.
        EXPECT_NEAR(outerCornerX[0], -piece.HalfThickness(), 1e-3f);
    }
}

// A span with square ends is handed back as modelled.
TEST(SpanMitre, AnUnmitredShapeLeavesThePieceUntouched)
{
    const FencePieceBounds piece = ZPanel();
    const PieceMesh box = BoxOf(piece);
    const MitreShape square = QuantizeMitreShape(1.1f, FenceEndPlane{}, FenceEndPlane{});
    EXPECT_FALSE(square.IsMitred());
    const MitreVariant variant = BuildMitreVariant(box, piece, square);
    ASSERT_EQ(variant.Mesh.Vertices.size(), box.Vertices.size());
    EXPECT_EQ(variant.Mesh.Indices, box.Indices);
}

namespace
{

// The castle kit's 5 m battlement: 1 m tall, laid along local X and
// corner-pivoted like the wall it stands on.
FencePieceBounds CastleBattlement()
{
    FencePieceBounds b;
    b.HalfExtents = V3(2.5f, 0.5f, 0.25f);
    b.Center = V3(-2.5f, 0.5f, 0.0f);
    return b;
}

// Two 10 m legs climbing at `pitchDegrees` the whole way, meeting at a bare
// authored point that turns them `turnDegrees` to the right.
std::vector<CenterSample> ClimbingTurn(float32 pitchDegrees, float32 turnDegrees)
{
    constexpr float32 kLeg = 10.0f;
    constexpr uint32 kSteps = 200u;
    const float32 grade = std::tan(pitchDegrees * kPi / 180.0f);
    const float32 turn = turnDegrees * kPi / 180.0f;
    const V3 first(0.0f, 0.0f, 1.0f);
    const V3 second(std::sin(turn), 0.0f, std::cos(turn));
    std::vector<CenterSample> center;
    for (uint32 i = 0; i <= 2u * kSteps; ++i)
    {
        const float32 walked = kLeg * static_cast<float32>(i) / static_cast<float32>(kSteps);
        const V3 plan = walked <= kLeg ? first * walked : first * kLeg + second * (walked - kLeg);
        center.push_back({V3(plan.x, grade * walked, plan.z), V3(0, 1, 0), true});
    }
    return center;
}

// How far the cut end of `piece`, laid by `pose` at `lengthScale` and cut to
// `start`/`end`, stands off the plane through `station` with `normal`, at the
// ends of its four long edges: both sides, bottom and top. A cut end lies on
// its plane over its whole height; an end that stops short of it does not.
float32 CutEndOffPlane(const FencePieceBounds& piece, const SplineLayout::TilePose& pose,
                       float32 lengthScale, const FenceEndPlane& start, const FenceEndPlane& end,
                       bool atStart, const V3& station, const V3& normal)
{
    const MitreShape shape = QuantizeMitreShape(lengthScale, start, end);
    const MitreVariant variant = BuildMitreVariant(BoxOf(piece), piece, shape);
    FenceSpan span;
    span.Pose = pose;
    span.LengthScale = lengthScale;
    const int along = piece.Axis() == SplineLayout::PieceAxis::X ? 0 : 2;
    const int across = 2 - along;
    const V3 lo = piece.Center - piece.HalfExtents;
    const V3 hi = piece.Center + piece.HalfExtents;
    float32 worst = 0.0f;
    for (const float32 side : {(&lo.x)[across], (&hi.x)[across]})
    {
        for (const float32 height : {lo.y, hi.y})
        {
            bool found = false;
            V3 extreme;
            for (const SplineVertex& v : variant.Mesh.Vertices)
            {
                if (std::abs((&v.Position.x)[across] - side) > 1e-4f ||
                    std::abs(v.Position.y - height) > 1e-4f)
                    continue;
                const bool further = atStart ? (&v.Position.x)[along] < (&extreme.x)[along]
                                             : (&v.Position.x)[along] > (&extreme.x)[along];
                if (!found || further)
                    extreme = v.Position;
                found = true;
            }
            if (!found)
                return 1e9f;
            const V3 world = ToWorld(span, piece, extreme);
            worst = std::max(worst, std::abs(V3::Dot(world - station, normal)));
        }
    }
    return worst;
}

} // namespace

// Every cut end reaches its join plane over its full height, on every grade.
// Cut on the same plane is necessary but not sufficient: a racked piece's end
// is square to its pitched chord, so an end laid out to the plane only at its
// base line stops short of it at the top by the height times tan(pitch) and
// opens a wedge; and a plane rebuilt from a rounded key has to land within a
// millimetre of the true one at the top of the piece, not just at its foot.
// Walls and the registered battlements on them, both the closing and the
// opening end of the join, climbing and descending through a sweep of grades
// that does not favour round angles, and turns of 1.5, 12, 30 and 90 degrees;
// the cap is raised so every join is mitred (the cap's own rule is tested
// below).
TEST(SpanMitre, EveryCutEndReachesItsPlaneOverItsFullHeight)
{
    const FencePieceBounds walls[1] = {CastleWall()};
    const FencePieceBounds crests[1] = {CastleBattlement()};
    std::vector<float32> pitches;
    for (float32 pitch = -20.0f; pitch <= 20.0f; pitch += 0.73f)
        pitches.push_back(pitch);
    for (const Components::SplineSpanGrade grade :
         {Components::SplineSpanGrade::Racked, Components::SplineSpanGrade::Stepped,
          Components::SplineSpanGrade::Sheared})
    {
        for (const float32 pitch : pitches)
        {
            for (const float32 turn : {1.5f, 12.0f, 30.0f, 90.0f})
            {
                const std::vector<CenterSample> center = ClimbingTurn(pitch, turn);
                const float32 last = static_cast<float32>(center.size() - 1u);
                const float32 boundaries[3] = {0.0f, last * 0.5f, last};
                FenceLayoutParams params;
                params.RunBoundaries = boundaries;
                params.SpanPieces = walls;
                params.CrestPieces = crests;
                params.SpanGrade = grade;
                params.SpanMaxStretch = 2.0f;
                const FenceLayoutResult result = BuildFenceLayout(center, params);
                const std::string what = "grade " + std::to_string(static_cast<int>(grade)) +
                                         ", pitch " + std::to_string(pitch) + ", turn " +
                                         std::to_string(turn);
                ASSERT_EQ(result.Spans.size(), 4u) << what;
                ASSERT_EQ(result.Crests.size(), 4u) << what;
                const FenceSpan& before = result.Spans[1];
                const FenceSpan& after = result.Spans[2];
                ASSERT_TRUE(before.End.Mitred && after.Start.Mitred) << what;
                const V3 station = result.Stations[2].Pose.Base;
                const V3 a = V3(before.Pose.Forward.x, 0.0f, before.Pose.Forward.z).Normalize();
                const V3 b = V3(after.Pose.Forward.x, 0.0f, after.Pose.Forward.z).Normalize();
                const V3 normal = (a + b).Normalize();
                EXPECT_LE(CutEndOffPlane(CastleWall(), before.Pose, before.LengthScale,
                                         before.Start, before.End, false, station, normal),
                          1e-3f)
                    << what << ", closing wall";
                EXPECT_LE(CutEndOffPlane(CastleWall(), after.Pose, after.LengthScale, after.Start,
                                         after.End, true, station, normal),
                          1e-3f)
                    << what << ", opening wall";
                const auto& closing = result.Crests[1];
                const auto& opening = result.Crests[2];
                EXPECT_LE(CutEndOffPlane(CastleBattlement(), closing.Pose, closing.LengthScale,
                                         closing.Start, closing.End, false, station, normal),
                          1e-3f)
                    << what << ", closing crest";
                EXPECT_LE(CutEndOffPlane(CastleBattlement(), opening.Pose, opening.LengthScale,
                                         opening.Start, opening.End, true, station, normal),
                          1e-3f)
                    << what << ", opening crest";
            }
        }
    }
}

// The reach is a stretch, so the cap governs it: a racked 20 degree climb
// through a 90 degree bare corner needs its closing wall about 35 % longer to
// cross the plane at its top, past the default 1.25x cap, so the corner keeps
// square ends and says why; at 3 degrees the same corner mitres.
TEST(SpanMitre, AReachPastTheStretchCapKeepsSquareEndsAndIsReported)
{
    const FencePieceBounds walls[1] = {CastleWall()};
    const auto build = [&](float32 pitch)
    {
        const std::vector<CenterSample> center = ClimbingTurn(pitch, 90.0f);
        const float32 last = static_cast<float32>(center.size() - 1u);
        const float32 boundaries[3] = {0.0f, last * 0.5f, last};
        FenceLayoutParams params;
        params.RunBoundaries = boundaries;
        params.SpanPieces = walls;
        return BuildFenceLayout(center, params);
    };
    const FenceLayoutResult steep = build(20.0f);
    ASSERT_EQ(steep.Spans.size(), 4u);
    EXPECT_FALSE(steep.Spans[1].End.Mitred);
    EXPECT_FALSE(steep.Spans[2].Start.Mitred);
    EXPECT_LE(steep.Spans[1].LengthScale, 1.25f);
    const auto reported = std::count_if(steep.Validation.begin(), steep.Validation.end(),
                                        [](const std::string& m)
                                        {
                                            return m.find("past the 1.250x cap") != std::string::npos &&
                                                   m.find("point 1") != std::string::npos;
                                        });
    EXPECT_EQ(reported, 1);

    const FenceLayoutResult gentle = build(3.0f);
    EXPECT_TRUE(gentle.Spans[1].End.Mitred);
    EXPECT_TRUE(gentle.Spans[2].Start.Mitred);
}

// Pieces of unequal thickness meet on the bisector plane, so near straight
// their reach stays within the thicker piece's halfThickness * tan(turn / 2):
// the plane through the crossings of two unequal pairs of side faces would run
// off along the spans as (h1 - h2) / sin(turn) (22.9 m at half a degree for a
// 0.5 m wall against a 0.1 m piece).
TEST(SpanMitre, UnequalThicknessesMeetOnTheBisectorNearStraight)
{
    for (const float32 degrees : {0.5f, 2.0f, 10.0f, 30.0f})
    {
        const float32 turn = degrees * kPi / 180.0f;
        SplineLayout::SpanJoinSide before;
        before.Pose.Forward = V3(0.0f, 0.0f, 1.0f);
        before.Pose.Up = V3(0.0f, 1.0f, 0.0f);
        before.Pose.Right = V3(1.0f, 0.0f, 0.0f);
        before.HalfThickness = 0.25f;
        before.Top = 5.0f;
        SplineLayout::SpanJoinSide after = before;
        after.Pose.Forward = V3(std::sin(turn), 0.0f, std::cos(turn));
        after.Pose.Right = V3(std::cos(turn), 0.0f, -std::sin(turn));
        after.HalfThickness = 0.05f;
        SplineLayout::SpanJoinMeasure measure;
        ASSERT_TRUE(SplineLayout::MeasureSpanJoin(before, after, V3(0, 0, 0), measure)) << degrees;
        const float32 bound = 0.25f * std::tan(0.5f * turn) + 1e-5f;
        EXPECT_LE(measure.BeforeOverhangMetres, bound) << degrees;
        EXPECT_LE(measure.AfterOverhangMetres, bound) << degrees;
    }
}

// A crest wider than its wall carries the wall's end planes, so the reach is
// laid to the crest's own corners too: a 0.35 m half-thick coping on a 0.25 m
// wall, laid to the wall's corners only, falls 25.9 mm short of the plane at
// 30 degrees and 70.7 mm at 90.
TEST(SpanMitre, ACrestWiderThanItsWallReachesThePlane)
{
    FencePieceBounds wide = CastleBattlement();
    wide.HalfExtents.z = 0.35f;
    const FencePieceBounds walls[1] = {CastleWall()};
    const FencePieceBounds crests[1] = {wide};
    for (const float32 turn : {30.0f, 90.0f})
    {
        const std::vector<CenterSample> center = Corner(CastleWall().Length(), turn, 0.05f);
        const float32 last = static_cast<float32>(center.size() - 1u);
        const float32 boundaries[3] = {0.0f, last * 0.5f, last};
        FenceLayoutParams params;
        params.RunBoundaries = boundaries;
        params.SpanPieces = walls;
        params.CrestPieces = crests;
        const FenceLayoutResult result = BuildFenceLayout(center, params);
        ASSERT_EQ(result.Spans.size(), 2u) << turn;
        ASSERT_EQ(result.Crests.size(), 2u) << turn;
        const V3 station = result.Stations[1].Pose.Base;
        const V3 normal = BisectorOf(turn);
        for (size_t k = 0; k < 2u; ++k)
        {
            const bool atStart = k == 1u;
            const auto& crest = result.Crests[k];
            ASSERT_TRUE(atStart ? crest.Start.Mitred : crest.End.Mitred) << turn;
            EXPECT_LE(CutEndOffPlane(wide, crest.Pose, crest.LengthScale, crest.Start, crest.End,
                                     atStart, station, normal),
                      1e-3f)
                << turn << " crest " << k;
            const FenceSpan& span = result.Spans[k];
            EXPECT_LE(CutEndOffPlane(CastleWall(), span.Pose, span.LengthScale, span.Start, span.End,
                                     atStart, station, normal),
                      1e-3f)
                << turn << " span " << k;
        }
    }
}

// Two 6.5 m legs of 5 m walls: one wall at 1.3x or two at 0.65x, neither within
// the 1.25x cap or its 0.8x floor, so the fill reports and is built at 1.3x.
// The bare 30 degree corner between them adds about 1 % of reach; the cap is
// not the join's to enforce again, so the join stays mitred and nothing
// further is reported.
TEST(SpanMitre, AFillAlreadyPastTheCapKeepsItsJoinsMitredAndUnreported)
{
    const FencePieceBounds walls[1] = {CastleWall()};
    const std::vector<CenterSample> center = Corner(6.5f, 30.0f, 0.05f);
    const float32 last = static_cast<float32>(center.size() - 1u);
    const float32 boundaries[3] = {0.0f, last * 0.5f, last};
    FenceLayoutParams params;
    params.RunBoundaries = boundaries;
    params.SpanPieces = walls;
    const FenceLayoutResult result = BuildFenceLayout(center, params);
    ASSERT_EQ(result.Spans.size(), 2u);
    EXPECT_GT(result.Spans[0].LengthScale, 1.25f);
    EXPECT_TRUE(result.Spans[0].End.Mitred);
    EXPECT_TRUE(result.Spans[1].Start.Mitred);
    size_t mitring = 0;
    for (const std::string& message : result.Validation)
        mitring += message.find("mitring") != std::string::npos ? 1u : 0u;
    EXPECT_EQ(mitring, 0u);
    EXPECT_FALSE(result.Validation.empty()) << "the fill past the cap still reports";
}
