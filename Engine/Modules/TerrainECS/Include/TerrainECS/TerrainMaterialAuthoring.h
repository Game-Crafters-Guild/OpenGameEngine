#pragma once

#include "Terrain/TerrainMaterialRecord.h"
// TerrainMaterialEntry by value: the authored entry is this header's input. Engine's include
// directories are already a precondition of this module's public headers (IRenderFeature.h).
#include "Assets/TerrainMaterialLibraryAsset.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace GameEngine
{
namespace Components { struct Terrain; }

// Opaque enum declarations, not includes: Assets/TextureAsset.h drags in GameEngine::TextureFormat,
// which collides with GameEngine::Rendering::TextureFormat in every TU this public header reaches.
// An opaque enum-declaration is a complete type, so it can be a struct member and a return value —
// only the enumerator names need the real headers, which the .cpp and its tests include.
enum class TextureColorSpace : std::uint8_t;
enum class TextureCookUsage : std::uint8_t;

namespace TerrainECS
{

// Resolves an authored texture GUID to the bindless descriptor index a record holds, or 0 (the
// reserved sentinel) when the GUID is null or its texture is not resident. Extraction binds this to
// the render services' texture cache; a test binds a stub, which is what makes record authoring
// checkable without a device.
using TerrainTextureResolver = std::function<std::uint32_t(const GUID&)>;

// What one of a terrain material's texture references IS. Nothing downstream can infer this: a
// terrain resolves textures by GUID and never builds a Material, so the material slot-name ladder
// that classifies every other texture in the engine (color space, cook usage) never sees these.
// Left unclassified, a normal map uploads sRGB and its decoded vectors are gamma-corrupted.
enum class TerrainTextureKind : std::uint8_t
{
    Albedo, // colour; keeps its extension-guessed color space, block-compressed as colour
    Normal, // tangent-space vectors; linear, two-channel (Z reconstructed in the shader)
    Orm,    // packed data channels (R = AO, G = roughness, B = metallic); linear
};

// Reports one texture reference's kind to whatever owns texture classification. Extraction binds
// this to the render services' texture service; a test binds a recorder, which is what makes the
// declaration checkable without a device.
using TerrainTextureDeclarer = std::function<void(const GUID&, TerrainTextureKind)>;

// What the texture system has to be told about one kind, in its own vocabulary. No default member
// initializers: the enums are opaquely declared above, so their enumerator names are not in scope
// here — and ClassifyTerrainTexture answers for every kind anyway.
struct TerrainTextureClassification
{
    TextureColorSpace ColorSpace;
    TextureCookUsage Usage;
};

// The classification each kind carries.
//
// Albedo declares NO color space (Unknown): the extension guess is already right for it — sRGB for
// an LDR source, linear for an HDR one — and pinning sRGB would mis-tag the latter. Normal and ORM
// must be declared linear, because for them the guess is actively WRONG: gamma-decoding a tangent
// vector or a roughness value produces garbage data, not a subtly different colour.
TerrainTextureClassification ClassifyTerrainTexture(TerrainTextureKind kind);

// Declare the kind of every texture an authored library's entries reference.
//
// Called once per library parse, BEFORE any record resolves those GUIDs to bindless indices: the
// classification has to reach the AssetDatabase ahead of the upload that reads it. Doing it per
// parse rather than per frame also keeps the metadata lookups out of extraction's hot path.
// Null references are skipped; a declarer that is empty makes this a no-op.
void DeclareTerrainLibraryTextures(const std::vector<TerrainMaterialEntry>& entries,
                                   const TerrainTextureDeclarer& declare);

// How the terrain surface reaches a record's textures, which decides what "this record is
// textured" means on the CPU (the mirror of CBT_MAT_HAS_ALBEDO): through the bindless index the
// record holds, or through the named bindings the compat profile fills from the presence flags.
enum class TerrainTextureAccess : std::uint8_t
{
    BindlessIndex,
    NamedBinding,
};

// The GPU record for one authored library entry. The fields it does not carry are the UV phase,
// which the caller derives because it depends on the render origin rather than on the material,
// and the Planar footprint scale (SetPlanarFootprintScale), which depends on the terrain.
Terrain::TerrainMaterialRecord AuthorTerrainMaterialRecord(
    const TerrainMaterialEntry& entry, const TerrainTextureResolver& resolveTexture,
    TerrainTextureAccess access);

// Scales a record's Planar footprint UV from the extent the terrain's textures cover (renderSize,
// the tile grid's extent for a tiled terrain) to the authored size, where the heightmap is spread.
// Without it a Planar image on a tiled terrain whose tile grid overhangs its size would cover the
// overhang too, and slide off the heightmap by up to a tile. A non-positive size leaves the scale
// at 1.
void SetPlanarFootprintScale(Terrain::TerrainMaterialRecord& record, float renderSizeX,
                             float renderSizeZ, float authoredSizeX, float authoredSizeZ);

// The GPU record for a terrain with no library: the built-in role material plus whatever the
// per-layer component fields override. This is what a scene authored before libraries existed
// shades with, and it is the only path those fields are read on.
Terrain::TerrainMaterialRecord AuthorLegacyTerrainMaterialRecord(
    const Components::Terrain& terrain, std::uint32_t role,
    const TerrainTextureResolver& resolveTexture, TerrainTextureAccess access);

// The entry channel role `role` shades with: the one holding SLOT ID
// terrain.LayerRoleSlot[role]. By slot, never by array position, so reordering the library's
// rows changes display order and cannot repaint a terrain. nullptr for a terrain with no library,
// an out-of-range role, or a slot no entry holds — each of which shades from the built-in material
// for that role. A RETIRED entry still resolves: a tombstone hides a material from pickers and
// keeps already-painted ground shading with it.
const TerrainMaterialEntry* FindTerrainRoleMaterial(
    const Components::Terrain& terrain, const std::vector<TerrainMaterialEntry>* library,
    std::uint32_t role);

// The four materials an unmigrated terrain's per-layer fields already describe, one per channel
// role and holding slot IDs 0-3 so the identity role binding resolves them unchanged.
//
// Every field is copied from the legacy source rather than defaulted, so authoring these entries
// through AuthorTerrainMaterialRecord reproduces AuthorLegacyTerrainMaterialRecord's records
// exactly — that equality is what lets a terrain be migrated onto a library without its appearance
// moving, and TerrainMaterialTableTests pins it field by field.
//
// Always returns kTerrainLayerRoleCount entries: the per-layer fields always exist, so there
// is no input for which this can produce an empty library. A caller must never bind an empty one —
// that would drop the terrain to the built-in materials and lose the per-layer authoring.
std::vector<TerrainMaterialEntry> MintTerrainMaterialLibrary(const Components::Terrain& terrain);

} // namespace TerrainECS
} // namespace GameEngine
