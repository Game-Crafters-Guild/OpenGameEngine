#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{

/// The Editor's conventional custom Player source tree. Native gameplay sources
/// elsewhere in Assets belong to the separately staged user module.
std::vector<std::filesystem::path> CollectCustomPlayerSources(const std::filesystem::path& assetRoot);

/// Prepares the desktop executable's source list. Custom sources are read-only;
/// a custom main.cpp or PlayerApplication.cpp replaces the default application.
/// Otherwise DesktopSources.txt in the SDK selects templates to refresh ONLY in
/// generatedSourceDir (normally <project>/.Build/Player/Source).
/// Old Assets/Source template copies have no ownership marker and remain custom
/// project files; exporting never overwrites or deletes them.
bool PreparePlayerSources(const std::filesystem::path& templateDir,
                          const std::filesystem::path& generatedSourceDir,
                          const std::vector<std::filesystem::path>& customSources,
                          std::vector<std::filesystem::path>& sources,
                          std::string& error);

} // namespace GameEngine
