// C10 end-to-end integration test: generate → compile → load → register.
//
// Drives NativeScriptManager::BuildAndLoad on a freshly-written fixture component:
// it generates the standalone user CMakeLists, invokes a REAL cmake configure+build
// (in the user project's own build dir — never touching the engine build dir),
// loads the produced DLL, and asserts the component is visible in the engine's
// shared registries. This is the rigorous proof of the whole C10 pipeline.
//
// Heavyweight: it shells out to cmake + the MSVC toolchain, so it is slower than a
// unit test (longer ctest timeout). The engine build interface (cmake path, Engine
// import lib, include dirs, compile defs, SDK paths, config) is supplied by CMake
// via a generated `c10_config.txt` placed next to this test exe — see CMakeLists.

#include "Core/Application.h" // PathUtils::GetExecutableDirectory
#include "ECS/ComponentFactory.h"
#include "ECS/ComponentFieldRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h" // drives the async build path (B9 deferral test)
#include "NativeScripting/BuildCacheRecord.h" // packaged record + engine_abi marker (C1/C2 ship path)
#include "NativeScripting/NativeBuildConfig.h"
#include "NativeScripting/NativeScriptManager.h"
#include "NativeScripting/PrebuiltModuleBinaries.h" // P3 fingerprint-keyed prebuilt layout
#include "NativeScripting/ToolchainFingerprint.h"
#include "NativeScripting/UserSystemRegistry.h" // per-module registration asserts (P1)

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ns = GameEngine::NativeScripting;
namespace ge = GameEngine::ECS;
namespace fs = std::filesystem;

namespace
{
std::vector<std::string> SplitList(const std::string& v, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(v);
    while (std::getline(in, cur, sep))
        if (!cur.empty())
            out.push_back(cur);
    return out;
}

// Parse the CMake-generated key=value config (one entry per line) next to the exe.
std::map<std::string, std::string> ReadConfigFile(const fs::path& path, std::string& err)
{
    std::map<std::string, std::string> kv;
    std::ifstream in(path);
    if (!in)
    {
        err = "cannot open config file " + path.string();
        return kv;
    }
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}
// The common NativeBuildConfig for a fixture rooted at `root` (sources in
// root/<sourceSubdir>, build in root/build). Callers add scanner fields + write sources.
ns::NativeBuildConfig MakeBaseConfig(const std::map<std::string, std::string>& kv, const fs::path& root,
                                     const std::string& sourceSubdir)
{
    ns::NativeBuildConfig config;
    config.SourceDir = root / sourceSubdir;
    config.BuildDir = root / "build";
    config.ActiveDir = root / "active";
    config.CMakeExe = kv.at("cmake");
    config.Config = kv.at("config");
    config.EngineImportLib = kv.at("importlib");
    config.SdkEntrySource = kv.at("sdkentry");
    config.ModuleName = "UserScripts";
    for (const auto& d : SplitList(kv.at("includes"), ';'))
        config.IncludeDirs.emplace_back(d);
    config.IncludeDirs.emplace_back(kv.at("sdkinc"));
    for (const auto& d : SplitList(kv.count("defs") ? kv.at("defs") : "", ';'))
        config.CompileDefinitions.push_back(d);
    for (const auto& d : SplitList(kv.count("globaldefs") ? kv.at("globaldefs") : "", ';'))
        config.CompileDefinitions.push_back(d);
    return config;
}

std::map<std::string, std::string> LoadConfigOrFail()
{
    const fs::path exeDir = GameEngine::PathUtils::GetExecutableDirectory();
    std::string cfgErr;
    auto kv = ReadConfigFile(exeDir / "c10_config.txt", cfgErr);
    EXPECT_TRUE(cfgErr.empty()) << cfgErr;
    for (const char* required : {"cmake", "importlib", "config", "sdkinc", "sdkentry", "workdir", "includes"})
        EXPECT_TRUE(kv.count(required)) << "missing config key: " << required;
    return kv;
}

// Run BuildAndLoad for `config` and assert it fully succeeded, dumping the build log
// tail on failure. `outResult` (optional) receives the result for follow-up asserts.
void RunAndExpectSuccess(ns::NativeScriptManager& mgr, const ns::NativeBuildConfig& config,
                         ns::NativeBuildResult* outResult = nullptr)
{
    const ns::NativeBuildResult result = mgr.BuildAndLoad(config);
    if (outResult)
        *outResult = result;
    ASSERT_TRUE(result.Success())
        << "BuildAndLoad failed: " << result.Error
        << "\n(configured=" << result.Configured << " compiled=" << result.Compiled
        << " loaded=" << result.Loaded << " registered=" << result.Registered << ")"
        << "\n--- build output (tail) ---\n"
        << (result.Output.size() > 4000 ? result.Output.substr(result.Output.size() - 4000) : result.Output);
}

// C6: the built module must carry symbols in EVERY config (Debug via CMake defaults,
// non-Debug via the generated /Zi;/Zo;/DEBUG block) — assert the .pdb sits beside the
// built DLL. Walks the user build dir because the generator's config subdir varies
// (Ninja vs VS); the newest module DLL is the one just built.
void ExpectPdbBesideBuiltDll(const ns::NativeBuildConfig& config)
{
#if defined(_WIN32)
    fs::path builtDll;
    fs::file_time_type newest{};
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(config.BuildDir, ec);
         !ec && it != fs::recursive_directory_iterator(); ++it)
    {
        if (it->path().extension() != ".dll" ||
            it->path().stem().generic_string().find(config.ModuleName) == std::string::npos)
            continue;
        const auto t = fs::last_write_time(it->path(), ec);
        if (!ec && (builtDll.empty() || t > newest))
        {
            builtDll = it->path();
            newest = t;
        }
    }
    ASSERT_FALSE(builtDll.empty()) << "no built module DLL under " << config.BuildDir.string();
    fs::path pdb = builtDll;
    pdb.replace_extension(".pdb");
    EXPECT_TRUE(fs::exists(pdb, ec)) << "user module built without symbols: missing " << pdb.string();
#else
    (void)config;
#endif
}
} // namespace

