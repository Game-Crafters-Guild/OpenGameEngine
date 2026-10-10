// The terrain material library asset: its JSON round-trip, the slot-ID identity
// rules the painted splat depends on, and the dependency edges that decide whether
// a packaged project carries the textures its terrains shade with.

#include <gtest/gtest.h>

#include "AssetCore/AssetTypes.h"
#include "AssetCore/DepEdge.h"
#include "Assets/Parsers/TerrainMaterialLibraryParser.h"
#include "Assets/ParserRegistry.h"
#include "Assets/TerrainMaterialLibraryAsset.h"

#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

const GUID kAlbedoGuid("11111111-2222-4333-8444-555555555555");
const GUID kNormalGuid("66666666-7777-4888-8999-aaaaaaaaaaaa");
const GUID kOrmGuid("77777777-8888-4999-8aaa-bbbbbbbbbbbb");
const GUID kLibraryGuid("bbbbbbbb-cccc-4ddd-8eee-ffffffffffff");

// Collects every edge a parser emits, so a test can assert on the whole set
// rather than on the first one.
class RecordingDepEdgeSink final : public DepEdgeSink
{
public:
    void Emit(DepEdge edge) override { Edges.push_back(std::move(edge)); }
    std::vector<DepEdge> Edges;
};

TerrainMaterialEntry MakeRockEntry()
{
    TerrainMaterialEntry e{};
    e.Name = "Cliff Rock";
    e.SlotId = 7;
    e.AlbedoR = 0.55f;
    e.AlbedoG = 0.50f;
    e.AlbedoB = 0.42f;
    e.Tiling = 2.5f;
    e.AlbedoTexture = kAlbedoGuid;
    e.NormalTexture = kNormalGuid;
    e.Roughness = 0.65f;
    e.Ao = 0.9f;
    e.NormalStrength = 1.5f;
    e.VariationStrength = 0.24f;
    e.VariationHue = 0.12f;
    e.VariationScale = 0.022f;
    e.HexTiling = true;
    return e;
}

