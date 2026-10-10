#include "Editor/Settings/BuildRenderPipelineSettings.h"

#include "Editor/Settings/BuildSettingsStore.h"
#include "Editor/Settings/RenderPipelineSettings.h"
#include "Core/Engine.h"

#include <algorithm>
#include <cctype>

namespace GameEngine {
namespace {

using Editor::LoadBuildBoolSetting;
using Editor::LoadBuildStringSetting;
using Editor::SaveBuildBoolSetting;
using Editor::SaveBuildStringSetting;

std::string ToLowerAsciiCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

constexpr const char* kGlobalBuildRenderPipelinePrefKey = "build.globalRenderPipeline";

std::string PlatformUseGlobalRenderPipelinePrefKey(const std::string& platformName)
{
    return "build.platform." + platformName + ".useGlobalRenderPipeline";
}

std::string PlatformRenderPipelinePrefKey(const std::string& platformName)
{
    return "build.platform." + platformName + ".renderPipeline";
}

} // namespace

std::string LoadBuildGlobalRenderPipeline()
{
    return LoadBuildStringSetting(kGlobalBuildRenderPipelinePrefKey);
}

void SaveBuildGlobalRenderPipeline(const std::string& path)
{
    SaveBuildStringSetting(kGlobalBuildRenderPipelinePrefKey, path);
}

bool LoadBuildPlatformUseGlobalRenderPipeline(const std::string& platformName)
{
    return LoadBuildBoolSetting(PlatformUseGlobalRenderPipelinePrefKey(platformName), true);
}

void SaveBuildPlatformUseGlobalRenderPipeline(const std::string& platformName, bool useGlobal)
{
    SaveBuildBoolSetting(PlatformUseGlobalRenderPipelinePrefKey(platformName), useGlobal);
}

std::string LoadBuildPlatformRenderPipeline(const std::string& platformName)
{
    return LoadBuildStringSetting(PlatformRenderPipelinePrefKey(platformName));
}

void SaveBuildPlatformRenderPipeline(const std::string& platformName, const std::string& path)
{
    SaveBuildStringSetting(PlatformRenderPipelinePrefKey(platformName), path);
}

std::string ResolveBuildGlobalRenderPipeline()
{
    const std::string configured = LoadBuildGlobalRenderPipeline();
    if (!configured.empty())
        return configured;

    const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    const std::string editorPipeline =
        Editor::LoadActiveRenderPipelinePathFromProjectSettings(workspaceRoot).generic_string();
    if (!editorPipeline.empty())
        return editorPipeline;

    return kDefaultBuildRenderPipelinePath;
}

std::string ResolveBuildRenderPipelineForPlatform(const std::string& platformName)
{
    if (LoadBuildPlatformUseGlobalRenderPipeline(platformName))
        return ResolveBuildGlobalRenderPipeline();

    const std::string platformPipeline = LoadBuildPlatformRenderPipeline(platformName);
    if (!platformPipeline.empty())
        return platformPipeline;

    if (ToLowerAsciiCopy(platformName) == "steam")
        return kDefaultBuildRenderPipelinePath;

    return ResolveBuildGlobalRenderPipeline();
}

} // namespace GameEngine
