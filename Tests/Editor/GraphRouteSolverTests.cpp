// The router's contract is what the canvas cannot check per frame: the route
// it hands back never passes through a node body, and among the routes that
// don't, it is the short one.

#include <gtest/gtest.h>

#include "Graph/GraphRouteSolver.h"

#include "Mathematics/Rect.h"
#include "Mathematics/Vector2.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace GameEngine::GraphRouting;
using GameEngine::Mathematics::Rect;
using GameEngine::Mathematics::Vector2;

namespace
{
/// Test data reads as edges; the router takes a rect and a policy.
Obstacle Body(float left, float top, float right, float bottom, float clearance = -1.f,
              bool portRowsExempt = false)
{
    return Obstacle{Rect{left, top, right - left, bottom - top}, clearance, portRowsExempt};
}
} // namespace

namespace
{

RouteRequest Request(float startX, float startY, float endX, float endY)
{
    RouteRequest request;
    request.Start = {startX, startY};
    request.End = {endX, endY};
    request.ExitX = startX + 14.f;
    request.EnterX = endX - 14.f;
    request.Clearance = 16.f;
    return request;
}

float RouteLength(const std::vector<Vector2>& points)
{
    float length = 0.f;
    for (size_t i = 1; i < points.size(); ++i)
        length += std::abs(points[i].x - points[i - 1].x) + std::abs(points[i].y - points[i - 1].y);
    return length;
}

// Every point but the endpoints must sit outside the body itself; the segments
// between them must not cross it either.
bool RouteEntersBody(const std::vector<Vector2>& points, const Obstacle& body)
{
    for (size_t i = 1; i < points.size(); ++i)
    {
        const float xMin = std::min(points[i - 1].x, points[i].x);
        const float xMax = std::max(points[i - 1].x, points[i].x);
        const float yMin = std::min(points[i - 1].y, points[i].y);
        const float yMax = std::max(points[i - 1].y, points[i].y);
        constexpr float kTouchTolerance = 0.5f;
        if (xMax > body.Body.X + kTouchTolerance && xMin < body.Body.Right() - kTouchTolerance &&
            yMax > body.Body.Y + kTouchTolerance && yMin < body.Body.Bottom() - kTouchTolerance)
            return true;
    }
    return false;
}

bool IsOrthogonal(const std::vector<Vector2>& points)
{
    for (size_t i = 1; i < points.size(); ++i)
    {
        if (std::abs(points[i].x - points[i - 1].x) > 0.5f &&
            std::abs(points[i].y - points[i - 1].y) > 0.5f)
            return false;
    }
    return true;
}

} // namespace

TEST(GraphRouteSolver, AnEmptyRunIsTheDirectRoute)
{
    std::vector<Vector2> points;
    ASSERT_TRUE(SolveOrthogonalRoute(Request(0.f, 0.f, 300.f, 0.f), {}, points));
    EXPECT_TRUE(IsOrthogonal(points));
    EXPECT_NEAR(RouteLength(points), 300.f, 0.5f);
}

TEST(GraphRouteSolver, ALevelRunWithABodyInTheWayGoesAroundIt)
{
    const Obstacle body = Body(100.f, -40.f, 200.f, 40.f);
    std::vector<Vector2> points;
    ASSERT_TRUE(SolveOrthogonalRoute(Request(0.f, 0.f, 300.f, 0.f), {body}, points));
    EXPECT_TRUE(IsOrthogonal(points));
    EXPECT_FALSE(RouteEntersBody(points, body));
    // Around the 80-tall body plus clearance on both sides, not through it.
    EXPECT_GT(RouteLength(points), 300.f);
}

TEST(GraphRouteSolver, ItTakesTheShorterWayPastAnOffCentreBody)
{
    // The body's bottom edge is far nearer the wire than its top edge, so a
    // route that reads as "shortest" must dip under it.
    const Obstacle body = Body(100.f, -400.f, 200.f, 20.f);
    std::vector<Vector2> points;
    ASSERT_TRUE(SolveOrthogonalRoute(Request(0.f, 0.f, 300.f, 0.f), {body}, points));
    ASSERT_FALSE(RouteEntersBody(points, body));
    const float lowest = std::max_element(points.begin(), points.end(), [](const Vector2& a, const Vector2& b){ return a.y < b.y; })->y;
    const float highest = std::min_element(points.begin(), points.end(), [](const Vector2& a, const Vector2& b){ return a.y < b.y; })->y;
    EXPECT_GT(lowest, 20.f) << "the route should pass below the body";
    EXPECT_GT(highest, -400.f) << "the route should not climb over the tall side";
}

