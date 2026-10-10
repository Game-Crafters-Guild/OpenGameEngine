#pragma once

#include <filesystem>
#include <string>

namespace GameEngine::Editor::Startup
{
struct BuiltinSyncStats;

// Install-provided Editor assets (shipped with the app / staged next to the executable).
std::filesystem::path GetEditorInstallAssetsRoot();

// User-writable root for editor-owned caches.
std::filesystem::path GetEditorUserCacheRoot();

// A user-writable default project location used when the user hasn't chosen a project yet.
std::filesystem::path GetDefaultProjectRoot();

// User-writable editor assets root (seeded from install assets on first run).
std::filesystem::path GetEditorUserAssetsRoot();

// Seed user-writable editor assets from the install assets directory. Custom extra
// files are preserved; engine-owned render pipelines and shaders are refreshed
// (byte-identical copies skipped), plus UI assets on macOS where the runtime mounts
// a writable mirror of the signed bundle. Builtins a previous install shipped that
// the current one no longer does are pruned via the manifest beside the mirror.
bool EnsureEditorUserAssetsSeeded(std::string* outError = nullptr,
                                  BuiltinSyncStats* outStats = nullptr);
} // namespace GameEngine::Editor::Startup
