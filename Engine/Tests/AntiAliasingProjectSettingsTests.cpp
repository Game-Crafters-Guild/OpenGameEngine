// AntiAliasingProjectSettings — the serialized shape of the project's
// anti-aliasing settings, and the chain that carries it into a BUILT GAME.
//
// Three traps are locked here.
//
// The first is the sample count's resolution. The count used to carry a
// sentinel: 0 meant "auto", and every reader that saw one asked the device for
// its MAXIMUM. That put an 8x-capable machine on 8x MSAA for a project that had
// asked for nothing, and it could be cooked into a shipped game verbatim
// ("msaa": "auto"). The count is now always concrete, resolved in exactly one
// place — ReadFrom — to kDefaultMsaaSampleCount, which
// SetDefaultMSAASampleCount then halves down to the device's cap.
//
// The second is the legacy migration's blast radius: a sample count above 1 and
// no aaMode key means a pre-aaMode project that chose MSAA. Now that an absent
// count parses as 4 rather than 0, that rule has to read the count the FILE
// named, or every keyless project would silently be handed MSAA.
//
// The third is the ship chain: the settings are authored into
// .Editor/ProjectSettings.json, which never stages into a packaged game, so
// they reach the Player only by being cooked into game.config — and the Player
// applies that with no editor in front of it to resolve an unchosen mode. That
// resolve therefore lives in ApplyAntiAliasingTo, and ShipChain* walks all four
// hops: author -> Load -> game.config -> Player apply.
//
// Fixtures are authored as raw JSON text, never by calling the writer under
// test — a fixture produced by the code being tested cannot disprove it.

#include "Engine/Build/GameConfig.h"
#include "Engine/Rendering/AntiAliasing.h"
#include "Engine/Rendering/AntiAliasingProjectSettings.h"
#include "Engine/Rendering/RenderServices.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

namespace er = GameEngine::Engine::Renderer;
namespace gr = GameEngine::Rendering;
using GameEngine::GameConfig;
using er::AntiAliasingMode;
using er::RenderServices;

namespace
{

// Sets an environment variable for one test and restores it after, so an
// override under test cannot leak into the suite's other rows.
class ScopedEnvVar
{
  public:
    ScopedEnvVar(const char* name, const char* value) : m_Name(name)
    {
        if (const char* existing = std::getenv(name))
        {
            m_Had = true;
            m_Previous = existing;
        }
        Set(value);
    }
    ~ScopedEnvVar() { Set(m_Had ? m_Previous.c_str() : nullptr); }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

  private:
    void Set(const char* value) const
    {
#if defined(_WIN32)
        _putenv_s(m_Name, value ? value : "");
#else
        if (value)
            setenv(m_Name, value, 1);
        else
            unsetenv(m_Name);
#endif
    }

    const char* m_Name;
    bool m_Had = false;
    std::string m_Previous;
};

// The "rendering" object as JSON, without going through the writer under test.
nlohmann::json ParseRendering(const std::string& body)
{
    const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    EXPECT_FALSE(parsed.is_discarded()) << "fixture is not readable JSON: " << body;
    return parsed;
}

gr::AntiAliasingProjectSettings ReadRendering(const std::string& body)
{
    return gr::AntiAliasingProjectSettings::ReadFrom(ParseRendering(body));
}

class AntiAliasingProjectSettingsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Root = std::filesystem::temp_directory_path() /
                 ("ge_aa_ship_" + std::to_string(::testing::UnitTest::GetInstance()
                                                     ->current_test_info()
                                                     ->line()));
        std::filesystem::remove_all(m_Root);
        std::filesystem::create_directories(m_Root);
    }
    void TearDown() override { std::filesystem::remove_all(m_Root); }

    // The appliers honour these, so a developer shell that has one set would
    // otherwise red half this file. Rows that want an override construct their
    // own ScopedEnvVar inside the test, nested within these.
    ScopedEnvVar m_NoAaModeOverride{"GE_AA_MODE", nullptr};
    ScopedEnvVar m_NoRenderScaleOverride{"GE_TAA_RENDER_SCALE", nullptr};

    // Authors <root>/.Editor/ProjectSettings.json with a literal "rendering"
    // body, exactly as a hand edit or the settings page would leave it.
    void AuthorProjectSettings(const std::string& renderingBody) const
    {
        const std::filesystem::path dir = m_Root / ".Editor";
        std::filesystem::create_directories(dir);
        std::ofstream file(dir / "ProjectSettings.json");
        ASSERT_TRUE(file.is_open());
        file << "{\n  \"schemaVersion\": 1,\n  \"rendering\": " << renderingBody << "\n}\n";
    }

    std::filesystem::path GameConfigPath() const { return m_Root / "game.config"; }

    std::filesystem::path m_Root;
};

