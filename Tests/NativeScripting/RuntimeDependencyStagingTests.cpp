// Ship-safety C5: curated runtime dependency staging for packaged games. The
// tables replace the old ship-everything *.dll glob; these tests exercise the
// MECHANISM against fake directories — candidate ordering, wildcard entries,
// lib-prefix normalization, junk exclusion, loud failure on missing required
// entries — driving the fixtures from the real exported tables so they can
// never drift from what ships.

#include "Engine/Build/RuntimeDependencyStaging.h"

#include "Engine/Build/BuildPipeline.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using GameEngine::BuildPipeline;
using GameEngine::EngineRuntimeDependencySet;
using GameEngine::ManagedHostingDependencySet;
using GameEngine::RuntimeDependency;
using GameEngine::StageCuratedDependencySet;
using GameEngine::StageManagedRuntimeAssemblies;

namespace
{
void Touch(const fs::path& path)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "x";
}

void WriteFile(const fs::path& path, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// First candidate of an entry as a concrete on-disk file name: exact candidates
// verbatim, wildcard candidates with a plausible version suffix.
std::string FirstCandidateFileName(const RuntimeDependency& dep)
{
    std::string candidate(dep.Candidates);
    if (const size_t sep = candidate.find(';'); sep != std::string::npos)
        candidate.resize(sep);
    if (!candidate.empty() && candidate.back() == '*')
        candidate = candidate.substr(0, candidate.size() - 1) + "62";
    return candidate + ".dll";
}

struct StagingDirs
{
    fs::path Root;
    fs::path Source;
    fs::path Dest;
    std::vector<std::string> Errors;

    explicit StagingDirs(const char* name)
    {
        Root = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(Root, ec);
        Source = Root / "editor";
        Dest = Root / "game";
        fs::create_directories(Source, ec);
        fs::create_directories(Dest, ec);
    }

    ~StagingDirs()
    {
        std::error_code ec;
        fs::remove_all(Root, ec);
    }

    void PopulateAllRequired(std::span<const RuntimeDependency> entries)
    {
        for (const RuntimeDependency& dep : entries)
            if (dep.Required)
                Touch(Source / FirstCandidateFileName(dep));
    }
};
} // namespace

TEST(RuntimeDependencyStaging, ShipsEveryRequiredEntryAndExcludesJunk)
{
    StagingDirs dirs("ge_rds_curated");
    dirs.PopulateAllRequired(EngineRuntimeDependencySet());
    // Junk the old glob used to ship: test frameworks, tooling, editor-only
    // managed assemblies, stale applocal leftovers.
    for (const char* junk : {"gtest.dll", "benchmark.dll", "SPIRV-Tools-shared.dll", "pkgconf-7.dll",
                             "GameEngine.Editor.Managed.dll", "harfbuzz-subset.dll", "brotlienc.dll"})
        Touch(dirs.Source / junk);

    ASSERT_TRUE(StageCuratedDependencySet({dirs.Source}, dirs.Dest, EngineRuntimeDependencySet(), dirs.Errors))
        << (dirs.Errors.empty() ? "" : dirs.Errors.front());

    std::error_code ec;
    for (const RuntimeDependency& dep : EngineRuntimeDependencySet())
        if (dep.Required)
            EXPECT_TRUE(fs::exists(dirs.Dest / FirstCandidateFileName(dep), ec))
                << "required entry not staged: " << dep.Name;
    for (const char* junk : {"gtest.dll", "benchmark.dll", "SPIRV-Tools-shared.dll", "pkgconf-7.dll",
                             "GameEngine.Editor.Managed.dll", "harfbuzz-subset.dll", "brotlienc.dll"})
        EXPECT_FALSE(fs::exists(dirs.Dest / junk, ec)) << "junk must not ship: " << junk;
}

TEST(RuntimeDependencyStaging, MissingRequiredEntryFailsNamingIt)
{
    StagingDirs dirs("ge_rds_missing");
    dirs.PopulateAllRequired(EngineRuntimeDependencySet());
    std::error_code ec;
    fs::remove(dirs.Source / "glfw3.dll", ec); // knock out one required entry

    EXPECT_FALSE(StageCuratedDependencySet({dirs.Source}, dirs.Dest, EngineRuntimeDependencySet(), dirs.Errors));
    ASSERT_FALSE(dirs.Errors.empty());
    EXPECT_NE(dirs.Errors.front().find("GLFW"), std::string::npos) << dirs.Errors.front();
}

