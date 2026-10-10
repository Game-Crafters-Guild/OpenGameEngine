#include "TerrainECS/TerrainRoutePolyline.h"

#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine::TerrainECS
{

void BuildRoutePolyline(const Spline::SplineData& spline, float32 stationSpacing,
                        RoutePolyline& out)
{
    out.Stations.clear();

    const float32 totalLength = spline.TotalArcLength;
    if (!spline.IsValid() || spline.ArcLengthTable.empty() || !(totalLength > 0.0f))
        return;

    const float32 spacing = std::max(stationSpacing, kMinRouteStationSpacing);

    // Interval count from the route length, then the cap. Capping the COUNT
    // rather than clamping the spacing keeps a legitimately long route from
    // silently changing shape: it widens that one route's spacing instead.
    uint32 intervals = static_cast<uint32>(std::ceil(totalLength / spacing));
    intervals = std::clamp(intervals, 1u, kMaxRouteStations - 1u);

    out.Stations.reserve(static_cast<std::size_t>(intervals) + 1u);
    for (uint32 i = 0; i <= intervals; ++i)
    {
        // The last station is pinned to the route end rather than to i*spacing,
        // so a length that is not a whole multiple of the spacing still grades
        // all the way to its end point.
        const float32 distance = (i == intervals)
            ? totalLength
            : static_cast<float32>(i) * (totalLength / static_cast<float32>(intervals));

        const Spline::SplineFrame frame = Spline::EvaluateAtDistance(spline, distance);
        RouteStation station{};
        station.X = frame.Position.x;
        station.Z = frame.Position.z;
        station.Y = frame.Position.y;
        station.HalfWidth = frame.Radius;
        out.Stations.push_back(station);
    }
}

RouteSample ClosestStationXZ(const RoutePolyline& polyline, float32 worldX, float32 worldZ)
{
    RouteSample sample{};
    sample.Distance = std::numeric_limits<float32>::infinity();

    if (!polyline.IsValid())
        return sample;

    const std::size_t count = polyline.Stations.size();
    for (std::size_t i = 0; i + 1 < count; ++i)
    {
        const RouteStation& a = polyline.Stations[i];
        const RouteStation& b = polyline.Stations[i + 1];

        const float32 dx = b.X - a.X;
        const float32 dz = b.Z - a.Z;
        const float32 lengthSq = dx * dx + dz * dz;
        if (lengthSq < 1e-9f)
            continue; // Coincident stations describe no segment to project onto.

        const float32 t = std::clamp(((worldX - a.X) * dx + (worldZ - a.Z) * dz) / lengthSq,
                                     0.0f, 1.0f);
        const float32 offX = worldX - (a.X + t * dx);
        const float32 offZ = worldZ - (a.Z + t * dz);
        const float32 distance = std::sqrt(offX * offX + offZ * offZ);

        // Strictly nearer, so the FIRST winning segment keeps a tie. A sample
        // equidistant from two segments sits on their shared station, where both
        // interpolate to the same grade and width anyway.
        if (distance < sample.Distance)
        {
            sample.Distance = distance;
            sample.Height = a.Y + t * (b.Y - a.Y);
            sample.HalfWidth = a.HalfWidth + t * (b.HalfWidth - a.HalfWidth);
        }
    }

    return sample;
}

void RoutePolylineBoundsXZ(const RoutePolyline& polyline, float32 falloff,
                           float32& outMinX, float32& outMinZ,
                           float32& outMaxX, float32& outMaxZ)
{
    outMinX = std::numeric_limits<float32>::max();
    outMinZ = std::numeric_limits<float32>::max();
    outMaxX = -std::numeric_limits<float32>::max();
    outMaxZ = -std::numeric_limits<float32>::max();

    if (!polyline.IsValid())
        return;

    for (const RouteStation& station : polyline.Stations)
    {
        // Each station's own half-width, not the route's largest: the box is a
        // union of per-station discs, which is tight on a route that tapers.
        const float32 reach = station.HalfWidth + falloff;
        outMinX = std::min(outMinX, station.X - reach);
        outMinZ = std::min(outMinZ, station.Z - reach);
        outMaxX = std::max(outMaxX, station.X + reach);
        outMaxZ = std::max(outMaxZ, station.Z + reach);
    }
}

} // namespace GameEngine::TerrainECS
