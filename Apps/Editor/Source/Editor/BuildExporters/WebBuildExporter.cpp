#include "Editor/BuildExporters/WebBuildExporter.h"

#include "Editor/Registries/BuildExporterRegistry.h"
#include "Editor/EditorPaths.h"
#include "Editor/Settings/SettingsStore.h"
#include "Engine/Build/BuildPipeline.h"
#include "Engine/Build/BuildPlatforms.h"
#include "Engine/Build/WebExportRunner.h"
#include "Logger/Logger.h"

#include <filesystem>
#include <string>
#include <utility>

namespace GameEngine::Editor
{
namespace
{

bool RunWebBuildExport(const BuildSettings& settings, BuildPipeline& pipeline,
                       const BuildExporterDescriptor::ProgressCallback& progress)
{
    if (pipeline.IsCancelled())
        return false;

    progress(BuildProgress{BuildProgress::Stage::PreparingProject, 0.05f,
                           "Exporting web build..."});

    std::string error;
    // Host-resolved inputs: the template is an editor preference and the shader
    // packages come from this Editor's staged assets, so the editor resolves both and
    // engine build tooling keeps no dependency on editor settings or paths.
    WebExportInputs webInputs;
    webInputs.shaderPackagesRoot = GetEditorGlobalPaths().installAssetsRoot / "Shaders";
    {
        auto prefs = settings.projectRoot.empty() ? OpenEditorPreferences()
                                                  : OpenProjectSettings(settings.projectRoot);
        std::string prefError;
        prefs.Load(&prefError);
        std::string configured;
        if (prefs.TryGetString("build.platform.Web.playerTemplate", configured)
            && !configured.empty())
        {
            webInputs.playerTemplate = std::filesystem::path(configured);
            if (webInputs.playerTemplate.is_relative() && !settings.projectRoot.empty())
                webInputs.playerTemplate = settings.projectRoot / webInputs.playerTemplate;
        }
    }
    const bool exported = RunWebExport(
        settings, pipeline, webInputs,
        [&progress](const std::string& line) {
            if (line.empty())
                return;
            Logger::Log::Info("Build: {}", line);
            progress(BuildProgress{BuildProgress::Stage::CollectingAssets, 0.5f, line});
        },
        error);

    if (!exported)
    {
        const bool cancelled = pipeline.IsCancelled();
        Logger::Log::Error("Build: {}", error);
        progress(BuildProgress{BuildProgress::Stage::Failed, 0.0f, error, {}, {}, cancelled});
        return false;
    }

    progress(BuildProgress{BuildProgress::Stage::Complete, 1.0f,
                           "Web export complete: " + settings.outputDirectory.string()});
    return true;
}

} // namespace

void RegisterWebBuildExporter()
{
    BuildExporterDescriptor exporter;
    exporter.ExporterId = "web";
    exporter.DisplayName = "Web export";
    exporter.HandlesPlatform = [](std::string_view platformName) {
        return platformName == kWebBuildPlatformName;
    };
    exporter.Run = &RunWebBuildExport;
    BuildExporterRegistry::Get().Register(std::move(exporter));
}

} // namespace GameEngine::Editor
