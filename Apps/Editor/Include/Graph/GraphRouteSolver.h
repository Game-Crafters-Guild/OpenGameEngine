#pragma once

#include "Mathematics/Rect.h"
#include "Mathematics/Vector2.h"

#include <vector>

namespace GameEngine::GraphRouting
{

/// A node body a wire must not cross, in the router's own units.
struct Obstacle
{
    Mathematics::Rect Body;
    /// Gap this body alone demands; negative takes the request's.
    float Clearance = -1.f;
    /// Set on the wire's own source and target. Their gap cannot hold on the
    /// rows the wire's ports sit on — it leaves and arrives through it — but it
    /// holds everywhere else, so the wire keeps its distance from the body it
    /// is attached to instead of tracing its outline.
    bool PortRowsExempt = false;
};

struct RouteRequest
{
    /// Source port, on the right edge of its node.
    Mathematics::Vector2 Start;
    /// Target port, on the left edge of its node.
    Mathematics::Vector2 End;
    /// Stub columns the route prefers to leave from and arrive on, carrying
    /// whatever per-port fan the caller applies. They are candidates, not
    /// constraints: where a body sits on one, the router picks its own.
    float ExitX = 0.f;
    float EnterX = 0.f;
    /// Shortest horizontal run the wire may have at either port.
    float MinStub = 10.f;
    /// Gap kept between a wire and a node body.
    float Clearance = 16.f;
    /// Extra gap for this wire alone, so two wires detouring around one node
    /// take separate lines instead of the same one. Non-negative.
    float LaneSpread = 0.f;
};

/**
 * @brief Shortest orthogonal route from the source port to the target port that
 *        crosses no obstacle.
 *
 * Searches the lattice of lines the obstacles and the endpoints define — the
 * shortest obstacle-free orthogonal path always lies on it — scoring by length
 * plus a per-bend penalty, so the route reads as few straight runs rather than
 * a staircase of equal length.
 *
 * @return false when the endpoints or their stubs are unreachable, leaving the
 *         caller to fall back to its own geometry. `outPoints` is then
 *         untouched.
 */
bool SolveOrthogonalRoute(const RouteRequest& request, const std::vector<Obstacle>& obstacles,
                          std::vector<Mathematics::Vector2>& outPoints);

/// One wire's polyline, as the solver and the nudge below exchange it.
struct Route
{
    std::vector<Mathematics::Vector2> Points;
};

/**
 * @brief Slides each straight run to the middle of the free space it crosses.
 *
 * The shortest route hugs whatever it went around, tracing that node's outline
 * at exactly the clearance distance. A run that can move without lengthening
 * the route — one whose neighbours leave on opposite sides — is centred in the
 * gap between whatever bounds it, so wires read as running down alleys instead
 * of outlining nodes. Runs already inside a body's clearance are left alone: a
 * wire hugs its own source and target on purpose.
 *
 * Run before NudgeSharedRuns; centring is what puts shared runs on the same
 * line in the first place, and the nudge is what then tells them apart.
 */
void CentreRunsInFreeSpace(std::vector<Route>& routes, const std::vector<Obstacle>& obstacles,
                           float clearance);

/**
 * @brief Separates wires that ended up sharing a straight run.
 *
 * Routing each wire on its own puts two of them on the same line whenever that
 * line is the shortest way for both, and a shared run reads as one wire. This
 * spreads the wires across such a run by `gap`, ordered by where they enter and
 * leave it so the separation does not manufacture crossings. Port stubs are left
 * alone: a wire must meet its port where the port is.
 */
/// What one run's separation came down to, for the wire audit to report.
struct NudgeNote
{
    size_t RouteIndex = 0;
    size_t SegmentIndex = 0;
    bool Horizontal = false;
    bool Fixed = false;   ///< a port stub, which holds its line
    bool HasRoom = false; ///< false when nothing could move it
    float From = 0.f;
    float To = 0.f;
    float Lo = 0.f;
    float Hi = 0.f;
};

void NudgeSharedRuns(std::vector<Route>& routes, const std::vector<Obstacle>& obstacles,
                     float clearance, float gap, std::vector<NudgeNote>* notes = nullptr);

} // namespace GameEngine::GraphRouting
