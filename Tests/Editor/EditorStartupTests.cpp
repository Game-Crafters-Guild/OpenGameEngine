#include "Editor/EditorPaths.h"
#include "Startup/EditorStartup.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace GameEngine::Editor::Startup
{
// WorkspaceDirectoryIsFallback carries the project precedence downstream: the startup
// auto-load consults the last-project preference only while the flag is set, so an
// explicit --project must clear it or the preference replaces the requested project.
TEST(EditorStartupTests, UsesWritableDefaultProjectWhenProjectIsUnspecified)
{
    const EditorCommandLineArgs editorArgs;
    const EngineArgs engineArgs;

    const ApplicationConfig config = BuildEditorApplicationConfig(editorArgs, engineArgs);

    EXPECT_EQ(std::filesystem::path(config.WorkspaceDirectory),
              GameEngine::Editor::GetEditorGlobalPaths().defaultProjectRoot);
    EXPECT_FALSE(config.WorkspaceDirectory.empty());
    EXPECT_TRUE(config.WorkspaceDirectoryIsFallback);
}

TEST(EditorStartupTests, PreservesExplicitProjectRoot)
{
    EditorCommandLineArgs editorArgs;
    editorArgs.projectRoot = std::filesystem::path("/test/project");
    const EngineArgs engineArgs;

    const ApplicationConfig config = BuildEditorApplicationConfig(editorArgs, engineArgs);

    EXPECT_EQ(std::filesystem::path(config.WorkspaceDirectory), *editorArgs.projectRoot);
    EXPECT_FALSE(config.WorkspaceDirectoryIsFallback);
}
} // namespace GameEngine::Editor::Startup