// --- The sample count is always concrete ------------------------------------

TEST_F(AntiAliasingProjectSettingsTest, NothingPersistedLeavesTheModeUnchosenAndTheCountConcrete)
{
    const gr::AntiAliasingProjectSettings settings = gr::AntiAliasingProjectSettings::Load(m_Root);
    EXPECT_FALSE(settings.ModeChosen) << "an absent file is 'never chose', not 'chose Off'";
    EXPECT_EQ(settings.MsaaSamples, er::kDefaultMsaaSampleCount)
        << "the count must never be a sentinel a later reader has to interpret";
}

TEST_F(AntiAliasingProjectSettingsTest, ARetiredAutoCountParsesAsTheDefaultCountNotADeviceMaximum)
{
    // The one that used to reach the device and come back with its maximum.
    // A project asking for MSAA without naming a count gets the product
    // default; the device clamp caps it per machine from there.
    const gr::AntiAliasingProjectSettings settings =
        ReadRendering(R"({"aaMode": "msaa", "msaa": "auto"})");

    EXPECT_TRUE(settings.ModeChosen);
    EXPECT_EQ(settings.AAMode, AntiAliasingMode::MSAA);
    EXPECT_EQ(settings.MsaaSamples, er::kDefaultMsaaSampleCount);
}

TEST_F(AntiAliasingProjectSettingsTest, UnreadableAndAbsentCountsTakeTheDefaultCount)
{
    for (const char* body : {R"({"aaMode": "msaa"})",
                             R"({"aaMode": "msaa", "msaa": "nonsense"})",
                             R"({"aaMode": "msaa", "msaa": 3})",
                             R"({"aaMode": "msaa", "msaa": 0})",
                             R"({"aaMode": "msaa", "msaa": true})"})
    {
        const gr::AntiAliasingProjectSettings settings = ReadRendering(body);
        EXPECT_EQ(settings.MsaaSamples, er::kDefaultMsaaSampleCount) << body;
    }
}

TEST_F(AntiAliasingProjectSettingsTest, EveryCountTheVocabularyNamesSurvivesTheRead)
{
    const std::pair<const char*, GameEngine::uint32> cases[] = {
        {R"({"msaa": "off"})", 1u}, {R"({"msaa": "1"})", 1u},  {R"({"msaa": "1x"})", 1u},
        {R"({"msaa": "2"})", 2u},   {R"({"msaa": "2x"})", 2u}, {R"({"msaa": 4})", 4u},
        {R"({"msaa": "8x"})", 8u},
    };
    for (const auto& [body, expected] : cases)
        EXPECT_EQ(ReadRendering(body).MsaaSamples, expected) << body;
}

// --- The legacy migration reads the FILE's count, not the filled-in one -----

TEST_F(AntiAliasingProjectSettingsTest, AProjectNamingNothingAtAllIsNotMigratedToMsaa)
{
    // The trap: the default count is above 1, and the pre-aaMode migration
    // rule is "a count above 1 with no mode key means MSAA". Reading the
    // filled-in default as the file's own answer would hand MSAA to every
    // project that never chose, bypassing the capability ladder entirely.
    for (const char* body : {R"({})", R"({"taaRenderScale": 1.0})",
                             R"({"msaa": "auto"})", R"({"msaa": "nonsense"})"})
    {
        const gr::AntiAliasingProjectSettings settings = ReadRendering(body);
        EXPECT_FALSE(settings.ModeChosen) << body;
    }
}

