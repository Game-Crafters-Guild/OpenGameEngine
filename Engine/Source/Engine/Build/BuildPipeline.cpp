#include "Engine/Build/BuildPipeline.h"
#include "Engine/Build/AppIconGenerator.h"
#include "Engine/Build/BuildHost.h"
#include "Engine/Build/DirectoryCopy.h"
#include "Engine/Build/LinuxRuntimeTemplate.h"
#include "Engine/Build/MacBundleAssembler.h"
#include "Engine/Build/NativeAotProject.h"
#include "Engine/Build/NativeAotPublish.h"
#include "Engine/Build/NativeScriptStaging.h"
#include "Engine/Build/OutputPromotion.h"
#include "Engine/Build/PackageBuildStaging.h"
#include "Engine/Build/PlayerBuildConfig.h"
#include "Engine/Build/TerrainPageExport.h"
#include "Engine/Build/TexturePackageCook.h"
#include "Engine/Build/PlayerSourcePreparation.h"
#include "Engine/Build/RuntimeDependencyStaging.h"
#include "Engine/Build/StagedAssetCook.h"
#include "Scripting/ScriptTargetFramework.h"
#include "Engine/Build/CancellableShellProcess.h"
#include "Engine/GameUI/UIScaleProjectSettings.h"
#include "Assets/Packages/PackagesIndex.h"
#include "Scripting/NativeAotScriptsLibrary.h"
#include "Scripting/PathResolver.h"
#include "Scripting/ScriptManager.h"
#include "NativeScripting/BuildCacheRecord.h" // ComputeEngineAbiDigest
#include "NativeScripting/NativeBuildConfig.h"
#include "NativeScripting/NativeScriptManager.h" // MsvcToolchain env for dotnet publish
#include "NativeScripting/SdkManifest.h"
#include "Assets/AssetManager.h"
#include "Assets/FbxLoaderOptions.h"
#include "Assets/LodAssetSettings.h"
#include "Assets/RuntimeAssetMetadata.h"
#include "Assets/MeshLODCache.h"
#include "TerrainECS/TerrainBakeCache.h"
#include "Assets/MeshLODGenerator.h"
#include "Assets/ModelAsset.h"
#include "Core/Application.h" // PathUtils
#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "AssetDatabase/AssetRecord.h"
#include "AssetCore/PathNormalization.h"

#include <nlohmann/json.hpp>
#include <cmath>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h> // NativeAOT export-table probe (CompileScriptsNativeAOT)
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <unordered_map>
#include <map>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace GameEngine {

static bool ProjectHasCSharpSources(const fs::path& assetsRoot);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool CopyDirectoryRecursive(const fs::path& src,
                                   const fs::path& dst,
                                   const std::function<bool()>& shouldCancel = nullptr)
{
    if (!fs::is_directory(src))
        return true;

    fs::create_directories(dst);
    for (const auto& entry : fs::recursive_directory_iterator(src))
    {
        if (shouldCancel && shouldCancel())
            return false;

        const auto rel = fs::relative(entry.path(), src);
        const auto target = dst / rel;
        if (entry.is_directory())
            fs::create_directories(target);
        else
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
    }
    return true;
}

// True when this build should produce a macOS .app via the prebuilt-inject path.
// Only on a macOS host — the bundle assembly needs ditto/codesign/install_name_tool
// (cross-host Mac builds are unsupported and rejected up front in ExecuteImpl).
static bool IsMacBundleTarget(const BuildSettings& s)
{
#if defined(__APPLE__)
    return s.platformName == "Mac";
#else
    (void)s;
    return false;
#endif
}

// The identity of the engine this package will ship, computed from the staged SDK
// exactly the way the editor's user-script builds compute it (LoadSdkManifest feeds
// both). `packageDefines` must be the same package-provided define set the editor
// appended to that module's build config (union of enabled-package defines for the
// project module, the module's own effective defines for a package module) —
// compile definitions are digest inputs, so recomputing with manifest-only defines
// falsely fails the C2 staleness gate for every defines-bearing package. Empty
// when the SDK manifest is absent — the native-script staging steps then refuse
// to package a native module they cannot validate.
static std::string ComputeCurrentEngineAbiDigest(const BuildSettings& s,
                                                 const std::vector<std::string>& packageDefines)
{
    NativeScripting::NativeBuildConfig cfg;
    std::string manifestErr;
    if (!NativeScripting::LoadSdkManifest(s.editorSDKPath, cfg, manifestErr))
    {
        Logger::Log::Warning("Build: no SDK manifest ({}); native user scripts cannot be packaged", manifestErr);
        return {};
    }
    NativeScripting::AppendPackageDefines(cfg, packageDefines);
    // Packaged builds ship RUNTIME modules only, and the editor stamps their
    // build records with the EditorSDK interface cleared — the editor's staged
    // manifest carries editorimportlib, so hashing cfg directly would fold the
    // EditorSDK identity in and fail the C2 gate for every fresh build.
    return NativeScripting::ComputeRuntimeEngineAbiDigest(std::move(cfg));
}

// Union of enabled AND disabled package defines — the digest variant the C2
// gate uses to recognize "the record was stamped while a now-disabled package
// was still enabled" instead of misreporting genuine engine-ABI staleness.
static std::vector<std::string> CollectEnabledAndDisabledPackageDefines(
    const PackageResolution& resolution)
{
    std::vector<std::string> defines = CollectAllPackageDefines(resolution);
    for (const ResolvedPackage& package : resolution.Disabled)
    {
        std::string define = PackageDefine(package.Manifest.Name);
        if (std::find(defines.begin(), defines.end(), define) == defines.end())
            defines.push_back(std::move(define));
    }
    return defines;
}

// The digest variants for the C2 staleness gate (see EngineAbiDigestSet).
static EngineAbiDigestSet ComputeEngineAbiDigestSet(const BuildSettings& s,
                                                    const std::vector<std::string>& packageDefines,
                                                    const std::vector<std::string>& allPackageDefines)
{
    EngineAbiDigestSet digests;
    digests.Current = ComputeCurrentEngineAbiDigest(s, packageDefines);
    digests.WithoutPackageDefines = ComputeCurrentEngineAbiDigest(s, {});
    if (allPackageDefines != packageDefines)
        digests.WithAllPackageDefines = ComputeCurrentEngineAbiDigest(s, allPackageDefines);
    return digests;
}

// The engine runtime the packaged game loads beside its executable: Engine and the
// GameEngine.Native shim, under this platform's shared-library names.
static bool HoldsEngineRuntime(const fs::path& dir)
{
#if defined(_WIN32)
    constexpr const char* kRuntimeFiles[] = {"Engine.dll", "GameEngine.Native.dll"};
#elif defined(__APPLE__)
    constexpr const char* kRuntimeFiles[] = {"libEngine.dylib", "libGameEngine.Native.dylib"};
#else
    constexpr const char* kRuntimeFiles[] = {"libEngine.so", "libGameEngine.Native.so"};
#endif
    std::error_code ec;
    for (const char* file : kRuntimeFiles)
    {
        if (!fs::exists(dir / file, ec))
            return false;
    }
    return true;
}

BuildPipeline::PackagedEngineRuntime BuildPipeline::ResolvePackagedEngineRuntime(const BuildSettings& settings,
                                                                                 std::string_view linkConfig,
                                                                                 std::string_view engineConfig,
                                                                                 std::string_view nativeModules)
{
    const std::string requested = "Build configuration '" + settings.buildConfiguration + "'";
    if (linkConfig.empty())
        return {{}, requested + " links no engine configuration the SDK recorded (" + std::string(kLinkConfigFileName) +
                        " in the player's build directory). Rebuild this editor so its SDK's GameEngineConfig.cmake "
                        "records it."};
    const std::string linked(linkConfig);
    if (linkConfig != engineConfig && !nativeModules.empty())
    {
        // Only a configuration a player can be built in can be requested; a DebugFast editor's
        // own configuration is not one, so it is offered only when it is.
        const std::string own(engineConfig);
        const std::string requestOwn =
            IsSupportedPlayerBuildConfig(engineConfig)
                ? "Set build.platform." + settings.platformName + ".buildConfig to '" + own + "', "
                : std::string();
        return {{}, requested + " links the SDK's '" + linked + "' engine, but this editor runs the '" + own +
                        "' engine and builds " + std::string(nativeModules) +
                        " against it, so they would load beside an engine whose classes lay out differently. " +
                        requestOwn + (requestOwn.empty() ? "Export" : "export") + " from an editor built in '" +
                        linked + "', or remove the native module from this export (#2806 tracks building it for '" +
                        linked + "')."};
    }
    const fs::path dir =
        linkConfig == engineConfig ? settings.runtimeDepsPath : settings.editorSDKPath / "lib" / linked;
    if (!HoldsEngineRuntime(dir))
        return {{}, requested + " links the SDK's '" + linked + "' engine, but " + dir.generic_string() +
                        " holds no engine runtime (Engine and GameEngine.Native). Build the engine in '" + linked +
                        "' and rebuild this editor so its SDK stages that runtime."};
    return {dir, {}};
}

std::vector<fs::path> BuildPipeline::EngineRuntimeSearchDirs(const fs::path& engineRuntimeDir,
                                                             const std::vector<fs::path>& crossFlavorVcpkgBinDirs,
                                                             const fs::path& editorDir)
{
    if (engineRuntimeDir.empty())
        return {};
    std::vector<fs::path> dirs{engineRuntimeDir};
    if (crossFlavorVcpkgBinDirs.empty())
        dirs.push_back(editorDir);
    else
        dirs.insert(dirs.end(), crossFlavorVcpkgBinDirs.begin(), crossFlavorVcpkgBinDirs.end());
    return dirs;
}

std::string BuildPipeline::DescribeStagedNativeModules(const BuildSettings& s) const
{
    std::string modules;
    std::error_code ec;
    if (fs::exists(s.projectRoot / ".Cache" / "NativeScripts" / "build" / "last_build.txt", ec) ||
        fs::exists(s.projectRoot / "NativeScripts" / "build" / "last_build.txt", ec))
        modules = "the project's native C++ user scripts";
    for (const PackageCodeModule& module : m_PackageRuntimeModules)
    {
        if (module.Lang != PackageModuleRecord::ModuleLang::Cpp)
            continue;
        const std::string moduleName = "package '" + module.PackageName + "' (native runtime module)";
        modules = modules.empty() ? moduleName : (modules + " and " + moduleName);
        break;
    }
    return modules;
}

bool BuildPipeline::ResolvePlayerEngineRuntime(const BuildSettings& s, const fs::path& playerBuildDir)
{
    std::string linkConfig;
    std::ifstream(playerBuildDir / kLinkConfigFileName) >> linkConfig;
    const PackagedEngineRuntime runtime =
        ResolvePackagedEngineRuntime(s, linkConfig, GE_ENGINE_BUILD_CONFIG, DescribeStagedNativeModules(s));
    if (!runtime.Refusal.empty())
    {
        m_Progress.errors.push_back(runtime.Refusal);
        return false;
    }
    m_EngineRuntimeDir = runtime.Dir;
    if (linkConfig != GE_ENGINE_BUILD_CONFIG)
        Logger::Log::Info("Build: the player links the SDK's '{}' engine; its runtime ships from '{}' "
                          "(staged when that config was last built; rebuild it if stale)",
                          linkConfig, m_EngineRuntimeDir.string());
    return true;
}

std::vector<fs::path> BuildPipeline::FindVcpkgRuntimeBinDirsForFlavor(const fs::path& vcpkgInstalledDir,
                                                                      bool debugCrt)
{
    std::error_code ec;
    if (vcpkgInstalledDir.empty() || !fs::is_directory(vcpkgInstalledDir, ec))
        return {};
    for (const auto& entry : fs::directory_iterator(vcpkgInstalledDir, ec))
    {
        if (!entry.is_directory() || entry.path().filename() == "vcpkg")
            continue;
        const fs::path flavorBin =
            debugCrt ? (entry.path() / "debug" / "bin") : (entry.path() / "bin");
        if (!fs::is_directory(flavorBin, ec))
            continue;
        std::vector<fs::path> dirs{flavorBin};
        // Ports the overlay triplets build release-only (cmake/triplets) install
        // no debug DLLs at all: the debug flavor falls back to the release bin,
        // trailing so same-named debug variants keep shadowing it. The release
        // flavor gets no debug fallback — a debug-first candidate name
        // (freetyped, zd, …) must never pull a debug DLL into a release package.
        if (debugCrt)
        {
            const fs::path releaseBin = entry.path() / "bin";
            if (fs::is_directory(releaseBin, ec))
                dirs.push_back(releaseBin);
        }
        return dirs;
    }
    return {};
}

