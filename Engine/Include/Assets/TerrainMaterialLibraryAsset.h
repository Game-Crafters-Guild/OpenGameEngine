#pragma once

#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine {

// How a terrain material's textures are laid onto the ground.
enum class TerrainMaterialProjection : std::uint8_t
{
    // Three world-space projections blended by slope: the right choice for a tiling material
    // (rock, grass, sand), which has no position and must not stretch on a cliff.
    Triplanar,
    // One top-down projection at the terrain's own footprint UV: 0..1 across the terrain, times
    // Tiling. The right choice for an image of THIS terrain, such as an orthophoto: it lies on the
    // heightmap it was captured with, and a cliff shows that image stretched rather than an
    // unrelated part of it.
    Planar,
};

// One authored terrain material. This is the AUTHORED half of the pair whose GPU
// half is Terrain::TerrainMaterialRecord: it carries what a GPU record cannot —
// the user's name for the material, the stable slot ID the painted splat stores,
// and texture references as GUIDs rather than the per-frame bindless indices the
// record holds. Extraction derives the record from this every frame, which is
// also where the render-origin-dependent UV phase is computed.
struct TerrainMaterialEntry
{
    // Display name. Free text; never an identity — two materials may share one.
    std::string Name;

    // Identity. Assigned at creation and never reused while the entry lives, so a
    // reorder moves rows and cannot repaint a terrain. Array position is display
    // order only.
    std::uint8_t SlotId = 0;

    // Linear tint. Multiplies a bound albedo texture, and IS the colour without one.
    float AlbedoR = 1.0f;
    float AlbedoG = 1.0f;
    float AlbedoB = 1.0f;
    // Triplanar: multiplies the terrain's global MaterialTiling (1 = the terrain's own rate).
    // Planar: repeats across the terrain's footprint (1 = one image over the whole terrain).
    float Tiling = 1.0f;
    TerrainMaterialProjection Projection = TerrainMaterialProjection::Triplanar;

    GUID AlbedoTexture;
    GUID NormalTexture; // tangent-space, triplanar-reoriented
    GUID OrmTexture;    // R = ambient occlusion, G = roughness, B = metallic

    // Each scalar multiplies its paired texture's sampled value when one is bound,
    // so every default is the multiplier IDENTITY: a new material that binds a map
    // reads that map unmodified, and only an edit the author made trims it. A
    // look-tuned default here would silently scale every bound map instead.
    //
    // Roughness and Ao are ALSO the value used when their map is unbound. NormalStrength
    // is not — it scales a sampled tangent XY, so with no normal map bound it has
    // nothing to scale and the surface takes the terrain normal whatever it says
    // (cbt_surface.glsl CBT_MaterialNormal is only reached when NormalTex is bound).
    float Roughness = 1.0f;
    float Ao = 1.0f;
    float NormalStrength = 1.0f;

    // Procedural break-up for the untextured case.
    float VariationStrength = 0.0f; // brightness jitter [0,1]; 0 => flat tint
    float VariationHue = 0.0f;      // chroma jitter [0,1]
    float VariationScale = 0.0f;    // noise frequency (1/m); 0 => variation off

    // Blends three hash-rotated copies of the texture on a hexagonal lattice to
    // break visible repetition, at roughly 3x the sample cost.
    bool HexTiling = false;
    // Tombstone: keeps rendering for already-painted texels but disappears from
    // pickers, and holds its slot ID reserved until an explicit purge.
    bool Retired = false;
    // Whether the ORM map's BLUE channel is metallic or padding. It cannot be
    // guessed: the ORM convention names B metallic, but a map authored for a
    // dielectric surface leaves it as whatever the packer wrote, and reading a
    // white pad as metallic turns terrain into a black mirror. Off means the
    // surface stays dielectric no matter what B holds.
    bool OrmHasMetallic = false;
};

// The entry a slot ID resolves to, or nullptr when no entry holds it — a terrain painted against
// a since-purged material, which the caller must shade as the fallback rather than as whatever
// sits at that index. Free-standing because the runtime reads slots out of a cached copy of the
// list rather than out of the asset, and both must resolve a slot identically.
const TerrainMaterialEntry* FindTerrainMaterialBySlotId(
    const std::vector<TerrainMaterialEntry>& entries, std::uint8_t slotId);

// The editable sibling, for authoring surfaces that address a row by its stable slot ID rather
// than by array position — a position goes stale the moment another row is removed.
TerrainMaterialEntry* FindTerrainMaterialBySlotIdMutable(std::vector<TerrainMaterialEntry>& entries,
                                                         std::uint8_t slotId);

// The lowest slot ID no entry holds, or kInvalidTerrainMaterialSlotId when all 256 are spoken
// for. Free-standing so an authoring edit can resolve it against the list it is ABOUT TO MUTATE:
// a value read when the panel was built names whatever was free then, and binds a new material to
// a slot another edit has since taken.
inline constexpr std::uint32_t kInvalidTerrainMaterialSlotId = 0x100u;
std::uint32_t NextFreeTerrainMaterialSlotId(const std::vector<TerrainMaterialEntry>& entries);

// A terrain material library (.terrainmatlib JSON): the ordered material list a
// terrain's surface shades from. Referenced by GUID from the Terrain component,
// so several terrains can share one library.
//
// Schema:
//   { "schemaVersion": 1,
//     "assetType": "TerrainMaterialLibrary",
//     "materials": [ { "name": ..., "slotId": ..., ... }, ... ] }
//
// Entries are stored in file order. Lookup is by SlotId, never by index — see
// FindBySlotId.
class TerrainMaterialLibraryAsset : public Asset
{
public:
    TerrainMaterialLibraryAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::TerrainMaterialLibrary, path)
    {
    }

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    const std::vector<TerrainMaterialEntry>& GetMaterials() const { return m_Materials; }

    // The entry a painted slot ID resolves to; see FindTerrainMaterialBySlotId.
    const TerrainMaterialEntry* FindBySlotId(std::uint8_t slotId) const
    {
        return FindTerrainMaterialBySlotId(m_Materials, slotId);
    }

    // The lowest slot ID no entry holds, so a new material never takes a retired
    // material's ID. kInvalidSlotId when all 256 are spoken for.
    static constexpr std::uint32_t kInvalidSlotId = kInvalidTerrainMaterialSlotId;
    std::uint32_t NextFreeSlotId() const { return NextFreeTerrainMaterialSlotId(m_Materials); }

    // Authoring access for the editor's library inspector. Runtime callers read
    // through the const accessors above.
    std::vector<TerrainMaterialEntry>& EditMaterials() { return m_Materials; }

    // Serialize the current in-memory list back to the asset's path.
    bool Save() const;

    // The document text for a material list, so a caller that has entries but no
    // file yet — the create-asset flow, and the migration that mints a library
    // from a terrain's per-layer fields — writes exactly what Save writes.
    static std::string MakeDocumentText(const std::vector<TerrainMaterialEntry>& materials);

    // The document text a newly created library starts from, so the create-asset
    // flow and this parser cannot disagree about the schema.
    static std::string MakeEmptyDocumentText();

private:
    bool ParseFromText(const std::string& text);

    std::vector<TerrainMaterialEntry> m_Materials;
};

} // namespace GameEngine