TEST_F(AntiAliasingProjectSettingsTest, ALegacySampleCountWithNoModeKeyStillMeansMsaa)
{
    const gr::AntiAliasingProjectSettings settings = ReadRendering(R"({"msaa": "8"})");
    EXPECT_TRUE(settings.ModeChosen);
    EXPECT_EQ(settings.AAMode, AntiAliasingMode::MSAA);
    EXPECT_EQ(settings.MsaaSamples, 8u);
}

TEST_F(AntiAliasingProjectSettingsTest, AStoredOffCountIsNotAModeChoice)
{
    // Only a count ABOVE 1 is a legacy MSAA choice; 1 is "no multisampling",
    // which says nothing about which mode the project wants.
    const gr::AntiAliasingProjectSettings settings = ReadRendering(R"({"msaa": "off"})");
    EXPECT_FALSE(settings.ModeChosen);
    EXPECT_EQ(settings.MsaaSamples, 1u);
}

// --- The writer emits no token a reader has to resolve ----------------------

TEST_F(AntiAliasingProjectSettingsTest, WriteToNeverEmitsAuto)
{
    // A chosen mode writes a concrete pair; the count is a plain integer that
    // reads back as itself, never a token a later reader has to resolve.
    for (const char* body : {R"({"aaMode": "msaa", "msaa": "auto"})",
                             R"({"aaMode": "taa"})",
                             R"({"aaMode": "msaa", "msaa": "8"})",
                             R"({"msaa": "4"})"})
    {
        nlohmann::json rendering = nlohmann::json::object();
        const gr::AntiAliasingProjectSettings settings = ReadRendering(body);
        ASSERT_TRUE(settings.ModeChosen) << body;
        settings.WriteTo(rendering);

        ASSERT_TRUE(rendering.contains("aaMode")) << body;
        EXPECT_NE(rendering["aaMode"].get<std::string>(), "auto") << body;

        ASSERT_TRUE(rendering.contains("msaa")) << body;
        const std::string written = rendering["msaa"].get<std::string>();
        EXPECT_NE(written, "auto") << body;
        const GameEngine::uint32 count = static_cast<GameEngine::uint32>(std::stoul(written));
        EXPECT_EQ(written, std::to_string(count))
            << body << ": the written count must be a plain integer";
        EXPECT_EQ(ReadRendering(R"({"aaMode": "msaa", "msaa": ")" + written + R"("})").MsaaSamples,
                  count)
            << body << ": what was written must read back as the same count";
    }
}

TEST_F(AntiAliasingProjectSettingsTest, AnUnchosenSnapshotWritesNeitherKeyAndClearsARetiredToken)
{
    // ABSENT is how "never chose" is spelled on disk. A writer that emitted
    // ToAAModeToken's "off" here would convert it into "chose no
    // anti-aliasing" — which is exactly what the cook did to a project that had
    // not been materialized, shipping a game with AA off on hardware that can
    // multisample.
    for (const char* body : {R"({})", R"({"msaa": "auto"})",
                             R"({"aaMode": "auto", "msaa": "auto"})",
                             R"({"aaMode": "auto"})"})
    {
        // Write over the fixture itself, so a retired token already in the
        // object has to be cleared rather than merely not re-emitted.
        nlohmann::json rendering = ParseRendering(body);
        const gr::AntiAliasingProjectSettings settings = gr::AntiAliasingProjectSettings::ReadFrom(rendering);
        ASSERT_FALSE(settings.ModeChosen) << body;
        settings.WriteTo(rendering);

        EXPECT_FALSE(rendering.contains("aaMode")) << body;
        EXPECT_FALSE(rendering.contains("msaa")) << body;
        EXPECT_FALSE(gr::AntiAliasingProjectSettings::ReadFrom(rendering).ModeChosen)
            << body << ": the write turned 'never chose' into a choice";
    }
}

