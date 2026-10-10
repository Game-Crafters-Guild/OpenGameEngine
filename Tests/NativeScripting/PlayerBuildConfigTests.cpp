// The Player build-configuration contract: the configuration the settings UI
// shows, the configuration the build request carries, the CRT flavour check and
// the NativeAOT gate all read one table (Engine/Build/PlayerBuildConfig.h).
//
// GE_BUILD_CONFIG is $<CONFIG>, handed to this target by CMake — the only
// source that can name the configuration a binary was built in. These tests run
// in whatever configuration they were compiled in and assert the per-config
// truth for that one, so running the suite across configurations covers the
// whole table.

#include "Engine/Build/PlayerBuildConfig.h"

#include <gtest/gtest.h>

#include <string_view>

using GameEngine::DefaultPlayerBuildConfig;
using GameEngine::IsShipOptimizedPlayerBuildConfig;
using GameEngine::IsSupportedPlayerBuildConfig;
using GameEngine::kHostUsesDebugCrt;
using GameEngine::kPlayerBuildConfigs;
using GameEngine::PlayerBuildConfigInfo;
using GameEngine::PlayerBuildConfigUsesDebugCrt;

namespace
{
constexpr std::string_view kTrueConfig = GE_BUILD_CONFIG;

// The configuration name the editor derived from preprocessor state before the
// table existed. It survives here, and only here, as the thing the first test
// proves unsound — it is not a code path any more.
std::string_view LegacyPreprocessorConfigName()
{
#if defined(_DEBUG) || defined(DEBUG)
    return "Debug";
#elif defined(NDEBUG)
    return "Release";
#else
    return "RelWithDebInfo";
#endif
}
} // namespace

// Preprocessor state cannot name a configuration. DebugFast links the release
// CRT, so MSVC auto-defines neither _DEBUG nor NDEBUG, and the build adds no
// /DNDEBUG anywhere — while RelWithDebInfo and MinSizeRel are indistinguishable
// from Release. Three of the five configurations come out wrong.
TEST(PlayerBuildConfig, PreprocessorStateCannotNameTheBuildConfiguration)
{
    const bool cascadeWasCorrect = LegacyPreprocessorConfigName() == kTrueConfig;
    const bool onlyDebugAndReleaseSurvive = (kTrueConfig == "Debug" || kTrueConfig == "Release");
    EXPECT_EQ(cascadeWasCorrect, onlyDebugAndReleaseSurvive)
        << "built as '" << kTrueConfig << "'; preprocessor cascade says '"
        << LegacyPreprocessorConfigName() << "'";
}

// The question the preprocessor CAN answer, in all five configurations — and
// the one that actually constrains the choice, because a player linking the
// other CRT dies at static init on the first cross-module STL access.
TEST(PlayerBuildConfig, HostCrtFlavourIsCorrectInEveryConfiguration)
{
    EXPECT_EQ(kHostUsesDebugCrt, kTrueConfig == "Debug") << "built as '" << kTrueConfig << "'";
}

// The default reaches cmake, dotnet and the Player output path, so it has to be
// a configuration the generated Player project actually declares.
TEST(PlayerBuildConfig, DefaultIsBuildableAndMatchesHostCrtFlavour)
{
    EXPECT_TRUE(IsSupportedPlayerBuildConfig(DefaultPlayerBuildConfig()));
    EXPECT_EQ(PlayerBuildConfigUsesDebugCrt(DefaultPlayerBuildConfig()), kHostUsesDebugCrt);
    EXPECT_EQ(IsShipOptimizedPlayerBuildConfig(DefaultPlayerBuildConfig()), !kHostUsesDebugCrt);
}

// The default is the most ship-optimized configuration on this editor's CRT
// flavour. The cascade it replaced answered "RelWithDebInfo" from a DebugFast
// editor — release-CRT, buildable and ship-optimized, so every other assertion
// here passes on it — while the settings UI displayed "Release". That one
// configuration is where the panel and the build disagreed.
TEST(PlayerBuildConfig, DefaultIsTheMostShipOptimizedConfigurationForThisHost)
{
    EXPECT_EQ(DefaultPlayerBuildConfig(),
              kHostUsesDebugCrt ? std::string_view("Debug") : std::string_view("Release"));
    if (kTrueConfig == "DebugFast")
        EXPECT_NE(DefaultPlayerBuildConfig(), LegacyPreprocessorConfigName());
}

