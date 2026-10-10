#include <gtest/gtest.h>

#include "EditorChangeNotifications.h"
#include "Scene/SceneDocumentManager.h"
#include "Scene/SceneEditorController.h"

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "Scene/SceneIO.h"
#include "UI/UIElement.h"

#include "TestTempDir.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace GameEngine;
using GameEngine::ECS::World;
namespace fs = std::filesystem;

// Scene opens must resolve project-relative paths through the AssetManager
// mounts (project first, then registered sources), never the process CWD:
// scripted/IPC opens run with the editor's CWD at the exe directory, where a
// CWD-relative read finds nothing. These tests drive the shared open funnel
// (SceneEditorController::RequestOpenScene -> Update) headless.

class ScopedCurrentPath
{
  public:
    explicit ScopedCurrentPath(const fs::path& newCurrentPath)
    {
        std::error_code ec;
        m_PreviousPath = fs::current_path(ec);
        if (ec)
            return;
        fs::current_path(newCurrentPath, ec);
        m_Active = !ec;
    }

    ~ScopedCurrentPath()
    {
        if (!m_Active)
            return;
        std::error_code ec;
        fs::current_path(m_PreviousPath, ec);
    }

    bool IsActive() const { return m_Active; }

  private:
    fs::path m_PreviousPath;
    bool m_Active{false};
};

// Redirects the user-data env vars so AddRecentScenePath (triggered by
// successful opens) writes Preferences.json under a temp dir instead of the
// developer's real profile.
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

class OpenSceneResolutionTests : public ::testing::Test
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
        m_AssetRoot = EngineCore::GetInstance().GetAssetManager().GetAssetRoot();
        ASSERT_FALSE(m_AssetRoot.empty());
        ASSERT_TRUE(m_AssetRoot.is_absolute());

        // Every test process of this build shares the asset root: a subfolder of this process
        // keeps a concurrent run of these tests from deleting the scenes this one opens.
        m_SceneSubdir = "OpenSceneResolutionTests_" + std::to_string(TestUtils::GetProcessIdForTests());
        m_SceneDir = m_AssetRoot / m_SceneSubdir;
        m_AwayDir = TestUtils::MakeUniqueTempDirectory("GameEngine_OpenSceneResolutionTests_away");
        std::error_code ec;
        fs::remove_all(m_SceneDir, ec);
        fs::create_directories(m_SceneDir, ec);
        fs::create_directories(m_AwayDir, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_SceneDir, ec);
        fs::remove_all(m_AwayDir, ec);
    }

    // Mints a valid (empty-world) scene file at `absPath` through the real save path.
    static void MintScene(const fs::path& absPath)
    {
        World world;
        Editor::SceneDocumentManager doc;
        doc.SetWorld(&world);
        ASSERT_TRUE(doc.SaveAs(absPath));
        ASSERT_TRUE(fs::exists(absPath));
    }

    static std::string Canon(const fs::path& p)
    {
        std::error_code ec;
        fs::path c = fs::weakly_canonical(p, ec);
        if (ec || c.empty())
            c = p.lexically_normal();
        std::string s = c.generic_string();
#if defined(_WIN32)
        for (auto& ch : s)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
#endif
        return s;
    }

    fs::path m_AssetRoot;
    fs::path m_SceneSubdir; // m_SceneDir relative to the asset root
    fs::path m_SceneDir;
    fs::path m_AwayDir; // CWD parked here: a CWD-relative read must find nothing
};

TEST_F(OpenSceneResolutionTests, RelativeOpenResolvesUnderProjectRootNotCwd)
{
    const fs::path sceneAbs = m_SceneDir / "relative_open.scene";
    MintScene(sceneAbs);

    World world;
    Editor::EditorChangeNotifications notifications;
    UIElement root("TestRoot");
    Editor::SceneEditorController controller;
    controller.AttachToRoot(root, world, notifications, nullptr);

    ScopedUserDataRedirect prefsGuard(m_AwayDir);
    ScopedCurrentPath cwdGuard(m_AwayDir);
    ASSERT_TRUE(cwdGuard.IsActive());

    controller.RequestOpenScene(m_SceneSubdir / "relative_open.scene",
                                /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);

    const auto active = controller.GetActiveScenePath();
    ASSERT_TRUE(active.has_value()) << "project-relative open must load with CWD elsewhere";
    EXPECT_TRUE(active->is_absolute());
    EXPECT_EQ(Canon(*active), Canon(sceneAbs));
}

TEST_F(OpenSceneResolutionTests, AbsolutePathPassesThroughUnchanged)
{
    const fs::path sceneAbs = m_SceneDir / "absolute_open.scene";
    MintScene(sceneAbs);

    World world;
    Editor::EditorChangeNotifications notifications;
    UIElement root("TestRoot");
    Editor::SceneEditorController controller;
    controller.AttachToRoot(root, world, notifications, nullptr);

    ScopedUserDataRedirect prefsGuard(m_AwayDir);
    ScopedCurrentPath cwdGuard(m_AwayDir);
    ASSERT_TRUE(cwdGuard.IsActive());

    controller.RequestOpenScene(sceneAbs, /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);

    const auto active = controller.GetActiveScenePath();
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(Canon(*active), Canon(sceneAbs));
}

TEST_F(OpenSceneResolutionTests, MissingRelativePathFailsCleanlyAnchoredToProjectRoot)
{
    World world;
    Editor::EditorChangeNotifications notifications;
    UIElement root("TestRoot");
    Editor::SceneEditorController controller;
    controller.AttachToRoot(root, world, notifications, nullptr);

    ScopedUserDataRedirect prefsGuard(m_AwayDir);
    ScopedCurrentPath cwdGuard(m_AwayDir);
    ASSERT_TRUE(cwdGuard.IsActive());

    controller.RequestOpenScene(m_SceneSubdir / "does_not_exist.scene",
                                /*additive*/ false,
                                Editor::SceneEditorController::OpenRestorePolicy::Discard);
    controller.Update(nullptr);

    EXPECT_FALSE(controller.GetActiveScenePath().has_value())
        << "a missing scene must not adopt a document path";

    // The failure must report the project-anchored candidate, proving the
    // lookup went through mount resolution rather than the CWD.
    const Scene::SceneIOError& err = Scene::GetLastSceneIOError();
    ASSERT_FALSE(err.file.empty());
    EXPECT_TRUE(err.file.is_absolute());
    const std::string errFile = Canon(err.file);
    const std::string rootPrefix = Canon(m_AssetRoot);
    EXPECT_EQ(errFile.rfind(rootPrefix, 0), 0u)
        << "error path '" << errFile << "' should be anchored under '" << rootPrefix << "'";
}

} // namespace
