// ApplyProjectRenderSettings — the project-switch contract for the renderer
// knobs the project settings file owns.
//
// The trap these lock: MSAA, AA mode, TAAU render scale, render-scale mode and
// mesh LOD are all per-RenderServices state, and a project switch does not tear
// RenderServices down. So the apply has to be TOTAL — an absent key must apply
// the engine default, not skip the setter — or the outgoing project's value
// stays standing while the settings page, which reads its display value back
// from the incoming project's file, reports the new one.
//
// The sharpest case is the render-scale pair. SetDynamicResolutionMode carries
// transition semantics meant for user edits: leaving Dynamic restores
// m_DrsFixedScale, the scale the user last set. On a switch that value belongs
// to the OUTGOING project, so applying scale-then-mode is not enough — the mode
// setter overwrites the scale that was just applied. DynamicToFixed* below is
// the regression test for that; it fails if the normalize-to-Off step is
// removed from ApplyProjectRenderSettings.

#include "Editor/Settings/RenderProjectSettings.h"

#include "Editor/Settings/DynamicResolutionProjectSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "Engine/Rendering/DynamicResolutionController.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace ed = GameEngine::Editor;
namespace er = GameEngine::Engine::Renderer;
namespace gr = GameEngine::Rendering;
using GameEngine::Engine::Renderer::AntiAliasingMode;
using GameEngine::Engine::Renderer::DynamicResolutionMode;
using GameEngine::Engine::Renderer::RenderServices;
using GameEngine::Engine::Renderer::ShadowMapRenderFeature;

namespace
{

uint32_t ProjectShadowResolution(RenderServices& renderServices)
{
    const auto* feature = renderServices.GetFeature<ShadowMapRenderFeature>();
    return feature ? feature->GetProjectResolutionOverride().value_or(0u) : 0u;
}

void SetEnvVar(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

// Sets (or clears, with nullptr) an environment variable for a scope and puts
// the previous value back. Restoring rather than clearing is what lets the
// fixture neutralize a variable that the test itself then sets.
class ScopedEnvVar
{
  public:
    ScopedEnvVar(const char* name, const char* value) : m_Name(name)
    {
        if (const char* previous = std::getenv(name))
            m_Previous = previous;
        SetEnvVar(name, value);
    }
    ~ScopedEnvVar() { SetEnvVar(m_Name, m_Previous ? m_Previous->c_str() : nullptr); }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

  private:
    const char* m_Name;
    std::optional<std::string> m_Previous;
};

class RenderProjectSettingsTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Root = std::filesystem::temp_directory_path() /
                 ("ge_render_settings_" + std::to_string(::testing::UnitTest::GetInstance()
                                                             ->current_test_info()
                                                             ->line()));
        std::filesystem::remove_all(m_Root);
        std::filesystem::create_directories(m_Root);
    }
    void TearDown() override { std::filesystem::remove_all(m_Root); }

    // Both variables suppress part of the apply under test, so a harness
    // machine that happens to export either would red this suite for a reason
    // that has nothing to do with the code. Neutralized for every test; the one
    // test that wants an override sets it inside its own scope.
    ScopedEnvVar m_NoAaModeOverride{"GE_AA_MODE", nullptr};
    ScopedEnvVar m_NoRenderScaleOverride{"GE_TAA_RENDER_SCALE", nullptr};

    // A project directory carrying exactly the given "rendering" object.
    std::filesystem::path MakeProject(const std::string& name,
                                      const nlohmann::json& rendering) const
    {
        const std::filesystem::path root = m_Root / name;
        std::filesystem::create_directories(root);
        ed::SettingsStore store = ed::OpenProjectSettings(root);
        std::string err;
        (void)store.Load(&err);
        store.Json()["rendering"] = rendering;
        EXPECT_TRUE(store.Save(&err)) << err;
        return root;
    }

    std::filesystem::path MakeEmptyProject(const std::string& name) const
    {
        const std::filesystem::path root = m_Root / name;
        std::filesystem::create_directories(root);
        return root;
    }

    // The project's "rendering" object as it stands ON DISK. A store the code
    // under test still holds would report its own in-memory edits, which is the
    // opposite of what the one-shot write has to prove.
    static nlohmann::json ReadRenderingFromDisk(const std::filesystem::path& root)
    {
        ed::SettingsStore store = ed::OpenProjectSettings(root);
        std::string err;
        (void)store.Load(&err);
        const auto& json = store.Json();
        if (!json.is_object())
            return nlohmann::json::object();
        const auto it = json.find("rendering");
        if (it == json.end() || !it->is_object())
            return nlohmann::json::object();
        return *it;
    }

    std::filesystem::path m_Root;
};

