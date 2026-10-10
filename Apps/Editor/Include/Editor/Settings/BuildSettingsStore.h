#pragma once

#include <string>
#include <vector>

namespace GameEngine::Editor
{

// Build settings persist per-project in ProjectSettings.json while a project is
// open, and in Editor preferences otherwise. Every reader and writer goes
// through this store so the two locations can never disagree about which one a
// given key lives in.
//
// An empty stored value reads as absent, so a cleared field falls back to its
// default rather than to "".
std::string LoadBuildStringSetting(const std::string& prefKey, const std::string& defaultValue = {});
void SaveBuildStringSetting(const std::string& prefKey, const std::string& value);
bool LoadBuildBoolSetting(const std::string& prefKey, bool defaultValue);
void SaveBuildBoolSetting(const std::string& prefKey, bool value);

// Pref keys the Build page, the Build panel and the exporters share.
inline constexpr const char* kBuildGameNamePrefKey = "build.gameName";
inline constexpr const char* kGlobalBuildIconPrefKey = "build.globalIcon";

std::string BuildPlatformPrefKey(const std::string& platformName, const std::string& leaf);

// Scene lists are stored newline-separated under a single key.
std::vector<std::string> LoadBuildScenes(const std::string& platformName);
void SaveBuildScenes(const std::string& platformName, const std::vector<std::string>& scenes);
std::vector<std::string> LoadGlobalBuildScenes();
void SaveGlobalBuildScenes(const std::vector<std::string>& scenes);

// Reads that own a non-false default; the matching writes go through
// SaveBuildBoolSetting so the default has exactly one home.
bool LoadBuildPlatformUseGlobalScenes(const std::string& platformName);
bool LoadBuildPlatformUseGlobalIcon(const std::string& platformName);

std::string LoadBuildGlobalIcon();
std::string LoadBuildPlatformIcon(const std::string& platformName);

} // namespace GameEngine::Editor