// Explicit-macro path: a .cpp with GE_REGISTER_COMPONENT compiles + loads + registers.
TEST(NativeUserModuleBuildLoad, CompilesAndRegistersAComponentViaMacro)
{
    const auto kv = LoadConfigOrFail();
    const fs::path root = fs::path(kv.at("workdir")) / "explicit";
    // No scanner configured → macros-only mode.
    ns::NativeBuildConfig config = MakeBaseConfig(kv, root, "Gameplay");
    // Exercise the SDK-rooted path shape end-to-end through a REAL cmake configure:
    // the GameSDK dir contains both sdkinc (Include/) and sdkentry (Source/...), so the
    // generated project references them via ${GAMEENGINE_SDK_DIR} and the configure
    // receives -DGAMEENGINE_SDK_DIR (the scan-path test below keeps the absolute-path
    // fallback shape covered).
    config.SdkRoot = fs::path(kv.at("sdkinc")).parent_path();
    const fs::path sourceDir = config.SourceDir;
    std::error_code ec;
    fs::remove_all(root, ec); // clean slate each run
    fs::create_directories(sourceDir, ec);
    ASSERT_FALSE(ec) << "cannot create fixture dir " << sourceDir.string() << ": " << ec.message();

    {
        std::ofstream src(sourceDir / "SampleHealth.cpp", std::ios::trunc);
        ASSERT_TRUE(src.is_open());
        src << "#include <GameSDK/GameSDK.h>\n"
            << "namespace ge_c10 { struct SampleHealth { float Current = 42.0f; int Lives = 3; }; }\n"
            << "GE_REGISTER_COMPONENT(ge_c10::SampleHealth, Current, Lives);\n";
    }

    EXPECT_EQ(ge::ComponentFieldRegistry::FindByName("SampleHealth"), 0u); // unknown before build

    ns::NativeScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(nullptr));
    ns::NativeBuildResult buildResult;
    RunAndExpectSuccess(mgr, config, &buildResult);
    EXPECT_EQ(buildResult.ModuleId, config.ModuleName);
    EXPECT_TRUE(buildResult.RegistrationsChanged);

    const ns::NativeBuildResult cachedResult = mgr.BuildAndLoad(config);
    ASSERT_TRUE(cachedResult.Success()) << cachedResult.Error;
    EXPECT_EQ(cachedResult.ModuleId, config.ModuleName);
    EXPECT_FALSE(cachedResult.RegistrationsChanged)
        << "re-reporting an already mapped cached module must not trigger inspector refreshes";

    const ge::ComponentTypeId id = ge::ComponentFieldRegistry::FindByName("SampleHealth");
    EXPECT_NE(id, 0u) << "user component not visible after load";
    EXPECT_TRUE(ge::ComponentFactory::Has(id)) << "user component has no factory entry";
    EXPECT_EQ(ge::ComponentFieldRegistry::Get(id).size(), 2u) << "expected fields Current + Lives";
    ExpectPdbBesideBuiltDll(config);

    // --- C1/C2 packaged-game ship path --------------------------------------
    // Stage the just-built module exactly the way BuildPipeline packages a
    // Windows/Linux game: a flat <game>/NativeScripts/ dir holding the DLL, a
    // RELATIVE record, and the engine_abi marker. The Player-side loader must
    // accept the matching package — and refuse the very same package when the
    // shipped engine identity differs (a stale user DLL against a patched engine
    // is silent UB otherwise). Reuses this test's real DLL instead of paying for
    // another cmake build.
    const fs::path builtDll = buildResult.LoadedModulePath;
    ASSERT_FALSE(builtDll.empty());
    const std::string engineAbi = ns::ComputeEngineAbiDigest(config);
    const fs::path pkgRoot = root / "packaged-game";
    const fs::path pkgScripts = pkgRoot / "NativeScripts";
    fs::create_directories(pkgScripts, ec);
    const std::string dllName = builtDll.filename().string();
    fs::copy_file(builtDll, pkgScripts / dllName, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << "failed to stage built DLL into the packaged layout: " << ec.message();
    ASSERT_TRUE(ns::WriteBuildCacheRecord(
        pkgScripts, ns::BuildCacheRecord{"packaged", "NativeScripts/" + dllName, engineAbi}));
    ASSERT_TRUE(ns::WriteEngineAbiMarker(pkgScripts, engineAbi));
    EXPECT_TRUE(mgr.LoadPrebuiltUserModule(pkgRoot))
        << "packaged module whose record matches the shipped engine_abi marker must load";

    ASSERT_TRUE(ns::WriteEngineAbiMarker(pkgScripts, "deadbeefdeadbeef"));
    EXPECT_FALSE(mgr.LoadPrebuiltUserModule(pkgRoot))
        << "the same package against a different shipped engine identity must be refused";

    // mgr is destroyed here but the loaded DLL is intentionally leaked (kept mapped),
    // so there is no teardown unload + no dangling-registry SIGSEGV. Safe unload is C12.
}

