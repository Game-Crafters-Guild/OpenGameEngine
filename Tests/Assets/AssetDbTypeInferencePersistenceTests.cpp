#include <gtest/gtest.h>

#include "AssetCore/AssetTypes.h"
#include "Assets/AssetRegistry.h"
#include "Assets/ParserRegistry.h"
#include "AssetDatabase/AssetStore_TextJsonl.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "TestTempDir.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{
static void WriteTextFile(const std::filesystem::path& p, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << p.string();
    out << text;
}
} // namespace

TEST(AssetDatabase, TypeInferenceUpgradesUnknownTypesAndPersistsAcrossRuns)
{
    namespace fs = std::filesystem;

    const fs::path tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_assetdb_type_inference");
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);
    fs::create_directories(tmpRoot, ec);

    // Create a few representative assets (content doesn't matter for registry/type tests).
    const fs::path pGltf = tmpRoot / "model.gltf";
    const fs::path pGlb = tmpRoot / "model.glb";
    const fs::path pMat = tmpRoot / "material.mat";
    const fs::path pCss = tmpRoot / "style.css";
    const fs::path pUiXml = tmpRoot / "layout.xml";
    const fs::path pGenericXml = tmpRoot / "doc.xml";
    const fs::path pPng = tmpRoot / "image.png";
    const fs::path pSvg = tmpRoot / "vector.svg";
    const fs::path pKtx2 = tmpRoot / "ibl.ktx2";

    WriteTextFile(pGltf, "DUMMY");
    WriteTextFile(pGlb, "DUMMY");
    WriteTextFile(pMat, "DUMMY");
    WriteTextFile(pCss, "DUMMY");
    // UI layout XML should be sniffed as UILayout, while generic XML should be classified as XML.
    WriteTextFile(pUiXml, "<?xml version=\"1.0\"?>\n<UI id=\"root\"></UI>\n");
    WriteTextFile(pGenericXml, "<?xml version=\"1.0\"?>\n<root></root>\n");
    WriteTextFile(pPng, "DUMMY");
    WriteTextFile(pSvg, "<svg></svg>");
    WriteTextFile(pKtx2, "DUMMY");

    // Pre-create an authoritative AssetDatabase.assetdb with all types set to Unknown.
    const GUID gGltf("11111111-1111-1111-1111-111111111111");
    const GUID gGlb("22222222-2222-2222-2222-222222222222");
    const GUID gMat("33333333-3333-3333-3333-333333333333");
    const GUID gCss("44444444-4444-4444-4444-444444444444");
    const GUID gUiXml("55555555-5555-5555-5555-555555555555");
    const GUID gGenericXml("88888888-8888-8888-8888-888888888888");
    const GUID gPng("66666666-6666-6666-6666-666666666666");
    const GUID gSvg("77777777-7777-7777-7777-777777777777");
    const GUID gKtx2("99999999-9999-9999-9999-999999999999");

    const fs::path dbPath = tmpRoot / "AssetDatabase.assetdb";
    {
        std::ofstream out(dbPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open()) << dbPath.string();

        auto emit = [&](const GUID& guid, const char* rel)
        {
            out << "{\"guid\":\"" << guid.ToString() << "\","
                << "\"path\":\"" << rel << "\","
                << "\"type\":\"Unknown\","
                << "\"missing\":false,"
                << "\"kv\":{}}\n";
        };

        emit(gGltf, "model.gltf");
        emit(gGlb, "model.glb");
        emit(gMat, "material.mat");
        emit(gCss, "style.css");
        emit(gUiXml, "layout.xml");
        emit(gGenericXml, "doc.xml");
        emit(gPng, "image.png");
        emit(gSvg, "vector.svg");
        emit(gKtx2, "ibl.ktx2");
    }

    JobSystem::WorkStealingThreadPool pool(2);

    // First run: load Unknown-typed DB and ensure types are inferred correctly.
    {
        ParserRegistry parsers;
        ASSERT_TRUE(parsers.Initialize());

        AssetRegistry reg;
        reg.SetParserRegistry(&parsers);
        ASSERT_TRUE(reg.Initialize(tmpRoot, &pool));

        AssetMetadata md{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(pGltf, md));
        EXPECT_EQ(md.Type, AssetType::Model);
        ASSERT_TRUE(reg.TryGetAssetMetadata(pGlb, md));
        EXPECT_EQ(md.Type, AssetType::Model);
        ASSERT_TRUE(reg.TryGetAssetMetadata(pMat, md));
        EXPECT_EQ(md.Type, AssetType::Material);
        ASSERT_TRUE(reg.TryGetAssetMetadata(pCss, md));
        EXPECT_EQ(md.Type, AssetType::UIStyle);
        ASSERT_TRUE(reg.TryGetAssetMetadata(pUiXml, md));
        EXPECT_EQ(md.Type, AssetType::UILayout);
        ASSERT_TRUE(reg.TryGetAssetMetadata(pGenericXml, md));
        EXPECT_EQ(md.Type, AssetType::XML);
        ASSERT_TRUE(reg.TryGetAssetMetadata(pPng, md));
        EXPECT_EQ(md.Type, AssetType::Texture);
        ASSERT_TRUE(reg.TryGetAssetMetadata(pKtx2, md));
        EXPECT_EQ(md.Type, AssetType::Texture);

        // Persist, then verify the authoritative file stores the inferred types (not Unknown).
        ASSERT_TRUE(reg.SaveToFile({}));
        reg.Shutdown();
    }

    {
        AssetDatabase::AssetStore_TextJsonl store;
        std::string err;
        ASSERT_TRUE(store.LoadFromFile(dbPath, &err)) << err;

        AssetDatabase::AssetRecord r{};
        ASSERT_TRUE(store.TryGetAsset(gGltf, r));
        EXPECT_EQ(r.type, AssetType::Model);
        ASSERT_TRUE(store.TryGetAsset(gGlb, r));
        EXPECT_EQ(r.type, AssetType::Model);
        ASSERT_TRUE(store.TryGetAsset(gMat, r));
        EXPECT_EQ(r.type, AssetType::Material);
        ASSERT_TRUE(store.TryGetAsset(gCss, r));
        EXPECT_EQ(r.type, AssetType::UIStyle);
        ASSERT_TRUE(store.TryGetAsset(gUiXml, r));
        EXPECT_EQ(r.type, AssetType::UILayout);
        ASSERT_TRUE(store.TryGetAsset(gGenericXml, r));
        EXPECT_EQ(r.type, AssetType::XML);
        ASSERT_TRUE(store.TryGetAsset(gPng, r));
        EXPECT_EQ(r.type, AssetType::Texture);
        ASSERT_TRUE(store.TryGetAsset(gSvg, r));
        EXPECT_EQ(r.type, AssetType::Texture);
        ASSERT_TRUE(store.TryGetAsset(gKtx2, r));
        EXPECT_EQ(r.type, AssetType::Texture);
    }

    // Second run: without wiping the DB, types should remain correct (no "reset to Unknown").
    {
        ParserRegistry parsers;
        ASSERT_TRUE(parsers.Initialize());

        AssetRegistry reg2;
        reg2.SetParserRegistry(&parsers);
        ASSERT_TRUE(reg2.Initialize(tmpRoot, &pool));

        AssetMetadata md{};
        ASSERT_TRUE(reg2.TryGetAssetMetadata(pGltf, md));
        EXPECT_EQ(md.Type, AssetType::Model);
        ASSERT_TRUE(reg2.TryGetAssetMetadata(pKtx2, md));
        EXPECT_EQ(md.Type, AssetType::Texture);

        reg2.Shutdown();
    }

    fs::remove_all(tmpRoot, ec);
}