std::filesystem::path MakeLibraryDir(const char* name)
{
    const std::filesystem::path dir = TestUtils::MakeUniqueTempDirectory(name);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace

// Every authored field survives Save -> Load. A field that silently drops is a
// material the user edited and the terrain never shades with.
TEST(TerrainMaterialLibrary, EveryAuthoredFieldSurvivesTheFileRoundTrip)
{
    const std::filesystem::path dir = MakeLibraryDir("ge_terrainmatlib_roundtrip");
    const std::filesystem::path path = dir / "island.terrainmatlib";

    {
        TerrainMaterialLibraryAsset written(kLibraryGuid, path);
        written.EditMaterials().push_back(MakeRockEntry());

        TerrainMaterialEntry retired{};
        retired.Name = "Old Sand";
        retired.SlotId = 3;
        retired.Retired = true;
        written.EditMaterials().push_back(retired);

        // The ORM pair. OrmHasMetallic is authored intent that cannot be recovered from the map
        // itself — a dielectric ORM leaves blue as padding, and reading padding as metallic shades
        // the ground as a black mirror — so a flag that drops in the round trip is a look change.
        TerrainMaterialEntry ore{};
        ore.Name = "Ore Vein";
        ore.SlotId = 9;
        ore.OrmTexture = kOrmGuid;
        ore.OrmHasMetallic = true;
        written.EditMaterials().push_back(ore);

        ASSERT_TRUE(written.Save());
    }

    TerrainMaterialLibraryAsset read(kLibraryGuid, path);
    ASSERT_TRUE(read.Load());
    ASSERT_EQ(read.GetMaterials().size(), 3u);

    const TerrainMaterialEntry expected = MakeRockEntry();
    const TerrainMaterialEntry& rock = read.GetMaterials()[0];
    EXPECT_EQ(rock.Name, expected.Name);
    EXPECT_EQ(rock.SlotId, expected.SlotId);
    EXPECT_FLOAT_EQ(rock.AlbedoR, expected.AlbedoR);
    EXPECT_FLOAT_EQ(rock.AlbedoG, expected.AlbedoG);
    EXPECT_FLOAT_EQ(rock.AlbedoB, expected.AlbedoB);
    EXPECT_FLOAT_EQ(rock.Tiling, expected.Tiling);
    EXPECT_EQ(rock.AlbedoTexture, expected.AlbedoTexture);
    EXPECT_EQ(rock.NormalTexture, expected.NormalTexture);
    EXPECT_TRUE(rock.OrmTexture.IsNull());
    EXPECT_FLOAT_EQ(rock.Roughness, expected.Roughness);
    EXPECT_FLOAT_EQ(rock.Ao, expected.Ao);
    EXPECT_FLOAT_EQ(rock.NormalStrength, expected.NormalStrength);
    EXPECT_FLOAT_EQ(rock.VariationStrength, expected.VariationStrength);
    EXPECT_FLOAT_EQ(rock.VariationHue, expected.VariationHue);
    EXPECT_FLOAT_EQ(rock.VariationScale, expected.VariationScale);
    EXPECT_TRUE(rock.HexTiling);
    EXPECT_FALSE(rock.Retired);
    EXPECT_FALSE(rock.OrmHasMetallic) << "a material with no ORM must not claim metallic";

    EXPECT_EQ(read.GetMaterials()[1].Name, "Old Sand");
    EXPECT_TRUE(read.GetMaterials()[1].Retired);

    const TerrainMaterialEntry& ore = read.GetMaterials()[2];
    EXPECT_EQ(ore.Name, "Ore Vein");
    EXPECT_EQ(ore.OrmTexture, kOrmGuid);
    EXPECT_TRUE(ore.OrmHasMetallic)
        << "the ORM metallic opt-in dropped in the round trip; the surface would shade the map's "
           "blue channel as dielectric padding";

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Lookup is by slot ID, never by array position: the splat stores IDs, so a
// reorder must move rows without repainting anything, and a purged ID must
// resolve to nothing rather than to whatever moved into its place.
TEST(TerrainMaterialLibrary, SlotIdIsIdentityAndPositionIsOnlyDisplayOrder)
{
    TerrainMaterialLibraryAsset lib(kLibraryGuid, "unused.terrainmatlib");
    TerrainMaterialEntry a{};
    a.Name = "Grass";
    a.SlotId = 4;
    TerrainMaterialEntry b{};
    b.Name = "Snow";
    b.SlotId = 9;
    lib.EditMaterials() = {a, b};

    ASSERT_NE(lib.FindBySlotId(4), nullptr);
    EXPECT_EQ(lib.FindBySlotId(4)->Name, "Grass");
    ASSERT_NE(lib.FindBySlotId(9), nullptr);
    EXPECT_EQ(lib.FindBySlotId(9)->Name, "Snow");

    // Position 0 is now Snow; slot 4 still means Grass.
    std::swap(lib.EditMaterials()[0], lib.EditMaterials()[1]);
    ASSERT_NE(lib.FindBySlotId(4), nullptr);
    EXPECT_EQ(lib.FindBySlotId(4)->Name, "Grass");

    // Never resolves a slot nobody holds — including the array positions.
    EXPECT_EQ(lib.FindBySlotId(0), nullptr);
    EXPECT_EQ(lib.FindBySlotId(1), nullptr);

    // A new material takes the lowest ID nobody holds, so it cannot inherit a
    // live material's painted texels.
    EXPECT_EQ(lib.NextFreeSlotId(), 0u);
    TerrainMaterialEntry zero{};
    zero.SlotId = 0;
    lib.EditMaterials().push_back(zero);
    EXPECT_EQ(lib.NextFreeSlotId(), 1u);
}

// The runtime resolves slots out of a CACHED COPY of the list, not out of the asset, so the same
// lookup has to be reachable without one. A second implementation is how a reorder ends up
// repainting a terrain on one path and not the other.
TEST(TerrainMaterialLibrary, SlotLookupResolvesOutOfABareEntryList)
{
    TerrainMaterialEntry a{};
    a.Name = "Grass";
    a.SlotId = 4;
    TerrainMaterialEntry b{};
    b.Name = "Snow";
    b.SlotId = 9;
    const std::vector<TerrainMaterialEntry> entries = {b, a}; // display order != slot order

    ASSERT_NE(FindTerrainMaterialBySlotId(entries, 4), nullptr);
    EXPECT_EQ(FindTerrainMaterialBySlotId(entries, 4)->Name, "Grass");
    ASSERT_NE(FindTerrainMaterialBySlotId(entries, 9), nullptr);
    EXPECT_EQ(FindTerrainMaterialBySlotId(entries, 9)->Name, "Snow");
    EXPECT_EQ(FindTerrainMaterialBySlotId(entries, 0), nullptr);
    EXPECT_EQ(FindTerrainMaterialBySlotId({}, 0), nullptr);
}

// A library holding 256 materials has no free ID, and says so rather than
// handing back a slot another material already owns.
TEST(TerrainMaterialLibrary, AFullLibraryReportsNoFreeSlot)
{
    TerrainMaterialLibraryAsset lib(kLibraryGuid, "unused.terrainmatlib");
    for (std::uint32_t i = 0; i < 256u; ++i)
    {
        TerrainMaterialEntry e{};
        e.SlotId = static_cast<std::uint8_t>(i);
        lib.EditMaterials().push_back(e);
    }
    EXPECT_EQ(lib.NextFreeSlotId(), TerrainMaterialLibraryAsset::kInvalidSlotId);
}

// The parser claims the extension and the type, so ParserRegistry dispatches
// .terrainmatlib here instead of to the binary fallback.
TEST(TerrainMaterialLibrary, ParserClaimsTheExtensionAndType)
{
    TerrainMaterialLibraryParser parser;
    EXPECT_EQ(parser.GetAssetType(), AssetType::TerrainMaterialLibrary);
    ASSERT_EQ(parser.GetSupportedExtensions().size(), 1u);
    EXPECT_EQ(parser.GetSupportedExtensions()[0], ".terrainmatlib");
    EXPECT_EQ(GetAssetTypeFromExtension(".terrainmatlib"), AssetType::TerrainMaterialLibrary);
}

// Registration into the default parser set is a separate fact from the parser
// existing: without it a .terrainmatlib file falls through to the binary
// fallback parser, loads as opaque bytes, and every material reads as absent.
TEST(TerrainMaterialLibrary, TheDefaultParserSetDispatchesTheExtension)
{
    ParserRegistry registry;
    ASSERT_TRUE(registry.Initialize());

    std::shared_ptr<AssetParser> parser = registry.FindParser("island.terrainmatlib");
    ASSERT_NE(parser, nullptr) << ".terrainmatlib reached no parser at all";
    EXPECT_EQ(parser->GetName(), "TerrainMaterialLibraryParser");
    EXPECT_EQ(parser->GetAssetType(), AssetType::TerrainMaterialLibrary);
}

// Every texture a library references is a dependency edge. Without them a
// packaged project drops the maps and every terrain using it loads back to flat
// tints — the failure mode that looks like a lighting bug.
TEST(TerrainMaterialLibrary, EveryReferencedTextureBecomesADependencyEdge)
{
    const std::filesystem::path dir = MakeLibraryDir("ge_terrainmatlib_deps");
    const std::filesystem::path path = dir / "island.terrainmatlib";

    {
        TerrainMaterialLibraryAsset written(kLibraryGuid, path);
        written.EditMaterials().push_back(MakeRockEntry()); // albedo + normal, no ORM
        TerrainMaterialEntry plain{};
        plain.Name = "Plain";
        plain.SlotId = 1;
        written.EditMaterials().push_back(plain); // no textures at all
        ASSERT_TRUE(written.Save());
    }

    AssetMetadata metadata{};
    metadata.Guid = kLibraryGuid;
    metadata.Path = path;
    metadata.Type = AssetType::TerrainMaterialLibrary;

    RecordingDepEdgeSink sink;
    TerrainMaterialLibraryParser parser;
    ASSERT_TRUE(parser.ExtractDependencies(kLibraryGuid, metadata, sink));

    ASSERT_EQ(sink.Edges.size(), 2u) << "expected one edge per bound texture";
    EXPECT_EQ(sink.Edges[0].Target, kAlbedoGuid);
    EXPECT_EQ(sink.Edges[0].FieldLocator, "materials[slot 7].albedoTexture");
    EXPECT_EQ(sink.Edges[1].Target, kNormalGuid);
    EXPECT_EQ(sink.Edges[1].FieldLocator, "materials[slot 7].normalTexture");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// A file that is not JSON fails rather than loading as an empty library: an
// empty library and a corrupt one look identical to every consumer, and one of
// them should not silently repaint a terrain with fallback tints.
TEST(TerrainMaterialLibrary, MalformedJsonFailsToLoad)
{
    const std::filesystem::path dir = MakeLibraryDir("ge_terrainmatlib_malformed");
    const std::filesystem::path path = dir / "broken.terrainmatlib";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "{ this is not json";
    }

    TerrainMaterialLibraryAsset lib(kLibraryGuid, path);
    EXPECT_FALSE(lib.Load());
    EXPECT_EQ(lib.GetState(), AssetState::Failed);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

namespace
{

// Loads one hand-written document body and reports whether the library accepted
// it. Hot-reload calls Load() on the main thread with no enclosing try, so a
// throw out of here is an editor crash, not a failed load.
bool LoadDocumentBody(const char* dirName, const std::string& body)
{
    const std::filesystem::path dir = MakeLibraryDir(dirName);
    const std::filesystem::path path = dir / "typed.terrainmatlib";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << body;
    }

    TerrainMaterialLibraryAsset lib(kLibraryGuid, path);
    const bool loaded = lib.Load();

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return loaded;
}

// A document whose materials array holds one entry with the given field text.
std::string DocumentWithEntryFields(const std::string& fields)
{
    return "{\"schemaVersion\":1,\"assetType\":\"TerrainMaterialLibrary\","
           "\"materials\":[{" + fields + "}]}";
}

} // namespace

// Valid JSON carrying a wrong-typed field is the hot-reload crash surface: the
// text parses, so allow_exceptions=false never fires, and every field read that
// coerces on type throws out of Load() -> Asset::Reload -> CheckForReloads ->
// AssetManager::Update, none of which catch. Each case must FAIL THE LOAD rather
// than throw, so they stand as separate tests: one throw would otherwise abort
// the rest of the set and hide which reads are still unguarded.

// name: number where a string belongs.
TEST(TerrainMaterialLibrary, WrongTypedNameFailsTheLoad)
{
    EXPECT_FALSE(LoadDocumentBody("ge_terrainmatlib_type_name",
                                  DocumentWithEntryFields("\"name\":42,\"slotId\":1")));
}

// slotId: string where a number belongs.
TEST(TerrainMaterialLibrary, WrongTypedSlotIdFailsTheLoad)
{
    EXPECT_FALSE(LoadDocumentBody("ge_terrainmatlib_type_slot",
                                  DocumentWithEntryFields("\"name\":\"Rock\",\"slotId\":\"abc\"")));
}

// albedo: a 3-array, but of strings.
TEST(TerrainMaterialLibrary, WrongTypedAlbedoFailsTheLoad)
{
    EXPECT_FALSE(LoadDocumentBody(
        "ge_terrainmatlib_type_albedo",
        DocumentWithEntryFields("\"slotId\":1,\"albedo\":[\"a\",\"b\",\"c\"]")));
}

// roughness: string where a number belongs.
TEST(TerrainMaterialLibrary, WrongTypedRoughnessFailsTheLoad)
{
    EXPECT_FALSE(LoadDocumentBody(
        "ge_terrainmatlib_type_roughness",
        DocumentWithEntryFields("\"slotId\":1,\"roughness\":\"high\"")));
}

// A slot ID is the identity the painted splat stores. Narrowing an out-of-domain value would
// silently rename the material it refers to — 256 lands on slot 0, -1 on slot 255 — and repaint
// whatever already held that slot.
TEST(TerrainMaterialLibrary, OutOfDomainSlotIdFailsTheLoadInsteadOfWrapping)
{
    EXPECT_FALSE(LoadDocumentBody("ge_terrainmatlib_slot_256",
                                  DocumentWithEntryFields("\"name\":\"Rock\",\"slotId\":256")));
    EXPECT_FALSE(LoadDocumentBody("ge_terrainmatlib_slot_neg",
                                  DocumentWithEntryFields("\"name\":\"Rock\",\"slotId\":-1")));
    // The domain's own endpoints still load.
    EXPECT_TRUE(LoadDocumentBody("ge_terrainmatlib_slot_0",
                                 DocumentWithEntryFields("\"name\":\"Rock\",\"slotId\":0")));
    EXPECT_TRUE(LoadDocumentBody("ge_terrainmatlib_slot_255",
                                 DocumentWithEntryFields("\"name\":\"Rock\",\"slotId\":255")));
}

// Without a header check, any JSON object carrying a `materials` array loads as a terrain
// material library — including a future schema whose entries mean something else, which would
// present as materials that quietly read wrong rather than as a file this build cannot open.
TEST(TerrainMaterialLibrary, AForeignOrFutureDocumentHeaderFailsTheLoad)
{
    EXPECT_FALSE(LoadDocumentBody(
        "ge_terrainmatlib_schema_future",
        "{\"schemaVersion\":2,\"assetType\":\"TerrainMaterialLibrary\",\"materials\":[]}"));
    EXPECT_FALSE(LoadDocumentBody(
        "ge_terrainmatlib_schema_type",
        "{\"schemaVersion\":1,\"assetType\":\"SomethingElse\",\"materials\":[]}"));
    EXPECT_FALSE(LoadDocumentBody(
        "ge_terrainmatlib_schema_typed",
        "{\"schemaVersion\":\"one\",\"assetType\":\"TerrainMaterialLibrary\",\"materials\":[]}"));
    // The document this build writes is the one it accepts.
    EXPECT_TRUE(LoadDocumentBody(
        "ge_terrainmatlib_schema_ok",
        "{\"schemaVersion\":1,\"assetType\":\"TerrainMaterialLibrary\",\"materials\":[]}"));
}

// A well-formed document still loads: the guards must reject wrong types, not
// every document that reaches them.
TEST(TerrainMaterialLibrary, CorrectlyTypedFieldsStillLoad)
{
    EXPECT_TRUE(LoadDocumentBody(
        "ge_terrainmatlib_type_ok",
        DocumentWithEntryFields("\"name\":\"Rock\",\"slotId\":1,\"albedo\":[0.5,0.25,0.125],"
                                "\"roughness\":0.4,\"hexTiling\":true")));
}

// The same wrong-typed value reached through the dependency extractor, which
// registry scans run over files the loader has not vetted. It must keep
// scanning rather than throw out of the scan thread.
TEST(TerrainMaterialLibrary, WrongTypedSlotIdDoesNotThrowOutOfTheDependencyExtractor)
{
    const std::filesystem::path dir = MakeLibraryDir("ge_terrainmatlib_deps_type");
    const std::filesystem::path path = dir / "broken.terrainmatlib";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "{\"schemaVersion\":1,\"assetType\":\"TerrainMaterialLibrary\",\"materials\":["
               "{\"slotId\":\"abc\",\"albedoTexture\":\"11111111-2222-4333-8444-555555555555\"},"
               "{\"slotId\":9,\"normalTexture\":\"66666666-7777-4888-8999-aaaaaaaaaaaa\"}]}";
    }

    AssetMetadata metadata{};
    metadata.Guid = kLibraryGuid;
    metadata.Path = path;
    metadata.Type = AssetType::TerrainMaterialLibrary;

    RecordingDepEdgeSink sink;
    TerrainMaterialLibraryParser parser;
    ASSERT_TRUE(parser.ExtractDependencies(kLibraryGuid, metadata, sink));

    // The unnameable entry drops out; the well-formed one still ships its texture.
    ASSERT_EQ(sink.Edges.size(), 1u);
    EXPECT_EQ(sink.Edges[0].Target, kNormalGuid);
    EXPECT_EQ(sink.Edges[0].FieldLocator, "materials[slot 9].normalTexture");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// The scalar defaults are the multiplier identity, so that a newly authored material reads a bound
// map unmodified. That flip is only safe because a SAVED entry always carries its scalars
// explicitly: an omit-when-default writer would let every stored library re-take whatever the
// default happens to be at load time, silently repainting terrains authored under the old one.
// Both halves are pinned here — the explicit write, and the load that honours it.
TEST(TerrainMaterialLibrary, AStoredLibraryWritesItsScalarsExplicitlyAndIgnoresLaterDefaultChanges)
{
    const std::filesystem::path dir = MakeLibraryDir("ge_terrainmatlib_scalar_migration");
    const std::filesystem::path path = dir / "authored.terrainmatlib";

    // An entry whose scalars are left exactly as authoring created them.
    {
        TerrainMaterialLibraryAsset written(kLibraryGuid, path);
        TerrainMaterialEntry fresh{};
        fresh.Name = "Fresh";
        fresh.SlotId = 1;
        written.EditMaterials().push_back(fresh);
        ASSERT_TRUE(written.Save());
    }

    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_NE(text.find("\"roughness\""), std::string::npos)
        << "roughness was omitted from the saved entry; a later default change would repaint it";
    EXPECT_NE(text.find("\"ao\""), std::string::npos) << "ao was omitted from the saved entry";
    EXPECT_NE(text.find("\"normalStrength\""), std::string::npos)
        << "normalStrength was omitted from the saved entry";

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// A library authored before the scalar defaults became the multiplier identity stored 0.85 as an
// explicit value. It is authored data, so it must survive the change untouched — the flip governs
// what a NEW material starts at, never what a stored one reloads as.
TEST(TerrainMaterialLibrary, AnExplicitlyStoredRoughnessOutranksTheStructDefault)
{
    const std::filesystem::path dir = MakeLibraryDir("ge_terrainmatlib_legacy_roughness");
    const std::filesystem::path path = dir / "legacy.terrainmatlib";
    {
        // Two entries on purpose. 0.85 is the REAL migration case (it is what the old default
        // wrote), but on its own it cannot discriminate: revert the default and a broken read
        // still yields 0.85 by coincidence. 0.37 is a value no default has ever supplied, so only
        // an honest read of the file can produce it.
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "{\"schemaVersion\":1,\"assetType\":\"TerrainMaterialLibrary\",\"materials\":["
               "{\"name\":\"Legacy Snow\",\"slotId\":3,\"roughness\":0.85,\"ao\":0.9},"
               "{\"name\":\"Odd Value\",\"slotId\":4,\"roughness\":0.37}]}";
    }

    TerrainMaterialLibraryAsset lib(kLibraryGuid, path);
    ASSERT_TRUE(lib.Load());
    ASSERT_EQ(lib.GetMaterials().size(), 2u);
    EXPECT_FLOAT_EQ(lib.GetMaterials()[0].Roughness, 0.85f)
        << "a stored roughness was overwritten by the struct default; every library authored "
           "before the default changed would shift look on load";
    EXPECT_FLOAT_EQ(lib.GetMaterials()[0].Ao, 0.9f);
    EXPECT_FLOAT_EQ(lib.GetMaterials()[1].Roughness, 0.37f)
        << "the load path is not reading roughness out of the document at all";

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// The document a newly created library starts from parses back as an empty
// library, so the create-asset flow and the parser cannot disagree on schema.
TEST(TerrainMaterialLibrary, TheEmptyDocumentTemplateParsesAsAnEmptyLibrary)
{
    const std::filesystem::path dir = MakeLibraryDir("ge_terrainmatlib_template");
    const std::filesystem::path path = dir / "new.terrainmatlib";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << TerrainMaterialLibraryAsset::MakeEmptyDocumentText();
    }

    TerrainMaterialLibraryAsset lib(kLibraryGuid, path);
    ASSERT_TRUE(lib.Load());
    EXPECT_TRUE(lib.GetMaterials().empty());
    EXPECT_EQ(lib.NextFreeSlotId(), 0u);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// A Planar material samples its albedo at the terrain's own footprint UV, which is what lets one
// image (an orthophoto) lie on the heightmap it was captured with. The choice is authored intent the
// texture cannot reveal, so a library that drops it silently re-projects the photo triplanar.
TEST(TerrainMaterialLibrary, PlanarProjectionSurvivesTheRoundTripAndTriplanarStaysOffTheFile)
{
    const std::filesystem::path dir = MakeLibraryDir("ge_terrainmatlib_projection");
    const std::filesystem::path path = dir / "basemap.terrainmatlib";
    {
        TerrainMaterialLibraryAsset written(kLibraryGuid, path);
        TerrainMaterialEntry photo{};
        photo.Name = "Orthophoto";
        photo.SlotId = 0;
        photo.AlbedoTexture = kAlbedoGuid;
        photo.Projection = TerrainMaterialProjection::Planar;
        written.EditMaterials().push_back(photo);
        written.EditMaterials().push_back(MakeRockEntry());
        ASSERT_TRUE(written.Save());
    }

    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(text.find("\"projection\""), text.rfind("\"projection\""))
        << "the Triplanar entry wrote a projection key; every library saved before Planar existed "
           "would rewrite on its next save";

    TerrainMaterialLibraryAsset read(kLibraryGuid, path);
    ASSERT_TRUE(read.Load());
    ASSERT_EQ(read.GetMaterials().size(), 2u);
    EXPECT_EQ(read.GetMaterials()[0].Projection, TerrainMaterialProjection::Planar)
        << "the Planar choice dropped in the round trip; the photo would project triplanar again";
    EXPECT_EQ(read.GetMaterials()[1].Projection, TerrainMaterialProjection::Triplanar);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// A projection this build does not know is refused rather than read as Triplanar: guessing would
// render an orthophoto through the side projections without a word.
TEST(TerrainMaterialLibrary, AnUnknownOrWrongTypedProjectionFailsTheLoad)
{
    EXPECT_FALSE(LoadDocumentBody(
        "ge_terrainmatlib_projection_unknown",
        DocumentWithEntryFields("\"slotId\":1,\"projection\":\"cylindrical\"")));
    EXPECT_FALSE(LoadDocumentBody("ge_terrainmatlib_projection_typed",
                                  DocumentWithEntryFields("\"slotId\":1,\"projection\":1")));
    EXPECT_TRUE(LoadDocumentBody("ge_terrainmatlib_projection_planar",
                                 DocumentWithEntryFields("\"slotId\":1,\"projection\":\"planar\"")));
    EXPECT_TRUE(LoadDocumentBody(
        "ge_terrainmatlib_projection_triplanar",
        DocumentWithEntryFields("\"slotId\":1,\"projection\":\"triplanar\"")));
}