static fs::path ResolveThirdPartyNoticesSource(const BuildSettings& s)
{
    std::error_code ec;
    const std::array<fs::path, 4> candidates{
        s.runtimeDepsPath / "ThirdPartyNotices",
        s.runtimeDepsPath.parent_path() / "Resources" / "ThirdPartyNotices",
        s.editorSDKPath / "ThirdPartyNotices",
        s.editorSDKPath.parent_path() / "ThirdPartyNotices",
    };

    for (const fs::path& candidate : candidates)
    {
        if (!candidate.empty() && fs::is_directory(candidate, ec))
            return candidate;
        ec.clear();
    }

    return {};
}

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------

void BuildPipeline::ReportProgress(BuildProgress::Stage stage, float progress, const std::string& message)
{
    m_Progress.currentStage = stage;
    m_Progress.progress = progress;
    m_Progress.statusMessage = message;
    if (m_ProgressCb)
        m_ProgressCb(m_Progress);
    Logger::Log::Info("Build: [{}] {}", progress * 100.0f, message);
}

// ---------------------------------------------------------------------------
// Custom steps
// ---------------------------------------------------------------------------

BuildPipeline::BuildPipeline(NativeScripting::NativeScriptManager* nativeModuleBuilds)
    : m_NativeModuleBuilds(nativeModuleBuilds)
{
    // Built-in package staging phase, registered through the same hook external
    // steps use. PreAssetCopy fits it exactly: the step receives the collected
    // manifest MUTABLE, appends each enabled package's Shaders/ folder and runtimeAssets, and runs
    // the loud build validation before a single asset is copied.
    BuildStep packageStep;
    packageStep.name = "PackageAssetStaging";
    packageStep.phase = BuildStep::Phase::PreAssetCopy;
    packageStep.execute = [this](BuildContext& ctx) { return RunPackageAssetStep(ctx); };
    RegisterStep(std::move(packageStep));
}

void BuildPipeline::RegisterStep(BuildStep step)
{
    m_CustomSteps.push_back(std::move(step));
}

void BuildPipeline::RequestCancel()
{
    m_CancelRequested.store(true);
    m_ActiveShellProcess.Kill();
}

bool BuildPipeline::IsCancelled() const
{
    return m_CancelRequested.load();
}

bool BuildPipeline::FailIfCancelled()
{
    if (!m_CancelRequested.load())
        return false;

    m_Progress.cancelled = true;
    ReportProgress(BuildProgress::Stage::Failed, m_Progress.progress, "Build cancelled");
    return true;
}

ShellProcessResult BuildPipeline::RunShellCommand(const std::string& command,
                                                  std::function<void(const std::string& line)> onLine)
{
    return RunShellCommandImpl(command, std::move(onLine));
}

ShellProcessResult BuildPipeline::RunShellCommandImpl(const std::string& command,
                                                     std::function<void(const std::string& line)> onLine)
{
    return m_ActiveShellProcess.Run(
        command,
        [this]() { return m_CancelRequested.load(); },
        std::move(onLine));
}