TEST(GraphRouteSolver, LaneSpreadPushesASecondWireOffTheFirstsDetour)
{
    const Obstacle body = Body(100.f, -40.f, 200.f, 40.f);
    std::vector<Vector2> firstPoints, secondPoints;
    RouteRequest first = Request(0.f, 0.f, 300.f, 0.f);
    ASSERT_TRUE(SolveOrthogonalRoute(first, {body}, firstPoints));

    RouteRequest second = first;
    second.LaneSpread = 10.f;
    ASSERT_TRUE(SolveOrthogonalRoute(second, {body}, secondPoints));

    // The detour may go either way around a centred body; compare how far.
    auto farthestFromTheWire = [](const std::vector<Vector2>& route)
    {
        float extreme = 0.f;
        for (const Vector2& point : route)
            extreme = std::max(extreme, std::abs(point.y));
        return extreme;
    };
    const float firstExtreme = farthestFromTheWire(firstPoints);
    const float secondExtreme = farthestFromTheWire(secondPoints);
    EXPECT_NE(firstExtreme, secondExtreme) << "both wires took the same detour line";
    EXPECT_NEAR(std::abs(secondExtreme - firstExtreme), 10.f, 0.5f);
}

TEST(GraphRouteSolver, APortInsideANeighboursClearanceStillGetsARoute)
{
    // Nodes pack tightly enough that a port lands inside the box beside it. No
    // route can honour that box — the wire ends in there — so the wire is
    // routed rather than abandoned to the caller's crossing fallback.
    const Obstacle crowding = Body(280.f, -200.f, 320.f, 200.f);
    std::vector<Vector2> points;
    ASSERT_TRUE(SolveOrthogonalRoute(Request(0.f, 0.f, 300.f, 0.f), {crowding}, points));
    EXPECT_TRUE(IsOrthogonal(points));
    EXPECT_FLOAT_EQ(points.front().x, 0.f);
    EXPECT_FLOAT_EQ(points.front().y, 0.f);
    EXPECT_FLOAT_EQ(points.back().x, 300.f);
    EXPECT_FLOAT_EQ(points.back().y, 0.f);
}

TEST(GraphRouteSolver, ABodyThatHoldsNoEndpointIsStillHonoured)
{
    // The relaxation only drops boxes an endpoint sits inside; one that merely
    // stands between the ports is routed around, not through.
    const Obstacle between = Body(140.f, -200.f, 180.f, 200.f);
    std::vector<Vector2> points;
    ASSERT_TRUE(SolveOrthogonalRoute(Request(0.f, 0.f, 300.f, 0.f), {between}, points));
    EXPECT_FALSE(RouteEntersBody(points, between));
}

TEST(GraphRouteSolver, AWireHugsTheBodyItStartsOn)
{
    // A wire's own source and target pass zero clearance: it may run along them
    // — it began on that edge — but not through them.
    Obstacle ownBody = Body(100.f, -40.f, 200.f, 40.f);
    ownBody.Clearance = 0.f;
    std::vector<Vector2> points;
    ASSERT_TRUE(SolveOrthogonalRoute(Request(0.f, 0.f, 300.f, 0.f), {ownBody}, points));
    EXPECT_FALSE(RouteEntersBody(points, ownBody));
    // Hugging costs the body's half-height plus its width, and no clearance.
    EXPECT_NEAR(RouteLength(points), 300.f + 2.f * 40.f, 0.5f);
}

TEST(GraphRouteSolver, ARouteRunsBetweenTwoBodiesRatherThanAroundBoth)
{
    // A gap wider than twice the clearance is a corridor, and taking it is far
    // shorter than going around the pair.
    const Obstacle above = Body(100.f, -300.f, 200.f, -50.f);
    const Obstacle below = Body(100.f, 50.f, 200.f, 300.f);
    std::vector<Vector2> points;
    ASSERT_TRUE(SolveOrthogonalRoute(Request(0.f, 0.f, 300.f, 0.f), {above, below}, points));
    EXPECT_FALSE(RouteEntersBody(points, above));
    EXPECT_FALSE(RouteEntersBody(points, below));
    EXPECT_NEAR(RouteLength(points), 300.f, 0.5f) << "the corridor is a straight shot";
}

