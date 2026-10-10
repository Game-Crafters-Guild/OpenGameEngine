// BuildExporterRegistry semantics — what the Build panel relies on when it
// resolves the exporter for the platform it is about to build:
//   * incomplete registrations are rejected loudly and never resolve,
//   * lookup goes through the exporter's own HandlesPlatform predicate, so an
//     exporter that answers to several spellings resolves for each of them,
//   * same-ExporterId registration replaces forward,
//   * the copied-out descriptor's Run is callable on its own,
//   * the optional OutputDirectory hook is copied out with it, so the panel
//     reports the exporter's own output location without knowing its name.
//
// The registry is a process singleton, so every test uses unique ids and
// platform names.

#include "Editor/Registries/BuildExporterRegistry.h"

#include "Engine/Build/BuildPipeline.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace ed = GameEngine::Editor;

namespace
{

ed::BuildExporterDescriptor MakeExporter(std::string id, std::string platformName)
{
    ed::BuildExporterDescriptor exporter;
    exporter.ExporterId = std::move(id);
    exporter.DisplayName = exporter.ExporterId + " display";
    exporter.HandlesPlatform = [platformName](std::string_view name) { return name == platformName; };
    exporter.Run = [](const GameEngine::BuildSettings&, GameEngine::BuildPipeline&,
                      const ed::BuildExporterDescriptor::ProgressCallback&) { return true; };
    return exporter;
}

} // namespace

TEST(BuildExporterRegistryTests, IncompleteRegistrationIsRejected)
{
    auto& registry = ed::BuildExporterRegistry::Get();
    ed::BuildExporterDescriptor found;

    ed::BuildExporterDescriptor noId = MakeExporter("", "test.reject.noId");
    registry.Register(noId);
    EXPECT_FALSE(registry.TryFindForPlatform("test.reject.noId", found));

    ed::BuildExporterDescriptor noPredicate = MakeExporter("test.reject.noPredicate", "test.reject.noPredicate");
    noPredicate.HandlesPlatform = nullptr;
    registry.Register(noPredicate);
    EXPECT_FALSE(registry.TryFindForPlatform("test.reject.noPredicate", found));

    ed::BuildExporterDescriptor noRun = MakeExporter("test.reject.noRun", "test.reject.noRun");
    noRun.Run = nullptr;
    registry.Register(noRun);
    EXPECT_FALSE(registry.TryFindForPlatform("test.reject.noRun", found));
}

TEST(BuildExporterRegistryTests, ResolvesThroughHandlesPlatformPredicate)
{
    auto& registry = ed::BuildExporterRegistry::Get();

    ed::BuildExporterDescriptor exporter = MakeExporter("test.alias", "test.alias.stored");
    exporter.HandlesPlatform = [](std::string_view name) {
        return name == "test.alias.stored" || name == "test.alias.spelling";
    };
    registry.Register(exporter);

    ed::BuildExporterDescriptor found;
    ASSERT_TRUE(registry.TryFindForPlatform("test.alias.stored", found));
    EXPECT_EQ(found.ExporterId, "test.alias");
    EXPECT_EQ(found.DisplayName, "test.alias display");

    ASSERT_TRUE(registry.TryFindForPlatform("test.alias.spelling", found));
    EXPECT_EQ(found.ExporterId, "test.alias");
    // The copied predicate is what the Build panel uses to make the spellings
    // select the same platform row.
    EXPECT_TRUE(found.HandlesPlatform("test.alias.stored"));
    EXPECT_FALSE(found.HandlesPlatform("test.alias.other"));

    EXPECT_FALSE(registry.TryFindForPlatform("test.alias.other", found));
}

TEST(BuildExporterRegistryTests, SameIdReplacesForward)
{
    auto& registry = ed::BuildExporterRegistry::Get();

    registry.Register(MakeExporter("test.replace", "test.replace.first"));
    registry.Register(MakeExporter("test.replace", "test.replace.second"));

    ed::BuildExporterDescriptor found;
    EXPECT_FALSE(registry.TryFindForPlatform("test.replace.first", found));
    ASSERT_TRUE(registry.TryFindForPlatform("test.replace.second", found));
    EXPECT_EQ(found.ExporterId, "test.replace");
}

TEST(BuildExporterRegistryTests, CopiedRunIsCallableWithPipelineAndProgress)
{
    auto& registry = ed::BuildExporterRegistry::Get();

    ed::BuildExporterDescriptor exporter = MakeExporter("test.run", "test.run.platform");
    exporter.Run = [](const GameEngine::BuildSettings& settings, GameEngine::BuildPipeline& pipeline,
                      const ed::BuildExporterDescriptor::ProgressCallback& progress) {
        GameEngine::BuildProgress report;
        report.currentStage = GameEngine::BuildProgress::Stage::Complete;
        report.progress = 1.0f;
        report.statusMessage = settings.platformName + " done";
        report.cancelled = pipeline.IsCancelled();
        progress(report);
        return !report.cancelled;
    };
    registry.Register(exporter);

    ed::BuildExporterDescriptor found;
    ASSERT_TRUE(registry.TryFindForPlatform("test.run.platform", found));

    GameEngine::BuildSettings settings;
    settings.platformName = "test.run.platform";
    GameEngine::BuildPipeline pipeline(nullptr);
    GameEngine::BuildProgress last;
    int reports = 0;
    const bool ok = found.Run(settings, pipeline, [&](const GameEngine::BuildProgress& p) {
        last = p;
        ++reports;
    });

    EXPECT_TRUE(ok);
    EXPECT_EQ(reports, 1);
    EXPECT_EQ(last.currentStage, GameEngine::BuildProgress::Stage::Complete);
    EXPECT_EQ(last.statusMessage, "test.run.platform done");
    EXPECT_FALSE(last.cancelled);
}

TEST(BuildExporterRegistryTests, CopiedOutputDirectoryReportsExporterLocation)
{
    auto& registry = ed::BuildExporterRegistry::Get();

    ed::BuildExporterDescriptor nested = MakeExporter("test.output.nested", "test.output.nested");
    nested.OutputDirectory = [](const GameEngine::BuildSettings& settings) {
        return settings.outputDirectory / "package";
    };
    registry.Register(nested);
    registry.Register(MakeExporter("test.output.default", "test.output.default"));

    GameEngine::BuildSettings settings;
    settings.outputDirectory = "Build/Target";

    ed::BuildExporterDescriptor found;
    ASSERT_TRUE(registry.TryFindForPlatform("test.output.nested", found));
    ASSERT_TRUE(found.OutputDirectory);
    EXPECT_EQ(found.OutputDirectory(settings), std::filesystem::path("Build/Target") / "package");

    ASSERT_TRUE(registry.TryFindForPlatform("test.output.default", found));
    EXPECT_FALSE(found.OutputDirectory);
}
