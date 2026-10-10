#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace GameEngine::Editor
{

enum class SvgRasterSettingsScope : uint8_t
{
    EditorUi,
    TextureImport,
};

// SettingsStore-backed SVG raster defaults. The editor UI value is a personal
// preference; the texture-import value belongs to the active project.
class SvgRasterSettings
{
  public:
    static constexpr uint32_t kDefaultSize = 256u;

    static uint32_t Load(SvgRasterSettingsScope scope,
                         const std::filesystem::path& workspaceRoot,
                         std::string* outError = nullptr);

    // Persists and applies a new default. When reloadAssets is true, loaded SVG
    // assets in the affected source are scheduled for reload.
    static bool SaveAndApply(SvgRasterSettingsScope scope,
                             const std::filesystem::path& workspaceRoot,
                             uint32_t pixels,
                             bool reloadAssets = true,
                             std::string* outError = nullptr);

    // Applies both persisted defaults without rewriting either settings file.
    // Used before editor assets load and whenever the active project changes.
    static void ApplySavedDefaults(const std::filesystem::path& workspaceRoot);
};

} // namespace GameEngine::Editor