TEST_F(RenderProjectSettingsTest, AppliesEveryKnobTheProjectCarries)
{
    const auto project = MakeProject("full", {{"aaMode", "taa"},
                                              {"msaa", "4"},
                                              {"directionalShadowResolution", 4096},
                                              {"taaRenderScale", 0.6},
                                              {"drsMode", "fixed"},
                                              {"lodMode", "coverage"},
                                              {"lodErrorBudgetPx", 3.5}});

    RenderServices rs;
    ed::ApplyProjectRenderSettings(rs, project);

    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), AntiAliasingMode::TAA);
    EXPECT_EQ(rs.GetDefaultMSAASampleCount(), 4u);
    EXPECT_EQ(ProjectShadowResolution(rs), 4096u);
    EXPECT_FLOAT_EQ(rs.GetDefaultRenderScale(), 0.6f);
    EXPECT_EQ(rs.GetDynamicResolutionMode(), DynamicResolutionMode::Fixed);
    EXPECT_EQ(rs.GetMeshGPURegistry().GetLodSelectionMode(), gr::LodSelectionMode::Coverage);
    EXPECT_FLOAT_EQ(rs.GetLODErrorBudgetPx(), 3.5f);
}

TEST_F(RenderProjectSettingsTest, AppliesBothFxaaModes)
{
    {
        const auto project = MakeProject("fxaa", {{"aaMode", "fxaa"}});
        RenderServices rs;
        ed::ApplyProjectRenderSettings(rs, project);
        EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), AntiAliasingMode::FXAA);
    }
    {
        const auto project = MakeProject("tfxaa", {{"aaMode", "temporalfxaa"}});
        RenderServices rs;
        ed::ApplyProjectRenderSettings(rs, project);
        EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), AntiAliasingMode::TemporalFXAA);
    }
}

TEST_F(RenderProjectSettingsTest,
       AProjectWithNoRenderingKeysTakesTheCapabilityDefaultAndLeavesEveryOtherKnobAlone)
{
    const auto project = MakeEmptyProject("bare");

    RenderServices rs;

    ed::ApplyProjectRenderSettings(rs, project);

    // An absent aaMode is not "the user chose no anti-aliasing", it is "the user
    // never chose", so it takes the capability default rather than Off. This is
    // the one knob whose startup apply is deliberately NOT a no-op against the
    // constructed defaults.
    const auto expected = er::ResolveDefaultAntiAliasing({}); // no device on a bare RenderServices
    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), expected.Mode);
    EXPECT_EQ(rs.GetDefaultMSAASampleCount(), expected.SampleCount);

    // Every other knob still must be, which is what lets the function write them
    // all unconditionally without changing launch behaviour.
    EXPECT_EQ(ProjectShadowResolution(rs), ed::kDefaultDirectionalShadowResolution);
    EXPECT_FLOAT_EQ(rs.GetDefaultRenderScale(), 1.0f);
    EXPECT_EQ(rs.GetDynamicResolutionMode(), DynamicResolutionMode::Off);
}

TEST_F(RenderProjectSettingsTest, SwitchingToAProjectWithNoKeysDropsThePreviousProjectsValues)
{
    const auto projectA = MakeProject("A", {{"aaMode", "taa"},
                                            {"msaa", "4"},
                                            {"directionalShadowResolution", 8192},
                                            {"taaRenderScale", 0.6},
                                            {"drsMode", "fixed"},
                                            {"lodMode", "coverage"}});
    const auto projectB = MakeEmptyProject("B");

    RenderServices rs;

    ed::ApplyProjectRenderSettings(rs, projectA);
    ASSERT_EQ(rs.GetDefaultAntiAliasingMode(), AntiAliasingMode::TAA) << "project A did not apply";

    ed::ApplyProjectRenderSettings(rs, projectB);

    // B's settings page reports the defaults for all of these, so the renderer
    // must too — and B having no aaMode key means it takes the capability
    // default, so A's explicit TAA and 4x must not survive the switch.
    const auto expected = er::ResolveDefaultAntiAliasing({});
    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), expected.Mode);
    EXPECT_EQ(rs.GetDefaultMSAASampleCount(), expected.SampleCount);
    EXPECT_EQ(ProjectShadowResolution(rs), ed::kDefaultDirectionalShadowResolution);
    EXPECT_FLOAT_EQ(rs.GetDefaultRenderScale(), 1.0f);
    EXPECT_EQ(rs.GetDynamicResolutionMode(), DynamicResolutionMode::Off);
    EXPECT_EQ(rs.GetMeshGPURegistry().GetLodSelectionMode(), gr::LodSelectionMode::Sse);
}

