// Terrain material table authoring: how a channel role finds the material it shades with, and
// what the GPU record carries once it has. The resolve is by SLOT ID, never by row position — the
// identity rule the whole library design rests on — so these tests scramble row order deliberately.

#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Assets/TerrainMaterialLibraryAsset.h"
#include "Components/Terrain/Terrain.h"
#include "Terrain/TerrainMaterialRecord.h"
#include "TerrainECS/TerrainMaterialAuthoring.h"
#include "TerrainECS/TerrainService.h" // ComputeTiledRenderExtent

#include <string>
#include <vector>

using namespace GameEngine;
using GameEngine::TerrainECS::AuthorLegacyTerrainMaterialRecord;
using GameEngine::TerrainECS::AuthorTerrainMaterialRecord;
using GameEngine::TerrainECS::FindTerrainRoleMaterial;
using GameEngine::TerrainECS::TerrainTextureResolver;
using GameEngine::TerrainECS::TerrainTextureAccess;

namespace
{

constexpr const char* kAlbedoGuidText = "1f2e3d4c-5b6a-4798-8877-665544332211";
constexpr uint32_t kAlbedoBindless = 41u;
constexpr const char* kOrmGuidText = "2a3b4c5d-6e7f-4081-9293-a4b5c6d7e8f9";
constexpr uint32_t kOrmBindless = 57u;

// Stands in for the render services' texture cache: known GUIDs resolve to known bindless indices,
// everything else is the unbound sentinel. That is all the record authoring reads of it, so the
// whole path is checkable without a device.
TerrainTextureResolver StubResolver()
{
    return [](const GUID& guid) -> uint32_t
    {
        if (guid == GUID(kAlbedoGuidText))
            return kAlbedoBindless;
        if (guid == GUID(kOrmGuidText))
            return kOrmBindless;
        return Terrain::kTerrainMaterialUnboundTexture;
    };
}

TerrainMaterialEntry MakeEntry(uint8_t slotId, const std::string& name)
{
    TerrainMaterialEntry entry{};
    entry.SlotId = slotId;
    entry.Name = name;
    return entry;
}

} // namespace

TEST(TerrainMaterialTable, DefaultRoleBindingIsIdentity)
{
    const Components::Terrain terrain{};
    for (uint32_t role = 0; role < Terrain::kTerrainLayerRoleCount; ++role)
        EXPECT_EQ(terrain.LayerRoleSlot[role], static_cast<uint8_t>(role)) << "role " << role;
}

// Row order is display order. A resolve that indexed the vector would return the wrong material
// here for every role, which is exactly the corruption slot IDs exist to prevent.
TEST(TerrainMaterialTable, RoleResolvesBySlotIdNotByRowPosition)
{
    std::vector<TerrainMaterialEntry> library;
    library.push_back(MakeEntry(3, "Snow"));
    library.push_back(MakeEntry(0, "Grass"));
    library.push_back(MakeEntry(9, "Sand"));
    library.push_back(MakeEntry(1, "Rock"));

    Components::Terrain terrain{};
    terrain.LayerRoleSlot[0] = 9; // grass role shades with Sand
    terrain.LayerRoleSlot[1] = 3; // rock role shades with Snow
    terrain.LayerRoleSlot[2] = 0;
    terrain.LayerRoleSlot[3] = 1;

    const TerrainMaterialEntry* role0 = FindTerrainRoleMaterial(terrain, &library, 0);
    ASSERT_NE(role0, nullptr);
    EXPECT_EQ(role0->Name, "Sand");

    const TerrainMaterialEntry* role1 = FindTerrainRoleMaterial(terrain, &library, 1);
    ASSERT_NE(role1, nullptr);
    EXPECT_EQ(role1->Name, "Snow");

    const TerrainMaterialEntry* role2 = FindTerrainRoleMaterial(terrain, &library, 2);
    ASSERT_NE(role2, nullptr);
    EXPECT_EQ(role2->Name, "Grass");

    const TerrainMaterialEntry* role3 = FindTerrainRoleMaterial(terrain, &library, 3);
    ASSERT_NE(role3, nullptr);
    EXPECT_EQ(role3->Name, "Rock");
}

