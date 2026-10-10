// Asset path identity is case-insensitive: one file is one store row and one
// GUID whatever case a query, a platform, or a historical journal line spells
// it in.
//
// The defect these pin: the authoritative store indexed rows by the spelling
// they were written with, while every producer of those keys folds case
// (NormalizePathForMap on Windows/macOS, NormalizeForRegistryKey at every
// GUID::Derive callsite). A journal row written as "Models/X.gltf" was
// therefore invisible to the "models/x.gltf" the registry wrote back, so the
// store could never displace it and the journal accumulated a permanent dead
// row per asset. In the Biggles project's journal, 24 of 26 assets carry two.
//
// Two identity schemes meet here and they fail differently:
//  - DERIVED (the editor's project source, AssetManager.cpp): identity is
//    Derive(namespace, "project/" + case-folded path), so both spellings
//    already derive to ONE GUID. The duplication is rows, not identities, and
//    the retired row holds a pre-flip random GUID that content may still bind.
//  - STORED (package mounts): the row's GUID IS the identity, so a second row
//    is a second identity for one file — the sharper failure.
//
// Either way the retired GUID is redirected onto the survivor and cascaded
// through subassets.deriveKeys, the way an offline rename heal does, so a
// reference authored against it keeps resolving.

#include <gtest/gtest.h>

#include "AssetCore/PathNormalization.h"
#include "AssetCore/SubassetDeriveKeys.h"
#include "AssetDatabase/AssetDatabasePaths.h"
#include "AssetDatabase/IAssetStore.h"
#include "Assets/AssetRegistry.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "StagedTestPaths.h"
#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::AssetDatabase;

namespace
{

void WriteFileText(const std::filesystem::path& p, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << p.string();
    out << text;
}

bool ReadFileText(const std::filesystem::path& p, std::string& out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open())
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

AssetSourceDesc MakeDerivedProjectDesc(const std::filesystem::path& root)
{
    const AssetDatabasePaths dbPaths = GetDefaultPathsForAssetRoot(root, {}, {});
    AssetSourceDesc desc{};
    desc.Alias = "project";
    desc.Root = root;
    desc.DerivedIdentity = true;
    desc.AuthoritativeDbFile = dbPaths.authoritativeFile;
    desc.CacheRoot = dbPaths.cacheRoot;
    desc.Priority = 100;
    return desc;
}

std::filesystem::path AuthoritativeFileFor(const std::filesystem::path& root)
{
    return GetDefaultPathsForAssetRoot(root, {}, {}).authoritativeFile;
}

// One editor session over a derived mount — the editor's own project-source
// shape (AssetManager::MountProjectSource). Mirrors AssetDbSubassetCascadeTests.
template <typename Body>
void RunDerivedSession(const std::filesystem::path& root, Body&& body)
{
    JobSystem::WorkStealingThreadPool pool(2);
    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(&pool));
    ASSERT_TRUE(reg.RegisterSource(MakeDerivedProjectDesc(root)));
    reg.WaitForStartupScan();
    body(reg);
    reg.Shutdown();
}

// One session over a STORED-identity project source — the Initialize overload
// that mounts with DerivedIdentity=false, where the journal's GUID is identity.
template <typename Body>
void RunStoredSession(const std::filesystem::path& root, Body&& body)
{
    const AssetDatabasePaths dbPaths = GetDefaultPathsForAssetRoot(root, {}, {});
    JobSystem::WorkStealingThreadPool pool(2);
    AssetRegistry reg;
    ASSERT_TRUE(reg.Initialize(root, &pool, dbPaths.authoritativeFile, dbPaths.cacheRoot));
    reg.WaitForStartupScan();
    body(reg);
    reg.Shutdown();
}

// Live store rows grouped by the case-folded path key. A well-formed store has
// exactly one row per group; a case-shadowed one has two.
std::vector<std::string> PathKeysWithMultipleRows(const IAssetStore& store)
{
    std::map<std::string, std::vector<GUID>> byKey;
    for (const auto& rec : store.EnumerateAssets())
    {
        if (rec.path.empty())
            continue;
        byKey[AssetPaths::NormalizeForRegistryKey(rec.path)].push_back(rec.guid);
    }
    std::vector<std::string> shadowed;
    for (const auto& [key, guids] : byKey)
    {
        if (guids.size() > 1)
            shadowed.push_back(key);
    }
    return shadowed;
}