TEST_F(RenderProjectSettingsTest, DirectionalShadowResolutionSupportsAllTiersAndSnapsInvalidValues)
{
    RenderServices rs;
    for (uint32_t tier : {1024u, 2048u, 4096u, 8192u})
    {
        const auto project = MakeProject("tier" + std::to_string(tier),
                                         {{"directionalShadowResolution", tier}});
        ed::ApplyProjectRenderSettings(rs, project);
        EXPECT_EQ(ProjectShadowResolution(rs), tier);
    }

    const auto invalid = MakeProject("invalidTier", {{"directionalShadowResolution", 5000}});
    ed::ApplyProjectRenderSettings(rs, invalid);
    EXPECT_EQ(ProjectShadowResolution(rs), 4096u);
}

TEST_F(RenderProjectSettingsTest, DirectionalShadowSettingUsesTheLoadedSettingsStore)
{
    const auto project = MakeEmptyProject("storeApi");
    ed::SettingsStore store = ed::OpenProjectSettings(project);
    std::string error;
    ASSERT_TRUE(store.Load(&error)) << error;

    ed::SetDirectionalShadowResolution(store, 8192u);
    EXPECT_EQ(ed::GetDirectionalShadowResolution(store), 8192u);
    ASSERT_TRUE(store.Save(&error)) << error;

    ed::SettingsStore reloaded = ed::OpenProjectSettings(project);
    ASSERT_TRUE(reloaded.Load(&error)) << error;
    EXPECT_EQ(ed::GetDirectionalShadowResolution(reloaded), 8192u);
}

TEST_F(RenderProjectSettingsTest, DynamicToFixedTakesTheIncomingProjectsScaleNotTheOutgoingOne)
{
    // The two scales differ, and neither is the 1.0 default, so a pass cannot
    // come from the value simply being left alone.
    const auto projectA = MakeProject("dynamic", {{"drsMode", "dynamic"},
                                                  {"taaRenderScale", 0.6}});
    const auto projectB = MakeProject("fixed", {{"drsMode", "fixed"},
                                                {"taaRenderScale", 0.75}});

    RenderServices rs;
    ed::ApplyProjectRenderSettings(rs, projectA);
    ASSERT_EQ(rs.GetDynamicResolutionMode(), DynamicResolutionMode::Dynamic);

    ed::ApplyProjectRenderSettings(rs, projectB);

    EXPECT_EQ(rs.GetDynamicResolutionMode(), DynamicResolutionMode::Fixed);
    EXPECT_FLOAT_EQ(rs.GetDefaultRenderScale(), 0.75f)
        << "the render-scale mode setter restored the outgoing project's "
           "m_DrsFixedScale over the incoming project's scale";
}

TEST_F(RenderProjectSettingsTest, DynamicToOffPinsTheScaleToNative)
{
    const auto projectA = MakeProject("dynamic", {{"drsMode", "dynamic"},
                                                  {"taaRenderScale", 0.6}});
    // An explicit "off" alongside a sub-1.0 scale: Off means native, so the
    // mode wins over the leftover scale rather than silently upscaling.
    const auto projectB = MakeProject("off", {{"drsMode", "off"},
                                              {"taaRenderScale", 0.7}});

    RenderServices rs;
    ed::ApplyProjectRenderSettings(rs, projectA);
    ed::ApplyProjectRenderSettings(rs, projectB);

    EXPECT_EQ(rs.GetDynamicResolutionMode(), DynamicResolutionMode::Off);
    EXPECT_FLOAT_EQ(rs.GetDefaultRenderScale(), 1.0f);
}

TEST_F(RenderProjectSettingsTest, AMissingModeKeyStillMigratesFromThePersistedScale)
{
    // A project saved before rendering.drsMode existed states its intent only
    // through the scale, and must keep behaving as it did: sub-1.0 means Fixed.
    const auto project = MakeProject("legacy", {{"taaRenderScale", 0.8}});

    RenderServices rs;
    ed::ApplyProjectRenderSettings(rs, project);

    EXPECT_EQ(rs.GetDynamicResolutionMode(), DynamicResolutionMode::Fixed);
    EXPECT_FLOAT_EQ(rs.GetDefaultRenderScale(), 0.8f);
}

// The dynamic target is the second half of the render-scale mode: the settings
// page and the debug server both persist it and then lean on this apply to
// carry it to every window's RenderServices.
TEST_F(RenderProjectSettingsTest, TheDynamicTargetFpsReachesTheControllerConfig)
{
    const auto project = MakeProject("target", {{"drsMode", "dynamic"},
                                                {"drsTargetFps", 120.0}});

    RenderServices rs;
    ed::ApplyProjectRenderSettings(rs, project);

    EXPECT_EQ(rs.GetDynamicResolutionMode(), DynamicResolutionMode::Dynamic);
    EXPECT_FLOAT_EQ(rs.GetDynamicResolutionConfig().TargetGpuMs, 1000.0f / 120.0f);
}

