// P2 package build staging — pure filesystem/data halves tested against fake
// staged dirs (same approach as NativeScriptStagingTests): native module
// staging into Packages/<alias>/NativeScripts, managed runtime assembly
// staging into Managed/Packages, packages.index emission, and the loud
// build-time validation matrix (disabled-package attribution, editor-assembly
// references, sourceless native modules, zero-staged rationale warnings).
// One test drives a real NativeScriptManager build (InFlightModuleBuild.h) to
// prove the export waits for an in-flight package module build before it stages.

#include "Engine/Build/PackageBuildStaging.h"

#include "InFlightModuleBuild.h"
#include "Assets/AssetRegistry.h"
#include "Assets/Packages/PackagesIndex.h"
#include "NativeScripting/BuildCacheRecord.h"
#include "NativeScripting/NativeBuildConfig.h"
#include "NativeScripting/NativeScriptManager.h"
#include "NativeScripting/PrebuiltModuleBinaries.h"
#include "NativeScripting/ToolchainFingerprint.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace ns = GameEngine::NativeScripting;
using namespace GameEngine;

namespace
{

constexpr const char* kEngineAbi = "aaaa111122223333";

// Digest provider for staging calls whose test doesn't exercise per-module
// defines: every module ships against the same engine digest.
std::function<GameEngine::EngineAbiDigestSet(const GameEngine::PackageCodeModule&)>
FixedAbi(const char* digest)
{
    return [digest](const GameEngine::PackageCodeModule&) {
        return GameEngine::EngineAbiDigestSet{digest, digest, ""};
    };
}

void WriteBytes(const fs::path& path, const char* bytes)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

bool AnyContains(const std::vector<std::string>& messages, std::string_view needle)
{
    for (const std::string& message : messages)
        if (message.find(needle) != std::string::npos)
            return true;
    return false;
}

// A resolved package rooted under `root` (no resolver run — built by hand the
// way PackageResolver would emit it).
ResolvedPackage MakePackage(const fs::path& root, const std::string& name)
{
    ResolvedPackage package;
    package.Manifest.Name = name;
    package.RootDir = root / name;
    package.AssetsDir = package.RootDir / "Assets";
    package.Alias = SanitizePackageAlias(name);
    package.Priority = 30;
    return package;
}

PackageCodeModule MakeModule(const ResolvedPackage& package,
                             PackageModuleRecord::ModuleLang lang,
                             const std::string& assemblyName)
{
    PackageCodeModule module;
    module.PackageName = package.Manifest.Name;
    module.AssemblyName = assemblyName;
    module.Kind = PackageModuleRecord::ModuleKind::Runtime;
    module.Lang = lang;
    module.RootDir = package.RootDir / "Native";
    module.PackageRootDir = package.RootDir;
    // The embedded-package writable cache (CollectPackageCodeModules derives
    // this; hand-built modules must match it for staging to find the build).
    module.CacheDir = package.RootDir / ".Cache";
    return module;
}

struct StagingFixture
{
    fs::path Root;
    fs::path ContentRoot;
    std::vector<std::string> Errors;
    std::vector<std::string> Warnings;

    explicit StagingFixture(const char* name)
    {
        Root = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(Root, ec);
        ContentRoot = Root / "staged-game";
        fs::create_directories(ContentRoot, ec);
    }

    ~StagingFixture()
    {
        std::error_code ec;
        fs::remove_all(Root, ec);
    }

