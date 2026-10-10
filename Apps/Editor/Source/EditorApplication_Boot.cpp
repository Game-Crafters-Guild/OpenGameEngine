// Boot sequencing for EditorApplication: idle the world path while the project
// picker covers the canvas, and open GE_EDITOR_STARTUP_SCENE only after Update
// has a settled workspace. Split from EditorApplication.cpp.
#include "EditorApplication.h"

#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "Platform/Capabilities.h"
#include "Scene/SceneEditorController.h"

#include <cstdlib>

namespace GameEngine
{

bool EditorApplication::ShouldIdleWorldPathForProjectPicker() const
{
    // Only a host that owns the frame loop needs the world path parked: there a
    // cold first frame behind the picker starves the picker of input. Where the
    // editor pumps its own loop the world keeps rendering behind the picker.
    return Platform::HostDrivesFrameLoop() && m_ProjectFolderModal != nullptr &&
           m_ProjectFolderModal->IsVisible();
}

void EditorApplication::SyncWorldPathForProjectPicker()
{
    const bool idle = ShouldIdleWorldPathForProjectPicker();
    if (idle == m_WorldPathIdledForProjectPicker)
        return;
    m_WorldPathIdledForProjectPicker = idle;
    EngineCore::GetInstance().SetRenderingLoopAutoDrive(!idle);
    if (idle)
        Logger::Log::Info("Editor: idling world path while the project picker is open");
    else
        Logger::Log::Info("Editor: resuming world path after project picker closed");
}

void EditorApplication::QueueDeferredStartupScene()
{
    if (!m_SceneEditor)
        return;
    // Web host maps `?GE_EDITOR_STARTUP_SCENE=` (and WebMain seeds it from the
    // project's game.config when `?project=` is set) so a freshly planted
    // project actually opens its scene. Queue it for Update: doing the open
    // from Initialize is the same boot-open class the wasm last-project skip
    // avoids.
    const char* startupScene = std::getenv("GE_EDITOR_STARTUP_SCENE");
    if (!startupScene || startupScene[0] == '\0')
        return;
    m_DeferredStartupScene = startupScene;
    m_DeferredStartupSceneWorkspace = GetConfig().WorkspaceDirectory;
    Logger::Log::Info(
        "Editor: deferring GE_EDITOR_STARTUP_SCENE '{}' until the world path is settled",
        startupScene);
}

void EditorApplication::TryOpenDeferredStartupScene()
{
    if (m_DeferredStartupScene.empty() || !m_SceneEditor || m_IsShuttingDown)
        return;

    if (m_StartupFrameCount < GetConfig().StartupSceneOpenFrame)
        return;
    if (ShouldIdleWorldPathForProjectPicker())
        return;
    if (GetConfig().WorkspaceDirectoryIsFallback)
        return;

    const std::filesystem::path workspace =
        EngineCore::GetInstance().GetWorkspaceRoot().lexically_normal();
    const std::filesystem::path expected = m_DeferredStartupSceneWorkspace.lexically_normal();
    if (workspace.empty() || workspace != expected)
        return;

    Logger::Log::Info("Editor: opening GE_EDITOR_STARTUP_SCENE '{}'",
                      m_DeferredStartupScene.string());
    const std::filesystem::path path = std::move(m_DeferredStartupScene);
    m_DeferredStartupScene.clear();
    m_SceneEditor->RequestOpenScene(path, /*additive=*/false);
}

} // namespace GameEngine