TEST(RuntimeDependencyStaging, WildcardEntryShipsEveryMatch)
{
    StagingDirs dirs("ge_rds_wildcard");
    dirs.PopulateAllRequired(EngineRuntimeDependencySet());
    Touch(dirs.Source / "avcodec-61.dll"); // second ABI version beside avcodec-62

    ASSERT_TRUE(StageCuratedDependencySet({dirs.Source}, dirs.Dest, EngineRuntimeDependencySet(), dirs.Errors));
    std::error_code ec;
    EXPECT_TRUE(fs::exists(dirs.Dest / "avcodec-61.dll", ec));
    EXPECT_TRUE(fs::exists(dirs.Dest / "avcodec-62.dll", ec));
}

TEST(RuntimeDependencyStaging, PerConfigCandidatesAndLibPrefixSatisfyOneEntry)
{
    StagingDirs dirs("ge_rds_names");
    dirs.PopulateAllRequired(EngineRuntimeDependencySet());
    std::error_code ec;
    // Release-flavored zlib name instead of the debug-first candidate.
    fs::remove(dirs.Source / "zd.dll", ec);
    Touch(dirs.Source / "z.dll");
    // Unix-flavored engine name: entries must match through the "lib" prefix.
    fs::remove(dirs.Source / "Engine.dll", ec);
    Touch(dirs.Source / "libEngine.so");

    ASSERT_TRUE(StageCuratedDependencySet({dirs.Source}, dirs.Dest, EngineRuntimeDependencySet(), dirs.Errors))
        << (dirs.Errors.empty() ? "" : dirs.Errors.front());
    EXPECT_TRUE(fs::exists(dirs.Dest / "z.dll", ec));
    EXPECT_TRUE(fs::exists(dirs.Dest / "libEngine.so", ec));
}

TEST(RuntimeDependencyStaging, ManagedAssembliesShipWithSidecarsAndWithoutEditorOnly)
{
    StagingDirs dirs("ge_rds_managed");
    for (const char* required : {"GameEngine.CoreBridge.dll", "GameEngine.HotReload.dll",
                                 "GameEngine.Scripting.ABI.dll", "GameEngine.ECS.ABI.dll",
                                 "GameEngine.Scripting.Runtime.dll"})
        Touch(dirs.Source / required);
    WriteFile(dirs.Source / "GameEngine.CoreBridge.deps.json", R"({"targets":{".NETCoreApp,Version=v9.0":{}}})");
    Touch(dirs.Source / "GameEngine.HotReload.runtimeconfig.json");
    Touch(dirs.Source / "GameEngine.Editor.Managed.dll");        // editor-only: must not ship
    Touch(dirs.Source / "GameEngine.Editor.Scripting.ABI.dll");  // editor ABI: must not ship

    ASSERT_TRUE(StageManagedRuntimeAssemblies(dirs.Source, dirs.Dest, dirs.Errors))
        << (dirs.Errors.empty() ? "" : dirs.Errors.front());
    std::error_code ec;
    EXPECT_TRUE(fs::exists(dirs.Dest / "GameEngine.CoreBridge.dll", ec));
    EXPECT_TRUE(fs::exists(dirs.Dest / "GameEngine.CoreBridge.deps.json", ec));
    EXPECT_TRUE(fs::exists(dirs.Dest / "GameEngine.HotReload.runtimeconfig.json", ec));
    EXPECT_FALSE(fs::exists(dirs.Dest / "GameEngine.Editor.Managed.dll", ec));
    EXPECT_FALSE(fs::exists(dirs.Dest / "GameEngine.Editor.Scripting.ABI.dll", ec));
}

