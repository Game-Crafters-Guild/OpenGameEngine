// Degenerate geometry the canvas can hand the router: coincident ports, a target
// left of its source, a body of no size, one that swallows both ports, and more
// obstacles than the lattice can hold. None of it may hang, read out of bounds,
// or return a route that is not a route.

#include <gtest/gtest.h>

#include "Graph/GraphRouteSolver.h"

#include "Mathematics/Rect.h"
#include "Mathematics/Vector2.h"

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

RouteRequest Hostile(float startX, float startY, float endX, float endY)
{
    RouteRequest request;
    request.Start = {startX, startY};
    request.End = {endX, endY};
    request.ExitX = startX + 14.f;
    request.EnterX = endX - 14.f;
    request.Clearance = 16.f;
    return request;
}

// A route is a polyline of orthogonal steps that starts and ends on its ports.
void ExpectWellFormed(const RouteRequest& request, const std::vector<Vector2>& points)
{
    ASSERT_GE(points.size(), 2u);
    EXPECT_FLOAT_EQ(points.front().x, request.Start.x);
    EXPECT_FLOAT_EQ(points.front().y, request.Start.y);
    EXPECT_FLOAT_EQ(points.back().x, request.End.x);
    EXPECT_FLOAT_EQ(points.back().y, request.End.y);
    for (const Vector2& point : points)
    {
        ASSERT_TRUE(std::isfinite(point.x));
        ASSERT_TRUE(std::isfinite(point.y));
    }
    for (size_t i = 1; i < points.size(); ++i)
    {
        EXPECT_TRUE(std::abs(points[i].x - points[i - 1].x) < 0.5f ||
                    std::abs(points[i].y - points[i - 1].y) < 0.5f)
            << "diagonal step at " << i;
    }
}

} // namespace

TEST(GraphRouteSolverHostile, PortsOnTopOfEachOtherReportFailureAndKeepTheOutput)
{
    // A wire has to leave its output port rightwards and arrive at its input port
    // from the left, which coincident ports cannot both satisfy. There is no
    // route to give, so the caller keeps its own geometry.
    const RouteRequest request = Hostile(100.f, 100.f, 100.f, 100.f);
    std::vector<Vector2> points{{7.f, 9.f}};
    EXPECT_FALSE(SolveOrthogonalRoute(request, {}, points));
    ASSERT_EQ(points.size(), 1u);
    EXPECT_FLOAT_EQ(points[0].x, 7.f);
    EXPECT_FLOAT_EQ(points[0].y, 9.f);
}

TEST(GraphRouteSolverHostile, ATargetBehindItsSource)
{
    const RouteRequest request = Hostile(400.f, 0.f, 100.f, 200.f);
    std::vector<Vector2> points;
    const bool solved = SolveOrthogonalRoute(request, {}, points);
    EXPECT_TRUE(solved);
    if (solved)
        ExpectWellFormed(request, points);
}

TEST(GraphRouteSolverHostile, ABodyOfNoSize)
{
    const Obstacle degenerate = Body(200.f, 100.f, 200.f, 100.f);
    const RouteRequest request = Hostile(0.f, 0.f, 400.f, 0.f);
    std::vector<Vector2> points;
    ASSERT_TRUE(SolveOrthogonalRoute(request, {degenerate}, points));
    ExpectWellFormed(request, points);
}

TEST(GraphRouteSolverHostile, ABodyThatSwallowsBothPorts)
{
    const Obstacle swallowing = Body(-1000.f, -1000.f, 1000.f, 1000.f);
    const RouteRequest request = Hostile(0.f, 0.f, 400.f, 0.f);
    std::vector<Vector2> points;
    const bool solved = SolveOrthogonalRoute(request, {swallowing}, points);
    EXPECT_TRUE(solved);
    if (solved)
        ExpectWellFormed(request, points);
}

TEST(GraphRouteSolverHostile, InvertedAndNestedBodies)
{
    // Left > Right and Top > Bottom: the canvas should never produce it, but the
    // router may not spin or emit a diagonal if it does.
    const std::vector<Obstacle> bodies{{300.f, 200.f, 100.f, 50.f}, {150.f, -50.f, 350.f, 250.f}};
    const RouteRequest request = Hostile(0.f, 0.f, 500.f, 100.f);
    std::vector<Vector2> points;
    const bool solved = SolveOrthogonalRoute(request, bodies, points);
    EXPECT_TRUE(solved);
    if (solved)
        ExpectWellFormed(request, points);
}

TEST(GraphRouteSolverHostile, MoreBodiesThanTheLatticeHolds)
{
    std::vector<Obstacle> crowd;
    crowd.reserve(400);
    for (int i = 0; i < 400; ++i)
    {
        const float x = static_cast<float>((i % 20) * 120);
        const float y = static_cast<float>((i / 20) * 90);
        crowd.push_back(Body(x, y, x + 80.f, y + 50.f));
    }
    const RouteRequest request = Hostile(-200.f, -200.f, 2600.f, 1900.f);
    std::vector<Vector2> points;
    const bool solved = SolveOrthogonalRoute(request, crowd, points);
    EXPECT_TRUE(solved);
    if (solved)
        ExpectWellFormed(request, points);
}

TEST(GraphRouteSolverHostile, HugeCoordinates)
{
    const RouteRequest request = Hostile(-1.0e7f, -1.0e7f, 1.0e7f, 1.0e7f);
    const Obstacle body = Body(-100.f, -100.f, 100.f, 100.f);
    std::vector<Vector2> points;
    const bool solved = SolveOrthogonalRoute(request, {body}, points);
    EXPECT_TRUE(solved);
    if (solved)
        ExpectWellFormed(request, points);
}

TEST(GraphRouteSolverHostile, TheNudgeAndTheCentringSurviveShortAndEmptyRoutes)
{
    Route empty;
    Route single;
    single.Points = {{10.f, 20.f}};
    Route pair;
    pair.Points = {{0.f, 0.f}, {100.f, 0.f}};
    std::vector<Route> routes{empty, single, pair};
    const Obstacle body = Body(40.f, -20.f, 60.f, 20.f);

    CentreRunsInFreeSpace(routes, {body}, 16.f);
    NudgeSharedRuns(routes, {body}, 16.f, 12.f);

    EXPECT_TRUE(routes[0].Points.empty());
    EXPECT_EQ(routes[1].Points.size(), 1u);
    EXPECT_FLOAT_EQ(routes[2].Points[0].x, 0.f);
    EXPECT_FLOAT_EQ(routes[2].Points[1].x, 100.f);
}

TEST(GraphRouteSolverHostile, ANudgeWithNoGapDoesNothing)
{
    Route route;
    route.Points = {{0.f, 0.f}, {20.f, 0.f}, {20.f, 100.f}, {200.f, 100.f}, {220.f, 100.f}};
    std::vector<Route> routes{route, route};
    NudgeSharedRuns(routes, {}, 16.f, 0.f);
    EXPECT_EQ(routes[0].Points, routes[1].Points);
    
}
