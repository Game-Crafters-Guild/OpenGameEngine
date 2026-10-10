#include "Graph/GraphRouteSolver.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <queue>
#include <utility>

namespace GameEngine::GraphRouting
{
namespace
{
/* Layout sits on far coarser steps than this, so two lines closer than an
   epsilon apart are the same line and merging them keeps the lattice small. */
constexpr float kLineEpsilon = 0.25f;
/* What a bend is worth in length. High enough that the router prefers one long
   run to a staircase of the same length, low enough that it still detours
   rather than crossing a node. */
constexpr float kTurnPenalty = 24.f;
/* The lattice is quadratic in this and A* runs per wire per frame, so the first
   attempt keeps it small. A route long enough to need more lines than this gets
   a second, wider attempt rather than no route at all — a wire that cannot be
   solved falls back to geometry that crosses whatever is in the way. */
constexpr size_t kMaxLinesPerAxis = 40;
constexpr size_t kMaxLinesPerAxisWide = 128;
/* How much of a port stub the centring and the nudge must leave standing. */
constexpr float kStubHoldGraph = 8.f;
/* The gap a nudge may not spend, however crowded the line it is sharing. */
constexpr float kNudgeBodyMarginGraph = 4.f;

enum class Axis
{
    Horizontal,
    Vertical
};

/* Blocked spans per lattice line, so an edge test is a scan of the few spans
   on its own line instead of a pass over every obstacle. */
struct BlockedSpans
{
    std::vector<std::pair<float, float>> Spans;

    bool Crosses(float a, float b) const
    {
        const float lo = std::min(a, b);
        const float hi = std::max(a, b);
        for (const auto& span : Spans)
        {
            if (std::max(lo, span.first) < std::min(hi, span.second) - kLineEpsilon)
                return true;
        }
        return false;
    }

    bool Contains(float v) const
    {
        for (const auto& span : Spans)
        {
            if (v > span.first + kLineEpsilon && v < span.second - kLineEpsilon)
                return true;
        }
        return false;
    }
};

void AddLine(std::vector<float>& lines, float value)
{
    lines.push_back(value);
}

void NormalizeLines(std::vector<float>& lines, float spanLo, float spanHi, size_t cap)
{
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end(),
                            [](float a, float b) { return std::abs(a - b) <= kLineEpsilon; }),
                lines.end());
    if (lines.size() <= cap)
        return;

    /* Keep the lines the route can actually turn on — those between its two
       endpoints, and then the nearest ones outside, since a detour leaves that
       span only far enough to get around what blocks it. Pruning by distance to
       the route's middle instead drops the ends of a long backtracking route,
       which is exactly where such a route has to turn. */
    const float lo = std::min(spanLo, spanHi);
    const float hi = std::max(spanLo, spanHi);
    auto distanceOutside = [lo, hi](float v)
    { return v < lo ? lo - v : (v > hi ? v - hi : 0.f); };
    std::vector<float> kept = lines;
    std::nth_element(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(cap), kept.end(),
                     [&](float a, float b)
                     { return distanceOutside(a) < distanceOutside(b); });
    kept.resize(cap);
    std::sort(kept.begin(), kept.end());
    lines.swap(kept);
}

size_t IndexOf(const std::vector<float>& lines, float value)
{
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (std::abs(lines[i] - value) <= kLineEpsilon)
            return i;
    }
    return lines.size();
}