// The managed set is derived, not listed: every engine ABI surface beside the engine ships (a new
// one included), and so does each assembly a staged assembly's deps.json declares at runtime.
TEST(RuntimeDependencyStaging, ManagedSetIsTheAbiSurfacesAndTheDepsJsonClosure)
{
    StagingDirs dirs("ge_rds_managed_closure");
    for (const char* name : {"GameEngine.CoreBridge.dll", "GameEngine.HotReload.dll",
                             "GameEngine.Scripting.Runtime.dll", "GameEngine.Audio.ABI.dll",
                             "GameEngine.Runtime.Support.dll", "GameEngine.Runtime.Support.Extra.dll",
                             "GameEngine.Unreferenced.dll"})
        Touch(dirs.Source / name);
    WriteFile(dirs.Source / "GameEngine.Scripting.Runtime.deps.json", R"({"targets":{".NETCoreApp,Version=v9.0":{
        "GameEngine.Scripting.Runtime/1.0.0":{"runtime":{"GameEngine.Scripting.Runtime.dll":{}}},
        "GameEngine.Runtime.Support/1.0.0":{"runtime":{"lib/net9.0/GameEngine.Runtime.Support.dll":{}}}}}})");
    WriteFile(dirs.Source / "GameEngine.Runtime.Support.deps.json", R"({"targets":{".NETCoreApp,Version=v9.0":{
        "GameEngine.Runtime.Support.Extra/1.0.0":{"runtime":{"GameEngine.Runtime.Support.Extra.dll":{}}}}}})");

    ASSERT_TRUE(StageManagedRuntimeAssemblies(dirs.Source, dirs.Dest, dirs.Errors))
        << (dirs.Errors.empty() ? "" : dirs.Errors.front());
    std::error_code ec;
    for (const char* shipped : {"GameEngine.Scripting.Runtime.dll", "GameEngine.Audio.ABI.dll",
                                "GameEngine.Runtime.Support.dll", "GameEngine.Runtime.Support.Extra.dll"})
        EXPECT_TRUE(fs::exists(dirs.Dest / shipped, ec)) << "not staged: " << shipped;
    EXPECT_FALSE(fs::exists(dirs.Dest / "GameEngine.Unreferenced.dll", ec))
        << "an assembly nothing loads must not ship";
}

TEST(RuntimeDependencyStaging, DepsJsonDependencyMissingFromTheEditorFailsNamingIt)
{
    StagingDirs dirs("ge_rds_managed_closure_missing");
    for (const char* name : {"GameEngine.CoreBridge.dll", "GameEngine.HotReload.dll",
                             "GameEngine.Scripting.Runtime.dll"})
        Touch(dirs.Source / name);
    WriteFile(dirs.Source / "GameEngine.Scripting.Runtime.deps.json", R"({"targets":{".NETCoreApp,Version=v9.0":{
        "GameEngine.ECS.ABI/1.0.0":{"runtime":{"GameEngine.ECS.ABI.dll":{}}}}}})");

    EXPECT_FALSE(StageManagedRuntimeAssemblies(dirs.Source, dirs.Dest, dirs.Errors));
    ASSERT_FALSE(dirs.Errors.empty());
    EXPECT_NE(dirs.Errors.front().find("GameEngine.ECS.ABI.dll"), std::string::npos) << dirs.Errors.front();
    EXPECT_NE(dirs.Errors.front().find("GameEngine.Scripting.Runtime.deps.json"), std::string::npos)
        << "the error must say what asked for the missing assembly: " << dirs.Errors.front();
}

TEST(RuntimeDependencyStaging, MalformedDepsJsonFailsNamingIt)
{
    StagingDirs dirs("ge_rds_managed_bad_deps");
    for (const char* name : {"GameEngine.CoreBridge.dll", "GameEngine.HotReload.dll",
                             "GameEngine.Scripting.Runtime.dll"})
        Touch(dirs.Source / name);
    WriteFile(dirs.Source / "GameEngine.HotReload.deps.json", "{ not json");

    EXPECT_FALSE(StageManagedRuntimeAssemblies(dirs.Source, dirs.Dest, dirs.Errors));
    ASSERT_FALSE(dirs.Errors.empty());
    EXPECT_NE(dirs.Errors.front().find("GameEngine.HotReload.deps.json"), std::string::npos) << dirs.Errors.front();
}

TEST(RuntimeDependencyStaging, MissingRequiredManagedAssemblyFailsNamingIt)
{
    StagingDirs dirs("ge_rds_managed_missing");
    Touch(dirs.Source / "GameEngine.CoreBridge.dll"); // everything else absent

    EXPECT_FALSE(StageManagedRuntimeAssemblies(dirs.Source, dirs.Dest, dirs.Errors));
    ASSERT_FALSE(dirs.Errors.empty());
    EXPECT_NE(dirs.Errors.front().find("GameEngine.HotReload.dll"), std::string::npos) << dirs.Errors.front();
}

