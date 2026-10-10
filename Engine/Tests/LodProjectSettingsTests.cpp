// LodProjectSettings — the serialized shape of the project's mesh-LOD selection
// settings, and the chain that carries it into a BUILT GAME.
//
// Two traps are locked here.
//
// The first is the ship chain itself: the settings are authored into
// .Editor/ProjectSettings.json, which never stages into a packaged game, so
// they reach the Player only by being cooked into game.config. A knob that is
// readable, applied by the editor, and absent from EITHER transport is silently
// default in every shipped build. ShipChain* walks all four hops with one
// object: author -> Load -> game.config -> Player apply.
//
// The second is apply totality: the renderer's LOD state is per-RenderServices
// and survives a project switch, so ApplyTo has to write EVERY knob. A version
// that skipped absent keys left the outgoing project's budget standing while
// the settings page reported the incoming project's.
//
// Fixtures are authored as raw JSON text, never by calling the writer under
// test — a fixture produced by the code being tested cannot disprove it.

#include "Engine/Build/GameConfig.h"
#include "Engine/Rendering/LodProjectSettings.h"
#include "Engine/Rendering/RenderServices.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

namespace gr = GameEngine::Rendering;
using GameEngine::GameConfig;
using GameEngine::Engine::Renderer::RenderServices;

namespace
{

class LodProjectSettingsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Root = std::filesystem::temp_directory_path() /
                 ("ge_lod_ship_" + std::to_string(::testing::UnitTest::GetInstance()
                                                      ->current_test_info()
                                                      ->line()));
        std::filesystem::remove_all(m_Root);
        std::filesystem::create_directories(m_Root);
    }
    void TearDown() override { std::filesystem::remove_all(m_Root); }

    // Authors <root>/.Editor/ProjectSettings.json with a literal "rendering"
    // body, exactly as a hand edit or the settings page would leave it.
    void AuthorProjectSettings(const std::filesystem::path& root, const std::string& renderingBody) const
    {
        const std::filesystem::path dir = root / ".Editor";
        std::filesystem::create_directories(dir);
        std::ofstream file(dir / "ProjectSettings.json");
        ASSERT_TRUE(file.is_open());
        file << "{\n  \"schemaVersion\": 1,\n  \"rendering\": " << renderingBody << "\n}\n";
    }

    std::filesystem::path GameConfigPath() const { return m_Root / "game.config"; }

    std::filesystem::path m_Root;
};

// --- Shape ------------------------------------------------------------------

TEST_F(LodProjectSettingsTest, DefaultsWhenNothingIsPersisted)
{
    const gr::LodProjectSettings settings = gr::LodProjectSettings::Load(m_Root);
    EXPECT_EQ(settings.SelectionMode, gr::LodSelectionMode::Sse);
    EXPECT_FALSE(settings.ErrorBudgetPx.has_value())
        << "an absent key must stay absent, not materialize as 0";
    EXPECT_FALSE(settings.SkinnedBudgetScale.has_value());
    EXPECT_FLOAT_EQ(settings.EffectiveErrorBudgetPx(), gr::kDefaultLodErrorBudgetPx);
    EXPECT_FLOAT_EQ(settings.EffectiveSkinnedBudgetScale(), gr::kDefaultLodSkinnedBudgetScale);
}

TEST_F(LodProjectSettingsTest, LoadsTheNestedRenderingObjectFromTheProjectFile)
{
    AuthorProjectSettings(m_Root, R"({"lodMode": "coverage", "lodErrorBudgetPx": 12.5,
                                      "lodSkinnedBudgetScale": 0.4})");

    const gr::LodProjectSettings settings = gr::LodProjectSettings::Load(m_Root);
    EXPECT_EQ(settings.SelectionMode, gr::LodSelectionMode::Coverage);
    ASSERT_TRUE(settings.ErrorBudgetPx.has_value());
    EXPECT_FLOAT_EQ(*settings.ErrorBudgetPx, 12.5f);
    ASSERT_TRUE(settings.SkinnedBudgetScale.has_value());
    EXPECT_FLOAT_EQ(*settings.SkinnedBudgetScale, 0.4f);
}