TEST_F(AntiAliasingProjectSettingsTest, TheWriterIsTheReadersInverseForEveryStoredShape)
{
    // The round-trip is what makes a whole-snapshot save safe: the settings
    // page, set_aa_mode and the cook all read-modify-write, so any state the
    // pair cannot round-trip is a setting that changes itself when an unrelated
    // one is saved.
    for (const char* body : {R"({})", R"({"msaa": "off"})", R"({"msaa": "8"})",
                             R"({"aaMode": "off"})", R"({"aaMode": "taa", "msaa": "2"})",
                             R"({"aaMode": "msaa", "msaa": "auto"})"})
    {
        const gr::AntiAliasingProjectSettings first = ReadRendering(body);
        nlohmann::json rendering = nlohmann::json::object();
        first.WriteTo(rendering);
        const gr::AntiAliasingProjectSettings second =
            gr::AntiAliasingProjectSettings::ReadFrom(rendering);

        EXPECT_EQ(second.ModeChosen, first.ModeChosen) << body;
        EXPECT_EQ(second.AAMode, first.AAMode) << body;
        // The count is a stored choice only under a stored mode. An unchosen
        // project's count is inert — the ladder supplies mode and count
        // together — so it is written away with the mode rather than left
        // behind for the legacy "a count above 1 means MSAA" rule to read as a
        // choice on the next open.
        if (first.ModeChosen)
            EXPECT_EQ(second.MsaaSamples, first.MsaaSamples) << body;
        else
            EXPECT_EQ(second.MsaaSamples, er::kDefaultMsaaSampleCount) << body;
    }
}

// --- Apply: the one place an unchosen mode becomes a real one ---------------

TEST_F(AntiAliasingProjectSettingsTest, AnUnchosenModeTakesTheCapabilityLadderOnApply)
{
    // No editor in front of this call — this is the shape the Player uses.
    // A bare RenderServices has no device, so the ladder reports its no-device
    // rung; the rungs above it are pinned by ResolveDefaultAntiAliasingTest
    // (Tests/EditorSDK/RenderProjectSettingsTests.cpp), which can drive every
    // capability without owning the hardware that reports it.
    RenderServices rs;
    gr::AntiAliasingProjectSettings settings{};
    ASSERT_FALSE(settings.ModeChosen);
    settings.ApplyAntiAliasingTo(rs);

    const er::ResolvedAntiAliasing expected = er::ResolveDefaultAntiAliasing({});
    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), expected.Mode);
    EXPECT_EQ(rs.GetDefaultMSAASampleCount(), expected.SampleCount);
}

TEST_F(AntiAliasingProjectSettingsTest, AChosenModeIsAppliedVerbatimAndTheLadderStaysOut)
{
    RenderServices rs;
    ReadRendering(R"({"aaMode": "msaa", "msaa": "8"})").ApplyAntiAliasingTo(rs);

    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), AntiAliasingMode::MSAA);
    EXPECT_EQ(rs.GetDefaultMSAASampleCount(), 8u);
}

TEST_F(AntiAliasingProjectSettingsTest, AnExplicitOffSurvivesTheApply)
{
    // "Off" is a choice; only an ABSENT mode routes into the ladder. A ladder
    // that overrode this would be the engine arguing with the user.
    RenderServices rs;
    ReadRendering(R"({"aaMode": "off"})").ApplyAntiAliasingTo(rs);

    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), AntiAliasingMode::Off);
}

TEST_F(AntiAliasingProjectSettingsTest, TheParserReportsWhetherTheFileNamedAUsableCount)
{
    // SamplesChosen is what lets the editor's materializer retire an unusable
    // stored count without restating this vocabulary, so it has to distinguish
    // "named nothing" from "named something unreadable" from "named a count".
    EXPECT_FALSE(ReadRendering(R"({})").SamplesChosen);
    EXPECT_FALSE(ReadRendering(R"({"msaa": "auto"})").SamplesChosen);
    EXPECT_FALSE(ReadRendering(R"({"msaa": 3})").SamplesChosen);
    EXPECT_FALSE(ReadRendering(R"({"msaa": "4x4"})").SamplesChosen);
    EXPECT_TRUE(ReadRendering(R"({"msaa": "off"})").SamplesChosen);
    EXPECT_TRUE(ReadRendering(R"({"msaa": "4"})").SamplesChosen);
    EXPECT_TRUE(ReadRendering(R"({"msaa": 8})").SamplesChosen);
}

// --- Env precedence: env > authored > ladder, in EVERY host -----------------
//
// The guard lives on these two appliers rather than in a host because the
// Player applies its snapshot straight after RenderServices::Initialize with
// nothing in between: a guard in the editor's apply path left both overrides
// dead in a packaged game.