// A role pointed at a purged slot must report NO material so the caller shades the built-in one.
// Returning "whatever sits at that index" would shade with a material the author never bound.
TEST(TerrainMaterialTable, RoleBoundToAnUnheldSlotResolvesToNothing)
{
    // Four rows, so a resolve that indexed by position would find one at role 2 rather than
    // reporting the miss — the failure this test has to be able to see.
    std::vector<TerrainMaterialEntry> library;
    library.push_back(MakeEntry(0, "Grass"));
    library.push_back(MakeEntry(1, "Rock"));
    library.push_back(MakeEntry(4, "Sand"));
    library.push_back(MakeEntry(5, "Snow"));

    Components::Terrain terrain{};
    terrain.LayerRoleSlot[2] = 77;

    EXPECT_EQ(FindTerrainRoleMaterial(terrain, &library, 2), nullptr);
    EXPECT_EQ(FindTerrainRoleMaterial(terrain, nullptr, 0), nullptr);
    EXPECT_EQ(FindTerrainRoleMaterial(terrain, &library, Terrain::kTerrainLayerRoleCount),
              nullptr);
}

// A tombstone hides a material from pickers and keeps already-painted ground shading with it, so a
// role already bound to one still resolves — and the record carries the Retired flag through.
TEST(TerrainMaterialTable, RetiredMaterialStillResolvesAndFlagsItsRecord)
{
    std::vector<TerrainMaterialEntry> library;
    TerrainMaterialEntry retired = MakeEntry(5, "Old Dirt");
    retired.Retired = true;
    library.push_back(retired);

    Components::Terrain terrain{};
    terrain.LayerRoleSlot[1] = 5;

    const TerrainMaterialEntry* entry = FindTerrainRoleMaterial(terrain, &library, 1);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->Name, "Old Dirt");

    const Terrain::TerrainMaterialRecord record = AuthorTerrainMaterialRecord(*entry, StubResolver(), TerrainTextureAccess::BindlessIndex);
    EXPECT_NE(record.Flags & Terrain::kTerrainMaterialFlagRetired, 0u);
}

// The ORM map's blue channel is metallic only when the author says so. The surface gates its
// metallic read on this flag, so the record must carry the authored intent — and must NOT set it
// without a map to read, which would point the surface at the unbound sentinel.
TEST(TerrainMaterialTable, OrmMetallicFlagNeedsBothTheIntentAndTheMap)
{
    TerrainMaterialEntry entry = MakeEntry(4, "Ore Vein");
    entry.OrmTexture = GUID(kOrmGuidText);
    entry.OrmHasMetallic = true;

    const Terrain::TerrainMaterialRecord withBoth =
        AuthorTerrainMaterialRecord(entry, StubResolver(), TerrainTextureAccess::BindlessIndex);
    EXPECT_EQ(withBoth.OrmTex, kOrmBindless);
    EXPECT_NE(withBoth.Flags & Terrain::kTerrainMaterialFlagOrmHasMetallic, 0u)
        << "authored metallic intent never reached the record; the surface shades it dielectric";

    // Intent without a map: nothing to sample, so the flag must stay clear.
    TerrainMaterialEntry intentOnly = entry;
    intentOnly.OrmTexture = GUID();
    const Terrain::TerrainMaterialRecord withoutMap =
        AuthorTerrainMaterialRecord(intentOnly, StubResolver(), TerrainTextureAccess::BindlessIndex);
    EXPECT_EQ(withoutMap.OrmTex, Terrain::kTerrainMaterialUnboundTexture);
    EXPECT_EQ(withoutMap.Flags & Terrain::kTerrainMaterialFlagOrmHasMetallic, 0u);

    // A map without the opt-in: the common case. Blue is padding on most terrain ORM maps, and
    // shading padding as metallic turns the ground into a black mirror.
    TerrainMaterialEntry mapOnly = entry;
    mapOnly.OrmHasMetallic = false;
    const Terrain::TerrainMaterialRecord dielectric =
        AuthorTerrainMaterialRecord(mapOnly, StubResolver(), TerrainTextureAccess::BindlessIndex);
    EXPECT_EQ(dielectric.OrmTex, kOrmBindless);
    EXPECT_EQ(dielectric.Flags & Terrain::kTerrainMaterialFlagOrmHasMetallic, 0u);
}

