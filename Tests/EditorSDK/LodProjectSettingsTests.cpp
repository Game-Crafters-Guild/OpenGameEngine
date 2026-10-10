// SaveLodProjectSettings — the editor-side write of the project's mesh-LOD
// settings into .Editor/ProjectSettings.json.
//
// Engine owns the JSON shape and the read side (Engine/Tests/
// LodProjectSettingsTests.cpp covers those, plus the cook into game.config).
// What is editor-specific, and what this file locks, is the WRITE contract:
//
//   - the keys land NESTED under "rendering". SettingsStore's own key helpers
//     are flat and treat a dot as a literal character, so a dotted top-level
//     key would be written where nothing reads it, and the failure is silent.
//   - unrelated keys survive. The settings page reload-mutate-saves one field at
//     a time, so a whole-object write would eat a sibling rendering.* setting.
//   - the round trip closes against Engine's reader, which is what keeps the
//     page's displayed value equal to what the renderer was given.

#include "Editor/Settings/LodProjectSettingsWriter.h"

#include "Editor/Settings/SettingsStore.h"
#include "Engine/Rendering/LodProjectSettings.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace ed = GameEngine::Editor;
namespace gr = GameEngine::Rendering;

namespace
{

class LodProjectSettingsWriterTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Root = std::filesystem::temp_directory_path() /
                 ("ge_lod_settings_" + std::to_string(::testing::UnitTest::GetInstance()
                                                          ->current_test_info()
                                                          ->line()));
        std::filesystem::remove_all(m_Root);
        std::filesystem::create_directories(m_Root);
    }
    void TearDown() override { std::filesystem::remove_all(m_Root); }

    nlohmann::json ReadStoredJson() const
    {
        ed::SettingsStore store = ed::OpenProjectSettings(m_Root);
        std::string err;
        EXPECT_TRUE(store.Load(&err)) << err;
        return store.Json();
    }

    void WriteStoredJson(const nlohmann::json& rendering) const
    {
        ed::SettingsStore store = ed::OpenProjectSettings(m_Root);
        std::string err;
        (void)store.Load(&err);
        store.Json()["rendering"] = rendering;
        ASSERT_TRUE(store.Save(&err)) << err;
    }

    std::filesystem::path m_Root;
};

TEST_F(LodProjectSettingsWriterTest, SavesNestedUnderRenderingAndReloads)
{
    gr::LodProjectSettings settings;
    settings.SelectionMode = gr::LodSelectionMode::Off;
    settings.ErrorBudgetPx = 12.5f;
    ASSERT_TRUE(ed::SaveLodProjectSettings(m_Root, settings));

    const nlohmann::json root = ReadStoredJson();
    ASSERT_TRUE(root.contains("rendering")) << "the object every rendering.* key lives under";
    const nlohmann::json& rendering = root["rendering"];
    // Presence before value: nlohmann's const operator[] only asserts on a
    // missing key in a debug build, so a blind read would stop covering a
    // dropped key under NDEBUG.
    ASSERT_TRUE(rendering.contains("lodMode"));
    ASSERT_TRUE(rendering.contains("lodErrorBudgetPx"));
    EXPECT_EQ(rendering["lodMode"].get<std::string>(), "off");
    EXPECT_FLOAT_EQ(rendering["lodErrorBudgetPx"].get<float>(), 12.5f);
    EXPECT_FALSE(rendering.contains("lodSkinnedBudgetScale"))
        << "an unset optional must not write its key";
    EXPECT_FALSE(root.contains("rendering.lodMode"))
        << "a flat dotted key is the silent-failure shape: nothing reads it";

    // The reader the editor's startup apply and the cook both use.
    const gr::LodProjectSettings reloaded = gr::LodProjectSettings::Load(m_Root);
    EXPECT_EQ(reloaded.SelectionMode, gr::LodSelectionMode::Off);
    ASSERT_TRUE(reloaded.ErrorBudgetPx.has_value());
    EXPECT_FLOAT_EQ(*reloaded.ErrorBudgetPx, 12.5f);
    EXPECT_FALSE(reloaded.SkinnedBudgetScale.has_value());
}