TEST(RuntimeDependencyStaging, HostingSetRequiresNethostAndHostfxr)
{
    StagingDirs dirs("ge_rds_hosting");
    Touch(dirs.Source / "nethost.dll"); // hostfxr deliberately missing

    EXPECT_FALSE(StageCuratedDependencySet({dirs.Source}, dirs.Dest, ManagedHostingDependencySet(), dirs.Errors));
    ASSERT_FALSE(dirs.Errors.empty());
    EXPECT_NE(dirs.Errors.front().find("hostfxr"), std::string::npos) << dirs.Errors.front();
}

TEST(RuntimeDependencyStaging, MultiDirSearchEarlierDirShadowsLaterSameName)
{
    // Cross-CRT-flavor packaging searches the SDK's staged engine runtime dir
    // first, then the vcpkg flavor bin dirs. An entry may be satisfied from
    // any dir, and a file name present in several must ship from the FIRST.
    StagingDirs dirs("ge_rds_multidir");
    const fs::path sdkRuntime = dirs.Root / "sdk-runtime";
    std::error_code ec;
    fs::create_directories(sdkRuntime, ec);

    // Engine binaries only in the SDK runtime dir; every other required entry
    // only in the vcpkg-like Source dir.
    dirs.PopulateAllRequired(EngineRuntimeDependencySet());
    fs::remove(dirs.Source / "Engine.dll", ec);
    fs::remove(dirs.Source / "GameEngine.Native.dll", ec);
    Touch(sdkRuntime / "Engine.dll");
    Touch(sdkRuntime / "GameEngine.Native.dll");
    // Same name in both dirs: the earlier dir's copy must win.
    {
        std::ofstream first(sdkRuntime / "glfw3.dll", std::ios::binary | std::ios::trunc);
        first << "first-dir";
        std::ofstream second(dirs.Source / "glfw3.dll", std::ios::binary | std::ios::trunc);
        second << "second-dir";
    }

    ASSERT_TRUE(StageCuratedDependencySet({sdkRuntime, dirs.Source}, dirs.Dest,
                                          EngineRuntimeDependencySet(), dirs.Errors))
        << (dirs.Errors.empty() ? "" : dirs.Errors.front());
    EXPECT_TRUE(fs::exists(dirs.Dest / "Engine.dll", ec));
    EXPECT_TRUE(fs::exists(dirs.Dest / "GameEngine.Native.dll", ec));
    std::ifstream staged(dirs.Dest / "glfw3.dll", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(staged)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "first-dir") << "earlier source dir must shadow the later one";
}

TEST(RuntimeDependencyStaging, ShadercSharedShipsWhenPresentAndStaysOptional)
{
    // shaderc-shared overlay builds (cmake/ports/shaderc) give Engine.dll a
    // shaderc_shared.dll import; static-shaderc builds ship none. The entry
    // must stage the DLL when the source dir has it and never fail a package
    // when it doesn't.
    StagingDirs dirs("ge_rds_shaderc");
    dirs.PopulateAllRequired(EngineRuntimeDependencySet());
    std::error_code ec;

    // Static-shaderc source dir: the absent DLL must not fail the package.
    ASSERT_TRUE(StageCuratedDependencySet({dirs.Source}, dirs.Dest, EngineRuntimeDependencySet(), dirs.Errors))
        << (dirs.Errors.empty() ? "" : dirs.Errors.front());
    EXPECT_FALSE(fs::exists(dirs.Dest / "shaderc_shared.dll", ec));

    // Shared-shaderc source dir: the DLL must ship.
    Touch(dirs.Source / "shaderc_shared.dll");
    dirs.Errors.clear();
    ASSERT_TRUE(StageCuratedDependencySet({dirs.Source}, dirs.Dest, EngineRuntimeDependencySet(), dirs.Errors))
        << (dirs.Errors.empty() ? "" : dirs.Errors.front());
    EXPECT_TRUE(fs::exists(dirs.Dest / "shaderc_shared.dll", ec))
        << "shaderc_shared.dll beside the engine must ship with the package";
}

