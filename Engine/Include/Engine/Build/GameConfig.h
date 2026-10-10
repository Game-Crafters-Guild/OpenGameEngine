#pragma once

#include "Engine/Rendering/AntiAliasingProjectSettings.h"
#include "Engine/Rendering/LodProjectSettings.h"
#include "Rendering/Core/Device.h"
#include "Types/Types.h"
#include "UI/UIScaleSettings.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace GameEngine {

enum class WindowMode : uint8_t
{
    Windowed,
    BorderlessFullscreen,
    ExclusiveFullscreen
};

// Project-global LOD import defaults shipped to the packaged Player. Seeds the
// engine's LODImportSettings singleton at boot (the editor seeds it from its
// SettingsStore instead). Per-asset overrides travel separately in the staged
// .assetmanifest kv.
struct LodImportConfig
{
    // Default-on for static meshes so packaged builds match the editor default;
    // skinned meshes are excluded downstream (ShouldGenerateLODsForMesh).
    bool autoGenerate = true;
    uint32_t count = 4;
    float ratios[4] = {1.0f, 0.5f, 0.25f, 0.10f};
    float errors[4] = {0.0f, 0.06f, 0.16f, 0.35f};
    // MeshLODBorderRule value: 0 = SeamPlanes (lock only kit-seam borders on
    // the AABB face planes), 1 = LockAll, 2 = Free. Matches the editor /
    // engine default (see MeshLODConfig).
    uint32_t borderRule = 0;
};

struct GameConfig
{
    std::string gameName = "My Game";
    std::string startupScene;       // asset-relative, e.g. "Scenes/Main.scene"
    std::string renderPipeline;     // asset-relative, e.g. "RenderPipelines/ForwardPlus.rendergraph"

    LodImportConfig lod;

    // Runtime LOD SELECTION, distinct from the import defaults above: which
    // mapping seeds each mesh's switch points and how much error it may spend.
    // Authored per project as rendering.lod* and copied here at package time
    // (BuildPipeline::WriteGameConfig), because `.Editor/` never ships. The
    // Player applies it to RenderServices at startup.
    Rendering::LodProjectSettings lodSelection;

    // Engine-wide anti-aliasing + render-scale defaults (rendering.aaMode /
    // msaa / fxaaQuality / taaRenderScale / drsMode / drsTargetFps), copied
    // from the project at package time and applied to RenderServices at
    // startup — a shipped game runs the same AA stack the editor previewed,
    // SSAA included. Per-camera Camera overrides still win per view.
    Rendering::AntiAliasingProjectSettings renderQuality;

    // Window: the size and mode the runtime creates its window with at startup.
    uint32_t windowWidth = 1920;
    uint32_t windowHeight = 1080;
    WindowMode windowMode = WindowMode::Windowed;
    bool vsync = true;
    UI::UIScaleSettings uiScale;

    // HDR output for shipped builds. HDR10 PQ is the default mode when enabled.
    bool hdrEnabled = false;
    Rendering::HdrOutputMode hdrMode = Rendering::HdrOutputMode::HDR10_PQ;
    Rendering::HdrSwapchainBitDepth hdrSwapchainBitDepth = Rendering::HdrSwapchainBitDepth::Bit10;
    int hdrTargetDisplay = -1;
    Rendering::HdrStaticMetadata hdrStaticMetadata{};

    // Scripting — assembly loaded from Managed/ relative to exe
    std::string scriptAssemblyPath = "Managed/UserScripts.dll";

    // Branding
    std::string appIconPath; // optional, for exe icon / macOS bundle
};

/// Parse a window mode string (game.config, CLI, env). Accepts windowed, borderless,
/// fullscreen (alias for borderless), exclusive, and hyphenated variants.
bool TryParseWindowMode(std::string_view text, WindowMode& outMode);

/// Serialize a window mode for game.config and build settings.
const char* WindowModeToString(WindowMode mode);

/// Load a GameConfig from a JSON file. Returns default config on parse failure.
GameConfig LoadGameConfig(const std::filesystem::path& path);

/// Save a GameConfig to a JSON file.
bool SaveGameConfig(const std::filesystem::path& path, const GameConfig& cfg);

} // namespace GameEngine