TEST_F(RenderProjectSettingsTest, AnAbsentDynamicTargetDropsThePreviousProjectsTarget)
{
    const auto projectA = MakeProject("fastTarget", {{"drsMode", "dynamic"},
                                                     {"drsTargetFps", 120.0}});
    const auto projectB = MakeProject("noTarget", {{"drsMode", "dynamic"}});

    RenderServices rs;
    ed::ApplyProjectRenderSettings(rs, projectA);
    ASSERT_FLOAT_EQ(rs.GetDynamicResolutionConfig().TargetGpuMs, 1000.0f / 120.0f);

    ed::ApplyProjectRenderSettings(rs, projectB);

    EXPECT_FLOAT_EQ(rs.GetDynamicResolutionConfig().TargetGpuMs,
                    1000.0f / ed::kDefaultDrsTargetFps)
        << "B's settings page shows the default target, so the renderer must hold it too";
}

// GE_TAA_RENDER_SCALE suppresses the render-scale MODE and the dynamic target,
// not only the scale, and that is load-bearing rather than an oversight:
// SetDynamicResolutionMode owns the scale (Off pins 1.0; leaving Dynamic
// restores the remembered value), so applying the project's mode would
// overwrite the very value the harness pinned. The Player has no DRS plumbing
// at all -- it never calls SetDynamicResolutionMode -- so this variable is the
// only render-scale lever that reaches it, and it has to survive intact.
TEST_F(RenderProjectSettingsTest, ATaaRenderScaleEnvOverrideOutranksTheWholeModeBlock)
{
    const auto project = MakeProject("envPinned", {{"aaMode", "taa"},
                                                   {"msaa", "4"},
                                                   {"drsMode", "fixed"},
                                                   {"drsTargetFps", 120.0},
                                                   {"taaRenderScale", 0.6}});

    // Control: the same project with no override applies all of it. Without
    // this the assertions below could pass on a project carrying nothing.
    {
        RenderServices control;
        ed::ApplyProjectRenderSettings(control, project);
        ASSERT_EQ(control.GetDynamicResolutionMode(), DynamicResolutionMode::Fixed);
        ASSERT_FLOAT_EQ(control.GetDefaultRenderScale(), 0.6f);
        ASSERT_FLOAT_EQ(control.GetDynamicResolutionConfig().TargetGpuMs, 1000.0f / 120.0f);
    }

    RenderServices rs;
    const float defaultTargetGpuMs = rs.GetDynamicResolutionConfig().TargetGpuMs;
    // Stands in for RenderServices::Initialize, which is what actually reads the
    // variable; the bare instances these tests build never run it.
    rs.SetDefaultRenderScale(0.8f);

    const ScopedEnvVar pinned("GE_TAA_RENDER_SCALE", "0.8");
    ed::ApplyProjectRenderSettings(rs, project);

    EXPECT_FLOAT_EQ(rs.GetDefaultRenderScale(), 0.8f)
        << "the project's scale overwrote the env override";
    EXPECT_EQ(rs.GetDynamicResolutionMode(), DynamicResolutionMode::Off)
        << "applying the project's mode pins or restores the scale, which would "
           "destroy the override -- the mode is suppressed with it";
    EXPECT_FLOAT_EQ(rs.GetDynamicResolutionConfig().TargetGpuMs, defaultTargetGpuMs)
        << "the dynamic target belongs to the suppressed block";

    // Scoped to the render-scale block: the AA knobs are a different override's
    // business and still apply.
    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), AntiAliasingMode::TAA);
    EXPECT_EQ(rs.GetDefaultMSAASampleCount(), 4u);
}

TEST_F(RenderProjectSettingsTest, LegacyExplicitMsaaSamplesWithNoModeKeyStillMeanMsaa)
{
    const auto project = MakeProject("legacyMsaa", {{"msaa", "8"}});

    RenderServices rs;
    ed::ApplyProjectRenderSettings(rs, project);

    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), AntiAliasingMode::MSAA);
    EXPECT_EQ(rs.GetDefaultMSAASampleCount(), 8u);
}

// The capability ladder a project that has never chosen an anti-aliasing mode
// starts on: 4x MSAA -> 2x MSAA -> TAA, first rung the device can run. It lives
// in Engine/Rendering/AntiAliasing.h because the Player takes the same rung for
// a game.config that names no mode; its tests stay here, next to the
// project-settings apply that is their reason for existing. Driven
// as a pure function so every rung is reachable without owning a device that
// actually reports that capability — the point is the ladder, and a test that
// could only exercise whichever rung this machine's GPU lands on would gate
// nothing.