bool BuildPipeline::RunCustomSteps(BuildStep::Phase phase, BuildContext& ctx)
{
    for (const auto& step : m_CustomSteps)
    {
        if (step.phase != phase)
            continue;
        if (FailIfCancelled())
            return false;

        Logger::Log::Info("Build: Running custom step '{}'", step.name);
        if (!step.execute(ctx))
        {
            m_Progress.errors.push_back("Custom step '" + step.name + "' failed");
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------

bool BuildPipeline::Execute(const BuildSettings& settings, ProgressCallback progressCb)
{
    m_ProgressCb = std::move(progressCb);
    m_Progress = BuildProgress{};

    try
    {
        return ExecuteImpl(settings);
    }
    catch (const std::exception& e)
    {
        m_Progress.errors.push_back(std::string("Exception during build: ") + e.what());
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Build failed with exception");
        return false;
    }
    catch (...)
    {
        m_Progress.errors.push_back("Unknown exception during build");
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Build failed with unknown exception");
        return false;
    }
}

bool BuildPipeline::ExecuteImpl(const BuildSettings& settings)
{
    // A cancel requested before Execute (the Build panel closing right after it
    // started this build) stops here, before the wait below can block on Ticks that
    // the closing editor no longer runs.
    if (FailIfCancelled())
        return false;

    // 0. The editor builds native modules (project and packages) asynchronously and
    // writes each build record when the build's load completes. Every record read
    // below (the cross-host and cross-CRT refusals, steps 11b/11c, the bundle
    // assembler) comes after this wait, so none of them sees a build in flight.
    if (!WaitForNativeModuleBuilds())
        return false;

    const fs::path normalizedOutput = fs::weakly_canonical(settings.outputDirectory);
    const fs::path projectFromOutput = fs::weakly_canonical(settings.projectRoot).lexically_relative(normalizedOutput);
    if (settings.outputDirectory.empty() || settings.projectRoot.empty() ||
        (!projectFromOutput.empty() && *projectFromOutput.begin() != ".."))
    {
        m_Progress.errors.push_back("Build output must be a separate directory, not the project root or its ancestor");
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Unsafe build output directory");
        return false;
    }
    const bool prebuiltLinux = !settings.prebuiltPlayerDirectory.empty();
    if (prebuiltLinux)
    {
        const auto outputFromTemplate = normalizedOutput.lexically_relative(fs::weakly_canonical(settings.prebuiltPlayerDirectory));
        if (!outputFromTemplate.empty() && *outputFromTemplate.begin() != "..")
        {
            m_Progress.errors.push_back("Template input and export output must not overlap");
            ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Unsafe template output directory");
            return false;
        }
        if ((settings.platformName != "Steam" && settings.platformName != "Linux") ||
            !IsLinuxX64Elf(settings.prebuiltPlayerDirectory / "Player"))
        {
            m_Progress.errors.push_back("A Linux runtime template requires a Linux/Steam target and an x64 ELF Player");
            ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Invalid runtime template target");
            return false;
        }
    }
#if defined(__linux__) && defined(__x86_64__)
    const bool crossHostTemplate = false;
#else
    const bool crossHostTemplate = prebuiltLinux;
#endif

    // Resolve the project's package set once for the whole build — the single
    // fresh snapshot every consumer reads: asset collection, validation, code
    // staging, defines/ABI digests, and the shipped packages.index. The
    // session's registered package mounts are captured alongside so the
    // PreAssetCopy validation can attribute mid-session enable/disable drift
    // (mounts only update on project open). Player context — Editor-kind
    // modules never ship.
    m_SessionMountedPackageAliases.clear();
    for (const AssetSourceDesc& source :
         EngineCore::GetInstance().GetAssetManager().GetRegisteredSources())
    {
        if (source.Alias != kAssetSourceAliasProject && source.Alias != kAssetSourceAliasEditor)
            m_SessionMountedPackageAliases.push_back(source.Alias);
    }
    m_PackageResolution =
        ResolveBuildTimePackages(settings.projectRoot, m_SessionMountedPackageAliases);
    m_PackageRuntimeModules =
        CollectPackageCodeModules(m_PackageResolution, /*editorContext=*/false);

    // A native template is a runtime, not a cross-compiler. Refuse projects
    // needing target code instead of dropping their gameplay or shipping the
    // running editor's Mach-O/PE modules in a Linux game.
    if (crossHostTemplate &&
        (ProjectHasCSharpSources(settings.projectRoot / "Assets") ||
         !settings.userCppSources.empty() ||
         !m_PackageRuntimeModules.empty() ||
         fs::exists(settings.projectRoot / ".Cache" / "NativeScripts" / "build" / "last_build.txt") ||
         fs::exists(settings.projectRoot / "NativeScripts" / "build" / "last_build.txt")))
    {
        m_Progress.errors.push_back(
            "This Linux export contains gameplay code. Build it with the Linux x64 editor/SDK "
            "so C#, native scripts and package modules match the target, then prepare/upload "
            "that output with publish_game.py from any host. A runtime template alone cannot compile target gameplay.");
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Linux gameplay requires a Linux x64 build worker");
        return false;
    }

    const BuildHost host = CurrentBuildHost();
    if (!prebuiltLinux && !BuildHostCompilesTarget(host, settings.platformName))
    {
        m_Progress.errors.push_back(std::string(UnsupportedBuildTargetMessage(host)));
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Build host does not support the selected target");
        return false;
    }

    const fs::path stagingDir = fs::path(settings.outputDirectory.string() + ".staging");

    // Clean previous staging directory.
    std::error_code ec;
    fs::remove_all(stagingDir, ec);
    fs::create_directories(stagingDir, ec);

    // The configuration is free text in the settings UI, and it reaches cmake,
    // dotnet and the Player output path. A name the generated Player project
    // does not declare fails deep inside the build with MSBuild's MSB8013 —
    // refuse it here instead, naming what is buildable.
    if (!IsSupportedPlayerBuildConfig(settings.buildConfiguration))
    {
        std::string supported;
        for (const PlayerBuildConfigInfo& info : kPlayerBuildConfigs)
        {
            if (!supported.empty())
                supported += ", ";
            supported += std::string(info.Name);
        }
        m_Progress.errors.push_back(
            "Build configuration '" + settings.buildConfiguration +
            "' is not one the packaged Player can be built in (" + supported +
            "). The engine's own 'DebugFast' is an editor configuration and does not exist in the "
            "generated Player project. Set build.platform." + settings.platformName +
            ".buildConfig to one of those configurations, or clear it to use the default (" +
            std::string(DefaultPlayerBuildConfig()) + ").");
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Unsupported build configuration");
        return false;
    }

    // The packaged game's engine runtime is the one its player links, which the
    // player's configure records (ResolvePlayerEngineRuntime, in CompilePlayer).
    m_EngineRuntimeDir.clear();
    m_CrossFlavorVcpkgBinDirs.clear();

#if defined(_WIN32)
    // The engine's vcpkg runtime DLLs must match the PLAYER's CRT flavor. Only
    // "Debug" uses the debug CRT (/MDd, _ITERATOR_DEBUG_LEVEL=2); a mixed-CRT
    // player links cleanly — LNK2038 never crosses a DLL import boundary — then
    // dies at static init on the first cross-module STL access
    // (AutoComponentRegistrar -> registry map find with mismatched
    // std::unordered_map layouts). Same flavor as this editor: the editor's own
    // directory carries them. Other flavor: the vcpkg tree's per-flavor runtime
    // DLLs, refusing, naming exactly what is missing, only when that flavor is
    // genuinely absent.
    {
#if defined(_DEBUG)
        const bool editorDebugCrt = true;
#else
        const bool editorDebugCrt = false;
#endif
        const bool playerDebugCrt = PlayerBuildConfigUsesDebugCrt(settings.buildConfiguration);
        if (!prebuiltLinux && playerDebugCrt != editorDebugCrt)
        {
            const std::string flavorName = playerDebugCrt ? "debug-CRT (/MDd)" : "release-CRT (/MD)";
            const std::vector<fs::path> vcpkgBinDirs =
                FindVcpkgRuntimeBinDirsForFlavor(settings.vcpkgInstalledDir, playerDebugCrt);

            const std::string nativeModuleBlocker = DescribeStagedNativeModules(settings);

            std::string refusal;
            if (vcpkgBinDirs.empty())
            {
                refusal = std::string("the vcpkg installed tree has no ") +
                          (playerDebugCrt ? "<triplet>/debug/bin" : "<triplet>/bin") +
                          " runtime DLLs for that flavor (build.vcpkgInstalledDir = '" +
                          settings.vcpkgInstalledDir.string() +
                          "'; set it in the project's .Editor/ProjectSettings.json to the engine build's "
                          "vcpkg_installed directory)";
            }
            else if (!nativeModuleBlocker.empty())
            {
                refusal = "native modules build in the editor's own CRT flavor and cannot cross it: " +
                          nativeModuleBlocker +
                          ". Package in a config matching this editor (" +
                          std::string(editorDebugCrt ? "Debug" : "a release-CRT config") +
                          "), or disable the package(s) for this project";
            }
            if (!refusal.empty())
            {
                m_Progress.errors.push_back(
                    "Build configuration '" + settings.buildConfiguration + "' needs the " + flavorName +
                    " engine runtime, but this editor runs the other flavor and " + refusal +
                    ". Alternatively set build.platform." + settings.platformName +
                    ".buildConfig to a config matching this editor's flavor.");
                ReportProgress(BuildProgress::Stage::Failed, 0.0f,
                               "Requested CRT flavor is not available for packaging");
                return false;
            }

            m_CrossFlavorVcpkgBinDirs = vcpkgBinDirs;
            std::string vcpkgDirList;
            for (const fs::path& dir : vcpkgBinDirs)
            {
                if (!vcpkgDirList.empty())
                    vcpkgDirList += "', '";
                vcpkgDirList += dir.string();
            }
            Logger::Log::Info("Build: cross-flavor package — vcpkg runtime from '{}'", vcpkgDirList);
        }
    }
#endif

    // macOS ships a prebuilt-inject .app: the Player binary, MoltenVK, Frameworks
    // and engine ABIs all come from the SDK template, and the project's content is
    // injected into a copy of it (no per-game recompile). For that target, assets
    // and game.config are written straight into the bundle's Contents/Resources,
    // the content root the Player resolves from its Contents/MacOS (PathUtils::
    // InstallContentRootFor); otherwise they go flat into the staging dir.
    const bool macBundle = IsMacBundleTarget(settings);
    // Filesystem-safe .app folder name: the display name (CFBundleName, game.config)
    // keeps the raw value, but the bundle directory + the fs::rename/remove_all of the
    // output must never contain path separators or leading dots (a malicious/typo'd game
    // name like "../x" or "a/b" must not escape the output dir).
    std::string gameName = PathUtils::SanitizeForFolderName(settings.playerConfig.gameName);
    // SanitizeForFolderName scrubs separators/illegal chars but keeps dots; strip any
    // leading dots so the name can't begin a path escape ("../x") of the output dir.
    while (!gameName.empty() && gameName.front() == '.')
        gameName.erase(gameName.begin());
    if (gameName.empty())
        gameName = "Game";
    const fs::path bundlePath = stagingDir / (gameName + ".app");
    const fs::path contentRoot =
        macBundle ? PathUtils::InstallContentRootFor(bundlePath / "Contents" / "MacOS") : stagingDir;

    // Helper: check for cancellation between phases.
    auto checkCancel = [this]() -> bool {
        return FailIfCancelled();
    };

    // Build a placeholder manifest for early hook phases.
    AssetManifest manifest;
    BuildContext ctx{settings, stagingDir, manifest};

    // 1. Prepare player project (Windows/Linux) OR copy the prebuilt .app template (macOS).
    ReportProgress(BuildProgress::Stage::PreparingProject, 0.05f, "Preparing player project...");
    if (FailIfCancelled())
        return false;
    if (prebuiltLinux)
    {
        // Copy runtime only. Project content is collected below, through the
        // exact same hooks, GUID manifests and package index as native builds.
        const bool includeSymbols = PlayerBuildConfigIncludesDebugSymbols(settings.buildConfiguration);
        if (!CopyDirectoryKeepingLinks(settings.prebuiltPlayerDirectory, stagingDir, ec,
            checkCancel, [includeSymbols](const fs::path& relative) {
                return IsRuntimeTemplateGamePayload(*relative.begin()) ||
                    (!includeSymbols && IsRuntimeTemplateDebugSymbol(relative));
            }))
        {
            if (ec)
            {
                m_Progress.errors.push_back("Could not copy runtime template: " + ec.message());
                ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Failed to copy runtime template");
            }
            return false;
        }
        fs::remove(stagingDir / "Managed" / ScriptingPaths::kScriptsAssemblyFileName, ec);
    }
    else if (macBundle)
    {
        const fs::path sdkTemplate = settings.editorSDKPath / "templates" / "Player.app";
        if (!MacBundleAssembler::PrepareBundle(sdkTemplate, bundlePath, m_Progress.errors))
        {
            ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Failed to prepare app bundle from template");
            return false;
        }
    }
    else if (!PreparePlayerProject(settings, stagingDir))
    {
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Failed to prepare player project");
        return false;
    }

    // 2. PreCompile hooks
    if (!RunCustomSteps(BuildStep::Phase::PreCompile, ctx))
    {
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "PreCompile hook failed");
        return false;
    }

    if (checkCancel()) return false;

    // 3. Compile player (skipped for macOS — the prebuilt template IS the player binary).
    if (!macBundle && !prebuiltLinux)
    {
        ReportProgress(BuildProgress::Stage::CompilingPlayer, 0.15f, "Compiling player...");
        if (!CompilePlayer(settings))
        {
            if (m_Progress.cancelled)
                return false;
            ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Failed to compile player");
            return false;
        }
    }

    // 4. Compile scripts. A project with no C# returns true (quiet skip); an
    // attempted compile that FAILS is a build failure in every configuration —
    // Debug used to continue on a warning and ship a silently broken package
    // (no Managed/GameEngine.Scripts.dll, scriptAssemblyPath "", '[100] Build
    // complete!').
    if (settings.compileScripts && !crossHostTemplate)
    {
        ReportProgress(BuildProgress::Stage::CompilingScripts, 0.35f, "Compiling scripts...");
        if (!CompileScripts(settings, stagingDir))
        {
            if (m_Progress.cancelled)
                return false;
            if (!m_PackageResolution.Disabled.empty())
            {
                std::string disabledNames;
                for (const ResolvedPackage& package : m_PackageResolution.Disabled)
                {
                    if (!disabledNames.empty())
                        disabledNames += ", ";
                    disabledNames += package.Manifest.Name;
                }
                m_Progress.errors.push_back(
                    "Note: package(s) [" + disabledNames + "] are disabled in "
                    "Packages/manifest.json — if the compile errors reference their types or "
                    "#if defines, enable the package(s) or remove the references");
            }
            m_Progress.errors.push_back(
                "Script compilation failed — the packaged game would ship without its C# gameplay");
            ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Script compilation failed");
            return false;
        }
    }

    // 5. PostCompile hooks
    if (!RunCustomSteps(BuildStep::Phase::PostCompile, ctx))
    {
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "PostCompile hook failed");
        return false;
    }

    if (checkCancel()) return false;

    // 6. Collect assets
    ReportProgress(BuildProgress::Stage::CollectingAssets, 0.45f, "Collecting assets...");
    if (checkCancel()) return false;
    manifest = CollectAssets(settings);
    AssetCollector runtimeCollector(EngineCore::GetInstance().GetAssetManager());
    if (!runtimeCollector.CollectRuntimeHumanoidProfile(manifest))
    {
        ReportProgress(BuildProgress::Stage::Failed, 0.0f,
                       "Runtime humanoid profile is missing or conflicts with project content; see the asset collection log");
        return false;
    }
    if (!runtimeCollector.CollectDefaultUIFont(manifest))
    {
        ReportProgress(BuildProgress::Stage::Failed, 0.0f,
                       "Default game UI font is missing; see the asset collection log");
        return false;
    }
    // ctx.manifest binds the local `manifest`, updated in place above — so the
    // PreAssetCopy/PostAssetCopy/PrePackage hooks below see the collected manifest.

    // 7. PreAssetCopy hooks (baking goes here)
    if (!RunCustomSteps(BuildStep::Phase::PreAssetCopy, ctx))
    {
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "PreAssetCopy hook failed");
        return false;
    }

    // 8. Copy assets. Always the FULL manifest: the staging dir is recreated
    // from scratch every build (and renamed away on success), so an incremental
    // diff against a persisted manifest starves the fresh staging of every
    // unchanged asset — catastrophically so when the manifest survived a FAILED
    // build whose staging never shipped (P2.1 BUG 3: "next build copies 0
    // assets"). No cross-build copy state exists any more; the stale
    // .Build/asset_manifest.json from older editors is removed below.
    ReportProgress(BuildProgress::Stage::CopyingAssets, 0.55f, "Copying assets...");
    {
        std::error_code staleEc;
        fs::remove(settings.projectRoot / ".Build" / "asset_manifest.json", staleEc);
        if (!CopyAssets(contentRoot, manifest))
        {
            if (m_Progress.cancelled)
                return false;
            ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Failed to copy assets");
            return false;
        }
    }

    // 9. Copy engine shaders
    if (!CopyEngineShaders(contentRoot))
    {
        if (m_Progress.cancelled)
            return false;
        m_Progress.warnings.push_back("Engine shader copy failed or skipped");
    }

    // 9b. Stage third-party notices/licenses for bundled engine modules and assets.
    if (!CopyThirdPartyNotices(contentRoot, settings))
    {
        if (m_Progress.cancelled)
            return false;
        m_Progress.warnings.push_back("Third-party notice copy failed or skipped");
    }

    // 10. PostAssetCopy hooks
    if (!RunCustomSteps(BuildStep::Phase::PostAssetCopy, ctx))
    {
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "PostAssetCopy hook failed");
        return false;
    }

    if (checkCancel()) return false;

    // 11. Assemble runtime: copy flat deps (Windows/Linux) OR inject into the .app (macOS).
    ReportProgress(BuildProgress::Stage::Assembling, 0.70f, "Assembling runtime...");
    if (macBundle)
    {
        // Generate the .icns into a scratch dir (the in-pipeline icon step lives in
        // PreparePlayerProject, which the bundle path skips).
        const fs::path iconScratch = stagingDir / ".iconbuild";
        fs::path icnsPath;
        const AppIconGenerationResult iconResult = GenerateApplicationIcons(settings, iconScratch);
        if (iconResult.success)
            icnsPath = iconScratch / "icons" / "Mac" / "AppIcon.icns";
        else if (!iconResult.error.empty())
            m_Progress.warnings.push_back("Application icon: " + iconResult.error);
        for (const std::string& warning : iconResult.warnings)
            m_Progress.warnings.push_back("Application icon: " + warning);

        if (!MacBundleAssembler::InjectRuntime(settings, bundlePath, stagingDir, icnsPath, m_Progress.errors,
                                               m_Progress.warnings))
        {
            ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Failed to assemble app bundle");
            return false;
        }
    }
    else if (!prebuiltLinux && !CopyRuntimeDependencies(stagingDir, settings))
    {
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Failed to copy runtime dependencies");
        return false;
    }

    // 11b. Editor-built native C++ user scripts (+ the engine_abi marker the
    // Player validates them against), into the content root on every target. A
    // project with no native scripts stages nothing; a native module that cannot
    // be packaged correctly fails the build — a "successful" game without its C++
    // gameplay is worse.
    if (!crossHostTemplate && StageNativeUserScriptsForPackage(
            settings.projectRoot, contentRoot,
            ComputeEngineAbiDigestSet(
                settings, CollectAllPackageDefines(m_PackageResolution),
                CollectEnabledAndDisabledPackageDefines(m_PackageResolution)),
            m_Progress.errors, PlayerBuildConfigIncludesDebugSymbols(settings.buildConfiguration)) == NativeScriptStageOutcome::Failed)
    {
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Failed to package native user scripts");
        return false;
    }

    // 11c. Package code: native runtime modules per package (same record +
    // engine_abi contract as 11b) and compiled package C# runtime assemblies
    // next to GameEngine.Scripts.dll. Same loudness rule as 11b.
    if (!crossHostTemplate && !StagePackageCode(contentRoot, settings))
    {
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Failed to package code modules of enabled packages");
        return false;
    }

    // 11d. A bundle's code lives in Contents/Frameworks, where codesign signs it with
    // the bundle; the content root keeps only the records the Player reads.
    if (macBundle && !MacBundleAssembler::MoveNativeModulesIntoFrameworks(bundlePath, m_Progress.errors))
    {
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Failed to move native modules into the app bundle");
        return false;
    }

    // 12. Write game.config (into the bundle's Contents/Resources on macOS).
    if (!WriteGameConfig(contentRoot, settings))
    {
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Failed to write game config");
        return false;
    }

    // 13. Stage the authoritative GUID identity as flat, read-only `.assetmanifest`s:
    // the project's beside Assets/, one per staged package beside its own
    // Packages/<alias>/Assets/ — each mount stays an independent GUID namespace.
    // The packaged Player mounts them (no filesystem scan, no SQLite); without them,
    // imported-asset references re-mint random GUIDs and fail to resolve.
    if (!StageAssetManifest(contentRoot, manifest))
    {
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Failed to stage asset manifest");
        return false;
    }

    // 13b. Packages/packages.index — the Player's package mount list (dev layouts
    // resolve Packages/manifest.json instead; a packaged game re-resolves nothing).
    if (!StagePackagesIndex(contentRoot, manifest, m_PackageResolution, m_PackageRuntimeModules,
                            m_Progress.errors))
    {
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Failed to stage packages.index");
        return false;
    }

    // 13c. Cook the staged content: strip the editor-only components from the staged scenes,
    // bake the copied texture bytes using the exact metadata just staged, then ship every asset
    // whose parser has a packaged form cooked (particle stacks). Cancellation is polled
    // synchronously; the exporter cannot publish a partially cooked package, and an asset that
    // cannot cook fails the build rather than shipping broken.
    TexturePackageCookStats textureStats;
    std::string cookError;
    const auto targetQuality = PlayerBuildConfigUsesDebugCrt(settings.buildConfiguration)
        ? TextureCookEncodeQuality::QuickBC7 : TextureCookEncodeQuality::Full;
    AssetManager& assets = EngineCore::GetInstance().GetAssetManager();
    if (!CookStagedContent(contentRoot, manifest, assets.GetRegistry(), assets.GetParserRegistry(), targetQuality,
                           assets.GetTextureCookWorkers(), [this]() { return m_CancelRequested.load(); },
                           textureStats, cookError))
    {
        if (FailIfCancelled())
            return false;
        m_Progress.errors.push_back(cookError);
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Failed to cook staged content");
        return false;
    }
    Logger::Log::Info("Build: Staged {} texture artifacts for {} textures ({} reused, {} bytes)",
        textureStats.Artifacts, textureStats.Textures, textureStats.ReusedArtifacts, textureStats.Bytes);
    if (textureStats.UnclassifiedTextures || textureStats.SkippedTextures)
        Logger::Log::Warning("Build: Texture fallback summary: {} unclassified, {} without registered identity",
                             textureStats.UnclassifiedTextures, textureStats.SkippedTextures);

    // 13d. Cook per-model LOD caches (.gelod) beside the manifest so the packaged
    // Player consumes them without a runtime meshopt dependency (Phase B). A
    // cook failure is non-fatal: the Player falls back to runtime generation.
    CookMeshLODs(contentRoot, manifest);

    // 13e. Stage the project's baked terrains beside the manifest, where the
    // packaged Player looks them up by content key; a terrain with no bake there
    // is baked at load as before.
    StageTerrainBakes(contentRoot, settings, manifest);

    // 13f. Pack each shipped terrain heightmap's cooked pages into its terrain container
    // (Assets/Cooked/Terrain/<guid>.geterrain), cooking it in the project cache first when it changed.
    {
        AssetManager& pageAssets = EngineCore::GetInstance().GetAssetManager();
        const TerrainPageExportResult pages = ExportTerrainPages(
            settings.projectRoot, contentRoot, manifest, pageAssets.GetRegistry(), &EngineCore::GetInstance().GetJobSystem(),
            &m_CancelRequested);
        for (const std::string& warning : pages.Warnings)
            m_Progress.warnings.push_back(warning);
    }

    // 14. PrePackage hooks (platform-specific bundling)
    if (!macBundle && !PackageLinuxIconsToStaging(settings, stagingDir))
        m_Progress.warnings.push_back("Failed to package Linux application icons");

    if (!RunCustomSteps(BuildStep::Phase::PrePackage, ctx))
    {
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "PrePackage hook failed");
        return false;
    }

    // 15. Finalize.
    ReportProgress(BuildProgress::Stage::Assembling, 0.95f, "Finalizing output...");
    if (macBundle)
    {
        // Codesign is the FINAL mutation — game.config + manifest were just written
        // into the bundle, which would otherwise invalidate the signature.
        if (!MacBundleAssembler::Codesign(bundlePath, std::string(), m_Progress.errors))
        {
            ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Failed to codesign app bundle");
            return false;
        }
        // Move ONLY the finished .app into the output dir (the staging dir also
        // holds compile scratch). stagingDir is a sibling of outputDirectory, so
        // the rename stays on one filesystem.
        std::error_code fec;
        const fs::path finalApp = settings.outputDirectory / (gameName + ".app");
        fs::create_directories(settings.outputDirectory, fec);
        if (!FinalizeOutput(bundlePath, finalApp))
        {
            if (!m_Progress.cancelled)
                ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Failed to finalize app bundle");
            return false;
        }
        fs::remove_all(stagingDir, fec);
        Logger::Log::Info("Build: macOS app finalized at '{}'", finalApp.string());
    }
    else if (!FinalizeOutput(stagingDir, settings.outputDirectory))
    {
        if (!m_Progress.cancelled)
            ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Failed to finalize output");
        return false;
    }

    ReportProgress(BuildProgress::Stage::Complete, 1.0f, "Build complete!");
    return true;
}