// A directory scan and a caller registering the same asset are two registrars
// of one record, and the scan's chunk can land after the caller's. When the
// scan cannot classify the file, its chunk carries no answer — and must not be
// allowed to overwrite the answer the caller supplied, in the registry's own
// view or on disk. Under CPU contention that ordering is what #1010 hit; here
// it is arranged directly, so the assertion does not depend on the scheduler.
//
// The scan chunk is built exactly as ProcessSingleAssetForScan builds one for
// an extension no table claims, including the "Unknown" type id that shape used
// to carry: a store that reads that string as an answer reintroduces the defect
// whatever the scan producer does.
TEST(AssetDatabase, ScanChunkLandingAfterRegistrationKeepsTheRegisteredType)
{
    namespace fs = std::filesystem;

    const fs::path tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_assetdb_scan_after_register");
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);
    fs::create_directories(tmpRoot, ec);
    const fs::path dbPath = tmpRoot / "AssetDatabase.assetdb";
    const fs::path assetPath = tmpRoot / "asset_texture.gedat";

    GUID guid;
    {
        ParserRegistry parsers;
        ASSERT_TRUE(parsers.Initialize());

        AssetRegistry reg;
        reg.SetParserRegistry(&parsers);
        // No job pool: the mount scan runs inline over an empty directory, so
        // the only registrations are the two this test issues, in this order.
        ASSERT_TRUE(reg.Initialize(tmpRoot, nullptr));

        WriteTextFile(assetPath, "DUMMY");
        guid = reg.GetOrCreateAssetGUID(assetPath);
        ASSERT_FALSE(guid.IsNull());

        AssetMetadata md{};
        md.Guid = guid;
        md.Path = assetPath;
        md.Name = assetPath.stem().string();
        md.Extension = ".gedat";
        md.Type = AssetType::Texture;
        ASSERT_TRUE(reg.RegisterAssetMetadata(md));

        // The arrangement under test: the typed registration is the state the
        // scan chunk is about to land on. Assert it took effect, or the test
        // below proves nothing.
        AssetMetadata before{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(guid, before));
        ASSERT_EQ(before.Type, AssetType::Texture);

        Vector<AssetMetadata> scanChunk;
        AssetMetadata scanned{};
        scanned.Guid = guid;
        scanned.Path = assetPath;
        scanned.Type = AssetType::Unknown;
        scanned.TypeId = AssetTypeToString(AssetType::Unknown);
        scanChunk.push_back(std::move(scanned));
        ASSERT_EQ(reg.RegisterAssetMetadataBatch(std::move(scanChunk), nullptr), 1u);

        AssetMetadata after{};
        ASSERT_TRUE(reg.TryGetAssetMetadata(guid, after));
        EXPECT_EQ(after.Type, AssetType::Texture)
            << "an unclassified scan chunk erased the registered type; the registry reports '"
            << AssetTypeToString(after.Type) << "'";
        EXPECT_EQ(after.TypeId, std::string("Texture"));

        ASSERT_TRUE(reg.SaveToFile({}));
        reg.Shutdown();
    }

    {
        AssetDatabase::AssetStore_TextJsonl store;
        std::string err;
        ASSERT_TRUE(store.LoadFromFile(dbPath, &err)) << err;

        AssetDatabase::AssetRecord r{};
        ASSERT_TRUE(store.TryGetAsset(guid, r));
        EXPECT_EQ(r.type, AssetType::Texture)
            << "an unclassified scan chunk persisted over the registered type as '"
            << AssetTypeToString(r.type) << "'";
        EXPECT_EQ(r.typeId, std::string("Texture"));
    }

    fs::remove_all(tmpRoot, ec);
}

