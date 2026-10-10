#pragma once

#include <filesystem>
#include <functional>
#include <optional>

namespace GameEngine::Editor
{

// The path of the scene the editor has open, or nullopt when it has none (an
// untitled scene has no path until it is saved).
using ActiveScenePathProvider = std::function<std::optional<std::filesystem::path>()>;

// Registers the project-wide asset pipeline commands with EditorMenuRegistry
// under their UiReplayCommandIds: Reimport LODs (project) and Bake HLOD
// (project). Call once at startup, after the scene editor exists.
void RegisterAssetPipelineCommands(ActiveScenePathProvider activeScenePath);

} // namespace GameEngine::Editor