// A material the author has only just created, with a map bound and nothing else touched, must
// read that map UNMODIFIED. cbt_surface computes `mat.Roughness * orm.g` and `mat.Ao * orm.r`, so
// any scalar default other than 1.0 silently trims every bound map by that factor — an authored
// 0.4 roughness texel shading as 0.34 with nobody having asked for it. The scalars are a trim the
// author opts into, never a toll the default charges.
TEST(TerrainMaterialTable, DefaultScalarsLetABoundMapPassThroughUntrimmed)
{
    TerrainMaterialEntry entry = MakeEntry(6, "Fresh Sand");
    entry.OrmTexture = GUID(kOrmGuidText);

    const Terrain::TerrainMaterialRecord record =
        AuthorTerrainMaterialRecord(entry, StubResolver(), TerrainTextureAccess::BindlessIndex);
    ASSERT_EQ(record.OrmTex, kOrmBindless);
    EXPECT_FLOAT_EQ(record.Roughness, 1.0f)
        << "a default Roughness other than 1.0 scales every texel of a bound ORM's G channel";
    EXPECT_FLOAT_EQ(record.Ao, 1.0f)
        << "a default Ao other than 1.0 scales every texel of a bound ORM's R channel";
    EXPECT_FLOAT_EQ(record.NormalStrength, 1.0f)
        << "a default NormalStrength other than 1.0 rescales a bound normal map's tangent XY";
}

// The legacy path is the only reader of the per-layer component fields, and every one of them has
// to land: a record that dropped tiling or hex would change an unmigrated terrain's appearance.
TEST(TerrainMaterialTable, LegacyRecordCarriesEveryPerLayerField)
{
    Components::Terrain terrain{};
    terrain.LayerTiling[2] = 4.5f;
    terrain.LayerHexTiling = (1u << 2);
    terrain.LayerAlbedoTexture[2].Set(GUID(kAlbedoGuidText));

    const Terrain::TerrainMaterialRecord record =
        AuthorLegacyTerrainMaterialRecord(terrain, 2, StubResolver(), TerrainTextureAccess::BindlessIndex);

    EXPECT_FLOAT_EQ(record.Tiling, 4.5f);
    EXPECT_FLOAT_EQ(record.HexRotStrength, 1.0f);
    EXPECT_NE(record.Flags & Terrain::kTerrainMaterialFlagHexTiling, 0u);
    EXPECT_EQ(record.AlbedoTex, kAlbedoBindless);
    // Roughness and the variation dials come from the built-in material for the role — the four
    // numbers that were TerrainGPUParams::LayerRoughness before the table existed.
    EXPECT_FLOAT_EQ(record.Roughness, Terrain::kDefaultTerrainMaterials[2].Roughness);
    EXPECT_FLOAT_EQ(record.VariationStrength, Terrain::kDefaultTerrainMaterials[2].VariationStrength);
}

// Zero tiling means "use the terrain's global rate", which the record spells as 1. A record that
// carried the 0 through would multiply the global tiling to nothing and stretch one texel over the
// whole terrain.
TEST(TerrainMaterialTable, ZeroTilingResolvesToTheGlobalRateOnBothPaths)
{
    Components::Terrain terrain{};
    terrain.LayerTiling[0] = 0.0f;
    EXPECT_FLOAT_EQ(AuthorLegacyTerrainMaterialRecord(terrain, 0, StubResolver(), TerrainTextureAccess::BindlessIndex).Tiling, 1.0f);

    TerrainMaterialEntry entry = MakeEntry(0, "Grass");
    entry.Tiling = 0.0f;
    EXPECT_FLOAT_EQ(AuthorTerrainMaterialRecord(entry, StubResolver(), TerrainTextureAccess::BindlessIndex).Tiling, 1.0f);
}

// ---------------------------------------------------------------------------
// Migration: minting a library out of a terrain's per-layer fields
// ---------------------------------------------------------------------------

