// The Player's HDR output resolution: command line > environment > game.config.
//
// This is the shipped surface — a game runs with whatever these three layers
// agree on, and no editor UI exists to contradict a wrong answer. The cases that
// matter most here are the REFUSALS: an unrecognized mode must never resolve to
// Off, because a typo that silently ships SDR looks exactly like a working run.

#include "PlayerHdrOptions.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using GameEngine::PlayerHdrEnvironment;
using GameEngine::PlayerHdrOptions;
using GameEngine::PlayerHdrProjectSettings;
using GameEngine::ResolvePlayerHdrOptions;
using GameEngine::Rendering::HdrOutputMode;
using GameEngine::Rendering::HdrSwapchainBitDepth;

namespace
{

// game.config's own defaults (GameConfig.h): HDR off, but the mode field still
// names HDR10_PQ. A project that never touched HDR therefore carries a mode.
constexpr PlayerHdrProjectSettings kProjectDefaults{false, HdrOutputMode::HDR10_PQ,
                                                    HdrSwapchainBitDepth::Bit10};
// A project that ships with HDR pinned on.
constexpr PlayerHdrProjectSettings kProjectHdrOn{true, HdrOutputMode::HDR10_PQ,
                                                 HdrSwapchainBitDepth::Bit10};

PlayerHdrOptions Resolve(std::vector<const char*> args,
                         const PlayerHdrEnvironment& environment = {},
                         const PlayerHdrProjectSettings& project = kProjectDefaults)
{
    args.insert(args.begin(), "Player.exe");
    return ResolvePlayerHdrOptions(static_cast<int>(args.size()), args.data(), environment, project);
}

PlayerHdrEnvironment EnvMode(const char* value)
{
    PlayerHdrEnvironment environment;
    environment.Mode = value;
    return environment;
}

} // namespace

// --- Precedence: the launch command outranks an inherited shell -------------

TEST(PlayerHdrOptions, CommandLineModeBeatsEnvironment)
{
    const PlayerHdrOptions options = Resolve({"--hdr-mode", "hdr10"}, EnvMode("off"), kProjectDefaults);
    EXPECT_EQ(options.Mode, HdrOutputMode::HDR10_PQ);
    EXPECT_TRUE(options.Enabled);
    EXPECT_STREQ(options.ModeSource, "--hdr-mode");
}

TEST(PlayerHdrOptions, EnvironmentModeBeatsGameConfig)
{
    const PlayerHdrOptions options = Resolve({}, EnvMode("hlg"), kProjectHdrOn);
    EXPECT_EQ(options.Mode, HdrOutputMode::HLG);
    EXPECT_STREQ(options.ModeSource, "GE_HDR_MODE");
}

TEST(PlayerHdrOptions, GameConfigGovernsWhenNothingElseSpeaks)
{
    const PlayerHdrOptions options = Resolve({}, PlayerHdrEnvironment{}, kProjectHdrOn);
    EXPECT_EQ(options.Mode, HdrOutputMode::HDR10_PQ);
    EXPECT_TRUE(options.Enabled);
    EXPECT_STREQ(options.ModeSource, "game.config");
}

TEST(PlayerHdrOptions, DisablingFlagBeatsEnvironmentThatEnables)
{
    const PlayerHdrOptions options = Resolve({"--no-hdr"}, EnvMode("hdr10"), kProjectHdrOn);
    EXPECT_EQ(options.Mode, HdrOutputMode::Off);
    EXPECT_FALSE(options.Enabled);
    EXPECT_STREQ(options.ModeSource, "--no-hdr");
}

// --- Refusals: a typo must not ship SDR silently ----------------------------

TEST(PlayerHdrOptions, UnrecognizedCommandLineModeKeepsProjectSettingRatherThanDisabling)
{
    const PlayerHdrOptions options = Resolve({"--hdr-mode", "hdr11"}, PlayerHdrEnvironment{}, kProjectHdrOn);
    EXPECT_TRUE(options.Enabled);
    EXPECT_EQ(options.Mode, HdrOutputMode::HDR10_PQ);
    EXPECT_STREQ(options.ModeSource, "game.config");
}

