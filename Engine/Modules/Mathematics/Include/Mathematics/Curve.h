#pragma once

#include "Mathematics/Interpolation.h" // Lerp

#include <algorithm> // std::max in ResolveKeySlope
#include <cassert>
#include <cmath>     // std::log2, std::exp2 in EvaluateCurveKeysLogarithmic
#include <cstdint>
#include <type_traits>
#include <vector>

namespace GameEngine::Math
{

// How the segment from a key to the NEXT key is interpolated. Tangents on the bracketing
// keys are consulted only for Smooth segments.
enum class CurveInterp : uint8_t
{
    Linear = 0,
    Smooth = 1,
    Step = 2,
};

// How a Smooth key's tangents are authored. `Auto` derives the slope from neighbours
// each evaluation; the others read the stored In/OutTangent the editor wrote. `Flat`
// forces zero slope. Only consulted on Smooth segments.
enum class CurveTangentMode : uint8_t
{
    Auto = 0,
    Flat = 1,
    Broken = 2,   // in/out independent
    Manual = 3,   // in/out dragged together
    Mirrored = 4, // in == out
};

// One keyframe. Times are sorted ascending; by convention the curve domain is [0, 1] and
// the consumer maps it to its own units (seconds, hours, lifetime fraction). `Interp`
// selects the interpolation of the segment that STARTS at this key. `OutTangent` /
// `InTangent` are value-over-time slopes used by Smooth segments (the segment uses the
// left key's `OutTangent` and the right key's `InTangent`).
struct CurveKey
{
    float Time = 0.0f;
    float Value = 0.0f;
    float InTangent = 0.0f;
    float OutTangent = 0.0f;
    CurveInterp Interp = CurveInterp::Linear;
    CurveTangentMode TangentMode = CurveTangentMode::Auto;
    uint8_t _Pad[2] = {0, 0};
};

// Resolve a key's tangent slope (value/time units) per its TangentMode. `incoming` picks
// the in- vs out-tangent for Broken keys; Auto derives a Catmull-Rom-style slope from the
// surrounding keys. Keys are assumed Time-sorted.
inline float ResolveKeySlope(const CurveKey* keys, uint32_t count, uint32_t index, bool incoming)
{
    const CurveKey& k = keys[index];
    switch (k.TangentMode)
    {
        case CurveTangentMode::Flat:
            return 0.0f;
        case CurveTangentMode::Broken:
        case CurveTangentMode::Manual:
        case CurveTangentMode::Mirrored:
            return incoming ? k.InTangent : k.OutTangent;
        case CurveTangentMode::Auto:
        default:
            break;
    }
    if (count < 2)
        return 0.0f;
    if (index == 0)
        return (keys[1].Value - k.Value) / std::max(1e-4f, keys[1].Time - k.Time);
    if (index + 1 >= count)
        return (k.Value - keys[index - 1].Value) / std::max(1e-4f, k.Time - keys[index - 1].Time);
    return (keys[index + 1].Value - keys[index - 1].Value) /
           std::max(1e-4f, keys[index + 1].Time - keys[index - 1].Time);
}

// Evaluate a Time-sorted key array at `t`, clamped to the key range (flat before the first
// key and after the last). The single evaluator shared by every curve container so fixed
// and growable curves never diverge.
inline float EvaluateCurveKeys(const CurveKey* keys, uint32_t count, float t)
{
    if (count == 0)
        return 0.0f;
    if (count == 1 || t <= keys[0].Time)
        return keys[0].Value;
    if (t >= keys[count - 1].Time)
        return keys[count - 1].Value;

    uint32_t i = 1;
    while (i < count && t > keys[i].Time)
        ++i;

    const CurveKey& a = keys[i - 1];
    const CurveKey& b = keys[i];
    const float span = b.Time - a.Time;
    if (span <= 1e-6f)
        return b.Value;
    const float u = (t - a.Time) / span;

    switch (a.Interp)
    {
        case CurveInterp::Step:
            return a.Value;
        case CurveInterp::Smooth:
        {
            // Cubic Hermite; tangents are scaled by the segment width so they read as
            // value/time slopes regardless of spacing.
            const float u2 = u * u;
            const float u3 = u2 * u;
            const float m0 = ResolveKeySlope(keys, count, i - 1, false) * span;
            const float m1 = ResolveKeySlope(keys, count, i, true) * span;
            return (2.0f * u3 - 3.0f * u2 + 1.0f) * a.Value + (u3 - 2.0f * u2 + u) * m0 +
                   (-2.0f * u3 + 3.0f * u2) * b.Value + (u3 - u2) * m1;
        }
        case CurveInterp::Linear:
        default:
            return Lerp(a.Value, b.Value, u);
    }
}

// Evaluate a cyclic curve over a span of `period` (e.g. 24 hours, or one animation loop): inside the
// key time range it is the normal EvaluateCurveKeys; outside, it linearly closes the loop from the
// last key across the wrap gap back to the first. The single wrapped evaluator the runtime and the
// editor preview both call, so a cyclic curve renders the same way it is evaluated.
inline float EvaluateCurveKeysWrapped(const CurveKey* keys, uint32_t count, float t, float period)
{
    if (count >= 2)
    {
        const float first = keys[0].Time;
        const float last = keys[count - 1].Time;
        if (t < first || t > last)
        {
            const float wrapSpan = (first + period) - last;
            if (wrapSpan <= 1e-6f)
                return keys[0].Value;
            const float elapsed = (t > last) ? (t - last) : (t + period - last);
            return Lerp(keys[count - 1].Value, keys[0].Value, std::clamp(elapsed / wrapSpan, 0.0f, 1.0f));
        }
    }
    return EvaluateCurveKeys(keys, count, t);
}

namespace CurveLogarithm
{
// log2 of a key value, with every value at or below `floorValue` (zero, negatives, NaN) taken as the floor.
inline float LogOfValue(float value, float floorValue)
{
    return std::log2(value > floorValue ? value : floorValue);
}

// A value the logarithmic evaluator returns: anything at or below the floor reads as 0.
inline float ValueOrZero(float value, float floorValue)
{
    return value > floorValue ? value : 0.0f;
}

// The value of an interpolated log2, compared in the log domain so a segment between two keys at the
// floor reads 0 even where exp2 rounds a hair above it.
inline float ValueOfLog(float logValue, float floorValue)
{
    return logValue > std::log2(floorValue) ? std::exp2(logValue) : 0.0f;
}
} // namespace CurveLogarithm

// Evaluate a Time-sorted key array whose values span orders of magnitude (an illuminance from twilight
// to noon) by interpolating log2 of the value between keys with the keys' own shapes (Linear, Smooth,
// Step), then converting back with exp2. The keys stay in the authored unit; a Linear segment is
// geometric, and a Smooth segment cannot go negative. A Smooth key's stored tangents are slopes in the
// authored unit and mean nothing in log2, so every Smooth key but a Flat one takes the Auto tangent
// its neighbours give in log2.
//
// `floorValue` (> 0) is the smallest value the curve distinguishes from nothing: a key at or below it
// (including 0, negatives and NaN) is interpolated as the floor, and any result at or below it reads
// as 0, so a key of 0 fades its segment out instead of reaching minus infinity. A key returns its own
// value exactly at its time, except where a Step segment holds the previous key up to it, as in
// EvaluateCurveKeys.
//
// `period` > 0 treats the curve as cyclic over that span and closes it from the last key across the
// wrap gap to the first, log-linearly, as EvaluateCurveKeysWrapped does; 0 holds the end keys flat
// outside the key range, as EvaluateCurveKeys does. CurveField's logarithmic axis draws through it;
// a runtime consumer that evaluates the same keys through it draws and evaluates alike.
inline float EvaluateCurveKeysLogarithmic(const CurveKey* keys, uint32_t count, float t, float floorValue, float period)
{
    using CurveLogarithm::LogOfValue;
    using CurveLogarithm::ValueOfLog;
    using CurveLogarithm::ValueOrZero;
    assert(floorValue > 0.0f && "EvaluateCurveKeysLogarithmic needs a floor above 0: log2 of 0 is minus infinity");
    if (count == 0)
        return 0.0f;
    if (count == 1)
        return ValueOrZero(keys[0].Value, floorValue);

    const float first = keys[0].Time;
    const float last = keys[count - 1].Time;
    if (period > 0.0f && (t < first || t > last))
    {
        const float wrapSpan = (first + period) - last;
        if (wrapSpan <= 1e-6f)
            return ValueOrZero(keys[0].Value, floorValue);
        const float elapsed = (t > last) ? (t - last) : (t + period - last);
        const float u = std::clamp(elapsed / wrapSpan, 0.0f, 1.0f);
        const float logValue = Lerp(LogOfValue(keys[count - 1].Value, floorValue), LogOfValue(keys[0].Value, floorValue), u);
        return ValueOfLog(logValue, floorValue);
    }
    if (t <= first)
        return ValueOrZero(keys[0].Value, floorValue);
    if (t >= last)
        return ValueOrZero(keys[count - 1].Value, floorValue);

    uint32_t i = 1;
    while (i < count && t > keys[i].Time)
        ++i;
    if (keys[i - 1].Interp == CurveInterp::Step)
        return ValueOrZero(keys[i - 1].Value, floorValue);
    if (t == keys[i].Time)
        return ValueOrZero(keys[i].Value, floorValue);

    // The segment's two keys plus the neighbour on each side that an Auto tangent reads, with their
    // values in log2: the linear evaluator over this window is the curve in the log domain.
    constexpr uint32_t kWindowCapacity = 4;
    const uint32_t begin = (i >= 2) ? i - 2 : 0;
    const uint32_t end = std::min(count, i + 2);
    CurveKey window[kWindowCapacity];
    for (uint32_t k = begin; k < end; ++k)
    {
        window[k - begin] = keys[k];
        window[k - begin].Value = LogOfValue(keys[k].Value, floorValue);
        if (window[k - begin].TangentMode != CurveTangentMode::Flat)
            window[k - begin].TangentMode = CurveTangentMode::Auto;
    }
    return ValueOfLog(EvaluateCurveKeys(window, end - begin, t), floorValue);
}

// Fixed-capacity, trivially-copyable curve usable directly as an ECS component field: it
// lives in an SoA chunk and memcpy-snapshots for undo. `N` is the maximum key count; pick
// the smallest alias that fits the use. Keys are kept Time-sorted.
template <uint8_t N>
struct CurveBase
{
    static_assert(N >= 2, "A curve needs room for at least two keys.");