// The store is the arbiter of what a registration means, because the merge that
// decides it has to happen inside the store's lock: two registrars that read,
// merge and write outside it resolve last-writer-wins on stale snapshots.
TEST(AssetDatabase, StoreMergeKeepsTheAnswerAnObservationDoesNotCarry)
{
    AssetDatabase::AssetStore_TextJsonl store;
    const GUID guid("aaaaaaaa-0000-4000-8000-00000000c001");
    ASSERT_FALSE(guid.IsNull());

    AssetDatabase::AssetObservation classified{};
    classified.guid = guid;
    classified.path = "asset.gedat";
    classified.type = AssetType::Texture;

    AssetDatabase::AssetRecord merged{};
    EXPECT_EQ(store.MergeObservation(classified, merged, nullptr), AssetDatabase::StoreMergeResult::Changed);
    EXPECT_EQ(merged.type, AssetType::Texture);
    EXPECT_EQ(merged.typeId, std::string("Texture"));

    // User-authored metadata a concurrent writer added is not the observer's to
    // drop, and it never appears in an observation.
    ASSERT_TRUE(store.SetKeyValue(guid, "shader_stage", "fragment", nullptr));

    // An observation that could not classify the file: same path, no answer.
    AssetDatabase::AssetObservation unclassified{};
    unclassified.guid = guid;
    unclassified.path = "asset.gedat";
    unclassified.type = AssetType::Unknown;
    unclassified.typeId = AssetTypeToString(AssetType::Unknown);

    AssetDatabase::AssetRecord after{};
    EXPECT_EQ(store.MergeObservation(unclassified, after, nullptr), AssetDatabase::StoreMergeResult::Unchanged);
    EXPECT_EQ(after.type, AssetType::Texture);
    EXPECT_EQ(after.typeId, std::string("Texture"));
    EXPECT_EQ(after.kv.at("shader_stage"), std::string("fragment"));

    // A programmable id the enum does not declare is still an answer.
    AssetDatabase::AssetObservation custom{};
    custom.guid = guid;
    custom.path = "asset.gedat";
    custom.typeId = "MyPlugin.CustomType";

    AssetDatabase::AssetRecord customMerged{};
    EXPECT_EQ(store.MergeObservation(custom, customMerged, nullptr), AssetDatabase::StoreMergeResult::Changed);
    EXPECT_EQ(customMerged.typeId, std::string("MyPlugin.CustomType"));

    // Observing an asset is seeing its file, so the tombstone clears.
    ASSERT_TRUE(store.MarkMissing(guid, true, nullptr));
    AssetDatabase::AssetRecord revived{};
    EXPECT_EQ(store.MergeObservation(unclassified, revived, nullptr), AssetDatabase::StoreMergeResult::Changed);
    EXPECT_FALSE(revived.missing);
    EXPECT_EQ(revived.typeId, std::string("MyPlugin.CustomType"));
}