TEST_F(LodProjectSettingsTest, AFlatDottedKeyIsNotRead)
{
    // SettingsStore's key helpers are FLAT (a dot is a literal character), so a
    // dotted top-level key is the silent-failure shape: it round-trips through
    // the file and nothing ever reads it.
    const std::filesystem::path dir = m_Root / ".Editor";
    std::filesystem::create_directories(dir);
    std::ofstream file(dir / "ProjectSettings.json");
    file << R"({"rendering.lodMode": "off", "rendering.lodErrorBudgetPx": 42.0})";
    file.close();

    const gr::LodProjectSettings settings = gr::LodProjectSettings::Load(m_Root);
    EXPECT_EQ(settings.SelectionMode, gr::LodSelectionMode::Sse);
    EXPECT_FALSE(settings.ErrorBudgetPx.has_value());
}

TEST_F(LodProjectSettingsTest, OutOfRangeValuesClampAndUnknownModeKeepsTheDefault)
{
    AuthorProjectSettings(m_Root, R"({"lodMode": "nonsense", "lodErrorBudgetPx": 1000.0,
                                      "lodSkinnedBudgetScale": 0.0,
                                      "lodHysteresisBand": 7.5})");

    const gr::LodProjectSettings settings = gr::LodProjectSettings::Load(m_Root);
    EXPECT_EQ(settings.SelectionMode, gr::LodSelectionMode::Sse)
        << "an unreadable mode must not silently select one";
    ASSERT_TRUE(settings.ErrorBudgetPx.has_value());
    EXPECT_FLOAT_EQ(*settings.ErrorBudgetPx, gr::LodProjectSettings::kMaxErrorBudgetPx);
    ASSERT_TRUE(settings.SkinnedBudgetScale.has_value());
    EXPECT_FLOAT_EQ(*settings.SkinnedBudgetScale, gr::LodProjectSettings::kMinSkinnedBudgetScale);
    ASSERT_TRUE(settings.HysteresisBand.has_value());
    EXPECT_FLOAT_EQ(*settings.HysteresisBand, gr::LodProjectSettings::kMaxHysteresisBand);
}

TEST_F(LodProjectSettingsTest, MalformedSettingsFileLoadsDefaultsInsteadOfThrowing)
{
    // A built game has no editor to repair a truncated settings file with, and
    // the cook runs this same Load — a parse failure must not take the whole
    // build or the renderer's LOD state with it.
    const std::filesystem::path dir = m_Root / ".Editor";
    std::filesystem::create_directories(dir);
    std::ofstream file(dir / "ProjectSettings.json");
    file << R"({"rendering": {"lodMode": "off",)";
    file.close();

    const gr::LodProjectSettings settings = gr::LodProjectSettings::Load(m_Root);
    EXPECT_EQ(settings.SelectionMode, gr::LodSelectionMode::Sse);
    EXPECT_FALSE(settings.ErrorBudgetPx.has_value());
}

TEST_F(LodProjectSettingsTest, NoWorkspaceRootIsADefaultLoad)
{
    const gr::LodProjectSettings settings = gr::LodProjectSettings::Load({});
    EXPECT_EQ(settings.SelectionMode, gr::LodSelectionMode::Sse);
}

TEST_F(LodProjectSettingsTest, WriteToLeavesSiblingRenderingKeysAlone)
{
    nlohmann::json rendering = {{"taaRenderScale", 0.75f}};
    gr::LodProjectSettings settings;
    settings.SelectionMode = gr::LodSelectionMode::Off;
    settings.WriteTo(rendering);

    EXPECT_FLOAT_EQ(rendering["taaRenderScale"].get<float>(), 0.75f);
    EXPECT_EQ(rendering["lodMode"].get<std::string>(), "off");
    EXPECT_FALSE(rendering.contains("lodSkinnedBudgetScale"))
        << "an unset optional must not write its key";
}

// --- The ship chain ---------------------------------------------------------

