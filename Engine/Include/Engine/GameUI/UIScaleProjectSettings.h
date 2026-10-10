#pragma once

#include "UI/UIScaleSettings.h"

#include <filesystem>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine
{

// The project's game UI scale policy, and the sole owner of its serialized
// shape. Two transports carry the same object (the LodProjectSettings pattern):
//
//   authored  <ProjectRoot>/.Editor/ProjectSettings.json -> "uiScale": { ... }
//   shipped   <exe dir>/game.config                      -> "uiScale": { ... }
//
//   { "uiScale": { "mode": "fit", "referenceWidth": 1600, "referenceHeight": 800 } }
//
// The editor authors the first and previews it in its game UI hosts;
// BuildPipeline copies it into the second at package time, because `.Editor/`
// never stages into a built game. The Player applies the shipped copy, so a HUD
// scales in a build the way it scaled in the editor.
struct UIScaleProjectSettings
{
    static constexpr const char* kUIScaleKey = "uiScale";

    // Persisted range of the reference size, in authored UI pixels. Both
    // transports clamp into it and the settings sliders use exactly these
    // bounds, so a slider cannot show a value the file does not hold.
    static constexpr float kMinReferenceSize = 16.0f;
    static constexpr float kMaxReferenceSize = 16384.0f;

    // The persisted mode vocabulary: "platform" | "width" | "height" | "fit" |
    // "fill". An unrecognized token parses as Platform.
    static const char* ToModeToken(UI::UIScaleMode mode);
    static UI::UIScaleMode ParseModeToken(std::string_view token);

    // Reads a "uiScale" object. Absent keys keep the UIScaleSettings defaults.
    static UI::UIScaleSettings ReadFrom(const nlohmann::json& uiScale);
    // Writes every key into a "uiScale" object.
    static void WriteTo(const UI::UIScaleSettings& settings, nlohmann::json& uiScale);

    // Read-only load from <workspaceRoot>/.Editor/ProjectSettings.json — the
    // editor's game UI hosts and the build pipeline's cook. The WRITE side is
    // Editor::SaveUIScaleProjectSettings; the settings page is the only writer.
    static UI::UIScaleSettings Load(const std::filesystem::path& workspaceRoot);
};

} // namespace GameEngine