TEST(RuntimeDependencyStaging, DirectXTexShipsWhenPresentAndStaysOptional)
{
    // The vcpkg directxtex dependency is platform-gated (windows | linux | osx):
    // engines built without it never import DirectXTex.dll. The entry must
    // stage the DLL when the source dir has it and never fail a package when
    // it doesn't.
    StagingDirs dirs("ge_rds_directxtex");
    dirs.PopulateAllRequired(EngineRuntimeDependencySet());
    std::error_code ec;

    // No DirectXTex beside the engine: the absent DLL must not fail the package.
    ASSERT_TRUE(StageCuratedDependencySet({dirs.Source}, dirs.Dest, EngineRuntimeDependencySet(), dirs.Errors))
        << (dirs.Errors.empty() ? "" : dirs.Errors.front());
    EXPECT_FALSE(fs::exists(dirs.Dest / "DirectXTex.dll", ec));

    // DirectXTex beside the engine: the DLL must ship.
    Touch(dirs.Source / "DirectXTex.dll");
    dirs.Errors.clear();
    ASSERT_TRUE(StageCuratedDependencySet({dirs.Source}, dirs.Dest, EngineRuntimeDependencySet(), dirs.Errors))
        << (dirs.Errors.empty() ? "" : dirs.Errors.front());
    EXPECT_TRUE(fs::exists(dirs.Dest / "DirectXTex.dll", ec))
        << "DirectXTex.dll beside the engine must ship with the package";
}

// ---------------------------------------------------------------------------
// Cross-flavor vcpkg bin-dir resolution (BuildPipeline::FindVcpkgRuntimeBinDirsForFlavor)
// and its interaction with the curated staging scan. Overlay-triplet ports
// (cmake/triplets) build release-only: their DLLs exist ONLY under
// <triplet>/bin, so a debug-CRT package's search list must end with the
// release bin while debug/bin keeps priority for same-named kept ports.
// ---------------------------------------------------------------------------

namespace
{
struct FakeVcpkgTree
{
    fs::path Installed;
    fs::path DebugBin;
    fs::path ReleaseBin;

    explicit FakeVcpkgTree(const fs::path& root)
    {
        Installed = root / "vcpkg_installed";
        const fs::path triplet = Installed / "x64-windows";
        DebugBin = triplet / "debug" / "bin";
        ReleaseBin = triplet / "bin";
        std::error_code ec;
        // Metadata dir the triplet scan must skip.
        fs::create_directories(Installed / "vcpkg", ec);
    }
};
} // namespace

TEST(RuntimeDependencyStaging, VcpkgBinDirsReleaseFlavorIsReleaseBinOnly)
{
    StagingDirs dirs("ge_rds_vcpkg_release");
    FakeVcpkgTree tree(dirs.Root);
    Touch(tree.DebugBin / "pugixml.dll");
    Touch(tree.ReleaseBin / "pugixml.dll");

    const std::vector<fs::path> resolved =
        BuildPipeline::FindVcpkgRuntimeBinDirsForFlavor(tree.Installed, /*debugCrt=*/false);
    ASSERT_EQ(resolved.size(), 1u)
        << "a release package must never search debug/bin (debug-first candidate "
           "names like freetyped would match the wrong flavor)";
    EXPECT_EQ(resolved.front(), tree.ReleaseBin);
}

TEST(RuntimeDependencyStaging, VcpkgBinDirsDebugFlavorAppendsReleaseBinFallback)
{
    StagingDirs dirs("ge_rds_vcpkg_debug");
    FakeVcpkgTree tree(dirs.Root);
    Touch(tree.DebugBin / "pugixml.dll");
    Touch(tree.ReleaseBin / "pugixml.dll");

    const std::vector<fs::path> resolved =
        BuildPipeline::FindVcpkgRuntimeBinDirsForFlavor(tree.Installed, /*debugCrt=*/true);
    ASSERT_EQ(resolved.size(), 2u)
        << "release-only overlay ports ship no debug DLLs; the debug flavor "
           "needs the release bin as a trailing fallback";
    EXPECT_EQ(resolved[0], tree.DebugBin) << "debug/bin must keep search priority";
    EXPECT_EQ(resolved[1], tree.ReleaseBin);
}

