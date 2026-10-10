#include <gtest/gtest.h>

#include "ExternalScriptEditorLauncher.h"

#include "JobSystem/WorkStealingThreadPool.h"
#include "Scripting/ScriptManager.h"
#include "Scripting/ScriptsConfig.h"

#include "../TestTempDir.h"

#include <filesystem>

using namespace GameEngine;

// A script opens in the external editor with the scripts project the editor
// generates. The default layout writes it in the asset root, apart from the
// compiled assemblies, so a project looked for beside the assemblies is never
// there and the script opens with no project.
TEST(ExternalScriptEditorLauncher, OpensScriptsWithTheGeneratedProject)
{
    const TestUtils::ScopedTempDir workspace{TestUtils::MakeUniqueTempDirectory("ge_script_launcher_project")};
    ScriptsConfig config{};
    config.workspaceRoot = workspace.Path();
    config.scriptsRoot = workspace.Path() / "Assets";
    config.assembliesRoot = workspace.Path() / "ScriptAssemblies";
    config.generatedProjectRoot = workspace.Path() / "Assets";
    config.disableClr = true;
    config.enableHotReload = false;
    config.enableAsyncHotReload = false;
    config.enableAutoProjectGeneration = true;

    JobSystem::WorkStealingThreadPool pool(1);
    ScriptManager scripts;
    ASSERT_TRUE(scripts.Initialize(config, pool));

    const std::filesystem::path project = ExternalScriptEditorLauncher::ScriptProjectPath(scripts);
    EXPECT_EQ(project, workspace.Path() / "Assets" / "GameEngine.Scripts.csproj");
    EXPECT_TRUE(std::filesystem::exists(project)) << "no project at " << project.string();
    scripts.Shutdown();
}
