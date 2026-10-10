#pragma once

#include "Assets/AssetRegistry.h"
#include "Scripting/ScriptsConfig.h"

#include <filesystem>
#include <optional>
#include <string>

namespace GameEngine {

// The shipped (packaged) game layout, detected as a POSITIVE signal from what
// the build pipeline stages in the content root: game.config AND the staged
// asset-identity manifest (Assets/.assetmanifest). Dev runs (repo layouts,
// --asset-root project runs) never stage both together, so they never detect
// as packaged. Detection runs in the Player BEFORE engine/script
// initialization so packaged behavior (prebuilt-only scripts, no csproj
// generation, no dotnet, index-based package mounts) is decided up front,
// never inferred mid-flight from which files happen to load.
struct PackagedGameLayout
{
    bool IsPackaged = false;

    // Directory holding game.config + Assets/: PathUtils::InstallContentRootFor
    // the executable directory (the exe dir on Windows/Linux, Contents/Resources
    // inside a mac bundle).
    std::filesystem::path ContentRoot;

    // game.config scriptAssemblyPath resolved against ContentRoot. Empty when
    // the game ships no managed scripts (none authored, or NativeAOT).
    std::filesystem::path PrebuiltScriptAssembly;
    bool PrebuiltScriptAssemblyExists = false;
};

// Detect the packaged layout rooted at `contentRoot`. `scriptAssemblyPath` is
// game.config's scriptAssemblyPath verbatim (may be empty or relative).
PackagedGameLayout DetectPackagedGameLayout(const std::filesystem::path& contentRoot,
                                            const std::string& scriptAssemblyPath);

// The Player's ScriptsConfig for a detected layout. Packaged mode loads
// exactly the staged prebuilt assembly (package assemblies in
// Managed/Packages/ load beside it) and hard-disables every compile and
// project-generation path in ScriptManager. Non-packaged (dev) runs keep the
// CLR off unless a prebuilt assembly is explicitly staged. `useNativeAOT`
// (scripts shipped as a native library) disables the CLR outright.
ScriptsConfig BuildPlayerScriptsConfig(const PackagedGameLayout& layout, bool useNativeAOT);

// True when the Player must register ManagedSystemBridge so C# GameSystems tick
// each frame. Covers BOTH script modes: NativeAOT (CLR off, bridge loads the
// native library) and CoreCLR packaged mode (prebuilt assembly loaded, bridge
// resolves GameSystemRunnerExports). The editor never uses this — there
// PlayModeDriver owns the managed tick. Without the bridge a packaged CoreCLR
// game loads its script assembly (module initializers run) but no GameSystem
// ever receives OnCreate/OnUpdate.
bool PlayerNeedsManagedSystemBridge(const ScriptsConfig& config, bool useNativeAOT);

// The Player's 'editor' asset source for a run whose project mount roots at
// `projectAssetsRoot`, with the runtime-staged engine assets at
// `stagedAssetsRoot`, the install assets root (PathUtils::GetInstallAssetsRoot).
// Three layouts:
//  - distinct staged root: a real second mount (dev --asset-root run; engine
//    shaders/defaults ship in the install assets root, project content elsewhere);
//  - same root, dev layout: an alias-only mount (no scan, no watcher) so
//    `editor:`-pinned paths keep resolving without duplicate scans;
//  - same root, packaged layout (Assets/.assetmanifest): nullopt. Build
//    staging fuses editor-owned content flat into Assets/, and the packaged
//    manifest mount rejects overlapping roots, so a same-root 'editor'
//    source would be refused at registration; callers fall back to the
//    project mount instead.
std::optional<AssetSourceDesc> BuildPlayerEditorSourceDesc(
    const std::filesystem::path& projectAssetsRoot,
    const std::filesystem::path& stagedAssetsRoot);

} // namespace GameEngine