// The registry validates an incoming metadata type before persisting it, and
// resets anything it does not recognize to Unknown. Every type the enum declares
// must survive that gate, or importing an asset of that type writes a record
// with no type at all.
//
// The extension here is deliberately one no extension table claims: extension
// re-inference heals a dropped type whenever the suffix happens to identify it,
// which masks the gate for most of the enum and for none of the types whose
// suffix is generic (.json sidecars) or not yet registered.
//
// The case list is generated from GE_ASSET_TYPE_LIST, so a type added to the
// enum is covered here with no edit.
TEST(AssetDatabase, RegistryPersistsEveryTypeTheEnumDeclares)
{
    namespace fs = std::filesystem;

    struct TypeCase
    {
        AssetType type;
        const char* name;
    };
    static constexpr TypeCase kAllTypes[] = {
#define GE_TEST_ASSET_TYPE_CASE(entry) {AssetType::entry, #entry},
        GE_ASSET_TYPE_LIST(GE_TEST_ASSET_TYPE_CASE)
#undef GE_TEST_ASSET_TYPE_CASE
    };

    const fs::path tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_assetdb_type_gate");
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);
    fs::create_directories(tmpRoot, ec);
    const fs::path dbPath = tmpRoot / "AssetDatabase.assetdb";

    std::vector<GUID> guids;
    std::vector<const TypeCase*> cases;

    JobSystem::WorkStealingThreadPool pool(2);
    {
        ParserRegistry parsers;
        ASSERT_TRUE(parsers.Initialize());

        AssetRegistry reg;
        reg.SetParserRegistry(&parsers);
        ASSERT_TRUE(reg.Initialize(tmpRoot, &pool));

        for (const TypeCase& c : kAllTypes)
        {
            // Unknown is the answer the gate falls back to; there is nothing to
            // preserve about it.
            if (c.type == AssetType::Unknown)
                continue;

            const fs::path path = tmpRoot / (std::string("asset_") + c.name + ".gedat");
            WriteTextFile(path, "DUMMY");

            const GUID guid = reg.GetOrCreateAssetGUID(path);
            ASSERT_FALSE(guid.IsNull()) << c.name;

            AssetMetadata md{};
            md.Guid = guid;
            md.Path = path;
            md.Name = path.stem().string();
            md.Extension = ".gedat";
            md.Type = c.type;
            // TypeId left empty so the registry derives it from the resolved
            // type — the field under test, not a value the caller supplied.
            ASSERT_TRUE(reg.RegisterAssetMetadata(md)) << c.name;

            guids.push_back(guid);
            cases.push_back(&c);
        }

        // The registry's own view, before anything touches disk.
        for (size_t i = 0; i < guids.size(); ++i)
        {
            AssetMetadata md{};
            ASSERT_TRUE(reg.TryGetAssetMetadata(guids[i], md)) << cases[i]->name;
            EXPECT_EQ(md.Type, cases[i]->type)
                << "registry dropped type '" << cases[i]->name << "'; it reports '"
                << AssetTypeToString(md.Type) << "'";
        }

        ASSERT_TRUE(reg.SaveToFile({}));
        reg.Shutdown();
    }

    // And the record it wrote: the type has to be on disk, or the next session
    // starts with an untyped asset.
    {
        AssetDatabase::AssetStore_TextJsonl store;
        std::string err;
        ASSERT_TRUE(store.LoadFromFile(dbPath, &err)) << err;

        for (size_t i = 0; i < guids.size(); ++i)
        {
            AssetDatabase::AssetRecord r{};
            ASSERT_TRUE(store.TryGetAsset(guids[i], r)) << cases[i]->name;
            EXPECT_EQ(r.type, cases[i]->type)
                << "type '" << cases[i]->name << "' was persisted as '"
                << AssetTypeToString(r.type) << "'";
            EXPECT_EQ(r.typeId, std::string(cases[i]->name));
        }
    }

    fs::remove_all(tmpRoot, ec);
}

