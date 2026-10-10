#include "UI/Controls/CurveTangents.h"

namespace GameEngine
{
namespace CurveEdit
{
namespace
{

// The auto (neighbour-derived) slope at a key, regardless of its current mode.
float AutoSlopeAt(std::vector<Math::CurveKey>& keys, int keyIndex)
{
    Math::CurveKey& k = keys[keyIndex];
    const Math::CurveTangentMode saved = k.TangentMode;
    k.TangentMode = Math::CurveTangentMode::Auto;
    const float slope = Math::ResolveKeySlope(keys.data(), static_cast<uint32_t>(keys.size()),
                                              static_cast<uint32_t>(keyIndex), false);
    k.TangentMode = saved;
    return slope;
}

} // namespace

bool SegmentIsSmooth(const std::vector<Math::CurveKey>& keys, int segStart)
{
    if (segStart < 0 || segStart + 1 >= static_cast<int>(keys.size()))
        return false;
    return keys[segStart].Interp == Math::CurveInterp::Smooth;
}

bool KeyHasTangents(const std::vector<Math::CurveKey>& keys, int keyIndex)
{
    return SegmentIsSmooth(keys, keyIndex - 1) || SegmentIsSmooth(keys, keyIndex);
}

void SetKeyMode(std::vector<Math::CurveKey>& keys, int keyIndex,
                Math::CurveInterp interp, Math::CurveTangentMode mode)
{
    if (keyIndex < 0 || keyIndex >= static_cast<int>(keys.size()))
        return;
    Math::CurveKey& k = keys[keyIndex];
    k.Interp = interp;
    k.TangentMode = mode;
    if (mode == Math::CurveTangentMode::Flat)
    {
        k.InTangent = 0.0f;
        k.OutTangent = 0.0f;
    }
    else if (mode == Math::CurveTangentMode::Manual || mode == Math::CurveTangentMode::Broken ||
             mode == Math::CurveTangentMode::Mirrored)
    {
        // Seed from the current auto slope so grabbing a handle starts on the curve.
        const float s = AutoSlopeAt(keys, keyIndex);
        k.InTangent = s;
        k.OutTangent = s;
    }
}

void ApplyTangentSlope(std::vector<Math::CurveKey>& keys, int keyIndex, bool incoming,
                       float slope, bool broken)
{
    if (keyIndex < 0 || keyIndex >= static_cast<int>(keys.size()))
        return;
    Math::CurveKey& k = keys[keyIndex];
    if (broken)
    {
        k.TangentMode = Math::CurveTangentMode::Broken;
        if (incoming)
            k.InTangent = slope;
        else
            k.OutTangent = slope;
    }
    else
    {
        k.TangentMode = Math::CurveTangentMode::Mirrored;
        k.InTangent = slope;
        k.OutTangent = slope;
    }

    // Force the touching segment(s) Smooth so the tangent actually shapes the curve.
    if (incoming || !broken)
    {
        if (keyIndex - 1 >= 0)
            keys[keyIndex - 1].Interp = Math::CurveInterp::Smooth;
    }
    if (!incoming || !broken)
    {
        if (keyIndex + 1 < static_cast<int>(keys.size()))
            k.Interp = Math::CurveInterp::Smooth;
    }
}

} // namespace CurveEdit
} // namespace GameEngine