// The head of the ladder, and the assertion that gates it: a device that can
// run 4x gets 4x, never the 2x rung below it. This is the one the 4x-first
// decision rests on — it reds if the head moves.
TEST(ResolveDefaultAntiAliasingTest, PrefersFourSampleMsaaOnEveryDeviceThatOffersIt)
{
    for (const uint32_t maxSamples : {4u, 8u, 16u})
    {
        const auto resolved = er::ResolveDefaultAntiAliasing({maxSamples, false});
        EXPECT_EQ(resolved.Mode, AntiAliasingMode::MSAA) << "maxMsaaSamples=" << maxSamples;
        EXPECT_EQ(resolved.SampleCount, 4u) << "maxMsaaSamples=" << maxSamples;
    }
}

TEST(ResolveDefaultAntiAliasingTest, DropsToTwoSampleMsaaWhenTheDeviceCannotReachFour)
{
    const auto resolved = er::ResolveDefaultAntiAliasing({/*MaxMsaaSamples=*/2u,
                                                          /*PrefersNoDefaultMsaa=*/false});
    EXPECT_EQ(resolved.Mode, AntiAliasingMode::MSAA);
    EXPECT_EQ(resolved.SampleCount, 2u);
}

TEST(ResolveDefaultAntiAliasingTest, FallsBackToTaaWhenMsaaIsUnsupported)
{
    // 0 is the "no device answered" value; 1 is a device reporting no
    // multisampling. Neither can multisample, so both take TAA.
    for (const uint32_t maxSamples : {0u, 1u})
    {
        const auto resolved = er::ResolveDefaultAntiAliasing({maxSamples, false});
        EXPECT_EQ(resolved.Mode, AntiAliasingMode::TAA) << "maxMsaaSamples=" << maxSamples;
        EXPECT_EQ(resolved.SampleCount, 1u) << "maxMsaaSamples=" << maxSamples;
    }
}

TEST(ResolveDefaultAntiAliasingTest, SkipsMsaaEntirelyWhenTheDriverAsksNotToDefaultToIt)
{
    // prefersNoDefaultMSAA exists for drivers with fragile MSAA paths, and a
    // default nobody asked for is precisely the case it is meant to suppress —
    // so a device that could run 8x still gets TAA.
    const auto resolved = er::ResolveDefaultAntiAliasing({8u, /*PrefersNoDefaultMsaa=*/true});
    EXPECT_EQ(resolved.Mode, AntiAliasingMode::TAA);
    EXPECT_EQ(resolved.SampleCount, 1u);
}

// The ladder's -> off tail is NOT this function's: TAA applicability is
// per-view (orthographic / 2D / fixed-orientation panes drop it in
// SceneViewController's per-frame resolve). Resolving to TAA therefore means
// "TAA where it applies, otherwise off", and this pins that the resolver never
// itself returns Off — an Off here would silently disable AA on capable
// devices.
TEST(ResolveDefaultAntiAliasingTest, NeverResolvesToOffBecauseTheOffTailIsPerView)
{
    for (const uint32_t maxSamples : {0u, 1u, 2u, 4u, 8u, 16u})
    {
        for (const bool prefersNone : {false, true})
        {
            const auto resolved = er::ResolveDefaultAntiAliasing({maxSamples, prefersNone});
            EXPECT_NE(resolved.Mode, AntiAliasingMode::Off)
                << "maxMsaaSamples=" << maxSamples << " prefersNoDefaultMsaa=" << prefersNone;
        }
    }
}

// The one-shot write. The default is resolved ONCE against the machine that
// opens the project and stored concretely; after that it is an ordinary
// setting, and nothing re-derives it per frame or per machine.

TEST_F(RenderProjectSettingsTest, MaterializingWritesTheDeviceDefaultIntoAProjectThatNeverChose)
{
    const auto project = MakeEmptyProject("materializeBare");

    ed::SettingsStore store = ed::OpenProjectSettings(project);
    std::string error;
    ASSERT_TRUE(store.Load(&error)) << error;
    EXPECT_TRUE(ed::MaterializeDefaultAntiAliasing(store, {8u, false}));

    // Read the FILE back, not the store: the point of the one-shot is that the
    // concrete value is on disk and travels with the project.
    const nlohmann::json rendering = ReadRenderingFromDisk(project);
    EXPECT_EQ(rendering.value("aaMode", std::string{}), "msaa");
    EXPECT_EQ(rendering.value("msaa", std::string{}), "4");

    // One-shot: a second open finds a project that HAS chosen and leaves it be.
    ed::SettingsStore reopened = ed::OpenProjectSettings(project);
    ASSERT_TRUE(reopened.Load(&error)) << error;
    EXPECT_FALSE(ed::MaterializeDefaultAntiAliasing(reopened, {2u, false}))
        << "the stored mode was re-derived against a different machine";
    EXPECT_EQ(ReadRenderingFromDisk(project).value("msaa", std::string{}), "4");
}