// Every knob this struct owns, at a value distinguishable from the default in
// both directions. The totality assertions below read from this one table, so a
// knob added to the struct without being added here fails to be covered
// loudly rather than silently.
gr::LodProjectSettings ShippedSettings()
{
    gr::LodProjectSettings settings;
    settings.SelectionMode = gr::LodSelectionMode::Coverage;
    settings.ErrorBudgetPx = 27.5f;
    settings.SkinnedBudgetScale = 0.3f;
    settings.GameViewBudget = {true, 200.0f};
    settings.SceneViewBudget = {true, 50.0f};
    settings.CrossfadeDuration = 0.35f;
    settings.HysteresisBand = 0.25f;
    return settings;
}

void ExpectRendererMatches(const RenderServices& rs, const gr::LodProjectSettings& expected)
{
    EXPECT_EQ(rs.GetMeshGPURegistry().GetLodSelectionMode(), expected.SelectionMode);
    EXPECT_FLOAT_EQ(rs.GetLODErrorBudgetPx(), expected.EffectiveErrorBudgetPx());
    EXPECT_FLOAT_EQ(rs.GetLODSkinnedBudgetScale(), expected.EffectiveSkinnedBudgetScale());
    const auto game = rs.GetLODViewBudgetOverride(gr::ViewPurpose::Game);
    const auto scene = rs.GetLODViewBudgetOverride(gr::ViewPurpose::EditorScene);
    EXPECT_EQ(game.Enabled, expected.GameViewBudget.Enabled);
    EXPECT_FLOAT_EQ(game.BudgetPercent, expected.GameViewBudget.BudgetPercent);
    EXPECT_EQ(scene.Enabled, expected.SceneViewBudget.Enabled);
    EXPECT_FLOAT_EQ(scene.BudgetPercent, expected.SceneViewBudget.BudgetPercent);
    EXPECT_FLOAT_EQ(rs.GetLODCrossfadeDuration(), expected.EffectiveCrossfadeDuration());
    EXPECT_FLOAT_EQ(rs.GetLODHysteresisBand(), expected.EffectiveHysteresisBand());
}

TEST_F(LodProjectSettingsTest, ShipChainCarriesEveryKnobFromProjectFileToShippedRenderer)
{
    // Hop 1: authored in the project the way the editor leaves it.
    AuthorProjectSettings(m_Root, R"({"lodMode": "coverage", "lodErrorBudgetPx": 27.5,
                                      "lodSkinnedBudgetScale": 0.3,
                                      "lodGameViewBudgetEnabled": true,
                                      "lodGameViewBudgetPercent": 200.0,
                                      "lodSceneViewBudgetEnabled": true,
                                      "lodSceneViewBudgetPercent": 50.0,
                                      "lodCrossfadeDuration": 0.35,
                                      "lodHysteresisBand": 0.25})");

    // Hop 2: the cook (BuildPipeline::WriteGameConfig) reads the project file.
    GameConfig cooked;
    cooked.lodSelection = gr::LodProjectSettings::Load(m_Root);
    ASSERT_TRUE(GameEngine::SaveGameConfig(GameConfigPath(), cooked));

    // Hop 3: the shipped game.config, as the Player reads it. Assert on the
    // FILE too — a writer that dropped a key would still round-trip through a
    // reader that shares the omission. Key PRESENCE is asserted before each
    // read: nlohmann's const operator[] only asserts on a missing key in a
    // debug build, so reading blind would make this hop's coverage evaporate
    // under NDEBUG.
    {
        std::ifstream file(GameConfigPath());
        ASSERT_TRUE(file.is_open());
        const nlohmann::json doc = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
        ASSERT_FALSE(doc.is_discarded()) << "game.config is not readable JSON";
        ASSERT_TRUE(doc.contains("rendering")) << "game.config carries no LOD selection at all";
        const nlohmann::json& rendering = doc["rendering"];
        ASSERT_TRUE(rendering.contains("lodMode"));
        ASSERT_TRUE(rendering.contains("lodErrorBudgetPx"));
        ASSERT_TRUE(rendering.contains("lodSkinnedBudgetScale"));
        ASSERT_TRUE(rendering.contains("lodGameViewBudgetEnabled"));
        ASSERT_TRUE(rendering.contains("lodGameViewBudgetPercent"));
        ASSERT_TRUE(rendering.contains("lodSceneViewBudgetPercent"));
        ASSERT_TRUE(rendering.contains("lodCrossfadeDuration"));
        EXPECT_FLOAT_EQ(rendering["lodCrossfadeDuration"].get<float>(), 0.35f);
        ASSERT_TRUE(rendering.contains("lodHysteresisBand"));
        EXPECT_FLOAT_EQ(rendering["lodHysteresisBand"].get<float>(), 0.25f);
        EXPECT_EQ(rendering["lodMode"].get<std::string>(), "coverage");
        EXPECT_FLOAT_EQ(rendering["lodErrorBudgetPx"].get<float>(), 27.5f);
        EXPECT_FLOAT_EQ(rendering["lodSkinnedBudgetScale"].get<float>(), 0.3f);
        EXPECT_TRUE(rendering["lodGameViewBudgetEnabled"].get<bool>());
        EXPECT_FLOAT_EQ(rendering["lodGameViewBudgetPercent"].get<float>(), 200.0f);
        EXPECT_FLOAT_EQ(rendering["lodSceneViewBudgetPercent"].get<float>(), 50.0f);
    }

    const GameConfig shipped = GameEngine::LoadGameConfig(GameConfigPath());

    // Hop 4: PlayerApplication::InitRendering.
    RenderServices rs;
    shipped.lodSelection.ApplyTo(rs);
    ExpectRendererMatches(rs, ShippedSettings());
}

