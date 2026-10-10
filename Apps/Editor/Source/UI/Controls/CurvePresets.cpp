#include "UI/Controls/CurvePresets.h"

#include "Editor/EditorPaths.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace CurvePresets
{
namespace
{

const char* KindName(CurvePresetKind kind)
{
    return kind == CurvePresetKind::CubicBezier ? "cubicBezier" : "keyCurve";
}

CurvePresetKind ParseKind(const nlohmann::json& json)
{
    const std::string kind = json.value("kind", std::string("keyCurve"));
    if (kind == "cubicBezier" || kind == "bezier")
        return CurvePresetKind::CubicBezier;
    return CurvePresetKind::KeyCurve;
}

void ParseKeys(const nlohmann::json& keys, Math::DynamicCurve& outCurve)
{
    if (!keys.is_array())
        return;

    for (const auto& jk : keys)
    {
        if (!jk.is_array() || jk.size() < 2)
            continue;
        Math::CurveKey key;
        key.Time = jk[0].get<float>();
        key.Value = jk[1].get<float>();
        if (jk.size() > 2)
            key.InTangent = jk[2].get<float>();
        if (jk.size() > 3)
            key.OutTangent = jk[3].get<float>();
        if (jk.size() > 4)
            key.Interp = static_cast<Math::CurveInterp>(jk[4].get<int>());
        if (jk.size() > 5)
            key.TangentMode = static_cast<Math::CurveTangentMode>(jk[5].get<int>());
        outCurve.Keys.push_back(key);
    }
}

void WriteKeys(const Math::DynamicCurve& curve, nlohmann::json& outKeys)
{
    outKeys = nlohmann::json::array();
    for (const auto& key : curve.Keys)
    {
        outKeys.push_back(nlohmann::json::array({key.Time, key.Value, key.InTangent, key.OutTangent,
                                                 static_cast<int>(key.Interp),
                                                 static_cast<int>(key.TangentMode)}));
    }
}

std::string TrimName(std::string name)
{
    const size_t first = name.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const size_t last = name.find_last_not_of(" \t\r\n");
    return name.substr(first, last - first + 1);
}

} // namespace

Math::DynamicCurve MakeCurveFromEase(Math::TweenEasing ease, int sampleCount)
{
    Math::DynamicCurve curve;
    const int n = std::max(2, sampleCount);
    curve.Keys.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        Math::CurveKey key;
        key.Time = t;
        key.Value = Math::EvalTweenEasing(ease, t);
        key.Interp = Math::CurveInterp::Smooth;
        key.TangentMode = Math::CurveTangentMode::Auto;
        curve.Keys.push_back(key);
    }
    return curve;
}

float EvaluatePreset01(const CurvePreset& preset, float normalizedTime)
{
    const float t = std::clamp(normalizedTime, 0.0f, 1.0f);
    if (preset.Kind == CurvePresetKind::KeyCurve && preset.HasBuiltinEasing)
        return Math::EvalTweenEasing(preset.BuiltinEasing, t);
    if (preset.Kind == CurvePresetKind::CubicBezier)
    {
        return Math::EvalCubicBezierAnchored01(
            std::clamp(preset.Bezier.AnchorStartY, 0.0f, 1.0f),
            std::clamp(preset.Bezier.AnchorEndY, 0.0f, 1.0f),
            preset.Bezier.Control1X,
            preset.Bezier.Control1Y,
            preset.Bezier.Control2X,
            preset.Bezier.Control2Y,
            t);
    }
    return preset.Curve.Evaluate(t);
}

Math::DynamicCurve PresetAsDynamicCurve(const CurvePreset& preset, int sampleCount)
{
    if (preset.Kind == CurvePresetKind::KeyCurve)
        return preset.Curve;

    Math::DynamicCurve curve;
    const int n = std::max(2, sampleCount);
    curve.Keys.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        Math::CurveKey key;
        key.Time = t;
        key.Value = EvaluatePreset01(preset, t);
        key.Interp = Math::CurveInterp::Smooth;
        key.TangentMode = Math::CurveTangentMode::Auto;
        curve.Keys.push_back(key);
    }
    return curve;
}

const std::vector<CurvePreset>& Builtin()
{
    static const std::vector<CurvePreset> presets = []() {
        std::vector<CurvePreset> list;
        for (const auto& entry : Math::kTweenEasingEntries)
        {
            if (entry.Value == Math::TweenEasing::Custom)
                continue;
            CurvePreset preset;
            preset.Name = Math::FormatTweenEasingDisplayName(entry.Name);
            preset.Kind = CurvePresetKind::KeyCurve;
            preset.Curve = MakeCurveFromEase(entry.Value);
            preset.HasBuiltinEasing = true;
            preset.BuiltinEasing = entry.Value;
            list.push_back(std::move(preset));
        }
        return list;
    }();
    return presets;
}

UserPresetLibrary& UserPresetLibrary::Get()
{
    static UserPresetLibrary library;
    return library;
}

