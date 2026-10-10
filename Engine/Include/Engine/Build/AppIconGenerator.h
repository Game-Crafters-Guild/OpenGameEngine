#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine {

struct BuildSettings;

/// Generates platform-specific application icon assets under `.Build/Player/icons/`
/// and returns the CMake snippet to embed them in the Player target.
struct AppIconGenerationResult
{
    bool success = false;
    std::string cmakeIconSection;
    std::vector<std::string> warnings;
    std::string error;
};

/// Editor / SDK icon used when build settings do not specify a custom icon.
std::filesystem::path ResolveDefaultEditorApplicationIconPath(
    const std::filesystem::path& editorSdkPath = {},
    const std::filesystem::path& runtimeDepsPath = {});

/// Resolves `settings.applicationIconPath` to an on-disk image file, or the default editor icon.
std::filesystem::path ResolveApplicationIconSourcePath(const BuildSettings& settings);

/// Creates icons for the active build platform (Windows .ico/.rc, macOS .icns, Linux PNG tree).
AppIconGenerationResult GenerateApplicationIcons(const BuildSettings& settings,
                                                 const std::filesystem::path& playerProjectDir);

/// Copies Linux-style icon PNGs into the staging directory for desktop/Steam shortcuts.
bool PackageLinuxIconsToStaging(const BuildSettings& settings, const std::filesystem::path& stagingDir);

} // namespace GameEngine
