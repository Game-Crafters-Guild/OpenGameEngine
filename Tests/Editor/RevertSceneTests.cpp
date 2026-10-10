#include <gtest/gtest.h>

#include "EditorChangeNotifications.h"
#include "Scene/SceneEditorController.h"

#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "UI/UIElement.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{
using namespace GameEngine;
using GameEngine::ECS::World;
namespace fs = std::filesystem;

constexpr const char* kOneEntityScene =
    "[scene name=\"RevertTarget\" version=1]\n"
    "\n"
    "[entity id=\"kept\"]\n"
    "Transform.position = (1, 2, 3)\n";

fs::path ProcessScratchRoot()
{
#if defined(_WIN32)
    const auto pid = static_cast<unsigned long>(_getpid());
#else
    const auto pid = static_cast<unsigned long>(getpid());
#endif
    return fs::temp_directory_path() / ("GameEngine_RevertSceneTests_" + std::to_string(pid));
}

class ScopedUserDataRedirect
{
  public:
    explicit ScopedUserDataRedirect(const fs::path& dir)
    {
#if defined(_WIN32)
        Capture("APPDATA");
        Capture("LOCALAPPDATA");
        _putenv_s("APPDATA", dir.string().c_str());
        _putenv_s("LOCALAPPDATA", dir.string().c_str());
#elif defined(__APPLE__)
        Capture("HOME");
        setenv("HOME", dir.string().c_str(), 1);
#else
        Capture("XDG_DATA_HOME");
        setenv("XDG_DATA_HOME", dir.string().c_str(), 1);
#endif
    }

    ~ScopedUserDataRedirect()
    {
        for (const auto& [name, value] : m_Saved)
        {
#if defined(_WIN32)
            _putenv_s(name.c_str(), value.c_str());
#else
            if (value.empty())
                unsetenv(name.c_str());
            else
                setenv(name.c_str(), value.c_str(), 1);
#endif
        }
    }

  private:
    void Capture(const char* name)
    {
        const char* v = std::getenv(name);
        m_Saved.emplace_back(name, v ? v : "");
    }

    std::vector<std::pair<std::string, std::string>> m_Saved;
};

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

class RevertSceneTests : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        EngineCore& engine = EngineCore::GetInstance();
        if (!engine.IsInitialized())
        {
            ApplicationConfig config{};
            config.AssetDirectory = ".";
            config.WorkspaceDirectory = ".";
            config.EnableEditor = true;
            ASSERT_TRUE(engine.Initialize(config));
        }
    }

    void SetUp() override
    {
        m_Dir = ProcessScratchRoot();
        std::error_code ec;
        fs::remove_all(m_Dir, ec);
        fs::create_directories(m_Dir, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_Dir, ec);
    }

    fs::path WriteScene(const std::string& name, const char* contents) const
    {
        const fs::path path = m_Dir / name;
        std::ofstream out(path);
        EXPECT_TRUE(out.good()) << path;
        out << contents;
        return path;
    }

    fs::path m_Dir;
};
} // namespace

TEST(RevertSceneWiring, ToolbarSaveMenuDeclaresRevertScene)
{
    const std::string src = ReadFile(std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Source" /
                                     "UI" / "Controls" / "EditorTopToolbar.cpp");
    ASSERT_FALSE(src.empty()) << "EditorTopToolbar.cpp not found under " << GE_EDITOR_SOURCE_DIR;
    EXPECT_NE(src.find("Revert Scene"), std::string::npos)
        << "the save-button context menu does not declare Revert Scene";
    EXPECT_NE(src.find("onRevertSceneClicked"), std::string::npos)
        << "the save-button menu is not wired to the revert callback";
}

TEST_F(RevertSceneTests, ConfirmReloadsTheSavedSceneAndClearsDirty)
{
    const fs::path scene = WriteScene("revert_target.scene", kOneEntityScene);

    World world;
    Editor::EditorChangeNotifications notifications;
    UIElement root("TestRoot");
    Editor::SceneEditorController controller;
    controller.AttachToRoot(root, world, notifications, nullptr);

    ScopedUserDataRedirect prefsGuard(m_Dir);
    controller.RequestOpenScene(scene, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);

    ASSERT_TRUE(controller.GetActiveScenePath().has_value());
    const std::size_t savedCount = world.GetEntityCount();
    ASSERT_GE(savedCount, 1u);

    world.CreateEntity();
    controller.MarkSceneDirty();
    ASSERT_TRUE(controller.IsSceneDirty());
    ASSERT_GT(world.GetEntityCount(), savedCount);

    controller.RequestRevertScene();
    EXPECT_EQ(controller.GetActiveModalKind(), "revertScene");
    EXPECT_TRUE(controller.IsAnyModalOpen());

    std::string error;
    ASSERT_TRUE(controller.RespondToActiveModal("confirm", &error)) << error;
    EXPECT_TRUE(controller.GetActiveModalKind().empty());

    controller.Update(nullptr);
    EXPECT_EQ(world.GetEntityCount(), savedCount);
    EXPECT_FALSE(controller.IsSceneDirty());
    ASSERT_TRUE(controller.GetActiveScenePath().has_value());
    EXPECT_EQ(*controller.GetActiveScenePath(), scene);
}