// Scan path: a macro-free struct inheriting ECS::ComponentBase is auto-detected by the
// build-time scanner, which generates its registration — no GE_REGISTER_COMPONENT.
TEST(NativeUserModuleBuildLoad, ScansAndRegistersInheritedComponent)
{
    const auto kv = LoadConfigOrFail();
    for (const char* required : {"dotnet", "scannerdll"})
        ASSERT_TRUE(kv.count(required)) << "missing config key: " << required;

    const fs::path root = fs::path(kv.at("workdir")) / "scanned";
    // Deliberately name the source dir "Components": this reproduces the include-root bug
    // where, for a SourceDir named/under "Components", the scanner emitted a parent-relative
    // `#include "Components/X.h"` the user DLL (which has only SourceDir on its include path)
    // could not resolve. User-mode include roots must be relative to SourceDir.
    ns::NativeBuildConfig config = MakeBaseConfig(kv, root, "Components");
    config.DotnetExe = kv.at("dotnet");
    config.ComponentScannerDll = kv.at("scannerdll");
    config.DetectBase = "ComponentBase";
    const fs::path sourceDir = config.SourceDir;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(sourceDir, ec);
    ASSERT_FALSE(ec) << "cannot create fixture dir " << sourceDir.string() << ": " << ec.message();

    // A component declared with NO macro — just inherit the tag base, in a HEADER (so
    // the scanner can see it and the generated TU can #include it).
    {
        std::ofstream src(sourceDir / "ScannedEnergy.h", std::ios::trunc);
        ASSERT_TRUE(src.is_open());
        src << "#pragma once\n"
            << "#include <GameSDK/GameSDK.h>\n"
            << "namespace ge_c10 { struct ScannedEnergy : ECS::ComponentBase { float Energy = 5.0f; int Charges = 2; }; }\n";
    }

    EXPECT_EQ(ge::ComponentFieldRegistry::FindByName("ScannedEnergy"), 0u); // unknown before build

    ns::NativeScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(nullptr));
    RunAndExpectSuccess(mgr, config);

    const ge::ComponentTypeId id = ge::ComponentFieldRegistry::FindByName("ScannedEnergy");
    EXPECT_NE(id, 0u) << "scanned (macro-free) component not visible after load";
    EXPECT_TRUE(ge::ComponentFactory::Has(id)) << "scanned component has no factory entry";
    EXPECT_EQ(ge::ComponentFieldRegistry::Get(id).size(), 2u) << "expected fields Energy + Charges";
}