TEST_F(RenderProjectSettingsTest, MaterializingTheTaaRungWritesOnlyTheModeAndLeavesTheCountAlone)
{
    // The sample count is honoured only in msaa mode, so a TAA default has no
    // business rewriting a count the user may have set. The stored 1 is not a
    // legacy MSAA choice (only a count above 1 is), so this project has still
    // never chosen a mode.
    const auto project = MakeProject("materializeTaa", {{"msaa", "1"}});

    ed::SettingsStore store = ed::OpenProjectSettings(project);
    std::string error;
    ASSERT_TRUE(store.Load(&error)) << error;
    ASSERT_TRUE(ed::MaterializeDefaultAntiAliasing(store, {8u, /*PrefersNoDefaultMsaa=*/true}));

    const nlohmann::json rendering = ReadRenderingFromDisk(project);
    EXPECT_EQ(rendering.value("aaMode", std::string{}), "taa");
    EXPECT_EQ(rendering.value("msaa", std::string{}), "1");
}

TEST_F(RenderProjectSettingsTest, MaterializingRewritesTheRetiredAutoToken)
{
    // The migration for the projects that stored "auto" while the mode resolved
    // per frame. One rewrite, through the same ladder, and the token is gone.
    const auto project = MakeProject("materializeAuto", {{"aaMode", "auto"}, {"msaa", "8"}});

    ed::SettingsStore store = ed::OpenProjectSettings(project);
    std::string error;
    ASSERT_TRUE(store.Load(&error)) << error;
    EXPECT_TRUE(ed::MaterializeDefaultAntiAliasing(store, {8u, false}));

    const nlohmann::json rendering = ReadRenderingFromDisk(project);
    EXPECT_EQ(rendering.value("aaMode", std::string{}), "msaa");
    // The ladder owns the count, so the 8 that "auto" used to override is
    // replaced rather than left standing.
    EXPECT_EQ(rendering.value("msaa", std::string{}), "4");
}

TEST_F(RenderProjectSettingsTest, MaterializingWritesAConcreteCountEvenOnTheTaaRung)
{
    // The TAA rung does not choose a count, so the count that lands is whatever
    // the read produced. That used to be the "auto" sentinel, which the writer
    // put straight back into the file and the cook then baked into a shipped
    // game. Every rung must leave a count a reader can act on without a device.
    const auto project = MakeProject("materializeTaaNoCount", {{"aaMode", "auto"}});

    ed::SettingsStore store = ed::OpenProjectSettings(project);
    std::string error;
    ASSERT_TRUE(store.Load(&error)) << error;
    ASSERT_TRUE(ed::MaterializeDefaultAntiAliasing(store, {8u, /*PrefersNoDefaultMsaa=*/true}));

    const nlohmann::json rendering = ReadRenderingFromDisk(project);
    EXPECT_EQ(rendering.value("aaMode", std::string{}), "taa");
    EXPECT_EQ(rendering.value("msaa", std::string{}),
              std::to_string(er::kDefaultMsaaSampleCount));
}

TEST_F(RenderProjectSettingsTest, TheLaddersTopRungIsTheSameCountAnUnnamedOneParsesAs)
{
    // Two answers to "how many samples by default" — the mode ladder's MSAA
    // rung and the parser's fill-in for a file naming no count. They are the
    // same product decision, and a machine that reaches 4x must not get one
    // number from the ladder and a different one from a file it half-read.
    EXPECT_EQ(er::ResolveDefaultAntiAliasing({8u, false}).SampleCount,
              er::kDefaultMsaaSampleCount);
}

TEST_F(RenderProjectSettingsTest, AStoredAutoCountUnderAChosenMsaaModeAppliesTheDefaultCount)
{
    // The mode IS chosen here, so the ladder stays out of it and only the count
    // is missing. It used to resolve to the device's maximum, which is how an
    // 8x-capable machine ended up on 8x MSAA nobody selected.
    const auto project = MakeProject("autoCountChosenMode", {{"aaMode", "msaa"},
                                                             {"msaa", "auto"}});

    RenderServices rs;
    ed::ApplyProjectRenderSettings(rs, project);

    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), AntiAliasingMode::MSAA);
    EXPECT_EQ(rs.GetDefaultMSAASampleCount(), er::kDefaultMsaaSampleCount);
}

