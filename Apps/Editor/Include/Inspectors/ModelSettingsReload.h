#pragma once

#include <filesystem>

namespace GameEngine
{
class AssetManager;
class GUID;
} // namespace GameEngine

namespace GameEngine::Editor
{

/// Applies a model's changed import settings, never waiting on the calling thread: a resident
/// model (or one still loading) reloads on a worker; a model that is not loaded is requested,
/// named on the Scene View while it loads ("Loading <file>..."), and announced as reloaded once
/// it lands, so the inspector's preview and the thumbnails follow the new settings. `path` is the
/// model's source file. Main thread.
void ReloadModelForChangedSettings(AssetManager& assets, const GUID& guid, const std::filesystem::path& path);

} // namespace GameEngine::Editor