// Substituting the editor's own truthful configuration name into the build
// request is the naive label fix, and it is unsafe: "DebugFast" is what a
// daily-driver editor honestly is, and the generated Player project has no such
// configuration (cmake --build --config DebugFast fails with MSB8013).
TEST(PlayerBuildConfig, EditorsOwnConfigurationIsNotAlwaysAPlayerConfiguration)
{
    EXPECT_FALSE(IsSupportedPlayerBuildConfig("DebugFast"));
    if (!IsSupportedPlayerBuildConfig(kTrueConfig))
        EXPECT_NE(kTrueConfig, DefaultPlayerBuildConfig()) << "built as '" << kTrueConfig << "'";
}

// The NativeAOT gate keys on the shipping-artifact property, not on the name
// "Release". The exact string test it replaced silently shipped loose managed
// assemblies — plus a runtime dependency a Release package does not carry —
// from every other shipping configuration.
TEST(PlayerBuildConfig, ShipOptimizedCoversEveryShippingConfiguration)
{
    for (const PlayerBuildConfigInfo& info : kPlayerBuildConfigs)
        EXPECT_EQ(IsShipOptimizedPlayerBuildConfig(info.Name), info.Name != "Debug") << info.Name;

    EXPECT_TRUE(IsShipOptimizedPlayerBuildConfig("Release"));
    EXPECT_TRUE(IsShipOptimizedPlayerBuildConfig("RelWithDebInfo"));
    EXPECT_TRUE(IsShipOptimizedPlayerBuildConfig("MinSizeRel"));
    EXPECT_FALSE(IsShipOptimizedPlayerBuildConfig("Debug"));
}

// Only Debug links /MDd. The SDK runtime-dir search runs this over ENGINE
// configuration names too, where "DebugFast" must answer release-CRT.
TEST(PlayerBuildConfig, OnlyDebugUsesTheDebugCrt)
{
    EXPECT_TRUE(PlayerBuildConfigUsesDebugCrt("Debug"));
    for (std::string_view name : {"Release", "RelWithDebInfo", "MinSizeRel", "DebugFast"})
        EXPECT_FALSE(PlayerBuildConfigUsesDebugCrt(name)) << name;
}

// A name the Player project cannot build answers no to everything, so a typo in
// the free-text settings field can never be read as "debug CRT" or "ship this
// AOT" on its way to the refusal.
TEST(PlayerBuildConfig, UnsupportedNamesAnswerNoToEveryProperty)
{
    for (std::string_view name : {"DebugFast", "", "release", "RELEASE", "Retail", "Shipping"})
    {
        EXPECT_FALSE(IsSupportedPlayerBuildConfig(name)) << '\'' << name << '\'';
        EXPECT_FALSE(PlayerBuildConfigUsesDebugCrt(name)) << '\'' << name << '\'';
        EXPECT_FALSE(IsShipOptimizedPlayerBuildConfig(name)) << '\'' << name << '\'';
    }
}

TEST(PlayerBuildConfig, OnlyDebugConfigurationsShipSeparateSymbols)
{
    EXPECT_TRUE(GameEngine::PlayerBuildConfigIncludesDebugSymbols("Debug"));
    EXPECT_TRUE(GameEngine::PlayerBuildConfigIncludesDebugSymbols("RelWithDebInfo"));
    EXPECT_FALSE(GameEngine::PlayerBuildConfigIncludesDebugSymbols("Release"));
    EXPECT_FALSE(GameEngine::PlayerBuildConfigIncludesDebugSymbols("MinSizeRel"));
    EXPECT_FALSE(GameEngine::PlayerBuildConfigIncludesDebugSymbols("Unknown"));
}