// B9: a build already in flight when play-mode deferral engages must NOT hot-swap on
// completion — the completion task stashes the built DLL, and the play-exit flush
// (TriggerDeferredHotReloadIfPending) performs the load. Drives the REAL async path:
// RequestRebuild -> Tick (kick) -> worker cmake build -> completion task under deferral.
// Deterministic despite the worker: the completion task only runs inside Tick, which the
// test drives after setting the deferral flag.
TEST(NativeUserModuleBuildLoad, BuildCompletingDuringPlayDefersModuleSwapUntilFlush)
{
    const auto kv = LoadConfigOrFail();
    const fs::path root = fs::path(kv.at("workdir")) / "deferred";
    ns::NativeBuildConfig config = MakeBaseConfig(kv, root, "Gameplay");
    config.SdkRoot = fs::path(kv.at("sdkinc")).parent_path();
    const fs::path sourceDir = config.SourceDir;
    std::error_code ec;
    fs::remove_all(root, ec); // clean slate: no build cache, so the kick always dispatches
    fs::create_directories(sourceDir, ec);
    ASSERT_FALSE(ec) << "cannot create fixture dir " << sourceDir.string() << ": " << ec.message();

    {
        std::ofstream src(sourceDir / "DeferredCharge.cpp", std::ios::trunc);
        ASSERT_TRUE(src.is_open());
        src << "#include <GameSDK/GameSDK.h>\n"
            << "namespace ge_c10 { struct DeferredCharge { float Amount = 1.0f; }; }\n"
            << "GE_REGISTER_COMPONENT(ge_c10::DeferredCharge, Amount);\n";
    }

    JobSystem::WorkStealingThreadPool pool(2);
    ns::NativeScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(&pool));
    mgr.SetBuildConfig(config);

    int completions = 0;
    ns::NativeBuildResult lastResult;
    mgr.SetBuildCompletedCallback([&completions, &lastResult](const ns::NativeBuildResult& r) {
        ++completions;
        lastResult = r;
    });

    mgr.RequestRebuild();
    mgr.Tick();                     // kicks the async build (first kick is immediate)
    mgr.SetHotReloadDeferred(true); // play-mode enters while the build runs

    // Frame loop: drain until the completion task lands. A failure fires the completed
    // callback instead of stashing, so the loop exits on either outcome.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (!mgr.HasDeferredHotReloadPending() && completions == 0 &&
           std::chrono::steady_clock::now() < deadline)
    {
        mgr.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_EQ(completions, 0) << "build must not complete/load mid-play; failed with: " << lastResult.Error;
    ASSERT_TRUE(mgr.HasDeferredHotReloadPending()) << "build did not finish within the deadline";
    EXPECT_EQ(ge::ComponentFieldRegistry::FindByName("DeferredCharge"), 0u)
        << "module hot-swapped mid-play despite deferral";

    mgr.SetHotReloadDeferred(false); // play-mode exits...
    EXPECT_TRUE(mgr.TriggerDeferredHotReloadIfPending()); // ...and the editor flushes

    EXPECT_EQ(completions, 1) << "the deferred completion must report exactly once, at the flush";
    EXPECT_TRUE(lastResult.Success()) << lastResult.Error;
    EXPECT_NE(ge::ComponentFieldRegistry::FindByName("DeferredCharge"), 0u)
        << "stashed module must load on the play-exit flush";
    EXPECT_FALSE(mgr.HasDeferredHotReloadPending());
}