// The store writes a record's type as AssetTypeToString() and parses it back on
// load. Every type the writer can emit must therefore read back as itself —
// without the extension-inference sweep in AssetRegistry, which is skipped
// entirely on warm start and would mask a broken parse here.
//
// The case list is generated from GE_ASSET_TYPE_LIST, so a type added to the
// enum is covered by this test with no edit here.
TEST(AssetDatabase, EveryAssetTypeRoundTripsThroughTheStoreFile)
{
    namespace fs = std::filesystem;

    struct TypeCase
    {
        AssetType type;
        const char* name;
    };
    static constexpr TypeCase kAllTypes[] = {
#define GE_TEST_ASSET_TYPE_CASE(entry) {AssetType::entry, #entry},
        GE_ASSET_TYPE_LIST(GE_TEST_ASSET_TYPE_CASE)
#undef GE_TEST_ASSET_TYPE_CASE
    };
    constexpr size_t kTypeCount = std::size(kAllTypes);

    const fs::path tmpRoot = TestUtils::MakeUniqueTempDirectory("ge_assetdb_type_roundtrip");
    std::error_code ec;
    fs::remove_all(tmpRoot, ec);
    fs::create_directories(tmpRoot, ec);
    const fs::path dbPath = tmpRoot / "AssetDatabase.assetdb";

    std::vector<GUID> guids;
    guids.reserve(kTypeCount);

    {
        AssetDatabase::AssetStore_TextJsonl store;
        for (size_t i = 0; i < kTypeCount; ++i)
        {
            char guidText[64] = {};
            std::snprintf(guidText, sizeof(guidText), "aaaaaaaa-0000-4000-8000-%012zu", i);

            AssetDatabase::AssetRecord r{};
            r.guid = GUID(std::string(guidText));
            ASSERT_FALSE(r.guid.IsNull()) << guidText;
            r.path = std::string("asset_") + kAllTypes[i].name + ".bin";
            r.type = kAllTypes[i].type;
            // typeId deliberately left empty: the writer then emits
            // AssetTypeToString(r.type), which is the conversion under test.

            std::string err;
            ASSERT_TRUE(store.UpsertAsset(r, &err)) << kAllTypes[i].name << ": " << err;
            guids.push_back(r.guid);
        }

        std::string err;
        ASSERT_TRUE(store.SaveToFile(dbPath, &err)) << err;
    }

    AssetDatabase::AssetStore_TextJsonl reloaded;
    std::string loadErr;
    ASSERT_TRUE(reloaded.LoadFromFile(dbPath, &loadErr)) << loadErr;
    ASSERT_EQ(reloaded.CountAssets(), kTypeCount);

    for (size_t i = 0; i < kTypeCount; ++i)
    {
        AssetDatabase::AssetRecord r{};
        ASSERT_TRUE(reloaded.TryGetAsset(guids[i], r)) << kAllTypes[i].name;
        EXPECT_EQ(r.type, kAllTypes[i].type)
            << "type '" << kAllTypes[i].name << "' did not round-trip; read back as '"
            << AssetTypeToString(r.type) << "'";
        EXPECT_EQ(r.typeId, std::string(kAllTypes[i].name));
    }

    fs::remove_all(tmpRoot, ec);
}

// AssetTypeFromString is the exact inverse of AssetTypeToString for every listed
// type, and rejects names it does not know rather than guessing.
TEST(AssetDatabase, AssetTypeStringConversionsAreInverses)
{
#define GE_TEST_ASSET_TYPE_INVERSE(entry)                                                          \
    EXPECT_EQ(AssetTypeToString(AssetType::entry), std::string(#entry));                           \
    EXPECT_EQ(AssetTypeFromString(#entry), AssetType::entry);
    GE_ASSET_TYPE_LIST(GE_TEST_ASSET_TYPE_INVERSE)
#undef GE_TEST_ASSET_TYPE_INVERSE

    EXPECT_EQ(AssetTypeFromString(""), AssetType::Unknown);
    EXPECT_EQ(AssetTypeFromString("NotAnAssetType"), AssetType::Unknown);
    // Case-sensitive by contract: the writer only ever emits the exact names.
    EXPECT_EQ(AssetTypeFromString("texture"), AssetType::Unknown);
}


