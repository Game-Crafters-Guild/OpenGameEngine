// P2.2 packaged-mode detection: the shipped layout (game.config +
// Assets/.assetmanifest beside the exe) is the POSITIVE signal that flips the
// Player into packaged script mode, and BuildPlayerScriptsConfig is the single
// place that turns a detected layout into the ScriptsConfig the Player installs
// before engine init. Tested against fake staged dirs.

#include "Engine/Build/PackagedGameLayout.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace GameEngine;

namespace
{

void WriteFile(const fs::path& path, const char* bytes)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

struct LayoutFixture
{
    fs::path Root;

    explicit LayoutFixture(const char* name)
    {
        Root = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(Root, ec);
        fs::create_directories(Root, ec);
    }

    ~LayoutFixture()
    {
        std::error_code ec;
        fs::remove_all(Root, ec);
    }

    // The full staged footprint of a packaged game with managed scripts.
    void StagePackagedGame()
    {
        WriteFile(Root / "game.config", "{}");
        WriteFile(Root / "Assets" / ".assetmanifest", "");
        WriteFile(Root / "Managed" / "GameEngine.Scripts.dll", "fake assembly");
    }
};

} // namespace

TEST(PackagedGameLayout, FullStagedFootprintDetectsPackaged)
{
    LayoutFixture fx("ge_pkglayout_full");
    fx.StagePackagedGame();

    const PackagedGameLayout layout =
        DetectPackagedGameLayout(fx.Root, "Managed/GameEngine.Scripts.dll");
    EXPECT_TRUE(layout.IsPackaged);
    EXPECT_EQ(layout.ContentRoot, fx.Root);
    EXPECT_EQ(layout.PrebuiltScriptAssembly,
              (fx.Root / "Managed" / "GameEngine.Scripts.dll").lexically_normal());
    EXPECT_TRUE(layout.PrebuiltScriptAssemblyExists);
}

TEST(PackagedGameLayout, GameConfigAloneIsNotPackaged)
{
    // A dev run can have a game.config beside the exe; without the staged
    // asset-identity manifest it must never flip into packaged mode.
    LayoutFixture fx("ge_pkglayout_config_only");
    WriteFile(fx.Root / "game.config", "{}");

    EXPECT_FALSE(DetectPackagedGameLayout(fx.Root, "").IsPackaged);
}

TEST(PackagedGameLayout, AssetManifestAloneIsNotPackaged)
{
    LayoutFixture fx("ge_pkglayout_manifest_only");
    WriteFile(fx.Root / "Assets" / ".assetmanifest", "");

    EXPECT_FALSE(DetectPackagedGameLayout(fx.Root, "").IsPackaged);
}

TEST(PackagedGameLayout, MissingConfiguredAssemblyIsResolvedButFlaggedMissing)
{
    // Incomplete package: scriptAssemblyPath configured, DLL never staged. The
    // layout still resolves the path (for the loud error) and stays packaged.
    LayoutFixture fx("ge_pkglayout_missing_dll");
    WriteFile(fx.Root / "game.config", "{}");
    WriteFile(fx.Root / "Assets" / ".assetmanifest", "");

    const PackagedGameLayout layout =
        DetectPackagedGameLayout(fx.Root, "Managed/GameEngine.Scripts.dll");
    EXPECT_TRUE(layout.IsPackaged);
    EXPECT_EQ(layout.PrebuiltScriptAssembly,
              (fx.Root / "Managed" / "GameEngine.Scripts.dll").lexically_normal());
    EXPECT_FALSE(layout.PrebuiltScriptAssemblyExists);
}

TEST(PackagedGameLayout, AbsoluteScriptAssemblyPathIsKeptVerbatim)
{
    LayoutFixture fx("ge_pkglayout_abs_path");
    const fs::path absDll = fx.Root / "Elsewhere" / "Scripts.dll";
    WriteFile(absDll, "fake assembly");

    const PackagedGameLayout layout =
        DetectPackagedGameLayout(fx.Root, absDll.generic_string());
    EXPECT_EQ(layout.PrebuiltScriptAssembly, absDll.lexically_normal());
    EXPECT_TRUE(layout.PrebuiltScriptAssemblyExists);
}