    // Editor-built native module cache under the package root (the layout the
    // editor's per-package NativeBuildConfig produces).
    void WriteNativeBuild(const ResolvedPackage& package, const std::string& moduleName,
                          const char* abiDigest)
    {
        const fs::path buildDir = package.RootDir / ".Cache" / "NativeScripts" / "build";
        const fs::path dll =
            package.RootDir / ".Cache" / "NativeScripts" / "active" / (moduleName + "_1a2b3c4d.dll");
        WriteBytes(dll, "fake package module bytes");
        ASSERT_TRUE(ns::WriteBuildCacheRecord(
            buildDir, ns::BuildCacheRecord{"fulldigest", dll.generic_string(), abiDigest}));
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Native module staging
// ---------------------------------------------------------------------------

TEST(PackageBuildStaging, NativeModuleStagesUnderPackageAliasWithRelativeRecord)
{
    StagingFixture fx("ge_pbs_native_ok");
    const ResolvedPackage package = MakePackage(fx.Root, "ocean-pack");
    fx.WriteNativeBuild(package, "OceanPack", kEngineAbi);

    PackageResolution resolution;
    resolution.MountOrder.push_back(package);
    const std::vector<PackageCodeModule> modules = {
        MakeModule(package, PackageModuleRecord::ModuleLang::Cpp, "OceanPack")};

    ASSERT_TRUE(StagePackageNativeModules(fx.ContentRoot, resolution, modules,
                                          FixedAbi(kEngineAbi), fx.Errors, false))
        << (fx.Errors.empty() ? "" : fx.Errors[0]);

    const fs::path stagedDir = fx.ContentRoot / "Packages" / "ocean-pack" / "NativeScripts";
    EXPECT_TRUE(fs::exists(stagedDir / "OceanPack.dll"));

    // The record is RELATIVE and the engine_abi marker present — exactly what
    // LoadPrebuiltUserModule(<game>/Packages/ocean-pack, "OceanPack") validates.
    const auto record = ns::ReadBuildCacheRecord(stagedDir);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->DllPath, "NativeScripts/OceanPack.dll");
    EXPECT_EQ(record->AbiDigest, kEngineAbi);
    EXPECT_EQ(ns::ReadEngineAbiMarker(stagedDir), kEngineAbi);
}

// Git packages: the module's writable cache is the package cache's
// .derived/<entry> sibling (the entry itself is immutable), so staging must
// read the editor build from <CacheDir>/NativeScripts — not from a .Cache
// inside the entry payload.
TEST(PackageBuildStaging, GitModuleStagesFromDerivedCacheDir)
{
    StagingFixture fx("ge_pbs_native_git_derived");
    ResolvedPackage package = MakePackage(fx.Root, "ocean-pack");
    package.SourceKind = PackageSourceKind::Git;
    package.RootDir = fx.Root / "cache" / "ocean-pack@1.0.0-abcabcabcabc";
    package.AssetsDir = package.RootDir / "Assets";

    PackageCodeModule module = MakeModule(package, PackageModuleRecord::ModuleLang::Cpp, "OceanPack");
    module.CacheDir = fx.Root / "cache" / ".derived" / "ocean-pack@1.0.0-abcabcabcabc";

    const fs::path buildDir = module.CacheDir / "NativeScripts" / "build";
    const fs::path dll = module.CacheDir / "NativeScripts" / "active" / "OceanPack_1a2b3c4d.dll";
    WriteBytes(dll, "fake git module bytes");
    ASSERT_TRUE(ns::WriteBuildCacheRecord(
        buildDir, ns::BuildCacheRecord{"fulldigest", dll.generic_string(), kEngineAbi}));

    PackageResolution resolution;
    resolution.MountOrder.push_back(package);
    ASSERT_TRUE(StagePackageNativeModules(fx.ContentRoot, resolution, {module},
                                          FixedAbi(kEngineAbi), fx.Errors, false))
        << (fx.Errors.empty() ? "" : fx.Errors[0]);

    const fs::path stagedDir =
        fx.ContentRoot / "Packages" / package.Alias / "NativeScripts";
    EXPECT_TRUE(fs::exists(stagedDir / "OceanPack.dll"));
    const auto record = ns::ReadBuildCacheRecord(stagedDir);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->DllPath, "NativeScripts/OceanPack.dll");
    // Nothing was written inside the immutable entry payload.
    EXPECT_FALSE(fs::exists(package.RootDir / ".Cache"));
}

// P3: a shipped prebuilt with the host's fingerprint dir AND a matching
// engine_abi marker is staged verbatim — no editor build cache needed.
TEST(PackageBuildStaging, ValidPrebuiltIsPreferredOverEditorBuild)
{
    StagingFixture fx("ge_pbs_native_prebuilt");
    ResolvedPackage package = MakePackage(fx.Root, "ocean-pack");
    // NO editor build cache — the prebuilt must carry the whole module.

    PackageCodeModule module = MakeModule(package, PackageModuleRecord::ModuleLang::Cpp, "OceanPack");
    module.PrebuiltDir = package.RootDir / "Binaries";
    const fs::path platformDir =
        module.PrebuiltDir / ns::PrebuiltPlatformDirName(ns::HostToolchainFingerprint());
    WriteBytes(platformDir / ns::PrebuiltModuleFileName("OceanPack"), "prebuilt module bytes");
    WriteBytes(platformDir / "OceanPack.pdb", "prebuilt symbol bytes");
    ASSERT_TRUE(ns::WriteEngineAbiMarker(platformDir, kEngineAbi));

    PackageResolution resolution;
    resolution.MountOrder.push_back(package);
    ASSERT_TRUE(StagePackageNativeModules(fx.ContentRoot, resolution, {module},
                                          FixedAbi(kEngineAbi), fx.Errors, true))
        << (fx.Errors.empty() ? "" : fx.Errors[0]);

    const fs::path stagedDir = fx.ContentRoot / "Packages" / "ocean-pack" / "NativeScripts";
    EXPECT_TRUE(fs::exists(stagedDir / ns::PrebuiltModuleFileName("OceanPack")));
    EXPECT_TRUE(fs::exists(stagedDir / "OceanPack.pdb"));
    const auto record = ns::ReadBuildCacheRecord(stagedDir);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->DllPath, "NativeScripts/" + ns::PrebuiltModuleFileName("OceanPack"));
    EXPECT_EQ(record->AbiDigest, kEngineAbi);
    EXPECT_EQ(ns::ReadEngineAbiMarker(stagedDir), kEngineAbi);
}

