#pragma once

#include "Assets/Packages/PackageCodeModules.h"
#include "Assets/Packages/PackageResolver.h"
#include "Engine/Build/AssetCollector.h"
#include "Engine/Build/CancellableShellProcess.h"
#include "Engine/Build/GameConfig.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine {

namespace NativeScripting
{
class NativeScriptManager;
}

struct BuildSettings
{
    std::string platformName;
    std::vector<std::string> scenes;            // asset-relative paths
    std::string applicationIconPath;            // asset-relative texture path (empty = default)
    std::string renderPipelinePath;             // asset-relative path
    GameConfig playerConfig;
    std::filesystem::path outputDirectory;
    std::filesystem::path projectRoot;          // workspace root
    std::filesystem::path editorSDKPath;        // <Editor>/SDK
    std::filesystem::path runtimeDepsPath;      // <Editor>/ directory (DLLs colocated with Editor)
    // Verified Linux x64 runtime template. Content still passes through this
    // pipeline's asset/package/configuration stages; host runtime libraries
    // must never be copied into a template for a different target.
    std::filesystem::path prebuiltPlayerDirectory;
    std::vector<std::filesystem::path> userCppSources; // custom Player sources; gameplay ships in the native module
    bool compileScripts = true;
    std::string buildConfiguration = "Release";
    std::string cmakeGenerator;                 // e.g. "Visual Studio 17 2022", detected from environment
    std::filesystem::path vcpkgToolchainFile;   // path to vcpkg.cmake toolchain (for find_package resolution)
    std::filesystem::path vcpkgInstalledDir;    // engine build's vcpkg_installed tree, optional: the other
                                                // CRT flavor's runtime DLLs for a cross-flavor package, and
                                                // extra Player link inputs (-DGAMEENGINE_VCPKG_INSTALLED);
                                                // the Player links against the SDK alone without it
    std::string engineVersion;                  // engine version for SDK compatibility check (e.g. "2026.10.0-alpha.4")
};

struct BuildProgress
{
    enum class Stage
    {
        Idle,
        PreparingProject,
        CompilingPlayer,
        CompilingScripts,
        CollectingAssets,
        CopyingAssets,
        Assembling,
        Complete,
        Failed
    };

    Stage currentStage = Stage::Idle;
    float progress = 0.0f;         // 0..1
    std::string statusMessage;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    bool cancelled = false;
};

/// Build context passed to custom steps.
struct BuildContext
{
    const BuildSettings& settings;
    const std::filesystem::path& stagingDir;
    AssetManifest& manifest; // mutable — PreAssetCopy hooks can add/rewrite entries
};

/// Custom build step hook for extensibility.
/// Users can register steps that run at specific phases of the pipeline
/// (e.g., texture compression, localization packaging, platform post-processing).
/// Future: register from C# scripts or project config files.
struct BuildStep
{
    std::string name;
    std::function<bool(BuildContext& ctx)> execute;

    enum class Phase
    {
        PreCompile,
        PostCompile,
        PreAssetCopy,   // receives mutable AssetManifest — baking hooks go here
        PostAssetCopy,
        PrePackage      // platform-specific packaging (macOS .app bundle etc.)
    };
    Phase phase = Phase::PostAssetCopy;
};

/// Orchestrates the complete game build process.
///
/// Execution order (all output goes to a staging dir; renamed to final output on success):
///  0. WaitForNativeModuleBuilds — before anything reads a native module build record
///  1. PreparePlayerProject   — copy template if needed, generate CMakeLists.txt
///  2. hooks: PreCompile
///  3. CompilePlayer          — cmake configure + build
///  4. CompileScripts         — dotnet build
///  5. hooks: PostCompile
///  6. CollectAssets           — produces AssetManifest (NOT a hook, core phase)
///  7. hooks: PreAssetCopy    — receives mutable AssetManifest& (baking hooks go here)
///  8. CopyAssets             — copies files listed in manifest
///  9. CopyEngineShaders      — engine shaders (not GUID-tracked)
/// 10. hooks: PostAssetCopy
/// 11. CopyRuntimeDependencies — Player.exe, DLLs, managed assemblies
///     11b. native user scripts; 11c. package code (native modules + C# assemblies)
/// 12. WriteGameConfig        — emit game.config JSON
/// 13. StageAssetManifest     — the read-only .assetmanifest per mount (project beside
///     Assets/, one per staged package) + Packages/packages.index (Player mount list),
///     then the texture and LOD cooks and every staged asset's parser cook
/// 14. hooks: PrePackage
/// 15. FinalizeOutput         — rename staging/ -> outputDir/ on success
class BuildPipeline
{
public:
    using ProgressCallback = std::function<void(const BuildProgress&)>;

