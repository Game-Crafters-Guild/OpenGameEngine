#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace GameEngine::Math
{

enum class EasingFunction : uint8_t
{
    Linear,
    Ease,
    EaseIn,
    EaseOut,
    EaseInOut
};

// Evaluate a cubic bezier timing function y(x) for control points
// (0,0)-(x1,y1)-(x2,y2)-(1,1). Uses Newton-Raphson to solve for t given x.
inline float CubicBezierEval(float x1, float y1, float x2, float y2, float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;

    // Newton-Raphson: find t such that bezierX(t) = x
    float t = x;
    for (int i = 0; i < 8; ++i)
    {
        float t2 = t * t;
        float t3 = t2 * t;
        float xt = 3.0f * (1.0f - t) * (1.0f - t) * t * x1
                 + 3.0f * (1.0f - t) * t2 * x2
                 + t3;
        float dxt = 3.0f * (1.0f - t) * (1.0f - t) * x1
                  + 6.0f * (1.0f - t) * t * (x2 - x1)
                  + 3.0f * t2 * (1.0f - x2);
        if (std::abs(dxt) < 1e-6f) break;
        t -= (xt - x) / dxt;
        t = std::clamp(t, 0.0f, 1.0f);
    }

    // Evaluate bezierY(t)
    float t2 = t * t;
    float t3 = t2 * t;
    return 3.0f * (1.0f - t) * (1.0f - t) * t * y1
         + 3.0f * (1.0f - t) * t2 * y2
         + t3;
}

inline float EvalEasing(EasingFunction fn, float t)
{
    switch (fn)
    {
    case EasingFunction::Linear:    return t;
    case EasingFunction::Ease:      return CubicBezierEval(0.25f, 0.1f, 0.25f, 1.0f, t);
    case EasingFunction::EaseIn:    return CubicBezierEval(0.42f, 0.0f, 1.0f, 1.0f, t);
    case EasingFunction::EaseOut:   return CubicBezierEval(0.0f, 0.0f, 0.58f, 1.0f, t);
    case EasingFunction::EaseInOut: return CubicBezierEval(0.42f, 0.0f, 0.58f, 1.0f, t);
    default: return t;
    }
}

enum class TweenEasing : uint8_t
{
    Linear,
    Smooth,
    Fade,
    Constant,

    QuadIn,
    QuadOut,
    QuadInOut,
    QuadOutIn,

    CubicIn,
    CubicOut,
    CubicInOut,
    CubicOutIn,

    QuartIn,
    QuartOut,
    QuartInOut,
    QuartOutIn,

    QuintIn,
    QuintOut,
    QuintInOut,
    QuintOutIn,

    SineIn,
    SineOut,
    SineInOut,
    SineOutIn,

    ExpoIn,
    ExpoOut,
    ExpoInOut,
    ExpoOutIn,

    CircIn,
    CircOut,
    CircInOut,
    CircOutIn,

    ElasticIn,
    ElasticOut,
    ElasticInOut,
    ElasticOutIn,

    BackIn,
    BackOut,
    BackInOut,
    BackOutIn,

    BounceIn,
    BounceOut,
    BounceInOut,
    BounceOutIn,

    Custom
};

struct TweenEasingEntry
{
    TweenEasing Value;
    std::string_view Name;
};

inline constexpr std::array<TweenEasingEntry, 45> kTweenEasingEntries = {{
    {TweenEasing::Linear, "Linear"},
    {TweenEasing::Smooth, "Smooth"},
    {TweenEasing::Fade, "Fade"},
    {TweenEasing::Constant, "Constant"},
    {TweenEasing::QuadIn, "QuadIn"},
    {TweenEasing::QuadOut, "QuadOut"},
    {TweenEasing::QuadInOut, "QuadInOut"},
    {TweenEasing::QuadOutIn, "QuadOutIn"},
    {TweenEasing::CubicIn, "CubicIn"},
    {TweenEasing::CubicOut, "CubicOut"},
    {TweenEasing::CubicInOut, "CubicInOut"},
    {TweenEasing::CubicOutIn, "CubicOutIn"},
    {TweenEasing::QuartIn, "QuartIn"},
    {TweenEasing::QuartOut, "QuartOut"},
    {TweenEasing::QuartInOut, "QuartInOut"},
    {TweenEasing::QuartOutIn, "QuartOutIn"},
    {TweenEasing::QuintIn, "QuintIn"},
    {TweenEasing::QuintOut, "QuintOut"},
    {TweenEasing::QuintInOut, "QuintInOut"},
    {TweenEasing::QuintOutIn, "QuintOutIn"},
    {TweenEasing::SineIn, "SineIn"},
    {TweenEasing::SineOut, "SineOut"},
    {TweenEasing::SineInOut, "SineInOut"},
    {TweenEasing::SineOutIn, "SineOutIn"},
    {TweenEasing::ExpoIn, "ExpoIn"},
    {TweenEasing::ExpoOut, "ExpoOut"},
    {TweenEasing::ExpoInOut, "ExpoInOut"},
    {TweenEasing::ExpoOutIn, "ExpoOutIn"},
    {TweenEasing::CircIn, "CircIn"},
    {TweenEasing::CircOut, "CircOut"},
    {TweenEasing::CircInOut, "CircInOut"},
    {TweenEasing::CircOutIn, "CircOutIn"},
    {TweenEasing::ElasticIn, "ElasticIn"},
    {TweenEasing::ElasticOut, "ElasticOut"},
    {TweenEasing::ElasticInOut, "ElasticInOut"},
    {TweenEasing::ElasticOutIn, "ElasticOutIn"},
    {TweenEasing::BackIn, "BackIn"},
    {TweenEasing::BackOut, "BackOut"},
    {TweenEasing::BackInOut, "BackInOut"},
    {TweenEasing::BackOutIn, "BackOutIn"},
    {TweenEasing::BounceIn, "BounceIn"},
    {TweenEasing::BounceOut, "BounceOut"},
    {TweenEasing::BounceInOut, "BounceInOut"},
    {TweenEasing::BounceOutIn, "BounceOutIn"},
    {TweenEasing::Custom, "Custom"},
}};

inline bool EqualsTweenEasingNameAsciiIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const auto ac = static_cast<unsigned char>(a[i]);
        const auto bc = static_cast<unsigned char>(b[i]);
        if (std::tolower(ac) != std::tolower(bc))
            return false;
    }
    return true;
}

