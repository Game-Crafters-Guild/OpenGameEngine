#pragma once

#include "Mathematics/Curve.h"
#include "Mathematics/Easing.h"
#include "Types/Types.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace GameEngine::Components
{

// Where ValueCurve writes its evaluated value when ApplyToTarget is on.
enum class ValueCurveTarget : uint8
{
    None = 0,
    TransformPositionX,
    TransformPositionY,
    TransformPositionZ,
    TransformScaleX,
    TransformScaleY,
    TransformScaleZ,
    LightIntensity,
    PostProcessWeight
};

enum class ValueCurveShapeMode : uint8
{
    KeyCurve = 0,
    CubicBezier = 1,
};

// The default shape: a smooth 0 -> 1 ramp. The user can reshape it in the curve editor, replace
// it from the preset picker, or keep an untouched built-in easing source for exact runtime eval.
inline Math::Curve MakeDefaultValueCurveShape()
{
    Math::Curve shape;
    Math::CurveKey a;
    a.Time = 0.0f;
    a.Value = 0.0f;
    a.Interp = Math::CurveInterp::Smooth;
    Math::CurveKey b;
    b.Time = 1.0f;
    b.Value = 1.0f;
    b.Interp = Math::CurveInterp::Smooth;
    shape.TryInsert(a);
    shape.TryInsert(b);
    return shape;
}

inline Math::Curve MakeValueCurveShapeFromEasing(Math::TweenEasing easing, int sampleCount = 9)
{
    Math::Curve shape;
    const int n = std::clamp(sampleCount, 2, static_cast<int>(Math::Curve::Capacity));
    for (int i = 0; i < n; ++i)
    {
        const float32 t = static_cast<float32>(i) / static_cast<float32>(n - 1);
        Math::CurveKey key;
        key.Time = t;
        key.Value = Math::EvalTweenEasing(easing, t);
        key.Interp = Math::CurveInterp::Smooth;
        key.TangentMode = Math::CurveTangentMode::Auto;
        shape.TryInsert(key);
    }
    return shape;
}

// Authoring component for tween-like value animation: a normalized [0,1] shape sampled across
// DurationSeconds (with optional loop / ping-pong) and remapped from StartValue to EndValue.
// ShapeMode selects either the reusable Math::Curve key editor or the dedicated four-handle
// cubic Bezier editor restored from the older ValueCurve UI.
struct ValueCurve
{
    bool Enabled = true;
    // Off holds the curve at StartValue: a value EvaluateValueCurveValue reads.
    static constexpr bool KeepsOwnEnabledField = true;
    bool Playing = true;
    bool Loop = false;
    bool PingPong = false;
    bool ApplyToTarget = false;

    ValueCurveTarget Target = ValueCurveTarget::None;
    ValueCurveShapeMode ShapeMode = ValueCurveShapeMode::KeyCurve;
    bool UseBuiltinEasing = false;
    Math::TweenEasing BuiltinEasing = Math::TweenEasing::Smooth;

    float32 DurationSeconds = 1.0f;
    float32 StartValue = 0.0f;
    float32 EndValue = 1.0f;

    Math::Curve Shape = MakeDefaultValueCurveShape();

    float32 CubicBezierControl1X = 0.25f;
    float32 CubicBezierControl1Y = 0.10f;
    float32 CubicBezierControl2X = 0.25f;
    float32 CubicBezierControl2Y = 1.00f;
    float32 CubicBezierAnchorStartY = 0.0f;
    float32 CubicBezierAnchorEndY = 1.0f;

    // Runtime state updated by ValueCurveSystem.
    float32 ElapsedSeconds = 0.0f;
    float32 CurrentValue = 0.0f;
};

inline float32 EvaluateValueCurveCubicBezier01(const ValueCurve& curve, float32 normalizedTime)
{
    return Math::EvalCubicBezierAnchored01(
        std::clamp(curve.CubicBezierAnchorStartY, 0.0f, 1.0f),
        std::clamp(curve.CubicBezierAnchorEndY, 0.0f, 1.0f),
        curve.CubicBezierControl1X,
        curve.CubicBezierControl1Y,
        curve.CubicBezierControl2X,
        curve.CubicBezierControl2Y,
        normalizedTime);
}

inline float32 EvaluateValueCurve01(const ValueCurve& curve, float32 normalizedTime)
{
    const float32 t01 = std::clamp(normalizedTime, 0.0f, 1.0f);
    if (curve.ShapeMode == ValueCurveShapeMode::CubicBezier)
        return EvaluateValueCurveCubicBezier01(curve, t01);
    if (curve.UseBuiltinEasing)
        return Math::EvalTweenEasing(curve.BuiltinEasing, t01);
    return curve.Shape.Evaluate(t01);
}

inline float32 EvaluateValueCurveValue(const ValueCurve& curve, float32 elapsedSeconds)
{
    if (!curve.Enabled)
        return curve.StartValue;

    constexpr float32 kMinDurationSeconds = 0.0001f;
    const float32 durationSeconds = std::max(curve.DurationSeconds, kMinDurationSeconds);

    float32 t01 = 0.0f;
    if (curve.Loop)
    {
        const float32 cycleLength = curve.PingPong ? 2.0f : 1.0f;
        float32 cycleT = std::fmod(elapsedSeconds / durationSeconds, cycleLength);
        if (cycleT < 0.0f)
            cycleT += cycleLength;
        t01 = (curve.PingPong && cycleT > 1.0f) ? (2.0f - cycleT) : cycleT;
    }
    else
    {
        float32 cycleT = elapsedSeconds / durationSeconds;
        if (curve.PingPong)
        {
            cycleT = std::clamp(cycleT, 0.0f, 2.0f);
            t01 = (cycleT > 1.0f) ? (2.0f - cycleT) : cycleT;
        }
        else
        {
            t01 = std::clamp(cycleT, 0.0f, 1.0f);
        }
    }

    const float32 eased = EvaluateValueCurve01(curve, t01);
    return curve.StartValue + (curve.EndValue - curve.StartValue) * eased;
}

/// Sets CurrentValue from the curve at ElapsedSeconds (inspector preview after edits, Persist, etc.).
inline void RefreshValueCurveCurrentValueFromEvaluation(ValueCurve& c)
{
    c.CurrentValue = EvaluateValueCurveValue(c, c.ElapsedSeconds);
}

static_assert(std::is_trivially_copyable_v<ValueCurve>, "ValueCurve must be trivially copyable");
static_assert(std::is_standard_layout_v<ValueCurve>, "ValueCurve must be standard layout");

} // namespace GameEngine::Components
