#pragma once

#include <filesystem>

namespace GameEngine
{
class AssetManager;
}

namespace GameEngine::Editor
{
class SettingsStore;

// Load the active render pipeline path from project settings, normalizing
// absolute/stale paths into portable asset-relative form. Returns empty
// path when no setting is configured or the value cannot be resolved.
std::filesystem::path LoadActiveRenderPipelinePathFromProjectSettings(
    const std::filesystem::path& workspaceRoot);

// Persist the active render pipeline path into the project settings JSON.
// Returns false if the file could not be written.
bool PersistActiveRenderPipelinePathToProjectSettings(
    SettingsStore& store,
    const std::filesystem::path& pipelinePath,
    const char* contextLabel = nullptr);

// Normalize a configured render pipeline path (which might be absolute,
// prefixed with "Assets/", etc.) into a portable asset-relative form.
// Sets outWasMigrated=true when the value was changed from its input form.
std::filesystem::path NormalizeConfiguredRenderPipelinePath(
    const std::filesystem::path& configuredPath,
    const AssetManager& assetManager,
    bool& outWasMigrated);

} // namespace GameEngine::Editor