inline std::string_view ToTweenEasingName(TweenEasing easing)
{
    for (const auto& e : kTweenEasingEntries)
    {
        if (e.Value == easing)
            return e.Name;
    }
    return "Linear";
}

/// UI label: inserts spaces at camel-case boundaries (e.g. QuadOut -> "Quad Out"). Parsing uses compact
/// names from ToTweenEasingName / kTweenEasingEntries.
inline std::string FormatTweenEasingDisplayName(std::string_view compactName)
{
    std::string out;
    if (compactName.empty())
        return out;
    out.reserve(compactName.size() + compactName.size() / 4);
    out.push_back(static_cast<char>(compactName[0]));
    for (size_t i = 1; i < compactName.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(compactName[i]);
        const unsigned char p = static_cast<unsigned char>(compactName[i - 1]);
        const bool isCap = (c >= 'A' && c <= 'Z');
        if (isCap)
        {
            const bool prevLower = (p >= 'a' && p <= 'z');
            const bool prevUpper = (p >= 'A' && p <= 'Z');
            const bool nextLower =
                (i + 1 < compactName.size()) &&
                static_cast<unsigned char>(compactName[i + 1]) >= 'a' &&
                static_cast<unsigned char>(compactName[i + 1]) <= 'z';
            if (prevLower || (prevUpper && nextLower))
                out.push_back(' ');
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

inline std::string FormatTweenEasingDisplayName(TweenEasing easing)
{
    return FormatTweenEasingDisplayName(ToTweenEasingName(easing));
}

inline bool TryParseTweenEasingName(std::string_view name, TweenEasing& out)
{
    for (const auto& e : kTweenEasingEntries)
    {
        if (EqualsTweenEasingNameAsciiIgnoreCase(name, e.Name))
        {
            out = e.Value;
            return true;
        }
    }
    return false;
}

// Cubic Bézier curve with endpoints P0=(0, anchorStartY) and P3=(1, anchorEndY), normalized-time X.
inline float EvalCubicBezierAnchored01(float anchorStartY, float anchorEndY,
                                       float c1x, float c1y, float c2x, float c2y, float t)
{
    const float x = std::clamp(t, 0.0f, 1.0f);
    anchorStartY = std::clamp(anchorStartY, 0.0f, 1.0f);
    anchorEndY = std::clamp(anchorEndY, 0.0f, 1.0f);
    c1x = std::clamp(c1x, 0.0f, 1.0f);
    c1y = std::clamp(c1y, 0.0f, 1.0f);
    c2x = std::clamp(c2x, 0.0f, 1.0f);
    c2y = std::clamp(c2y, 0.0f, 1.0f);

    auto sampleX = [](float c1, float c2, float u) {
        const float inv = 1.0f - u;
        return 3.0f * inv * inv * u * c1 +
               3.0f * inv * u * u * c2 +
               u * u * u;
    };

    auto sampleY = [=](float u) {
        const float inv = 1.0f - u;
        const float inv3 = inv * inv * inv;
        const float u3 = u * u * u;
        return inv3 * anchorStartY +
               3.0f * inv * inv * u * c1y +
               3.0f * inv * u * u * c2y +
               u3 * anchorEndY;
    };

    float lo = 0.0f;
    float hi = 1.0f;
    float u = x;
    for (int i = 0; i < 16; ++i)
    {
        u = (lo + hi) * 0.5f;
        if (sampleX(c1x, c2x, u) < x)
            lo = u;
        else
            hi = u;
    }
    return std::clamp(sampleY(u), 0.0f, 1.0f);
}

inline float EvalCubicBezier01(float c1x, float c1y, float c2x, float c2y, float t)
{
    return EvalCubicBezierAnchored01(0.0f, 1.0f, c1x, c1y, c2x, c2y, t);
}

inline float EvalTweenEasing(TweenEasing easing, float t)
{
    const float x = std::clamp(t, 0.0f, 1.0f);
    constexpr float kEasingPi = 3.14159265358979323846f;

    auto quadIn = [](float v) { return v * v; };
    auto quadOut = [](float v) { return 1.0f - (1.0f - v) * (1.0f - v); };
    auto cubicIn = [](float v) { return v * v * v; };
    auto cubicOut = [](float v) { const float oneMinus = 1.0f - v; return 1.0f - oneMinus * oneMinus * oneMinus; };
    auto quartIn = [](float v) { return v * v * v * v; };
    auto quartOut = [](float v) { const float oneMinus = 1.0f - v; return 1.0f - oneMinus * oneMinus * oneMinus * oneMinus; };
    auto quintIn = [](float v) { return v * v * v * v * v; };
    auto quintOut = [](float v) { const float oneMinus = 1.0f - v; return 1.0f - oneMinus * oneMinus * oneMinus * oneMinus * oneMinus; };
    auto sineIn = [](float v) { return 1.0f - std::cos((v * kEasingPi) * 0.5f); };
    auto sineOut = [](float v) { return std::sin((v * kEasingPi) * 0.5f); };
    auto expoIn = [](float v) { return (v <= 0.0f) ? 0.0f : std::pow(2.0f, 10.0f * (v - 1.0f)); };
    auto expoOut = [](float v) { return (v >= 1.0f) ? 1.0f : (1.0f - std::pow(2.0f, -10.0f * v)); };
    auto circIn = [](float v) { return 1.0f - std::sqrt(std::max(0.0f, 1.0f - v * v)); };
    auto circOut = [](float v) { const float oneMinus = v - 1.0f; return std::sqrt(std::max(0.0f, 1.0f - oneMinus * oneMinus)); };
    auto backIn = [](float v) { constexpr float c1 = 1.70158f; constexpr float c3 = c1 + 1.0f; return c3 * v * v * v - c1 * v * v; };
    auto backOut = [](float v) { constexpr float c1 = 1.70158f; constexpr float c3 = c1 + 1.0f; const float oneMinus = v - 1.0f; return 1.0f + c3 * oneMinus * oneMinus * oneMinus + c1 * oneMinus * oneMinus; };
    auto elasticIn = [](float v) {
        if (v <= 0.0f) return 0.0f;
        if (v >= 1.0f) return 1.0f;
        constexpr float c4 = (2.0f * kEasingPi) / 3.0f;
        return -std::pow(2.0f, 10.0f * v - 10.0f) * std::sin((v * 10.0f - 10.75f) * c4);
    };
    auto elasticOut = [](float v) {
        if (v <= 0.0f) return 0.0f;
        if (v >= 1.0f) return 1.0f;
        constexpr float c4 = (2.0f * kEasingPi) / 3.0f;
        return std::pow(2.0f, -10.0f * v) * std::sin((v * 10.0f - 0.75f) * c4) + 1.0f;
    };
    auto bounceOut = [](float v) {
        constexpr float n1 = 7.5625f;
        constexpr float d1 = 2.75f;
        if (v < 1.0f / d1) return n1 * v * v;
        if (v < 2.0f / d1) { v -= 1.5f / d1; return n1 * v * v + 0.75f; }
        if (v < 2.5f / d1) { v -= 2.25f / d1; return n1 * v * v + 0.9375f; }
        v -= 2.625f / d1;
        return n1 * v * v + 0.984375f;
    };
    auto bounceIn = [&bounceOut](float v) { return 1.0f - bounceOut(1.0f - v); };

    auto inOut = [](float v, const auto& inFn, const auto& outFn) {
        return (v < 0.5f) ? (0.5f * inFn(v * 2.0f))
                          : (0.5f * outFn((v - 0.5f) * 2.0f) + 0.5f);
    };
    auto outIn = [](float v, const auto& inFn, const auto& outFn) {
        return (v < 0.5f) ? (0.5f * outFn(v * 2.0f))
                          : (0.5f * inFn((v - 0.5f) * 2.0f) + 0.5f);
    };

    switch (easing)
    {
    case TweenEasing::Linear: return x;
    case TweenEasing::Smooth: return x * x * (3.0f - 2.0f * x);
    case TweenEasing::Fade: return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
    case TweenEasing::Constant: return (x < 1.0f) ? 0.0f : 1.0f;
    case TweenEasing::QuadIn: return quadIn(x);
    case TweenEasing::QuadOut: return quadOut(x);
    case TweenEasing::QuadInOut: return inOut(x, quadIn, quadOut);
    case TweenEasing::QuadOutIn: return outIn(x, quadIn, quadOut);
    case TweenEasing::CubicIn: return cubicIn(x);
    case TweenEasing::CubicOut: return cubicOut(x);
    case TweenEasing::CubicInOut: return inOut(x, cubicIn, cubicOut);
    case TweenEasing::CubicOutIn: return outIn(x, cubicIn, cubicOut);
    case TweenEasing::QuartIn: return quartIn(x);
    case TweenEasing::QuartOut: return quartOut(x);
    case TweenEasing::QuartInOut: return inOut(x, quartIn, quartOut);
    case TweenEasing::QuartOutIn: return outIn(x, quartIn, quartOut);
    case TweenEasing::QuintIn: return quintIn(x);
    case TweenEasing::QuintOut: return quintOut(x);
    case TweenEasing::QuintInOut: return inOut(x, quintIn, quintOut);
    case TweenEasing::QuintOutIn: return outIn(x, quintIn, quintOut);
    case TweenEasing::SineIn: return sineIn(x);
    case TweenEasing::SineOut: return sineOut(x);
    case TweenEasing::SineInOut: return inOut(x, sineIn, sineOut);
    case TweenEasing::SineOutIn: return outIn(x, sineIn, sineOut);
    case TweenEasing::ExpoIn: return expoIn(x);
    case TweenEasing::ExpoOut: return expoOut(x);
    case TweenEasing::ExpoInOut: return inOut(x, expoIn, expoOut);
    case TweenEasing::ExpoOutIn: return outIn(x, expoIn, expoOut);
    case TweenEasing::CircIn: return circIn(x);
    case TweenEasing::CircOut: return circOut(x);
    case TweenEasing::CircInOut: return inOut(x, circIn, circOut);
    case TweenEasing::CircOutIn: return outIn(x, circIn, circOut);
    case TweenEasing::ElasticIn: return elasticIn(x);
    case TweenEasing::ElasticOut: return elasticOut(x);
    case TweenEasing::ElasticInOut: return inOut(x, elasticIn, elasticOut);
    case TweenEasing::ElasticOutIn: return outIn(x, elasticIn, elasticOut);
    case TweenEasing::BackIn: return backIn(x);
    case TweenEasing::BackOut: return backOut(x);
    case TweenEasing::BackInOut: return inOut(x, backIn, backOut);
    case TweenEasing::BackOutIn: return outIn(x, backIn, backOut);
    case TweenEasing::BounceIn: return bounceIn(x);
    case TweenEasing::BounceOut: return bounceOut(x);
    case TweenEasing::BounceInOut: return inOut(x, bounceIn, bounceOut);
    case TweenEasing::BounceOutIn: return outIn(x, bounceIn, bounceOut);
    case TweenEasing::Custom: return EvalCubicBezier01(0.25f, 0.1f, 0.25f, 1.0f, x);
    default: return x;
    }
}

} // namespace GameEngine::Math