TEST_F(LodProjectSettingsWriterTest, SavingTheModeKeepsUnrelatedRenderingKeys)
{
    WriteStoredJson({{"taaRenderScale", 0.75f}, {"lodErrorBudgetPx", 42.0f}});

    // Reload-mutate-save, the shape the settings row uses: the sibling key and
    // the hand-edited budget must both survive a mode change.
    gr::LodProjectSettings settings = gr::LodProjectSettings::Load(m_Root);
    ASSERT_TRUE(settings.ErrorBudgetPx.has_value());
    EXPECT_FLOAT_EQ(*settings.ErrorBudgetPx, 42.0f);
    settings.SelectionMode = gr::LodSelectionMode::Coverage;
    ASSERT_TRUE(ed::SaveLodProjectSettings(m_Root, settings));

    const nlohmann::json root = ReadStoredJson();
    ASSERT_TRUE(root.contains("rendering"));
    const nlohmann::json& rendering = root["rendering"];
    ASSERT_TRUE(rendering.contains("taaRenderScale")) << "a sibling rendering.* key was eaten";
    ASSERT_TRUE(rendering.contains("lodErrorBudgetPx"));
    ASSERT_TRUE(rendering.contains("lodMode"));
    EXPECT_FLOAT_EQ(rendering["taaRenderScale"].get<float>(), 0.75f);
    EXPECT_FLOAT_EQ(rendering["lodErrorBudgetPx"].get<float>(), 42.0f);
    EXPECT_EQ(rendering["lodMode"].get<std::string>(), "coverage");
    ASSERT_TRUE(root.contains("schemaVersion"));
    EXPECT_EQ(root["schemaVersion"].get<int>(), 1) << "the store's own convention must survive";
}

TEST_F(LodProjectSettingsWriterTest, NoWorkspaceRootIsAFailedSave)
{
    EXPECT_FALSE(ed::SaveLodProjectSettings({}, gr::LodProjectSettings{}))
        << "no project open must report failure, not write somewhere";
}
// --- Per-view-class budget overrides ----------------------------------------
// The write flavor only: shape, clamping and ApplyTo live with the owner in
// Engine/Tests/LodProjectSettingsTests.cpp.

TEST_F(LodProjectSettingsWriterTest, ViewClassOverridesRoundTripThroughTheWriter)
{
    gr::LodProjectSettings settings;
    settings.GameViewBudget = {true, 200.0f};
    settings.SceneViewBudget = {true, 50.0f};
    ASSERT_TRUE(ed::SaveLodProjectSettings(m_Root, settings));

    const nlohmann::json root = ReadStoredJson();
    ASSERT_TRUE(root.contains("rendering"));
    const nlohmann::json& rendering = root["rendering"];
    ASSERT_TRUE(rendering.contains("lodGameViewBudgetEnabled"));
    ASSERT_TRUE(rendering.contains("lodGameViewBudgetPercent"));
    ASSERT_TRUE(rendering.contains("lodSceneViewBudgetPercent"));
    EXPECT_TRUE(rendering["lodGameViewBudgetEnabled"].get<bool>());
    EXPECT_FLOAT_EQ(rendering["lodGameViewBudgetPercent"].get<float>(), 200.0f);
    EXPECT_FLOAT_EQ(rendering["lodSceneViewBudgetPercent"].get<float>(), 50.0f);
    EXPECT_FALSE(root.contains("rendering.lodGameViewBudgetEnabled"))
        << "a flat dotted key is the silent-failure shape: nothing reads it";

    const gr::LodProjectSettings reloaded = gr::LodProjectSettings::Load(m_Root);
    EXPECT_TRUE(reloaded.GameViewBudget.Enabled);
    EXPECT_FLOAT_EQ(reloaded.GameViewBudget.BudgetPercent, 200.0f);
    EXPECT_TRUE(reloaded.SceneViewBudget.Enabled);
    EXPECT_FLOAT_EQ(reloaded.SceneViewBudget.BudgetPercent, 50.0f);
}

TEST_F(LodProjectSettingsWriterTest, DisablingAnOverrideErasesItsStoredKeys)
{
    gr::LodProjectSettings settings;
    settings.GameViewBudget = {true, 200.0f};
    ASSERT_TRUE(ed::SaveLodProjectSettings(m_Root, settings));

    // Same contract as the unset optionals: turning the override back off must
    // erase the pair from the file, not store "false" forever.
    settings.GameViewBudget = {false, 200.0f};
    ASSERT_TRUE(ed::SaveLodProjectSettings(m_Root, settings));

    const nlohmann::json root = ReadStoredJson();
    ASSERT_TRUE(root.contains("rendering"));
    EXPECT_FALSE(root["rendering"].contains("lodGameViewBudgetEnabled"));
    EXPECT_FALSE(root["rendering"].contains("lodGameViewBudgetPercent"));
}

} // namespace
