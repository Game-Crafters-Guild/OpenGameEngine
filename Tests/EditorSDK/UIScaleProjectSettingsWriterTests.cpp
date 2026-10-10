// SaveUIScaleProjectSettings — the editor-side write of the project's game UI
// scale policy into .Editor/ProjectSettings.json.
//
// Engine owns the JSON shape and the read side (Engine/Tests/
// UIScaleProjectSettingsTests.cpp covers those, plus the cook into
// game.config). What this file locks is the editor's half: the keys land under
// "uiScale", unrelated keys survive, the round trip closes against Engine's
// reader, and a save announces itself so a previewing game UI host re-reads.

#include "Editor/Settings/UIScaleProjectSettingsWriter.h"

#include "Editor/Settings/SettingsStore.h"
#include "Engine/GameUI/UIScaleProjectSettings.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace ed = GameEngine::Editor;
using GameEngine::UIScaleProjectSettings;
using GameEngine::UI::UIScaleMode;
using GameEngine::UI::UIScaleSettings;

namespace
{

class UIScaleProjectSettingsWriterTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Root = std::filesystem::temp_directory_path() /
                 ("ge_ui_scale_settings_" + std::to_string(::testing::UnitTest::GetInstance()
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

    std::filesystem::path m_Root;
};

TEST_F(UIScaleProjectSettingsWriterTest, SavesUnderUIScaleKeepsSiblingsAndReloads)
{
    {
        ed::SettingsStore store = ed::OpenProjectSettings(m_Root);
        std::string err;
        (void)store.Load(&err);
        store.Json()["rendering"] = {{"msaa", "4"}};
        ASSERT_TRUE(store.Save(&err)) << err;
    }

    ASSERT_TRUE(ed::SaveUIScaleProjectSettings(m_Root, UIScaleSettings{UIScaleMode::Height, 1280.0f, 720.0f}));

    const nlohmann::json root = ReadStoredJson();
    ASSERT_TRUE(root.contains("uiScale"));
    const nlohmann::json& uiScale = root["uiScale"];
    ASSERT_TRUE(uiScale.contains("mode"));
    EXPECT_EQ(uiScale["mode"].get<std::string>(), "height");
    ASSERT_TRUE(root.contains("rendering")) << "a save must not eat a sibling object";
    ASSERT_TRUE(root["rendering"].contains("msaa"));
    EXPECT_EQ(root["rendering"]["msaa"].get<std::string>(), "4");

    const UIScaleSettings reloaded = UIScaleProjectSettings::Load(m_Root);
    EXPECT_EQ(reloaded.mode, UIScaleMode::Height);
    EXPECT_FLOAT_EQ(reloaded.referenceWidth, 1280.0f);
    EXPECT_FLOAT_EQ(reloaded.referenceHeight, 720.0f);
}

TEST_F(UIScaleProjectSettingsWriterTest, ASaveAnnouncesItselfAndAFailedSaveDoesNot)
{
    int saves = 0;
    const GameEngine::EventSubscription subscription =
        ed::UIScaleProjectSettingsSaved().Subscribe([&saves]() { ++saves; });

    ASSERT_TRUE(ed::SaveUIScaleProjectSettings(m_Root, UIScaleSettings{UIScaleMode::Fit, 1600.0f, 800.0f}));
    EXPECT_EQ(saves, 1) << "a previewing host re-reads the policy on this event";

    EXPECT_FALSE(ed::SaveUIScaleProjectSettings({}, UIScaleSettings{}));
    EXPECT_EQ(saves, 1) << "nothing was written, so nothing is announced";
}

} // namespace
