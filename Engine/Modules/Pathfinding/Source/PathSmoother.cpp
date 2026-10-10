#include "PathSmoother.h"
#include "Pathfinding/GridMap.h"

namespace GameEngine::Pathfinding
{

uint32 PathSmoother::Smooth(const GridMap& grid, std::vector<PathPoint>& points)
{
    if (points.size() <= 2)
        return static_cast<uint32>(points.size());

    std::vector<PathPoint> smoothed;
    smoothed.reserve(points.size());
    smoothed.push_back(points.front());

    uint32 anchor = 0;
    uint32 current = 1;

    while (current < static_cast<uint32>(points.size()))
    {
        uint32 farthestVisible = current;

        for (uint32 test = current + 1; test < static_cast<uint32>(points.size()); ++test)
        {
            float32 hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
            bool blocked = grid.Raycast(
                points[anchor].X, points[anchor].Y, points[anchor].Z,
                points[test].X, points[test].Y, points[test].Z,
                hitX, hitY, hitZ);

            if (!blocked)
                farthestVisible = test;
            else
                break;
        }

        smoothed.push_back(points[farthestVisible]);
        anchor = farthestVisible;
        current = farthestVisible + 1;
    }

    // Ensure the last point is always included
    const auto& last = points.back();
    const auto& smoothedLast = smoothed.back();
    if (smoothedLast.X != last.X || smoothedLast.Y != last.Y || smoothedLast.Z != last.Z)
    {
        smoothed.push_back(last);
    }

    points = std::move(smoothed);
    return static_cast<uint32>(points.size());
}

} // namespace GameEngine::Pathfinding