TEST_F(LodProjectSettingsTest, AGameConfigWithNoRenderingObjectShipsTheEngineDefaults)
{
    // Older game.config files, and projects that never opened the page. The
    // Player must land on the constructed defaults, not on zeros.
    GameConfig cooked;
    ASSERT_TRUE(GameEngine::SaveGameConfig(GameConfigPath(), cooked));

    const GameConfig shipped = GameEngine::LoadGameConfig(GameConfigPath());
    EXPECT_EQ(shipped.lodSelection.SelectionMode, gr::LodSelectionMode::Sse);
    EXPECT_FALSE(shipped.lodSelection.ErrorBudgetPx.has_value());

    RenderServices rs;
    shipped.lodSelection.ApplyTo(rs);
    EXPECT_FLOAT_EQ(rs.GetLODErrorBudgetPx(), gr::kDefaultLodErrorBudgetPx);
    EXPECT_FLOAT_EQ(rs.GetLODSkinnedBudgetScale(), gr::kDefaultLodSkinnedBudgetScale);
    EXPECT_FLOAT_EQ(rs.GetLODCrossfadeDuration(),
                    gr::LodProjectSettings::kDefaultCrossfadeDuration)
        << "a game.config with no rendering object must still ship the crossfade default";
}

TEST_F(LodProjectSettingsTest, ShippedApplyIsTotalOverAPreLoadedRenderer)
{
    // The Player's apply runs against a fresh RenderServices, so a partial
    // apply hides there. Pre-load the renderer with the opposite of everything
    // the shipped config says: any knob the apply skips keeps the wrong value.
    RenderServices rs;
    rs.SetLODErrorBudgetPx(1.0f);
    rs.SetLODSkinnedBudgetScale(gr::LodProjectSettings::kMaxSkinnedBudgetScale);
    rs.GetMeshGPURegistry().SetLodSelectionMode(gr::LodSelectionMode::Off);
    rs.SetLODViewBudgetOverride(gr::ViewPurpose::Game, {true, 33.0f});
    rs.SetLODViewBudgetOverride(gr::ViewPurpose::EditorScene, {true, 333.0f});

    GameConfig cooked;
    cooked.lodSelection = ShippedSettings();
    ASSERT_TRUE(GameEngine::SaveGameConfig(GameConfigPath(), cooked));
    GameEngine::LoadGameConfig(GameConfigPath()).lodSelection.ApplyTo(rs);

    ExpectRendererMatches(rs, ShippedSettings());
}