// The migration guarantee, stated as an equality rather than as an intention: authoring the minted
// entries produces the SAME records the legacy per-layer path produces, so a terrain that gains a
// library does not change what it looks like. Every field is compared — a mint that dropped one
// would move the appearance in exactly the way this migration promises it will not.
TEST(TerrainMaterialTable, MintedLibraryProducesTheRecordsTheLegacyPathProduced)
{
    // A terrain that has actually authored something on every channel, so the comparison is not
    // running over four copies of the defaults.
    Components::Terrain terrain{};
    terrain.LayerTiling[0] = 3.0f;
    terrain.LayerTiling[1] = 0.0f; // means "global rate"; both paths must resolve it to 1
    terrain.LayerTiling[2] = 0.25f;
    terrain.LayerTiling[3] = 12.0f;
    terrain.LayerHexTiling = (1u << 1) | (1u << 3);
    terrain.LayerAlbedoTexture[2].Set(GUID(kAlbedoGuidText));

    const std::vector<TerrainMaterialEntry> minted =
        GameEngine::TerrainECS::MintTerrainMaterialLibrary(terrain);
    ASSERT_EQ(minted.size(), static_cast<size_t>(Terrain::kTerrainLayerRoleCount));

    for (uint32_t role = 0; role < Terrain::kTerrainLayerRoleCount; ++role)
    {
        // The mint hands role r slot r, which is the component's identity default — so the
        // migrated terrain resolves these entries through an unchanged role binding.
        EXPECT_EQ(minted[role].SlotId, static_cast<uint8_t>(role)) << "role " << role;

        const Terrain::TerrainMaterialRecord legacy =
            AuthorLegacyTerrainMaterialRecord(terrain, role, StubResolver(), TerrainTextureAccess::BindlessIndex);
        const Terrain::TerrainMaterialRecord fromLibrary =
            AuthorTerrainMaterialRecord(minted[role], StubResolver(), TerrainTextureAccess::BindlessIndex);

        SCOPED_TRACE("role " + std::to_string(role));
        EXPECT_FLOAT_EQ(fromLibrary.AlbedoR, legacy.AlbedoR);
        EXPECT_FLOAT_EQ(fromLibrary.AlbedoG, legacy.AlbedoG);
        EXPECT_FLOAT_EQ(fromLibrary.AlbedoB, legacy.AlbedoB);
        EXPECT_FLOAT_EQ(fromLibrary.Tiling, legacy.Tiling);
        EXPECT_EQ(fromLibrary.AlbedoTex, legacy.AlbedoTex);
        EXPECT_EQ(fromLibrary.NormalTex, legacy.NormalTex);
        EXPECT_EQ(fromLibrary.OrmTex, legacy.OrmTex);
        EXPECT_EQ(fromLibrary.Flags, legacy.Flags);
        EXPECT_FLOAT_EQ(fromLibrary.Roughness, legacy.Roughness);
        EXPECT_FLOAT_EQ(fromLibrary.Ao, legacy.Ao);
        EXPECT_FLOAT_EQ(fromLibrary.NormalStrength, legacy.NormalStrength);
        EXPECT_FLOAT_EQ(fromLibrary.HexRotStrength, legacy.HexRotStrength);
        EXPECT_FLOAT_EQ(fromLibrary.VariationStrength, legacy.VariationStrength);
        EXPECT_FLOAT_EQ(fromLibrary.VariationHue, legacy.VariationHue);
        EXPECT_FLOAT_EQ(fromLibrary.VariationScale, legacy.VariationScale);
    }
}

// The mint sources from fields that always exist, so there is no terrain for which it can produce
// an empty library — the state a caller must never bind, because it would drop the terrain to the
// built-in materials and lose exactly the authoring the mint exists to carry over.
TEST(TerrainMaterialTable, MintIsNeverEmptyAndNamesEveryChannel)
{
    const std::vector<TerrainMaterialEntry> minted =
        GameEngine::TerrainECS::MintTerrainMaterialLibrary(Components::Terrain{});

    ASSERT_EQ(minted.size(), static_cast<size_t>(Terrain::kTerrainLayerRoleCount));
    for (uint32_t role = 0; role < Terrain::kTerrainLayerRoleCount; ++role)
    {
        EXPECT_FALSE(minted[role].Name.empty()) << "role " << role;
        EXPECT_EQ(minted[role].Name, Terrain::kTerrainLayerRoleNames[role]);
        EXPECT_FALSE(minted[role].Retired);
    }
}

// A minted library is a library like any other: the runtime resolves its entries through the same
// slot lookup, so the four records reach the table by the ordinary path and not a migration one.
TEST(TerrainMaterialTable, MintedLibraryResolvesThroughTheOrdinaryRoleLookup)
{
    Components::Terrain terrain{};
    terrain.LayerTiling[3] = 7.5f;

    const std::vector<TerrainMaterialEntry> minted =
        GameEngine::TerrainECS::MintTerrainMaterialLibrary(terrain);

    for (uint32_t role = 0; role < Terrain::kTerrainLayerRoleCount; ++role)
    {
        const TerrainMaterialEntry* entry = FindTerrainRoleMaterial(terrain, &minted, role);
        ASSERT_NE(entry, nullptr) << "role " << role;
        EXPECT_EQ(entry->SlotId, static_cast<uint8_t>(role));
    }
    EXPECT_FLOAT_EQ(FindTerrainRoleMaterial(terrain, &minted, 3)->Tiling, 7.5f);
}