// How many path keys a raw journal spells more than one way. Read straight off
// the text so a fixture can be checked for the defect it is meant to carry,
// without mounting anything.
size_t CountShadowedPathKeys(const std::string& journalText)
{
    std::map<std::string, std::set<std::string>> byKey;
    std::istringstream lines(journalText);
    std::string line;
    while (std::getline(lines, line))
    {
        const size_t pathPos = line.find("\"path\":\"");
        const size_t guidPos = line.find("\"guid\":\"");
        if (pathPos == std::string::npos || guidPos == std::string::npos)
            continue;
        const size_t pathBegin = pathPos + 8;
        const size_t guidBegin = guidPos + 8;
        const size_t pathEnd = line.find('"', pathBegin);
        const size_t guidEnd = line.find('"', guidBegin);
        if (pathEnd == std::string::npos || guidEnd == std::string::npos)
            continue;
        byKey[AssetPaths::NormalizeForRegistryKey(line.substr(pathBegin, pathEnd - pathBegin))]
            .insert(line.substr(guidBegin, guidEnd - guidBegin));
    }
    size_t shadowed = 0;
    for (const auto& [key, guids] : byKey)
    {
        if (guids.size() > 1)
            ++shadowed;
    }
    return shadowed;
}

// A v2 journal built line by line, so a test can seed exactly the row order a
// defective session produced.
class JournalBuilder
{
  public:
    JournalBuilder() { m_Lines.push_back(R"({"format":"assetdb","version":2})"); }

    JournalBuilder& Asset(const GUID& guid, const std::string& path, const std::string& type)
    {
        m_Lines.push_back("{\"guid\":\"" + guid.ToString() + "\",\"path\":\"" + path +
                          "\",\"type\":\"" + type + "\"}");
        return *this;
    }

    JournalBuilder& AssetWithDeriveKeys(const GUID& guid, const std::string& path,
                                        const std::string& type, const Vector<String>& keys)
    {
        std::string joined;
        for (const String& key : keys)
        {
            if (!joined.empty())
                joined += "\\n"; // escaped newline inside the JSON string
            joined += key;
        }
        m_Lines.push_back("{\"guid\":\"" + guid.ToString() + "\",\"path\":\"" + path +
                          "\",\"type\":\"" + type + "\",\"kv\":{\"" +
                          kSubassetDeriveKeysKvKey + "\":\"" + joined + "\"}}");
        return *this;
    }

    std::string Build() const
    {
        std::string out;
        for (const auto& line : m_Lines)
        {
            out += line;
            out += '\n';
        }
        return out;
    }

  private:
    std::vector<std::string> m_Lines;
};

// Every GUID-shaped token in a text file. Used to hold the shipped scene's
// bindings against the migrated journal without transcribing them here — a
// transcription would stop testing the scene the moment it is re-saved.
std::set<std::string> ExtractGuidStrings(const std::string& text)
{
    std::set<std::string> found;
    constexpr size_t kGuidLen = 36;
    auto isHex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    for (size_t i = 0; i + kGuidLen <= text.size(); ++i)
    {
        bool ok = true;
        for (size_t j = 0; j < kGuidLen && ok; ++j)
        {
            const char c = text[i + j];
            if (j == 8 || j == 13 || j == 18 || j == 23)
                ok = (c == '-');
            else
                ok = isHex(c);
        }
        if (ok)
        {
            found.insert(text.substr(i, kGuidLen));
            i += kGuidLen - 1;
        }
    }
    return found;
}

// The Biggles identities this suite pins by value.
//
// kBigglesModelSurvivor is not an arbitrary GUID: it is
// Derive(Derive(Null, "asset-source-namespace:project"),
//        "project/models/planes/aircodh2/scene.gltf"),
// i.e. what the derived-identity project source computes for that path on any
// machine — the test asserts that equality rather than trusting the constant.
// kBigglesModelRetired is the pre-flip random GUID its dead journal row holds.
constexpr const char* kBigglesModelSurvivor = "78fd7c8b-97d3-4f9e-9a7d-b76084b32c6f";
constexpr const char* kBigglesModelRetired = "fd86a9e5-94f4-41f5-9c2f-cf6cb42f8a7a";
constexpr const char* kBigglesModelRel = "models/planes/aircodh2/scene.gltf";

} // namespace