TEST_F(LodProjectSettingsTest, ShippedApplyOfAnAbsentKeyOverwritesAStandingValue)
{
    // The same totality contract for the ABSENT direction: a config that stores
    // no budget must push the engine default, not leave whatever was standing.
    RenderServices rs;
    rs.SetLODErrorBudgetPx(30.0f);
    rs.SetLODSkinnedBudgetScale(0.1f);
    rs.SetLODViewBudgetOverride(gr::ViewPurpose::Game, {true, 200.0f});
    rs.SetLODCrossfadeDuration(1.5f);
    rs.SetLODHysteresisBand(0.4f);

    GameConfig cooked;
    cooked.lodSelection.SelectionMode = gr::LodSelectionMode::Off;
    ASSERT_TRUE(GameEngine::SaveGameConfig(GameConfigPath(), cooked));
    GameEngine::LoadGameConfig(GameConfigPath()).lodSelection.ApplyTo(rs);

    EXPECT_EQ(rs.GetMeshGPURegistry().GetLodSelectionMode(), gr::LodSelectionMode::Off);
    EXPECT_FLOAT_EQ(rs.GetLODErrorBudgetPx(), gr::kDefaultLodErrorBudgetPx);
    EXPECT_FLOAT_EQ(rs.GetLODSkinnedBudgetScale(), gr::kDefaultLodSkinnedBudgetScale);
    EXPECT_FALSE(rs.GetLODViewBudgetOverride(gr::ViewPurpose::Game).Enabled)
        << "a standing override must not survive a config that stores none";
    EXPECT_FLOAT_EQ(rs.GetLODCrossfadeDuration(),
                    gr::LodProjectSettings::kDefaultCrossfadeDuration)
        << "a standing crossfade must not survive a config that stores none";
    EXPECT_FLOAT_EQ(rs.GetLODHysteresisBand(), gr::LodProjectSettings::kDefaultHysteresisBand)
        << "a standing hysteresis band must not survive a config that stores none";
}

// The shipped crossfade default is ON at 0.25 s. Pinned as a literal so a
// revert to off reds here rather than silently changing what every project
// renders, and pinned in all three places the value can be read: the constant,
// a settings object with no key, and a freshly constructed renderer. If the
// renderer's own initializer ever drifts from the constant, applying an absent
// key to a fresh renderer would change its duration.
TEST(RenderServicesLodKnobs, CrossfadeShipsOnByDefault)
{
    EXPECT_FLOAT_EQ(gr::LodProjectSettings::kDefaultCrossfadeDuration, 0.25f);
    EXPECT_FLOAT_EQ(gr::LodProjectSettings{}.EffectiveCrossfadeDuration(),
                    gr::LodProjectSettings::kDefaultCrossfadeDuration);
    RenderServices rs;
    EXPECT_FLOAT_EQ(rs.GetLODCrossfadeDuration(),
                    gr::LodProjectSettings::kDefaultCrossfadeDuration)
        << "a fresh renderer must construct at the settings default";
}

// The runtime setter is the set_lod IPC path, which bypasses the settings
// loader's ReadClampedNumber — it must enforce the same [0, 1] domain itself,
// and map NaN to off (the crossfade-setter convention).
TEST(RenderServicesLodKnobs, HysteresisBandSetterClampsToTheSettingsDomain)
{
    RenderServices rs;
    rs.SetLODHysteresisBand(0.4f);
    EXPECT_FLOAT_EQ(rs.GetLODHysteresisBand(), 0.4f);
    rs.SetLODHysteresisBand(1.7f);
    EXPECT_FLOAT_EQ(rs.GetLODHysteresisBand(), gr::LodProjectSettings::kMaxHysteresisBand);
    rs.SetLODHysteresisBand(-0.5f);
    EXPECT_FLOAT_EQ(rs.GetLODHysteresisBand(), 0.0f);
    rs.SetLODHysteresisBand(std::numeric_limits<float>::quiet_NaN());
    EXPECT_FLOAT_EQ(rs.GetLODHysteresisBand(), 0.0f);
}

// --- The editor's project switch, unchanged by the move ---------------------