TEST(PlayerHdrOptions, UnrecognizedEnvironmentModeKeepsProjectSettingRatherThanDisabling)
{
    const PlayerHdrOptions options = Resolve({}, EnvMode("hdr-eleven"), kProjectHdrOn);
    EXPECT_TRUE(options.Enabled);
    EXPECT_EQ(options.Mode, HdrOutputMode::HDR10_PQ);
    EXPECT_STREQ(options.ModeSource, "game.config");
}

TEST(PlayerHdrOptions, RefusedFlagFallsToTheNextLayerDownNotToOff)
{
    const PlayerHdrOptions options = Resolve({"--hdr-mode", "nonsense"}, EnvMode("hlg"), kProjectDefaults);
    EXPECT_TRUE(options.Enabled);
    EXPECT_EQ(options.Mode, HdrOutputMode::HLG);
    EXPECT_STREQ(options.ModeSource, "GE_HDR_MODE");
}

TEST(PlayerHdrOptions, MissingModeValueDoesNotSwallowTheFollowingFlag)
{
    // "--hdr-mode --no-hdr": the missing value is refused, and --no-hdr must
    // still be seen rather than consumed as the mode.
    const PlayerHdrOptions options = Resolve({"--hdr-mode", "--no-hdr"}, PlayerHdrEnvironment{}, kProjectHdrOn);
    EXPECT_FALSE(options.Enabled);
    EXPECT_EQ(options.Mode, HdrOutputMode::Off);
    EXPECT_STREQ(options.ModeSource, "--no-hdr");
}

// --- Boolean gating over a named mode ---------------------------------------

TEST(PlayerHdrOptions, EnableFlagKeepsTheModeTheProjectNamed)
{
    const PlayerHdrOptions options = Resolve({"--hdr"}, PlayerHdrEnvironment{}, kProjectDefaults);
    EXPECT_TRUE(options.Enabled);
    EXPECT_EQ(options.Mode, HdrOutputMode::HDR10_PQ);
    // --hdr only gated the mode; game.config is still where HDR10_PQ came from.
    EXPECT_STREQ(options.ModeSource, "game.config");
}

TEST(PlayerHdrOptions, EnabledRequestNeverCarriesModeOff)
{
    const PlayerHdrProjectSettings offProject{false, HdrOutputMode::Off, HdrSwapchainBitDepth::Bit10};
    const PlayerHdrOptions options = Resolve({"--hdr"}, PlayerHdrEnvironment{}, offProject);
    EXPECT_TRUE(options.Enabled);
    EXPECT_EQ(options.Mode, HdrOutputMode::Auto);
    // Here the flag really did author the mode, so it owns the attribution.
    EXPECT_STREQ(options.ModeSource, "--hdr");
}

TEST(PlayerHdrOptions, LastCommandLineHdrArgumentWins)
{
    const PlayerHdrOptions options = Resolve({"--hdr-mode", "hlg", "--no-hdr"});
    EXPECT_FALSE(options.Enabled);
    EXPECT_EQ(options.Mode, HdrOutputMode::Off);
}

TEST(PlayerHdrOptions, UnrecognizedEnabledEnvironmentValueIsRefusedNotTreatedAsTrue)
{
    PlayerHdrEnvironment environment;
    environment.Enabled = "flase";
    const PlayerHdrOptions options = Resolve({}, environment, kProjectDefaults);
    EXPECT_FALSE(options.Enabled);
    EXPECT_STREQ(options.ModeSource, "game.config");
}

TEST(PlayerHdrOptions, EnabledEnvironmentAcceptsSpelledBooleans)
{
    PlayerHdrEnvironment environment;
    environment.Enabled = "false";
    const PlayerHdrOptions options = Resolve({}, environment, kProjectHdrOn);
    EXPECT_FALSE(options.Enabled);
    EXPECT_EQ(options.Mode, HdrOutputMode::Off);
    EXPECT_STREQ(options.ModeSource, "GE_HDR");
}

