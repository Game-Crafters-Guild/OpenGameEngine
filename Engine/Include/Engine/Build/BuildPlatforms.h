#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace GameEngine {

/// One target a project can be built for.
///
/// `Name` is the identity everything else keys on — BuildSettings::platformName,
/// the `build.platform.<Name>.*` preference keys, and the platform argument the
/// debug/MCP surface accepts. It is persisted, so renaming one orphans a
/// project's saved per-platform settings.
struct BuildPlatformInfo
{
    std::string_view Name;
    /// What the UI shows. Differs from Name where the stored identity is
    /// historical (a "Steam" build targets the Steam Deck).
    std::string_view DisplayName;
    std::string_view DefaultVersion;
    bool EnabledByDefault = false;
    /// Whether the settings tree offers a page for it. The Build panel lists
    /// every platform regardless; the console targets have no reachable
    /// settings page.
    bool ShowInSettingsTree = true;
    /// CSS class for the platform's row in the settings tree.
    std::string_view SettingsTreeRowClass;
};

/// Stored name of the browser target. It is the one platform that does not go
/// through BuildPipeline's native compile phases — a Web build runs the web
/// export (Tools/Web/export_web_player.py) over a prebuilt wasm Player — so the
/// build path and its settings page both recognise it by name.
inline constexpr std::string_view kWebBuildPlatformName = "Web";

/// Every build target, in presentation order.
///
/// The single list: the Build panel, the settings tree and the debug/MCP
/// build-settings response all read it, so a new target is one entry here and
/// not an edit in each of them.
std::span<const BuildPlatformInfo> GetBuildPlatforms();

/// The platform with this stored name, or nullptr when it is not a build target.
const BuildPlatformInfo* FindBuildPlatform(std::string_view name);

/// Position of `name` in GetBuildPlatforms(), or npos when unknown.
std::size_t GetBuildPlatformIndex(std::string_view name);

/// Display name for a stored platform name, falling back to the name itself so
/// an unknown platform is shown rather than blanked.
std::string GetBuildPlatformDisplayName(std::string_view name);

} // namespace GameEngine