TEST_F(RenderProjectSettingsTest, TheAaModeEnvOverrideOutranksTheCapabilityLadderToo)
{
    // GE_AA_MODE names a mode, which makes the project's "never chose" moot:
    // the apply must keep the renderer's env-set mode rather than resolving a
    // capability default over the top of it. An authored mode is outranked the
    // same way, which AnAuthoredModeLosesToTheEnvOverride pins below.
    const auto project = MakeEmptyProject("envModeNeverChose");

    RenderServices rs;
    // Stands in for RenderServices::Initialize, which is what actually reads
    // the variable; the bare instances these tests build never run it.
    rs.SetDefaultAntiAliasingMode(AntiAliasingMode::SMAA);

    const ScopedEnvVar pinned("GE_AA_MODE", "smaa");
    ed::ApplyProjectRenderSettings(rs, project);

    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), AntiAliasingMode::SMAA)
        << "the capability ladder overrode an explicitly requested mode";
}

TEST_F(RenderProjectSettingsTest, AnAuthoredModeLosesToTheEnvOverrideButTheCountStillApplies)
{
    // env > authored > ladder. GE_AA_MODE suppresses exactly one setter, so the
    // project's sample count and FXAA quality still reach the renderer.
    const auto project = MakeProject("envModeChosen", {{"aaMode", "msaa"}, {"msaa", "2"}});

    RenderServices rs;
    rs.SetDefaultAntiAliasingMode(AntiAliasingMode::SMAA);

    const ScopedEnvVar pinned("GE_AA_MODE", "smaa");
    ed::ApplyProjectRenderSettings(rs, project);

    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), AntiAliasingMode::SMAA);
    EXPECT_EQ(rs.GetDefaultMSAASampleCount(), 2u)
        << "GE_AA_MODE names one setter; the count is not its business";
}

TEST_F(RenderProjectSettingsTest, MaterializingRewritesAnUnusableCountUnderAChosenMode)
{
    // The half a "return early if the project chose" materializer cannot reach.
    // The MODE is chosen, so nothing about it is re-derived — but the COUNT is
    // the retired token, and the vocabulary alone answers it, so it is rewritten
    // rather than left in the file to be re-resolved on every read forever.
    for (const char* mode : {"msaa", "taa"})
    {
        const auto project = MakeProject(std::string("autoCountChosen") + mode,
                                         {{"aaMode", mode}, {"msaa", "auto"}});

        ed::SettingsStore store = ed::OpenProjectSettings(project);
        std::string error;
        ASSERT_TRUE(store.Load(&error)) << error;
        EXPECT_TRUE(ed::MaterializeDefaultAntiAliasing(store, {8u, false})) << mode;

        const nlohmann::json rendering = ReadRenderingFromDisk(project);
        EXPECT_EQ(rendering.value("aaMode", std::string{}), mode) << "the chosen mode was touched";
        EXPECT_EQ(rendering.value("msaa", std::string{}),
                  std::to_string(er::kDefaultMsaaSampleCount)) << mode;

        // One-shot: the token is gone, so a second open finds nothing to do.
        ed::SettingsStore reopened = ed::OpenProjectSettings(project);
        ASSERT_TRUE(reopened.Load(&error)) << error;
        EXPECT_FALSE(ed::MaterializeDefaultAntiAliasing(reopened, {8u, false})) << mode;
    }
}

TEST_F(RenderProjectSettingsTest, MaterializingRewritesAnUnreadableCountWithNoDeviceToAsk)
{
    // The count needs no device, so a device-less host still retires the token.
    // The MODE is the half that waits, and it must still be left absent here.
    const auto project = MakeProject("unreadableCountNoDevice", {{"msaa", 3}});

    ed::SettingsStore store = ed::OpenProjectSettings(project);
    std::string error;
    ASSERT_TRUE(store.Load(&error)) << error;
    EXPECT_TRUE(ed::MaterializeDefaultAntiAliasing(store, {}));

    const nlohmann::json rendering = ReadRenderingFromDisk(project);
    EXPECT_FALSE(rendering.contains("aaMode"))
        << "a device-less host decided a mode nothing could answer";
    EXPECT_FALSE(rendering.contains("msaa"))
        << "an unchosen snapshot writes the count away with the mode";
}

TEST_F(RenderProjectSettingsTest, MaterializingLeavesAProjectWithNoCountKeyAlone)
{
    // The trigger is an UNUSABLE stored count, not a missing one. An absent key
    // is what every project that never opened the page looks like, and writing
    // to all of them on open would make the one-shot a no-op-that-saves.
    const auto project = MakeProject("chosenNoCountKey", {{"aaMode", "taa"}});

    ed::SettingsStore store = ed::OpenProjectSettings(project);
    std::string error;
    ASSERT_TRUE(store.Load(&error)) << error;
    EXPECT_FALSE(ed::MaterializeDefaultAntiAliasing(store, {8u, false}));

    EXPECT_FALSE(ReadRenderingFromDisk(project).contains("msaa"));
}