// ---------------------------------------------------------------------------
// Derived identity: the spelling in the journal must not fork the row
// ---------------------------------------------------------------------------

// The defect in its original form. A journal row carrying the on-disk spelling
// and a pre-flip GUID, opened by a build that writes back the folded path:
// the store could not see that the two spellings were one file, so it kept
// both rows forever.
TEST(AssetDbCaseIdentity, JournalSpellingUnlikeTheQueryDoesNotForkTheRow)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_case_identity_nofork");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteFileText(root / "Sub" / "Model.txt", "container content");

    const GUID preFlip("a1b2c3d4-0000-4000-8000-000000000001");
    WriteFileText(AuthoritativeFileFor(root),
                  JournalBuilder().Asset(preFlip, "Sub/Model.txt", "Unknown").Build());

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);

        EXPECT_TRUE(PathKeysWithMultipleRows(*pinned->Store).empty())
            << "the session added a second row for a file the journal already names";

        // Identity for a derived source is a function of the folded path, so
        // every spelling of it resolves to the one row.
        const GUID derived = reg.GetOrCreateAssetGUID(root / "Sub" / "Model.txt");
        ASSERT_FALSE(derived.IsNull());
        for (const char* spelling : {"Sub/Model.txt", "sub/model.txt", "SUB/MODEL.TXT"})
        {
            const auto found = pinned->Store->LookupGuidByPath(spelling);
            ASSERT_TRUE(found.has_value()) << "no store row for '" << spelling << "'";
            EXPECT_EQ(*found, derived) << "'" << spelling << "' resolved to a forked row";
        }
        EXPECT_EQ(reg.GetOrCreateAssetGUID(root / "sub" / "model.txt"), derived);

        // The retired pre-flip identity forwards instead of dangling.
        EXPECT_EQ(reg.ResolveGuid(preFlip), derived)
            << "a reference holding the pre-flip GUID does not resolve forward";
    });

    fs::remove_all(root, ec);
}

// The forked state is reachable from a second direction: a session that writes
// its rows back and a later session that re-reads them must converge, not
// accumulate.
TEST(AssetDbCaseIdentity, RemountingAProjectDoesNotAccumulateRows)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_case_identity_remount");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteFileText(root / "Sub" / "Model.txt", "container content");
    WriteFileText(root / "Sub" / "Sibling.txt", "sibling content");

    size_t firstSessionRows = 0;
    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        firstSessionRows = pinned->Store->CountAssets();
        EXPECT_GE(firstSessionRows, 2u);
        ASSERT_TRUE(reg.SaveToFile({}));
    });

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        EXPECT_TRUE(PathKeysWithMultipleRows(*pinned->Store).empty());
        EXPECT_EQ(pinned->Store->CountAssets(), firstSessionRows) << "a remount grew the store";
    });

    fs::remove_all(root, ec);
}

// A journal that already carries both rows — the state the shipped example is
// committed in. The dead row is retired at mount, not on some later write.
TEST(AssetDbCaseIdentity, ShadowPairInTheJournalIsMergedAtMount)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_case_identity_merge");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteFileText(root / "Sub" / "Model.txt", "container content");

    const GUID preFlip("a1b2c3d4-0000-4000-8000-00000000000a");
    const GUID derivedInJournal =
        AssetRegistry::DeriveGuidForSourcePath("project", "sub/model.txt");
    ASSERT_FALSE(derivedInJournal.IsNull());

    WriteFileText(AuthoritativeFileFor(root),
                  JournalBuilder()
                      .Asset(preFlip, "Sub/Model.txt", "Unknown")
                      .Asset(derivedInJournal, "sub/model.txt", "Unknown")
                      .Build());

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);

        EXPECT_TRUE(PathKeysWithMultipleRows(*pinned->Store).empty())
            << "both rows are still live";

        const auto owner = pinned->Store->LookupGuidByPath("Sub/Model.txt");
        ASSERT_TRUE(owner.has_value());
        EXPECT_EQ(*owner, derivedInJournal);

        const auto target = pinned->Store->ResolveRedirect(preFlip);
        ASSERT_TRUE(target.has_value()) << "the retired identity was dropped, not redirected";
        EXPECT_EQ(*target, derivedInJournal);
        EXPECT_EQ(reg.ResolveGuid(preFlip), derivedInJournal);
    });

    fs::remove_all(root, ec);
}

