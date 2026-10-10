#pragma once

#include "Mathematics/Curve.h"
#include "Mathematics/Easing.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{
namespace CurvePresets
{

enum class CurvePresetKind : uint8_t
{
    KeyCurve = 0,
    CubicBezier = 1,
};

struct CubicBezierPreset
{
    float Control1X = 0.25f;
    float Control1Y = 0.10f;
    float Control2X = 0.25f;
    float Control2Y = 1.00f;
    float AnchorStartY = 0.0f;
    float AnchorEndY = 1.0f;
};

// A named curve shape that can be applied to any curve via the preset picker. Shapes are stored
// normalized to [0, 1] in both time and value, so a preset is reusable across curves with any
// domain: each host remaps into/out of [0, 1]^2 (the sky inspector maps to its hours x value
// range; the ValueCurve editor is already [0, 1]^2 and stores the shape directly). Key-curve
// presets carry Math::DynamicCurve keys; cubic Bezier presets carry their dedicated handles.
struct CurvePreset
{
    std::string Name;
    CurvePresetKind Kind = CurvePresetKind::KeyCurve;
    Math::DynamicCurve Curve;
    CubicBezierPreset Bezier;
    bool HasBuiltinEasing = false;
    Math::TweenEasing BuiltinEasing = Math::TweenEasing::Linear;
};

// Sample an analytic tween ease into a smooth multi-key curve over [0, 1].
Math::DynamicCurve MakeCurveFromEase(Math::TweenEasing ease, int sampleCount = 9);
float EvaluatePreset01(const CurvePreset& preset, float normalizedTime);
Math::DynamicCurve PresetAsDynamicCurve(const CurvePreset& preset, int sampleCount = 9);

// The curated, code-defined built-in preset shapes (sampled from the engine's tween eases).
// Always available, independent of the user library.
const std::vector<CurvePreset>& Builtin();

// User-saved presets, persisted to <userDataRoot>/CurvePresets.json and shared across all
// curve editors, scenes, and projects. Loaded lazily on first use; saved on every change.
class UserPresetLibrary
{
  public:
    static UserPresetLibrary& Get();
    const std::vector<CurvePreset>& Presets() const { return m_Presets; }
    void Add(const std::string& name, const Math::DynamicCurve& curve);
    void Add(const std::string& name, const CubicBezierPreset& bezier);
    void Add(CurvePreset preset);
    void Rename(int index, const std::string& name);
    void Remove(int index);

  private:
    UserPresetLibrary();
    void Save() const;
    std::vector<CurvePreset> m_Presets;
    std::filesystem::path m_File;
};

// Lowest-unused "My Preset N" name, so saving after a delete does not collide with an existing
// entry. Shared by every "save current curve" action so the naming scheme stays consistent.
std::string NextUserPresetName();

} // namespace CurvePresets
} // namespace GameEngine
