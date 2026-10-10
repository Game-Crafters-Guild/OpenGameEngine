#include "TerrainECS/TerrainMaterialAuthoring.h"

#include "Components/Terrain/Terrain.h"
#include "Terrain/TerrainTypes.h"
#include "Assets/TextureAsset.h" // TextureColorSpace enumerators
#include "Assets/TextureCook.h"  // TextureCookUsage enumerators

namespace GameEngine::TerrainECS
{
namespace
{

std::uint32_t ResolveTexture(const TerrainTextureResolver& resolveTexture, const GUID& guid)
{
    if (!resolveTexture || guid.IsNull())
        return Terrain::kTerrainMaterialUnboundTexture;
    return resolveTexture(guid);
}

// Legacy-path only: the per-layer component fields carry no user-authored tint, so a textured
// layer's record must not inherit the built-in role colour as a multiplier — the texture reads
// unmodified. A LIBRARY entry's tint is authored data and rides through as the multiplier the
// record contract and cbt_surface's `col * tint` define, so the library author never calls this.
// Raise the presence bit for each slot whose SOURCE GUID is set. Kept separate from the index
// resolve so both authoring paths carry the same contract.
void SetTexturePresenceFlags(Terrain::TerrainMaterialRecord& record, const GUID& albedo,
                             const GUID& normal, const GUID& orm)
{
    if (!albedo.IsNull())
        record.Flags |= Terrain::kTerrainMaterialFlagHasAlbedo;
    if (!normal.IsNull())
        record.Flags |= Terrain::kTerrainMaterialFlagHasNormal;
    if (!orm.IsNull())
        record.Flags |= Terrain::kTerrainMaterialFlagHasOrm;
}

// Whether the SURFACE will actually tap this record's albedo — the CPU mirror of
// CBT_MAT_HAS_ALBEDO. It has to match per profile, not just "a GUID was authored": on the full
// profile a GUID that failed to resolve leaves the index at the sentinel and the surface takes
// the tint path, so whitening the tint there would render a broken texture as WHITE instead of
// falling back to the layer's role colour.
bool SurfaceSamplesAlbedo(const Terrain::TerrainMaterialRecord& record, TerrainTextureAccess access)
{
    if (access == TerrainTextureAccess::NamedBinding)
        return (record.Flags & Terrain::kTerrainMaterialFlagHasAlbedo) != 0u;
    return record.AlbedoTex != Terrain::kTerrainMaterialUnboundTexture;
}

// The ORM twin of the above, for the metallic-in-B opt-in: the flag must not claim metallic
// when the surface will not reach the map it would read B from.
bool SurfaceSamplesOrm(const Terrain::TerrainMaterialRecord& record, TerrainTextureAccess access)
{
    if (access == TerrainTextureAccess::NamedBinding)
        return (record.Flags & Terrain::kTerrainMaterialFlagHasOrm) != 0u;
    return record.OrmTex != Terrain::kTerrainMaterialUnboundTexture;
}

void NeutralizeTintWhenTextured(Terrain::TerrainMaterialRecord& record, TerrainTextureAccess access)
{
    if (SurfaceSamplesAlbedo(record, access))
        record.AlbedoR = record.AlbedoG = record.AlbedoB = 1.0f;
}

} // namespace

Terrain::TerrainMaterialRecord AuthorTerrainMaterialRecord(
    const TerrainMaterialEntry& entry, const TerrainTextureResolver& resolveTexture,
    TerrainTextureAccess access)
{
    Terrain::TerrainMaterialRecord record{};
    record.AlbedoR = entry.AlbedoR;
    record.AlbedoG = entry.AlbedoG;
    record.AlbedoB = entry.AlbedoB;
    record.Tiling = entry.Tiling > 0.0f ? entry.Tiling : 1.0f;
    record.AlbedoTex = ResolveTexture(resolveTexture, entry.AlbedoTexture);
    record.NormalTex = ResolveTexture(resolveTexture, entry.NormalTexture);
    record.OrmTex = ResolveTexture(resolveTexture, entry.OrmTexture);
    // Presence comes from the GUID, not from the resolved index: a profile without descriptor
    // indexing resolves every index to the unbound sentinel and reaches the texture through a
    // named binding instead, so an index-only test reads "untextured" there for every layer.
    SetTexturePresenceFlags(record, entry.AlbedoTexture, entry.NormalTexture, entry.OrmTexture);
    if (entry.HexTiling)
        record.Flags |= Terrain::kTerrainMaterialFlagHexTiling;
    if (entry.Retired)
        record.Flags |= Terrain::kTerrainMaterialFlagRetired;
    if (entry.Projection == TerrainMaterialProjection::Planar)
        record.Flags |= Terrain::kTerrainMaterialFlagPlanar;
    // Both halves are required: the authored intent, and an ORM map to read B from. The flag
    // without a map would tell the surface to sample a texture slot that is the unbound sentinel.
    // Presence, not the index — the index is the sentinel on every compat-profile material.
    if (entry.OrmHasMetallic && SurfaceSamplesOrm(record, access))
        record.Flags |= Terrain::kTerrainMaterialFlagOrmHasMetallic;
    record.Roughness = entry.Roughness;
    record.Ao = entry.Ao;
    record.NormalStrength = entry.NormalStrength;
    record.HexRotStrength = entry.HexTiling ? 1.0f : 0.0f;
    record.VariationStrength = entry.VariationStrength;
    record.VariationHue = entry.VariationHue;
    record.VariationScale = entry.VariationScale;
    return record;
}

void SetPlanarFootprintScale(Terrain::TerrainMaterialRecord& record, float renderSizeX,
                             float renderSizeZ, float authoredSizeX, float authoredSizeZ)
{
    record.PlanarUVScaleX =
        renderSizeX > 0.0f && authoredSizeX > 0.0f ? renderSizeX / authoredSizeX : 1.0f;
    record.PlanarUVScaleZ =
        renderSizeZ > 0.0f && authoredSizeZ > 0.0f ? renderSizeZ / authoredSizeZ : 1.0f;
}

Terrain::TerrainMaterialRecord AuthorLegacyTerrainMaterialRecord(
    const Components::Terrain& terrain, std::uint32_t role,
    const TerrainTextureResolver& resolveTexture, TerrainTextureAccess access)
{
    if (role >= Terrain::kTerrainLayerRoleCount)
        return Terrain::TerrainMaterialRecord{};

    Terrain::TerrainMaterialRecord record = Terrain::kDefaultTerrainMaterials[role];
    record.Tiling = terrain.LayerTiling[role] > 0.0f ? terrain.LayerTiling[role] : 1.0f;
    const bool hexTiling = ((terrain.LayerHexTiling >> role) & 1u) != 0u;
    record.HexRotStrength = hexTiling ? 1.0f : 0.0f;
    if (hexTiling)
        record.Flags |= Terrain::kTerrainMaterialFlagHexTiling;
    const GUID albedoGuid = terrain.LayerAlbedoTexture[role].ToGuid();
    record.AlbedoTex = ResolveTexture(resolveTexture, albedoGuid);
    SetTexturePresenceFlags(record, albedoGuid, GUID{}, GUID{});
    NeutralizeTintWhenTextured(record, access);
    return record;
}

std::vector<TerrainMaterialEntry> MintTerrainMaterialLibrary(const Components::Terrain& terrain)
{
    std::vector<TerrainMaterialEntry> entries;
    entries.reserve(Terrain::kTerrainLayerRoleCount);

    for (std::uint32_t role = 0; role < Terrain::kTerrainLayerRoleCount; ++role)
    {
        const Terrain::TerrainMaterialRecord& builtIn = Terrain::kDefaultTerrainMaterials[role];

        TerrainMaterialEntry entry{};
        entry.SlotId = static_cast<std::uint8_t>(role);
        entry.Name = Terrain::kTerrainLayerRoleNames[role];
        // Roughness and the variation dials are the built-in role material — the numbers the
        // surface blended out of TerrainGPUParams before the table existed. The tint is the
        // built-in role colour for an untextured layer, and WHITE for a textured one: a library
        // tint multiplies a bound albedo, and the legacy path shaded a textured layer with the
        // texture unmodified, so white is the value that keeps the mint appearance-exact.
        const bool textured = !terrain.LayerAlbedoTexture[role].IsNull();
        entry.AlbedoR = textured ? 1.0f : builtIn.AlbedoR;
        entry.AlbedoG = textured ? 1.0f : builtIn.AlbedoG;
        entry.AlbedoB = textured ? 1.0f : builtIn.AlbedoB;
        entry.Roughness = builtIn.Roughness;
        entry.Ao = builtIn.Ao;
        entry.NormalStrength = builtIn.NormalStrength;
        entry.VariationStrength = builtIn.VariationStrength;
        entry.VariationHue = builtIn.VariationHue;
        entry.VariationScale = builtIn.VariationScale;
        // The tiling, hex opt-in and albedo are whatever the terrain authored per layer.
        entry.Tiling = terrain.LayerTiling[role] > 0.0f ? terrain.LayerTiling[role] : 1.0f;
        entry.HexTiling = ((terrain.LayerHexTiling >> role) & 1u) != 0u;
        entry.AlbedoTexture = terrain.LayerAlbedoTexture[role].ToGuid();
        entries.push_back(std::move(entry));
    }

    return entries;
}

TerrainTextureClassification ClassifyTerrainTexture(TerrainTextureKind kind)
{
    switch (kind)
    {
    case TerrainTextureKind::Normal:
        return {TextureColorSpace::Linear, TextureCookUsage::Normal};
    case TerrainTextureKind::Orm:
        return {TextureColorSpace::Linear, TextureCookUsage::Packed};
    case TerrainTextureKind::Albedo:
        break;
    }
    return {TextureColorSpace::Unknown, TextureCookUsage::Color};
}

void DeclareTerrainLibraryTextures(const std::vector<TerrainMaterialEntry>& entries,
                                   const TerrainTextureDeclarer& declare)
{
    if (!declare)
        return;
    for (const TerrainMaterialEntry& entry : entries)
    {
        if (!entry.AlbedoTexture.IsNull())
            declare(entry.AlbedoTexture, TerrainTextureKind::Albedo);
        if (!entry.NormalTexture.IsNull())
            declare(entry.NormalTexture, TerrainTextureKind::Normal);
        if (!entry.OrmTexture.IsNull())
            declare(entry.OrmTexture, TerrainTextureKind::Orm);
    }
}

const TerrainMaterialEntry* FindTerrainRoleMaterial(
    const Components::Terrain& terrain, const std::vector<TerrainMaterialEntry>* library,
    std::uint32_t role)
{
    if (!library || role >= Terrain::kTerrainLayerRoleCount)
        return nullptr;
    return FindTerrainMaterialBySlotId(*library, terrain.LayerRoleSlot[role]);
}

} // namespace GameEngine::TerrainECS