bool TrySolve(const RouteRequest& request, const std::vector<Obstacle>& obstacles, size_t lineCap,
              std::vector<Mathematics::Vector2>& outPoints)
{
    const float clearance = std::max(0.f, request.Clearance);
    const float spread = std::max(0.f, request.LaneSpread);

    std::vector<float> xs;
    std::vector<float> ys;
    xs.reserve(obstacles.size() * 2 + 4);
    ys.reserve(obstacles.size() * 2 + 4);
    AddLine(xs, request.Start.x);
    AddLine(xs, request.End.x);
    AddLine(xs, request.ExitX);
    AddLine(xs, request.EnterX);
    AddLine(ys, request.Start.y);
    AddLine(ys, request.End.y);
    auto clearanceOf = [clearance](const Obstacle& obstacle)
    { return obstacle.Clearance >= 0.f ? obstacle.Clearance : clearance; };
    for (const Obstacle& obstacle : obstacles)
    {
        const float gap = clearanceOf(obstacle);
        AddLine(xs, obstacle.Body.X - gap - spread);
        AddLine(xs, obstacle.Body.Right() + gap + spread);
        AddLine(ys, obstacle.Body.Y - gap - spread);
        AddLine(ys, obstacle.Body.Bottom() + gap + spread);
    }
    /* A wire leaves and enters its port sideways, so no turn may land within a
       stub of either port: drop those columns and the lattice cannot offer one.
       The stub columns the caller asked for stay as candidates — the router
       takes them when they are free and picks its own when they are not. */
    const float minStub = std::max(0.f, request.MinStub);
    xs.erase(std::remove_if(xs.begin(), xs.end(),
                            [&](float x)
                            {
                                const bool onStartStub =
                                    x > request.Start.x + kLineEpsilon &&
                                    x < request.Start.x + minStub - kLineEpsilon;
                                const bool onEndStub = x < request.End.x - kLineEpsilon &&
                                                       x > request.End.x - minStub + kLineEpsilon;
                                return onStartStub || onEndStub;
                            }),
             xs.end());
    NormalizeLines(xs, request.Start.x, request.End.x, lineCap);
    NormalizeLines(ys, request.Start.y, request.End.y, lineCap);

    const size_t startI = IndexOf(xs, request.Start.x);
    const size_t startJ = IndexOf(ys, request.Start.y);
    const size_t goalI = IndexOf(xs, request.End.x);
    const size_t goalJ = IndexOf(ys, request.End.y);
    if (startI == xs.size() || startJ == ys.size() || goalI == xs.size() || goalJ == ys.size())
        return false;

    /* One blocked-span list per line. A wire may touch a body's clearance
       boundary but not pass inside it. */
    std::vector<BlockedSpans> rowSpans(ys.size());
    std::vector<BlockedSpans> colSpans(xs.size());
    for (const Obstacle& obstacle : obstacles)
    {
        const float gap = clearanceOf(obstacle);
        const float left = obstacle.Body.X - gap;
        const float right = obstacle.Body.Right() + gap;
        const float top = obstacle.Body.Y - gap;
        const float bottom = obstacle.Body.Bottom() + gap;
        for (size_t j = 0; j < ys.size(); ++j)
        {
            if (obstacle.PortRowsExempt && (j == startJ || j == goalJ))
                continue; // the wire's own port row: it leaves and arrives through the gap
            if (ys[j] > top + kLineEpsilon && ys[j] < bottom - kLineEpsilon)
                rowSpans[j].Spans.emplace_back(left, right);
        }
        for (size_t i = 0; i < xs.size(); ++i)
        {
            if (xs[i] > left + kLineEpsilon && xs[i] < right - kLineEpsilon)
                colSpans[i].Spans.emplace_back(top, bottom);
        }
    }

    const size_t width = xs.size();
    const size_t stateCount = width * ys.size() * 2;
    auto stateIndex = [width](size_t i, size_t j, Axis axis)
    { return (j * width + i) * 2 + (axis == Axis::Vertical ? 1 : 0); };

    constexpr float kInfinity = std::numeric_limits<float>::max();
    std::vector<float> best(stateCount, kInfinity);
    std::vector<size_t> cameFrom(stateCount, stateCount);

    auto heuristic = [&](size_t i, size_t j)
    { return std::abs(xs[i] - xs[goalI]) + std::abs(ys[j] - ys[goalJ]); };

    struct Open
    {
        float Estimate;
        size_t State;
        bool operator<(const Open& other) const { return Estimate > other.Estimate; }
    };
    std::priority_queue<Open> open;

    /* The wire leaves the source port sideways, so the first lattice move is
       already a horizontal arrival. */
    const size_t startState = stateIndex(startI, startJ, Axis::Horizontal);
    best[startState] = 0.f;
    open.push({heuristic(startI, startJ), startState});

    size_t goalState = stateCount;
    while (!open.empty())
    {
        const Open current = open.top();
        open.pop();
        const size_t state = current.State;
        const Axis axis = (state % 2) == 1 ? Axis::Vertical : Axis::Horizontal;
        const size_t cell = state / 2;
        const size_t i = cell % width;
        const size_t j = cell / width;
        const float cost = best[state];
        if (current.Estimate > cost + heuristic(i, j) + kLineEpsilon)
            continue;
        if (i == goalI && j == goalJ)
        {
            goalState = state;
            break;
        }

        auto relax = [&](size_t ni, size_t nj, Axis moveAxis, float distance)
        {
            /* The wire leaves its output port to the right and arrives at its
               input port from the left; anything else doubles back over a port. */
            if (state == startState && !(moveAxis == Axis::Horizontal && ni > i))
                return;
            if (ni == goalI && nj == goalJ && !(moveAxis == Axis::Horizontal && ni > i))
                return;
            const float turn = (moveAxis == axis) ? 0.f : kTurnPenalty;
            const float next = cost + distance + turn;
            const size_t nextState = stateIndex(ni, nj, moveAxis);
            if (next >= best[nextState])
                return;
            best[nextState] = next;
            cameFrom[nextState] = state;
            open.push({next + heuristic(ni, nj), nextState});
        };

        if (i + 1 < width && !rowSpans[j].Crosses(xs[i], xs[i + 1]))
            relax(i + 1, j, Axis::Horizontal, xs[i + 1] - xs[i]);
        if (i > 0 && !rowSpans[j].Crosses(xs[i - 1], xs[i]))
            relax(i - 1, j, Axis::Horizontal, xs[i] - xs[i - 1]);
        if (j + 1 < ys.size() && !colSpans[i].Crosses(ys[j], ys[j + 1]))
            relax(i, j + 1, Axis::Vertical, ys[j + 1] - ys[j]);
        if (j > 0 && !colSpans[i].Crosses(ys[j - 1], ys[j]))
            relax(i, j - 1, Axis::Vertical, ys[j] - ys[j - 1]);
    }

    if (goalState == stateCount)
        return false;

    std::vector<std::pair<float, float>> reversed;
    for (size_t state = goalState; state != stateCount; state = cameFrom[state])
    {
        const size_t cell = state / 2;
        reversed.emplace_back(xs[cell % width], ys[cell / width]);
        if (state == startState)
            break;
    }

    /* Built aside and published only on success: the caller keeps its own
       geometry when there is no route, and it is told so before it looks. */
    std::vector<Mathematics::Vector2> points;
    auto emit = [&](float x, float y)
    {
        const size_t n = points.size();
        if (n >= 1 && std::abs(points[n - 1].x - x) <= kLineEpsilon &&
            std::abs(points[n - 1].y - y) <= kLineEpsilon)
            return;
        /* Collinear runs are one segment: the lattice hands back a point per
           line crossed, and the hit test walks every one of them. */
        if (n >= 2)
        {
            const bool horizontal = std::abs(points[n - 1].y - points[n - 2].y) <= kLineEpsilon &&
                                    std::abs(y - points[n - 1].y) <= kLineEpsilon;
            const bool vertical = std::abs(points[n - 1].x - points[n - 2].x) <= kLineEpsilon &&
                                  std::abs(x - points[n - 1].x) <= kLineEpsilon;
            if (horizontal || vertical)
            {
                points[n - 1] = {x, y};
                return;
            }
        }
        points.push_back({x, y});
    };

    for (auto it = reversed.rbegin(); it != reversed.rend(); ++it)
        emit(it->first, it->second);
    if (points.size() < 2)
        return false;
    outPoints.swap(points);
    return true;
}

