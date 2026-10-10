#include "Startup/EditorStartup.h"

#include "Core/Application.h" // PathUtils
#include "Editor/EditorPaths.h"

namespace GameEngine::Editor::Startup
{
GameEngine::ApplicationConfig BuildEditorApplicationConfig(const EditorCommandLineArgs& editorArgs,
                                                           const GameEngine::EngineArgs& engineArgs)
{
    GameEngine::ApplicationConfig config;
    config.Name = "Open Engine Editor";
    config.EnableEditor = true;

    if (editorArgs.projectRoot.has_value())
    {
        config.WorkspaceDirectory = editorArgs.projectRoot->string();
    }
    else
    {
        // Never let EngineCore fall back to the executable directory. In a macOS
        // bundle that is Contents/MacOS, so runtime caches would mutate the signed
        // app and make the next build fail code signing. The default project root
        // is user-writable on every platform and is already the path advertised by
        // the editor's project picker.
        config.WorkspaceDirectory = GameEngine::Editor::GetEditorGlobalPaths().defaultProjectRoot.string();
        config.WorkspaceDirectoryIsFallback = true;
    }

    // Watch and hot-reload everything in Assets/
    if (config.AssetDirectory.empty())
        config.AssetDirectory = "Assets";

    GameEngine::ApplyEngineArgsToConfig(engineArgs, config);
    return config;
}
} // namespace GameEngine::Editor::Startup
