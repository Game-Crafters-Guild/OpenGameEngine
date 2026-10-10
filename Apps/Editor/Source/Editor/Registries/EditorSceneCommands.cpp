#include "Editor/Registries/EditorSceneCommands.h"

#include "Logger/Logger.h"

#include <utility>

namespace GameEngine::Editor
{

EditorSceneCommands& EditorSceneCommands::Get()
{
    static EditorSceneCommands s_Instance;
    return s_Instance;
}

void EditorSceneCommands::SetOpenSceneHandler(OpenSceneHandler handler)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_OpenScene = std::move(handler);
}

void EditorSceneCommands::SetRegisterAssetsHandler(RegisterAssetsHandler handler)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_RegisterAssets = std::move(handler);
}

void EditorSceneCommands::OpenScene(const std::filesystem::path& scenePath) const
{
    OpenSceneHandler handler;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        handler = m_OpenScene;
    }
    if (!handler)
    {
        Logger::Log::Error("EditorSceneCommands: OpenScene('{}') before the editor installed a "
                           "handler; ignored",
                           scenePath.string());
        return;
    }
    handler(scenePath);
}

size_t EditorSceneCommands::RegisterImportedAssets(
    const std::vector<std::filesystem::path>& files) const
{
    RegisterAssetsHandler handler;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        handler = m_RegisterAssets;
    }
    if (!handler)
    {
        Logger::Log::Warning("EditorSceneCommands: RegisterImportedAssets({} file(s)) before the "
                             "editor installed a handler; they resolve after the next scan",
                             files.size());
        return files.size();
    }
    return handler(files);
}

} // namespace GameEngine::Editor
