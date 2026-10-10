// EditorSceneCommands semantics — the installed-service seam package modules
// (the unity-import modal) reach the editor's scene/asset flows through:
//   * OpenScene dispatches through the editor-installed handler and is a loud
//     no-op without one,
//   * RegisterImportedAssets returns the handler's failure count, and reports
//     EVERY file as unregistered when no handler is installed (callers treat
//     unregistered as "resolves after the next scan", never as fatal).
//
// Links EditorSDK.dll — the same service instance Editor.exe and module DLLs
// share. Process-wide: tests restore empty handlers before finishing.

#include "Editor/Registries/EditorSceneCommands.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace ed = GameEngine::Editor;

namespace
{
struct HandlerResetGuard
{
    ~HandlerResetGuard()
    {
        ed::EditorSceneCommands::Get().SetOpenSceneHandler(nullptr);
        ed::EditorSceneCommands::Get().SetRegisterAssetsHandler(nullptr);
    }
};
} // namespace

TEST(EditorSceneCommands, OpenSceneIsLoudNoOpWithoutHandlerAndDispatchesWithOne)
{
    auto& commands = ed::EditorSceneCommands::Get();
    HandlerResetGuard reset;

    commands.SetOpenSceneHandler(nullptr);
    commands.OpenScene("Scenes/SdkTestNoHandler.scene"); // must not crash

    std::filesystem::path opened;
    commands.SetOpenSceneHandler([&](const std::filesystem::path& p) { opened = p; });
    commands.OpenScene("Scenes/SdkTestOpen.scene");
    EXPECT_EQ(opened, std::filesystem::path("Scenes/SdkTestOpen.scene"));
}

TEST(EditorSceneCommands, RegisterImportedAssetsReportsAllUnregisteredWithoutHandler)
{
    auto& commands = ed::EditorSceneCommands::Get();
    HandlerResetGuard reset;

    commands.SetRegisterAssetsHandler(nullptr);
    const std::vector<std::filesystem::path> files{"a.png", "b.fbx", "c.scene"};
    EXPECT_EQ(commands.RegisterImportedAssets(files), files.size());
    EXPECT_EQ(commands.RegisterImportedAssets({}), 0u);
}

TEST(EditorSceneCommands, RegisterImportedAssetsReturnsHandlerFailureCount)
{
    auto& commands = ed::EditorSceneCommands::Get();
    HandlerResetGuard reset;

    std::vector<std::filesystem::path> seen;
    commands.SetRegisterAssetsHandler([&](const std::vector<std::filesystem::path>& files) -> size_t {
        seen = files;
        return 1; // one simulated registration failure
    });
    const std::vector<std::filesystem::path> files{"x.png", "y.fbx"};
    EXPECT_EQ(commands.RegisterImportedAssets(files), 1u);
    EXPECT_EQ(seen, files);
}