// ---------------------------------------------------------------------------
// PreparePlayerProject
// ---------------------------------------------------------------------------

bool BuildPipeline::PreparePlayerProject(const BuildSettings& s, const fs::path& /*stagingDir*/)
{
    const fs::path buildDir = s.projectRoot / ".Build" / "Player";
    std::error_code ec;
    fs::create_directories(buildDir, ec);

    std::vector<fs::path> allSources;
    std::string sourceError;
    if (!PreparePlayerSources(s.editorSDKPath / "templates" / "Player",
                              buildDir / "Source", s.userCppSources, allSources, sourceError))
    {
        m_Progress.errors.push_back(std::move(sourceError));
        return false;
    }
    // Generate CMakeLists.txt from template.
    const fs::path templateFile = s.editorSDKPath / "templates" / "Player" / "GeneratedCMakeLists.txt.in";
    if (!fs::exists(templateFile))
    {
        m_Progress.errors.push_back("CMakeLists template not found: " + templateFile.string());
        return false;
    }

    std::ifstream templateIn(templateFile);
    if (!templateIn.is_open())
    {
        m_Progress.errors.push_back("Failed to read CMakeLists template: " + templateFile.string());
        return false;
    }
    std::string templateContent((std::istreambuf_iterator<char>(templateIn)),
                                 std::istreambuf_iterator<char>());

    std::string sourcesList;
    for (const auto& src : allSources)
        sourcesList += "    \"" + src.generic_string() + "\"\n";

    // Build include dirs (unique parent directories of .h files).
    std::unordered_set<std::string> includeDirSet;
    std::string includeDirsList;
    for (const auto& src : allSources)
    {
        if (src.extension() == ".h")
        {
            std::string dir = "\"" + src.parent_path().generic_string() + "\"";
            if (includeDirSet.insert(dir).second)
                includeDirsList += "    " + dir + "\n";
        }
    }

    // Substitute template variables.
    auto replaceAll = [](std::string& str, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = str.find(from, pos)) != std::string::npos)
        {
            str.replace(pos, from.length(), to);
            pos += to.length();
        }
    };

    // Sanitize game name for cmake project() — remove characters that break cmake syntax.
    std::string safeName = s.playerConfig.gameName;
    for (char& c : safeName)
        if (c == '"' || c == '\\' || c == ';')
            c = '_';
    replaceAll(templateContent, "@GE_GAME_NAME@", safeName);
    replaceAll(templateContent, "@GE_SDK_CMAKE_DIR@",
               (s.editorSDKPath / "cmake").generic_string());
    replaceAll(templateContent, "@GE_DISCOVERED_SOURCES@", sourcesList);
    replaceAll(templateContent, "@GE_USER_INCLUDE_DIRS@", includeDirsList);

    std::string iconCmakeSection = "# No application icon configured\n";
    const AppIconGenerationResult iconResult = GenerateApplicationIcons(s, buildDir);
    if (!iconResult.error.empty())
        m_Progress.warnings.push_back("Application icon: " + iconResult.error);
    else if (iconResult.success)
        iconCmakeSection = iconResult.cmakeIconSection;
    for (const std::string& warning : iconResult.warnings)
        m_Progress.warnings.push_back("Application icon: " + warning);
    replaceAll(templateContent, "@GE_APP_ICON_SECTION@", iconCmakeSection);

    const fs::path outputCMake = buildDir / "CMakeLists.txt";
    std::ofstream out(outputCMake);
    if (!out.is_open())
    {
        m_Progress.errors.push_back("Failed to write " + outputCMake.string());
        return false;
    }
    out << templateContent;
    if (!out.good())
    {
        m_Progress.errors.push_back("Failed to write " + outputCMake.string());
        return false;
    }
    out.close();

    Logger::Log::Info("Build: Generated CMakeLists.txt with {} source files", allSources.size());
    return true;
}

// ---------------------------------------------------------------------------
// CompilePlayer
// ---------------------------------------------------------------------------

static std::string FindCmakeExecutable()
{
    // Try common cmake locations on Windows.
    const char* candidates[] = {
        "cmake",
        "C:\\Program Files\\CMake\\bin\\cmake.exe",
        "C:\\Program Files (x86)\\CMake\\bin\\cmake.exe",
    };
    for (const char* c : candidates)
    {
        const ShellProcessResult probe = RunProcessCaptured(c, {"--version"});
        if (probe.exitCode == 0)
            return c;
    }
    return "cmake"; // hope it's in PATH
}

bool BuildPipeline::CompilePlayer(const BuildSettings& s)
{
    const fs::path sourceDir = s.projectRoot / ".Build" / "Player";
    const fs::path buildDir = sourceDir / "build";

    static std::string cmakeExe = FindCmakeExecutable();

    // Configure
    std::string configureCmd = "\"" + cmakeExe + "\" -S \"" + sourceDir.generic_string() +
                               "\" -B \"" + buildDir.generic_string() + "\"";
    if (!s.cmakeGenerator.empty())
        configureCmd += " -G \"" + s.cmakeGenerator + "\"";
    if (!s.vcpkgToolchainFile.empty())
        configureCmd += " -DCMAKE_TOOLCHAIN_FILE=\"" + s.vcpkgToolchainFile.generic_string() + "\"";
    // Optional: the Player links against the SDK alone. A vcpkg tree, when one is known,
    // adds its libraries for game C++ that calls a vcpkg library the engine does not
    // export; the SDK's config carries no baked machine path, so it travels per configure.
    if (!s.vcpkgInstalledDir.empty())
        configureCmd += " -DGAMEENGINE_VCPKG_INSTALLED=\"" + s.vcpkgInstalledDir.generic_string() + "\"";
    else
        Logger::Log::Info("Build: the Player links against the engine SDK only. If game C++ in the Player "
                          "calls a vcpkg library directly and the link fails, set build.vcpkgInstalledDir "
                          "in the project's .Editor/ProjectSettings.json to the engine build's "
                          "vcpkg_installed directory");
    // find_package() consults the cached GameEngine_DIR before the template's
    // PATHS hint, so a .Build/Player/build/CMakeCache.txt from a previous
    // editor keeps pinning that editor's SDK (stale libs → LNK2038 CRT
    // mismatch). Pass the current SDK explicitly — the -D overwrites the cache
    // on every configure.
    configureCmd += " -DGameEngine_DIR=\"" + (s.editorSDKPath / "cmake").generic_string() + "\"";
    // Multi-config generators pick the SDK lib flavor at configure time with no
    // CMAKE_BUILD_TYPE; name the config this build will actually compile so the
    // SDK config matches lib/<config> instead of defaulting to lib/Debug.
    if (!s.buildConfiguration.empty())
        configureCmd += " -DGAMEENGINE_BUILD_CONFIG=" + s.buildConfiguration;

    // A record a previous configure left must not stand in for this one's.
    std::error_code removeEc;
    fs::remove(buildDir / kLinkConfigFileName, removeEc);
    Logger::Log::Info("Build: cmake configure: {}", configureCmd);
    ShellProcessResult configureResult = RunShellCommandImpl(configureCmd + " 2>&1", nullptr);
    if (configureResult.cancelled)
        return FailIfCancelled(), false;
    if (configureResult.exitCode != 0)
    {
        m_Progress.errors.push_back("cmake configure failed (exit " + std::to_string(configureResult.exitCode) + ")");
        m_Progress.errors.push_back(configureResult.output);
        Logger::Log::Error("Build: cmake configure failed:\n{}", configureResult.output);
        return false;
    }

    // The configure recorded the engine configuration the player links; refuse a package
    // whose engine runtime cannot match it before compiling anything.
    if (!ResolvePlayerEngineRuntime(s, buildDir))
        return false;

    // Build
    std::string buildCmd = "\"" + cmakeExe + "\" --build \"" + buildDir.generic_string() +
                           "\" --config " + s.buildConfiguration;
    Logger::Log::Info("Build: cmake build: {}", buildCmd);
    ShellProcessResult buildResult = RunShellCommandImpl(buildCmd + " 2>&1", nullptr);
    if (buildResult.cancelled)
        return FailIfCancelled(), false;
    if (buildResult.exitCode != 0)
    {
        m_Progress.errors.push_back("cmake build failed (exit " + std::to_string(buildResult.exitCode) + ")");
        m_Progress.errors.push_back(buildResult.output);
        Logger::Log::Error("Build: cmake build failed:\n{}", buildResult.output);
        return false;
    }

    Logger::Log::Info("Build: Player compiled successfully");
    return true;
}

// ---------------------------------------------------------------------------
// CompileScripts
// ---------------------------------------------------------------------------

fs::path BuildPipeline::ResolveDotnetForPackaging(const fs::path& editorSdkPath, std::string& outError)
{
    // The staged SDK manifest records the build machine's dotnet; the shared
    // resolver falls back to DOTNET_ROOT, PATH, and standard install locations
    // for THIS machine (same discovery the editor's native-script builds use).
    // A missing manifest (SDK-less layouts) still gets the env/PATH scan.
    {
        NativeScripting::NativeBuildConfig cfg;
        std::string manifestErr;
        if (NativeScripting::LoadSdkManifest(editorSdkPath, cfg, manifestErr) && !cfg.DotnetExe.empty())
            return cfg.DotnetExe;
    }
    const fs::path resolved = NativeScripting::ResolveDotnetExecutable({});
    if (!resolved.empty())
        return resolved;

    outError = DescribeMissingDotnet(editorSdkPath);
    return {};
}

