// Ship-safety C1: StageNativeUserScriptsForPackage stages the editor-built native
// user module into a packaged Windows/Linux game — DLL (+PDB) under NativeScripts/,
// a RELATIVE record, and the engine_abi marker — and fails the BUILD (never
// silently skips) when the project has a native module that cannot be packaged
// correctly. Pure filesystem logic, tested against a fake project + staging dir;
// the Player-side consumption of this exact layout is covered by
// UserModuleBuildLoadTest's packaged-game section.

#include "Engine/Build/NativeScriptStaging.h"
#include "NativeScripting/BuildCacheRecord.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ns = GameEngine::NativeScripting;
using GameEngine::EngineAbiDigestSet;
using GameEngine::NativeScriptStageOutcome;
using GameEngine::StageNativeUserScriptsForPackage;

namespace
{
constexpr const char* kEngineAbi = "1111222233334444";

// The single-digest shape most tests exercise (no package defines in play).
EngineAbiDigestSet Digests(const char* current)
{
    return EngineAbiDigestSet{current, current, ""};
}

void WriteBytes(const fs::path& path, const char* bytes)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

// A project whose editor build produced a shadow-copied module + a PDB in the
// (toolchain-dependent) build output subdir, recorded in the build cache.
struct StagingFixture
{
    fs::path Root;
    fs::path BuildDir;
    fs::path ContentRoot;
    std::vector<std::string> Errors;

    explicit StagingFixture(const char* name)
    {
        Root = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(Root, ec);
        BuildDir = Root / ".Cache" / "NativeScripts" / "build";
        ContentRoot = Root / "staged-game";
        fs::create_directories(ContentRoot, ec);
    }

    ~StagingFixture()
    {
        std::error_code ec;
        fs::remove_all(Root, ec);
    }

    fs::path WriteModule(const char* abiDigest, bool withPdb = true)
    {
        const fs::path dll = Root / ".Cache" / "NativeScripts" / "active" / "UserScripts_1a2b3c4d.dll";
        WriteBytes(dll, "fake module bytes");
        if (withPdb)
            WriteBytes(BuildDir / "x64" / "UserScripts.pdb", "fake pdb bytes");
        EXPECT_TRUE(ns::WriteBuildCacheRecord(
            BuildDir, ns::BuildCacheRecord{"fulldigest", dll.generic_string(), abiDigest}));
        return dll;
    }
};
} // namespace

TEST(NativeScriptStaging, ProjectWithoutNativeScriptsStagesNothing)
{
    StagingFixture fx("ge_nss_none");

    const auto outcome = StageNativeUserScriptsForPackage(fx.Root, fx.ContentRoot, Digests(kEngineAbi), fx.Errors, false);

    EXPECT_EQ(outcome, NativeScriptStageOutcome::NoScripts);
    EXPECT_TRUE(fx.Errors.empty());
    std::error_code ec;
    EXPECT_FALSE(fs::exists(fx.ContentRoot / "NativeScripts", ec)) << "nothing may be staged";
}

TEST(NativeScriptStaging, StagesDllPdbRelativeRecordAndEngineAbiMarker)
{
    StagingFixture fx("ge_nss_happy");
    fx.WriteModule(kEngineAbi);

    const auto outcome = StageNativeUserScriptsForPackage(fx.Root, fx.ContentRoot, Digests(kEngineAbi), fx.Errors, true);

    ASSERT_EQ(outcome, NativeScriptStageOutcome::Staged) << (fx.Errors.empty() ? "" : fx.Errors.front());
    const fs::path dest = fx.ContentRoot / "NativeScripts";
    std::error_code ec;
    // The shadow-copy hash suffix is stripped so the module ships under its plain
    // name — which is also the PDB name its debug directory embeds.
    EXPECT_TRUE(fs::exists(dest / "UserScripts.dll", ec));
    EXPECT_TRUE(fs::exists(dest / "UserScripts.pdb", ec));

    const auto record = ns::ReadBuildCacheRecord(dest);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->DllPath, "NativeScripts/UserScripts.dll") << "record must be game-root-relative";
    EXPECT_EQ(record->AbiDigest, kEngineAbi);
    EXPECT_EQ(ns::ReadEngineAbiMarker(dest), kEngineAbi);
}

TEST(NativeScriptStaging, MissingPdbStillStagesTheModule)
{
    StagingFixture fx("ge_nss_nopdb");
    fx.WriteModule(kEngineAbi, /*withPdb=*/false);

    const auto outcome = StageNativeUserScriptsForPackage(fx.Root, fx.ContentRoot, Digests(kEngineAbi), fx.Errors, false);

    EXPECT_EQ(outcome, NativeScriptStageOutcome::Staged);
    std::error_code ec;
    EXPECT_TRUE(fs::exists(fx.ContentRoot / "NativeScripts" / "UserScripts.dll", ec));
    EXPECT_FALSE(fs::exists(fx.ContentRoot / "NativeScripts" / "UserScripts.pdb", ec));
}