    /// Registers the built-in package-staging step (PreAssetCopy: each
    /// package's Shaders/ folder and runtimeAssets + build validation) through the same
    /// RegisterStep hook external steps use. `nativeModuleBuilds` is the
    /// editor's native module builder that Execute waits for before it reads
    /// any build record; nullptr where no module builds run in this process
    /// (the export then reads the records as they are).
    explicit BuildPipeline(NativeScripting::NativeScriptManager* nativeModuleBuilds);

    bool Execute(const BuildSettings& settings, ProgressCallback progressCb);

    /// Preflight for the packaged C# compile steps: resolve the dotnet
    /// executable via the engine's shared discovery (the staged SDK manifest's
    /// recorded path, then PATH, then common install locations). Returns the
    /// resolved executable, or empty with `outError` set to
    /// DescribeMissingDotnet(). Public + static so the BuildPanel path, the
    /// headless trigger_build path, and tests all exercise the same resolution.
    static std::filesystem::path ResolveDotnetForPackaging(const std::filesystem::path& editorSdkPath,
                                                           std::string& outError);

    /// The loud, actionable preflight error the build reports when no dotnet
    /// SDK resolves — names the fix and everywhere discovery looked.
    static std::string DescribeMissingDotnet(const std::filesystem::path& editorSdkPath);

    /// The vcpkg installed tree's runtime DLL directories for the requested CRT
    /// flavor, in staging search order. Release flavor: {<triplet>/bin}. Debug
    /// flavor: {<triplet>/debug/bin, <triplet>/bin} — ports the overlay
    /// triplets build release-only ship no debug DLLs, so the release bin
    /// trails as a fallback and same-named debug variants shadow it. Empty when
    /// the tree is unset or the flavor's own bin dir is absent (the fallback
    /// never substitutes for the flavor dir). Public + static so packaging and
    /// tests exercise the same resolution.
    static std::vector<std::filesystem::path> FindVcpkgRuntimeBinDirsForFlavor(
        const std::filesystem::path& vcpkgInstalledDir, bool debugCrt);

    /// The file the SDK's GameEngineConfig.cmake writes into a consumer's build directory at
    /// configure (cmake/GameEngineSDK.cmake): the engine configuration it selected and links.
    static constexpr std::string_view kLinkConfigFileName = "GameEngineLinkConfig.txt";

    /// The engine runtime a packaged game ships, or why the export cannot ship one.
    struct PackagedEngineRuntime
    {
        std::filesystem::path Dir; // where Engine and GameEngine.Native ship from
        std::string Refusal;       // empty when Dir is set
    };

    /// The engine runtime for a player that links `linkConfig` (kLinkConfigFileName): the
    /// editor's own directory (`runtimeDepsPath`) when `linkConfig` is `engineConfig`, the
    /// configuration the running engine is built in, whose runtime the SDK does not duplicate;
    /// otherwise SDK/lib/<linkConfig>. Refused when that directory holds no engine runtime, and
    /// when `linkConfig` is not `engineConfig` while the export stages native modules
    /// (`nativeModules` names them, empty for none): they are built against this editor's own
    /// configuration, so their engine classes would lay out differently from the shipped
    /// engine's (#2806).
    static PackagedEngineRuntime ResolvePackagedEngineRuntime(const BuildSettings& settings,
                                                              std::string_view linkConfig,
                                                              std::string_view engineConfig,
                                                              std::string_view nativeModules);

    /// The directories the packaged engine runtime stages from, in search order: the resolved
    /// `engineRuntimeDir` first, then the vcpkg runtime DLLs' source (`crossFlavorVcpkgBinDirs`
    /// when the player's CRT flavor differs from this editor's, else `editorDir`). Empty when
    /// `engineRuntimeDir` was never resolved, so the export fails closed rather than shipping the
    /// editor's own engine.
    static std::vector<std::filesystem::path> EngineRuntimeSearchDirs(
        const std::filesystem::path& engineRuntimeDir,
        const std::vector<std::filesystem::path>& crossFlavorVcpkgBinDirs, const std::filesystem::path& editorDir);

    /// Request cancellation of the current build. Safe to call from any thread.
    void RequestCancel();
    bool IsCancelled() const;

    /// Runs a shell command with cancellation support. Used by editor-hosted build paths.
    ShellProcessResult RunShellCommand(const std::string& command,
                                       std::function<void(const std::string& line)> onLine = nullptr);

    void RegisterStep(BuildStep step);

private:
    bool ExecuteImpl(const BuildSettings& settings);

