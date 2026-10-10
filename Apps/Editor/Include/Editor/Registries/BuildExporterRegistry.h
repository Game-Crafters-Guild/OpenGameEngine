#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
struct BuildProgress;
struct BuildSettings;
class BuildPipeline;
}

namespace GameEngine::Editor
{

// What a build target's owning unit registers to run its own export in place
// of BuildPipeline's native compile-and-package phases. The Build panel asks
// the registry for the exporter of the platform it is about to build and runs
// it on the build thread; a platform nobody claims packages through
// BuildPipeline::Execute alone.
struct BuildExporterDescriptor
{
    // Stable id and replace-forward key, e.g. "web", "steamDeck".
    std::string ExporterId;
    std::string DisplayName; // e.g. "Web export"
    // Whether this exporter builds the named target. Called with the stored
    // platform name (BuildPlatformInfo::Name) and with the aliases the
    // debug/MCP surface accepts, so an exporter that answers to several
    // spellings makes them equivalent for platform selection too.
    std::function<bool(std::string_view platformName)> HandlesPlatform;

    using ProgressCallback = std::function<void(const BuildProgress&)>;
    // Runs the export on the build thread. `pipeline` is the cancel probe
    // (IsCancelled) and the cancellable child-process host (RunShellCommand);
    // an exporter that packages natively first calls its Execute. Reports the
    // terminal BuildProgress (Complete, or Failed with `cancelled` set when
    // the pipeline was cancelled) through `progress` and returns whether the
    // export succeeded.
    std::function<bool(const BuildSettings& settings, BuildPipeline& pipeline,
                       const ProgressCallback& progress)>
        Run;

    // Where the finished game lands for `settings`, when the exporter writes
    // it somewhere other than settings.outputDirectory. The Build panel
    // reports this directory as the build's output. Optional.
    std::function<std::filesystem::path(const BuildSettings& settings)> OutputDirectory;
};

// Registration and lookup are main-thread only (editor startup, Build panel
// event handlers), matching the other editor registries. Registering an
// ExporterId again replaces the earlier descriptor in place.
class BuildExporterRegistry
{
  public:
    static BuildExporterRegistry& Get();

    void Register(BuildExporterDescriptor descriptor);

    // Copies out the exporter that handles `platformName`. False when the
    // platform packages through BuildPipeline alone.
    bool TryFindForPlatform(std::string_view platformName,
                            BuildExporterDescriptor& outDescriptor) const;

  private:
    BuildExporterRegistry() = default;

    std::vector<BuildExporterDescriptor> m_Exporters; // registration order
};

} // namespace GameEngine::Editor