TEST(RuntimeDependencyStaging, VcpkgBinDirsDebugFlavorRequiresDebugBin)
{
    StagingDirs dirs("ge_rds_vcpkg_nodebug");
    FakeVcpkgTree tree(dirs.Root);
    Touch(tree.ReleaseBin / "pugixml.dll"); // debug/bin absent entirely

    EXPECT_TRUE(BuildPipeline::FindVcpkgRuntimeBinDirsForFlavor(tree.Installed, /*debugCrt=*/true).empty())
        << "the release bin is a fallback, never a substitute: kept ports' "
           "debug variants (C++-interface DLLs) must exist for a debug package";
}

TEST(RuntimeDependencyStaging, ReleaseOnlyPortsStageThroughTrailingReleaseBin)
{
    // Post-drop layout, modeled on the real x64-windows tree: kept ports keep a
    // debug variant under debug/bin; the ten release-only ports exist solely
    // under bin, with release file names.
    StagingDirs dirs("ge_rds_release_only");
    FakeVcpkgTree tree(dirs.Root);
    const fs::path sdkRuntime = dirs.Root / "sdk-runtime";
    for (const char* name : {"Engine.dll", "GameEngine.Native.dll"})
        Touch(sdkRuntime / name);
    for (const char* name : {"glfw3.dll", "lexbor.dll", "utf8proc.dll", "tinyexpr.dll", "thorvg-1.dll",
                             "meshoptimizer.dll", "ktx.dll", "mimalloc-debug.dll", "mimalloc-redirect.dll"})
        Touch(tree.DebugBin / name);
    WriteFile(tree.DebugBin / "pugixml.dll", "debug-flavor");
    WriteFile(tree.DebugBin / "shaderc_shared.dll", "debug-flavor-shaderc");
    WriteFile(tree.DebugBin / "DirectXTex.dll", "debug-flavor-directxtex");
    for (const char* name : {"freetype.dll", "harfbuzz.dll", "zstd.dll", "z.dll", "sqlite3.dll",
                             "libpng16.dll", "brotlicommon.dll", "brotlidec.dll", "bz2.dll", "libcurl.dll",
                             "avutil-60.dll", "avcodec-62.dll", "avformat-62.dll", "swscale-9.dll",
                             "mimalloc.dll", "mimalloc-redirect.dll",
                             // Release-bin junk the exact candidates must keep excluding.
                             "harfbuzz-subset.dll", "brotlienc.dll"})
        Touch(tree.ReleaseBin / name);
    WriteFile(tree.ReleaseBin / "pugixml.dll", "release-flavor");
    WriteFile(tree.ReleaseBin / "shaderc_shared.dll", "release-flavor-shaderc");
    WriteFile(tree.ReleaseBin / "DirectXTex.dll", "release-flavor-directxtex");

    // Without the release-bin fallback (the pre-fallback search list) every
    // release-only port's Required entry is unsatisfiable — the exact failure a
    // release-CRT editor hit packaging a Debug-flavor player.
    EXPECT_FALSE(StageCuratedDependencySet({sdkRuntime, tree.DebugBin}, dirs.Dest,
                                           EngineRuntimeDependencySet(), dirs.Errors));
    ASSERT_FALSE(dirs.Errors.empty());
    for (const char* missing : {"FreeType", "HarfBuzz", "libcurl", "FFmpeg avutil", "SQLite"})
        EXPECT_NE(dirs.Errors.front().find(missing), std::string::npos)
            << "expected '" << missing << "' in: " << dirs.Errors.front();

    // With the resolved flavor dirs appended, staging succeeds: release-only
    // ports resolve from the trailing release bin under release names, while
    // debug/bin still wins for anything it carries. Fresh dest: the failing arm
    // above already staged its satisfiable entries, and leftovers would satisfy
    // this arm's per-file assertions without this arm staging anything.
    dirs.Errors.clear();
    {
        std::error_code resetEc;
        fs::remove_all(dirs.Dest, resetEc);
        fs::create_directories(dirs.Dest, resetEc);
    }
    std::vector<fs::path> engineDepDirs{sdkRuntime};
    const std::vector<fs::path> vcpkgDirs =
        BuildPipeline::FindVcpkgRuntimeBinDirsForFlavor(tree.Installed, /*debugCrt=*/true);
    engineDepDirs.insert(engineDepDirs.end(), vcpkgDirs.begin(), vcpkgDirs.end());
    ASSERT_TRUE(StageCuratedDependencySet(engineDepDirs, dirs.Dest, EngineRuntimeDependencySet(),
                                          dirs.Errors))
        << (dirs.Errors.empty() ? "" : dirs.Errors.front());

    std::error_code ec;
    for (const char* staged : {"freetype.dll", "harfbuzz.dll", "z.dll", "avutil-60.dll", "swscale-9.dll"})
        EXPECT_TRUE(fs::exists(dirs.Dest / staged, ec)) << "release-only port not staged: " << staged;
    EXPECT_EQ(ReadFile(dirs.Dest / "pugixml.dll"), "debug-flavor")
        << "debug/bin must shadow the release bin for same-named kept ports";
    EXPECT_EQ(ReadFile(dirs.Dest / "shaderc_shared.dll"), "debug-flavor-shaderc")
        << "shaderc_shared ships under one name in both flavors: debug/bin must win";
    EXPECT_EQ(ReadFile(dirs.Dest / "DirectXTex.dll"), "debug-flavor-directxtex")
        << "DirectXTex ships under one name in both flavors: debug/bin must win";
    EXPECT_TRUE(fs::exists(dirs.Dest / "mimalloc-debug.dll", ec))
        << "debug-first candidate order must pick the debug variant across mixed dirs";
    EXPECT_FALSE(fs::exists(dirs.Dest / "mimalloc.dll", ec))
        << "the release mimalloc must not ship into a debug-CRT package";
    for (const char* junk : {"harfbuzz-subset.dll", "brotlienc.dll"})
        EXPECT_FALSE(fs::exists(dirs.Dest / junk, ec)) << "junk must not ship: " << junk;
}