namespace
{
// WaitForModuleBuilds as the packaged export calls it: from a thread other than the
// one that runs Tick.
ns::NativeScriptManager::ModuleBuildWaitResult WaitFromExportThread(ns::NativeScriptManager& mgr)
{
    ns::NativeScriptManager::ModuleBuildWaitResult result;
    std::thread exportThread([&mgr, &result] {
        result = mgr.WaitForModuleBuilds([] { return false; }, [](const std::vector<std::string>&) {});
    });
    exportThread.join();
    return result;
}
} // namespace

// A module build that completes during play mode is stashed: its load and its build
// record wait for play exit. An export then must not read the older record as if the
// build had settled; the wait answers DeferredByPlayMode at once and names the module.
// After the play-exit flush the wait settles and the record is the new build's.
TEST(NativeUserModuleBuildLoad, ExportWaitReportsABuildDeferredByPlayMode)
{
    const auto kv = LoadConfigOrFail();
    const fs::path root = fs::path(kv.at("workdir")) / "deferred-export";
    ns::NativeBuildConfig config = MakeBaseConfig(kv, root, "Gameplay");
    config.SdkRoot = fs::path(kv.at("sdkinc")).parent_path();
    std::error_code ec;
    fs::remove_all(root, ec); // clean slate: no build cache, so the kick always dispatches
    fs::create_directories(config.SourceDir, ec);
    ASSERT_FALSE(ec) << "cannot create fixture dir " << config.SourceDir.string() << ": " << ec.message();
    {
        std::ofstream src(config.SourceDir / "PlayExportCharge.cpp", std::ios::trunc);
        ASSERT_TRUE(src.is_open());
        src << "#include <GameSDK/GameSDK.h>\n"
            << "namespace ge_c10 { struct PlayExportCharge { float Amount = 1.0f; }; }\n"
            << "GE_REGISTER_COMPONENT(ge_c10::PlayExportCharge, Amount);\n";
    }

    JobSystem::WorkStealingThreadPool pool(2);
    ns::NativeScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(&pool));
    mgr.SetBuildConfig(config);
    mgr.RequestRebuild();
    mgr.Tick();                     // kicks the async build
    mgr.SetHotReloadDeferred(true); // play-mode enters while the build runs

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (!mgr.HasDeferredHotReloadPending() && std::chrono::steady_clock::now() < deadline)
    {
        mgr.Tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(mgr.HasDeferredHotReloadPending()) << "build did not finish (or failed) within the deadline";
    EXPECT_FALSE(ns::ReadBuildCacheRecord(config.BuildDir).has_value())
        << "a build stashed during play must not have written its record yet";

    const ns::NativeScriptManager::ModuleBuildWaitResult duringPlay = WaitFromExportThread(mgr);
    EXPECT_EQ(duringPlay.Outcome, ns::NativeScriptManager::ModuleBuildWaitOutcome::DeferredByPlayMode);
    EXPECT_EQ(duringPlay.DeferredModules, std::vector<std::string>{"UserScripts"});

    mgr.SetHotReloadDeferred(false);
    EXPECT_TRUE(mgr.TriggerDeferredHotReloadIfPending());
    const ns::NativeScriptManager::ModuleBuildWaitResult afterPlay = WaitFromExportThread(mgr);
    EXPECT_EQ(afterPlay.Outcome, ns::NativeScriptManager::ModuleBuildWaitOutcome::Settled);
    EXPECT_TRUE(afterPlay.FailedModules.empty());
    EXPECT_TRUE(ns::ReadBuildCacheRecord(config.BuildDir).has_value())
        << "the play-exit flush writes the stashed build's record";
}