TEST(NativeScriptStaging, RecordedButMissingDllFailsTheBuild)
{
    StagingFixture fx("ge_nss_missingdll");
    const fs::path dll = fx.WriteModule(kEngineAbi);
    std::error_code ec;
    fs::remove(dll, ec);

    EXPECT_EQ(StageNativeUserScriptsForPackage(fx.Root, fx.ContentRoot, Digests(kEngineAbi), fx.Errors, false),
              NativeScriptStageOutcome::Failed);
    ASSERT_FALSE(fx.Errors.empty());
    EXPECT_NE(fx.Errors.front().find("missing on disk"), std::string::npos);
}

TEST(NativeScriptStaging, StaleEngineAbiFailsTheBuild)
{
    StagingFixture fx("ge_nss_stale");
    fx.WriteModule("aaaabbbbccccdddd"); // built against some other engine

    EXPECT_EQ(StageNativeUserScriptsForPackage(fx.Root, fx.ContentRoot, Digests(kEngineAbi), fx.Errors, false),
              NativeScriptStageOutcome::Failed);
    ASSERT_FALSE(fx.Errors.empty());
    EXPECT_NE(fx.Errors.front().find("stale"), std::string::npos);
    std::error_code ec;
    EXPECT_FALSE(fs::exists(fx.ContentRoot / "NativeScripts", ec)) << "a stale module must not be staged";
}

TEST(NativeScriptStaging, RecordWithoutAbiDigestFailsTheBuild)
{
    StagingFixture fx("ge_nss_noabi");
    fx.WriteModule("");

    EXPECT_EQ(StageNativeUserScriptsForPackage(fx.Root, fx.ContentRoot, Digests(kEngineAbi), fx.Errors, false),
              NativeScriptStageOutcome::Failed);
    ASSERT_FALSE(fx.Errors.empty());
}

TEST(NativeScriptStaging, UnknownCurrentEngineAbiFailsTheBuildWhenScriptsExist)
{
    StagingFixture fx("ge_nss_unknownengine");
    fx.WriteModule(kEngineAbi);

    EXPECT_EQ(StageNativeUserScriptsForPackage(fx.Root, fx.ContentRoot, Digests(""), fx.Errors, false),
              NativeScriptStageOutcome::Failed);
    ASSERT_FALSE(fx.Errors.empty());
}

TEST(NativeScriptStaging, RecordMatchingDisabledPackageDefinesAttributesThePackageSet)
{
    // The record was stamped while a now-disabled package's defines were active:
    // the CURRENT digest (defines dropped) mismatches, but the enabled+disabled
    // variant matches — the gate must say "package set changed", not "stale".
    StagingFixture fx("ge_nss_defines_disabled");
    fx.WriteModule("digest-with-pkg-defines");

    const EngineAbiDigestSet digests{kEngineAbi, kEngineAbi, "digest-with-pkg-defines"};
    EXPECT_EQ(StageNativeUserScriptsForPackage(fx.Root, fx.ContentRoot, digests, fx.Errors, false),
              NativeScriptStageOutcome::Failed);
    ASSERT_FALSE(fx.Errors.empty());
    EXPECT_NE(fx.Errors.front().find("now-DISABLED package"), std::string::npos);
    EXPECT_EQ(fx.Errors.front().find("stale relative to this engine"), std::string::npos);
}

TEST(NativeScriptStaging, RecordMatchingNoDefinesDigestAttributesTheNewPackageDefines)
{
    // The record predates the current package defines (a package was enabled
    // since the build): CURRENT (with defines) mismatches, the no-defines
    // variant matches — attributed to the package set, not the engine.
    StagingFixture fx("ge_nss_defines_added");
    fx.WriteModule("digest-no-defines");

    const EngineAbiDigestSet digests{"digest-with-new-defines", "digest-no-defines", ""};
    EXPECT_EQ(StageNativeUserScriptsForPackage(fx.Root, fx.ContentRoot, digests, fx.Errors, false),
              NativeScriptStageOutcome::Failed);
    ASSERT_FALSE(fx.Errors.empty());
    EXPECT_NE(fx.Errors.front().find("before the current package defines"), std::string::npos);
    EXPECT_EQ(fx.Errors.front().find("stale relative to this engine"), std::string::npos);
}

TEST(NativeScriptStaging, OmitsDebugSymbolsUnlessRequested)
{
    StagingFixture fx("ge_nss_no_symbols");
    fx.WriteModule(kEngineAbi);
    ASSERT_EQ(StageNativeUserScriptsForPackage(fx.Root, fx.ContentRoot, Digests(kEngineAbi), fx.Errors, false),
              NativeScriptStageOutcome::Staged);
    EXPECT_TRUE(fs::exists(fx.ContentRoot / "NativeScripts/UserScripts.dll"));
    EXPECT_FALSE(fs::exists(fx.ContentRoot / "NativeScripts/UserScripts.pdb"));
}
