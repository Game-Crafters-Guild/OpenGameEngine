// UIScaleProjectSettings — the serialized shape of the project's game UI scale
// policy, and the chain that carries it into a BUILT GAME.
//
// The policy is authored into .Editor/ProjectSettings.json, which never stages
// into a packaged game, so it reaches the Player only by being cooked into
// game.config. ShipChain walks the hops with one object: author -> Load ->
// game.config -> the Player's read.
//
// Fixtures are authored as raw JSON text, never by calling the writer under
// test — a fixture produced by the code being tested cannot disprove it.

#include "Engine/Build/GameConfig.h"
#include "Engine/GameUI/UIScaleProjectSettings.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

using GameEngine::GameConfig;
using GameEngine::UIScaleProjectSettings;
using GameEngine::UI::UIScaleMode;
using GameEngine::UI::UIScaleSettings;

namespace
{

class UIScaleProjectSettingsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Root = std::filesystem::temp_directory_path() /
                 ("ge_ui_scale_ship_" + std::to_string(::testing::UnitTest::GetInstance()
                                                           ->current_test_info()
                                                           ->line()));
        std::filesystem::remove_all(m_Root);
        std::filesystem::create_directories(m_Root);
    }
    void TearDown() override { std::filesystem::remove_all(m_Root); }

    // Authors <root>/.Editor/ProjectSettings.json with a literal body after the
    // schema version, exactly as a hand edit or the settings page would leave it.
    void AuthorProjectSettings(const std::string& body) const
    {
        const std::filesystem::path dir = m_Root / ".Editor";
        std::filesystem::create_directories(dir);
        std::ofstream file(dir / "ProjectSettings.json");
        ASSERT_TRUE(file.is_open());
        file << "{\n  \"schemaVersion\": 1" << body << "\n}\n";
    }

    std::filesystem::path GameConfigPath() const { return m_Root / "game.config"; }

    std::filesystem::path m_Root;
};

TEST_F(UIScaleProjectSettingsTest, AProjectThatNeverChoseScalesByPlatformDpi)
{
    AuthorProjectSettings(R"(, "rendering": { "msaa": "off" })");
    const UIScaleSettings settings = UIScaleProjectSettings::Load(m_Root);
    const UIScaleSettings defaults{};
    EXPECT_EQ(settings.mode, UIScaleMode::Platform);
    EXPECT_FLOAT_EQ(settings.referenceWidth, defaults.referenceWidth);
    EXPECT_FLOAT_EQ(settings.referenceHeight, defaults.referenceHeight);

    EXPECT_EQ(UIScaleProjectSettings::Load(m_Root / "no-such-project").mode, UIScaleMode::Platform);
    EXPECT_EQ(UIScaleProjectSettings::Load({}).mode, UIScaleMode::Platform);
}

TEST_F(UIScaleProjectSettingsTest, AnUnreadableProjectFileScalesByPlatformDpi)
{
    const std::filesystem::path dir = m_Root / ".Editor";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "ProjectSettings.json") << "{ \"uiScale\": { \"mode\": \"fit\" ";
    EXPECT_EQ(UIScaleProjectSettings::Load(m_Root).mode, UIScaleMode::Platform);
}

TEST_F(UIScaleProjectSettingsTest, ReadingClampsReferenceSizesAndRejectsUnknownModes)
{
    const nlohmann::json uiScale = nlohmann::json::parse(
        R"({ "mode": "stretch", "referenceWidth": 0, "referenceHeight": 1000000 })");
    const UIScaleSettings settings = UIScaleProjectSettings::ReadFrom(uiScale);
    EXPECT_EQ(settings.mode, UIScaleMode::Platform) << "an unknown mode token reads as Platform";
    EXPECT_FLOAT_EQ(settings.referenceWidth, UIScaleProjectSettings::kMinReferenceSize);
    EXPECT_FLOAT_EQ(settings.referenceHeight, UIScaleProjectSettings::kMaxReferenceSize);
}

TEST_F(UIScaleProjectSettingsTest, ShipChainCarriesThePolicyFromProjectFileToThePlayersRead)
{
    // Hop 1: authored in the project the way the editor leaves it.
    AuthorProjectSettings(
        R"(, "uiScale": { "mode": "fill", "referenceWidth": 1600, "referenceHeight": 800 })");

    // Hop 2: the cook (BuildPipeline::WriteGameConfig) reads the project file.
    GameConfig cooked;
    cooked.uiScale = UIScaleProjectSettings::Load(m_Root);
    ASSERT_TRUE(GameEngine::SaveGameConfig(GameConfigPath(), cooked));

    // Hop 3: the shipped game.config on disk. Asserted on the FILE too — a
    // writer that dropped a key would still round-trip through a reader that
    // shares the omission. Presence before value: nlohmann's const operator[]
    // only asserts on a missing key in a debug build.
    {
        std::ifstream file(GameConfigPath());
        ASSERT_TRUE(file.is_open());
        const nlohmann::json doc = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
        ASSERT_FALSE(doc.is_discarded()) << "game.config is not readable JSON";
        ASSERT_TRUE(doc.contains("uiScale")) << "game.config carries no UI scale policy";
        const nlohmann::json& uiScale = doc["uiScale"];
        ASSERT_TRUE(uiScale.contains("mode"));
        ASSERT_TRUE(uiScale.contains("referenceWidth"));
        ASSERT_TRUE(uiScale.contains("referenceHeight"));
        EXPECT_EQ(uiScale["mode"].get<std::string>(), "fill");
        EXPECT_FLOAT_EQ(uiScale["referenceWidth"].get<float>(), 1600.0f);
        EXPECT_FLOAT_EQ(uiScale["referenceHeight"].get<float>(), 800.0f);
    }

    // Hop 4: the Player's read (main.cpp LoadGameConfig), which
    // PlayerApplication hands to its game UI host.
    const GameConfig shipped = GameEngine::LoadGameConfig(GameConfigPath());
    EXPECT_EQ(shipped.uiScale.mode, UIScaleMode::Fill);
    EXPECT_FLOAT_EQ(shipped.uiScale.referenceWidth, 1600.0f);
    EXPECT_FLOAT_EQ(shipped.uiScale.referenceHeight, 800.0f);
}

} // namespace