// P1 packages: a package Cpp module builds its own DLL alongside the project
// module, each registers systems under its own module id, and reloading the
// package module clears ONLY its systems — the project module's survive.
TEST(NativeUserModuleBuildLoad, PackageModuleLoadsAlongsideProjectAndClearsPerModule)
{
    const auto kv = LoadConfigOrFail();
    // Deliberately short segment names: the fixture nests a per-package build
    // dir under an already-deep test workdir, and MSVC's tracker tlog paths in
    // cmake TryCompile scratch dirs blow past MAX_PATH otherwise (FTK1011).
    const fs::path root = fs::path(kv.at("workdir")) / "pkg";
    std::error_code ec;
    fs::remove_all(root, ec); // clean slate each run

    // Project module: one component + one hand-registered system (macros-only
    // mode — the adapter registration the scanner would generate, written
    // explicitly so this test doesn't need dotnet).
    ns::NativeBuildConfig projectCfg = MakeBaseConfig(kv, root / "prj", "Gameplay");
    projectCfg.SdkRoot = fs::path(kv.at("sdkinc")).parent_path();
    fs::create_directories(projectCfg.SourceDir, ec);
    ASSERT_FALSE(ec);
    {
        std::ofstream src(projectCfg.SourceDir / "ProjectPing.cpp", std::ios::trunc);
        ASSERT_TRUE(src.is_open());
        src << "#include <GameSDK/GameSDK.h>\n"
            << "#include <GameSDK/System.h>\n"
            << "namespace ge_p1 { struct ProjectPing { float Value = 1.0f; }; }\n"
            << "GE_REGISTER_COMPONENT(ge_p1::ProjectPing, Value);\n"
            << "namespace ge_p1 { struct ProjectPingSystem : ECS::SystemBase { void OnUpdate(ECS::World&, float) {} }; }\n"
            << "static bool s_regProjectPing = GameEngine::NativeScripting::RegisterUserSystem(\n"
            << "    new ECS::UserSystemAdapter<ge_p1::ProjectPingSystem>(\"ProjectPingSystem\"));\n";
    }

    // Package module: its own source root, its own BuildDir/ActiveDir rooted at
    // the package dir (the editor wiring uses <pkg>/.Cache/NativeScripts/*; the
    // shortened names here only dodge MAX_PATH), its own ModuleName.
    const fs::path pkgRoot = root / "op";
    ns::NativeBuildConfig packageCfg = MakeBaseConfig(kv, pkgRoot, "Native");
    packageCfg.SdkRoot = fs::path(kv.at("sdkinc")).parent_path();
    packageCfg.BuildDir = pkgRoot / "bld";
    packageCfg.ActiveDir = pkgRoot / "act";
    packageCfg.ModuleName = "OceanPack";
    fs::create_directories(packageCfg.SourceDir, ec);
    ASSERT_FALSE(ec);
    const auto writePackageSource = [&](const char* systemName) {
        std::ofstream src(packageCfg.SourceDir / "OceanPong.cpp", std::ios::trunc);
        ASSERT_TRUE(src.is_open());
        src << "#include <GameSDK/GameSDK.h>\n"
            << "#include <GameSDK/System.h>\n"
            << "namespace ge_p1 { struct OceanPong { int Waves = 7; }; }\n"
            << "GE_REGISTER_COMPONENT(ge_p1::OceanPong, Waves);\n"
            << "namespace ge_p1 { struct OceanPongSystem : ECS::SystemBase { void OnUpdate(ECS::World&, float) {} }; }\n"
            << "static bool s_regOceanPong = GameEngine::NativeScripting::RegisterUserSystem(\n"
            << "    new ECS::UserSystemAdapter<ge_p1::OceanPongSystem>(\"" << systemName << "\"));\n";
    };
    writePackageSource("OceanPongSystem");

    const auto countByModule = [](const std::string& moduleId) {
        size_t n = 0;
        for (const auto& entry : ns::GetUserSystems())
            if (entry.System && entry.ModuleId == moduleId)
                ++n;
        return n;
    };

    ns::ClearUserSystems();
    ns::NativeScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(nullptr));
    RunAndExpectSuccess(mgr, projectCfg);
    RunAndExpectSuccess(mgr, packageCfg);

    // Both modules' components and systems are visible, each under its own id.
    EXPECT_NE(ge::ComponentFieldRegistry::FindByName("ProjectPing"), 0u);
    EXPECT_NE(ge::ComponentFieldRegistry::FindByName("OceanPong"), 0u);
    EXPECT_EQ(countByModule("UserScripts"), 1u);
    EXPECT_EQ(countByModule("OceanPack"), 1u);

    // Package reload: edit the package source and rebuild — the new DLL loads,
    // OceanPack's old registration is cleared, and the project module's system
    // survives untouched.
    writePackageSource("OceanPongSystemV2");
    RunAndExpectSuccess(mgr, packageCfg);
    EXPECT_EQ(countByModule("OceanPack"), 1u) << "reload must clear the module's previous registration";
    EXPECT_EQ(countByModule("UserScripts"), 1u) << "project module systems must survive a package reload";
    bool foundV2 = false;
    for (const auto& entry : ns::GetUserSystems())
        if (entry.System && std::string(entry.System->Name()) == "OceanPongSystemV2")
            foundV2 = true;
    EXPECT_TRUE(foundV2) << "the reloaded package DLL's registration must be the live one";

    ns::ClearUserSystems();
}