TEST_F(RevertSceneTests, CancelLeavesTheDirtyWorldUntouched)
{
    const fs::path scene = WriteScene("revert_cancel.scene", kOneEntityScene);

    World world;
    Editor::EditorChangeNotifications notifications;
    UIElement root("TestRoot");
    Editor::SceneEditorController controller;
    controller.AttachToRoot(root, world, notifications, nullptr);

    ScopedUserDataRedirect prefsGuard(m_Dir);
    controller.RequestOpenScene(scene, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);

    world.CreateEntity();
    controller.MarkSceneDirty();
    const std::size_t dirtyCount = world.GetEntityCount();

    controller.RequestRevertScene();
    ASSERT_EQ(controller.GetActiveModalKind(), "revertScene");

    std::string error;
    ASSERT_TRUE(controller.RespondToActiveModal("cancel", &error)) << error;
    controller.Update(nullptr);

    EXPECT_TRUE(controller.GetActiveModalKind().empty());
    EXPECT_EQ(world.GetEntityCount(), dirtyCount);
    EXPECT_TRUE(controller.IsSceneDirty());
}

TEST_F(RevertSceneTests, UntitledCleanSceneDoesNotOpenTheModal)
{
    World world;
    Editor::EditorChangeNotifications notifications;
    UIElement root("TestRoot");
    Editor::SceneEditorController controller;
    controller.AttachToRoot(root, world, notifications, nullptr);

    controller.RequestNewScene();
    controller.Update(nullptr);
    EXPECT_FALSE(controller.GetActiveScenePath().has_value());
    EXPECT_FALSE(controller.IsSceneDirty());

    controller.RequestRevertScene();
    EXPECT_TRUE(controller.GetActiveModalKind().empty());
    EXPECT_FALSE(controller.IsAnyModalOpen());
}

TEST_F(RevertSceneTests, UntitledDirtyConfirmStartsANewCleanScene)
{
    World world;
    Editor::EditorChangeNotifications notifications;
    UIElement root("TestRoot");
    Editor::SceneEditorController controller;
    controller.AttachToRoot(root, world, notifications, nullptr);

    controller.RequestNewScene();
    controller.Update(nullptr);
    ASSERT_FALSE(controller.GetActiveScenePath().has_value());
    const std::size_t seededCount = world.GetEntityCount();

    world.CreateEntity();
    controller.MarkSceneDirty();
    ASSERT_TRUE(controller.IsSceneDirty());
    ASSERT_GT(world.GetEntityCount(), seededCount);

    controller.RequestRevertScene();
    EXPECT_EQ(controller.GetActiveModalKind(), "revertScene");
    EXPECT_TRUE(controller.IsAnyModalOpen());

    std::string error;
    ASSERT_TRUE(controller.RespondToActiveModal("confirm", &error)) << error;
    EXPECT_TRUE(controller.GetActiveModalKind().empty());

    controller.Update(nullptr);
    EXPECT_FALSE(controller.GetActiveScenePath().has_value());
    EXPECT_FALSE(controller.IsSceneDirty());
    EXPECT_EQ(world.GetEntityCount(), seededCount);
}

TEST_F(RevertSceneTests, PlayModeDoesNotOpenTheModal)
{
    const fs::path scene = WriteScene("revert_play.scene", kOneEntityScene);

    World world;
    Editor::EditorChangeNotifications notifications;
    UIElement root("TestRoot");
    Editor::SceneEditorController controller;
    controller.AttachToRoot(root, world, notifications, nullptr);
    controller.SetPlayModeProbe([]() { return true; });

    ScopedUserDataRedirect prefsGuard(m_Dir);
    controller.RequestOpenScene(scene, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);
    ASSERT_TRUE(controller.GetActiveScenePath().has_value());

    controller.MarkSceneDirty();
    controller.RequestRevertScene();
    EXPECT_TRUE(controller.GetActiveModalKind().empty())
        << "reverting while play mode holds the simulated world must not open the confirm modal";
    EXPECT_TRUE(controller.IsSceneDirty());
}

TEST_F(RevertSceneTests, ConfirmDuringPlayDoesNotReplaceTheWorld)
{
    const fs::path scene = WriteScene("revert_confirm_play.scene", kOneEntityScene);

    bool playing = false;
    World world;
    Editor::EditorChangeNotifications notifications;
    UIElement root("TestRoot");
    Editor::SceneEditorController controller;
    controller.AttachToRoot(root, world, notifications, nullptr);
    controller.SetPlayModeProbe([&playing]() { return playing; });

    ScopedUserDataRedirect prefsGuard(m_Dir);
    controller.RequestOpenScene(scene, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);
    ASSERT_TRUE(controller.GetActiveScenePath().has_value());

    world.CreateEntity();
    controller.MarkSceneDirty();
    const std::size_t dirtyCount = world.GetEntityCount();
    ASSERT_TRUE(controller.IsSceneDirty());

    controller.RequestRevertScene();
    ASSERT_EQ(controller.GetActiveModalKind(), "revertScene");

    playing = true;
    std::string error;
    ASSERT_TRUE(controller.RespondToActiveModal("confirm", &error)) << error;
    EXPECT_TRUE(controller.GetActiveModalKind().empty());

    controller.Update(nullptr);
    EXPECT_EQ(world.GetEntityCount(), dirtyCount)
        << "confirming revert after play starts must not replace the simulating world";
    EXPECT_TRUE(controller.IsSceneDirty());
    ASSERT_TRUE(controller.GetActiveScenePath().has_value());
    EXPECT_EQ(*controller.GetActiveScenePath(), scene);
}