TEST_F(RenderProjectSettingsTest, MaterializingLeavesAProjectThatChoseAlone)
{
    // A stored mode is the user's answer. Re-deriving it would be the engine
    // arguing with them, and would undo the choice on every open.
    for (const char* mode : {"off", "msaa", "taa"})
    {
        const auto project = MakeProject(std::string("materializeKeep") + mode,
                                         {{"aaMode", mode}, {"msaa", "2"}});

        ed::SettingsStore store = ed::OpenProjectSettings(project);
        std::string error;
        ASSERT_TRUE(store.Load(&error)) << error;
        EXPECT_FALSE(ed::MaterializeDefaultAntiAliasing(store, {8u, false})) << mode;

        const nlohmann::json rendering = ReadRenderingFromDisk(project);
        EXPECT_EQ(rendering.value("aaMode", std::string{}), mode);
        EXPECT_EQ(rendering.value("msaa", std::string{}), "2") << mode;
    }
}

TEST_F(RenderProjectSettingsTest, MaterializingLeavesALegacySampleCountAlone)
{
    // No aaMode key, but a sample count from before the key existed: that
    // project HAS chosen MSAA, and LoadAAModeChoice already says so.
    const auto project = MakeProject("materializeLegacy", {{"msaa", "8"}});

    ed::SettingsStore store = ed::OpenProjectSettings(project);
    std::string error;
    ASSERT_TRUE(store.Load(&error)) << error;
    EXPECT_FALSE(ed::MaterializeDefaultAntiAliasing(store, {8u, false}));

    EXPECT_EQ(ReadRenderingFromDisk(project).value("msaa", std::string{}), "8");
}

TEST_F(RenderProjectSettingsTest, MaterializingWritesNothingWhenNoDeviceAnswered)
{
    // Nothing is decided before something can answer. Burning TAA in here would
    // pin a device-less host's answer onto a machine that can multisample.
    const auto project = MakeEmptyProject("materializeNoDevice");

    ed::SettingsStore store = ed::OpenProjectSettings(project);
    std::string error;
    ASSERT_TRUE(store.Load(&error)) << error;
    EXPECT_FALSE(ed::MaterializeDefaultAntiAliasing(store, {}));

    EXPECT_FALSE(ReadRenderingFromDisk(project).contains("aaMode"));
}

// The in-memory fallback the apply keeps for a store that has not been
// materialized, and the two things it must NOT disturb.

TEST_F(RenderProjectSettingsTest, AnExplicitOffIsHonouredAndNotUpgradedToTheDefault)
{
    // ABSENT means "never chose". A stored "off" is a choice, and overriding it
    // would be the engine arguing with the user.
    const auto project = MakeProject("explicitOff", {{"aaMode", "off"}});

    RenderServices rs;
    ed::ApplyProjectRenderSettings(rs, project);

    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), AntiAliasingMode::Off);
}

TEST_F(RenderProjectSettingsTest, AMissingAaModeKeyWithNoLegacySamplesTakesTheCapabilityDefault)
{
    // rendering exists but says nothing about AA — still "never chose".
    const auto project = MakeProject("noAaKey", {{"directionalShadowResolution", 4096}});

    RenderServices rs;
    ed::ApplyProjectRenderSettings(rs, project);

    const auto expected = er::ResolveDefaultAntiAliasing({});
    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), expected.Mode);
    EXPECT_EQ(rs.GetDefaultMSAASampleCount(), expected.SampleCount);
}

TEST_F(RenderProjectSettingsTest, AStoredAutoTokenAppliesAsNeverChosenRatherThanOff)
{
    // Whitespace and case included: a hand-edited file, and the reader that
    // recognises the retired token is the only thing standing between it and
    // the unrecognised branch, which would silently resolve Off.
    //
    // No device on a bare RenderServices, so the ladder reports its no-MSAA
    // answer: the apply must not invent a sample count no device approved, and
    // the stored msaa=8 must not survive a mode the project never chose.
    const auto project = MakeProject("storedAuto", {{"aaMode", "  Auto "}, {"msaa", "8"}});

    RenderServices rs;
    ed::ApplyProjectRenderSettings(rs, project);

    const auto expected = er::ResolveDefaultAntiAliasing({});
    EXPECT_EQ(rs.GetDefaultAntiAliasingMode(), expected.Mode);
    EXPECT_EQ(rs.GetDefaultMSAASampleCount(), expected.SampleCount);
    EXPECT_NE(rs.GetDefaultMSAASampleCount(), 8u);
}

} // namespace