TEST_F(LodProjectSettingsTest, SwitchingToAProjectWithNoLodKeysDropsThePreviousProjectsValues)
{
    const std::filesystem::path projectA = m_Root / "A";
    const std::filesystem::path projectB = m_Root / "B";
    std::filesystem::create_directories(projectA);
    std::filesystem::create_directories(projectB);
    AuthorProjectSettings(projectA, R"({"lodMode": "off", "lodErrorBudgetPx": 30.0,
                                        "lodSkinnedBudgetScale": 0.1})");
    AuthorProjectSettings(projectB, R"({})");

    RenderServices rs;
    gr::LodProjectSettings::Load(projectA).ApplyTo(rs);
    ASSERT_EQ(rs.GetMeshGPURegistry().GetLodSelectionMode(), gr::LodSelectionMode::Off);
    ASSERT_FLOAT_EQ(rs.GetLODErrorBudgetPx(), 30.0f);
    ASSERT_FLOAT_EQ(rs.GetLODSkinnedBudgetScale(), 0.1f);

    // The switch EditorApplication::SetProjectFolder performs. Project B never
    // opened the page, so its settings page shows the engine defaults -- the
    // renderer must show them too. Skipping absent keys here is the defect:
    // the mode would follow B while the budget stayed on A's 30px.
    gr::LodProjectSettings::Load(projectB).ApplyTo(rs);
    EXPECT_EQ(rs.GetMeshGPURegistry().GetLodSelectionMode(), gr::LodSelectionMode::Sse);
    EXPECT_FLOAT_EQ(rs.GetLODErrorBudgetPx(), gr::kDefaultLodErrorBudgetPx);
    EXPECT_FLOAT_EQ(rs.GetLODSkinnedBudgetScale(), gr::kDefaultLodSkinnedBudgetScale);
}

TEST_F(LodProjectSettingsTest, SwitchingBetweenTwoConfiguredProjectsTakesTheIncomingValues)
{
    const std::filesystem::path projectA = m_Root / "A";
    const std::filesystem::path projectB = m_Root / "B";
    std::filesystem::create_directories(projectA);
    std::filesystem::create_directories(projectB);
    AuthorProjectSettings(projectA, R"({"lodMode": "coverage", "lodErrorBudgetPx": 30.0})");
    AuthorProjectSettings(projectB, R"({"lodMode": "off", "lodSkinnedBudgetScale": 0.5})");

    RenderServices rs;
    gr::LodProjectSettings::Load(projectA).ApplyTo(rs);
    gr::LodProjectSettings::Load(projectB).ApplyTo(rs);

    EXPECT_EQ(rs.GetMeshGPURegistry().GetLodSelectionMode(), gr::LodSelectionMode::Off);
    EXPECT_FLOAT_EQ(rs.GetLODSkinnedBudgetScale(), 0.5f);
    EXPECT_FLOAT_EQ(rs.GetLODErrorBudgetPx(), gr::kDefaultLodErrorBudgetPx)
        << "B leaves the budget key absent, so A's 30px must not survive the switch";
}


// --- Per-view-class budget overrides ----------------------------------------

TEST_F(LodProjectSettingsTest, ViewClassOverridesDefaultToDisabledAtFullBudget)
{
    const gr::LodProjectSettings settings = gr::LodProjectSettings::Load(m_Root);
    EXPECT_FALSE(settings.GameViewBudget.Enabled);
    EXPECT_FALSE(settings.SceneViewBudget.Enabled);
    EXPECT_FLOAT_EQ(settings.GameViewBudget.BudgetPercent, gr::kDefaultLodBudgetPercent);
    EXPECT_FLOAT_EQ(settings.SceneViewBudget.BudgetPercent, gr::kDefaultLodBudgetPercent);
}

TEST_F(LodProjectSettingsTest, ViewClassOverridesRoundTripThroughWriteToAndReadFrom)
{
    nlohmann::json rendering = nlohmann::json::object();
    gr::LodProjectSettings settings;
    settings.GameViewBudget = {true, 200.0f};
    settings.SceneViewBudget = {true, 50.0f};
    settings.WriteTo(rendering);

    ASSERT_TRUE(rendering.contains("lodGameViewBudgetEnabled"));
    ASSERT_TRUE(rendering.contains("lodGameViewBudgetPercent"));
    ASSERT_TRUE(rendering.contains("lodSceneViewBudgetPercent"));
    EXPECT_TRUE(rendering["lodGameViewBudgetEnabled"].get<bool>());
    EXPECT_FLOAT_EQ(rendering["lodGameViewBudgetPercent"].get<float>(), 200.0f);
    EXPECT_FLOAT_EQ(rendering["lodSceneViewBudgetPercent"].get<float>(), 50.0f);

    const gr::LodProjectSettings reloaded = gr::LodProjectSettings::ReadFrom(rendering);
    EXPECT_TRUE(reloaded.GameViewBudget.Enabled);
    EXPECT_FLOAT_EQ(reloaded.GameViewBudget.BudgetPercent, 200.0f);
    EXPECT_TRUE(reloaded.SceneViewBudget.Enabled);
    EXPECT_FLOAT_EQ(reloaded.SceneViewBudget.BudgetPercent, 50.0f);
}