    uint8_t KeyCount = 0;
    uint8_t _Pad[3] = {0, 0, 0};
    CurveKey Keys[N] = {};

    static constexpr uint8_t Capacity = N;

    float Evaluate(float t) const { return EvaluateCurveKeys(Keys, KeyCount, t); }

    // Insert keeping Time order; returns false if full. A key whose Time equals an existing key's is
    // placed immediately after it -- coincident keys are allowed (as in Unity's AnimationCurve);
    // EvaluateCurveKeys tolerates the resulting zero-width segment. Used when seeding defaults and by
    // the editor host when the user adds a key.
    bool TryInsert(const CurveKey& key)
    {
        if (KeyCount >= N)
            return false;
        uint8_t i = KeyCount;
        while (i > 0 && Keys[i - 1].Time > key.Time)
        {
            Keys[i] = Keys[i - 1];
            --i;
        }
        Keys[i] = key;
        ++KeyCount;
        return true;
    }

    void RemoveAt(uint8_t index)
    {
        if (index >= KeyCount)
            return;
        for (uint8_t i = index; i + 1 < KeyCount; ++i)
            Keys[i] = Keys[i + 1];
        --KeyCount;
    }
};

using Curve8 = CurveBase<8>;
using Curve32 = CurveBase<32>;
using Curve = Curve32; // default capacity

static_assert(std::is_trivially_copyable_v<Curve32>,
              "Curve must stay trivially copyable for ECS storage and memcpy undo snapshots.");
static_assert(std::is_standard_layout_v<Curve32>,
              "Curve must stay standard-layout for predictable SoA member offsets.");

// Growable, heap-backed curve for non-ECS contexts: asset-stored preset libraries,
// authoring scratch, and animation channels. NOT trivially copyable — never store in ECS.
struct DynamicCurve
{
    std::vector<CurveKey> Keys;

    float Evaluate(float t) const
    {
        return EvaluateCurveKeys(Keys.data(), static_cast<uint32_t>(Keys.size()), t);
    }
};

} // namespace GameEngine::Math