std::string BuildPipeline::DescribeMissingDotnet(const fs::path& editorSdkPath)
{
    return "dotnet SDK not found — the packaged game's C# scripts cannot compile. Install the .NET 10 SDK "
           "(https://dotnet.microsoft.com/download), set DOTNET_ROOT, or add 'dotnet' to PATH. Searched: "
           "the staged SDK manifest's recorded path (" +
           (editorSdkPath / "nativescripting" / "manifest.txt").generic_string() +
           "), DOTNET_ROOT, PATH, and the platform's common install locations.";
}

// True when any user-authored C# source exists under the project's Assets tree
// (obj/bin build litter excluded). Decides whether a missing generated csproj is
// a quiet no-scripts skip or a loud packaging failure.
static bool ProjectHasCSharpSources(const fs::path& assetsRoot)
{
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(assetsRoot, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        std::error_code entryEc;
        if (it->is_directory(entryEc))
        {
            const std::string dir = it->path().filename().string();
            if (dir == "obj" || dir == "bin")
            {
                it.disable_recursion_pending();
                continue;
            }
        }
        if (it->is_regular_file(entryEc) && it->path().extension() == ".cs")
            return true;
    }
    return false;
}

bool BuildPipeline::CompileScripts(const BuildSettings& s, const fs::path& stagingDir)
{
    // The csproj refresh and a Debug package's managed build are one job of the "Script
    // compiles" channel: they read the csproj and the package assemblies a live compile
    // rewrites, so they never run beside one. Every shipping configuration publishes the
    // scripts AOT, not just the one named "Release": a RelWithDebInfo or MinSizeRel package
    // is a shipping artifact with different symbol/size trade-offs, and it would otherwise
    // ship loose managed assemblies plus a runtime dependency the Release package does not
    // have. That publish builds its own csproj under the staging tree and reads nothing a
    // live compile writes, so it runs here, on the export's own job, after the refresh.
    const bool nativeAot = IsShipOptimizedPlayerBuildConfig(s.buildConfiguration);
    fs::path scriptsProj;
    fs::path dotnetExe;
    bool nothingToCompile = false;
    bool ok = false;
    const bool ran = EngineCore::GetInstance().GetScriptManager().RunCompile([&]() {
        ok = PrepareScriptProject(s, scriptsProj, dotnetExe, nothingToCompile);
        if (ok && !nothingToCompile && !nativeAot)
            ok = BuildManagedScripts(s, stagingDir, scriptsProj, dotnetExe);
    });
    if (!ran)
    {
        m_Progress.errors.push_back("The script compile was refused: the script manager or the job system is shutting down");
        return false;
    }
    if (!ok || nothingToCompile || !nativeAot)
        return ok;
    return CompileScriptsNativeAOT(s, stagingDir, scriptsProj, dotnetExe);
}

bool BuildPipeline::PrepareScriptProject(const BuildSettings& s, fs::path& outProject, fs::path& outDotnet,
                                         bool& outNothingToCompile)
{
    // Resolve the user script project the SAME way the ScriptManager decides
    // where to emit it (generated-project root, default the project root).
    //
    // Refresh first: the generated csproj is this path's ONLY reference carrier
    // (the editor's live compiles pass references per compile-server request),
    // so its baked package <Reference> items must match the session's current
    // package set before dotnet consumes it.
    outNothingToCompile = false;
    (void)EngineCore::GetInstance().GetScriptManager().RefreshGeneratedProject();
    fs::path scriptsProj = EngineCore::GetInstance().GetScriptManager().GeneratedProjectPath();
    if (scriptsProj.parent_path().empty() || !scriptsProj.is_absolute())
    {
        // No live scripts config bound (headless pipeline use): the engine's
        // default generated-project location inside the project root.
        scriptsProj = ScriptManager::GeneratedProjectPathFor(s.projectRoot);
    }

    std::error_code ec;
    if (!fs::exists(scriptsProj, ec))
    {
        // A project WITH C# sources but no csproj means the editor never
        // generated/compiled them — shipping a game without its scripts is a
        // build failure, never a silent skip.
        if (ProjectHasCSharpSources(s.projectRoot / "Assets"))
        {
            m_Progress.errors.push_back(
                "Project has C# sources but no script project at '" + scriptsProj.generic_string() +
                "' — open the project in the editor so scripts compile, then package again");
            Logger::Log::Error("Build: C# sources present but script project missing at '{}'",
                               scriptsProj.string());
            return false;
        }
        Logger::Log::Info("Build: No script project found at '{}', skipping", scriptsProj.string());
        outNothingToCompile = true;
        return true;
    }

    // The project HAS C# to compile past this point — refuse up front, naming
    // the fix, when no dotnet SDK resolves. Both packaged-C# paths (managed
    // build below, NativeAOT publish for ship-optimized configs) spawn dotnet;
    // a bare "dotnet" invocation is PATH-luck and used to fail deep inside the
    // build with an unactionable shell error.
    std::string dotnetErr;
    const fs::path dotnetExe = ResolveDotnetForPackaging(s.editorSDKPath, dotnetErr);
    if (dotnetExe.empty())
    {
        m_Progress.errors.push_back(dotnetErr);
        Logger::Log::Error("Build: {}", dotnetErr);
        return false;
    }
    outProject = scriptsProj;
    outDotnet = dotnetExe;
    return true;
}

bool BuildPipeline::BuildManagedScripts(const BuildSettings& s, const fs::path& stagingDir, const fs::path& scriptsProj,
                                        const fs::path& dotnetExe)
{
    // Debug: standard managed build.
    std::error_code ec;
    const fs::path managedOut = stagingDir / "Managed";
    fs::create_directories(managedOut, ec);

    // Pass the live engine bin dir so a project whose csproj bakes a stale
    // EngineBinDir (issue #333) still resolves engine references against this
    // Editor install. Trailing separator: HintPaths concatenate the property.
    std::string engineBinDir = ScriptingPaths::ResolveEngineManagedDirectory().generic_string();
    if (!engineBinDir.empty() && engineBinDir.back() != '/')
        engineBinDir += '/';

    std::string cmd = "\"" + dotnetExe.generic_string() + "\" build \"" + scriptsProj.generic_string() +
                      "\" --configuration " + s.buildConfiguration +
                      " -o \"" + managedOut.generic_string() + "\"" +
                      " -p:EngineBinDir=\"" + engineBinDir + "\"";

    // Package `defines` union, exactly what the editor's compile-server request
    // carries (P1 CollectAllPackageDefines) — the generated csproj appends
    // $(GamePackageDefines) to DefineConstants. %3B = MSBuild-escaped ';'.
    std::string packageDefines;
    for (const std::string& define : CollectAllPackageDefines(m_PackageResolution))
    {
        if (!packageDefines.empty())
            packageDefines += "%3B";
        packageDefines += define;
    }
    if (!packageDefines.empty())
        cmd += " -p:GamePackageDefines=\"" + packageDefines + "\"";

    Logger::Log::Info("Build: dotnet build: {}", cmd);
    ShellProcessResult buildResult = RunShellCommandImpl(cmd + " 2>&1", nullptr);
    if (buildResult.cancelled)
        return FailIfCancelled(), false;
    if (buildResult.exitCode != 0)
    {
        // The project HAS a script project; failing to compile it is a build
        // error, never a warning (the caller fails the whole build on it).
        m_Progress.errors.push_back("dotnet build failed (exit " +
                                    std::to_string(buildResult.exitCode) + "):\n" +
                                    buildResult.output);
        Logger::Log::Error("Build: dotnet build failed:\n{}", buildResult.output);
        return false;
    }

    // Editor-side artifacts must never ship in a game: the generated project
    // references GameEngine.Editor.dll (Private=false), but stale outputs or
    // MSBuild copy-local quirks have leaked editor csproj/assembly files into
    // the output before. Scrub them from the staged Managed/ set.
    static constexpr const char* kEditorArtifacts[] = {
        "GameEngine.Editor.csproj", "GameEngine.Editor.dll", "GameEngine.Editor.pdb",
        "GameEngine.Editor.deps.json", "GameEngine.Editor.xml"};
    for (const char* name : kEditorArtifacts)
    {
        std::error_code rmEc;
        if (fs::remove(managedOut / name, rmEc))
            Logger::Log::Info("Build: excluded editor artifact '{}' from staged Managed/", name);
    }

    // WriteGameConfig later points scriptAssemblyPath at this exact file; a
    // zero-exit build that staged nothing is still a broken package.
    const fs::path stagedScripts = managedOut / ScriptingPaths::kScriptsAssemblyFileName;
    if (!fs::exists(stagedScripts, ec))
    {
        m_Progress.errors.push_back("dotnet build reported success but staged no '" +
                                    stagedScripts.generic_string() +
                                    "' — the packaged game would ship without its C# gameplay");
        Logger::Log::Error("Build: scripts compile staged no assembly at '{}'",
                           stagedScripts.string());
        return false;
    }

    Logger::Log::Info("Build: Scripts compiled successfully");
    return true;
}

