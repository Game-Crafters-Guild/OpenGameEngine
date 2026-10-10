#pragma once

#include "Mathematics/Vector3.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace GameEngine::Spline
{

// ---------------------------------------------------------------------------
// Polyline simplification (Ramer–Douglas–Peucker)
// ---------------------------------------------------------------------------

// Point-to-line-segment distance squared (3D).
inline float32 PointToSegmentDistSq(const Mathematics::Vector3& p,
                                    const Mathematics::Vector3& a,
                                    const Mathematics::Vector3& b)
{
    Mathematics::Vector3 ab = b - a;
    float32 abLenSq = Mathematics::Vector3::Dot(ab, ab);
    if (abLenSq < 1e-12f)
        return Mathematics::Vector3::Dot(p - a, p - a);
    float32 t = std::clamp(Mathematics::Vector3::Dot(p - a, ab) / abLenSq, 0.0f, 1.0f);
    Mathematics::Vector3 proj = a + ab * t;
    Mathematics::Vector3 diff = p - proj;
    return Mathematics::Vector3::Dot(diff, diff);
}

// Ramer–Douglas–Peucker polyline simplification, reporting the KEPT INDICES
// into `input` (strictly increasing; first and last are always kept). The
// index form is the primitive so callers carrying per-sample payloads (spline
// frames, channel values) can map each kept point back to its source sample
// exactly. Preserves shape within `tolerance` (world units); iterative stack
// to avoid recursion.
inline void SimplifyPolylineIndices(const std::vector<Mathematics::Vector3>& input,
                                    float32 tolerance,
                                    std::vector<uint32>& outKeptIndices)
{
    outKeptIndices.clear();
    if (input.empty())
        return;
    if (input.size() <= 2)
    {
        for (uint32 i = 0; i < static_cast<uint32>(input.size()); ++i)
            outKeptIndices.push_back(i);
        return;
    }

    const float32 toleranceSq = tolerance * tolerance;

    struct Range { uint32 start; uint32 end; };
    std::vector<Range> stack;
    std::vector<bool> keep(input.size(), false);

    keep[0] = true;
    keep[input.size() - 1] = true;
    stack.push_back({0, static_cast<uint32>(input.size() - 1)});

    while (!stack.empty())
    {
        auto [start, end] = stack.back();
        stack.pop_back();

        if (end <= start + 1)
            continue;

        float32 maxDistSq = 0.0f;
        uint32 maxIdx = start;

        for (uint32 i = start + 1; i < end; ++i)
        {
            float32 distSq = PointToSegmentDistSq(input[i], input[start], input[end]);
            if (distSq > maxDistSq)
            {
                maxDistSq = distSq;
                maxIdx = i;
            }
        }

        if (maxDistSq > toleranceSq)
        {
            keep[maxIdx] = true;
            stack.push_back({start, maxIdx});
            stack.push_back({maxIdx, end});
        }
    }

    outKeptIndices.reserve(input.size());
    for (uint32 i = 0; i < static_cast<uint32>(input.size()); ++i)
    {
        if (keep[i])
            outKeptIndices.push_back(i);
    }
}

// Position-only convenience over SimplifyPolylineIndices for callers with no
// per-sample payload. Output is replaced, not appended.
inline void SimplifyPolyline(const std::vector<Mathematics::Vector3>& input,
                             float32 tolerance,
                             std::vector<Mathematics::Vector3>& output)
{
    std::vector<uint32> kept;
    SimplifyPolylineIndices(input, tolerance, kept);
    output.clear();
    output.reserve(kept.size());
    for (uint32 index : kept)
        output.push_back(input[index]);
}

} // namespace GameEngine::Spline