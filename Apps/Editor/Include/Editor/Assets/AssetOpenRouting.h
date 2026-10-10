#pragma once

#include <cstdint>
#include <string_view>

namespace GameEngine::Editor
{

/// Where the editor sends an asset the user asked to open — from an Assets
/// browser double-click, a context-menu Open, or an inspector button. One
/// decision for all of them, so every entry point agrees.
enum class AssetOpenTarget : uint8_t
{
    Scene,                 // SceneEditorController (dirty prompt, replace-open)
    ScriptEditor,          // internal Script Editor panel
    ExternalScriptEditor,  // the user's IDE via the project-aware launcher
    ExternalIde,           // native source in the IDE, with the native project folder
    NodeGraph,             // Node Graph panel (graph assets, shader-graph GLSL)
    Animation,             // Animation window
    Timeline,              // Timeline window
    ClipEditor,            // Clip Editor window
    Inspector,             // select the asset and bring the Inspector forward
    OperatingSystem,       // hand the file to the OS default application
};

struct AssetOpenRoutingInputs
{
    bool OpenCSharpInScriptEditor = true;
    bool OpenNativeSourceInScriptEditor = false;
    // Already combines the "shader graph GLSL opens in the Material Graph"
    // setting with the file-head probe; false for hand-written surfaces.
    bool GlslOpensInMaterialGraph = false;
};

/// `lowercaseExtension` includes the leading dot.
AssetOpenTarget ResolveAssetOpenTarget(std::string_view lowercaseExtension,
                                       const AssetOpenRoutingInputs& inputs);

} // namespace GameEngine::Editor