TEST_F(LodProjectSettingsTest, ADisabledOverrideErasesItsKeysFromTheRenderingObject)
{
    // The object already carries the keys, as a file saved while enabled would.
    nlohmann::json rendering = {{"lodGameViewBudgetEnabled", true},
                                {"lodGameViewBudgetPercent", 200.0f}};
    gr::LodProjectSettings settings;
    settings.GameViewBudget = {false, 200.0f};
    settings.WriteTo(rendering);

    // Same contract as the unset optionals: disabling ERASES the pair rather
    // than storing "false", so the file never accumulates dead settings.
    EXPECT_FALSE(rendering.contains("lodGameViewBudgetEnabled"));
    EXPECT_FALSE(rendering.contains("lodGameViewBudgetPercent"));
}

TEST_F(LodProjectSettingsTest, OutOfRangePercentClampsOnRead)
{
    const nlohmann::json rendering = {{"lodGameViewBudgetEnabled", true},
                                      {"lodGameViewBudgetPercent", 100000.0f},
                                      {"lodSceneViewBudgetEnabled", true},
                                      {"lodSceneViewBudgetPercent", 0.001f}};
    const gr::LodProjectSettings settings = gr::LodProjectSettings::ReadFrom(rendering);
    EXPECT_FLOAT_EQ(settings.GameViewBudget.BudgetPercent,
                    gr::LodProjectSettings::kMaxBudgetPercent);
    EXPECT_FLOAT_EQ(settings.SceneViewBudget.BudgetPercent,
                    gr::LodProjectSettings::kMinBudgetPercent);
}

TEST_F(LodProjectSettingsTest, ApplyToPushesEachClassToItsOwnViewPurpose)
{
    gr::LodProjectSettings settings;
    settings.GameViewBudget = {true, 200.0f};
    settings.SceneViewBudget = {true, 50.0f};

    RenderServices rs;
    settings.ApplyTo(rs);

    // MUTATION THAT MUST FAIL THIS: push both classes to one purpose, and the
    // two expectations below cannot both hold.
    const auto game = rs.GetLODViewBudgetOverride(gr::ViewPurpose::Game);
    const auto scene = rs.GetLODViewBudgetOverride(gr::ViewPurpose::EditorScene);
    EXPECT_TRUE(game.Enabled);
    EXPECT_FLOAT_EQ(game.BudgetPercent, 200.0f);
    EXPECT_TRUE(scene.Enabled);
    EXPECT_FLOAT_EQ(scene.BudgetPercent, 50.0f);
}

TEST_F(LodProjectSettingsTest, SwitchingToAProjectWithNoOverridesClearsThePreviousOnes)
{
    const std::filesystem::path projectA = m_Root / "A";
    const std::filesystem::path projectB = m_Root / "B";
    AuthorProjectSettings(projectA,
                          "{ \"lodGameViewBudgetEnabled\": true, \"lodGameViewBudgetPercent\": 400.0 }");
    AuthorProjectSettings(projectB, "{ }");

    RenderServices rs;
    gr::LodProjectSettings::Load(projectA).ApplyTo(rs);
    ASSERT_TRUE(rs.GetLODViewBudgetOverride(gr::ViewPurpose::Game).Enabled);

    // ApplyTo must stay TOTAL: project B never enabled an override, so A's
    // must not stay standing behind B's settings page reporting "off".
    gr::LodProjectSettings::Load(projectB).ApplyTo(rs);
    EXPECT_FALSE(rs.GetLODViewBudgetOverride(gr::ViewPurpose::Game).Enabled);
}

} // namespace