    // Core phases
    bool PreparePlayerProject(const BuildSettings& s, const std::filesystem::path& stagingDir);
    bool CompilePlayer(const BuildSettings& s);
    // Reads the configuration the player's configure linked and resolves the engine runtime
    // the package ships (ResolvePackagedEngineRuntime), refusing before the player compiles.
    bool ResolvePlayerEngineRuntime(const BuildSettings& s, const std::filesystem::path& playerBuildDir);
    // The native modules this export stages, named for a refusal: the project's native C++
    // user scripts and every package's native runtime module. Empty when there are none.
    std::string DescribeStagedNativeModules(const BuildSettings& s) const;
    bool CompileScripts(const BuildSettings& s, const std::filesystem::path& stagingDir);
    // Regenerates the scripts csproj and resolves it and the dotnet that builds it. True
    // with `outNothingToCompile` when the project has no script project and no C# sources.
    bool PrepareScriptProject(const BuildSettings& s, std::filesystem::path& outProject,
                              std::filesystem::path& outDotnet, bool& outNothingToCompile);
    // The Debug package's managed build of `scriptsProj` into <staging>/Managed.
    bool BuildManagedScripts(const BuildSettings& s, const std::filesystem::path& stagingDir,
                             const std::filesystem::path& scriptsProj, const std::filesystem::path& dotnetExe);
    bool CompileScriptsNativeAOT(const BuildSettings& s,
                                  const std::filesystem::path& stagingDir,
                                  const std::filesystem::path& scriptsProj,
                                  const std::filesystem::path& dotnetExe);
    AssetManifest CollectAssets(const BuildSettings& s);
    bool CopyAssets(const std::filesystem::path& stagingDir, const AssetManifest& manifest);
    bool CopyEngineShaders(const std::filesystem::path& stagingDir);
    bool CopyThirdPartyNotices(const std::filesystem::path& contentRoot, const BuildSettings& s);
    bool CopyRuntimeDependencies(const std::filesystem::path& stagingDir, const BuildSettings& s);
    bool WriteGameConfig(const std::filesystem::path& stagingDir, const BuildSettings& s);
    bool StageAssetManifest(const std::filesystem::path& contentRoot, const AssetManifest& manifest);
    bool CookMeshLODs(const std::filesystem::path& contentRoot, const AssetManifest& manifest);
    void StageTerrainBakes(const std::filesystem::path& contentRoot, const BuildSettings& s,
                           const AssetManifest& manifest);
    bool RunCustomSteps(BuildStep::Phase phase, BuildContext& ctx);
    bool FinalizeOutput(const std::filesystem::path& stagingDir, const std::filesystem::path& outputDir);

    // Package staging (P2). The PreAssetCopy half (asset collection into the
    // mutable manifest + validation) runs as the built-in registered step; the
    // code/index halves are core phases because they need the mac-aware content
    // root and must run after CopyRuntimeDependencies decided the script mode —
    // neither travels through BuildContext.
    bool RunPackageAssetStep(BuildContext& ctx);
    bool StagePackageCode(const std::filesystem::path& contentRoot, const BuildSettings& s);

    // Step 0: blocks this build thread until the editor's native module builds
    // settle, so every later read of a build record (the cross-host and cross-CRT
    // refusals, steps 11b/11c, the macOS bundle assembler) sees the latest build's
    // record. Reports each waited-on module set as progress. False (progress
    // already Failed, the error naming the fix) when cancelled, when the builds
    // were stopped, when play mode holds a finished build or a source change, or
    // when a shipped module's latest build failed.
    bool WaitForNativeModuleBuilds();

    // Progress reporting
    void ReportProgress(BuildProgress::Stage stage, float progress, const std::string& message);
    bool FailIfCancelled();
    ShellProcessResult RunShellCommandImpl(const std::string& command,
                                           std::function<void(const std::string& line)> onLine);

    std::vector<BuildStep> m_CustomSteps;
    ProgressCallback m_ProgressCb;
    BuildProgress m_Progress;
    // Resolved per Execute: the project's enabled/disabled package set (fresh
    // from the on-disk manifest via ResolveBuildTimePackages), its
    // Player-context (runtime-only) code modules, and the session's registered
    // package mounts (for mid-session enable/disable drift attribution).
    PackageResolution m_PackageResolution;
    std::vector<PackageCodeModule> m_PackageRuntimeModules;
    std::vector<std::string> m_SessionMountedPackageAliases;
    // Where the packaged Engine and GameEngine.Native come from: the runtime of the
    // configuration the player links (ResolvePlayerEngineRuntime). Empty for targets
    // that compile no player.
    std::filesystem::path m_EngineRuntimeDir;
    // Cross-CRT-flavor packaging (Windows): when the requested config's CRT flavor
    // differs from the running editor's, the engine's vcpkg runtime DLLs ship from
    // the vcpkg tree's flavor bin dirs (FindVcpkgRuntimeBinDirsForFlavor order)
    // instead of the editor's own directory. Empty for same-flavor builds.
    std::vector<std::filesystem::path> m_CrossFlavorVcpkgBinDirs;
    NativeScripting::NativeScriptManager* m_NativeModuleBuilds = nullptr;
    // One pipeline runs one Execute: the flag only ever goes from false to true,
    // so a cancel that lands before Execute starts is kept.
    std::atomic<bool> m_CancelRequested{false};
    CancellableShellProcess m_ActiveShellProcess;
};

} // namespace GameEngine