TEST(PackagedGameLayout, PlayerConfigPackagedWithScripts)
{
    LayoutFixture fx("ge_pkglayout_cfg_scripts");
    fx.StagePackagedGame();
    const PackagedGameLayout layout =
        DetectPackagedGameLayout(fx.Root, "Managed/GameEngine.Scripts.dll");

    const ScriptsConfig config = BuildPlayerScriptsConfig(layout, /*useNativeAOT=*/false);
    EXPECT_TRUE(config.packagedMode);
    EXPECT_FALSE(config.disableClr);
    EXPECT_EQ(config.prebuiltAssemblyPath,
              (fx.Root / "Managed" / "GameEngine.Scripts.dll").lexically_normal());
    EXPECT_FALSE(config.enableHotReload);
    EXPECT_FALSE(config.enableAsyncHotReload);
    EXPECT_FALSE(config.enableAutoProjectGeneration);
    EXPECT_FALSE(config.deferInitialLoad);
}

TEST(PackagedGameLayout, PlayerConfigPackagedWithMissingAssemblyKeepsClrOff)
{
    // The missing staged assembly must NOT put ScriptManager on the dev
    // compile path: CLR off, packaged flag still set (assert-level guards).
    LayoutFixture fx("ge_pkglayout_cfg_missing");
    WriteFile(fx.Root / "game.config", "{}");
    WriteFile(fx.Root / "Assets" / ".assetmanifest", "");
    const PackagedGameLayout layout =
        DetectPackagedGameLayout(fx.Root, "Managed/GameEngine.Scripts.dll");

    const ScriptsConfig config = BuildPlayerScriptsConfig(layout, /*useNativeAOT=*/false);
    EXPECT_TRUE(config.packagedMode);
    EXPECT_TRUE(config.disableClr);
    EXPECT_TRUE(config.prebuiltAssemblyPath.empty());
}

TEST(PackagedGameLayout, PlayerConfigNativeAotDisablesClrEvenWithStagedAssembly)
{
    LayoutFixture fx("ge_pkglayout_cfg_aot");
    fx.StagePackagedGame();
    const PackagedGameLayout layout =
        DetectPackagedGameLayout(fx.Root, "Managed/GameEngine.Scripts.dll");

    const ScriptsConfig config = BuildPlayerScriptsConfig(layout, /*useNativeAOT=*/true);
    EXPECT_TRUE(config.disableClr);
    EXPECT_TRUE(config.prebuiltAssemblyPath.empty());
}

TEST(PackagedGameLayout, PlayerConfigDevRunWithoutScriptsDisablesClr)
{
    LayoutFixture fx("ge_pkglayout_cfg_dev");
    const PackagedGameLayout layout = DetectPackagedGameLayout(fx.Root, "");

    const ScriptsConfig config = BuildPlayerScriptsConfig(layout, /*useNativeAOT=*/false);
    EXPECT_FALSE(config.packagedMode);
    EXPECT_TRUE(config.disableClr);
    EXPECT_TRUE(config.prebuiltAssemblyPath.empty());
}

// P4a FINDING 1: the Player must register ManagedSystemBridge in BOTH script
// modes — the packaged CoreCLR game used to load its script assembly (module
// initializers ran) while no GameSystem ever ticked, because registration was
// gated on NativeAOT only.
TEST(PackagedGameLayout, BridgeRegisteredForPackagedCoreClrGame)
{
    LayoutFixture fx("ge_pkglayout_bridge_clr");
    fx.StagePackagedGame();
    const PackagedGameLayout layout =
        DetectPackagedGameLayout(fx.Root, "Managed/GameEngine.Scripts.dll");

    const ScriptsConfig config = BuildPlayerScriptsConfig(layout, /*useNativeAOT=*/false);
    EXPECT_TRUE(PlayerNeedsManagedSystemBridge(config, /*useNativeAOT=*/false));
}

TEST(PackagedGameLayout, BridgeRegisteredForNativeAot)
{
    LayoutFixture fx("ge_pkglayout_bridge_aot");
    fx.StagePackagedGame();
    const PackagedGameLayout layout =
        DetectPackagedGameLayout(fx.Root, "Managed/GameEngine.Scripts.dll");

    const ScriptsConfig config = BuildPlayerScriptsConfig(layout, /*useNativeAOT=*/true);
    EXPECT_TRUE(config.disableClr); // AOT keeps the CLR off...
    EXPECT_TRUE(PlayerNeedsManagedSystemBridge(config, /*useNativeAOT=*/true)); // ...bridge still ticks
}

