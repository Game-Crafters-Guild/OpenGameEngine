#include "Engine/GameUI/UIScaleProjectSettings.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

namespace GameEngine
{
namespace
{
// The editor's per-project settings file. Read-only from here; EditorPaths owns
// the authoritative construction for the editor, which is also its only writer.
constexpr const char* kEditorDirName = ".Editor";
constexpr const char* kProjectSettingsFileName = "ProjectSettings.json";
constexpr const char* kModeKey = "mode";
constexpr const char* kReferenceWidthKey = "referenceWidth";
constexpr const char* kReferenceHeightKey = "referenceHeight";

void ReadReferenceSize(const nlohmann::json& uiScale, const char* key, float& inOutValue)
{
    const auto it = uiScale.find(key);
    if (it == uiScale.end() || !it->is_number())
        return;
    inOutValue = std::clamp(it->get<float>(), UIScaleProjectSettings::kMinReferenceSize,
                            UIScaleProjectSettings::kMaxReferenceSize);
}
} // namespace

const char* UIScaleProjectSettings::ToModeToken(UI::UIScaleMode mode)
{
    switch (mode)
    {
        case UI::UIScaleMode::Width: return "width";
        case UI::UIScaleMode::Height: return "height";
        case UI::UIScaleMode::Fit: return "fit";
        case UI::UIScaleMode::Fill: return "fill";
        case UI::UIScaleMode::Platform: break;
    }
    return "platform";
}

UI::UIScaleMode UIScaleProjectSettings::ParseModeToken(std::string_view token)
{
    if (token == "width")
        return UI::UIScaleMode::Width;
    if (token == "height")
        return UI::UIScaleMode::Height;
    if (token == "fit")
        return UI::UIScaleMode::Fit;
    if (token == "fill")
        return UI::UIScaleMode::Fill;
    return UI::UIScaleMode::Platform;
}

UI::UIScaleSettings UIScaleProjectSettings::ReadFrom(const nlohmann::json& uiScale)
{
    UI::UIScaleSettings settings{};
    if (!uiScale.is_object())
        return settings;
    if (const auto it = uiScale.find(kModeKey); it != uiScale.end() && it->is_string())
        settings.mode = ParseModeToken(it->get<std::string>());
    ReadReferenceSize(uiScale, kReferenceWidthKey, settings.referenceWidth);
    ReadReferenceSize(uiScale, kReferenceHeightKey, settings.referenceHeight);
    return settings;
}

void UIScaleProjectSettings::WriteTo(const UI::UIScaleSettings& settings, nlohmann::json& uiScale)
{
    if (!uiScale.is_object())
        uiScale = nlohmann::json::object();
    uiScale[kModeKey] = ToModeToken(settings.mode);
    uiScale[kReferenceWidthKey] = settings.referenceWidth;
    uiScale[kReferenceHeightKey] = settings.referenceHeight;
}

UI::UIScaleSettings UIScaleProjectSettings::Load(const std::filesystem::path& workspaceRoot)
{
    if (workspaceRoot.empty())
        return {};

    const std::filesystem::path settingsPath =
        (workspaceRoot / kEditorDirName / kProjectSettingsFileName).lexically_normal();
    std::ifstream file(settingsPath);
    if (!file.is_open())
        return {};

    const nlohmann::json root = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object())
    {
        Logger::Log::Warning("UIScaleProjectSettings: '{}' is not readable JSON — using platform scaling",
                             settingsPath.generic_string());
        return {};
    }

    const auto uiScale = root.find(kUIScaleKey);
    if (uiScale == root.end())
        return {};
    return ReadFrom(*uiScale);
}

} // namespace GameEngine