// The packaged engine runtime is the one the player links (#2290): a DebugFast editor whose SDK
// also stages lib/Release builds a Release player against lib/Release, so Engine.dll and
// GameEngine.Native.dll ship from there, not from beside the editor (a different
// GE_DEBUG_INSTRUMENTATION, which the engine refuses at start). A player that links the editor's
// own configuration ships the editor's own runtime.
namespace
{
struct LinkedRuntimeFixture
{
    StagingDirs Dirs{"ge_rds_link_config_runtime"};
    GameEngine::BuildSettings Settings;
    fs::path Release;

    LinkedRuntimeFixture()
    {
        Dirs.PopulateAllRequired(EngineRuntimeDependencySet());
        WriteFile(Dirs.Source / "Engine.dll", "editor-debugfast-engine");
        WriteFile(Dirs.Source / "GameEngine.Native.dll", "editor-debugfast-native");
        Settings.runtimeDepsPath = Dirs.Source;
        Settings.editorSDKPath = Dirs.Root / "SDK";
        Settings.buildConfiguration = "Release";
        Settings.platformName = "Windows";
        Touch(Settings.editorSDKPath / "lib" / "DebugFast" / "Engine.lib");
        Release = Settings.editorSDKPath / "lib" / "Release";
        Touch(Release / "Engine.lib");
    }

    void StageReleaseRuntime()
    {
        WriteFile(Release / "Engine.dll", "sdk-release-engine");
        WriteFile(Release / "GameEngine.Native.dll", "sdk-release-native");
    }
};
} // namespace

TEST(RuntimeDependencyStaging, ThePackagedEngineIsTheRuntimeOfTheConfigurationThePlayerLinks)
{
    LinkedRuntimeFixture fixture;
    fixture.StageReleaseRuntime();
    StagingDirs& dirs = fixture.Dirs;

    const BuildPipeline::PackagedEngineRuntime release =
        BuildPipeline::ResolvePackagedEngineRuntime(fixture.Settings, "Release", "DebugFast", "");
    ASSERT_TRUE(release.Refusal.empty()) << release.Refusal;
    ASSERT_TRUE(StageCuratedDependencySet({release.Dir, dirs.Source}, dirs.Dest, EngineRuntimeDependencySet(),
                                          dirs.Errors))
        << (dirs.Errors.empty() ? "" : dirs.Errors.front());
    EXPECT_EQ(ReadFile(dirs.Dest / "Engine.dll"), "sdk-release-engine");
    EXPECT_EQ(ReadFile(dirs.Dest / "GameEngine.Native.dll"), "sdk-release-native");

    const BuildPipeline::PackagedEngineRuntime own =
        BuildPipeline::ResolvePackagedEngineRuntime(fixture.Settings, "DebugFast", "DebugFast", "");
    ASSERT_TRUE(own.Refusal.empty()) << own.Refusal;
    std::error_code ec;
    fs::remove_all(dirs.Dest, ec);
    ASSERT_TRUE(StageCuratedDependencySet({own.Dir, dirs.Source}, dirs.Dest, EngineRuntimeDependencySet(),
                                          dirs.Errors))
        << (dirs.Errors.empty() ? "" : dirs.Errors.front());
    EXPECT_EQ(ReadFile(dirs.Dest / "Engine.dll"), "editor-debugfast-engine");
}