// Same spelling, two GUIDs: not a case shadow, but replay arbitration handles
// it the same way — the last claimant keeps the key and the loser is retired
// onto it as a redirect rather than kept as an inconsistent duplicate.
TEST(AssetDbCaseIdentity, SameSpellingTwoGuidCollisionEvictsAndRedirects)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_case_identity_samespelling");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteFileText(root / "Sub" / "Model.txt", "container content");

    const GUID preFlip("a1b2c3d4-0000-4000-8000-00000000000b");
    const GUID derivedInJournal =
        AssetRegistry::DeriveGuidForSourcePath("project", "sub/model.txt");
    ASSERT_FALSE(derivedInJournal.IsNull());

    WriteFileText(AuthoritativeFileFor(root),
                  JournalBuilder()
                      .Asset(preFlip, "Sub/Model.txt", "Unknown")
                      .Asset(derivedInJournal, "Sub/Model.txt", "Unknown")
                      .Build());

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);

        EXPECT_TRUE(PathKeysWithMultipleRows(*pinned->Store).empty())
            << "both rows are still live";

        const auto owner = pinned->Store->LookupGuidByPath("Sub/Model.txt");
        ASSERT_TRUE(owner.has_value());
        EXPECT_EQ(*owner, derivedInJournal);

        const auto target = pinned->Store->ResolveRedirect(preFlip);
        ASSERT_TRUE(target.has_value()) << "the evicted identity was dropped, not redirected";
        EXPECT_EQ(*target, derivedInJournal);
        EXPECT_EQ(reg.ResolveGuid(preFlip), derivedInJournal);
    });

    fs::remove_all(root, ec);
}

// The other half of the rule: on a derived-identity mount the path's own derived GUID is never a
// retired identity, even when a later journal row at the same spelling displaces it at replay. No
// redirect may lead away from it; the other GUID is the one forwarded, onto the derived one.
TEST(AssetDbCaseIdentity, ADisplacedDerivedIdentityIsNeverRedirected)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_case_identity_derived_displaced");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteFileText(root / "Sub" / "Model.txt", "container content");

    const GUID other("a1b2c3d4-0000-4000-8000-00000000000c");
    const GUID derived = AssetRegistry::DeriveGuidForSourcePath("project", "sub/model.txt");
    ASSERT_FALSE(derived.IsNull());

    WriteFileText(AuthoritativeFileFor(root),
                  JournalBuilder()
                      .Asset(derived, "Sub/Model.txt", "Unknown")
                      .Asset(other, "Sub/Model.txt", "Unknown")
                      .Build());

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);

        EXPECT_FALSE(pinned->Store->ResolveRedirect(derived).has_value())
            << "the path's own derived identity was redirected away";
        EXPECT_EQ(reg.ResolveGuid(derived), derived);
        EXPECT_EQ(reg.GetOrCreateAssetGUID(root / "Sub" / "Model.txt"), derived);
        EXPECT_EQ(reg.ResolveGuid(other), derived);
    });

    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// Stored identity: the sharper failure, where a second row IS a second asset
// ---------------------------------------------------------------------------