namespace
{

// A five-point route: port stub, a run along `shared`, then the stub into the
// far port. The interior run is what the nudge is allowed to move.
Route RouteAlong(float shared, float startY, float endY)
{
    Route route;
    route.Points = {{0.f, startY}, {20.f, startY}, {20.f, shared}, {200.f, shared}, {220.f, endY}};
    return route;
}

} // namespace

TEST(NudgeSharedRuns, TwoWiresOnOneRunAreSpreadAcrossIt)
{
    std::vector<Route> routes{RouteAlong(50.f, 0.f, 100.f), RouteAlong(50.f, 10.f, 110.f)};
    NudgeSharedRuns(routes, {}, 16.f, 10.f);

    const float first = routes[0].Points[2].y;
    const float second = routes[1].Points[2].y;
    EXPECT_NEAR(std::abs(second - first), 10.f, 0.01f);
    // Centred on the run they shared, so neither wire is the one that moved.
    EXPECT_NEAR((first + second) * 0.5f, 50.f, 0.01f);
    // The whole run moves together, not just one end of it.
    EXPECT_FLOAT_EQ(routes[0].Points[2].y, routes[0].Points[3].y);
    EXPECT_FLOAT_EQ(routes[1].Points[2].y, routes[1].Points[3].y);
}

TEST(NudgeSharedRuns, ThePortStubsStayOnTheirPorts)
{
    std::vector<Route> routes{RouteAlong(50.f, 0.f, 100.f), RouteAlong(50.f, 10.f, 110.f)};
    NudgeSharedRuns(routes, {}, 16.f, 10.f);

    EXPECT_FLOAT_EQ(routes[0].Points[0].y, 0.f);
    EXPECT_FLOAT_EQ(routes[0].Points[1].y, 0.f);
    EXPECT_FLOAT_EQ(routes[0].Points[4].y, 100.f);
    EXPECT_FLOAT_EQ(routes[1].Points[0].y, 10.f);
    EXPECT_FLOAT_EQ(routes[1].Points[4].y, 110.f);
}

TEST(NudgeSharedRuns, RunsThatNeverMeetAreLeftWhereTheyAre)
{
    // Different rows and different columns: nothing to tell apart.
    std::vector<Route> apart{RouteAlong(50.f, 0.f, 100.f), RouteAlong(90.f, 10.f, 110.f)};
    apart[1].Points = {{300.f, 10.f}, {320.f, 10.f}, {320.f, 90.f}, {500.f, 90.f}, {520.f, 110.f}};
    const std::vector<Route> before = apart;
    NudgeSharedRuns(apart, {}, 16.f, 10.f);
    EXPECT_EQ(apart[0].Points, before[0].Points);
    EXPECT_EQ(apart[1].Points, before[1].Points);

    // Same line, disjoint stretches of it: still not shared.
    std::vector<Route> disjoint{RouteAlong(50.f, 0.f, 100.f), RouteAlong(50.f, 10.f, 110.f)};
    disjoint[1].Points = {{400.f, 10.f}, {420.f, 10.f}, {420.f, 50.f}, {600.f, 50.f}, {620.f, 110.f}};
    const std::vector<Route> disjointBefore = disjoint;
    NudgeSharedRuns(disjoint, {}, 16.f, 10.f);
    EXPECT_EQ(disjoint[0].Points, disjointBefore[0].Points);
    EXPECT_EQ(disjoint[1].Points, disjointBefore[1].Points);
}

TEST(CentreRunsInFreeSpace, AVerticalRunSitsInTheMiddleOfItsAlley)
{
    // Enters from the left above both bodies, drops through the gap between
    // them, leaves to the right below them. The drop can sit anywhere in that
    // gap for the same length, and hugging the left one reads worst.
    Route route;
    route.Points = {{-400.f, -200.f}, {116.f, -200.f}, {116.f, 400.f}, {500.f, 400.f}};
    std::vector<Route> routes{route};

    const Obstacle left = Body(0.f, -100.f, 100.f, 300.f);
    const Obstacle right = Body(200.f, -100.f, 300.f, 300.f);
    CentreRunsInFreeSpace(routes, {left, right}, 16.f);

    // The alley runs from 100+16 to 200-16, so its middle is 150.
    EXPECT_NEAR(routes[0].Points[1].x, 150.f, 0.01f);
    EXPECT_FLOAT_EQ(routes[0].Points[1].x, routes[0].Points[2].x);
}

