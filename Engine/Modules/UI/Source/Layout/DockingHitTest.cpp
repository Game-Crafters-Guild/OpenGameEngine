#include "UI/Layout/DockingHitTest.h"
#include <algorithm>
#include <limits>

namespace GameEngine
{

static inline bool PointInRect(float x, float y, float rx, float ry, float rw, float rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

DockPosition DockingHitTest::CanonicalEdgeFromPoint(float x, float y, float left, float top, float width, float height)
{
    const float dl = x - left;
    const float dr = (left + width) - x;
    const float dt = y - top;
    const float db = (top + height) - y;
    float md = dl;
    DockPosition best = DockPosition::Left;
    if (dr < md)
    {
        md = dr;
        best = DockPosition::Right;
    }
    if (dt < md)
    {
        md = dt;
        best = DockPosition::Top;
    }
    if (db < md)
    {
        md = db;
        best = DockPosition::Bottom;
    }
    return best;
}

DockDropTarget DockingHitTest::Compute(const DockingManager& dm, float containerW, float containerH, float x, float y)
{
    DockDropTarget out{};

    // Early root ring test bounds
    const float rootL = 0.0f, rootT = 0.0f, rootW = containerW, rootH = containerH;

    // Build node rectangles from model
    std::vector<DockingManager::NodeRect> rects;
    dm.ComputeNodeRects(containerW, containerH, rects);

    // Find hovered leaf (there should be exactly one leaf under the point if inside dockspace)
    const DockingManager::NodeRect* hoveredLeaf = nullptr;
    for (const auto& r : rects)
    {
        if (!r.isLeaf)
            continue;
        if (PointInRect(x, y, r.x, r.y, r.width, r.height))
        {
            hoveredLeaf = &r;
            break;
        }
    }

    // Helper: enumerate ancestor split rects (including root if split) from shallowest to deepest
    auto enumerateAncestors = [&](const std::string& leafPath)
    {
        std::vector<const DockingManager::NodeRect*> ancestors;
        // For every rect that is a split and whose path is a prefix of leafPath
        for (const auto& r : rects)
        {
            if (r.isLeaf)
                continue;
            const std::string& p = r.path;
            if (p.size() <= leafPath.size() && leafPath.compare(0, p.size(), p) == 0)
            {
                ancestors.push_back(&r);
            }
        }
        // Sort by path length (shallowest first) so we can prefer deepest later
        std::sort(ancestors.begin(), ancestors.end(), [](auto a, auto b)
                  { return a->path.size() < b->path.size(); });
        return ancestors;
    };

    // Priority 0: TabBar — the narrow strip at the top of a leaf has highest
    // priority so that region/root edge-lock bands don't steal it.
    if (hoveredLeaf)
    {
        const auto& L = *hoveredLeaf;
        const bool inTabbar = PointInRect(x, y, L.x, L.y, L.width, std::min(kTabbarPx, L.height));
        if (inTabbar)
        {
            out.Kind = DockDropTarget::TargetKind::TabBar;
            out.Path = L.path;
            out.Edge = DockPosition::Center;
            return out;
        }
    }

    // Priority 1: Region split by edge-lock, layered after Root when edges coincide (with proportional compression)
    if (hoveredLeaf)
    {
        auto ancestors = enumerateAncestors(hoveredLeaf->path);
        for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it)
        { // deepest first
            const auto* a = *it;
            const float l = a->x, t = a->y, w = a->width, h = a->height;
            // Prune region that equals the whole container (would be same as Root)
            if (std::abs(l - rootL) <= 0.5f && std::abs(t - rootT) <= 0.5f &&
                std::abs(w - rootW) <= 0.5f && std::abs(h - rootH) <= 0.5f)
            {
                continue;
            }
            const float dl = std::max(0.0f, x - l);
            const float dr = std::max(0.0f, (l + w) - x);
            const float dt = std::max(0.0f, y - t);
            const float db = std::max(0.0f, (t + h) - y);
            float minD = dl;
            DockPosition minEdge = DockPosition::Left;
            if (dr < minD)
            {
                minD = dr;
                minEdge = DockPosition::Right;
            }
            if (dt < minD)
            {
                minD = dt;
                minEdge = DockPosition::Top;
            }
            if (db < minD)
            {
                minD = db;
                minEdge = DockPosition::Bottom;
            }

            // Edge equivalence pruning: if this side coincides with the root and the region spans full orthogonal extent,
            // splitting the region along this edge is equivalent to a root split; skip it.
            const bool spansFullHeight = (std::abs(t - rootT) <= 0.5f) && (std::abs((t + h) - (rootT + rootH)) <= 0.5f);
            const bool spansFullWidth = (std::abs(l - rootL) <= 0.5f) && (std::abs((l + w) - (rootL + rootW)) <= 0.5f);

            bool coincidesWithRoot = false;
            bool edgeEquivalentToRoot = false;
            switch (minEdge)
            {
            case DockPosition::Left:
                coincidesWithRoot = (l <= rootL + 0.5f);
                edgeEquivalentToRoot = coincidesWithRoot && spansFullHeight;
                break;
            case DockPosition::Right:
                coincidesWithRoot = (l + w >= rootL + rootW - 0.5f);
                edgeEquivalentToRoot = coincidesWithRoot && spansFullHeight;
                break;
            case DockPosition::Top:
                coincidesWithRoot = (t <= rootT + 0.5f);
                edgeEquivalentToRoot = coincidesWithRoot && spansFullWidth;
                break;
            case DockPosition::Bottom:
                coincidesWithRoot = (t + h >= rootT + rootH - 0.5f);
                edgeEquivalentToRoot = coincidesWithRoot && spansFullWidth;
                break;
            default:
                break;
            }
            if (edgeEquivalentToRoot)
                continue;

            // Proportional compression when coincident with root and the region is too shallow to host full bands
            float localRootLock = kOuterEdgeLockPx;
            float localRegionLock = kRegionEdgeLockPx;
            if (coincidesWithRoot)
            {
                const float along = (minEdge == DockPosition::Left || minEdge == DockPosition::Right) ? w : h;
                const float total = kOuterEdgeLockPx + kRegionEdgeLockPx;
                if (along < total)
                {
                    const float s = std::max(0.0f, along / std::max(1e-3f, total));
                    localRootLock = kOuterEdgeLockPx * s;
                    localRegionLock = kRegionEdgeLockPx * s;
                }
            }

            float effD = minD - (coincidesWithRoot ? localRootLock : 0.0f);
            if (effD >= 0.0f && effD <= localRegionLock)
            {
                out.Kind = DockDropTarget::TargetKind::RegionSplit;
                out.Path = a->path;
                out.Edge = minEdge;
                return out;
            }
        }
    }
    // Priority 2: Root split by edge-lock only (extreme outer border)
    if (containerW > 0 && containerH > 0)
    {
        const float dl = x - rootL;
        const float dr = (rootL + rootW) - x;
        const float dt = y - rootT;
        const float db = (rootT + rootH) - y;
        float minD = dl;
        DockPosition minEdge = DockPosition::Left;
        if (dr < minD)
        {
            minD = dr;
            minEdge = DockPosition::Right;
        }
        if (dt < minD)
        {
            minD = dt;
            minEdge = DockPosition::Top;
        }
        if (db < minD)
        {
            minD = db;
            minEdge = DockPosition::Bottom;
        }
        if (minD <= kOuterEdgeLockPx)
        {
            out.Kind = DockDropTarget::TargetKind::RootSplit;
            out.Edge = minEdge;
            return out;
        }
    }

    // Priority 3: Leaf split (inner edge inside hovered leaf) – distance-based pick
    if (hoveredLeaf)
    {
        const auto& L = *hoveredLeaf;
        const float innerX = std::max(kInnerEdgePx, L.width * kInnerEdgeFrac);
        const float innerY = std::max(kInnerEdgePx, L.height * kInnerEdgeFrac);
        const float dl = x - L.x;
        const float dr = (L.x + L.width) - x;
        const float dt = y - L.y;
        const float db = (L.y + L.height) - y;
        // Find nearest edge that is within the allowed inner band on that axis
        float bestDist = std::numeric_limits<float>::max();
        DockPosition bestEdge = DockPosition::Left;
        if (dl <= innerX && dl < bestDist)
        {
            bestDist = dl;
            bestEdge = DockPosition::Left;
        }
        if (dr <= innerX && dr < bestDist)
        {
            bestDist = dr;
            bestEdge = DockPosition::Right;
        }
        if (dt <= innerY && dt < bestDist)
        {
            bestDist = dt;
            bestEdge = DockPosition::Top;
        }
        if (db <= innerY && db < bestDist)
        {
            bestDist = db;
            bestEdge = DockPosition::Bottom;
        }
        if (bestDist != std::numeric_limits<float>::max())
        {
            out.Kind = DockDropTarget::TargetKind::LeafSplit;
            out.Path = L.path;
            out.Edge = bestEdge;
            return out;
        }
    }

    // Priority 3c: Center (undock zone) — inner rectangle of the leaf
    if (hoveredLeaf)
    {
        const auto& L = *hoveredLeaf;
        const float centerInsetX = L.width * kCenterInsetFrac;
        const float centerInsetY = L.height * kCenterInsetFrac;
        const float cx = L.x + centerInsetX;
        const float cy = L.y + centerInsetY;
        const float cw = L.width - 2.0f * centerInsetX;
        const float ch = L.height - 2.0f * centerInsetY;
        const bool inCenter = cw > 0 && ch > 0 && PointInRect(x, y, cx, cy, cw, ch);
        if (inCenter)
        {
            out.Kind = DockDropTarget::TargetKind::Tab;
            out.Path = L.path;
            out.Edge = DockPosition::Center;
            return out;
        }
    }

    // Priority 4: (no Root split here; handled earlier)

    // Default: treat as TabBar (dock as tab) into first leaf if available, else None
    if (hoveredLeaf)
    {
        out.Kind = DockDropTarget::TargetKind::TabBar;
        out.Path = hoveredLeaf->path;
        out.Edge = DockPosition::Center;
        return out;
    }

    return out; // None
}

} // namespace GameEngine