UserPresetLibrary::UserPresetLibrary()
{
    m_File = Editor::GetEditorGlobalPaths().userDataRoot / "CurvePresets.json";
    std::ifstream in(m_File);
    if (!in)
        return;
    try
    {
        nlohmann::json root;
        in >> root;
        if (!root.is_array())
            return;
        for (const auto& jp : root)
        {
            CurvePreset preset;
            preset.Name = jp.value("name", std::string("Preset"));
            preset.Kind = ParseKind(jp);
            ParseKeys(jp.value("keys", nlohmann::json::array()), preset.Curve);
            const std::string builtinEasing = jp.value("builtinEasing", std::string());
            if (!builtinEasing.empty())
            {
                Math::TweenEasing easing = Math::TweenEasing::Linear;
                if (Math::TryParseTweenEasingName(builtinEasing, easing) && easing != Math::TweenEasing::Custom)
                {
                    preset.HasBuiltinEasing = true;
                    preset.BuiltinEasing = easing;
                }
            }

            if (preset.Kind == CurvePresetKind::CubicBezier)
            {
                const auto bezier = jp.value("bezier", nlohmann::json::object());
                preset.Bezier.Control1X = bezier.value("control1X", preset.Bezier.Control1X);
                preset.Bezier.Control1Y = bezier.value("control1Y", preset.Bezier.Control1Y);
                preset.Bezier.Control2X = bezier.value("control2X", preset.Bezier.Control2X);
                preset.Bezier.Control2Y = bezier.value("control2Y", preset.Bezier.Control2Y);
                preset.Bezier.AnchorStartY = bezier.value("anchorStartY", preset.Bezier.AnchorStartY);
                preset.Bezier.AnchorEndY = bezier.value("anchorEndY", preset.Bezier.AnchorEndY);
            }
            m_Presets.push_back(std::move(preset));
        }
    }
    catch (...)
    {
        m_Presets.clear(); // ignore a corrupt library file
    }
}

void UserPresetLibrary::Save() const
{
    std::error_code ec;
    std::filesystem::create_directories(m_File.parent_path(), ec);
    nlohmann::json root = nlohmann::json::array();
    for (const auto& preset : m_Presets)
    {
        nlohmann::json jp;
        jp["name"] = preset.Name;
        jp["kind"] = KindName(preset.Kind);
        if (preset.Kind == CurvePresetKind::KeyCurve && preset.HasBuiltinEasing)
            jp["builtinEasing"] = std::string(Math::ToTweenEasingName(preset.BuiltinEasing));
        nlohmann::json keys;
        WriteKeys(PresetAsDynamicCurve(preset), keys);
        jp["keys"] = std::move(keys);
        if (preset.Kind == CurvePresetKind::CubicBezier)
        {
            jp["bezier"] = {
                {"control1X", preset.Bezier.Control1X},
                {"control1Y", preset.Bezier.Control1Y},
                {"control2X", preset.Bezier.Control2X},
                {"control2Y", preset.Bezier.Control2Y},
                {"anchorStartY", preset.Bezier.AnchorStartY},
                {"anchorEndY", preset.Bezier.AnchorEndY},
            };
        }
        root.push_back(std::move(jp));
    }
    std::ofstream out(m_File, std::ios::trunc);
    if (out)
        out << root.dump(2);
}

void UserPresetLibrary::Add(const std::string& name, const Math::DynamicCurve& curve)
{
    CurvePreset preset;
    preset.Name = name;
    preset.Kind = CurvePresetKind::KeyCurve;
    preset.Curve = curve;
    Add(std::move(preset));
}

void UserPresetLibrary::Add(const std::string& name, const CubicBezierPreset& bezier)
{
    CurvePreset preset;
    preset.Name = name;
    preset.Kind = CurvePresetKind::CubicBezier;
    preset.Bezier = bezier;
    Add(std::move(preset));
}

void UserPresetLibrary::Add(CurvePreset preset)
{
    m_Presets.push_back(std::move(preset));
    Save();
}

void UserPresetLibrary::Rename(int index, const std::string& name)
{
    if (index < 0 || index >= static_cast<int>(m_Presets.size()))
        return;
    std::string trimmed = TrimName(name);
    if (trimmed.empty() || m_Presets[static_cast<size_t>(index)].Name == trimmed)
        return;
    m_Presets[static_cast<size_t>(index)].Name = std::move(trimmed);
    Save();
}

void UserPresetLibrary::Remove(int index)
{
    if (index < 0 || index >= static_cast<int>(m_Presets.size()))
        return;
    m_Presets.erase(m_Presets.begin() + index);
    Save();
}

std::string NextUserPresetName()
{
    const auto& presets = UserPresetLibrary::Get().Presets();
    for (int ordinal = 1;; ++ordinal)
    {
        std::string name = "My Preset " + std::to_string(ordinal);
        const bool taken = std::any_of(presets.begin(), presets.end(),
                                       [&](const CurvePreset& p) { return p.Name == name; });
        if (!taken)
            return name;
    }
}

} // namespace CurvePresets
} // namespace GameEngine