TEST_F(AntiAliasingProjectSettingsTest, TheAaModeEnvOverrideSuppressesOnlyTheModeSetter)
{
    const ScopedEnvVar pinned("GE_AA_MODE", "smaa");

    RenderServices rs;
    // Stands in for RenderServices::Initialize, which is what actually reads
    // the variable; the bare instances these tests build never run it.
    rs.SetDefaultAntiAliasingMode(AntiAliasingMode::SMAA);
    ReadRendering(R"({"aaMode": "msaa", "msaa": "8", "fxaaQuality": "fast"})")
        .ApplyAntiAliasingTo(rs);

    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), AntiAliasingMode::SMAA)
        << "an authored mode overwrote the environment override";
    EXPECT_EQ(rs.GetDefaultMSAASampleCount(), 8u)
        << "GE_AA_MODE names one setter; the count is not its business";
    EXPECT_EQ(rs.GetFxaaQuality(), er::FxaaQuality::Fast);
}

TEST_F(AntiAliasingProjectSettingsTest, TheAaModeEnvOverrideOutranksTheLadderToo)
{
    const ScopedEnvVar pinned("GE_AA_MODE", "smaa");

    RenderServices rs;
    rs.SetDefaultAntiAliasingMode(AntiAliasingMode::SMAA);
    gr::AntiAliasingProjectSettings unchosen{};
    ASSERT_FALSE(unchosen.ModeChosen);
    unchosen.ApplyAntiAliasingTo(rs);

    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), AntiAliasingMode::SMAA);
}

TEST_F(AntiAliasingProjectSettingsTest, TheRenderScaleEnvOverrideSuppressesTheWholeScaleHalf)
{
    const ScopedEnvVar pinned("GE_TAA_RENDER_SCALE", "0.8");

    RenderServices rs;
    rs.SetDefaultRenderScale(0.8f);
    const float defaultTargetGpuMs = rs.GetDynamicResolutionConfig().TargetGpuMs;

    ReadRendering(R"({"taaRenderScale": 0.6, "drsMode": "fixed", "drsTargetFps": 120})")
        .ApplyRenderScaleTo(rs);

    EXPECT_FLOAT_EQ(rs.GetDefaultRenderScale(), 0.8f)
        << "the authored scale overwrote the environment override";
    EXPECT_EQ(rs.GetDynamicResolutionMode(), er::DynamicResolutionMode::Off)
        << "applying the mode pins or restores the scale, which destroys the override";
    EXPECT_FLOAT_EQ(rs.GetDynamicResolutionConfig().TargetGpuMs, defaultTargetGpuMs);
}

TEST_F(AntiAliasingProjectSettingsTest, ShipChainHonoursTheEnvOverrideTheWayTheEditorDoes)
{
    // The Player shape end to end: cook, load, apply — under GE_AA_MODE. This
    // is the row that reds if the guard ever migrates back into a host.
    AuthorProjectSettings(R"({"aaMode": "msaa", "msaa": "2"})");

    GameConfig cooked;
    cooked.renderQuality = gr::AntiAliasingProjectSettings::Load(m_Root);
    ASSERT_TRUE(GameEngine::SaveGameConfig(GameConfigPath(), cooked));
    const GameConfig shipped = GameEngine::LoadGameConfig(GameConfigPath());

    const ScopedEnvVar pinned("GE_AA_MODE", "taa");
    RenderServices rs;
    rs.SetDefaultAntiAliasingMode(AntiAliasingMode::TAA);
    shipped.renderQuality.ApplyTo(rs);

    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), AntiAliasingMode::TAA)
        << "the packaged game's authored mode overwrote GE_AA_MODE";
    EXPECT_EQ(rs.GetDefaultMSAASampleCount(), 2u);
}

// --- The ship chain ---------------------------------------------------------