bool BuildPipeline::CompileScriptsNativeAOT(const BuildSettings& s,
                                             const fs::path& stagingDir,
                                             const fs::path& scriptsProj,
                                             const fs::path& dotnetExe)
{
    // The managed side of an AOT publish is always -c Release: the player
    // configuration selects the artifact SHAPE (AOT vs loose assemblies), and
    // every shape that ships wants the optimized managed input.
    Logger::Log::Info("Build: NativeAOT compile for user scripts (player config '{}')",
                      s.buildConfiguration);

    std::error_code ec;

    // Resolve the AOT .csproj references against the STAGED SDK (ship-safety C4):
    // a relocated or shipped editor has no engine source tree, so the generated
    // project references the SDK's staged assemblies, never repo .csproj files.
    const fs::path scriptSources = scriptsProj.parent_path();
    const fs::path sdkManagedDir = s.editorSDKPath / "managed";
    const std::array<fs::path, 3> referenceDlls{
        sdkManagedDir / "GameEngine.Scripting.Runtime.dll",
        sdkManagedDir / "GameEngine.Scripting.ABI.dll",
        sdkManagedDir / "GameEngine.ECS.ABI.dll",
    };
    const fs::path generatorDll = sdkManagedDir / "analyzers" / "EntitySystemGenerator.dll";

    for (const fs::path& dll : referenceDlls)
    {
        if (!fs::exists(dll, ec))
        {
            m_Progress.errors.push_back("SDK managed scripting assembly not staged: " + dll.string() +
                                        " (rebuild the Editor with scripting enabled)");
            Logger::Log::Error("Build: SDK managed scripting assembly not staged at '{}'", dll.string());
            return false;
        }
    }

    // Generate the AOT .csproj.
    const fs::path aotDir = stagingDir / "NativeAOT";
    fs::create_directories(aotDir, ec);
    const fs::path aotCsproj = aotDir / (std::string(kNativeAotProjectName) + ".csproj");

    {
        std::ofstream f(aotCsproj);
        if (!f.is_open())
        {
            m_Progress.errors.push_back("Failed to create AOT .csproj at " + aotCsproj.string());
            return false;
        }

        // Editor-kind modules were excluded when the runtime module set was collected.
        NativeAotProjectDesc project;
        project.ScriptSources = scriptSources;
        project.PackageModules = m_PackageRuntimeModules;
        project.PackageDefines = CollectAllPackageDefines(m_PackageResolution);
        project.ReferenceDlls.assign(referenceDlls.begin(), referenceDlls.end());
        if (fs::exists(generatorDll, ec))
            project.GeneratorDll = generatorDll;
        f << GenerateNativeAotCsprojXml(project);
    }

    Logger::Log::Info("Build: Generated AOT .csproj at '{}'", aotCsproj.string());

    const fs::path publishDir = aotDir / "publish";
    std::string cmd = "\"" + dotnetExe.generic_string() + "\" publish \"" + aotCsproj.generic_string() + "\""
                      " -r " + std::string(kNativeAotRuntimeIdentifier) +
                      " -c Release"
                      " /p:NativeLib=Shared"
                      " -o \"" + publishDir.generic_string() + "\"";

#if defined(_WIN32)
    // ILC's native link step needs link.exe (plus LIB/INCLUDE); an editor
    // launched outside a VS dev prompt fails the publish with "Platform linker
    // not found". NativeScripting's MsvcToolchain already captures that
    // environment once for user-DLL builds — prepend the cached batch here.
    if (auto* nativeScripts = EngineCore::GetInstance().GetNativeScriptManager())
    {
        const NativeScripting::MsvcToolchain& toolchain = nativeScripts->Toolchain();
        toolchain.WaitUntilReady(std::chrono::seconds(30));
        const fs::path envBatch = toolchain.EnvBatch();
        if (!envBatch.empty())
        {
            // The captured environment carries link.exe on PATH plus LIB/
            // INCLUDE. IlcUseEnvironmentalTools makes ILC's native link step
            // use exactly that (CppLinker defaults to 'link') instead of
            // shelling out to vswhere/findvcvarsall, which needs the VS
            // Installer directory and fails on a clean process environment.
            cmd += " /p:IlcUseEnvironmentalTools=true";
            cmd = "call \"" + envBatch.string() + "\" && " + cmd;
        }
        else
            Logger::Log::Warning(
                "Build: no captured MSVC environment — the NativeAOT publish "
                "needs the platform linker; it will only succeed if the editor "
                "itself runs from a VS developer prompt");
    }
#endif

    Logger::Log::Info("Build: dotnet publish (NativeAOT): {}", cmd);

    ShellProcessResult publishResult = RunShellCommandImpl(cmd + " 2>&1", nullptr);
    if (publishResult.cancelled)
        return FailIfCancelled(), false;
    if (publishResult.exitCode != 0)
    {
        m_Progress.errors.push_back("NativeAOT publish failed (exit " + std::to_string(publishResult.exitCode) + ")");
        m_Progress.errors.push_back(publishResult.output);
        Logger::Log::Error("Build: NativeAOT publish failed:\n{}", publishResult.output);
        return false;
    }

    // Copy the published native library to the staging directory under the
    // name the Player loads.
    const fs::path aotLib = publishDir / kNativeAotPublishedLibraryName;
    const fs::path stagedLib = stagingDir / kNativeAotScriptsLibraryName;

    if (!fs::exists(aotLib, ec))
    {
        m_Progress.errors.push_back("NativeAOT output not found: " + aotLib.string());
        Logger::Log::Error("Build: NativeAOT output not found at '{}'", aotLib.string());
        return false;
    }

#if defined(_WIN32)
    // A publish whose ge_scripts_* exports got trimmed produces a library that
    // loads fine and silently never ticks C# in the shipped game. Probe the
    // export table (no code runs under DONT_RESOLVE_DLL_REFERENCES) and fail
    // the package loudly instead.
    {
        HMODULE probe =
            ::LoadLibraryExW(aotLib.wstring().c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        std::string missingExports;
        if (probe)
        {
            for (const char* entry : {"ge_scripts_initialize", "ge_scripts_tick", "ge_scripts_shutdown"})
            {
                if (!::GetProcAddress(probe, entry))
                {
                    if (!missingExports.empty())
                        missingExports += ", ";
                    missingExports += entry;
                }
            }
            ::FreeLibrary(probe);
        }
        else
        {
            missingExports = "<library failed to load for export probe>";
        }
        if (!missingExports.empty())
        {
            m_Progress.errors.push_back(
                "NativeAOT publish produced no script entry points (" + missingExports +
                ") — the packaged game would boot but its C# would never run. "
                "GameEngine.Scripting.Runtime must stay listed as an "
                "UnmanagedEntryPointsAssembly in the generated AOT project.");
            Logger::Log::Error("Build: NativeAOT library at '{}' is missing exports: {}",
                               aotLib.string(), missingExports);
            return false;
        }
    }
#endif

    fs::copy_file(aotLib, stagedLib, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        m_Progress.errors.push_back("Failed to copy NativeAOT library: " + ec.message());
        return false;
    }

    // The AOT scratch (generated csproj, obj/bin, unstripped publish output)
    // lives inside the staging dir for cancellation/cleanup reasons — but the
    // staging dir SHIPS on success, and games were shipping megabytes of
    // compile litter. The staged library above is the only artifact the game
    // needs; drop the scratch now (kept on failure paths for diagnostics).
    std::error_code scratchEc;
    fs::remove_all(aotDir, scratchEc);
    if (scratchEc)
        m_Progress.warnings.push_back("Could not remove NativeAOT scratch from the staged game: " +
                                      scratchEc.message());

    Logger::Log::Info("Build: NativeAOT scripts compiled -> '{}'", stagedLib.string());
    return true;
}

// ---------------------------------------------------------------------------
// CollectAssets
// ---------------------------------------------------------------------------

AssetManifest BuildPipeline::CollectAssets(const BuildSettings& s)
{
    auto& am = EngineCore::GetInstance().GetAssetManager();
    AssetCollector collector(am);
    AssetManifest manifest = collector.CollectFromScenes(s.scenes);

    // If the dependency-based collector found nothing, fall back to copying
    // the entire project Assets directory. This handles cases where the asset
    // registry hasn't scanned the project yet or scene paths don't match.
    if (manifest.entries.empty())
    {
        Logger::Log::Warning("Build: AssetCollector found no assets via dependency graph, falling back to full copy");
        manifest = collector.CollectAllAssets();
    }

    // If the collector found nothing useful, copy all files from the project's
    // Assets directory. This is the simplest reliable fallback — the dependency
    // graph may not be populated when the build runs on a background thread.
    if (manifest.entries.empty())
    {
        const auto& assetRoot = EngineCore::GetInstance().GetResolvedAssetRoot();
        std::error_code ec;
        if (fs::is_directory(assetRoot, ec))
        {
            for (const auto& entry : fs::recursive_directory_iterator(assetRoot, ec))
            {
                if (!entry.is_regular_file())
                    continue;
                // Skip Shaders — copied separately by CopyEngineShaders.
                auto rel = fs::relative(entry.path(), assetRoot);
                if (rel.generic_string().rfind("Shaders/", 0) == 0)
                    continue;
                // Don't ship C++/C# source, build files, or Editor/-convention
                // folders. Checked on the root-relative path so the deny only
                // sees segments under the asset root.
                if (!AssetCollector::IsShippableContent(rel))
                    continue;
                AssetManifestEntry me;
                me.sourcePath = entry.path();
                me.outputPath = fs::path("Assets") / rel;
                manifest.entries.push_back(std::move(me));
            }
            Logger::Log::Info("Build: Added {} asset files from project directory", manifest.entries.size());
        }
    }

    // Whichever way the content was collected, the pipeline game.config names ships
    // in the manifest: the Player resolves it by path through the .assetmanifest. A
    // packaged game has no stand-in pipeline, so one missing from the package draws nothing.
    if (!collector.CollectRenderPipeline(s.renderPipelinePath, manifest))
        m_Progress.warnings.push_back("Selected render pipeline not found; the game will draw nothing: " +
                                      s.renderPipelinePath);

    return manifest;
}

// ---------------------------------------------------------------------------
// Package staging (P2)
// ---------------------------------------------------------------------------

bool BuildPipeline::RunPackageAssetStep(BuildContext& ctx)
{
    // The scenes' dependency walk already staged the package assets they reach,
    // under Packages/<alias>/Assets/. Each enabled package adds what the walk
    // cannot see: its Shaders/ folder (shaders a material names by path) and the
    // runtimeAssets its code loads for the components the built scenes use.
    auto& am = EngineCore::GetInstance().GetAssetManager();
    AssetCollector collector(am);
    const std::unordered_set<std::string> usedComponents = AssetCollector::CollectComponentNames(ctx.manifest);
    for (const ResolvedPackage& package : m_PackageResolution.MountOrder)
    {
        for (const std::string& unmatched : collector.CollectPackageRuntimeAssets(package, usedComponents, ctx.manifest))
            m_Progress.warnings.push_back("package '" + package.Manifest.Name + "' lists runtimeAssets '" +
                                          unmatched + "', which matches no asset in the package; fix the path "
                                          "in its package.json");
    }

    // Loud validation over the final asset set (theme: never silently). Errors
    // land in m_Progress.errors and fail the build before any copy happens —
    // and, running at PreAssetCopy, before the native ABI gates of steps
    // 11b/11c, so a mid-session disable is attributed here first.
    return ValidatePackagedBuild(ctx.manifest, m_PackageResolution, m_PackageRuntimeModules,
                                 m_SessionMountedPackageAliases, m_Progress.errors,
                                 m_Progress.warnings);
}

namespace
{

// 'EZTree', 'Water' for messages.
std::string QuoteModuleNames(const std::vector<std::string>& moduleNames)
{
    std::string names;
    for (const std::string& name : moduleNames)
        names += (names.empty() ? "'" : ", '") + name + "'";
    return names;
}

// "Waiting for native module 'EZTree' to finish building..." for the progress line.
// No names: a build is requested but has not started yet.
std::string DescribeNativeModuleWait(const std::vector<std::string>& moduleNames)
{
    if (moduleNames.empty())
        return "Waiting for the requested native module build to start...";
    return (moduleNames.size() == 1 ? "Waiting for native module " : "Waiting for native modules ") +
           QuoteModuleNames(moduleNames) + " to finish building...";
}

} // namespace

bool BuildPipeline::WaitForNativeModuleBuilds()
{
    using Outcome = NativeScripting::NativeScriptManager::ModuleBuildWaitOutcome;
    if (!m_NativeModuleBuilds)
        return true;

    const NativeScripting::NativeScriptManager::ModuleBuildWaitResult wait = m_NativeModuleBuilds->WaitForModuleBuilds(
        [this] { return IsCancelled(); },
        [this](const std::vector<std::string>& moduleNames) {
            ReportProgress(BuildProgress::Stage::PreparingProject, 0.0f, DescribeNativeModuleWait(moduleNames));
        });
    switch (wait.Outcome)
    {
    case Outcome::Settled:
        if (wait.FailedModules.empty())
            return true;
        m_Progress.errors.push_back(
            "native module " + QuoteModuleNames(wait.FailedModules) +
            " failed its latest build in the editor, so the export would ship an older build of it; fix the "
            "compile errors shown in the Log and export again");
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Native module build failed");
        return false;
    case Outcome::DeferredByPlayMode:
        m_Progress.errors.push_back(
            wait.DeferredModules.empty()
                ? "Exit play mode before you export: native module sources changed during play mode and rebuild "
                  "only when play mode exits"
                : "Exit play mode before you export: native module " + QuoteModuleNames(wait.DeferredModules) +
                      " finished building during play mode and loads only when play mode exits");
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Exit play mode to export");
        return false;
    case Outcome::Cancelled:
        FailIfCancelled();
        return false;
    case Outcome::ShutDown:
        m_Progress.errors.push_back("the editor stopped its native module builds before they finished, so "
                                    "their modules cannot be packaged; reopen the project and build again");
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Native module builds stopped before they finished");
        return false;
    case Outcome::CalledFromTickThread:
        m_Progress.errors.push_back("the export ran on the editor's main thread, where it cannot wait for native "
                                    "module builds; start it from the Build panel");
        ReportProgress(BuildProgress::Stage::Failed, 0.0f, "Export started on the wrong thread");
        return false;
    }
    return false;
}

bool BuildPipeline::StagePackageCode(const fs::path& contentRoot, const BuildSettings& s)
{
    // Native runtime modules: per-package generalization of the 11b project
    // module staging — same relative record + engine_abi marker contract. The
    // editor stamped each package module's record with a digest over the SDK
    // config PLUS that module's effective defines, so the per-module digest is
    // recomputed with the same input here.
    if (!StagePackageNativeModules(
            contentRoot, m_PackageResolution, m_PackageRuntimeModules,
            [&s](const PackageCodeModule& module) {
                // Per-module defines are the module's own compile config; the
                // disabled-union variant does not apply (a disabled package's
                // module never reaches this staging loop).
                return ComputeEngineAbiDigestSet(s, module.Defines, module.Defines);
            },
            m_Progress.errors, PlayerBuildConfigIncludesDebugSymbols(s.buildConfiguration)))
        return false;

    bool hasRuntimeCSharp = false;
    for (const PackageCodeModule& module : m_PackageRuntimeModules)
    {
        if (module.Lang == PackageModuleRecord::ModuleLang::CSharp &&
            module.Kind == PackageModuleRecord::ModuleKind::Runtime)
            hasRuntimeCSharp = true;
    }
    if (!hasRuntimeCSharp)
        return true;

    // Managed C# runtime assemblies stage next to the project's compiled scripts
    // (the script domain loads Packages/*.dll beside GameEngine.Scripts.dll).
    // AOT builds compile package sources into the AOT csproj instead
    // (CompileScriptsNativeAOT), so there is nothing to stage here — this test
    // must stay the same predicate CompileScripts routes on, or a shipping
    // package either double-stages its C# or loses it entirely.
    std::error_code ec;
    const bool aotBuild = IsShipOptimizedPlayerBuildConfig(s.buildConfiguration);
    const fs::path managedOut = contentRoot / "Managed";
    if (aotBuild)
        return true;
    if (!fs::is_directory(managedOut, ec))
    {
        // No project scripts were compiled, so no script domain exists to load
        // package assemblies into (they anchor on GameEngine.Scripts.dll).
        m_Progress.warnings.push_back(
            "Enabled packages have C# runtime modules but the project has no scripts — "
            "package C# will not run in this game (add a project script to bootstrap the "
            "script domain)");
        return true;
    }

    const fs::path assembliesPackagesDir =
        EngineCore::GetInstance().GetScriptManager().PackageAssembliesDirectory(
            /*editorKind=*/false);
    return StagePackageManagedAssemblies(assembliesPackagesDir, managedOut,
                                         m_PackageRuntimeModules, m_Progress.errors,
                                         PlayerBuildConfigIncludesDebugSymbols(s.buildConfiguration));
}

// ---------------------------------------------------------------------------
// CopyAssets
// ---------------------------------------------------------------------------

bool BuildPipeline::CopyAssets(const fs::path& stagingDir, const AssetManifest& manifest)
{
    std::error_code ec;
    int copied = 0;

    for (const auto& entry : manifest.entries)
    {
        if (FailIfCancelled())
            return false;

        if (entry.sourcePath.empty())
            continue;

        // sourcePath may be relative to asset root or absolute.
        fs::path src = entry.sourcePath;
        if (src.is_relative())
        {
            const auto& assetRoot = EngineCore::GetInstance().GetResolvedAssetRoot();
            src = assetRoot / src;
        }

        if (!fs::exists(src, ec))
        {
            m_Progress.warnings.push_back("Asset not found: " + src.string());
            continue;
        }

        const fs::path dst = stagingDir / entry.outputPath;
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            m_Progress.warnings.push_back("Failed to copy: " + src.string() + " -> " + dst.string());
        }
        else
        {
            ++copied;
        }
    }

    Logger::Log::Info("Build: Copied {} assets", copied);
    return true;
}