// Under a stored-identity mount the journal's GUID is the identity, so the
// merge has to pick one and forward the other. It keeps the journal's last
// claimant of the path key — v2 replay is last-write-wins, and folding the key
// changes only that the two spellings are one key, not who wins.
TEST(AssetDbCaseIdentity, StoredIdentityShadowPairMergesOntoTheLastClaimant)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_case_identity_stored");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteFileText(root / "Sub" / "Model.txt", "container content");

    const GUID retired("a1b2c3d4-0000-4000-8000-00000000010a");
    const GUID survivor("a1b2c3d4-0000-4000-8000-00000000010b");
    const Vector<String> retiredKeys = {ModelMaterialDeriveKey(0), EmbeddedClipDeriveKey(0)};
    const Vector<String> survivorKeys = {ModelMaterialDeriveKey(0), ModelMaterialDeriveKey(1)};

    WriteFileText(AuthoritativeFileFor(root),
                  JournalBuilder()
                      .AssetWithDeriveKeys(retired, "Sub/Model.txt", "Model", retiredKeys)
                      .AssetWithDeriveKeys(survivor, "sub/model.txt", "Model", survivorKeys)
                      .Build());

    RunStoredSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);

        EXPECT_TRUE(PathKeysWithMultipleRows(*pinned->Store).empty());

        const auto owner = pinned->Store->LookupGuidByPath("Sub/Model.txt");
        ASSERT_TRUE(owner.has_value());
        EXPECT_EQ(*owner, survivor) << "the merge did not keep the journal's last claimant";
        EXPECT_EQ(reg.ResolveGuid(retired), survivor);

        // The cascade the shipped example's constraint is about: identities
        // derived from the retired container follow it.
        for (const String& key : retiredKeys)
        {
            EXPECT_EQ(reg.ResolveGuid(GUID::Derive(retired, key)), GUID::Derive(survivor, key))
                << "no cascade for retired key '" << key << "'";
        }

        // The acid property. Scenes bind the SURVIVOR's derived subassets; the
        // migration must not move them, or every such binding goes white.
        for (const String& key : survivorKeys)
        {
            const GUID sub = GUID::Derive(survivor, key);
            EXPECT_EQ(reg.ResolveGuid(sub), sub)
                << "the survivor's subasset '" << key << "' was re-keyed by the migration";
        }

        // The survivor keeps its own key row; it inherits only what it lacked.
        std::string row;
        ASSERT_TRUE(pinned->Store->TryGetKeyValue(survivor, kSubassetDeriveKeysKvKey, row));
        EXPECT_EQ(row, JoinSubassetDeriveKeys(survivorKeys));
    });

    fs::remove_all(root, ec);
}

// A merge is identity movement, so it must be journaled: a second session that
// re-reads the file resolves the retired GUID without redoing anything, and
// does not grow the redirect population.
TEST(AssetDbCaseIdentity, ShadowMergeSurvivesARestart)
{
    namespace fs = std::filesystem;
    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_case_identity_persist");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteFileText(root / "Sub" / "Model.txt", "container content");

    const GUID retired("a1b2c3d4-0000-4000-8000-00000000020a");
    const GUID survivor("a1b2c3d4-0000-4000-8000-00000000020b");

    WriteFileText(AuthoritativeFileFor(root), JournalBuilder()
                                                  .Asset(retired, "Sub/Model.txt", "Unknown")
                                                  .Asset(survivor, "sub/model.txt", "Unknown")
                                                  .Build());

    size_t redirectsAfterMerge = 0;
    RunStoredSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        EXPECT_EQ(reg.ResolveGuid(retired), survivor);
        redirectsAfterMerge = pinned->Store->EnumerateRedirects().size();
        EXPECT_GE(redirectsAfterMerge, 1u);
        ASSERT_TRUE(reg.SaveToFile({}));
    });

    RunStoredSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);
        EXPECT_EQ(reg.ResolveGuid(retired), survivor) << "the merge did not survive the restart";
        EXPECT_EQ(pinned->Store->EnumerateRedirects().size(), redirectsAfterMerge)
            << "a re-run of the migration grew the redirect population";
        EXPECT_TRUE(PathKeysWithMultipleRows(*pinned->Store).empty());
    });

    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------
// The real project
// ---------------------------------------------------------------------------

