#include "SplineGeometry/SplineEndTaper.h"

#include "Mathematics/Interpolation.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::SplineGeometry
{

void ApplyEndTaper(std::span<SplineStripStation> stations, const SplineEndTaperParams& params)
{
    if (params.ClosedLoop || stations.size() < 2u)
        return;
    // Off is a full early-out rather than a scale of one: a multiply by 1.0f is
    // bit-exact for finite widths but not for every value a station can carry,
    // and "off changes nothing" is the contract callers are given.
    if (!std::isfinite(params.TaperMetres) || !(params.TaperMetres > 0.0f))
        return;

    const float32 front = stations.front().Distance;
    const float32 back = stations.back().Distance;
    if (!std::isfinite(front) || !std::isfinite(back) || !(back > front))
        return;

    const float32 drop = std::isfinite(params.HeightDrop) ? params.HeightDrop : 0.0f;

    for (SplineStripStation& station : stations)
    {
        // Whichever end is nearer owns this station, which is what lets the two
        // pinches meet on a run too short for both.
        const float32 fromEnd = std::min(station.Distance - front, back - station.Distance);
        // Written as a rejection so a non-finite distance falls through
        // untouched rather than scaling by a NaN.
        if (!(fromEnd < params.TaperMetres))
            continue;

        const float32 scale = Math::SmoothStep(0.0f, params.TaperMetres, fromEnd);
        station.HalfWidthLeft *= scale;
        station.HalfWidthRight *= scale;
        station.Position.y -= drop * (1.0f - scale);
    }
}

} // namespace GameEngine::SplineGeometry