// A linked configuration whose runtime the SDK does not stage is refused by name, never replaced by
// the editor's own engine.
TEST(RuntimeDependencyStaging, ALinkedConfigurationWithoutAStagedRuntimeIsRefused)
{
    LinkedRuntimeFixture fixture;
    const BuildPipeline::PackagedEngineRuntime runtime =
        BuildPipeline::ResolvePackagedEngineRuntime(fixture.Settings, "Release", "DebugFast", "");
    EXPECT_TRUE(runtime.Dir.empty());
    EXPECT_NE(runtime.Refusal.find("links the SDK's 'Release' engine"), std::string::npos) << runtime.Refusal;
}

// Native modules are built against the editor's own configuration (#2806): beside another
// configuration's engine they would load with mismatched class layouts, so that export is refused,
// naming both configurations and only the ways out that work. A DebugFast editor's configuration is
// no player configuration, so the settings key is offered only to an editor whose configuration is
// one (a Release editor). The editor's own configuration still exports.
TEST(RuntimeDependencyStaging, NativeModulesRefuseAnEngineOtherThanTheEditorsOwn)
{
    LinkedRuntimeFixture fixture;
    fixture.StageReleaseRuntime();
    constexpr std::string_view kScripts = "the project's native C++ user scripts";

    const BuildPipeline::PackagedEngineRuntime fromDebugFast =
        BuildPipeline::ResolvePackagedEngineRuntime(fixture.Settings, "Release", "DebugFast", kScripts);
    EXPECT_TRUE(fromDebugFast.Dir.empty());
    for (const char* named : {"'Release'", "'DebugFast'", "native C++ user scripts", "#2806"})
        EXPECT_NE(fromDebugFast.Refusal.find(named), std::string::npos)
            << named << " missing from: " << fromDebugFast.Refusal;
    EXPECT_EQ(fromDebugFast.Refusal.find("buildConfig"), std::string::npos)
        << "DebugFast is no player configuration, so the settings key cannot help: " << fromDebugFast.Refusal;

    const BuildPipeline::PackagedEngineRuntime fromRelease =
        BuildPipeline::ResolvePackagedEngineRuntime(fixture.Settings, "Debug", "Release", kScripts);
    EXPECT_TRUE(fromRelease.Dir.empty());
    EXPECT_NE(fromRelease.Refusal.find("build.platform.Windows.buildConfig to 'Release'"), std::string::npos)
        << fromRelease.Refusal;

    const BuildPipeline::PackagedEngineRuntime own =
        BuildPipeline::ResolvePackagedEngineRuntime(fixture.Settings, "DebugFast", "DebugFast", kScripts);
    EXPECT_TRUE(own.Refusal.empty()) << own.Refusal;
    EXPECT_EQ(own.Dir, fixture.Dirs.Source);
}

// An engine runtime that was never resolved yields no search directories, so the export fails closed
// instead of staging from the editor's directory, which would ship the editor's own engine (#2290).
TEST(RuntimeDependencyStaging, AnUnresolvedEngineRuntimeStagesNothing)
{
    LinkedRuntimeFixture fixture;
    EXPECT_TRUE(BuildPipeline::EngineRuntimeSearchDirs({}, {}, fixture.Dirs.Source).empty());
    EXPECT_EQ(BuildPipeline::EngineRuntimeSearchDirs(fixture.Release, {}, fixture.Dirs.Source),
              (std::vector<fs::path>{fixture.Release, fixture.Dirs.Source}));
}