/* A port can sit inside a neighbour's clearance box — nodes pack that tightly —
   and no route can then honour that box: the wire begins or ends inside it. */
bool BoxHoldsAnEndpoint(const Obstacle& obstacle, const RouteRequest& request, float gap)
{
    if (obstacle.PortRowsExempt)
        return false; // it is meant to hold its own ports; its rows are exempt instead
    const float left = obstacle.Body.X - gap;
    const float right = obstacle.Body.Right() + gap;
    const float top = obstacle.Body.Y - gap;
    const float bottom = obstacle.Body.Bottom() + gap;
    auto holds = [&](float x, float y) { return x > left && x < right && y > top && y < bottom; };
    return holds(request.Start.x, request.Start.y) || holds(request.ExitX, request.Start.y) ||
           holds(request.EnterX, request.End.y) || holds(request.End.x, request.End.y);
}
} // namespace

bool SolveOrthogonalRoute(const RouteRequest& request, const std::vector<Obstacle>& obstacles,
                          std::vector<Mathematics::Vector2>& outPoints)
{
    if (TrySolve(request, obstacles, kMaxLinesPerAxis, outPoints))
        return true;
    if (TrySolve(request, obstacles, kMaxLinesPerAxisWide, outPoints))
        return true;

    /* A box the wire already starts or ends inside cannot be honoured as it
       stands, and keeping it turns "the shortest way around" into "no route",
       which leaves the caller's fallback crossing everything. Give up its
       clearance first and the body itself only if the port is inside that too:
       a node whose gap the wire has to break is still a node to route around. */
    const float clearance = std::max(0.f, request.Clearance);
    std::vector<Obstacle> relaxed;
    relaxed.reserve(obstacles.size());
    bool changed = false;
    for (const Obstacle& obstacle : obstacles)
    {
        const float gap = obstacle.Clearance >= 0.f ? obstacle.Clearance : clearance;
        if (!BoxHoldsAnEndpoint(obstacle, request, gap))
        {
            relaxed.push_back(obstacle);
            continue;
        }
        changed = true;
        if (BoxHoldsAnEndpoint(obstacle, request, 0.f))
            continue; // the port is inside the body: this one cannot be routed around
        Obstacle hugged = obstacle;
        hugged.Clearance = 0.f;
        relaxed.push_back(hugged);
    }
    if (!changed)
        return false;
    return TrySolve(request, relaxed, kMaxLinesPerAxis, outPoints) ||
           TrySolve(request, relaxed, kMaxLinesPerAxisWide, outPoints);
}