TEST(CentreRunsInFreeSpace, ARunThatWouldLengthenTheRouteStaysPut)
{
    // Both neighbours leave to the right: this is a wrap around a body, and
    // sliding the drop further left buys nothing but length.
    Route route;
    route.Points = {{400.f, -200.f}, {116.f, -200.f}, {116.f, 400.f}, {400.f, 400.f}};
    std::vector<Route> routes{route};

    const Obstacle left = Body(0.f, -100.f, 100.f, 300.f);
    CentreRunsInFreeSpace(routes, {left}, 16.f);
    EXPECT_FLOAT_EQ(routes[0].Points[1].x, 116.f);
}

TEST(CentreRunsInFreeSpace, ARunInsideABodyIsLeftAlone)
{
    // Nothing to centre between: the run is over the body, not beside it.
    Route route;
    route.Points = {{-400.f, -200.f}, {110.f, -200.f}, {110.f, 400.f}, {500.f, 400.f}};
    std::vector<Route> routes{route};

    const Obstacle over = Body(0.f, -100.f, 120.f, 300.f);
    CentreRunsInFreeSpace(routes, {over}, 16.f);
    EXPECT_FLOAT_EQ(routes[0].Points[1].x, 110.f);
}

TEST(CentreRunsInFreeSpace, ATightCorridorIsSharedRatherThanHugged)
{
    // The gap between the bodies is narrower than twice the clearance, so the
    // route had to come inside it. Centring then splits what room there is
    // instead of leaving the wire flush against one of them.
    Route route;
    route.Points = {{-400.f, -200.f}, {100.f, -200.f}, {100.f, 400.f}, {500.f, 400.f}};
    std::vector<Route> routes{route};

    const Obstacle left = Body(0.f, -100.f, 100.f, 300.f);
    const Obstacle right = Body(110.f, -100.f, 210.f, 300.f);
    CentreRunsInFreeSpace(routes, {left, right}, 16.f);
    EXPECT_NEAR(routes[0].Points[1].x, 105.f, 0.01f);
}

TEST(CentreRunsInFreeSpace, ARunBesideAStubLeavesTheStubStanding)
{
    // Centring the first interior run drags the stub's far end with it, and a
    // stub pulled onto its port puts the wire's turn on the port.
    Route route;
    route.Points = {{0.f, 0.f}, {12.f, 0.f}, {12.f, 300.f}, {400.f, 300.f}, {420.f, 300.f}};
    std::vector<Route> routes{route};
    CentreRunsInFreeSpace(routes, {}, 16.f);
    EXPECT_GE(routes[0].Points[1].x, 8.f);
    EXPECT_FLOAT_EQ(routes[0].Points[0].x, 0.f);
}

TEST(NudgeSharedRuns, RunsThatMerelyReadAsOneAreSeparatedToo)
{
    // Four units apart over a long shared stretch is one line to the eye. The
    // pass has to group by proximity, not by an exact shared coordinate.
    std::vector<Route> routes{RouteAlong(50.f, 0.f, 100.f), RouteAlong(54.f, 10.f, 110.f)};
    NudgeSharedRuns(routes, {}, 16.f, 10.f);
    EXPECT_NEAR(std::abs(routes[1].Points[2].y - routes[0].Points[2].y), 10.f, 0.01f);
}

TEST(NudgeSharedRuns, APortStubHoldsItsLineAndTheOtherWireStepsOff)
{
    // The stub cannot move — the port is where it is — so the run sharing its
    // line is the one that gives way.
    Route stub;
    stub.Points = {{0.f, 50.f}, {200.f, 50.f}, {220.f, 90.f}, {240.f, 90.f}};
    std::vector<Route> routes{stub, RouteAlong(50.f, 0.f, 100.f)};
    NudgeSharedRuns(routes, {}, 16.f, 10.f);

    EXPECT_FLOAT_EQ(routes[0].Points[0].y, 50.f) << "the stub moved off its port";
    EXPECT_FLOAT_EQ(routes[0].Points[1].y, 50.f);
    EXPECT_GE(std::abs(routes[1].Points[2].y - 50.f), 5.f) << "the movable run stayed on the stub's line";
}