TEST_F(AntiAliasingProjectSettingsTest, ShipChainCarriesTheChoiceFromProjectFileToShippedRenderer)
{
    // Hop 1: authored in the project the way the editor leaves it.
    AuthorProjectSettings(R"({"aaMode": "msaa", "msaa": "2", "fxaaQuality": "fast",
                              "taaRenderScale": 0.75, "drsMode": "fixed"})");

    // Hop 2: the cook (BuildPipeline::WriteGameConfig) reads the project file.
    GameConfig cooked;
    cooked.renderQuality = gr::AntiAliasingProjectSettings::Load(m_Root);
    ASSERT_TRUE(GameEngine::SaveGameConfig(GameConfigPath(), cooked));

    // Hop 3: the shipped game.config, as the Player reads it. Assert on the
    // FILE too — a writer that emitted a token only its own reader understands
    // would still round-trip. Key PRESENCE is asserted before each read:
    // nlohmann's const operator[] only asserts on a missing key in a debug
    // build, so reading blind would make this hop's coverage evaporate under
    // NDEBUG.
    {
        std::ifstream file(GameConfigPath());
        ASSERT_TRUE(file.is_open());
        const nlohmann::json doc = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
        ASSERT_FALSE(doc.is_discarded()) << "game.config is not readable JSON";
        ASSERT_TRUE(doc.contains("rendering")) << "game.config carries no AA settings at all";
        const nlohmann::json& rendering = doc["rendering"];
        ASSERT_TRUE(rendering.contains("aaMode"));
        ASSERT_TRUE(rendering.contains("msaa"));
        ASSERT_TRUE(rendering.contains("fxaaQuality"));
        EXPECT_EQ(rendering["aaMode"].get<std::string>(), "msaa");
        EXPECT_EQ(rendering["msaa"].get<std::string>(), "2");
        EXPECT_EQ(rendering["fxaaQuality"].get<std::string>(), "fast");
    }

    const GameConfig shipped = GameEngine::LoadGameConfig(GameConfigPath());

    // Hop 4: PlayerApplication::InitRendering.
    RenderServices rs;
    shipped.renderQuality.ApplyTo(rs);
    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), AntiAliasingMode::MSAA);
    EXPECT_EQ(rs.GetDefaultMSAASampleCount(), 2u);
    EXPECT_EQ(rs.GetFxaaQuality(), er::FxaaQuality::Fast);
}

TEST_F(AntiAliasingProjectSettingsTest, ShipChainResolvesTheLadderForAGameConfigThatNamesNoMode)
{
    // The Player applies game.config with nothing in front of it, so a game
    // built from a project that never chose a mode has only this apply to
    // resolve one. Landing on Off here is the shipped-game version of "no
    // anti-aliasing on hardware that can multisample".
    AuthorProjectSettings(R"({"taaRenderScale": 1.0})");

    GameConfig cooked;
    cooked.renderQuality = gr::AntiAliasingProjectSettings::Load(m_Root);
    ASSERT_FALSE(cooked.renderQuality.ModeChosen);
    ASSERT_TRUE(GameEngine::SaveGameConfig(GameConfigPath(), cooked));

    const GameConfig shipped = GameEngine::LoadGameConfig(GameConfigPath());

    RenderServices rs;
    shipped.renderQuality.ApplyTo(rs);

    const er::ResolvedAntiAliasing expected = er::ResolveDefaultAntiAliasing({});
    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), expected.Mode);
    EXPECT_EQ(rs.GetDefaultMSAASampleCount(), expected.SampleCount);
}

TEST_F(AntiAliasingProjectSettingsTest, AGameConfigWithNoRenderingObjectStillResolvesTheLadder)
{
    // Games built before the AA settings existed. Hand-authored rather than
    // cooked, because the cook always writes the object — this is the one shape
    // SaveGameConfig can no longer produce and a shipped build can still carry.
    {
        std::ofstream file(GameConfigPath());
        ASSERT_TRUE(file.is_open());
        file << R"({"gameName": "legacy", "startupScene": "Scenes/Main.scene"})";
    }

    const GameConfig shipped = GameEngine::LoadGameConfig(GameConfigPath());
    ASSERT_FALSE(shipped.renderQuality.ModeChosen);
    RenderServices rs;
    shipped.renderQuality.ApplyTo(rs);

    const er::ResolvedAntiAliasing expected = er::ResolveDefaultAntiAliasing({});
    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), expected.Mode);
    EXPECT_EQ(rs.GetDefaultMSAASampleCount(), expected.SampleCount);
}

} // namespace