// ---------------------------------------------------------------------------
// CopyEngineShaders
// ---------------------------------------------------------------------------

bool BuildPipeline::CopyEngineShaders(const fs::path& stagingDir)
{
    // Engine shaders are not GUID-tracked. Copy the entire Shaders directory the build staged
    // with the Editor executable that is running this build.
    const fs::path shaderSrc = PathUtils::GetInstallAssetsRoot() / "Shaders";
    if (!fs::is_directory(shaderSrc))
    {
        Logger::Log::Warning("Build: Engine shaders not found, skipping");
        return false;
    }

    // Shaders go into Assets/Shaders/ to match the Player's asset root layout.
    const fs::path dst = stagingDir / "Assets" / "Shaders";
    if (!CopyDirectoryRecursive(shaderSrc, dst, [this]() { return m_CancelRequested.load(); }))
        return FailIfCancelled(), false;
    Logger::Log::Info("Build: Copied engine shaders from '{}'", shaderSrc.string());
    return true;
}

// ---------------------------------------------------------------------------
// CopyThirdPartyNotices
// ---------------------------------------------------------------------------

bool BuildPipeline::CopyThirdPartyNotices(const fs::path& contentRoot, const BuildSettings& s)
{
    const fs::path src = ResolveThirdPartyNoticesSource(s);
    if (src.empty())
    {
        m_Progress.warnings.push_back("Third-party notices not found near the Editor or SDK");
        return false;
    }

    const fs::path dst = contentRoot / "ThirdPartyNotices";

    try
    {
        if (!CopyDirectoryRecursive(src, dst, [this]() { return m_CancelRequested.load(); }))
            return FailIfCancelled(), false;
    }
    catch (const std::exception& e)
    {
        m_Progress.warnings.push_back("Failed to copy third-party notices: " + std::string(e.what()));
        return false;
    }

    Logger::Log::Info("Build: Copied third-party notices from '{}'", src.string());
    return true;
}

// ---------------------------------------------------------------------------
// CopyRuntimeDependencies
// ---------------------------------------------------------------------------

bool BuildPipeline::CopyRuntimeDependencies(const fs::path& stagingDir, const BuildSettings& s)
{
    std::error_code ec;

    // Copy Player.exe from the build output.
    const fs::path playerBuildDir = s.projectRoot / ".Build" / "Player" / "build";
    fs::path playerExe;

    // Search common output locations.
    for (const auto& candidate : {
        playerBuildDir / s.buildConfiguration / "Player.exe",
        playerBuildDir / "Player.exe",
        playerBuildDir / "Debug" / "Player.exe",
        playerBuildDir / "Release" / "Player.exe",
    })
    {
        if (fs::exists(candidate, ec))
        {
            playerExe = candidate;
            break;
        }
    }

    if (playerExe.empty())
    {
        m_Progress.errors.push_back("Player.exe not found in build output");
        return false;
    }

    // Copy Player exe, renamed to the game name.
    const std::string exeName = s.playerConfig.gameName + ".exe";
    fs::copy_file(playerExe, stagingDir / exeName, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        m_Progress.errors.push_back("Failed to copy Player.exe: " + ec.message());
        return false;
    }

    // Ship separate symbols only when the requested Player configuration includes them.
    fs::path playerPdb = playerExe;
    playerPdb.replace_extension(".pdb");
    if (PlayerBuildConfigIncludesDebugSymbols(s.buildConfiguration) && fs::exists(playerPdb, ec))
    {
        const std::string pdbName = s.playerConfig.gameName + ".pdb";
        fs::copy_file(playerPdb, stagingDir / pdbName, fs::copy_options::overwrite_existing, ec);
        if (ec)
            Logger::Log::Warning("Build: Failed to copy PDB: {}", ec.message());
    }

    if (!fs::is_directory(s.runtimeDepsPath, ec))
    {
        m_Progress.errors.push_back("Runtime deps directory not found: " + s.runtimeDepsPath.string());
        return false;
    }

    // Curated engine runtime set (ship-safety C5) — needed by BOTH script modes:
    // Engine is a shared library, and GameEngine.Native is the C ABI shim managed
    // AND AOT scripts P/Invoke into. Replaces the old ship-everything *.dll glob;
    // a missing required entry fails the build here, not the game at launch.
    // Engine and GameEngine.Native come first from the runtime of the
    // configuration the player links (m_EngineRuntimeDir), shadowing the editor's
    // own copies. Their vcpkg dependencies: same CRT flavor as this editor, the
    // editor's own directory; cross-flavor, the vcpkg tree's flavor bin dirs in
    // search order (debug flavor: debug/bin, then the release bin for the
    // overlay's release-only ports) — never the editor dir, whose same-named DLLs
    // are the WRONG flavor (utf8proc.dll, lexbor.dll, … share names across
    // flavors).
    const std::vector<fs::path> engineDepDirs =
        EngineRuntimeSearchDirs(m_EngineRuntimeDir, m_CrossFlavorVcpkgBinDirs, s.runtimeDepsPath);
    if (engineDepDirs.empty())
    {
        m_Progress.errors.push_back("The engine runtime the player links was never resolved (ResolvePlayerEngineRuntime "
                                    "after the player's configure), so the export cannot tell which Engine to ship; "
                                    "this is a build pipeline defect, report it.");
        return false;
    }
    if (!StageCuratedDependencySet(engineDepDirs, stagingDir, EngineRuntimeDependencySet(),
                                   m_Progress.errors))
        return false;

    // NativeAOT scripts (produced by CompileScripts for every ship-optimized
    // config) are self-contained: no CoreCLR hosting, no managed assemblies.
    const bool aotBuild = fs::exists(stagingDir / kNativeAotScriptsLibraryName, ec);
    // CompileScripts (managed path) stages the user's compiled C# into <staging>/Managed;
    // a game with no C# scripts needs no CLR hosting or managed engine assemblies at all.
    const bool hasManagedScripts = fs::is_directory(stagingDir / "Managed", ec);

    if (aotBuild)
    {
        Logger::Log::Info("Build: NativeAOT scripts detected — skipping managed assembly staging");
    }
    else if (hasManagedScripts)
    {
        // CoreCLR hosting + the managed engine assemblies the Player loads, staged flat
        // next to the executable (PathResolver's contract), plus each assembly's sidecars.
        // Always the editor's own copies: nethost/hostfxr are Microsoft-built
        // (CRT-independent) and the assemblies are IL — no flavor to match.
        if (!StageCuratedDependencySet({s.runtimeDepsPath}, stagingDir, ManagedHostingDependencySet(),
                                       m_Progress.errors) ||
            !StageManagedRuntimeAssemblies(ScriptingPaths::ResolveEngineManagedDirectoryFrom(s.runtimeDepsPath),
                                           stagingDir, m_Progress.errors))
            return false;
    }
    else
    {
        Logger::Log::Info("Build: no managed scripts staged — skipping CLR hosting + managed assembly staging");
    }

    Logger::Log::Info("Build: Copied runtime dependencies (curated set)");
    return true;
}

// ---------------------------------------------------------------------------
// WriteGameConfig
// ---------------------------------------------------------------------------

bool BuildPipeline::WriteGameConfig(const fs::path& stagingDir, const BuildSettings& s)
{
    const fs::path configPath = stagingDir / "game.config";

    // Seed the shipped LOD default tier from the engine's live import settings
    // (the editor seeds these from the open project's SettingsStore). The Player
    // reads them back at boot; per-asset overrides ship in the manifest kv.
    GameConfig cfg = s.playerConfig;

    // scriptAssemblyPath must name what THIS build actually staged — the Player
    // loads exactly this file and never compiles. The default ("Managed/
    // UserScripts.dll") predates the pipeline and matches nothing; leaving it
    // (or any stale value) makes the packaged Player error at boot or, worse
    // historically, fall into the dev compile path.
    std::error_code scriptsEc;
    const std::string scriptsFileName(ScriptingPaths::kScriptsAssemblyFileName);
    if (fs::exists(stagingDir / "Managed" / scriptsFileName, scriptsEc))
        cfg.scriptAssemblyPath = "Managed/" + scriptsFileName;
    else
        cfg.scriptAssemblyPath.clear(); // no managed scripts staged (none, or NativeAOT)

    const LODImportSettings& lod = GetLODImportSettings();
    cfg.lod.autoGenerate = lod.AutoGenerateOnImport;
    cfg.lod.count = lod.Config.LodCount;
    cfg.lod.borderRule = static_cast<uint32_t>(lod.Config.BorderRule);
    for (uint32_t i = 0; i < 4u; ++i)
    {
        cfg.lod.ratios[i] = lod.Config.TargetRatios[i];
        cfg.lod.errors[i] = lod.Config.TargetError[i];
    }

    // Runtime LOD selection ships from the project's AUTHORED settings, not
    // from live engine state the way the import defaults above do: the settings
    // page and the debug server both poke RenderServices directly, so a
    // diagnostic tweak left standing in the session would otherwise bake into
    // the game.
    cfg.lodSelection = Rendering::LodProjectSettings::Load(s.projectRoot);

    // Engine-wide AA + render scale ship from the AUTHORED settings for the
    // same reason as the LOD selection above: the settings page and the debug
    // server poke RenderServices directly, so a diagnostic tweak left standing
    // in the session would otherwise bake into the game.
    cfg.renderQuality = Rendering::AntiAliasingProjectSettings::Load(s.projectRoot);

    // The game UI scale policy authored in the project settings.
    cfg.uiScale = UIScaleProjectSettings::Load(s.projectRoot);

    if (!SaveGameConfig(configPath, cfg))
    {
        m_Progress.errors.push_back("Failed to write game.config");
        return false;
    }
    Logger::Log::Info("Build: Wrote game.config (lod.autoGenerate={}, lodMode={}, lodErrorBudgetPx={})",
                      cfg.lod.autoGenerate, Rendering::ToString(cfg.lodSelection.SelectionMode),
                      cfg.lodSelection.EffectiveErrorBudgetPx());
    return true;
}

// ---------------------------------------------------------------------------
// StageAssetManifest
// ---------------------------------------------------------------------------

