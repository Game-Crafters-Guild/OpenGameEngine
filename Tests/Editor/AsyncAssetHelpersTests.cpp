// RunWhenAssetLoaded never waits: an asset that is not loaded is requested, named while it is
// pending, and its continuation runs from the main thread's poll once the load has finished;
// a continuation whose element went away, or whose world was cleared for another scene, is
// dropped. The model inspector's settings reload goes through it for a model that is not loaded.

#include <gtest/gtest.h>

#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Editor/Assets/AsyncAssetHelpers.h"
#include "Inspectors/ModelSettingsReload.h"
#include "UI/UIElement.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace GameEngine;

namespace
{

EngineCore& StartedEngine()
{
    EngineCore& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
    {
        ApplicationConfig config{};
        config.AssetDirectory = ".";
        config.WorkspaceDirectory = ".";
        config.EnableEditor = true;
        EXPECT_TRUE(engine.Initialize(config));
    }
    return engine;
}

// Polls until `done` or a deadline; the load of an unknown GUID finishes at once, empty.
bool PollUntil(const bool& done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done && std::chrono::steady_clock::now() < deadline)
    {
        Editor::PollPendingAssetLoads();
        std::this_thread::yield();
    }
    return done;
}

} // namespace

TEST(AsyncAssetHelpersTests, AContinuationRunsFromThePollOnceTheLoadHasFinished)
{
    EngineCore& engine = StartedEngine();
    ASSERT_TRUE(engine.IsInitialized());
    Editor::ClearPendingAssetLoads();

    bool ran = false;
    Editor::RunWhenAssetLoaded(engine.GetAssetManager(), GUID::Generate(), AssetLoadPriority::High, nullptr,
                               nullptr, [&ran] { ran = true; }, "dropped.glb");
    EXPECT_FALSE(ran) << "the continuation ran before the load was polled";
    ASSERT_EQ(Editor::PendingAssetLoadLabels().size(), 1u);
    EXPECT_EQ(Editor::PendingAssetLoadLabels().front(), "dropped.glb");

    EXPECT_TRUE(PollUntil(ran)) << "the continuation never ran";
    EXPECT_TRUE(Editor::PendingAssetLoadLabels().empty());
}

TEST(AsyncAssetHelpersTests, AContinuationWhoseElementWentAwayIsDropped)
{
    EngineCore& engine = StartedEngine();
    ASSERT_TRUE(engine.IsInitialized());
    Editor::ClearPendingAssetLoads();

    bool ran = false;
    auto target = std::make_unique<UIElement>();
    Editor::RunWhenAssetLoaded(engine.GetAssetManager(), GUID::Generate(), AssetLoadPriority::High, target.get(),
                               nullptr, [&ran] { ran = true; }, std::string());
    EXPECT_TRUE(Editor::PendingAssetLoadLabels().empty()) << "a load with no label was named";
    target.reset();

    bool other = false;
    Editor::RunWhenAssetLoaded(engine.GetAssetManager(), GUID::Generate(), AssetLoadPriority::High, nullptr,
                               nullptr, [&other] { other = true; }, std::string());
    ASSERT_TRUE(PollUntil(other));
    Editor::PollPendingAssetLoads();
    EXPECT_FALSE(ran) << "the continuation of a destroyed element ran";
}

// Opening a scene clears the editor's world in place (the same object, entity ids reused), so
// a drop aimed at the old scene must not land in the new one.
TEST(AsyncAssetHelpersTests, AContinuationWhoseWorldWasClearedIsDropped)
{
    EngineCore& engine = StartedEngine();
    ASSERT_TRUE(engine.IsInitialized());
    Editor::ClearPendingAssetLoads();

    ECS::World world;
    bool ran = false;
    Editor::RunWhenAssetLoaded(engine.GetAssetManager(), GUID::Generate(), AssetLoadPriority::High, nullptr,
                               &world, [&ran] { ran = true; }, "dropped.png");
    world.Clear();

    bool other = false;
    Editor::RunWhenAssetLoaded(engine.GetAssetManager(), GUID::Generate(), AssetLoadPriority::High, nullptr,
                               &world, [&other] { other = true; }, std::string());
    ASSERT_TRUE(PollUntil(other)) << "a continuation registered after the clear did not run";
    Editor::PollPendingAssetLoads();
    EXPECT_FALSE(ran) << "a continuation aimed at the world before it was cleared ran";
}

// A model inspector's settings change on a model that is neither loaded nor loading requests the
// model and returns: the load is named while pending, never waited for on the calling thread.
TEST(AsyncAssetHelpersTests, ASettingsReloadOfAModelThatIsNotLoadedRequestsItInsteadOfWaiting)
{
    EngineCore& engine = StartedEngine();
    ASSERT_TRUE(engine.IsInitialized());
    Editor::ClearPendingAssetLoads();

    Editor::ReloadModelForChangedSettings(engine.GetAssetManager(), GUID::Generate(), "settings_changed.glb");
    const std::vector<std::string> pending = Editor::PendingAssetLoadLabels();
    ASSERT_EQ(pending.size(), 1u) << "the reload loaded the model on the calling thread instead of requesting it";
    EXPECT_EQ(pending.front(), "settings_changed.glb");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!Editor::PendingAssetLoadLabels().empty() && std::chrono::steady_clock::now() < deadline)
    {
        Editor::PollPendingAssetLoads();
        std::this_thread::yield();
    }
    EXPECT_TRUE(Editor::PendingAssetLoadLabels().empty());
}