// P3 prebuilt package binaries: build a module once, stage its DLL as the
// package's fingerprint-keyed prebuilt (+ engine_abi marker), then drive a
// fresh config whose ONLY module source is the prebuilt — it must load and
// register with no cmake configure/compile at all.
TEST(NativeUserModuleBuildLoad, PrebuiltPackageBinaryLoadsWithoutBuild)
{
    const auto kv = LoadConfigOrFail();
    const fs::path root = fs::path(kv.at("workdir")) / "pbt";
    std::error_code ec;
    fs::remove_all(root, ec); // clean slate each run

    // Author side: build the package module from source once.
    ns::NativeBuildConfig authorCfg = MakeBaseConfig(kv, root / "a", "Native");
    authorCfg.SdkRoot = fs::path(kv.at("sdkinc")).parent_path();
    authorCfg.ModuleName = "PbtPack";
    fs::create_directories(authorCfg.SourceDir, ec);
    ASSERT_FALSE(ec);
    {
        std::ofstream src(authorCfg.SourceDir / "PbtProbe.cpp", std::ios::trunc);
        ASSERT_TRUE(src.is_open());
        src << "#include <GameSDK/GameSDK.h>\n"
            << "namespace ge_p3 { struct PbtProbe { int Fingerprints = 8; }; }\n"
            << "GE_REGISTER_COMPONENT(ge_p3::PbtProbe, Fingerprints);\n";
    }

    ns::NativeScriptManager author;
    ASSERT_TRUE(author.Initialize(nullptr));
    ns::NativeBuildResult built;
    RunAndExpectSuccess(author, authorCfg, &built);
    ASSERT_FALSE(built.LoadedModulePath.empty());

    // Stage the built DLL as the package's shipped prebuilt: the exact layout
    // the manifest `prebuilt` dir ships — Binaries/<platform-fp>/<Module>.dll
    // plus the engine_abi marker for the consuming engine.
    const fs::path prebuiltRoot = root / "pkg" / "Binaries";
    const fs::path platformDir =
        prebuiltRoot / ns::PrebuiltPlatformDirName(ns::HostToolchainFingerprint());
    fs::create_directories(platformDir, ec);
    ASSERT_FALSE(ec);
    const fs::path prebuiltDll = platformDir / ns::PrebuiltModuleFileName("PbtPack");
    fs::copy_file(built.LoadedModulePath, prebuiltDll, fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec);

    // Consumer side: fresh config, NO sources on disk, prebuilt only. The
    // marker is computed for the consumer config (same engine identity).
    ns::NativeBuildConfig consumerCfg = MakeBaseConfig(kv, root / "c", "Native");
    consumerCfg.SdkRoot = fs::path(kv.at("sdkinc")).parent_path();
    consumerCfg.ModuleName = "PbtPack";
    consumerCfg.PrebuiltDir = prebuiltRoot;
    ASSERT_TRUE(ns::WriteEngineAbiMarker(platformDir, ns::ComputeEngineAbiDigest(consumerCfg)));

    ns::NativeScriptManager consumer;
    ASSERT_TRUE(consumer.Initialize(nullptr));
    const ns::NativeBuildResult result = consumer.BuildAndLoad(consumerCfg);
    ASSERT_TRUE(result.Success()) << result.Error;
    EXPECT_TRUE(result.Loaded);
    EXPECT_TRUE(result.Registered);
    EXPECT_FALSE(result.Configured) << "prebuilt load must not run cmake configure";
    EXPECT_FALSE(result.Compiled) << "prebuilt load must not compile anything";
    EXPECT_EQ(result.LoadedModulePath, prebuiltDll);
    EXPECT_FALSE(fs::exists(consumerCfg.BuildDir)) << "no build dir may be created";
    EXPECT_NE(ge::ComponentFieldRegistry::FindByName("PbtProbe"), 0u);
}

