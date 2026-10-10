#pragma once

#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"
#include "Assets/FbxLoaderOptions.h"
#include "Assets/ModelExtras.h"
#include "Types/Types.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>

namespace GameEngine {

class AssetManager;
struct ModelAssetFbxTestAccess;
namespace Animation { struct SkeletonData; class AnimationClip; }

/**
 * @brief Model format types
 */
enum class ModelFormat {
    Unknown,
    OBJ,
    FBX,
    GLTF,
    GLB,
    BLEND
};

enum class MeshPrimitiveTopology : uint32 {
    Triangles = 0,
    Lines = 1,
    Points = 2,
};

/**
 * @brief Vertex data structure.
 *
 * Tangent is stored as vec4 per the glTF convention: xyz is the tangent
 * direction, w is the bitangent handedness (+1 or -1). The bitangent is
 * reconstructed in-shader as `cross(N, T) * Tangent.w` — there is no
 * stored bitangent. This matches Unity / Unreal / Godot and saves
 * 12 B/vertex of CPU + GPU storage.
 */
struct Vertex {
    float Position[3] = {0.0f, 0.0f, 0.0f};        // x, y, z
    float Normal[3] = {0.0f, 0.0f, 0.0f};          // nx, ny, nz
    float TexCoords[2] = {0.0f, 0.0f};             // u, v
    float Tangent[4] = {0.0f, 0.0f, 0.0f, 1.0f};   // tx, ty, tz, handedness
};

/**
 * @brief One morph target in sparse form: the vertices it moves and their deltas.
 *
 * VertexIndices lists, ascending and once each, the vertices whose position, normal or tangent delta
 * is non-zero. Each delta array holds x, y, z in engine axes for every listed vertex, in the same
 * order. NormalDeltas and TangentDeltas are empty when the source authors no such deltas. A target
 * that moves nothing lists no vertex.
 */
struct MorphTarget {
    String Name;
    Vector<uint32> VertexIndices;   // ascending, unique: the vertices this target moves
    Vector<float> PositionDeltas;   // 3 per listed vertex
    Vector<float> NormalDeltas;     // 3 per listed vertex, or empty
    Vector<float> TangentDeltas;    // 3 per listed vertex, or empty
};

/**
 * @brief A further source node that draws a mesh's geometry: glTF and FBX place one mesh under
 * several nodes (a chess set's pawns). The fields mean what the Mesh's own SourceNode fields mean.
 */
struct MeshPlacement {
    int32 SourceNodeIndex = -1;
    float SourceNodeTransform[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    float SourceNodeLocalTransform[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
};

/**
 * @brief Mesh data structure
 */
struct Mesh {
    String Name;
    Vector<Vertex> Vertices;
    Vector<uint32> Indices;
    // Optional simplified LOD index buffers over THIS mesh's vertex buffer.
    // Indices is LOD0; ExtraLODs[k] is LOD(k+1). meshopt_simplify reuses
    // existing vertex indices, so every LOD shares Vertices with no attribute
    // remapping. Empty when LODs were not generated.
    Vector<Vector<uint32>> ExtraLODs;
    // Achieved raw meshopt relative error per simplified level, parallel to
    // ExtraLODs (same size). ExtraLODErrors[j] is the error of ExtraLODs[j] =
    // LOD(j+1); LOD0's error (0) is implicit and not stored. Portable and
    // config-independent (relative to mesh extent, not the coverage threshold).
    // Consumed by the error->coverage threshold mapping in the GPU mesh row.
    Vector<float> ExtraLODErrors;
    // 1 when the matching ExtraLODs level fell back to meshopt_simplifySloppy.
    // Parallel to ExtraLODs. Sloppy errors are on a different scale than
    // quality-simplified errors, so these levels are excluded from the error
    // mapping and fall back to the default coverage thresholds.
    Vector<uint8> ExtraLODSloppy;
    // Per-LOD vertex blocks, parallel to ExtraLODs (ExtraLODVertices[k] backs
    // ExtraLODs[k] = LOD(k+1)). Empty[k] => LOD(k+1) is an index-only level that
    // SHARES this mesh's Vertices, and ExtraLODs[k] indexes Vertices directly
    // (Phase A/B behavior, unchanged). Non-empty[k] => LOD(k+1) carries its OWN
    // vertices, and ExtraLODs[k] is LOD-LOCAL (0-based into ExtraLODVertices[k]).
    // The GPU upload concatenates own-vertex blocks after LOD0's block and
    // records a per-level relative vertex offset so the draw resolves the
    // correct block. Two producers: artist-authored chains (AuthoredLodImport,
    // AuthoredLODs == true) and generated attribute-honest sloppy far shells
    // (MeshLODGenerator, AuthoredLODs == false) — provenance lives in the flag,
    // never inferred from these blocks.
    Vector<Vector<Vertex>> ExtraLODVertices;
    // Optional RGBA blocks parallel to ExtraLODs for authored own-vertex levels.
    // Coloured chains require exactly four finite values per vertex at every
    // own-vertex level. An index-only level has an empty block and shares Color0.
    // Entirely colourless chains keep this array empty, like legacy assets.
    Vector<Vector<float>> ExtraLODColor0;
    // True when the LOD chain is artist-authored (assembled by AuthoredLodImport
    // from _LOD suffix families, MSFT_lod, FBX LOD groups, or explicit slots).
    // Drives the authored-wins generation rule, the .gelod cook placeholder, and
    // the no-metric default threshold table. Distinct from "has own-vertex
    // blocks": generated sloppy shells also own vertices but are re-cookable.
    bool AuthoredLODs = false;
    // Authored switch coverages, parallel to ExtraLODs. When non-empty, feeds
    // lodThreshold directly (Phase C2's direct-coverage path) instead of the
    // meshopt-error-derived thresholds; empty falls back to the default table.
    Vector<float> ExtraLODCoverage;
    Vector<float> Color0;                 // optional flattened RGBA, 4 per vertex
    Vector<float> TexCoords1;             // optional flattened UV1, 2 per vertex
    Vector<Vector<float>> ExtraTexCoords; // optional UV2..UV7, each flattened vec2
    Vector<MorphTarget> MorphTargets;
    Vector<float> MorphTargetDefaultWeights;
    uint32 MaterialIndex = 0;
    MeshPrimitiveTopology PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    int32 SourceNodeIndex = -1;
    int32 SourceNodeParentIndex = -1;
    float SourceNodeTransform[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    float SourceNodeLocalTransform[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    // The nodes past the first (SourceNode* above) that draw this mesh; each instantiates as its
    // own entity sharing the geometry. A skinned mesh has none: its bones place it.
    Vector<MeshPlacement> ExtraPlacements;

    // Optional skinning data (present only when this is a skinned mesh)
    bool Skinned = false;                  // true if JOINTS_0/WEIGHTS_0 are populated
    Vector<uint16> Joints0;                // flattened, 4 per-vertex, size == Vertices.size()*4
    Vector<float>  Weights0;               // flattened, 4 per-vertex, size == Vertices.size()*4
    Vector<uint16> Joints1;                // optional second 4 influences per vertex
    Vector<float>  Weights1;               // optional second 4 influences per vertex

    // LOD0 reference box. ResolveMeshLODGeometry derives the culling envelope
    // without changing the reference used by authored switch distances.
    float MinBounds[3]{};
    float MaxBounds[3]{};

    bool IsSkinned() const { return Skinned && Joints0.size() == Vertices.size()*4 && Weights0.size() == Vertices.size()*4; }
    bool HasSkinning1() const { return IsSkinned() && Joints1.size() == Vertices.size()*4 && Weights1.size() == Vertices.size()*4; }
    bool HasColor0() const { return Color0.size() == Vertices.size()*4; }
    bool HasTexCoords1() const { return TexCoords1.size() == Vertices.size()*2; }
    bool HasMorphTargets() const { return !MorphTargets.empty(); }
    // Artist-authored chain: gates the authored-wins generation rule, the
    // .gelod cook placeholder, and the default threshold table. Explicit
    // provenance — never inferred from vertex blocks (generated sloppy shells
    // own vertices too, see HasOwnVertexLODs).
    bool HasAuthoredLODs() const { return AuthoredLODs; }
    // True if any LOD level carries its own vertex block (authored OR generated
    // shell). Gates the GPU upload's concatenation/relative-offset path and the
    // static-merge exclusion — pure mechanics, no provenance.
    bool HasOwnVertexLODs() const {
        for (const auto& block : ExtraLODVertices)
            if (!block.empty()) return true;
        return false;
    }

    // Validate raw streams, not HasColor0(): a malformed nonempty base stream
    // must not be mistaken for a colourless chain by import/upload consumers.
    bool HasValidLODColor0() const {
        const auto finite = [](const Vector<float>& values) {
            return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
        };
        if (!Color0.empty() && (Color0.size() != Vertices.size() * 4u || !finite(Color0)))
            return false;
        if (!ExtraLODColor0.empty() && ExtraLODColor0.size() != ExtraLODs.size())
            return false;
        for (size_t k = 0; k < ExtraLODs.size(); ++k) {
            const bool own = k < ExtraLODVertices.size() && !ExtraLODVertices[k].empty();
            const Vector<float>* colours = k < ExtraLODColor0.size() ? &ExtraLODColor0[k] : nullptr;
            if (!own || Color0.empty()) {
                if (colours && !colours->empty()) return false;
            } else if (!colours || colours->size() != ExtraLODVertices[k].size() * 4u || !finite(*colours)) {
                return false;
            }
        }
        return true;
    }

    // Own-vertex LOD blocks carry the Vertex-interleaved attributes plus an
    // aligned per-level RGBA block. The remaining parallel streams address
    // LOD0's block only, so a chain carrying them cannot hold the single
    // vertexOffset every stream shares and must draw LOD0-only.
    bool OwnVertexLODStreamsSupported() const {
        return HasValidLODColor0() && !HasTexCoords1() && !IsSkinned() &&
               !HasMorphTargets() && ExtraTexCoords.empty();
    }

    // Total LOD count (LOD0 + simplified levels).
    uint32 LODCount() const { return 1u + static_cast<uint32>(ExtraLODs.size()); }
    // Index buffer for a given LOD (0 = full-res Indices). Out-of-range falls
    // back to LOD0.
    const Vector<uint32>& LODIndices(uint32 lod) const {
        return (lod == 0u || lod > ExtraLODs.size()) ? Indices : ExtraLODs[lod - 1u];
    }
};

Mesh CreateMorphedMesh(const Mesh& sourceMesh, const float* weights, size_t weightCount);
void GenerateMeshTangents(Mesh& mesh, bool forceRegenerate = false);

/**
 * @brief glTF-compatible alpha modes
 */
enum class AlphaMode : uint32 {
    Opaque = 0,
    Mask   = 1,
    Blend  = 2
};

/**
 * @brief Raw image data embedded inside a model file (e.g. GLB buffer views).
 */
struct EmbeddedImage {
    std::string MimeType;   // "image/png", "image/jpeg", etc.
    Vector<uint8> Data;     // Raw compressed bytes (PNG/JPEG)
    // Set when the image is an external file the AssetDatabase already knows:
    // materials then reference that asset by GUID (its import settings — mips,
    // compression, filtering — apply) and Data stays empty.
    GUID AssetGuid;

    // True once the image can become a texture: it holds its bytes, or it names
    // the asset that holds them. A reader that waits for textures waits on this,
    // not on Data alone, which an asset-backed image never fills.
    bool HasContent() const { return !Data.empty() || !AssetGuid.IsNull(); }
};

// Prefix used in texture reference strings to indicate an embedded image index.
// Usage: "__embedded:0", "__embedded:1", etc.
inline constexpr const char* kEmbeddedTexturePrefix = "__embedded:";
inline constexpr size_t kEmbeddedTexturePrefixLen = std::string_view(kEmbeddedTexturePrefix).size();

// True when a material texture reference names one of its model's embedded images.
inline bool IsEmbeddedTextureRef(std::string_view ref)
{
    return ref.size() > kEmbeddedTexturePrefixLen && ref.starts_with(kEmbeddedTexturePrefix);
}

/**
 * @brief Import-time material data from model files (glTF, OBJ, FBX).
 *
 * This struct holds raw imported material properties. It is NOT the runtime
 * material type used by the rendering pipeline. Use ModelMaterialBridge to
 * convert this into a MaterialDocument for shader compilation.
 */
struct ImportedMaterialData {
    String Name;
    String DiffuseTexture;
    String NormalTexture;
    String SpecularTexture;
    String EmissiveTexture;
    String OcclusionTexture;
    String RoughnessTexture;
    String MetallicTexture;
    std::array<float, 8> DiffuseTextureTransform{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    std::array<float, 8> NormalTextureTransform{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    std::array<float, 8> SpecularTextureTransform{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    std::array<float, 8> EmissiveTextureTransform{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    std::array<float, 8> OcclusionTextureTransform{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    std::array<float, 8> RoughnessTextureTransform{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    std::array<float, 8> MetallicTextureTransform{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    float DiffuseColor[4];      // r, g, b, a
    float SpecularColor[3];     // r, g, b
    float EmissiveColor[3] = {0.0f, 0.0f, 0.0f};
    float Shininess;
    float Metallic;
    float Roughness;
    // Newly surfaced glTF flags
    bool DoubleSided = false;
    ::GameEngine::AlphaMode AlphaMode = ::GameEngine::AlphaMode::Opaque;
    float AlphaCutoff = 0.5f; // used when AlphaMode==Mask
    // Mask was derived from texture content (FBX alpha-merge), not authored.
    // Only inferred Mask is eligible for UV-footprint demotion in PostLoad —
    // authored alpha modes (glTF) are intent and must never be overridden.
    bool AlphaModeInferred = false;
    // The source material never reads the mesh's vertex colour. FBX materials
    // (Lambert, Phong, PBR) have no vertex-colour input: a colour layer is data
    // for custom shaders, and multiplying it in paints baked masks (black,
    // zero-alpha) over the model. glTF materials multiply COLOR_0 by spec.
    bool IgnoresVertexColor = false;
};

struct ImportedCameraData {
    String Name;
    float Transform[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    float LocalTransform[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    bool Perspective = true;
    float FovY = 60.0f;
    float OrthographicSize = 10.0f;
    float NearZ = 0.01f;
    float FarZ = 1000.0f;
    int32 SourceNodeIndex = -1;
    int32 ParentSourceNodeIndex = -1;
};

enum class ImportedLightType : uint32 {
    Directional = 0,
    Point = 1,
    Spot = 2,
    Area = 3,
    Volume = 4,
};

enum class ImportedAreaLightShape : uint32 {
    Rectangle = 0,
    Disc = 1,
    Sphere = 2,
    Cylinder = 3,
};

struct ImportedLightData {
    String Name;
    float Transform[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    float LocalTransform[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    ImportedLightType Type = ImportedLightType::Point;
    ImportedAreaLightShape AreaShape = ImportedAreaLightShape::Rectangle;
    float Color[3] = {1.0f, 1.0f, 1.0f};
    float Intensity = 1.0f;
    float Range = 10.0f;
    float InnerAngle = 0.5f;
    float OuterAngle = 0.8f;
    float AreaWidth = 1.0f;
    float AreaHeight = 1.0f;
    float AreaRadius = 0.5f;
    float Decay = 2.0f;
    bool CastsLight = true;
    bool CastsShadows = false;
    int32 SourceNodeIndex = -1;
    int32 ParentSourceNodeIndex = -1;
};

struct ImportedSceneNodeData {
    String Name;
    float Transform[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    float LocalTransform[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    int32 SourceNodeIndex = -1;
    int32 ParentSourceNodeIndex = -1;
};

/**
 * @brief Model asset implementation
 *
 * Handles loading and management of 3D model files including OBJ, FBX, GLTF, and other formats.
 * Provides access to mesh data, materials, and basic model information for rendering systems.
 */
class ModelAsset : public Asset {
public:
    ModelAsset(const GUID& guid, const std::filesystem::path& path);
    virtual ~ModelAsset();

    uint32 GetSkeletonId() const { return m_SkeletonId; }

    /**
     * @brief Load model from file
     */
    bool Load() override;

    /**
     * @brief Load model from memory data (for async loading)
     */
    bool LoadFromData(const Vector<uint8>& data) override;

    /**
     * @brief Unload model from memory
     */
    void Unload() override;

    /**
     * @brief Run model-specific post-load processing.
     *
     * Invoked by AssetManager (and Asset::Reload) after Load/LoadFromData
     * succeeds. Currently a no-op insertion point; mesh-picking will hook
     * its BVH bake here so it fires on both initial load and hot-reload.
     */
    void PostLoad() override;

    /**
     * @brief Hot-reload through the async pipeline: parse + LOD/PostLoad work
     * runs on workers, the decoded payload is adopted in place on the main
     * thread (AdoptReloadedPayload below). Same contract as TextureAsset.
     */
    bool SupportsAsyncReload() const override { return true; }

    /**
     * @brief Get model format
     */
    ModelFormat GetFormat() const { return m_Format; }

    /** Lowercased extension -> ModelFormat. Unknown for unrecognized suffixes. */
    static ModelFormat DetermineFormat(const String& extension);

    /** DetermineFormat on `path`'s extension (case-insensitive). */
    static ModelFormat FormatFromPath(const std::filesystem::path& path);

    /** FBX / glTF / GLB / Blend consume bake + engine-axis mirrors. */
    static bool UsesAxisImportOptions(ModelFormat format);

    /**
     * @brief Get number of meshes
     */
    uint32 GetMeshCount() const { return static_cast<uint32>(m_Meshes.size()); }

    /**
     * @brief Get mesh by index
     */
    const Mesh& GetMesh(uint32 index) const { return m_Meshes[index]; }

    /**
     * @brief Get all meshes
     */
    const Vector<Mesh>& GetMeshes() const { return m_Meshes; }

    /**
     * @brief Test hook: install synthesized meshes without a file load and
     * mark the asset Loaded. Enables in-memory fixtures for systems that
     * resolve meshes through a ModelAsset (the morph deep-path harness) —
     * the AnimationClip::SetChannelsAndDurationForTest precedent.
     */
    void SetMeshesForTest(Vector<Mesh> meshes)
    {
        m_Meshes = std::move(meshes);
        SetState(AssetState::Loaded);
    }

    /**
     * @brief Test hook: point this asset at an existing SkeletonStore skeleton,
     * the way PostLoad would after importing a rig. Lets a fixture drive the
     * skinned spawn path without a file load. Pairs with SetMeshesForTest.
     */
    void SetSkeletonIdForTest(uint32 skeletonId) { m_SkeletonId = skeletonId; }

    /**
     * @brief Test hook: mark the asset as carrying animations, as a load of a
     * file with clips would. Lets a fixture drive where the factory puts the
     * model's Animator. Pairs with SetSkeletonIdForTest.
     */
    void SetHasAnimationsForTest(bool hasAnimations) { m_HasAnimations = hasAnimations; }

    /**
     * @brief Test hook: install imported helper nodes without a file load and
     * mark the asset Loaded. Lets a fixture drive ModelEntityFactory's
     * helper-node spawn topology. Mirrors SetMeshesForTest.
     */
    void SetSceneNodesForTest(Vector<ImportedSceneNodeData> nodes)
    {
        m_SceneNodes = std::move(nodes);
        SetState(AssetState::Loaded);
    }

    /**
     * @brief Generate simplified LODs for the model's meshes (fills
     *        Mesh::ExtraLODs).
     *
     * Uses MeshLODGenerator. Safe to call repeatedly (regenerates). Skinned
     * meshes are skipped unless generateSkinned is set (position-only simplify
     * ignores joint-assignment seams). Callers that have already uploaded GPU
     * meshes must re-register them afterwards for the new LODs to take effect.
     * Returns the max LOD count produced.
     *
     * Covers every mesh only when the decode is not cancelled: the flag is
     * polled per submesh (and again per level inside the generator), so a
     * cancelled call generates for the submeshes it reached and none after —
     * possibly none at all, returning 1. The result is short, not wrong, and
     * must not be persisted; LoadOrGenerateLODs is what enforces that.
     */
    uint32 GenerateLODs(const struct MeshLODConfig& config, bool generateSkinned);

    /**
     * @brief Cook this loaded model's LODs to a .gelod for packaging (Phase B).
     *
     * Generates LODs via meshopt (honouring the skinned policy) then writes the
     * cooked cache to outFile with the key baked into the header so a packaged
     * Player can validate and consume it without recomputing a source
     * fingerprint. Reuses the same GenerateLODs + key composition the runtime
     * PostLoad path uses, so the cooked bytes and key match what the Player
     * would regenerate. Returns bytes written, or 0 when no submesh produced any
     * LOD (no file is written) or the write failed. Model must be Loaded.
     */
    uint64 CookLODCache(const struct MeshLODConfig& config, bool generateSkinned,
                        const std::filesystem::path& outFile);

    /**
     * @brief Get number of materials
     */
    uint32 GetMaterialCount() const { return static_cast<uint32>(m_Materials.size()); }

    /**
     * @brief Get material by index
     */
    const ImportedMaterialData& GetMaterial(uint32 index) const { return m_Materials[index]; }

    /**
     * @brief Get all materials
     */
    const Vector<ImportedMaterialData>& GetMaterials() const { return m_Materials; }

    /**
     * @brief Get embedded images extracted from GLB/glTF.
     */
    const Vector<EmbeddedImage>& GetEmbeddedImages() const { return m_EmbeddedImages; }

    const Vector<ImportedCameraData>& GetImportedCameras() const { return m_Cameras; }
    const Vector<ImportedLightData>& GetImportedLights() const { return m_Lights; }
    const Vector<ImportedSceneNodeData>& GetImportedSceneNodes() const { return m_SceneNodes; }

    /**
     * @brief The raw extras JSON text the import kept for the source file's scenes, nodes, meshes,
     * materials and animations, by object kind and name (glTF and GLB; empty for other formats).
     *
     * The engine reads none of it; ModelApi.GetExtras hands it to scripts.
     */
    const ModelExtras& GetExtras() const { return m_Extras; }

    /**
     * @brief Get total vertex count across all meshes
     */
    uint32 GetTotalVertexCount() const { return m_TotalVertexCount; }

    /**
     * @brief Get total index count across all meshes
     */
    uint32 GetTotalIndexCount() const { return m_TotalIndexCount; }

    /**
     * @brief FNV-1a content hash of the raw source bytes, captured at load.
     *
     * The portable source key for the cooked-LOD cache (.gelod) — see
     * MeshLODCache. Zero until LoadFromData has run.
     */
    uint64 GetSourceContentHash() const { return m_SourceContentHash; }

    /**
     * @brief Test hook: set the source content hash without a file load, so
     * fixtures can synthesize distinct-geometry models (the HLOD benefit
     * heuristic keys "distinct meshes" off this). Mirrors SetMeshesForTest.
     */
    void SetSourceContentHashForTest(uint64 hash) { m_SourceContentHash = hash; }

    /**
     * @brief Get model bounding box
     */
    void GetBoundingBox(float minBounds[3], float maxBounds[3]) const;

    /**
     * @brief Check if model has animations
     */
    bool HasAnimations() const { return m_HasAnimations; }

    // Names of animations embedded in the source file, in source-file order.
    // Empty when the model has no animations or when the loader couldn't
    // extract names (in which case HasAnimations may still be true).
    const Vector<String>& GetAnimationNames() const { return m_AnimationNames; }

    /// Stable derived GUIDs for each embedded animation, parallel to
    /// GetAnimationNames(). Each clip is eagerly registered into the runtime
    /// `ClipStore` at model-load time under this GUID so consumers (inspector,
    /// AnimationSystem, preview) can refer to embedded clips the same way they
    /// refer to standalone .anim/.fbx clip assets — no special "embedded vs
    /// external" branching at runtime.
    const Vector<GUID>& GetEmbeddedClipGuids() const { return m_EmbeddedClipGuids; }

    /// Compute the stable derived GUID for the Nth embedded animation of this
    /// model. Used by the loader at registration time and by editor tools
    /// before the model has fully loaded (e.g. resolving a clip GUID stored on
    /// an entity from a serialized scene). Returns Null if the index is out of
    /// range and the asset is loaded; for not-yet-loaded models it derives the
    /// GUID without a bounds check.
    static GUID DeriveEmbeddedClipGuid(const GUID& modelGuid, uint32 animationIndex);

protected:
    /**
     * @brief Swap the decoded model payload with a freshly loaded instance
     * (main thread). Mesh/clip members swap; skeleton rest/IBM copies into
     * this instance's existing SkeletonStore id so live SkeletonRefs stay
     * valid. See Asset::AdoptReloadedPayload.
     */
    bool AdoptReloadedPayload(Asset& staged) override;

private:
    friend struct ModelAssetFbxTestAccess;

    /**
     * @brief Load OBJ model file
     */
    bool LoadOBJ(const Vector<uint8>& data);

    /**
     * @brief Parse FBX bytes with explicit loader options.
     * LoadFromData resolves kv via ResolveFbxLoaderOptions and passes it here.
     * @param modelPathForExternalTextures When non-empty, used to resolve non-embedded texture paths
     *        (e.g. diffuse maps referenced by file name) relative to the FBX directory.
     */
    bool LoadFBX(const Vector<uint8>& data,
                 const std::filesystem::path& modelPathForExternalTextures,
                 const FbxLoaderOptions& loaderOptions);

    /**
     * @brief Parse glTF / GLB bytes with explicit loader options.
     * LoadFromData resolves kv via ResolveFbxLoaderOptions and passes it here.
     * Bake / mirrors share ModelImport::MakeEngineConversion with LoadFBX
     * (GltfImport::SourceConversion is identity; default Mirror X is the RH→LH bake).
     */
    bool LoadGLTF(const Vector<uint8>& data, const FbxLoaderOptions& loaderOptions);

    /**
     * @brief Load Blender (.blend) model file via vendored fbtBlend.
     * @param data Raw .blend file bytes (uncompressed or zlib/zstd-compressed —
     *        magic-prefix sniff dispatches to the right codec).
     * @param modelPathForExternalTextures When non-empty, used to resolve any
     *        non-embedded image paths relative to the .blend directory.
     *        Phase B5 adds the actual texture-resolution walk; Phase B2 only
     *        consumes this for diagnostics + future hot-reload bookkeeping.
     * @param loaderOptions Bake / mirrors share ModelImport::MakeEngineConversion
     *        with LoadFBX / LoadGLTF (BlendImport::SourceConversion + default
     *        Mirror X is the B_to_E map (x,y,z)->(-x,z,y)).
     */
    bool LoadFromBlendData(const Vector<uint8>& data,
                           const std::filesystem::path& modelPathForExternalTextures,
                           const FbxLoaderOptions& loaderOptions);

    // The SkeletonStore slot survives a Reload(): keep it so live SkeletonRefs
    // still name the same id while rest/IBM refill.
    Animation::SkeletonData* RetainOrCreateSkeleton(uint32 boneCount);

    /**
     * @brief Derive the Nth embedded clip's GUID and record both the GUID and
     * its derive key on this model (loader mint sites call this instead of
     * DeriveEmbeddedClipGuid so the key list stays exact per load).
     */
    GUID MintEmbeddedClipGuid(uint32 animationIndex);

    // First insert may run on a decode worker. Replace of a live ClipStore
    // slot is queued until PublishPendingRuntimeClips: AdoptReloadedPayload
    // on the main thread, or a load that entered with m_SkeletonId already
    // set (in-place refill of this instance). A superseded async decode is
    // a fresh ModelAsset (skeleton id 0) and never publishes.
    void StageOrPublishRuntimeClip(const GUID& guid, std::shared_ptr<Animation::AnimationClip> clip);
    void PublishPendingRuntimeClips();

    /**
     * @brief Journal this container's subasset derive keys (minted clip keys
     * plus the index-dense bridge-material scheme) via
     * AssetRegistry::RegisterSubassetDeriveKeys, so a container rename can
     * cascade redirects for the derived identities. No-op in a bare loader
     * harness without an engine asset manager.
     */
    void PersistSubassetDeriveKeys();

    /**
     * @brief Calculate bounding box for all meshes
     */
    void CalculateBoundingBox();

    /**
     * @brief Calculate vertex and index counts
     */
    void CalculateCounts();

    /**
     * @brief Resolve this model's LODs cache-first (design v0.2 §3.3).
     *
     * Looks up the cooked .gelod in the owning source's derived cache when its
     * policy permits cooking, including immutable development packages; shipped
     * Player sources use the sidecar beside the manifest. A hit applies when
     * the key / format / geometry validate. A miss generates via meshopt and
     * writes only to a cache whose policy permits cooking. Exactly
     * one place decides cooked-vs-generated. `assetManager` may be null (raw
     * harness) in which case it always generates.
     *
     * The warm-write is SKIPPED when the decode was cancelled. A cancelled
     * generation stops partway, so its chain is short by design; writing it
     * would key a TRUNCATED chain to the full config hash, and every later load
     * would take that as a cache hit — a permanent, silent LOD loss. Do not
     * remove that guard to make the write unconditional again.
     */
    void LoadOrGenerateLODs(AssetManager* assetManager,
                            const struct MeshLODConfig& config, bool generateSkinned);

    /**
     * @brief Demote FBX merge-inferred Mask materials whose diffuse-UV
     *        footprint is provably opaque (defined in ModelAssetLoadFbx.cpp).
     *
     * Must run from PostLoad after LOD resolution so authored-slot and
     * generated/cooked LOD triangles contribute their footprints. No-op for
     * authored alpha modes (glTF) — only AlphaModeInferred materials qualify.
     */
    void DemoteInferredMaskMaterials();

private:
    ModelFormat m_Format;
    Vector<Mesh> m_Meshes;
    Vector<ImportedMaterialData> m_Materials;
    Vector<EmbeddedImage> m_EmbeddedImages;
    Vector<ImportedCameraData> m_Cameras;
    Vector<ImportedLightData> m_Lights;
    Vector<ImportedSceneNodeData> m_SceneNodes;
    ModelExtras m_Extras;
    uint32 m_TotalVertexCount;
    uint32 m_TotalIndexCount;
    uint32 m_SkeletonId = 0;
    uint64 m_SourceContentHash = 0;
    // Hash of the resolved format-specific parse options that shaped the parsed
    // geometry (FBX / glTF / Blend; 0 for formats with none). Folded into
    // the cooked-LOD cache key so a parse-option change invalidates stale LODs (F1).
    uint64 m_ParseOptionsHash = 0;
    // Folded identity of explicit LOD-slot sources resolved in PostLoad (0 when no
    // slots). Authored geometry sourced from OTHER model assets lives outside this
    // model's source bytes, so it must extend the cooked-LOD cache key or a slot
    // edit / removal would leave a stale cook (C1 sibling-file carry-forward).
    uint64 m_AuthoredSlotSourceHash = 0;

    bool m_HasAnimations;
    Vector<String> m_AnimationNames;
    Vector<GUID> m_EmbeddedClipGuids;
    Vector<std::pair<GUID, std::shared_ptr<Animation::AnimationClip>>> m_PendingRuntimeClips;
    // Derive keys of every subasset identity this load minted from the model's
    // GUID (embedded clips, mint order). Cleared per load in LoadFromData;
    // journaled by PersistSubassetDeriveKeys so container renames can cascade
    // redirects for the derived GUIDs (see AssetCore/SubassetDeriveKeys.h).
    Vector<String> m_SubassetDeriveKeys;

    // Model bounding box
    float m_MinBounds[3];
    float m_MaxBounds[3];

    DISALLOW_COPY_AND_ASSIGN(ModelAsset);
};

} // namespace GameEngine