// --- Swapchain bit depth carries the same precedence ------------------------

TEST(PlayerHdrOptions, CommandLineBitDepthBeatsEnvironment)
{
    PlayerHdrEnvironment environment;
    environment.BitDepth = "10";
    const PlayerHdrOptions options = Resolve({"--hdr-bit-depth", "16"}, environment, kProjectDefaults);
    EXPECT_EQ(options.BitDepth, HdrSwapchainBitDepth::Float16);
    EXPECT_STREQ(options.BitDepthSource, "--hdr-bit-depth");
}

TEST(PlayerHdrOptions, UnrecognizedBitDepthKeepsTheLayerBelow)
{
    PlayerHdrEnvironment environment;
    environment.BitDepth = "12";
    const PlayerHdrOptions options = Resolve({}, environment, kProjectDefaults);
    EXPECT_EQ(options.BitDepth, HdrSwapchainBitDepth::Bit10);
    EXPECT_STREQ(options.BitDepthSource, "game.config");
}

// --- Runtime toggle ---------------------------------------------------------

TEST(PlayerHdrOptions, ToggleFlagBeatsEnvironment)
{
    PlayerHdrEnvironment environment;
    environment.Toggle = "0";
    const PlayerHdrOptions options = Resolve({"--allow-hdr-toggle"}, environment, kProjectDefaults);
    EXPECT_TRUE(options.AllowRuntimeToggle);
}

TEST(PlayerHdrOptions, UnrecognizedToggleValueLeavesTheToggleDisabled)
{
    PlayerHdrEnvironment environment;
    environment.Toggle = "sure";
    const PlayerHdrOptions options = Resolve({}, environment, kProjectDefaults);
    EXPECT_FALSE(options.AllowRuntimeToggle);
}

// --- Value hygiene ----------------------------------------------------------

TEST(PlayerHdrOptions, TrailingCarriageReturnDoesNotRefuseAMode)
{
    const PlayerHdrOptions options = Resolve({}, EnvMode("hdr10\r"), kProjectDefaults);
    EXPECT_EQ(options.Mode, HdrOutputMode::HDR10_PQ);
    EXPECT_STREQ(options.ModeSource, "GE_HDR_MODE");
}

TEST(PlayerHdrOptions, AbsentOrEmptyEnvironmentValueCountsAsUnset)
{
    // A wrapper script that exports the variable with no value must not outrank
    // the launch command. Exercised on the normalizer directly, because Windows
    // cannot deliver an empty value through getenv at all (see the header).
    EXPECT_FALSE(GameEngine::PlayerHdrEnvironmentValue(nullptr).has_value());
    EXPECT_FALSE(GameEngine::PlayerHdrEnvironmentValue("").has_value());

    const std::optional<std::string> set = GameEngine::PlayerHdrEnvironmentValue("hdr10");
    ASSERT_TRUE(set.has_value());
    EXPECT_EQ(*set, "hdr10");
}

TEST(PlayerHdrOptions, DeletedEnvironmentVariableReadsAsUnset)
{
    // The arm the platform CAN represent: _putenv_s(name, "") deletes on Windows.
    // Set a real value first, so the unset assertion cannot pass vacuously.
#ifdef _WIN32
    _putenv_s("GE_HDR_MODE", "hdr10");
#else
    setenv("GE_HDR_MODE", "hdr10", 1);
#endif
    ASSERT_TRUE(GameEngine::ReadPlayerHdrEnvironment().Mode.has_value());

#ifdef _WIN32
    _putenv_s("GE_HDR_MODE", "");
#else
    unsetenv("GE_HDR_MODE");
#endif
    EXPECT_FALSE(GameEngine::ReadPlayerHdrEnvironment().Mode.has_value());
}