bool BuildPipeline::StageAssetManifest(const fs::path& contentRoot, const AssetManifest& manifest)
{
    // Ship the authoritative GUID<->path identity as flat, read-only JSONL
    // `.assetmanifest`s at each staged asset root: the project's (+ editor-mount
    // strays) at Assets/, and one per staged package at Packages/<alias>/Assets/ —
    // per-package manifests keep every mount an independent GUID namespace, and
    // the sibling layout keeps the shipped mounts non-nesting (the Player's
    // package mounts reject overlapping roots). The Player mounts each via a
    // read-only package mount (no filesystem scan, no SQLite) and resolves every
    // reference to the same GUID the editor baked. The format is the same
    // AssetStore_TextJsonl the editor's AssetDatabase.assetdb uses, so identity is
    // preserved exactly.
    //
    // Store per output root: "" = the project manifest.
    std::map<std::string, AssetDatabase::AssetStore_TextJsonl> stores;
    const std::string assetsPrefix = "Assets/";

    // Copy runtime settings from the same catalog the asset readers use.
    const auto& assetManager = EngineCore::GetInstance().GetAssetManager();
    const AssetRegistry& registry = assetManager.GetRegistry();
    const auto textureUsages = CollectPackagedTextureUsages(contentRoot, manifest, assetManager);
    uint32_t metadataAssets = 0;
    uint32_t fbxKvAssets = 0;

    for (const auto& entry : manifest.entries)
    {
        // outputPath is relative to the content root: "Assets/Models/Foo.glb" for
        // project/editor entries, "Packages/<alias>/Assets/..." for package
        // entries. Each mount roots at its own .../Assets, so strip that prefix
        // to recover the canonical path the editor keys identity by.
        std::string rel = entry.outputPath.generic_string();
        const bool isPackageEntry = !entry.sourceAlias.empty() &&
                                    entry.sourceAlias != kAssetSourceAliasProject &&
                                    entry.sourceAlias != kAssetSourceAliasEditor;
        if (isPackageEntry)
        {
            const std::string packagePrefix =
                "Packages/" + entry.sourceAlias + "/" + assetsPrefix;
            if (rel.rfind(packagePrefix, 0) == 0)
                rel.erase(0, packagePrefix.size());
        }
        else if (rel.rfind(assetsPrefix, 0) == 0)
        {
            rel.erase(0, assetsPrefix.size());
        }
        AssetDatabase::AssetStore_TextJsonl& store =
            stores[isPackageEntry ? entry.sourceAlias : std::string()];

        // Store the path FIELD case-preserved (NormalizeCanonicalPath:
        // forward-slash + lexically-normal, real on-disk case) — the packaged
        // Player loads the asset bytes from this path, and folding the FIELD
        // breaks loading on case-sensitive filesystems. LookupGuidByPath now
        // folds its INDEX key (FoldStorePathKey), so uppercase paths resolve;
        // the stored spelling is what must stay faithful to disk. The
        // path-hash derive folds the AUTHORED scene path at the derive site —
        // it never reads this field.
        AssetDatabase::AssetRecord record;
        record.guid = entry.guid;
        record.path = AssetDatabase::AssetStore_TextJsonl::NormalizeCanonicalPath(rel);
        record.type = entry.type; // typeId is derived from the enum by the writer

        std::string value;
        for (const auto& key : kRuntimeAssetMetadataKeys)
        {
            // Texture staging also resolves the material-derived usage below.
            if (key.Type == entry.type && key.Type != AssetType::Texture &&
                registry.TryGetMetaValue(entry.sourcePath, key.Name, value))
                record.kv[key.Name] = value;
        }

        // Preserve authored texture interpretation and sampler metadata. The
        // cook below and the Player resolve their inputs from these same bytes.
        if (entry.type == AssetType::Texture)
        {
            const auto usage = textureUsages.find(entry.guid);
            StageTextureImportMetadata(registry, entry,
                usage == textureUsages.end() ? TextureCookUsage::Auto : usage->second, record);
        }

        // Model geometry depends on loader options resolved from
        // ProjectSettings.json (NOT shipped) layered under per-asset kv.
        // Stage the RESOLVED bake/mirrors (and FBX ufbx flags) as explicit
        // per-asset kv so the packaged Player parses the same geometry the
        // cook did. Only when non-default, so default projects (Player also
        // defaults, including Mirror X on) ship no extra kv.
        const ModelFormat modelFormat = ModelAsset::FormatFromPath(entry.sourcePath);
        const bool isFbx = modelFormat == ModelFormat::FBX;
        if (entry.type == AssetType::Model && ModelAsset::UsesAxisImportOptions(modelFormat))
        {
            const FbxLoaderOptions resolved = ResolveFbxLoaderOptions(entry.sourcePath);
            const FbxLoaderOptions defaults{};
            float bakeDeg[3];
            resolved.Axis.EffectiveBakeRotationDeg(bakeDeg);
            const bool bakeNonZero =
                std::fabs(bakeDeg[0]) > 1.0e-5f ||
                std::fabs(bakeDeg[1]) > 1.0e-5f ||
                std::fabs(bakeDeg[2]) > 1.0e-5f;
            const bool bakeOn = resolved.Axis.AnyBakeRotationEnabled() || bakeNonZero;
            const bool mirrorsNonDefault =
                resolved.Axis.MirrorAxis[0] != defaults.Axis.MirrorAxis[0] ||
                resolved.Axis.MirrorAxis[1] != defaults.Axis.MirrorAxis[1] ||
                resolved.Axis.MirrorAxis[2] != defaults.Axis.MirrorAxis[2];
            const bool ufbxNonDefault = isFbx && (
                resolved.GenerateMissingTangents != defaults.GenerateMissingTangents ||
                resolved.CleanSkinWeights != defaults.CleanSkinWeights ||
                resolved.AdjustPivots != defaults.AdjustPivots ||
                resolved.PreserveGeometryTransforms != defaults.PreserveGeometryTransforms ||
                resolved.ImportEmbeddedTextures != defaults.ImportEmbeddedTextures);
            if (ufbxNonDefault || bakeOn || mirrorsNonDefault)
            {
                const auto boolStr = [](bool v) { return v ? std::string("true") : std::string("false"); };
                if (isFbx)
                {
                    record.kv[FbxLoaderOptions::kUseGlobalKey] = "false";
                    record.kv[FbxLoaderOptions::kGenerateMissingTangentsKey] = boolStr(resolved.GenerateMissingTangents);
                    record.kv[FbxLoaderOptions::kCleanSkinWeightsKey] = boolStr(resolved.CleanSkinWeights);
                    record.kv[FbxLoaderOptions::kAdjustPivotsKey] = boolStr(resolved.AdjustPivots);
                    record.kv[FbxLoaderOptions::kPreserveGeometryTransformsKey] = boolStr(resolved.PreserveGeometryTransforms);
                    record.kv[FbxLoaderOptions::kImportEmbeddedTexturesKey] = boolStr(resolved.ImportEmbeddedTextures);
                }
                if (bakeOn)
                {
                    char bake[96];
                    std::snprintf(bake, sizeof(bake), "%.4g,%.4g,%.4g",
                                  static_cast<double>(resolved.Axis.BakeRotationDeg[0]),
                                  static_cast<double>(resolved.Axis.BakeRotationDeg[1]),
                                  static_cast<double>(resolved.Axis.BakeRotationDeg[2]));
                    record.kv[FbxLoaderOptions::kBakeRotationKey] = bake;
                    char axes[8];
                    std::snprintf(axes, sizeof(axes), "%d,%d,%d",
                                  resolved.Axis.BakeRotationAxisEnabled[0] ? 1 : 0,
                                  resolved.Axis.BakeRotationAxisEnabled[1] ? 1 : 0,
                                  resolved.Axis.BakeRotationAxisEnabled[2] ? 1 : 0);
                    record.kv[FbxLoaderOptions::kBakeRotationAxesKey] = axes;
                }
                if (mirrorsNonDefault)
                {
                    char mirrors[8];
                    std::snprintf(mirrors, sizeof(mirrors), "%d,%d,%d",
                                  resolved.Axis.MirrorAxis[0] ? 1 : 0,
                                  resolved.Axis.MirrorAxis[1] ? 1 : 0,
                                  resolved.Axis.MirrorAxis[2] ? 1 : 0);
                    record.kv[FbxLoaderOptions::kMirrorAxesKey] = mirrors;
                }
                ++fbxKvAssets;
            }
        }

        if (!record.kv.empty())
            ++metadataAssets;
        store.UpsertAsset(record, nullptr);
    }

    // The project manifest always ships (even empty — the Player's packaged
    // mount detection keys off its presence); package manifests only where a
    // package staged assets.
    stores[std::string()];

    for (auto& [alias, store] : stores)
    {
        std::error_code ec;
        const fs::path assetsRoot = alias.empty()
                                        ? contentRoot / "Assets"
                                        : contentRoot / "Packages" / alias / "Assets";
        fs::create_directories(assetsRoot, ec);
        const fs::path manifestPath = assetsRoot / ".assetmanifest";

        std::string err;
        if (!store.SaveToFile(manifestPath, &err))
        {
            m_Progress.errors.push_back("Failed to write .assetmanifest: " + err);
            Logger::Log::Error("Build: Failed to write .assetmanifest '{}': {}",
                               manifestPath.generic_string(), err);
            return false;
        }
    }

    Logger::Log::Info(
        "Build: Wrote .assetmanifest x{} ({} entries total, {} with runtime settings, {} with resolved FBX options)",
        stores.size(), manifest.entries.size(), metadataAssets, fbxKvAssets);
    return true;
}

// ---------------------------------------------------------------------------
// CookMeshLODs
// ---------------------------------------------------------------------------

bool BuildPipeline::CookMeshLODs(const fs::path& contentRoot, const AssetManifest& manifest)
{
    // Bake a .gelod per LOD-enabled model into <content>/Assets/.lod/<guid>.gelod
    // — the sibling tree ModelAsset::PlayerLodCacheFile addresses at runtime. The
    // Player validates the header key + geometry bounds and consumes the cooked
    // indices with meshopt compiled out; a missing/invalid file degrades to
    // runtime generation (Phase-A behaviour). All work is at build time.
    if (!IsMeshLODGenerationAvailable())
    {
        Logger::Log::Info("Build: LOD cook skipped — meshoptimizer not compiled in");
        return true;
    }

    AssetManager& assetManager = EngineCore::GetInstance().GetAssetManager();
    const fs::path lodDir = contentRoot / "Assets" / ".lod";

    uint32_t cooked = 0;
    uint32_t skippedNoLods = 0;
    uint64_t totalBytes = 0;
    for (const auto& entry : manifest.entries)
    {
        if (entry.type != AssetType::Model || entry.sourcePath.empty())
            continue;

        // Resolve the SAME config the packaged Player resolves: per-asset kv
        // (staged into the manifest) layered over the global tier (game.config
        // seeds the identical global at Player boot). Only opt-in models cook.
        const ResolvedLodSettings lod = ResolveLodSettings(&assetManager, entry.sourcePath);
        if (!lod.Generate)
            continue;

        fs::path srcAbs = entry.sourcePath;
        if (srcAbs.is_relative())
            srcAbs = EngineCore::GetInstance().GetResolvedAssetRoot() / srcAbs;

        ModelAsset model(entry.guid, srcAbs);
        if (!model.Load())
        {
            m_Progress.warnings.push_back("LOD cook: failed to load " + srcAbs.string());
            continue;
        }

        const fs::path outFile = lodDir / (entry.guid.ToString() + ".gelod");
        const uint64_t bytes = model.CookLODCache(lod.Config, lod.GenerateSkinned, outFile);
        if (bytes > 0)
        {
            ++cooked;
            totalBytes += bytes;
        }
        else
        {
            ++skippedNoLods;
        }
    }

    Logger::Log::Info("Build: Cooked LOD cache for {} models ({} skipped, no LODs), {} bytes -> '{}'",
                      cooked, skippedNoLods, totalBytes, lodDir.generic_string());
    return true;
}

// ---------------------------------------------------------------------------
// StageTerrainBakes
// ---------------------------------------------------------------------------

void BuildPipeline::StageTerrainBakes(const fs::path& contentRoot, const BuildSettings& s,
                                      const AssetManifest& manifest)
{
    // The editor keeps one artifact per terrain, under the scene the terrain was
    // loaded from (TerrainBakeCache.h). Stage the folders of exactly the scenes this
    // package ships; every other scene's artifacts stay in the project cache.
    std::vector<GUID> scenes;
    std::unordered_map<GUID, std::string> scenePaths;
    for (const auto& entry : manifest.entries)
    {
        if (entry.type != AssetType::Scene)
            continue;
        scenes.push_back(entry.guid);
        scenePaths[entry.guid] = entry.sourcePath.generic_string();
    }
    std::vector<fs::path> failures;
    const auto staged = TerrainECS::StageTerrainBakeArtifacts(s.projectRoot / ".Cache" / "TerrainBake", scenes,
                                                              contentRoot / "Assets" / ".terrainbake", failures);
    for (const fs::path& failure : failures)
        m_Progress.warnings.push_back("Terrain bake staging: failed to copy " + failure.string());
    // The editor stores each terrain's current bake when its scene is saved, so a saved
    // scene's folder holds exactly its terrains.
    for (const TerrainECS::TerrainBakeStagedScene& scene : staged)
        if (scene.Artifacts > 0)
            Logger::Log::Info("Build: Scene '{}' ships {} baked terrain(s) ({} bytes)", scenePaths[scene.Scene],
                              scene.Artifacts, scene.Bytes);
}

// ---------------------------------------------------------------------------
// FinalizeOutput
// ---------------------------------------------------------------------------

bool BuildPipeline::FinalizeOutput(const fs::path& stagingDir, const fs::path& outputDir)
{
    if (!PromoteBuildOutput(stagingDir, outputDir, m_Progress.errors, m_Progress.warnings,
                            [this]() { return m_CancelRequested.load(); }))
    {
        FailIfCancelled();
        return false;
    }
    Logger::Log::Info("Build: Output finalized at '{}'", outputDir.string());
    return true;
}

} // namespace GameEngine