// P3: a prebuilt without a marker (or with a mismatched one) cannot be
// verified at build time — staging falls back to the editor-built cache.
TEST(PackageBuildStaging, UnverifiablePrebuiltFallsBackToEditorBuild)
{
    StagingFixture fx("ge_pbs_native_prebuilt_bad");
    ResolvedPackage package = MakePackage(fx.Root, "ocean-pack");
    fx.WriteNativeBuild(package, "OceanPack", kEngineAbi); // editor cache = the fallback

    PackageCodeModule module = MakeModule(package, PackageModuleRecord::ModuleLang::Cpp, "OceanPack");
    module.PrebuiltDir = package.RootDir / "Binaries";
    const fs::path platformDir =
        module.PrebuiltDir / ns::PrebuiltPlatformDirName(ns::HostToolchainFingerprint());
    WriteBytes(platformDir / ns::PrebuiltModuleFileName("OceanPack"), "prebuilt WITHOUT marker");

    PackageResolution resolution;
    resolution.MountOrder.push_back(package);
    ASSERT_TRUE(StagePackageNativeModules(fx.ContentRoot, resolution, {module},
                                          FixedAbi(kEngineAbi), fx.Errors, false))
        << (fx.Errors.empty() ? "" : fx.Errors[0]);

    // The staged bytes are the editor build's, not the unverifiable prebuilt's.
    const fs::path stagedDll =
        fx.ContentRoot / "Packages" / "ocean-pack" / "NativeScripts" / "OceanPack.dll";
    ASSERT_TRUE(fs::exists(stagedDll));
    std::ifstream in(stagedDll, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(bytes, "fake package module bytes");
}

TEST(PackageBuildStaging, DeclaredNativeModuleWithoutEditorBuildFailsLoudly)
{
    StagingFixture fx("ge_pbs_native_nobuild");
    const ResolvedPackage package = MakePackage(fx.Root, "ocean-pack");
    // No .Cache/NativeScripts build — the editor never built the module.

    PackageResolution resolution;
    resolution.MountOrder.push_back(package);
    const std::vector<PackageCodeModule> modules = {
        MakeModule(package, PackageModuleRecord::ModuleLang::Cpp, "OceanPack")};

    EXPECT_FALSE(StagePackageNativeModules(fx.ContentRoot, resolution, modules,
                                           FixedAbi(kEngineAbi), fx.Errors, false));
    EXPECT_TRUE(AnyContains(fx.Errors, "ocean-pack"));
    EXPECT_TRUE(AnyContains(fx.Errors, "no built module"));
}

namespace
{

// One export started while the editor builds a package module. WaitingOrStaged
// turns true when the export thread reports a wait or has staged, so the main
// thread ticks only after the export reached one of the two. The export thread
// writes the rest and the test reads them after the join.
struct ModuleBuildExport
{
    std::atomic<bool> Cancel{false};
    std::atomic<bool> WaitingOrStaged{false};
    ns::NativeScriptManager::ModuleBuildWaitResult Wait;
    int BuildCompletionsAtStaging = -1;
    bool Staged = false;
    std::vector<std::vector<std::string>> WaitReports;
    std::vector<std::string> Errors;
};

// The export's two steps that meet the editor's module builds, in pipeline order:
// wait for them to settle (BuildPipeline::WaitForNativeModuleBuilds), then stage
// the records they wrote (step 11c).
void RunModuleBuildExport(ns::Testing::InFlightModuleBuild& build, const PackageResolution& resolution,
                          const std::vector<PackageCodeModule>& modules, const fs::path& contentRoot,
                          ModuleBuildExport& run)
{
    run.Wait = build.Manager().WaitForModuleBuilds(
        [&run] { return run.Cancel.load(); },
        [&run](const std::vector<std::string>& moduleNames) {
            run.WaitReports.push_back(moduleNames);
            run.WaitingOrStaged = true;
        });
    run.BuildCompletionsAtStaging = build.Completions();
    run.Staged = StagePackageNativeModules(contentRoot, resolution, modules, FixedAbi(kEngineAbi), run.Errors, false);
    run.WaitingOrStaged = true;
}

// How long the completion callback holds the completion task before it writes the
// record. Longer than the waiter's cancel-check period, so a waiter that could
// read the build state while a Tick runs would see the build done and stage
// before the record exists.
constexpr std::chrono::milliseconds kRecordWriteDelay{300};

} // namespace

// An export started while the editor still builds a package's native module
// waits for that build instead of failing with "no built module", and learns
// that the build failed. The completion callback writes a record (the last-good
// build an older run left) after kRecordWriteDelay, inside the main-thread task
// where a successful build writes its own.
TEST(PackageBuildStaging, ExportWaitsForAnInFlightPackageModuleBuild)
{
    StagingFixture fx("ge_pbs_native_wait_for_build");
    const ResolvedPackage package = MakePackage(fx.Root, "eztree");
    const PackageCodeModule module = MakeModule(package, PackageModuleRecord::ModuleLang::Cpp, "EZTree");
    PackageResolution resolution;
    resolution.MountOrder.push_back(package);
    const std::vector<PackageCodeModule> modules = {module};

    ns::Testing::InFlightModuleBuild build(module.RootDir, module.CacheDir, fx.Root / "scratch", "EZTree",
                                           ns::Testing::ModuleKind::Shipped,
                                           [&fx, &package] {
                                               std::this_thread::sleep_for(kRecordWriteDelay);
                                               fx.WriteNativeBuild(package, "EZTree", kEngineAbi);
                                           });
    ModuleBuildExport run;
    std::thread exportThread(RunModuleBuildExport, std::ref(build), std::cref(resolution), std::cref(modules),
                             std::cref(fx.ContentRoot), std::ref(run));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (!run.WaitingOrStaged.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    build.TickUntilCompleted(deadline);
    run.Cancel = true; // releases a waiter the deadline left behind
    exportThread.join();

    ASSERT_EQ(build.Completions(), 1) << "the module build never completed";
    EXPECT_EQ(run.Wait.Outcome, ns::NativeScriptManager::ModuleBuildWaitOutcome::Settled);
    EXPECT_EQ(run.Wait.FailedModules, std::vector<std::string>{"EZTree"});
    EXPECT_EQ(run.BuildCompletionsAtStaging, 1) << "the export staged before the module build completed";
    EXPECT_TRUE(run.Staged) << (run.Errors.empty() ? "" : run.Errors[0]);
    EXPECT_FALSE(AnyContains(run.Errors, "no built module"));
    ASSERT_EQ(run.WaitReports.size(), 1u);
    EXPECT_EQ(run.WaitReports[0], std::vector<std::string>{"EZTree"});
}

TEST(PackageBuildStaging, StaleNativeModuleAbiFailsLoudly)
{
    StagingFixture fx("ge_pbs_native_stale");
    const ResolvedPackage package = MakePackage(fx.Root, "ocean-pack");
    fx.WriteNativeBuild(package, "OceanPack", "deadbeefdeadbeef"); // built against another engine

    PackageResolution resolution;
    resolution.MountOrder.push_back(package);
    const std::vector<PackageCodeModule> modules = {
        MakeModule(package, PackageModuleRecord::ModuleLang::Cpp, "OceanPack")};

    EXPECT_FALSE(StagePackageNativeModules(fx.ContentRoot, resolution, modules,
                                           FixedAbi(kEngineAbi), fx.Errors, false));
    EXPECT_TRUE(AnyContains(fx.Errors, "stale"));
}

// P2.1 BUG 1: package defines enter the editor-side engine-ABI digest
// (HashEngineAbiInputs hashes CompileDefinitions), so package-time staging must
// recompute each module's digest with THAT module's defines. A defines-bearing
// module stamped by the editor must stage cleanly per-module — and must fail
// against the manifest-only digest (the pre-fix false positive, now inverted
// into the guard: mismatched defines = genuinely stale).
TEST(PackageBuildStaging, ModuleAbiDigestIncludesPackageDefines)
{
    StagingFixture fx("ge_pbs_abi_defines");
    const ResolvedPackage package = MakePackage(fx.Root, "ocean-pack");

    ns::NativeBuildConfig baseCfg;
    baseCfg.Config = "Debug";
    baseCfg.EngineImportLib = fx.Root / "Engine.lib"; // absent file hashes deterministically
    const std::string manifestOnlyDigest = ns::ComputeEngineAbiDigest(baseCfg);

    PackageCodeModule module = MakeModule(package, PackageModuleRecord::ModuleLang::Cpp, "OceanPack");
    module.Defines = {"GE_PACKAGE_OCEAN_PACK", "GE_PACKAGE_WATER_CORE"};

    // The digest the editor stamped: base config + the module's effective defines.
    ns::NativeBuildConfig moduleCfg = baseCfg;
    ns::AppendPackageDefines(moduleCfg, module.Defines);
    const std::string moduleDigest = ns::ComputeEngineAbiDigest(moduleCfg);
    ASSERT_NE(moduleDigest, manifestOnlyDigest) << "defines must be digest inputs";

    fx.WriteNativeBuild(package, "OceanPack", moduleDigest.c_str());

    PackageResolution resolution;
    resolution.MountOrder.push_back(package);
    const std::vector<PackageCodeModule> modules = {module};

    // Per-module recomputation (the fix): stages cleanly.
    ASSERT_TRUE(StagePackageNativeModules(
        fx.ContentRoot, resolution, modules,
        [&baseCfg](const PackageCodeModule& m) {
            ns::NativeBuildConfig cfg = baseCfg;
            ns::AppendPackageDefines(cfg, m.Defines);
            const std::string digest = ns::ComputeEngineAbiDigest(cfg);
            return EngineAbiDigestSet{digest, digest, ""};
        },
        fx.Errors, false))
        << (fx.Errors.empty() ? "" : fx.Errors[0]);
    EXPECT_EQ(ns::ReadEngineAbiMarker(fx.ContentRoot / "Packages" / "ocean-pack" / "NativeScripts"),
              moduleDigest);

    // Manifest-only digest (the pre-fix input): the C2 gate fires.
    EXPECT_FALSE(StagePackageNativeModules(fx.ContentRoot, resolution, modules,
                                           FixedAbi(manifestOnlyDigest.c_str()), fx.Errors, false));
    EXPECT_TRUE(AnyContains(fx.Errors, "stale"));
}

// ---------------------------------------------------------------------------
// Managed assembly staging
// ---------------------------------------------------------------------------

TEST(PackageBuildStaging, ManagedRuntimeAssembliesStageNextToScripts)
{
    StagingFixture fx("ge_pbs_managed_ok");
    const ResolvedPackage package = MakePackage(fx.Root, "ocean-pack");
    const fs::path assembliesDir = fx.Root / "ScriptAssemblies" / "Packages";
    WriteBytes(assembliesDir / "OceanPack.dll", "fake assembly");
    WriteBytes(assembliesDir / "OceanPack.pdb", "fake pdb");
    // Editor-kind output lives in Packages/Editor and must never be staged.
    WriteBytes(assembliesDir / "Editor" / "OceanPack.Editor.dll", "editor assembly");

    const fs::path managedOut = fx.ContentRoot / "Managed";
    const std::vector<PackageCodeModule> modules = {
        MakeModule(package, PackageModuleRecord::ModuleLang::CSharp, "OceanPack")};

    ASSERT_TRUE(StagePackageManagedAssemblies(assembliesDir, managedOut, modules, fx.Errors, true))
        << (fx.Errors.empty() ? "" : fx.Errors[0]);

    EXPECT_TRUE(fs::exists(managedOut / "Packages" / "OceanPack.dll"));
    EXPECT_TRUE(fs::exists(managedOut / "Packages" / "OceanPack.pdb"));
    EXPECT_FALSE(fs::exists(managedOut / "Packages" / "Editor"));
    EXPECT_FALSE(fs::exists(managedOut / "Packages" / "OceanPack.Editor.dll"));
}

TEST(PackageBuildStaging, MissingCompiledRuntimeAssemblyFailsLoudly)
{
    StagingFixture fx("ge_pbs_managed_missing");
    const ResolvedPackage package = MakePackage(fx.Root, "ocean-pack");
    const fs::path assembliesDir = fx.Root / "ScriptAssemblies" / "Packages"; // empty

    const std::vector<PackageCodeModule> modules = {
        MakeModule(package, PackageModuleRecord::ModuleLang::CSharp, "OceanPack")};

    EXPECT_FALSE(StagePackageManagedAssemblies(assembliesDir, fx.ContentRoot / "Managed",
                                               modules, fx.Errors, false));
    EXPECT_TRUE(AnyContains(fx.Errors, "OceanPack.dll"));
    EXPECT_TRUE(AnyContains(fx.Errors, "never compiled"));
}

// A module without sources never compiles: its error points at the declared
// prebuilt assembly the editor stages, not at a compile.
TEST(PackageBuildStaging, MissingPrebuiltRuntimeAssemblyNamesTheDeclaredDll)
{
    StagingFixture fx("ge_pbs_managed_prebuilt_missing");
    const ResolvedPackage package = MakePackage(fx.Root, "closed-pack");
    const fs::path assembliesDir = fx.Root / "ScriptAssemblies" / "Packages"; // empty

    PackageCodeModule module = MakeModule(package, PackageModuleRecord::ModuleLang::CSharp, "ClosedPack");
    module.RootDir.clear();
    module.PrebuiltDir = package.RootDir / "Binaries";

    EXPECT_FALSE(StagePackageManagedAssemblies(assembliesDir, fx.ContentRoot / "Managed", {module}, fx.Errors, false));
    EXPECT_TRUE(AnyContains(fx.Errors, "no prebuilt assembly staged"));
    EXPECT_TRUE(AnyContains(fx.Errors, (module.PrebuiltDir / "ClosedPack.dll").generic_string()));
    EXPECT_FALSE(AnyContains(fx.Errors, "never compiled"));
}

// ---------------------------------------------------------------------------
// packages.index
// ---------------------------------------------------------------------------

TEST(PackageBuildStaging, IndexListsPackagesWithStagedFootprintOnly)
{
    StagingFixture fx("ge_pbs_index");
    const ResolvedPackage withAssets = MakePackage(fx.Root, "content-pack");
    const ResolvedPackage withNative = MakePackage(fx.Root, "ocean-pack");
    const ResolvedPackage emptyPack = MakePackage(fx.Root, "unused-pack");

    PackageResolution resolution;
    resolution.MountOrder = {withAssets, withNative, emptyPack};
    const std::vector<PackageCodeModule> modules = {
        MakeModule(withNative, PackageModuleRecord::ModuleLang::Cpp, "OceanPack")};

    AssetManifest manifest;
    AssetManifestEntry entry;
    entry.outputPath = "Packages/content-pack/Assets/foo.png";
    entry.sourceAlias = "content-pack";
    manifest.entries.push_back(entry);

    ASSERT_TRUE(StagePackagesIndex(fx.ContentRoot, manifest, resolution, modules, fx.Errors));

    PackagesIndex index;
    std::string error;
    ASSERT_TRUE(TryLoadPackagesIndex(fx.ContentRoot / "Packages" / kPackagesIndexFileName,
                                     index, error))
        << error;
    ASSERT_EQ(index.Packages.size(), 2u); // unused-pack staged nothing
    EXPECT_EQ(index.Packages[0].Name, "content-pack");
    EXPECT_TRUE(index.Packages[0].NativeModules.empty());
    EXPECT_EQ(index.Packages[1].Name, "ocean-pack");
    ASSERT_EQ(index.Packages[1].NativeModules.size(), 1u);
    EXPECT_EQ(index.Packages[1].NativeModules[0], "OceanPack");
}

TEST(PackageBuildStaging, NoStagedPackagesWritesNoIndex)
{
    StagingFixture fx("ge_pbs_index_empty");
    PackageResolution resolution;
    AssetManifest manifest;

    ASSERT_TRUE(StagePackagesIndex(fx.ContentRoot, manifest, resolution, {}, fx.Errors));
    EXPECT_FALSE(fs::exists(fx.ContentRoot / "Packages" / kPackagesIndexFileName));
}

// ---------------------------------------------------------------------------
// Validation matrix
// ---------------------------------------------------------------------------

TEST(PackageBuildStaging, ReferenceToDisabledPackageAssetIsAttributedError)
{
    StagingFixture fx("ge_pbs_val_disabled");
    ResolvedPackage disabled = MakePackage(fx.Root, "heavy-pack");
    WriteBytes(disabled.AssetsDir / "Models" / "rock.glb", "GLB");

    PackageResolution resolution;
    resolution.Disabled.push_back(disabled);

    // The GUID a mounted 'heavy-pack' source would have derived for the file —
    // what a scene that referenced it while the package was enabled recorded.
    AssetManifest manifest;
    manifest.unresolvedDependencies.push_back(
        AssetRegistry::DeriveGuidForSourcePath("heavy-pack", "Models/rock.glb"));

    EXPECT_FALSE(ValidatePackagedBuild(manifest, resolution, {}, {}, fx.Errors, fx.Warnings));
    ASSERT_EQ(fx.Errors.size(), 1u);
    EXPECT_TRUE(AnyContains(fx.Errors, "DISABLED package"));
    EXPECT_TRUE(AnyContains(fx.Errors, "heavy-pack"));
    EXPECT_TRUE(AnyContains(fx.Errors, "Models/rock.glb"));
}

TEST(PackageBuildStaging, CollectedEntryOwnedByDisabledPackageIsAttributedError)
{
    // The live-editor shape: the disabled package is STILL MOUNTED in-session,
    // so its GUIDs resolve and the collector emits a normal entry (alias set,
    // nothing unresolved). Ownership validation must catch it anyway.
    StagingFixture fx("ge_pbs_val_disabled_mounted");
    ResolvedPackage disabled = MakePackage(fx.Root, "heavy-pack");

    PackageResolution resolution;
    resolution.Disabled.push_back(disabled);

    AssetManifest manifest;
    AssetManifestEntry entry;
    entry.guid = AssetRegistry::DeriveGuidForSourcePath("heavy-pack", "Textures/rock.png");
    entry.sourceAlias = "heavy-pack";
    entry.outputPath = "Packages/heavy-pack/Assets/Textures/rock.png";
    manifest.entries.push_back(entry);

    EXPECT_FALSE(ValidatePackagedBuild(manifest, resolution, {}, {}, fx.Errors, fx.Warnings));
    ASSERT_EQ(fx.Errors.size(), 1u);
    EXPECT_TRUE(AnyContains(fx.Errors, "DISABLED package"));
    EXPECT_TRUE(AnyContains(fx.Errors, "heavy-pack"));
    EXPECT_TRUE(AnyContains(fx.Errors, "Textures/rock.png"));
    EXPECT_TRUE(AnyContains(fx.Errors, "still mounted"));
}

TEST(PackageBuildStaging, DisabledPackageStillMountedInSessionIsAttributedError)
{
    // P2.3: the silent-drop shape — a package disabled mid-session. No
    // collected entry and no unresolved GUID references it (its assets only
    // ever shipped via the MountOrder-gated package step), so the two
    // reference nets are blind; the session-mount check alone must fail the
    // build with attribution.
    StagingFixture fx("ge_pbs_val_disabled_session");
    ResolvedPackage disabled = MakePackage(fx.Root, "heavy-pack");

    PackageResolution resolution;
    resolution.Disabled.push_back(disabled);

    EXPECT_FALSE(
        ValidatePackagedBuild({}, resolution, {}, {"heavy-pack"}, fx.Errors, fx.Warnings));
    ASSERT_EQ(fx.Errors.size(), 1u);
    EXPECT_TRUE(AnyContains(fx.Errors, "DISABLED"));
    EXPECT_TRUE(AnyContains(fx.Errors, "heavy-pack"));
    EXPECT_TRUE(AnyContains(fx.Errors, "still mounted"));

    // Restart shape: the mount is gone, the disable is coherent — building
    // without the package is exactly what the user asked for.
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    EXPECT_TRUE(ValidatePackagedBuild({}, resolution, {}, {}, errors, warnings));
    EXPECT_TRUE(errors.empty());
}

TEST(PackageBuildStaging, UnresolvedReferencesWithFailedResolutionCarryResolverErrors)
{
    StagingFixture fx("ge_pbs_val_failedres");
    PackageResolution resolution;
    resolution.Errors.push_back("package 'broken-pack': dependency 'missing-dep' not found");

    AssetManifest manifest;
    manifest.unresolvedDependencies.push_back(
        AssetRegistry::DeriveGuidForSourcePath("broken-pack", "whatever.png"));

    EXPECT_FALSE(ValidatePackagedBuild(manifest, resolution, {}, {}, fx.Errors, fx.Warnings));
    EXPECT_TRUE(AnyContains(fx.Errors, "could not be resolved"));
    EXPECT_TRUE(AnyContains(fx.Errors, "missing-dep"));
}

TEST(PackageBuildStaging, RuntimeCSharpModuleReferencingEditorAssemblyIsError)
{
    StagingFixture fx("ge_pbs_val_editor_ref");
    const ResolvedPackage package = MakePackage(fx.Root, "ocean-pack");
    PackageCodeModule module = MakeModule(package, PackageModuleRecord::ModuleLang::CSharp,
                                          "OceanPack");
    module.DependencyAssemblies = {"WaterCore", "WaterCore.Editor"};

    PackageResolution resolution;
    resolution.MountOrder.push_back(package);

    EXPECT_FALSE(ValidatePackagedBuild({}, resolution, {module}, {}, fx.Errors, fx.Warnings));
    EXPECT_TRUE(AnyContains(fx.Errors, "WaterCore.Editor"));
    EXPECT_TRUE(AnyContains(fx.Errors, "editor assemblies never ship"));
}

TEST(PackageBuildStaging, NativeModuleWithoutSourcesOrPrebuiltIsError)
{
    StagingFixture fx("ge_pbs_val_native_missing");
    const ResolvedPackage package = MakePackage(fx.Root, "ocean-pack");
    PackageCodeModule module = MakeModule(package, PackageModuleRecord::ModuleLang::Cpp,
                                          "OceanPack");
    // module.RootDir points at a directory that was never created.

    PackageResolution resolution;
    resolution.MountOrder.push_back(package);

    EXPECT_FALSE(ValidatePackagedBuild({}, resolution, {module}, {}, fx.Errors, fx.Warnings));
    EXPECT_TRUE(AnyContains(fx.Errors, "neither sources"));
}

TEST(PackageBuildStaging, EnabledPackageShippingNothingWarnsWithRationale)
{
    // An unmounted package had no registry for the package collection to read,
    // so staging nothing means its source never mounted this session — the
    // rationale the warning must state.
    StagingFixture fx("ge_pbs_val_rationale");
    const ResolvedPackage silent = MakePackage(fx.Root, "quiet-pack");
    const ResolvedPackage shipped = MakePackage(fx.Root, "content-pack");

    PackageResolution resolution;
    resolution.MountOrder = {silent, shipped};

    AssetManifest manifest;
    AssetManifestEntry entry;
    entry.sourceAlias = "content-pack";
    entry.outputPath = "Packages/content-pack/Assets/foo.png";
    manifest.entries.push_back(entry);

    EXPECT_TRUE(ValidatePackagedBuild(manifest, resolution, {}, {}, fx.Errors, fx.Warnings));
    EXPECT_TRUE(fx.Errors.empty());
    ASSERT_EQ(fx.Warnings.size(), 1u);
    EXPECT_TRUE(AnyContains(fx.Warnings, "quiet-pack"));
    EXPECT_TRUE(AnyContains(fx.Warnings, "not mounted"));
}

TEST(PackageBuildStaging, MountedPackageWithNoShippableContentIsNotAWarning)
{
    // An editor-only package (editor chrome and nothing a built scene reaches)
    // mounts and stages nothing. Nothing is missing from the build, so the
    // not-mounted remediation must not be offered for it.
    StagingFixture fx("ge_pbs_val_editor_only");
    const ResolvedPackage editorOnly = MakePackage(fx.Root, "chrome-pack");

    PackageResolution resolution;
    resolution.MountOrder = {editorOnly};

    EXPECT_TRUE(ValidatePackagedBuild({}, resolution, {}, {"chrome-pack"}, fx.Errors, fx.Warnings));
    EXPECT_TRUE(fx.Errors.empty());
    EXPECT_TRUE(fx.Warnings.empty()) << fx.Warnings.front();
}

TEST(PackageBuildStaging, CodeOnlyPackageStagingNothingIsNotAWarning)
{
    // A code-only package (empty manifest "assets" → empty AssetsDir, e.g.
    // unity-import) never mounts an asset source and never stages assets; the
    // zero-staged rationale warning does not apply to it.
    StagingFixture fx("ge_pbs_val_code_only");
    ResolvedPackage codeOnly = MakePackage(fx.Root, "tool-pack");
    codeOnly.AssetsDir.clear();

    PackageResolution resolution;
    resolution.MountOrder = {codeOnly};

    EXPECT_TRUE(ValidatePackagedBuild({}, resolution, {}, {}, fx.Errors, fx.Warnings));
    EXPECT_TRUE(fx.Errors.empty());
    EXPECT_TRUE(fx.Warnings.empty()) << fx.Warnings.front();
}

// ---------------------------------------------------------------------------
// Output layout mapping (the collision fix: per-alias subtrees)
// ---------------------------------------------------------------------------

TEST(PackageBuildStaging, SameRelativePathsFromDifferentMountsCannotCollide)
{
    const fs::path a = AssetCollector::PackageAssetOutputPath("pkg-a", "Textures/foo.png");
    const fs::path b = AssetCollector::PackageAssetOutputPath("pkg-b", "Textures/foo.png");
    EXPECT_NE(a, b);
    EXPECT_EQ(a.generic_string(), "Packages/pkg-a/Assets/Textures/foo.png");
    // ...and neither collides with the project's flat Assets/ tree.
    EXPECT_NE(a.generic_string().rfind("Assets/", 0), 0u);
}

TEST(PackageBuildStaging, OmitsPrebuiltDebugSymbolsUnlessRequested)
{
    StagingFixture fx("ge_pbs_prebuilt_no_symbols");
    const ResolvedPackage package = MakePackage(fx.Root, "ocean-pack");
    PackageCodeModule module = MakeModule(package, PackageModuleRecord::ModuleLang::Cpp, "OceanPack");
    module.PrebuiltDir = package.RootDir / "Binaries";
    const fs::path platformDir = module.PrebuiltDir / ns::PrebuiltPlatformDirName(ns::HostToolchainFingerprint());
    WriteBytes(platformDir / ns::PrebuiltModuleFileName("OceanPack"), "prebuilt module bytes");
    WriteBytes(platformDir / "OceanPack.pdb", "prebuilt symbol bytes");
    ASSERT_TRUE(ns::WriteEngineAbiMarker(platformDir, kEngineAbi));
    PackageResolution resolution;
    resolution.MountOrder.push_back(package);
    ASSERT_TRUE(StagePackageNativeModules(fx.ContentRoot, resolution, {module}, FixedAbi(kEngineAbi), fx.Errors, false));
    const fs::path staged = fx.ContentRoot / "Packages/ocean-pack/NativeScripts";
    EXPECT_TRUE(fs::exists(staged / ns::PrebuiltModuleFileName("OceanPack")));
    EXPECT_FALSE(fs::exists(staged / "OceanPack.pdb"));
}

TEST(PackageBuildStaging, OmitsManagedDebugSymbolsUnlessRequested)
{
    StagingFixture fx("ge_pbs_managed_no_symbols");
    const ResolvedPackage package = MakePackage(fx.Root, "ocean-pack");
    const fs::path assemblies = fx.Root / "ScriptAssemblies/Packages";
    WriteBytes(assemblies / "OceanPack.dll", "assembly bytes");
    WriteBytes(assemblies / "OceanPack.pdb", "symbol bytes");
    const std::vector<PackageCodeModule> modules = {
        MakeModule(package, PackageModuleRecord::ModuleLang::CSharp, "OceanPack")};
    ASSERT_TRUE(StagePackageManagedAssemblies(assemblies, fx.ContentRoot / "Managed", modules, fx.Errors, false));
    EXPECT_TRUE(fs::exists(fx.ContentRoot / "Managed/Packages/OceanPack.dll"));
    EXPECT_FALSE(fs::exists(fx.ContentRoot / "Managed/Packages/OceanPack.pdb"));
}