namespace
{
/* How far one run may slide across its own axis without any part of the wire
   entering a body: the run itself moves, and the two runs on either side
   stretch to follow it. `bindToNeighbours` additionally holds it between where
   the wire arrives from and where it leaves to, which is what makes the move
   free — one side run grows by exactly what the other loses. */
bool RunFreeInterval(const Route& route, size_t segment, bool horizontal,
                     const std::vector<Obstacle>& obstacles, float clearance, bool bindToNeighbours,
                     bool honourClearance, float& outLo, float& outHi)
{
    auto along = [&route, horizontal](size_t i)
    { return horizontal ? route.Points[i].x : route.Points[i].y; };
    auto across = [&route, horizontal](size_t i)
    { return horizontal ? route.Points[i].y : route.Points[i].x; };

    const float coord = across(segment);
    const float prev = across(segment - 1);
    const float next = across(segment + 2);
    float lo = -std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::max();
    if (bindToNeighbours)
    {
        lo = std::min(prev, next);
        hi = std::max(prev, next);
        if (coord <= lo + kLineEpsilon || coord >= hi - kLineEpsilon)
            return false; // both neighbours leave the same way: moving costs length
    }

    /* A stub keeps its length: sliding a run onto its port puts the wire's turn
       on the port, and the run beside a stub is the one that could do it. */
    const size_t points = route.Points.size();
    auto holdStub = [&](float port)
    {
        if (coord > port)
            lo = std::max(lo, port + kStubHoldGraph);
        else
            hi = std::min(hi, port - kStubHoldGraph);
    };
    if (segment == 1)
        holdStub(across(0));
    if (segment + 3 == points)
        holdStub(across(points - 1));

    const float spanLo = std::min(along(segment), along(segment + 1));
    const float spanHi = std::max(along(segment), along(segment + 1));

    for (const Obstacle& obstacle : obstacles)
    {
        const float declared = obstacle.Clearance >= 0.f ? obstacle.Clearance : clearance;
        /* Telling two wires apart is worth more than the last of the gap, so a
           nudge may spend the clearance down to a margin off the body. It may
           never spend the body. */
        const float gap = honourClearance ? declared : std::min(declared, kNudgeBodyMarginGraph);
        const float boxSpanLo = (horizontal ? obstacle.Body.X : obstacle.Body.Y) - gap;
        const float boxSpanHi = (horizontal ? obstacle.Body.Right() : obstacle.Body.Bottom()) + gap;
        const float boxLo = (horizontal ? obstacle.Body.Y : obstacle.Body.X) - gap;
        const float boxHi = (horizontal ? obstacle.Body.Bottom() : obstacle.Body.Right()) + gap;

        /* Where a corridor is too tight for the gap this body asks for, the
           route had to come inside it. Bound by the body itself then: the wire
           still may not cross it, and centring between the two bodies is what
           shares the little room there is. */
        const float bodyLo = horizontal ? obstacle.Body.Y : obstacle.Body.X;
        const float bodyHi = horizontal ? obstacle.Body.Bottom() : obstacle.Body.Right();
        auto bound = [&](float from)
        {
            if (boxLo >= from - kLineEpsilon)
                hi = std::min(hi, boxLo);
            else if (boxHi <= from + kLineEpsilon)
                lo = std::max(lo, boxHi);
            else if (bodyLo >= from - kLineEpsilon)
                hi = std::min(hi, bodyLo);
            else if (bodyHi <= from + kLineEpsilon)
                lo = std::max(lo, bodyHi);
            else
                return false; // inside the body itself
            return true;
        };

        // What the run itself may not enter.
        if (spanHi > boxSpanLo + kLineEpsilon && spanLo < boxSpanHi - kLineEpsilon)
        {
            if (!bound(coord))
                return false;
        }

        /* What the two runs on either side may not enter as they stretch. A side
           run that already starts inside this body constrains nothing: it is the
           port stub, and the port is on the body's edge by construction. Bailing
           there would freeze every run next to a stub — which is most of them. */
        auto boundBySideRun = [&](float sideAlong, float sideAcross)
        {
            if (sideAlong <= boxSpanLo + kLineEpsilon || sideAlong >= boxSpanHi - kLineEpsilon)
                return true;
            bound(sideAcross);
            return true;
        };
        if (!boundBySideRun(along(segment), prev) || !boundBySideRun(along(segment + 1), next))
            return false;
    }

    outLo = lo;
    outHi = hi;
    return hi - lo > kLineEpsilon;
}

void MoveRun(Route& route, size_t segment, bool horizontal, float coord)
{
    for (size_t i : {segment, segment + 1})
    {
        if (horizontal)
            route.Points[i].y = coord;
        else
            route.Points[i].x = coord;
    }
}

void CentreOneRun(Route& route, size_t segment, bool horizontal,
                  const std::vector<Obstacle>& obstacles, float clearance)
{
    float lo = 0.f;
    float hi = 0.f;
    if (!RunFreeInterval(route, segment, horizontal, obstacles, clearance, true, true, lo, hi))
        return;
    const float coord = horizontal ? route.Points[segment].y : route.Points[segment].x;
    const float centred = (lo + hi) * 0.5f;
    if (std::abs(centred - coord) > kLineEpsilon)
        MoveRun(route, segment, horizontal, centred);
}

struct SharedRun
{
    size_t RouteIndex = 0;
    size_t SegmentIndex = 0;
    float Coord = 0.f;   // the line the run sits on
    float SpanLo = 0.f;  // extent along the run
    float SpanHi = 0.f;
    float EntryOrder = 0.f; // where the wire comes from and goes to, across the line
    bool Fixed = false;     // a port stub: it holds the line, others move off it
};

void NudgeAxis(std::vector<Route>& routes, std::vector<SharedRun>& runs,
               const std::vector<Obstacle>& obstacles, float clearance, float gap, bool horizontal,
               std::vector<NudgeNote>* notes)
{
    if (runs.size() < 2)
        return;

    /* Two runs share a line when they are near enough to read as one and long
       enough alongside each other to notice — not only when they sit at the
       same coordinate. Grouping on exact coincidence leaves near-parallel runs
       in separate groups, each spread on its own, free to drift together. */
    std::vector<int> parent(runs.size());
    for (size_t i = 0; i < runs.size(); ++i)
        parent[i] = static_cast<int>(i);
    auto find = [&parent](int i)
    {
        while (parent[static_cast<size_t>(i)] != i)
            i = parent[static_cast<size_t>(i)];
        return i;
    };
    for (size_t a = 0; a < runs.size(); ++a)
    {
        for (size_t b = a + 1; b < runs.size(); ++b)
        {
            if (runs[a].RouteIndex == runs[b].RouteIndex)
                continue;
            if (std::abs(runs[a].Coord - runs[b].Coord) >= gap)
                continue;
            const float overlap =
                std::min(runs[a].SpanHi, runs[b].SpanHi) - std::max(runs[a].SpanLo, runs[b].SpanLo);
            if (overlap <= gap)
                continue;
            const int ra = find(static_cast<int>(a));
            const int rb = find(static_cast<int>(b));
            if (ra != rb)
                parent[static_cast<size_t>(rb)] = ra;
        }
    }

    std::vector<std::vector<size_t>> groups(runs.size());
    for (size_t i = 0; i < runs.size(); ++i)
        groups[static_cast<size_t>(find(static_cast<int>(i)))].push_back(i);

    for (auto& group : groups)
    {
        const size_t count = group.size();
        if (count <= 1)
            continue;

        /* Ordered by where each wire enters and leaves the line, so spreading
           them apart does not manufacture crossings. */
        std::sort(group.begin(), group.end(), [&runs](size_t a, size_t b)
                  { return runs[a].EntryOrder < runs[b].EntryOrder; });

        float centre = 0.f;
        for (size_t index : group)
            centre += runs[index].Coord;
        centre /= static_cast<float>(count);

        /* Separation is worth a little length but never a crossing, so every
           wire keeps to the room it has. A wire with none holds the line and
           the others step off it. */
        std::vector<float> placed;
        placed.reserve(count);
        auto taken = [&placed, gap](float v)
        {
            for (float other : placed)
            {
                if (std::abs(other - v) < gap * 0.5f)
                    return true;
            }
            return false;
        };

        for (size_t k = 0; k < count; ++k)
        {
            const SharedRun& run = runs[group[k]];
            float lo = 0.f;
            float hi = 0.f;
            const bool free =
                !run.Fixed && RunFreeInterval(routes[run.RouteIndex], run.SegmentIndex, horizontal,
                                              obstacles, clearance, false, false, lo, hi);
            if (!free)
            {
                placed.push_back(run.Coord);
                if (notes)
                    notes->push_back({run.RouteIndex, run.SegmentIndex, horizontal, run.Fixed,
                                      false, run.Coord, run.Coord, lo, hi});
                continue;
            }

            const float slot =
                centre + (static_cast<float>(k) - static_cast<float>(count - 1) * 0.5f) * gap;
            float target = std::clamp(slot, lo, hi);
            for (int attempt = 1; taken(target) && attempt <= 8; ++attempt)
            {
                const float step = gap * 0.5f * static_cast<float>(attempt);
                const float up = std::clamp(target + step, lo, hi);
                const float down = std::clamp(target - step, lo, hi);
                if (!taken(up))
                    target = up;
                else if (!taken(down))
                    target = down;
            }
            placed.push_back(target);
            if (notes)
                notes->push_back({run.RouteIndex, run.SegmentIndex, horizontal, false, true,
                                  run.Coord, target, lo, hi});
            MoveRun(routes[run.RouteIndex], run.SegmentIndex, horizontal, target);
        }
    }
}
} // namespace