// Migrates the Biggles journal as it stood BEFORE the case-shadow fix — a
// frozen byte-identical fixture (staged by StageTestAssets), since the live
// journal is migrated and would make every assertion here vacuous — and holds
// the result against the REAL Lanscape.scene.
//
// No asset files are created. The migration is a journal-only operation that
// runs at mount before the scan, and staging the 5 MB model payload to prove a
// property that does not depend on it would only make the fixture expensive.
TEST(AssetDbCaseIdentity, FrozenBigglesJournalMigratesWithLanscapeBindingsIntact)
{
    namespace fs = std::filesystem;

    const fs::path stagedRoot = TestPaths::StagedRoot();
    const fs::path stagedJournal =
        stagedRoot / "Tests" / "Assets" / "TestData" / "BigglesCaseShadow.assetdb";
    const fs::path stagedScene =
        stagedRoot / "Tests" / "Projects" / "Biggles" / "Assets" / "Scenes" / "Lanscape.scene";

    std::string journalText;
    std::string sceneText;
    ASSERT_TRUE(ReadFileText(stagedJournal, journalText)) << stagedJournal.string();
    ASSERT_TRUE(ReadFileText(stagedScene, sceneText)) << stagedScene.string();

    const std::set<std::string> sceneGuids = ExtractGuidStrings(sceneText);
    ASSERT_FALSE(sceneGuids.empty()) << "no GUID bindings found in " << stagedScene.string();

    const GUID survivor{std::string(kBigglesModelSurvivor)};
    const GUID retired{std::string(kBigglesModelRetired)};

    // The survivor is not a magic constant: it is what the derived-identity
    // project source computes for that path. Nothing about which row wins can
    // move it, which is why the scene's material bindings are safe.
    EXPECT_EQ(AssetRegistry::DeriveGuidForSourcePath("project", kBigglesModelRel), survivor)
        << "the model's derived identity is no longer the GUID this suite pins";
    EXPECT_EQ(AssetRegistry::DeriveGuidForSourcePath("project",
                                                          "Models/Planes/AircoDH2/scene.gltf"),
              survivor)
        << "derivation is case-sensitive — both spellings must yield one identity";
    ASSERT_NE(sceneGuids.find(kBigglesModelSurvivor), sceneGuids.end())
        << "the project scene no longer binds the model identity this test pins";

    // Precondition, not decoration: this test is only an instrument while the
    // fixture CARRIES the duplicate rows. It is the frozen pre-migration copy
    // of the journal Biggles carried — regenerating it from a migrated journal
    // would make every assertion below pass vacuously.
    const size_t shadowedInFixture = CountShadowedPathKeys(journalText);
    ASSERT_GT(shadowedInFixture, 0u)
        << "the fixture no longer carries duplicate rows, so this test would "
           "prove nothing — restore the frozen pre-migration journal copy";

    const fs::path root = TestUtils::MakeUniqueTempDirectory("ge_case_identity_biggles");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    WriteFileText(AuthoritativeFileFor(root), journalText);

    RunDerivedSession(root, [&](AssetRegistry& reg) {
        auto pinned = reg.ProjectSourcePinned();
        ASSERT_TRUE(pinned && pinned->Store);

        const std::vector<std::string> shadowed = PathKeysWithMultipleRows(*pinned->Store);
        EXPECT_TRUE(shadowed.empty())
            << shadowed.size() << " paths still carry two rows, e.g. '"
            << (shadowed.empty() ? std::string{} : shadowed.front()) << "'";

        // Both spellings the journal uses name the one surviving row.
        for (const char* spelling : {"Models/Planes/AircoDH2/scene.gltf", kBigglesModelRel})
        {
            const auto found = pinned->Store->LookupGuidByPath(spelling);
            ASSERT_TRUE(found.has_value()) << spelling;
            EXPECT_EQ(*found, survivor) << spelling;
        }

        EXPECT_EQ(reg.ResolveGuid(retired), survivor)
            << "the retired pre-flip identity does not resolve onto the survivor";
        EXPECT_EQ(reg.ResolveGuid(survivor), survivor)
            << "the live identity was redirected away from itself";

        // The acid test. Every GUID the project scene binds must still resolve
        // to itself — subasset identities have no store row of their own, so
        // "resolves to itself" is what a surviving binding looks like.
        for (const std::string& guidText : sceneGuids)
        {
            const GUID bound{guidText};
            if (bound.IsNull())
                continue;
            EXPECT_EQ(reg.ResolveGuid(bound), bound)
                << "scene binding " << guidText << " was moved by the migration";
        }

        // Pinned by value: the scene's four materials are
        // Derive(<survivor>, "material/0..3").
        for (uint32 i = 0; i < 4; ++i)
        {
            const GUID material = GUID::Derive(survivor, ModelMaterialDeriveKey(i));
            EXPECT_NE(sceneGuids.find(material.ToString()), sceneGuids.end())
                << "material/" << i << " (" << material.ToString()
                << ") is no longer bound by the shipped scene";
        }
    });

    fs::remove_all(root, ec);
}
