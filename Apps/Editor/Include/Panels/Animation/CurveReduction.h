#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace GameEngine
{
// Evaluate must return the error between the original and proposed curve at T.
// Sampling is always against the original, so accepted removals cannot compound
// the error budget by using an already simplified curve as their reference.
template<class Key, class EvaluateError, class Eligible>
std::vector<Key> ReduceCurveKeys(const std::vector<Key>& original, float maxSpacing,
                                float tolerance, EvaluateError evaluateError, Eligible eligible)
{
    // One copy preserves the reference curve while candidates are edited in place.
    // Returning the named buffer uses NRVO (or a vector move), not a second key copy.
    auto work = original;
    if (work.size() < 3 || !std::isfinite(tolerance) || tolerance < 0.0f || !std::isfinite(maxSpacing))
        return work;
    for (size_t i = 1; i < original.size(); ++i)
        if (!std::isfinite(original[i - 1].time) || !std::isfinite(original[i].time) ||
            original[i].time <= original[i - 1].time) return work;

    bool progressed = true;
    while (progressed)
    {
        progressed = false;
        for (size_t i = 1; i + 1 < work.size();)
        {
            const float start = work[i - 1].time;
            const float end = work[i + 1].time;
            if (!eligible(work[i]) || (maxSpacing > 0.0f && end - start > maxSpacing)) { ++i; continue; }
            const Key removed = work[i];
            work.erase(work.begin() + static_cast<std::ptrdiff_t>(i));
            bool acceptable = true;
            // Include both sides of every original segment (discontinuities)
            // and interior samples (drawn splines can bow between their keys).
            const auto firstRight = std::lower_bound(original.begin() + 1, original.end(), start,
                [](const Key& key, float time) { return key.time < time; });
            for (size_t segment = static_cast<size_t>(firstRight - original.begin());
                 segment < original.size() && acceptable; ++segment)
            {
                const float a = original[segment - 1].time;
                const float b = original[segment].time;
                if (a > end) break;
                for (int step = 0; step <= 16 && acceptable; ++step)
                {
                    const float t = a + (b - a) * (static_cast<float>(step) / 16.0f);
                    const float error = evaluateError(original, work, t);
                    acceptable = std::isfinite(error) && error <= tolerance;
                }
                if (acceptable)
                {
                    const float error = evaluateError(original, work, std::nextafter(b, a));
                    acceptable = std::isfinite(error) && error <= tolerance;
                }
            }
            if (acceptable) progressed = true;
            else { work.insert(work.begin() + static_cast<std::ptrdiff_t>(i), removed); ++i; }
        }
    }
    return work;
}
} // namespace GameEngine
