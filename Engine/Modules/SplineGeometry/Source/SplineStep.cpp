#include "SplineGeometry/SplineStep.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine::SplineGeometry
{
namespace
{

using V3 = Mathematics::Vector3;

// A station this close to a point's Distance stands on that point.
constexpr float32 kOnPointMetres = 1.0e-4f;

// Stand a ring plumb: its Up is the frame's vertical and its Forward the level
// direction of travel. Right is already level.
void Plumb(SplineStripStation& station, const V3& up)
{
    const V3 level = station.Forward - up * V3::Dot(station.Forward, up);
    const float32 length = std::sqrt(V3::Dot(level, level));
    if (length > 1.0e-6f)
        station.Forward = level * (1.0f / length);
    station.Up = up;
}

// The TopOffset that puts a plumb station's top at `level` plus the wall's
// height: the height it stands above the station's own.
[[nodiscard]] float32 TopOffsetFor(const SplineStripStation& station, float32 level, const V3& up)
{
    return level - V3::Dot(station.Position, up);
}

} // namespace

void StepTopsAtPoints(std::vector<SplineStripStation>& stations,
                      std::span<const float32> pointDistances, const Mathematics::Vector3& up,
                      bool closedLoop)
{
    const size_t count = stations.size();
    if (count < 2u)
        return;
    // A stepped wall stands plumb, so each top rises straight above its base.
    // A ring left leaning with the grade would carry its raised top along the
    // run as well as up, by a different amount at every ring, and fan the side
    // faces between them.
    for (SplineStripStation& station : stations)
        Plumb(station, up);
    const size_t pointCount = pointDistances.size();
    const size_t runCount = pointCount + 1u;

    // Each station's run, and the point it stands on (-1 for none). A station
    // on point k starts in run k, the run it ends; a Crease ring there, and any
    // ring after it on the same point, is the next run's.
    std::vector<size_t> run(count, 0u);
    std::vector<int32> onPoint(count, -1);
    size_t next = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const float32 distance = stations[i].Distance;
        while (next < pointCount && distance > pointDistances[next] + kOnPointMetres)
            ++next;
        run[i] = next;
        if (next < pointCount && std::abs(distance - pointDistances[next]) <= kOnPointMetres)
            onPoint[i] = static_cast<int32>(next);
    }
    for (size_t i = 1; i < count; ++i)
    {
        if (onPoint[i] >= 0 && onPoint[i] == onPoint[i - 1u] &&
            (stations[i].Join == SplineStationJoin::Crease || run[i - 1u] > run[i]))
            run[i] = static_cast<size_t>(onPoint[i]) + 1u;
    }

    // Each run's level: its highest station, counting the stations on the
    // points at both of its ends.
    std::vector<float32> level(runCount, std::numeric_limits<float32>::lowest());
    for (size_t i = 0; i < count; ++i)
    {
        const float32 height = V3::Dot(stations[i].Position, up);
        level[run[i]] = std::max(level[run[i]], height);
        if (onPoint[i] >= 0)
        {
            const size_t k = static_cast<size_t>(onPoint[i]);
            level[k] = std::max(level[k], height);
            level[k + 1u] = std::max(level[k + 1u], height);
        }
    }

    std::vector<SplineStripStation> stepped;
    stepped.reserve(count + runCount);
    size_t i = 0;
    while (i < count)
    {
        if (onPoint[i] < 0)
        {
            stepped.push_back(stations[i]);
            stepped.back().TopOffset = TopOffsetFor(stations[i], level[run[i]], up);
            ++i;
            continue;
        }

        // The stations standing on one point.
        const size_t first = i;
        size_t last = i;
        while (last + 1u < count && onPoint[last + 1u] == onPoint[first])
            ++last;
        i = last + 1u;
        const size_t k = static_cast<size_t>(onPoint[first]);
        const float32 before = level[k];
        const float32 after = level[k + 1u];

        // A Mitre already splits into the run before and the run after at its
        // Crease ring.
        const bool splits = run[last] != run[first];
        if (splits)
        {
            for (size_t s = first; s <= last; ++s)
            {
                stepped.push_back(stations[s]);
                stepped.back().TopOffset = TopOffsetFor(stations[s], level[run[s]], up);
            }
            continue;
        }

        // Anything else on the point stands at the higher level, and a Crease
        // copy on the lower side carries the step.
        const float32 high = std::max(before, after);
        if (before < after)
        {
            stepped.push_back(stations[first]);
            stepped.back().TopOffset = TopOffsetFor(stations[first], before, up);
        }
        for (size_t s = first; s <= last; ++s)
        {
            stepped.push_back(stations[s]);
            if (s == first && before < after)
                stepped.back().Join = SplineStationJoin::Crease;
            stepped.back().TopOffset = TopOffsetFor(stations[s], high, up);
        }
        if (before > after)
        {
            stepped.push_back(stations[last]);
            stepped.back().Join = SplineStationJoin::Crease;
            stepped.back().TopOffset = TopOffsetFor(stations[last], after, up);
        }
    }

    // A welded loop's seam is a step when its first and last runs stand apart.
    if (closedLoop && level.front() != level.back())
    {
        SplineStripStation seam = stepped.back();
        seam.Join = SplineStationJoin::Crease;
        seam.TopOffset = TopOffsetFor(seam, level.front(), up);
        stepped.push_back(seam);
    }
    stations = std::move(stepped);
}

} // namespace GameEngine::SplineGeometry