void CentreRunsInFreeSpace(std::vector<Route>& routes, const std::vector<Obstacle>& obstacles,
                           float clearance)
{
    for (Route& route : routes)
    {
        const size_t points = route.Points.size();
        if (points < 4)
            continue;
        /* Interior runs only: the first and last are the port stubs. */
        for (size_t i = 1; i + 2 < points; ++i)
        {
            const bool isHorizontal = std::abs(route.Points[i + 1].y - route.Points[i].y) <= kLineEpsilon;
            const bool isVertical = std::abs(route.Points[i + 1].x - route.Points[i].x) <= kLineEpsilon;
            if (isHorizontal == isVertical)
                continue;
            CentreOneRun(route, i, isHorizontal, obstacles, clearance);
        }
    }
}

void NudgeSharedRuns(std::vector<Route>& routes, const std::vector<Obstacle>& obstacles,
                     float clearance, float gap, std::vector<NudgeNote>* notes)
{
    if (gap <= 0.f)
        return;

    std::vector<SharedRun> horizontal;
    std::vector<SharedRun> vertical;
    for (size_t r = 0; r < routes.size(); ++r)
    {
        const Route& route = routes[r];
        const size_t points = route.Points.size();
        if (points < 4)
            continue;
        /* Port stubs take part as fixed members: a wire has to meet its port
           where the port is, but a run sharing that line still has to move. */
        for (size_t i = 0; i + 1 < points; ++i)
        {
            const bool isHorizontal = std::abs(route.Points[i + 1].y - route.Points[i].y) <= kLineEpsilon;
            const bool isVertical = std::abs(route.Points[i + 1].x - route.Points[i].x) <= kLineEpsilon;
            if (isHorizontal == isVertical)
                continue; // a zero-length or diagonal step shares nothing

            SharedRun run;
            run.RouteIndex = r;
            run.SegmentIndex = i;
            run.Fixed = i == 0 || i + 2 == points;
            if (isHorizontal)
            {
                run.Coord = route.Points[i].y;
                run.SpanLo = std::min(route.Points[i].x, route.Points[i + 1].x);
                run.SpanHi = std::max(route.Points[i].x, route.Points[i + 1].x);
                run.EntryOrder = run.Fixed ? run.Coord : route.Points[i - 1].y + route.Points[i + 2].y;
                horizontal.push_back(run);
            }
            else
            {
                run.Coord = route.Points[i].x;
                run.SpanLo = std::min(route.Points[i].y, route.Points[i + 1].y);
                run.SpanHi = std::max(route.Points[i].y, route.Points[i + 1].y);
                run.EntryOrder = run.Fixed ? run.Coord : route.Points[i - 1].x + route.Points[i + 2].x;
                vertical.push_back(run);
            }
        }
    }

    NudgeAxis(routes, horizontal, obstacles, clearance, gap, true, notes);
    NudgeAxis(routes, vertical, obstacles, clearance, gap, false, notes);
}

} // namespace GameEngine::GraphRouting