// A prebuilt whose engine_abi marker names a different engine is refused and
// the module falls back to building from source (loudly, not silently).
TEST(NativeUserModuleBuildLoad, PrebuiltMarkerMismatchFallsBackToSourceBuild)
{
    const auto kv = LoadConfigOrFail();
    const fs::path root = fs::path(kv.at("workdir")) / "pbm";
    std::error_code ec;
    fs::remove_all(root, ec); // clean slate each run

    ns::NativeBuildConfig config = MakeBaseConfig(kv, root / "m", "Native");
    config.SdkRoot = fs::path(kv.at("sdkinc")).parent_path();
    config.ModuleName = "PbtStale";
    fs::create_directories(config.SourceDir, ec);
    ASSERT_FALSE(ec);
    {
        std::ofstream src(config.SourceDir / "PbtStaleProbe.cpp", std::ios::trunc);
        ASSERT_TRUE(src.is_open());
        src << "#include <GameSDK/GameSDK.h>\n"
            << "namespace ge_p3 { struct PbtStaleProbe { int Value = 1; }; }\n"
            << "GE_REGISTER_COMPONENT(ge_p3::PbtStaleProbe, Value);\n";
    }

    // Prebuilt dir with the right platform layout but a stale engine marker;
    // the DLL bytes are garbage — they must never be loaded.
    const fs::path prebuiltRoot = root / "pkg" / "Binaries";
    const fs::path platformDir =
        prebuiltRoot / ns::PrebuiltPlatformDirName(ns::HostToolchainFingerprint());
    fs::create_directories(platformDir, ec);
    {
        std::ofstream fake(platformDir / ns::PrebuiltModuleFileName("PbtStale"),
                           std::ios::binary | std::ios::trunc);
        fake << "not a real module";
    }
    ASSERT_TRUE(ns::WriteEngineAbiMarker(platformDir, "another-engine-entirely"));
    config.PrebuiltDir = prebuiltRoot;

    ns::NativeScriptManager mgr;
    ASSERT_TRUE(mgr.Initialize(nullptr));
    const ns::NativeBuildResult result = mgr.BuildAndLoad(config);
    ASSERT_TRUE(result.Success()) << result.Error;
    EXPECT_TRUE(result.Compiled) << "mismatched prebuilt must fall back to the source build";
    EXPECT_TRUE(result.Registered);
    EXPECT_NE(result.LoadedModulePath.parent_path(), platformDir) << "the refused prebuilt was loaded";
    EXPECT_NE(result.LoadedModulePath.filename().generic_string().find("PbtStale"), std::string::npos);
    EXPECT_NE(ge::ComponentFieldRegistry::FindByName("PbtStaleProbe"), 0u);
}