// The tint IS the colour without a texture and a MULTIPLIER with one. A LIBRARY entry's tint is
// authored data, so it survives binding a texture — that is the only path by which an albedo
// edit on a textured material can reach pixels. The LEGACY per-layer path has no authored tint
// (its colour is the built-in role default), so a textured legacy layer neutralises to white and
// the texture reads unmodified.
TEST(TerrainMaterialTable, AuthoredTintSurvivesATextureAndTheLegacyDefaultDoesNot)
{
    TerrainMaterialEntry entry = MakeEntry(0, "Grass");
    entry.AlbedoR = 0.2f;
    entry.AlbedoG = 0.4f;
    entry.AlbedoB = 0.6f;

    const Terrain::TerrainMaterialRecord untextured = AuthorTerrainMaterialRecord(entry, StubResolver(), TerrainTextureAccess::BindlessIndex);
    EXPECT_FLOAT_EQ(untextured.AlbedoR, 0.2f);
    EXPECT_EQ(untextured.AlbedoTex, Terrain::kTerrainMaterialUnboundTexture);

    entry.AlbedoTexture = GUID(kAlbedoGuidText);
    const Terrain::TerrainMaterialRecord textured = AuthorTerrainMaterialRecord(entry, StubResolver(), TerrainTextureAccess::BindlessIndex);
    EXPECT_EQ(textured.AlbedoTex, kAlbedoBindless);
    EXPECT_FLOAT_EQ(textured.AlbedoR, 0.2f);
    EXPECT_FLOAT_EQ(textured.AlbedoG, 0.4f);
    EXPECT_FLOAT_EQ(textured.AlbedoB, 0.6f);

    Components::Terrain terrain{};
    terrain.LayerAlbedoTexture[0].Set(GUID(kAlbedoGuidText));
    const Terrain::TerrainMaterialRecord legacy =
        AuthorLegacyTerrainMaterialRecord(terrain, 0, StubResolver(), TerrainTextureAccess::BindlessIndex);
    EXPECT_EQ(legacy.AlbedoTex, kAlbedoBindless);
    EXPECT_FLOAT_EQ(legacy.AlbedoR, 1.0f);
    EXPECT_FLOAT_EQ(legacy.AlbedoG, 1.0f);
    EXPECT_FLOAT_EQ(legacy.AlbedoB, 1.0f);
}

// The mint writes WHITE for a textured layer's tint — the legacy path's effective colour — so a
// freshly minted library shades identically AND its albedo field starts at the neutral value an
// author then edits to tint the texture. An untextured layer keeps the built-in role colour.
TEST(TerrainMaterialTable, MintWritesWhiteTintForTexturedLayersAndTheRoleColourOtherwise)
{
    Components::Terrain terrain{};
    terrain.LayerAlbedoTexture[1].Set(GUID(kAlbedoGuidText));

    const std::vector<TerrainMaterialEntry> minted =
        GameEngine::TerrainECS::MintTerrainMaterialLibrary(terrain);
    ASSERT_EQ(minted.size(), static_cast<size_t>(Terrain::kTerrainLayerRoleCount));

    EXPECT_FLOAT_EQ(minted[1].AlbedoR, 1.0f);
    EXPECT_FLOAT_EQ(minted[1].AlbedoG, 1.0f);
    EXPECT_FLOAT_EQ(minted[1].AlbedoB, 1.0f);

    EXPECT_FLOAT_EQ(minted[0].AlbedoR, Terrain::kDefaultTerrainMaterials[0].AlbedoR);
    EXPECT_FLOAT_EQ(minted[0].AlbedoG, Terrain::kDefaultTerrainMaterials[0].AlbedoG);
    EXPECT_FLOAT_EQ(minted[0].AlbedoB, Terrain::kDefaultTerrainMaterials[0].AlbedoB);
}

