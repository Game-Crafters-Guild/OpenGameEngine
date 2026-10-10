#pragma once

#include "Graph/GraphModel.h"
#include "Graph/GraphNodeMetrics.h"
#include "Mathematics/Rect.h"
#include "Mathematics/Vector2.h"

#include <cmath>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace GameEngine::GraphNodeOverlap {

inline float SnapCoordinate(float value)
{
    return std::round(value / GraphNodeMetrics::kGridSizeGraph) * GraphNodeMetrics::kGridSizeGraph;
}

inline Mathematics::Vector2 SnapPosition(Mathematics::Vector2 p)
{
    return Mathematics::Vector2(SnapCoordinate(p.x), SnapCoordinate(p.y));
}

/** True when `node` at `position` comes within `margin` of another node's rect.
    Margin 0 asks the plain question — do these two rects actually intersect —
    and that is what decides whether an authored position needs correcting at
    all. A positive margin is for choosing where to PUT a node that has to move,
    so it lands with air around it. Answering the first question with the second
    margin is what turns a tidy column into a scattered graph: rows laid out one
    grid apart read as colliding, and every load shuffles them. */
template <typename WidthFn, typename HeightFn>
bool WouldOverlapAny(const Graph::Model& model, const Graph::Node& node,
                     Mathematics::Vector2 position,
                     const std::unordered_set<std::string>& ignoreNodeIds, WidthFn getWidth,
                     HeightFn getHeight, float margin = 0.f)
{
    const Mathematics::Rect candidate =
        Mathematics::Rect{position.x, position.y, getWidth(node), getHeight(node)}.Inflated(margin);
    for (const Graph::Node& other : model.Nodes)
    {
        if (other.Id == node.Id || ignoreNodeIds.count(other.Id) != 0)
            continue;
        const Mathematics::Rect otherRect{other.PositionX, other.PositionY, getWidth(other),
                                          getHeight(other)};
        if (candidate.Overlaps(otherRect))
            return true;
    }
    return false;
}

template <typename WidthFn, typename HeightFn>
void FindNearestNonOverlappingPosition(const Graph::Model& model, const Graph::Node& node,
                                       Mathematics::Vector2 desired,
                                       const std::unordered_set<std::string>& ignoreNodeIds,
                                       Mathematics::Vector2& out, WidthFn getWidth,
                                       HeightFn getHeight)
{
    const Mathematics::Vector2 base = SnapPosition(desired);
    out = base;
    /* Leave an authored position alone unless it truly intersects something. */
    if (!WouldOverlapAny(model, node, base, ignoreNodeIds, getWidth, getHeight))
        return;
    const float placementMargin = GraphNodeMetrics::kGridSizeGraph;

    constexpr int kMaxRings = 96;
    for (int ring = 1; ring <= kMaxRings; ++ring)
    {
        bool found = false;
        Mathematics::Vector2 best = base;
        float bestDistSq = std::numeric_limits<float>::max();

        auto consider = [&](int gx, int gy) {
            const Mathematics::Vector2 p(
                base.x + static_cast<float>(gx) * GraphNodeMetrics::kGridSizeGraph,
                base.y + static_cast<float>(gy) * GraphNodeMetrics::kGridSizeGraph);
            /* Where it LANDS keeps its air. */
            if (WouldOverlapAny(model, node, p, ignoreNodeIds, getWidth, getHeight,
                                placementMargin))
                return;
            const Mathematics::Vector2 d = p - base;
            const float distSq = Mathematics::Vector2::Dot(d, d);
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                best = p;
                found = true;
            }
        };

        for (int dx = -ring; dx <= ring; ++dx)
        {
            consider(dx, -ring);
            consider(dx, ring);
        }
        for (int dy = -ring + 1; dy <= ring - 1; ++dy)
        {
            consider(-ring, dy);
            consider(ring, dy);
        }

        if (found)
        {
            out = best;
            return;
        }
    }
}

template <typename WidthFn, typename HeightFn>
void ResolveNodeOverlaps(Graph::Model& model, const std::vector<std::string>& nodeIds,
                         WidthFn getWidth, HeightFn getHeight)
{
    for (const std::string& id : nodeIds)
    {
        Graph::Node* node = model.FindNode(id);
        if (!node)
            continue;

        std::unordered_set<std::string> ignore{id};
        Mathematics::Vector2 resolved(node->PositionX, node->PositionY);
        FindNearestNonOverlappingPosition(model, *node, resolved, ignore, resolved, getWidth,
                                          getHeight);
        node->PositionX = resolved.x;
        node->PositionY = resolved.y;
    }
}

template <typename WidthFn, typename HeightFn>
void ResolveAllNodeOverlaps(Graph::Model& model, WidthFn getWidth, HeightFn getHeight)
{
    std::vector<std::string> allIds;
    allIds.reserve(model.Nodes.size());
    for (const Graph::Node& node : model.Nodes)
        allIds.push_back(node.Id);
    ResolveNodeOverlaps(model, allIds, getWidth, getHeight);
}

} // namespace GameEngine::GraphNodeOverlap