TEST(PackagedGameLayout, BridgeSkippedWhenNoScriptsCanRun)
{
    // No prebuilt assembly, no AOT library: the CLR stays off and registering
    // the bridge would only log a per-session resolve error.
    LayoutFixture fx("ge_pkglayout_bridge_none");
    const PackagedGameLayout layout = DetectPackagedGameLayout(fx.Root, "");

    const ScriptsConfig config = BuildPlayerScriptsConfig(layout, /*useNativeAOT=*/false);
    EXPECT_FALSE(PlayerNeedsManagedSystemBridge(config, /*useNativeAOT=*/false));
}

// ---------------------------------------------------------------------------
// BuildPlayerEditorSourceDesc — the Player's 'editor' mount decision.
// ---------------------------------------------------------------------------

TEST(PlayerEditorSource, DistinctStagedRootMountsSecondSource)
{
    // Dev --asset-root run: engine assets staged next to the exe, project
    // content elsewhere — a real second mount that scans.
    LayoutFixture fx("ge_editorsrc_distinct");
    const fs::path projectAssets = fx.Root / "project" / "Assets";
    const fs::path stagedAssets = fx.Root / "exe" / "Assets";
    std::error_code ec;
    fs::create_directories(projectAssets, ec);
    fs::create_directories(stagedAssets, ec);

    const auto desc = BuildPlayerEditorSourceDesc(projectAssets, stagedAssets);
    ASSERT_TRUE(desc.has_value());
    EXPECT_EQ(desc->Alias, kAssetSourceAliasEditor);
    EXPECT_EQ(desc->Root, stagedAssets);
    EXPECT_TRUE(desc->RequiresScan);
    EXPECT_FALSE(desc->RegisterFileWatcher);
}

TEST(PlayerEditorSource, SameRootDevRunMountsAliasWithoutScan)
{
    // Same folder on disk with no packaged manifest: keep the alias so
    // `editor:`-pinned paths resolve, but never scan or watch it twice.
    LayoutFixture fx("ge_editorsrc_samedev");
    const fs::path assets = fx.Root / "Assets";
    std::error_code ec;
    fs::create_directories(assets, ec);

    const auto desc = BuildPlayerEditorSourceDesc(assets, assets);
    ASSERT_TRUE(desc.has_value());
    EXPECT_EQ(desc->Root, assets);
    EXPECT_FALSE(desc->RequiresScan);
    EXPECT_FALSE(desc->RegisterFileWatcher);
}

TEST(PlayerEditorSource, PackagedLayoutMountsNoEditorSource)
{
    // Packaged game: the manifest mount at the same root rejects overlapping
    // sources (RejectOverlappingRoots), and editor content is fused into
    // Assets/ at build time — mounting 'editor' would be refused loudly at
    // registration and then warn on every editor-pinned resolution.
    LayoutFixture fx("ge_editorsrc_packaged");
    const fs::path assets = fx.Root / "Assets";
    WriteFile(assets / ".assetmanifest", "");

    EXPECT_FALSE(BuildPlayerEditorSourceDesc(assets, assets).has_value());
}

TEST(PlayerEditorSource, MissingStagedRootFallsBackToProjectRoot)
{
    // No <exe>/Assets staged at all: the same-root alias shape applies.
    LayoutFixture fx("ge_editorsrc_nostaged");
    const fs::path projectAssets = fx.Root / "project" / "Assets";
    std::error_code ec;
    fs::create_directories(projectAssets, ec);

    const auto desc = BuildPlayerEditorSourceDesc(projectAssets, fx.Root / "exe" / "Assets");
    ASSERT_TRUE(desc.has_value());
    EXPECT_EQ(desc->Root, projectAssets);
    EXPECT_FALSE(desc->RequiresScan);
}

TEST(PlayerEditorSource, PackagedRunWithDistinctStagedRootStillMountsIt)
{
    // A packaged manifest at the project root does not suppress a genuinely
    // distinct staged editor root (e.g. --asset-root into packaged content).
    LayoutFixture fx("ge_editorsrc_packaged_distinct");
    const fs::path projectAssets = fx.Root / "game" / "Assets";
    const fs::path stagedAssets = fx.Root / "exe" / "Assets";
    WriteFile(projectAssets / ".assetmanifest", "");
    std::error_code ec;
    fs::create_directories(stagedAssets, ec);

    const auto desc = BuildPlayerEditorSourceDesc(projectAssets, stagedAssets);
    ASSERT_TRUE(desc.has_value());
    EXPECT_EQ(desc->Root, stagedAssets);
}
