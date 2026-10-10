#include "SplineGeometry/SplineFillField.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

namespace GameEngine::SplineGeometry
{
namespace
{

using V3 = Mathematics::Vector3;

// Waterline at an arc position, linearly interpolated between the two stations
// that bracket it and CLAMPED at both ends.
//
// The clamp is what gives the run a flat end rather than a downhill
// extrapolation: past the last station the field holds the last station's
// height, so a fill that reaches open ground stops where the ground rises to
// that height instead of chasing the slope away.
float32 WaterlineAtArc(std::span<const SplineStripStation> stations, float32 arc)
{
    if (arc <= stations.front().Distance)
        return stations.front().Position.y;
    if (arc >= stations.back().Distance)
        return stations.back().Position.y;

    // Stations are ascending in Distance, so a binary search finds the bracket.
    const auto upper = std::upper_bound(
        stations.begin(), stations.end(), arc,
        [](float32 value, const SplineStripStation& s) { return value < s.Distance; });
    const size_t hi = static_cast<size_t>(upper - stations.begin());
    const SplineStripStation& a = stations[hi - 1u];
    const SplineStripStation& b = stations[hi];
    const float32 span = b.Distance - a.Distance;
    if (!(span > 0.0f))
        return a.Position.y;
    const float32 t = (arc - a.Distance) / span;
    return a.Position.y + (b.Position.y - a.Position.y) * t;
}

// Horizontal unit direction, falling back when a station's frame is degenerate
// in plan view (a run going straight up has no XZ travel).
void PlanarFrame(const SplineStripStation& station, float32& fx, float32& fz, float32& rx,
                 float32& rz)
{
    fx = station.Forward.x;
    fz = station.Forward.z;
    const float32 fLen = std::sqrt(fx * fx + fz * fz);
    if (fLen > 1.0e-6f)
    {
        fx /= fLen;
        fz /= fLen;
    }
    else
    {
        fx = 0.0f;
        fz = 1.0f;
    }

    rx = station.Right.x;
    rz = station.Right.z;
    const float32 rLen = std::sqrt(rx * rx + rz * rz);
    if (rLen > 1.0e-6f)
    {
        rx /= rLen;
        rz /= rLen;
    }
    else
    {
        // Right of travel in this LH +Y-up engine, matching the convention the
        // placement recipes use so a filled region and a placed path agree
        // about sides.
        rx = -fz;
        rz = fx;
    }
}

} // namespace

SplineFillResult BuildSplineFillField(std::span<const SplineStripStation> stations,
                                      const SplineGroundGrid& ground,
                                      const SplineFillParams& params)
{
    SplineFillResult result;
    if (stations.empty() || !ground.IsValid())
        return result;

    const float32 reach = std::max(0.0f, params.MaxHalfWidth);
    const float32 tuck = std::max(0.0f, params.EdgeDrop);
    if (!(reach > 0.0f))
        return result;

    // The floor stands a hair above the level it names, so a mouth that merges
    // with a sea sits ON it rather than IN its surface (kSeaLevelFloorLiftMetres).
    const float32 seaFloor = params.SeaLevelFloor + kSeaLevelFloorLiftMetres;

    const uint32 cornerCount = ground.CornerCount();
    result.Corners.assign(cornerCount, SplineFillCorner{});

    // ---- Stamp: nearest station per corner, over a corridor plus one cell ----
    //
    // The halo matters. A corner just OUTSIDE the corridor still needs a
    // waterline and a distance, because the mesher interpolates the region
    // boundary across the edge that leaves the corridor; an unstamped corner
    // would collapse that crossing onto the lattice and put the staircase back.
    const float32 halo = std::sqrt(ground.SpacingX * ground.SpacingX +
                                   ground.SpacingZ * ground.SpacingZ);
    const float32 stampReach = reach + halo;
    const float32 stampReachSq = stampReach * stampReach;

    // The stamp keeps SQUARED distances while it runs and takes one square root
    // per corner at the end. Station spacing is a fraction of the corridor
    // radius, so every corner falls inside dozens of stations' discs and all but
    // one of those visits only ever loses a comparison -- comparing squares
    // makes the losing visit two multiplies and a branch instead of a sqrt.
    std::vector<float32> bestSquared(cornerCount, std::numeric_limits<float32>::infinity());

    for (uint32 s = 0; s < stations.size(); ++s)
    {
        const SplineStripStation& station = stations[s];
        const float32 sx = station.Position.x;
        const float32 sz = station.Position.z;

        // Corner window covering the stamp disc around this station.
        const float32 minX = sx - stampReach;
        const float32 maxX = sx + stampReach;
        const float32 minZ = sz - stampReach;
        const float32 maxZ = sz + stampReach;
        int32 x0 = static_cast<int32>(std::floor((minX - ground.OriginX) / ground.SpacingX));
        int32 x1 = static_cast<int32>(std::ceil((maxX - ground.OriginX) / ground.SpacingX));
        int32 z0 = static_cast<int32>(std::floor((minZ - ground.OriginZ) / ground.SpacingZ));
        int32 z1 = static_cast<int32>(std::ceil((maxZ - ground.OriginZ) / ground.SpacingZ));
        x0 = std::max(x0, 0);
        z0 = std::max(z0, 0);
        x1 = std::min(x1, static_cast<int32>(ground.CountX) - 1);
        z1 = std::min(z1, static_cast<int32>(ground.CountZ) - 1);
        if (x1 < x0 || z1 < z0)
            continue;

        float32 fx, fz, rx, rz;
        PlanarFrame(station, fx, fz, rx, rz);

        for (int32 cz = z0; cz <= z1; ++cz)
        {
            const float32 dz = ground.WorldZ(static_cast<uint32>(cz)) - sz;
            const float32 dzSquared = dz * dz;
            if (dzSquared > stampReachSq)
                continue;

            // Column span of the DISC on this row, so the inner loop walks only
            // corners that are already inside it. One square root per row
            // replaces a rejected test on roughly a fifth of the square window.
            const float32 halfSpan = std::sqrt(stampReachSq - dzSquared);
            int32 rx0 = static_cast<int32>(
                std::ceil((sx - halfSpan - ground.OriginX) / ground.SpacingX));
            int32 rx1 = static_cast<int32>(
                std::floor((sx + halfSpan - ground.OriginX) / ground.SpacingX));
            rx0 = std::max(rx0, x0);
            rx1 = std::min(rx1, x1);
            if (rx1 < rx0)
                continue;

            const uint32 rowBase = static_cast<uint32>(cz) * ground.CountX;
            float32 dx = ground.WorldX(static_cast<uint32>(rx0)) - sx;
            for (int32 cx = rx0; cx <= rx1; ++cx, dx += ground.SpacingX)
            {
                const float32 squared = dx * dx + dzSquared;
                const uint32 index = rowBase + static_cast<uint32>(cx);
                if (squared >= bestSquared[index])
                    continue;
                bestSquared[index] = squared;

                SplineFillCorner& corner = result.Corners[index];
                corner.Station = s;
                // Project into the station's own frame, so U and V are
                // continuous WITHIN a station's territory rather than
                // quantised to the station spacing.
                corner.ArcDistance = station.Distance + (dx * fx + dz * fz);
                corner.LateralSigned = dx * rx + dz * rz;
            }
        }
    }

    for (uint32 i = 0; i < cornerCount; ++i)
        if (result.Corners[i].Station != kNoStation)
            result.Corners[i].Distance = std::sqrt(bestSquared[i]);

    // ---- Escape elevation: the bank the water would have to be over ----
    //
    // Solved by one sweep in decreasing Distance. Every dependency of a corner
    // is a neighbour FURTHER from the centreline, so that order is a topological
    // order of the dependency graph and each corner settles in a single visit --
    // no priority queue, and no iteration to a fixed point.
    // Sorted as (distance, index) packed into one 64-bit word rather than as
    // indices with a comparator: the IEEE-754 bit pattern of a non-negative
    // float orders like the float, so the sort is over plain integers with no
    // callback and no random access into the corner array.
    std::vector<uint64> outward;
    outward.reserve(cornerCount);
    for (uint32 i = 0; i < cornerCount; ++i)
    {
        SplineFillCorner& corner = result.Corners[i];
        if (corner.Distance > reach)
        {
            // Already outside the corridor: the water has left the domain here,
            // and the last rim it cleared is the ground it is standing on. The
            // corridor is where the fill stops LOOKING, so a rim decided out
            // here is a rim the run cannot actually see.
            corner.Escape = ground.Heights[i];
            corner.EscapeUnseen = 1u;
            continue;
        }
        uint32 key;
        std::memcpy(&key, &corner.Distance, sizeof(key));
        outward.push_back((static_cast<uint64>(key) << 32) | i);
    }
    // Descending in distance: the far corners settle first, and every corner's
    // dependencies are corners further out.
    std::sort(outward.begin(), outward.end(), std::greater<uint64>());

    for (uint64 packed : outward)
    {
        const uint32 index = static_cast<uint32>(packed);
        const float32 here = result.Corners[index].Distance;
        float32 onward = std::numeric_limits<float32>::infinity();
        uint8 onwardUnseen = 0u;
        const uint32 cx = index % ground.CountX;
        const uint32 cz = index / ground.CountX;
        const auto consider = [&](uint32 other)
        {
            const SplineFillCorner& n = result.Corners[other];
            if (n.Distance > here && n.Escape < onward)
            {
                onward = n.Escape;
                onwardUnseen = n.EscapeUnseen;
            }
        };
        if (cx > 0u)
            consider(index - 1u);
        if (cx + 1u < ground.CountX)
            consider(index + 1u);
        if (cz > 0u)
            consider(index - ground.CountX);
        if (cz + 1u < ground.CountZ)
            consider(index + ground.CountX);

        // No outward route at all leaves it at infinity: a pocket that drains
        // only back into the river is held by the river, and must not be pared.
        SplineFillCorner& corner = result.Corners[index];
        if (ground.Heights[index] > onward)
        {
            // The highest ground on the way out is HERE, inside the corridor:
            // the rim is real terrain the run can see.
            corner.Escape = ground.Heights[index];
            corner.EscapeUnseen = 0u;
        }
        else
        {
            corner.Escape = onward;
            corner.EscapeUnseen = onwardUnseen;
        }
    }

    // ---- Waterline, then the inside-ness the mesher interpolates ----
    for (uint32 i = 0; i < cornerCount; ++i)
    {
        SplineFillCorner& corner = result.Corners[i];
        if (corner.Station == kNoStation)
            continue;

        corner.Waterline = std::max(WaterlineAtArc(stations, corner.ArcDistance), seaFloor);

        // Three constraints intersected, each in metres so the mesher can
        // interpolate their minimum to a sub-cell shoreline: the ground is below
        // the water, the corridor has not run out, and the water is not standing
        // over a bank it could flow across.
        //
        // The containment term EXCLUDES water rather than lowering it. Lowering
        // would leave a zero-depth film coplanar with the terrain -- and would
        // break the invariant the whole feature rests on, that the surface height
        // is an INPUT the fill reads and never writes.
        //
        // A sea level contains water the way a bank does, so it joins the escape
        // rather than being overruled by it: a mouth that merges with a sea is
        // held up by the sea.
        const float32 held = std::max(corner.Escape, seaFloor);
        const float32 phi = corner.Waterline - ground.Heights[i];
        corner.Inside =
            std::min({phi + tuck, reach - corner.Distance, held - corner.Waterline});
    }
    // Which corners the containment term actually REFUSED is not known here: one
    // the region encloses is taken back below. The count runs after the seal.

    // ---- Seeds: the station cells, at each station's own waterline ----
    std::vector<uint32> queue;
    const auto seed = [&](uint32 index)
    {
        SplineFillCorner& corner = result.Corners[index];
        if (corner.Wet || !(corner.Inside > 0.0f))
            return;
        corner.Wet = 1u;
        queue.push_back(index);
    };

    for (const SplineStripStation& station : stations)
    {
        const int32 cx = static_cast<int32>(
            std::lround((station.Position.x - ground.OriginX) / ground.SpacingX));
        const int32 cz = static_cast<int32>(
            std::lround((station.Position.z - ground.OriginZ) / ground.SpacingZ));
        if (cx < 0 || cz < 0 || cx >= static_cast<int32>(ground.CountX) ||
            cz >= static_cast<int32>(ground.CountZ))
            continue;

        ++result.Diagnostics.Seeds;
        const uint32 index = ground.Index(static_cast<uint32>(cx), static_cast<uint32>(cz));
        // Dry means the BED is not below the waterline here -- the thing an
        // author can act on. It is not the same test as the flood's, which also
        // carries the corridor, the tuck and the containment.
        if (ground.Heights[index] >= result.Corners[index].Waterline)
        {
            if (result.Diagnostics.DrySeeds == 0u)
                result.Diagnostics.FirstDrySeed = station.Position;
            ++result.Diagnostics.DrySeeds;
        }
        seed(index);
    }

    // ---- Flood: 4-connected, from the seeds, through Inside > 0 ----
    for (size_t head = 0; head < queue.size(); ++head)
    {
        const uint32 index = queue[head];
        const uint32 cx = index % ground.CountX;
        const uint32 cz = index / ground.CountX;

        if (cx > 0u)
            seed(index - 1u);
        if (cx + 1u < ground.CountX)
            seed(index + 1u);
        if (cz > 0u)
            seed(index - ground.CountX);
        if (cz + 1u < ground.CountZ)
            seed(index + ground.CountX);

        if (queue.size() > params.MaxWetCorners)
        {
            // Refuse rather than coarsen: the caller builds nothing and says so.
            result.Corners.assign(cornerCount, SplineFillCorner{});
            result.Diagnostics = SplineFillDiagnostics{};
            result.Diagnostics.ExceededBudget = true;
            return result;
        }
    }

    // ---- Seal: a dry pocket the water encloses has nowhere to drain ----
    //
    // The containment term asks whether a drop here could run outward over a
    // bank lower than itself. That question presumes the outside is reachable,
    // and for a corner ringed by water on every side it is not: the escape route
    // it was refused for runs through the very water the refusal removed. The
    // monotone outward scan cannot see that, because it is answered by the
    // settled region's TOPOLOGY rather than by any one corner's surroundings.
    //
    // Reachability decides only which corners the containment term governs. The
    // ground and corridor constraints still decide whether a corner holds water,
    // so ground standing above its own waterline stays a dry-topped islet, and a
    // dip at the region's EDGE — where the outside genuinely is reachable —
    // still takes its bite out of the bank.
    std::vector<uint8> drains(cornerCount, 0u);
    std::vector<uint32> dryQueue;
    const auto reachDry = [&](uint32 index)
    {
        if (drains[index] != 0u || result.Corners[index].Wet != 0u)
            return;
        drains[index] = 1u;
        dryQueue.push_back(index);
    };
    for (uint32 cx = 0; cx < ground.CountX; ++cx)
    {
        reachDry(ground.Index(cx, 0u));
        reachDry(ground.Index(cx, ground.CountZ - 1u));
    }
    for (uint32 cz = 0; cz < ground.CountZ; ++cz)
    {
        reachDry(ground.Index(0u, cz));
        reachDry(ground.Index(ground.CountX - 1u, cz));
    }
    for (size_t head = 0; head < dryQueue.size(); ++head)
    {
        const uint32 index = dryQueue[head];
        const uint32 cx = index % ground.CountX;
        const uint32 cz = index / ground.CountX;
        if (cx > 0u)
            reachDry(index - 1u);
        if (cx + 1u < ground.CountX)
            reachDry(index + 1u);
        if (cz > 0u)
            reachDry(index - ground.CountX);
        if (cz + 1u < ground.CountZ)
            reachDry(index + ground.CountX);
    }

    for (uint32 index = 0; index < cornerCount; ++index)
    {
        SplineFillCorner& corner = result.Corners[index];
        if (corner.Wet != 0u || drains[index] != 0u || corner.Station == kNoStation)
            continue;
        // The same field, minus the one term reachability just disqualified.
        const float32 inside = std::min(corner.Waterline - ground.Heights[index] + tuck,
                                        reach - corner.Distance);
        if (!(inside > 0.0f))
            continue;
        corner.Inside = inside;
        corner.Wet = 1u;
        queue.push_back(index);
        ++result.Diagnostics.SealedPocketCorners;
    }

    if (queue.size() > params.MaxWetCorners)
    {
        result.Corners.assign(cornerCount, SplineFillCorner{});
        result.Diagnostics = SplineFillDiagnostics{};
        result.Diagnostics.ExceededBudget = true;
        return result;
    }

    result.Diagnostics.WetCorners = static_cast<uint32>(queue.size());

    // ---- What the containment term finally refused ----
    //
    // Counted over the corners that are still dry, so a sealed pocket is not
    // reported as a surface that stopped at a bank it did not stop at.
    //
    // The worst overshoot of the unseen subset has no counter of its own -- the
    // warning quotes the position, not the magnitude -- so it is ranked here.
    float32 worstUnseenMetres = 0.0f;
    for (uint32 index = 0; index < cornerCount; ++index)
    {
        const SplineFillCorner& corner = result.Corners[index];
        if (corner.Wet != 0u || corner.Station == kNoStation)
            continue;
        const float32 held = std::max(corner.Escape, seaFloor);
        if (!(corner.Waterline - ground.Heights[index] + tuck > 0.0f) ||
            !(reach - corner.Distance > 0.0f) || held - corner.Waterline > 0.0f)
            continue;
        // Non-negative by the guard above, so the first refusal seeds the
        // ranking rather than losing to the zero it starts at: a run whose
        // banks are exactly at the waterline still names a sample.
        const float32 over = corner.Waterline - held;
        const SplineFillWorstSample here{ground.WorldX(index % ground.CountX),
                                         ground.WorldZ(index / ground.CountX),
                                         corner.ArcDistance};
        if (result.Diagnostics.OverBankCorners == 0u ||
            over > result.Diagnostics.MaxOverBankMetres)
        {
            result.Diagnostics.MaxOverBankMetres = over;
            result.Diagnostics.WorstOverBank = here;
        }
        ++result.Diagnostics.OverBankCorners;
        // The rim that let it go was outside the corridor, so the run never saw
        // the real bank: a wider MaxHalfWidth, not a deeper carve.
        if (corner.EscapeUnseen == 0u)
            continue;
        if (result.Diagnostics.UnseenBankCorners == 0u || over > worstUnseenMetres)
        {
            worstUnseenMetres = over;
            result.Diagnostics.WorstUnseenBank = here;
        }
        ++result.Diagnostics.UnseenBankCorners;
    }

    // ---- Diagnostics over the settled region ----
    for (uint32 index : queue)
    {
        const SplineFillCorner& corner = result.Corners[index];
        const float32 depth = corner.Waterline - ground.Heights[index];
        result.Diagnostics.MaxDepthMetres = std::max(result.Diagnostics.MaxDepthMetres, depth);


        const uint32 cx = index % ground.CountX;
        const uint32 cz = index / ground.CountX;
        const auto neighbour = [&](uint32 other)
        {
            const SplineFillCorner& n = result.Corners[other];
            if (n.Wet)
                result.Diagnostics.MedialStepMetres =
                    std::max(result.Diagnostics.MedialStepMetres,
                             std::abs(n.Waterline - corner.Waterline));
        };
        if (cx > 0u)
            neighbour(index - 1u);
        if (cx + 1u < ground.CountX)
            neighbour(index + 1u);
        if (cz > 0u)
            neighbour(index - ground.CountX);
        if (cz + 1u < ground.CountZ)
            neighbour(index + ground.CountX);
    }

    // ---- Validate the waterline instead of clamping it ----
    //
    // Clamping a rise would silently move water the author placed; the case it
    // would "fix" is an authoring error that should be visible.
    for (size_t i = 1; i < stations.size(); ++i)
    {
        const float32 rise = stations[i].Position.y - stations[i - 1u].Position.y;
        if (rise > result.Diagnostics.WaterlineRiseMetres)
        {
            result.Diagnostics.WaterlineRiseMetres = rise;
            result.Diagnostics.WaterlineRiseAtArc = stations[i].Distance;
        }
    }

    return result;
}

} // namespace GameEngine::SplineGeometry