// The projection reaches the GPU as one record flag: the surface takes a Planar material's taps at
// the terrain's footprint UV. A Triplanar material (every material authored before Planar existed)
// must leave the bit clear, or it stops rendering bit-identical.
TEST(TerrainMaterialTable, PlanarProjectionSetsTheRecordFlagAndTriplanarLeavesItClear)
{
    TerrainMaterialEntry photo = MakeEntry(2, "Orthophoto");
    photo.Projection = TerrainMaterialProjection::Planar;
    const Terrain::TerrainMaterialRecord planar =
        AuthorTerrainMaterialRecord(photo, StubResolver(), TerrainTextureAccess::BindlessIndex);
    EXPECT_NE(planar.Flags & Terrain::kTerrainMaterialFlagPlanar, 0u)
        << "the Planar choice never reached the record; the surface projects the photo triplanar";

    const TerrainMaterialEntry rock = MakeEntry(3, "Rock");
    const Terrain::TerrainMaterialRecord triplanar =
        AuthorTerrainMaterialRecord(rock, StubResolver(), TerrainTextureAccess::BindlessIndex);
    EXPECT_EQ(triplanar.Flags & Terrain::kTerrainMaterialFlagPlanar, 0u);

    // The flag is its own bit: it must not alias any bit the surface already tests.
    EXPECT_EQ(Terrain::kTerrainMaterialFlagPlanar &
                  (Terrain::kTerrainMaterialFlagHexTiling | Terrain::kTerrainMaterialFlagRetired |
                   Terrain::kTerrainMaterialFlagOrmHasMetallic |
                   Terrain::kTerrainMaterialFlagHasAlbedo | Terrain::kTerrainMaterialFlagHasNormal |
                   Terrain::kTerrainMaterialFlagHasOrm),
              0u);
}

// A tiled terrain's textures span its tile grid, which overhangs the authored size by up to a
// tile: 4098 m at 0.5 samples/m is three 2048 m tiles, 6144 m. The heightmap is spread over the
// authored 4098 m, so a Planar image must be too: its footprint UV is the surface's UV scaled by
// extent / size, which puts the terrain's far edge at exactly 1 instead of at two thirds.
TEST(TerrainMaterialTable, PlanarFootprintSpansTheAuthoredSizeNotTheTileGrid)
{
    TerrainECS::TiledTerrainConfig config{};
    config.TilesPerAxisX = 3;
    config.TilesPerAxisZ = 3;
    config.TileWorldSize = 2048.0f;
    config.TileConfig.HeightmapWidth = 1025;
    config.TileConfig.HeightmapHeight = 1025;
    const TerrainECS::TiledRenderExtent extent = TerrainECS::ComputeTiledRenderExtent(config);
    ASSERT_FLOAT_EQ(extent.WorldSizeX, 6144.0f);

    Terrain::TerrainMaterialRecord record{};
    TerrainECS::SetPlanarFootprintScale(record, extent.WorldSizeX, extent.WorldSizeZ, 4098.0f,
                                        4098.0f);
    const float farEdgeSurfaceUV = 4098.0f / extent.WorldSizeX;
    EXPECT_NEAR(farEdgeSurfaceUV * record.PlanarUVScaleX, 1.0f, 1e-6f)
        << "the Planar image does not end at the terrain's far edge; it slides off the heightmap";
    EXPECT_NEAR(farEdgeSurfaceUV * record.PlanarUVScaleZ, 1.0f, 1e-6f);

    // A non-square terrain on a non-square grid: 4098 x 1500 m on three tiles by one, 6144 x
    // 2048 m. The two axes overhang by different ratios, so a scale written to the wrong axis
    // misplaces the image along both.
    TerrainECS::TiledTerrainConfig wide = config;
    wide.TilesPerAxisZ = 1;
    const TerrainECS::TiledRenderExtent wideExtent = TerrainECS::ComputeTiledRenderExtent(wide);
    ASSERT_FLOAT_EQ(wideExtent.WorldSizeZ, 2048.0f);
    Terrain::TerrainMaterialRecord wideRecord{};
    TerrainECS::SetPlanarFootprintScale(wideRecord, wideExtent.WorldSizeX, wideExtent.WorldSizeZ,
                                        4098.0f, 1500.0f);
    EXPECT_NEAR((4098.0f / wideExtent.WorldSizeX) * wideRecord.PlanarUVScaleX, 1.0f, 1e-6f)
        << "the X scale does not put the terrain's east edge at the image's last column";
    EXPECT_NEAR((1500.0f / wideExtent.WorldSizeZ) * wideRecord.PlanarUVScaleZ, 1.0f, 1e-6f)
        << "the Z scale does not put the terrain's north edge at the image's first row";

    // An untiled terrain's textures span exactly its size: the scale is the identity.
    Terrain::TerrainMaterialRecord untiled{};
    TerrainECS::SetPlanarFootprintScale(untiled, 512.0f, 256.0f, 512.0f, 256.0f);
    EXPECT_FLOAT_EQ(untiled.PlanarUVScaleX, 1.0f);
    EXPECT_FLOAT_EQ(untiled.PlanarUVScaleZ, 1.0f);
}
