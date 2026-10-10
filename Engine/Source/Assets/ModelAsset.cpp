#include "Assets/ModelAsset.h"
#include "Assets/ImportedMaterialCache.h"
#include "Assets/MeshLODGeometry.h"
#include "AssetCore/SharedFileRead.h"
#include "AssetCore/SubassetDeriveKeys.h"
#include "Assets/AnimationClip.h"
#include "Assets/AssetDecodeCancellation.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "Assets/AuthoredLodImport.h"
#include "Assets/LodAssetSettings.h"
#include "Assets/ModelAssetSettings.h"
#include "Assets/MeshLODCache.h"
#include "Assets/MeshLODGenerator.h"
#include "Logger/Logger.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "ECSModules/Rendering/SkeletonStore.h"

#include "Animation/HumanoidImport.h"
#include "Animation/HumanoidRig.h"
#include "Animation/SkeletonProfile.h"
#include "Assets/RuntimeHumanoidProfile.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <cmath>
#include <optional>
#include <string_view>
#include <system_error>

namespace GameEngine {

static bool MeshHasTangents(const Mesh& mesh)
{
    for (const Vertex& v : mesh.Vertices)
    {
        if ((v.Tangent[0] * v.Tangent[0] + v.Tangent[1] * v.Tangent[1] + v.Tangent[2] * v.Tangent[2]) > 1e-10f)
            return true;
    }
    return false;
}

static glm::vec3 SafeNormalize(const glm::vec3& v, const glm::vec3& fallback)
{
    const float len2 = glm::dot(v, v);
    if (len2 <= 1e-12f)
        return fallback;
    return v * (1.0f / std::sqrt(len2));
}

static glm::vec3 BuildFallbackTangent(const glm::vec3& n)
{
    const glm::vec3 axis = std::abs(n.y) < 0.999f
        ? glm::vec3(0.0f, 1.0f, 0.0f)
        : glm::vec3(1.0f, 0.0f, 0.0f);
    return SafeNormalize(glm::cross(axis, n), glm::vec3(1.0f, 0.0f, 0.0f));
}

void GenerateMeshTangents(Mesh& mesh, bool forceRegenerate)
{
    if (mesh.Vertices.empty() || mesh.Indices.size() < 3)
        return;
    if (!forceRegenerate && MeshHasTangents(mesh))
        return;

    std::vector<glm::vec3> tangent(mesh.Vertices.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> bitangent(mesh.Vertices.size(), glm::vec3(0.0f));
    bool anyAccumulated = false;

    for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
    {
        const uint32 i0 = mesh.Indices[i + 0];
        const uint32 i1 = mesh.Indices[i + 1];
        const uint32 i2 = mesh.Indices[i + 2];
        if (i0 >= mesh.Vertices.size() || i1 >= mesh.Vertices.size() || i2 >= mesh.Vertices.size())
            continue;

        const Vertex& v0 = mesh.Vertices[i0];
        const Vertex& v1 = mesh.Vertices[i1];
        const Vertex& v2 = mesh.Vertices[i2];
        const glm::vec3 p0(v0.Position[0], v0.Position[1], v0.Position[2]);
        const glm::vec3 p1(v1.Position[0], v1.Position[1], v1.Position[2]);
        const glm::vec3 p2(v2.Position[0], v2.Position[1], v2.Position[2]);
        const glm::vec2 uv0(v0.TexCoords[0], v0.TexCoords[1]);
        const glm::vec2 uv1(v1.TexCoords[0], v1.TexCoords[1]);
        const glm::vec2 uv2(v2.TexCoords[0], v2.TexCoords[1]);

        const glm::vec3 e1 = p1 - p0;
        const glm::vec3 e2 = p2 - p0;
        const glm::vec2 d1 = uv1 - uv0;
        const glm::vec2 d2 = uv2 - uv0;
        const float det = d1.x * d2.y - d1.y * d2.x;
        if (std::abs(det) <= 1e-12f)
            continue;

        const float r = 1.0f / det;
        const glm::vec3 sdir = (e1 * d2.y - e2 * d1.y) * r;
        const glm::vec3 tdir = (e2 * d1.x - e1 * d2.x) * r;
        tangent[i0] += sdir; tangent[i1] += sdir; tangent[i2] += sdir;
        bitangent[i0] += tdir; bitangent[i1] += tdir; bitangent[i2] += tdir;
        anyAccumulated = true;
    }

    if (!anyAccumulated)
        return;

    for (size_t i = 0; i < mesh.Vertices.size(); ++i)
    {
        Vertex& v = mesh.Vertices[i];
        glm::vec3 n(v.Normal[0], v.Normal[1], v.Normal[2]);
        n = SafeNormalize(n, glm::vec3(0.0f, 1.0f, 0.0f));

        glm::vec3 t = tangent[i] - n * glm::dot(n, tangent[i]);
        t = SafeNormalize(t, BuildFallbackTangent(n));

        const float handedness = glm::dot(glm::cross(n, t), bitangent[i]) < 0.0f ? -1.0f : 1.0f;
        v.Tangent[0] = t.x;
        v.Tangent[1] = t.y;
        v.Tangent[2] = t.z;
        v.Tangent[3] = handedness;
    }
}

Mesh CreateMorphedMesh(const Mesh& sourceMesh, const float* weights, size_t weightCount)
{
    Mesh out = sourceMesh;
    out.MorphTargets.clear();

    if (!weights || weightCount == 0 || sourceMesh.MorphTargets.empty() || sourceMesh.Vertices.empty())
        return out;

    const size_t vertexCount = sourceMesh.Vertices.size();
    const bool hadTangents = MeshHasTangents(sourceMesh);
    bool normalsChanged = false;
    const size_t targetCount = std::min(weightCount, sourceMesh.MorphTargets.size());

    for (size_t targetIndex = 0; targetIndex < targetCount; ++targetIndex)
    {
        const float weight = weights[targetIndex];
        if (std::abs(weight) <= 1e-6f)
            continue;

        // A delta array applies only when it holds three floats per listed vertex; a listed vertex
        // past the mesh is skipped.
        const MorphTarget& target = sourceMesh.MorphTargets[targetIndex];
        const size_t listedCount = target.VertexIndices.size();
        if (target.PositionDeltas.size() == listedCount * 3u)
        {
            for (size_t entry = 0; entry < listedCount; ++entry)
            {
                const uint32 vertexIndex = target.VertexIndices[entry];
                if (vertexIndex >= vertexCount)
                    continue;
                Vertex& v = out.Vertices[vertexIndex];
                const float* delta = target.PositionDeltas.data() + entry * 3u;
                v.Position[0] += delta[0] * weight;
                v.Position[1] += delta[1] * weight;
                v.Position[2] += delta[2] * weight;
            }
        }
        if (!target.NormalDeltas.empty() && target.NormalDeltas.size() == listedCount * 3u)
        {
            normalsChanged = true;
            for (size_t entry = 0; entry < listedCount; ++entry)
            {
                const uint32 vertexIndex = target.VertexIndices[entry];
                if (vertexIndex >= vertexCount)
                    continue;
                Vertex& v = out.Vertices[vertexIndex];
                const float* delta = target.NormalDeltas.data() + entry * 3u;
                v.Normal[0] += delta[0] * weight;
                v.Normal[1] += delta[1] * weight;
                v.Normal[2] += delta[2] * weight;
            }
        }
    }

    if (normalsChanged)
    {
        for (Vertex& v : out.Vertices)
        {
            const float len2 = v.Normal[0] * v.Normal[0] + v.Normal[1] * v.Normal[1] + v.Normal[2] * v.Normal[2];
            if (len2 > 1e-12f)
            {
                const float invLen = 1.0f / std::sqrt(len2);
                v.Normal[0] *= invLen;
                v.Normal[1] *= invLen;
                v.Normal[2] *= invLen;
            }
        }
    }

    if (hadTangents)
        GenerateMeshTangents(out, true);

    out.MinBounds[0] = out.MinBounds[1] = out.MinBounds[2] = std::numeric_limits<float>::max();
    out.MaxBounds[0] = out.MaxBounds[1] = out.MaxBounds[2] = std::numeric_limits<float>::lowest();
    for (const Vertex& v : out.Vertices)
    {
        for (int c = 0; c < 3; ++c)
        {
            out.MinBounds[c] = std::min(out.MinBounds[c], v.Position[c]);
            out.MaxBounds[c] = std::max(out.MaxBounds[c], v.Position[c]);
        }
    }

    return out;
}

// Optional importer libraries
#if defined(GE_HAVE_TINYOBJ)
  #include <tiny_obj_loader.h>
#endif


ModelAsset::ModelAsset(const GUID& guid, const std::filesystem::path& path)
    : Asset(guid, AssetType::Model, path)
    , m_Format(ModelFormat::Unknown)
    , m_TotalVertexCount(0)
    , m_TotalIndexCount(0)
    , m_SkeletonId(0)
    , m_HasAnimations(false)
{
    // Initialize bounding box to invalid values
    m_MinBounds[0] = m_MinBounds[1] = m_MinBounds[2] = std::numeric_limits<float>::max();
    m_MaxBounds[0] = m_MaxBounds[1] = m_MaxBounds[2] = std::numeric_limits<float>::lowest();
}

GUID ModelAsset::DeriveEmbeddedClipGuid(const GUID& modelGuid, uint32 animationIndex)
{
    // Stable derivation rule shared by every consumer (ModelAsset loader,
    // inspector resolution, scene serialization). The key layout lives in
    // AssetCore/SubassetDeriveKeys.h: its "embedded:" prefix disambiguates
    // from any other derived-GUID namespace this model owns (e.g. derived
    // material/texture GUIDs), and the journal persists the keys so container
    // renames can cascade redirects for these GUIDs.
    return GUID::Derive(modelGuid, EmbeddedClipDeriveKey(animationIndex));
}

GUID ModelAsset::MintEmbeddedClipGuid(uint32 animationIndex)
{
    const GUID clipGuid = DeriveEmbeddedClipGuid(GetGUID(), animationIndex);
    m_EmbeddedClipGuids.push_back(clipGuid);
    m_SubassetDeriveKeys.push_back(EmbeddedClipDeriveKey(animationIndex));
    return clipGuid;
}

void ModelAsset::PersistSubassetDeriveKeys()
{
    // Minted clip keys, plus the bridge-material scheme — index-dense over the
    // material array by construction (ModelMaterialBridge::ConvertAll), so the
    // keys are derivable here without running the bridge.
    Vector<String> keys = m_SubassetDeriveKeys;
    keys.reserve(keys.size() + m_Materials.size());
    for (uint32 i = 0; i < static_cast<uint32>(m_Materials.size()); ++i)
        keys.push_back(ModelMaterialDeriveKey(i));

    // A bare loader harness has no asset manager, hence no registry/journal.
    AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager();
    if (!assetManager)
        return;
    assetManager->GetRegistry().RegisterSubassetDeriveKeys(GetPath(), keys);
}

ModelAsset::~ModelAsset() {
    Unload();
}

Animation::SkeletonData* ModelAsset::RetainOrCreateSkeleton(uint32 boneCount)
{
    auto& store = Engine::Renderer::SkeletonStore::Instance();
    const uint32 count = boneCount > 0 ? boneCount : 1u;
    if (m_SkeletonId != 0)
    {
        if (Animation::SkeletonData* existing = store.Get(m_SkeletonId))
        {
            store.EnsureSizes(m_SkeletonId, count);
            return existing;
        }
    }
    m_SkeletonId = store.CreateSkeleton(count);
    return store.Get(m_SkeletonId);
}

void ModelAsset::StageOrPublishRuntimeClip(const GUID& guid, std::shared_ptr<Animation::AnimationClip> clip)
{
    if (guid.IsNull() || !clip)
        return;
    auto& store = Engine::Renderer::ClipStore::Instance();
    if (store.TryInsertRuntimeClip(guid, clip) != 0)
        return;
    m_PendingRuntimeClips.emplace_back(guid, std::move(clip));
}

void ModelAsset::PublishPendingRuntimeClips()
{
    auto& store = Engine::Renderer::ClipStore::Instance();
    for (auto& entry : m_PendingRuntimeClips)
        store.RegisterRuntimeClip(entry.first, std::move(entry.second));
    m_PendingRuntimeClips.clear();
}

bool ModelAsset::Load() {
    if (GetState() == AssetState::Loaded) {
        return true;
    }

    SetState(AssetState::Loading);
    Logger::Log::Info("Loading model: {}", GetPath().string());

    if (!Exists()) {
        Logger::Log::Error("Model file does not exist: {}", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }

    // Read file data
    Vector<uint8> fileData;
    if (!ReadFileBytesShared(GetPath(), fileData)) {
        Logger::Log::Error("Failed to read model file: {}", GetPath().string());
        SetState(AssetState::Failed);
        return false;
    }

    return LoadFromData(fileData);
}

bool ModelAsset::LoadFromData(const Vector<uint8>& data) {
    if (GetState() == AssetState::Loaded) {
        return true;
    }

    // Staging async clones construct with m_SkeletonId == 0; in-place
    // Unload+Load keeps the live slot. Only the live slot publishes
    // ClipStore replacements (a dropped generation must not clobber).
    const bool publishClipReplacements = m_SkeletonId != 0;

    SetState(AssetState::Loading);
    Logger::Log::Info("Loading model from memory data: {}", GetName());

    if (data.empty()) {
        Logger::Log::Error("Empty model data provided");
        SetState(AssetState::Failed);
        return false;
    }

    // Portable source key for the cooked-LOD cache: the raw bytes are already in
    // hand here, so hashing them avoids a second read at cache-lookup time.
    m_SourceContentHash = ComputeLodSourceHash(data.data(), data.size());
    // Reset before parse; FBX / glTF / Blend loaders set it from the resolved
    // parse options. Formats without geometry-affecting options leave it 0.
    m_ParseOptionsHash = 0;

    // Exact per load: the format loaders only ever append these, and only
    // when the parsed content has animations, so the reset belongs here —
    // a re-load of clipless content must not report the previous parse's
    // clips. The failed-parse-then-retry path is what makes this reachable:
    // a loader can record clips and still return false.
    m_SubassetDeriveKeys.clear();
    m_HasAnimations = false;
    m_AnimationNames.clear();
    m_EmbeddedClipGuids.clear();

    String extension = GetExtension();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](char c) { return static_cast<char>(std::tolower(c)); });

    m_Format = DetermineFormat(extension);

    // Fall back to magic-number sniff when extension didn't classify. The
    // .blend format uniquely starts with "BLENDER" (uncompressed) or with
    // gzip / zstd magic bytes when compressed. We don't try to second-guess
    // the other formats here — wrong extensions on FBX / glTF / OBJ are far
    // less common in practice than headerless .blend autosaves.
    if (m_Format == ModelFormat::Unknown && data.size() >= 7) {
        const auto* d = data.data();
        if (std::memcmp(d, "BLENDER", 7) == 0) {
            m_Format = ModelFormat::BLEND;
        }
        else if (data.size() >= 4 && d[0] == 0x1F && d[1] == 0x8B) {
            // gzip magic — could be a Blender < 3.0 compressed .blend.
            m_Format = ModelFormat::BLEND;
        }
        else if (data.size() >= 4
                 && d[0] == 0x28 && d[1] == 0xB5 && d[2] == 0x2F && d[3] == 0xFD) {
            // zstd magic — Blender >= 3.0 compressed .blend.
            m_Format = ModelFormat::BLEND;
        }
    }

    bool success = false;
    switch (m_Format) {
        case ModelFormat::OBJ:
            success = LoadOBJ(data);
            break;
        case ModelFormat::FBX:
            success = LoadFBX(data, GetPath(), ResolveFbxLoaderOptions(GetPath()));
            break;
        case ModelFormat::GLTF:
        case ModelFormat::GLB:
            success = LoadGLTF(data, ResolveFbxLoaderOptions(GetPath()));
            break;
        case ModelFormat::BLEND:
            success = LoadFromBlendData(data, GetPath(), ResolveFbxLoaderOptions(GetPath()));
            break;
        default:
            Logger::Log::Error("Unsupported model format: {}", extension);
            SetState(AssetState::Failed);
            return false;
    }

    if (success) {
        // Fold `_LOD<N>` submesh families into authored LOD chains before bounds /
        // counts so both reflect the consumed submesh array (format-agnostic — it
        // operates on the assembled submesh names every loader produced).
        ConsumeLodSuffixFamilies(m_Meshes);
        CalculateBoundingBox();
        CalculateCounts();
        SetState(AssetState::Loaded);
        PersistSubassetDeriveKeys();
        if (publishClipReplacements)
            PublishPendingRuntimeClips();
        Logger::Log::Info("Model loaded successfully: {} ({} meshes, {} vertices, {} indices)",
                    GetName(), m_Meshes.size(), m_TotalVertexCount, m_TotalIndexCount);
    } else {
        SetState(AssetState::Failed);
    }

    return success;
}

struct ModelAssetFbxTestAccess
{
    static bool Load(ModelAsset& asset, const Vector<uint8>& data, const FbxLoaderOptions& options);
    static bool LoadGltf(ModelAsset& asset, const Vector<uint8>& data, const FbxLoaderOptions& options);
    static bool LoadBlend(ModelAsset& asset, const Vector<uint8>& data, const FbxLoaderOptions& options);
    static uint64 ParseOptionsHash(const ModelAsset& asset);
    static void DemoteMasks(ModelAsset& asset, Vector<Mesh> meshes,
                            Vector<ImportedMaterialData> materials, Vector<EmbeddedImage> images);
};

void ModelAssetFbxTestAccess::DemoteMasks(ModelAsset& asset, Vector<Mesh> meshes,
                                       Vector<ImportedMaterialData> materials, Vector<EmbeddedImage> images)
{
    asset.m_Meshes = std::move(meshes);
    asset.m_Materials = std::move(materials);
    asset.m_EmbeddedImages = std::move(images);
    asset.DemoteInferredMaskMaterials();
}

bool ModelAssetFbxTestAccess::Load(ModelAsset& asset, const Vector<uint8>& data, const FbxLoaderOptions& options)
{
    const bool publishClipReplacements = asset.m_SkeletonId != 0;
    asset.m_Format = ModelFormat::FBX;
    asset.m_SourceContentHash = ComputeLodSourceHash(data.data(), data.size());
    asset.m_SubassetDeriveKeys.clear();
    asset.m_HasAnimations = false;
    asset.m_AnimationNames.clear();
    asset.m_EmbeddedClipGuids.clear();
    if (!asset.LoadFBX(data, asset.GetPath(), options))
    {
        asset.SetState(AssetState::Failed);
        return false;
    }
    ConsumeLodSuffixFamilies(asset.m_Meshes);
    asset.CalculateBoundingBox();
    asset.CalculateCounts();
    asset.SetState(AssetState::Loaded);
    asset.PersistSubassetDeriveKeys();
    if (publishClipReplacements)
        asset.PublishPendingRuntimeClips();
    return true;
}

bool ModelAssetFbxTestAccess::LoadGltf(ModelAsset& asset, const Vector<uint8>& data, const FbxLoaderOptions& options)
{
    const bool publishClipReplacements = asset.m_SkeletonId != 0;
    asset.m_Format = ModelFormat::GLTF;
    if (data.size() >= 4 && std::memcmp(data.data(), "glTF", 4) == 0)
        asset.m_Format = ModelFormat::GLB;
    asset.m_SourceContentHash = ComputeLodSourceHash(data.data(), data.size());
    asset.m_SubassetDeriveKeys.clear();
    asset.m_HasAnimations = false;
    asset.m_AnimationNames.clear();
    asset.m_EmbeddedClipGuids.clear();
    if (!asset.LoadGLTF(data, options))
    {
        asset.SetState(AssetState::Failed);
        return false;
    }
    ConsumeLodSuffixFamilies(asset.m_Meshes);
    asset.CalculateBoundingBox();
    asset.CalculateCounts();
    asset.SetState(AssetState::Loaded);
    asset.PersistSubassetDeriveKeys();
    if (publishClipReplacements)
        asset.PublishPendingRuntimeClips();
    return true;
}

uint64 ModelAssetFbxTestAccess::ParseOptionsHash(const ModelAsset& asset)
{
    return asset.m_ParseOptionsHash;
}

bool ModelAssetFbxTestAccess::LoadBlend(ModelAsset& asset, const Vector<uint8>& data, const FbxLoaderOptions& options)
{
    const bool publishClipReplacements = asset.m_SkeletonId != 0;
    asset.m_Format = ModelFormat::BLEND;
    asset.m_SourceContentHash = ComputeLodSourceHash(data.data(), data.size());
    asset.m_SubassetDeriveKeys.clear();
    asset.m_HasAnimations = false;
    asset.m_AnimationNames.clear();
    asset.m_EmbeddedClipGuids.clear();
    if (!asset.LoadFromBlendData(data, asset.GetPath(), options))
    {
        asset.SetState(AssetState::Failed);
        return false;
    }
    ConsumeLodSuffixFamilies(asset.m_Meshes);
    asset.CalculateBoundingBox();
    asset.CalculateCounts();
    asset.SetState(AssetState::Loaded);
    asset.PersistSubassetDeriveKeys();
    if (publishClipReplacements)
        asset.PublishPendingRuntimeClips();
    return true;
}

namespace {

// Return the sibling sidecar path for a model: `<dir>/<stem>.humanoidrig.json`.
// e.g. `.../BusinessMale.fbx` -> `.../BusinessMale.humanoidrig.json`.
std::filesystem::path SidecarPathForModel(const std::filesystem::path& modelPath)
{
    std::filesystem::path p = modelPath;
    p.replace_extension(); // strip .fbx / .glb / etc.
    p += ".humanoidrig.json";
    return p;
}

// Missing, auto:true, or empty boneMap may be overwritten or deleted.
// auto:false with mapped bones is preserved.
bool SidecarIsReplaceable(const std::filesystem::path& sidecar, bool& outExists)
{
    std::error_code ec;
    outExists = std::filesystem::exists(sidecar, ec) && !ec;
    if (!outExists) return true;

    String text;
    if (!ReadFileTextShared(sidecar, text)) return false;

    try {
        nlohmann::json doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (doc.is_discarded()) return false;
        const auto autoIt = doc.find("auto");
        if (autoIt != doc.end() && autoIt->is_boolean() && autoIt->get<bool>())
            return true;
        const auto mapIt = doc.find("boneMap");
        return mapIt != doc.end() && mapIt->is_array() && mapIt->empty();
    } catch (...) {
        return false;
    }
}

void TryRemoveReplaceableSidecar(const std::filesystem::path& sidecar, std::string_view modelName)
{
    bool exists = false;
    if (!SidecarIsReplaceable(sidecar, exists) || !exists)
        return;
    std::error_code ec;
    std::filesystem::remove(sidecar, ec);
    if (!ec)
        Logger::Log::Info("ModelAsset[{}]: removed leftover humanoid sidecar '{}'",
                          modelName, sidecar.string());
}

std::string HexU64(uint64 value)
{
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(value));
    return std::string(buffer, 16);
}

// Fold the explicit-slot source identity into the LOD config hash. Folded only
// when non-zero (no slots), so the key is byte-identical to pre-C2a for every
// model without slots — the same fold-when-present discipline ComputeLodConfigHash
// uses for the parse-options hash, keeping the config-hash half of the key the
// single carrier of external authored-source identity.
uint64 FoldSlotSourceIntoConfigHash(uint64 configHash, uint64 slotSourceHash)
{
    if (slotSourceHash == 0)
        return configHash;
    constexpr uint64 kFnvPrime = 1099511628211ull;
    const auto* bytes = reinterpret_cast<const uint8*>(&slotSourceHash);
    for (int i = 0; i < 8; ++i) { configHash ^= bytes[i]; configHash *= kFnvPrime; }
    return configHash;
}

// Editor iteration cache file for a model's cooked LODs. Loose files under the
// owning source's derived-artifact cache root, name-encoded with the full key
// so a source edit, config change, or meshopt/generator bump lands on a fresh
// name (= miss, and the stale file is swept by the cache-GC / user-deletable by
// contract). Sources that permit derived-artifact cooking use their host-owned
// cache, including immutable development packages. Shipped Player mounts
// consume their baked .lod files instead. The policy and the cache root are
// both resolved from the GUID, so they always name the same owning source.
std::optional<std::filesystem::path> EditorLodCacheFile(
    AssetManager* assetManager, const GUID& guid, const LodCacheKey& key)
{
    if (!assetManager || !assetManager->GetRegistry().GetDerivedArtifactPolicy(guid).CooksOnMiss)
        return std::nullopt;
    std::optional<std::filesystem::path> cacheRoot =
        assetManager->GetRegistry().TryGetCacheRoot(guid);
    if (!cacheRoot)
        return std::nullopt;
    const std::string name = guid.ToString() + "-" + HexU64(key.SourceHash) + "-" +
                             HexU64(key.ConfigHash) + ".gelod";
    return *cacheRoot / "Lod" / name;
}

// Packaged-Player cooked-LOD file: <mount root>/.lod/<guid>.gelod, i.e. a sibling
// tree of the .assetmanifest, addressed by GUID (the build bakes the key into the
// header; the Player never recomputes a fingerprint). Empty when no AssetManager
// / mount root is available.
std::filesystem::path PlayerLodCacheFile(AssetManager* assetManager, const GUID& guid)
{
    if (!assetManager)
        return {};
    const std::filesystem::path root = assetManager->GetAssetRoot();
    if (root.empty())
        return {};
    return root / ".lod" / (guid.ToString() + ".gelod");
}

} // namespace

uint32 ModelAsset::GenerateLODs(const MeshLODConfig& config, bool generateSkinned) {
    const auto lodStart = std::chrono::steady_clock::now();
    uint32 maxLods = 1;
    size_t sourceTris = 0;
    MeshLODGenStats stats;
    for (Mesh& mesh : m_Meshes) {
        // Per-submesh granularity for a cancelled load; GenerateMeshLODs polls
        // again per level. Callers that persist the result must re-read the flag
        // before adopting it — the chain left here is short, not complete.
        if (IsAssetDecodeCancelled())
            break;
        sourceTris += mesh.Indices.size() / 3;
        if (mesh.HasAuthoredLODs()) {
            // Authored-wins (amendment #2a): an artist-supplied chain owns this
            // submesh. Skip generation and, unlike the skinned-skip below, leave
            // its parse-supplied ExtraLODs / ExtraLODVertices / ExtraLODCoverage
            // intact — clearing them would destroy the authored geometry.
            maxLods = std::max(maxLods, mesh.LODCount());
            continue;
        }
        if (!ShouldGenerateLODsForMesh(mesh, generateSkinned)) {
            // Skinned mesh skipped: drop any stale LODs so a regenerate after
            // toggling the option off leaves the mesh at LOD0-only.
            mesh.ExtraLODs.clear();
            mesh.ExtraLODErrors.clear();
            mesh.ExtraLODSloppy.clear();
            mesh.ExtraLODVertices.clear();
            mesh.ExtraLODColor0.clear();
            maxLods = std::max(maxLods, mesh.LODCount());
            continue;
        }
        maxLods = std::max(maxLods, GenerateMeshLODsInto(mesh, config, &stats));
    }
    CalculateBoundingBox(); // also covers the Editor's direct regeneration path
    const double lodMs = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - lodStart)
                             .count();
    Logger::Log::Info(
        "LOD gen: {} meshes -> {} LODs in {:.2f} ms ({} tris, {} shells refused, "
        "{} stream-skipped, {} lock-skipped) ({})",
        m_Meshes.size(), maxLods, lodMs, sourceTris, stats.ShellsRejected,
        stats.ShellsSkippedForStreams, stats.LevelsSkippedWithLocks, GetPath().string());
    return maxLods;
}

uint64 ModelAsset::CookLODCache(const MeshLODConfig& config, bool generateSkinned,
                                const std::filesystem::path& outFile) {
    GenerateLODs(config, generateSkinned);

    bool anyLods = false;
    for (const Mesh& mesh : m_Meshes) {
        if (!mesh.ExtraLODs.empty()) {
            anyLods = true;
            break;
        }
    }
    // No submesh reduced (all skinned-and-skipped, or too small): ship no
    // sidecar. The Player then stays LOD0-only for this model, same as a miss.
    if (!anyLods)
        return 0;

    const LodCacheKey key{m_SourceContentHash,
                          FoldSlotSourceIntoConfigHash(
                              ComputeLodConfigHash(config, generateSkinned, m_ParseOptionsHash),
                              m_AuthoredSlotSourceHash)};
    if (!WriteLodCache(outFile, key, ComputeGeneratedLodHash(m_Meshes), m_Meshes))
        return 0;

    std::error_code ec;
    const std::uintmax_t bytes = std::filesystem::file_size(outFile, ec);
    return ec ? 0u : static_cast<uint64>(bytes);
}

void ModelAsset::LoadOrGenerateLODs(AssetManager* assetManager,
                                    const MeshLODConfig& config, bool generateSkinned) {
    // configHash folds the LOD config, generateSkinned, the meshopt/generator
    // versions, AND the resolved parse-options hash (F1): FBX geometry depends on
    // loader options, so a cook keyed only on {source, LOD config} could be
    // consumed against differently-parsed geometry. Baking the options into the
    // key makes a parse-option change a clean miss.
    const LodCacheKey key{m_SourceContentHash,
                          FoldSlotSourceIntoConfigHash(
                              ComputeLodConfigHash(config, generateSkinned, m_ParseOptionsHash),
                              m_AuthoredSlotSourceHash)};

    // A cooked file whose bytes fail validation (truncated, or parsed geometry
    // disagrees with the cook) is a cache DEFEAT worth surfacing — as opposed to
    // a benign miss (file absent, or a name/format that predates a key change).
    const auto logRejection = [this](LodCacheStatus status, const std::filesystem::path& file) {
        if (status == LodCacheStatus::Corrupt || status == LodCacheStatus::StructureMismatch)
            Logger::Log::Warning("ModelAsset[{}]: cooked LODs rejected ({}) — regenerating: {}",
                                 GetName(),
                                 status == LodCacheStatus::Corrupt ? "corrupt" : "geometry mismatch",
                                 file.generic_string());
    };

    // Editor iteration cache: name-encoded key means a source/config/version/
    // parse-option change is a fresh filename (= miss). On-read bounds validation
    // guards the apply; a hit skips meshopt entirely.
    if (std::optional<std::filesystem::path> editorFile =
            EditorLodCacheFile(assetManager, GetGUID(), key)) {
        const LodCacheStatus status = ReadLodCacheInto(*editorFile, &key, m_Meshes);
        if (status == LodCacheStatus::Hit) {
            CalculateBoundingBox();
            return;
        }
        logRejection(status, *editorFile);
        GenerateLODs(config, generateSkinned);
        // A cancelled generation stops partway, so the chain is short by design.
        // Caching it would key a TRUNCATED chain to the full config and serve it
        // as a hit on every later load — a permanent silent LOD loss.
        if (IsAssetDecodeCancelled())
            return;
        WriteLodCache(*editorFile, key, ComputeGeneratedLodHash(m_Meshes), m_Meshes);
        return;
    }

    // Packaged Player: the build bakes a .gelod beside the manifest, keyed in the
    // header and validated by format + geometry + index bounds (null key — the
    // Player cannot recompute a fingerprint). A hit ships LODs with meshopt
    // compiled out.
    const std::filesystem::path playerFile = PlayerLodCacheFile(assetManager, GetGUID());
    if (!playerFile.empty()) {
        const LodCacheStatus status = ReadLodCacheInto(playerFile, nullptr, m_Meshes);
        if (status == LodCacheStatus::Hit) {
            CalculateBoundingBox();
            return;
        }
        logRejection(status, playerFile);
        if (status == LodCacheStatus::Missing)
            Logger::Log::Info("ModelAsset[{}]: cooked LODs unavailable — generating at runtime",
                              GetName());
    }

    // No cooked file (absent / invalid / no mount): fall back to runtime
    // generation, the Phase A behavior.
    GenerateLODs(config, generateSkinned);
}

void ModelAsset::PostLoad() {
    // Resolve this asset's LOD policy: per-asset kv (assets.lod.*) layered over
    // the process-global default tier. GetThreadCurrent() is worker-safe (this
    // runs on a Background job on both initial load and hot-reload); a null
    // manager (raw test harness) resolves the global tier alone.
    AssetManager* assetMgr = AssetManager::GetThreadCurrent();

    // Explicit per-asset LOD slots resolve BEFORE generation so authored-wins
    // skips the slot-authored submeshes, and BEFORE the cache key is built so the
    // slot identity is folded in. Independent of the generate policy — an artist
    // assembling a chain by hand does not need auto-generation on.
    const LodSlotResolution slots =
        ResolveExplicitLodSlots(m_Meshes, assetMgr, GetPath(), GetGUID());
    m_AuthoredSlotSourceHash = slots.SourceHash;

    const ResolvedLodSettings lod = ResolveLodSettings(assetMgr, GetPath());
    if (lod.Generate)
        LoadOrGenerateLODs(assetMgr, lod.Config, lod.GenerateSkinned);
    else
        CalculateBoundingBox(); // explicit slots also apply with generation disabled

    // Footprint demotion of merge-inferred Mask must see the FINAL LOD set:
    // authored-slot and generated/cooked levels appended above draw triangles
    // the load-time mesh set never covered. Runs on both initial load and
    // hot-reload (idempotent — a demoted material is no longer Mask).
    DemoteInferredMaskMaterials();

    // Record the final materials (after the demotion above, so the record matches
    // what registration converts) in the derived cache, where consumers that need
    // them without importing the model read them. Unchanged materials write nothing.
    if (assetMgr)
    {
        if (const auto file = ImportedMaterialCacheFile(assetMgr->GetRegistry(), GetPath(), GetGUID()))
            WriteImportedMaterialCache(*file, m_Materials);
    }

    // Auto-import HumanoidRig sidecar for skinned models. The sidecar is
    // what HumanoidRetargetSystem ingests as the source / target rig — the
    // ModelAsset itself never carries humanoid metadata. Skip cleanly for
    // non-skinned models. assets.model.rigKind=none opts the file out.
    if (m_SkeletonId == 0) return;

    auto& skStore = ::GameEngine::Engine::Renderer::SkeletonStore::Instance();
    const auto* skel = skStore.Get(m_SkeletonId);
    if (!skel || skel->BoneCount == 0) return;

    AssetManager* assetManager = AssetManager::GetThreadCurrent();
    if (!assetManager) {
        // Out of asset-loading context (e.g. raw test harness loading a model
        // without an AssetManager). Skip silently — the integration test
        // harness wires AssetManager itself when it wants the sidecar.
        return;
    }

    const ModelAssetSettings modelSettings = LoadModelAssetSettings(assetManager, GetPath());
    if (modelSettings.RigKind == ModelRigKind::NotHumanoid)
    {
        TryRemoveReplaceableSidecar(SidecarPathForModel(GetPath()), GetName());
        return;
    }

    const auto profile = LoadRuntimeHumanoidProfile(*assetManager);
    if (!profile)
    {
        Logger::Log::Warning("ModelAsset[{}]: cannot load '{}' - humanoid sidecar not generated",
                             GetName(), kRuntimeHumanoidProfilePath);
        return;
    }
    const GUID profileGuid = profile->GetGUID();

    Animation::HumanoidRig rig(GUID(), std::filesystem::path{});
    if (!Animation::AutoImportHumanoidRig(*skel, *profile, rig)) {
        Logger::Log::Debug("ModelAsset[{}]: humanoid auto-import skipped", GetName());
        TryRemoveReplaceableSidecar(SidecarPathForModel(GetPath()), GetName());
        return;
    }
    rig.SetAutoGenerated(true);
    rig.SetProfileRef(profileGuid);

    const std::filesystem::path sidecar = SidecarPathForModel(GetPath());
    bool sidecarExists = false;
    const bool canWrite = SidecarIsReplaceable(sidecar, sidecarExists);
    if (sidecarExists && !canWrite) {
        Logger::Log::Info(
            "ModelAsset[{}]: existing humanoid rig at '{}' is user-edited (auto=false); preserving",
            GetName(), sidecar.string());
        return;
    }

    // On one platform every import of an unchanged model derives the same
    // sidecar bytes (another platform's float formatting differs in the last
    // digits, so its sidecar is rewritten once: #2383). Rewriting identical
    // bytes would only report a change to the file watcher (the Assets panel
    // lists the folder again) and touch a committed file.
    Vector<uint8> derived;
    Vector<uint8> existing;
    const bool unchanged = sidecarExists && rig.SaveToData(derived) &&
                           ReadFileBytesShared(sidecar, existing) && existing == derived;
    if (!unchanged)
    {
        if (!rig.SaveToPath(sidecar)) {
            Logger::Log::Error(
                "ModelAsset[{}]: failed to write humanoid sidecar '{}'",
                GetName(), sidecar.string());
            return;
        }

        Logger::Log::Info(
            "ModelAsset[{}]: HumanoidRig auto-imported via heuristic ({} chains, {} bones mapped) -> {}",
            GetName(), rig.Chains().size(), rig.BoneMap().size(), sidecar.string());
    }

    // Register the new sidecar with the AssetManager so future GUID-based
    // resolution finds it without waiting for a filesystem rescan. Best-
    // effort: a missing source (e.g. raw test harness) just means the next
    // scan will pick it up.
    assetManager->ResolveAssetGuid(sidecar);
}

static void CopySkeletonStoreEntry(uint32 dstId, uint32 srcId)
{
    if (dstId == 0 || srcId == 0 || dstId == srcId)
        return;
    auto& store = Engine::Renderer::SkeletonStore::Instance();
    const Animation::SkeletonData* src = store.Get(srcId);
    if (!src)
        return;
    store.EnsureSizes(dstId, src->BoneCount);
    Animation::SkeletonData* dst = store.Get(dstId);
    if (!dst)
        return;
    *dst = *src;
}

bool ModelAsset::AdoptReloadedPayload(Asset& staged) {
    auto* other = dynamic_cast<ModelAsset*>(&staged);
    if (!other || !other->IsLoaded()) {
        return false;
    }

    // Swap exactly the members Unload() resets plus the loader/PostLoad
    // outputs (format, counts, hashes, bounds). The staged instance already
    // ran the full worker-side pipeline — parse, LOD resolve, skeleton/clip
    // registration — so adoption is member swaps plus a skeleton copy:
    // staged Load() mints a new SkeletonStore id, and live SkeletonRefs
    // still name this instance's id, so rest/IBM must land in that slot.
    const uint32 liveSkeletonId = m_SkeletonId;
    const uint32 stagedSkeletonId = other->m_SkeletonId;
    std::swap(m_Format, other->m_Format);
    std::swap(m_Meshes, other->m_Meshes);
    std::swap(m_Materials, other->m_Materials);
    std::swap(m_EmbeddedImages, other->m_EmbeddedImages);
    std::swap(m_Cameras, other->m_Cameras);
    std::swap(m_Lights, other->m_Lights);
    std::swap(m_SceneNodes, other->m_SceneNodes);
    std::swap(m_Extras, other->m_Extras);
    std::swap(m_TotalVertexCount, other->m_TotalVertexCount);
    std::swap(m_TotalIndexCount, other->m_TotalIndexCount);
    std::swap(m_SourceContentHash, other->m_SourceContentHash);
    std::swap(m_ParseOptionsHash, other->m_ParseOptionsHash);
    std::swap(m_AuthoredSlotSourceHash, other->m_AuthoredSlotSourceHash);
    std::swap(m_HasAnimations, other->m_HasAnimations);
    std::swap(m_AnimationNames, other->m_AnimationNames);
    std::swap(m_EmbeddedClipGuids, other->m_EmbeddedClipGuids);
    std::swap(m_PendingRuntimeClips, other->m_PendingRuntimeClips);
    std::swap(m_SubassetDeriveKeys, other->m_SubassetDeriveKeys);
    for (int i = 0; i < 3; ++i) {
        std::swap(m_MinBounds[i], other->m_MinBounds[i]);
        std::swap(m_MaxBounds[i], other->m_MaxBounds[i]);
    }
    if (liveSkeletonId != 0)
    {
        m_SkeletonId = liveSkeletonId;
        CopySkeletonStoreEntry(liveSkeletonId, stagedSkeletonId);
    }
    else
    {
        m_SkeletonId = stagedSkeletonId;
        other->m_SkeletonId = 0;
    }
    PublishPendingRuntimeClips();
    return true;
}

void ModelAsset::Unload() {
    m_Meshes.clear();
    m_Materials.clear();
    m_EmbeddedImages.clear();
    m_Cameras.clear();
    m_Lights.clear();
    m_SceneNodes.clear();
    m_Extras = {};
    m_TotalVertexCount = 0;
    m_TotalIndexCount = 0;
    m_HasAnimations = false;
    m_AnimationNames.clear();
    m_EmbeddedClipGuids.clear();
    m_PendingRuntimeClips.clear();
    m_SubassetDeriveKeys.clear();
    // Keep m_SkeletonId. Reload() unloads and reloads this same instance;
    // RetainOrCreateSkeleton refills the same store slot so live SkeletonRefs
    // still skin the re-parsed rest/IBM. The store entry is not removed
    // (same as before this keep).

    // Reset bounding box
    m_MinBounds[0] = m_MinBounds[1] = m_MinBounds[2] = std::numeric_limits<float>::max();
    m_MaxBounds[0] = m_MaxBounds[1] = m_MaxBounds[2] = std::numeric_limits<float>::lowest();

    SetState(AssetState::Unloaded);
    Logger::Log::Debug("Model unloaded: {}", GetName());
}

bool ModelAsset::LoadOBJ(const Vector<uint8>& data) {
#if defined(GE_HAVE_TINYOBJ)
    // Use tinyobjloader when available for robustness and speed
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    std::string content(reinterpret_cast<const char*>(data.data()), data.size());
    std::istringstream iss(content);
    bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, &iss, /*mtl_basepath*/nullptr, /*triangulate*/true);

    if (!warn.empty()) Logger::Log::Warning("tinyobj warn: {}", warn);
    if (!ret) {
        Logger::Log::Error("tinyobj error: {}", err);
        return false;
    }

    // Build one mesh per shape
    for (const auto& shape : shapes) {
        Mesh mesh;
        mesh.Name = shape.name.empty() ? GetName() : shape.name;
        mesh.MaterialIndex = 0; // Simple: single default material for now
        mesh.MinBounds[0] = mesh.MinBounds[1] = mesh.MinBounds[2] = std::numeric_limits<float>::max();
        mesh.MaxBounds[0] = mesh.MaxBounds[1] = mesh.MaxBounds[2] = std::numeric_limits<float>::lowest();

        // Iterate indices
        for (const auto& idx : shape.mesh.Indices) {
            Vertex v{};
            if (idx.vertex_index >= 0) {
                v.Position[0] = attrib.vertices[3 * size_t(idx.vertex_index) + 0];
                v.Position[1] = attrib.vertices[3 * size_t(idx.vertex_index) + 1];
                v.Position[2] = attrib.vertices[3 * size_t(idx.vertex_index) + 2];
                for (int i = 0; i < 3; ++i) {
                    mesh.MinBounds[i] = std::min(mesh.MinBounds[i], v.Position[i]);
                    mesh.MaxBounds[i] = std::max(mesh.MaxBounds[i], v.Position[i]);
                }
            }
            if (idx.normal_index >= 0) {
                v.Normal[0] = attrib.normals[3 * size_t(idx.normal_index) + 0];
                v.Normal[1] = attrib.normals[3 * size_t(idx.normal_index) + 1];
                v.Normal[2] = attrib.normals[3 * size_t(idx.normal_index) + 2];
            }
            if (idx.texcoord_index >= 0) {
                v.TexCoords[0] = attrib.texcoords[2 * size_t(idx.texcoord_index) + 0];
                v.TexCoords[1] = attrib.texcoords[2 * size_t(idx.texcoord_index) + 1];
            }
            mesh.Indices.push_back(static_cast<uint32>(mesh.Vertices.size()));
            mesh.Vertices.push_back(v);
        }

        if (!mesh.Vertices.empty() && !mesh.Indices.empty()) {
            m_Meshes.push_back(std::move(mesh));
        }
    }

    // Simple default material
    ImportedMaterialData defaultMaterial{}; defaultMaterial.Name = "DefaultMaterial";
    defaultMaterial.DiffuseColor[0] = defaultMaterial.DiffuseColor[1] = defaultMaterial.DiffuseColor[2] = 0.8f;
    defaultMaterial.DiffuseColor[3] = 1.0f;
    defaultMaterial.SpecularColor[0] = defaultMaterial.SpecularColor[1] = defaultMaterial.SpecularColor[2] = 0.2f;
    defaultMaterial.Shininess = 32.0f; defaultMaterial.Metallic = 0.0f; defaultMaterial.Roughness = 0.5f;
    m_Materials.push_back(defaultMaterial);

    Logger::Log::Info("OBJ loaded via tinyobj: meshes={} totalVerts={}", m_Meshes.size(),
                      m_Meshes.empty()?0: m_Meshes[0].Vertices.size());
    return !m_Meshes.empty();
#else
    // Fallback to simple parser
    String content(data.begin(), data.end());
    std::istringstream stream(content);
    String line;

    Vector<float> positions;
    Vector<float> normals;
    Vector<float> texCoords;

    Mesh mesh;
    mesh.Name = GetName();
    mesh.MaterialIndex = 0;
    mesh.MinBounds[0] = mesh.MinBounds[1] = mesh.MinBounds[2] = std::numeric_limits<float>::max();
    mesh.MaxBounds[0] = mesh.MaxBounds[1] = mesh.MaxBounds[2] = std::numeric_limits<float>::lowest();

    while (std::getline(stream, line)) {
        std::istringstream lineStream(line);
        String prefix; lineStream >> prefix;
        if (prefix == "v") {
            float x,y,z; lineStream >> x >> y >> z; positions.insert(positions.end(), {x,y,z});
        } else if (prefix == "vn") {
            float nx,ny,nz; lineStream >> nx >> ny >> nz; normals.insert(normals.end(), {nx,ny,nz});
        } else if (prefix == "vt") {
            float u,v; lineStream >> u >> v; texCoords.insert(texCoords.end(), {u,v});
        } else if (prefix == "f") {
            String a,b,c; lineStream >> a >> b >> c;
            auto parse = [&](const String& s)->uint32 {
                std::istringstream vs(s); String pi,ti,ni; std::getline(vs,pi,'/'); std::getline(vs,ti,'/'); std::getline(vs,ni,'/');
                int p = pi.empty()? -1 : std::stoi(pi)-1; if (p<0 || (p*3+2)>= (int)positions.size()) return UINT32_MAX;
                Vertex v{}; v.Position[0]=positions[p*3+0]; v.Position[1]=positions[p*3+1]; v.Position[2]=positions[p*3+2];
                for(int i=0;i<3;++i){ mesh.MinBounds[i]=std::min(mesh.MinBounds[i], v.Position[i]); mesh.MaxBounds[i]=std::max(mesh.MaxBounds[i], v.Position[i]); }
                if(!ti.empty()){ int t=std::stoi(ti)-1; if(t>=0 && (t*2+1)<(int)texCoords.size()){ v.TexCoords[0]=texCoords[t*2+0]; v.TexCoords[1]=texCoords[t*2+1]; }}
                if(!ni.empty()){ int n=std::stoi(ni)-1; if(n>=0 && (n*3+2)<(int)normals.size()){ v.Normal[0]=normals[n*3+0]; v.Normal[1]=normals[n*3+1]; v.Normal[2]=normals[n*3+2]; }}
                mesh.Vertices.push_back(v); return (uint32)mesh.Vertices.size()-1; };
            uint32 i0=parse(a), i1=parse(b), i2=parse(c);
            if(i0!=UINT32_MAX && i1!=UINT32_MAX && i2!=UINT32_MAX){ mesh.Indices.push_back(i0); mesh.Indices.push_back(i1); mesh.Indices.push_back(i2);}        }
    }

    if (!mesh.Vertices.empty() && !mesh.Indices.empty()) {
        m_Meshes.push_back(std::move(mesh));
        ImportedMaterialData defaultMaterial{}; defaultMaterial.Name = "DefaultMaterial";
        defaultMaterial.DiffuseColor[0]=defaultMaterial.DiffuseColor[1]=defaultMaterial.DiffuseColor[2]=0.8f; defaultMaterial.DiffuseColor[3]=1.0f;
        defaultMaterial.SpecularColor[0]=defaultMaterial.SpecularColor[1]=defaultMaterial.SpecularColor[2]=0.2f; defaultMaterial.Shininess=32.0f; defaultMaterial.Metallic=0.0f; defaultMaterial.Roughness=0.5f;
        m_Materials.push_back(defaultMaterial);
        Logger::Log::Debug("OBJ loaded (fallback): {} verts, {} idx", m_Meshes[0].Vertices.size(), m_Meshes[0].Indices.size());
        return true;
    }
    Logger::Log::Error("Failed to parse OBJ file: no valid geometry found");
    return false;
#endif
}

ModelFormat ModelAsset::DetermineFormat(const String& extension) {
    if (extension == ".obj") return ModelFormat::OBJ;
    if (extension == ".fbx") return ModelFormat::FBX;
    if (extension == ".gltf") return ModelFormat::GLTF;
    if (extension == ".glb") return ModelFormat::GLB;
    if (extension == ".blend") return ModelFormat::BLEND;
    return ModelFormat::Unknown;
}

ModelFormat ModelAsset::FormatFromPath(const std::filesystem::path& path)
{
    String extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return DetermineFormat(extension);
}

bool ModelAsset::UsesAxisImportOptions(ModelFormat format)
{
    switch (format)
    {
    case ModelFormat::FBX:
    case ModelFormat::GLTF:
    case ModelFormat::GLB:
    case ModelFormat::BLEND:
        return true;
    default:
        return false;
    }
}

// Grows [boundsMin, boundsMax] by the 8 corners of the box [minimum, maximum]
// transformed by the column-major matrix m.
static void ExpandByTransformedBox(const float minimum[3], const float maximum[3], const float* m,
                                   float boundsMin[3], float boundsMax[3])
{
    for (int cz = 0; cz < 2; ++cz)
        for (int cy = 0; cy < 2; ++cy)
            for (int cx = 0; cx < 2; ++cx)
            {
                const float px = cx ? maximum[0] : minimum[0];
                const float py = cy ? maximum[1] : minimum[1];
                const float pz = cz ? maximum[2] : minimum[2];
                const float tx = m[0]*px + m[4]*py + m[8]*pz  + m[12];
                const float ty = m[1]*px + m[5]*py + m[9]*pz  + m[13];
                const float tz = m[2]*px + m[6]*py + m[10]*pz + m[14];
                boundsMin[0] = std::min(boundsMin[0], tx);
                boundsMin[1] = std::min(boundsMin[1], ty);
                boundsMin[2] = std::min(boundsMin[2], tz);
                boundsMax[0] = std::max(boundsMax[0], tx);
                boundsMax[1] = std::max(boundsMax[1], ty);
                boundsMax[2] = std::max(boundsMax[2], tz);
            }
}

void ModelAsset::CalculateBoundingBox() {
    m_MinBounds[0] = m_MinBounds[1] = m_MinBounds[2] = std::numeric_limits<float>::max();
    m_MaxBounds[0] = m_MaxBounds[1] = m_MaxBounds[2] = std::numeric_limits<float>::lowest();

    bool hasBounds = false;
    for (const auto& mesh : m_Meshes) {
        const MeshLODGeometry geometry = ResolveMeshLODGeometry(mesh);
        if (geometry.LevelCount == 0) continue;
        hasBounds = true;
        const auto bounds = geometry.Bounds.ToAABB();
        const float minimum[3] = {bounds.min.x, bounds.min.y, bounds.min.z};
        const float maximum[3] = {bounds.max.x, bounds.max.y, bounds.max.z};
        if (mesh.SourceNodeIndex >= 0)
        {
            // The mesh's box at each node that draws it, so the aggregate
            // bounding box accounts for submesh placement within the model.
            ExpandByTransformedBox(minimum, maximum, mesh.SourceNodeTransform, m_MinBounds, m_MaxBounds);
            for (const MeshPlacement& placement : mesh.ExtraPlacements)
                ExpandByTransformedBox(minimum, maximum, placement.SourceNodeTransform, m_MinBounds, m_MaxBounds);
        }
        else
        {
            for (int i = 0; i < 3; ++i) {
                m_MinBounds[i] = std::min(m_MinBounds[i], minimum[i]);
                m_MaxBounds[i] = std::max(m_MaxBounds[i], maximum[i]);
            }
        }
    }
    if (!hasBounds) {
        std::fill_n(m_MinBounds, 3, 0.0f);
        std::fill_n(m_MaxBounds, 3, 0.0f);
    }
}

void ModelAsset::CalculateCounts() {
    m_TotalVertexCount = 0;
    m_TotalIndexCount = 0;

    for (const auto& mesh : m_Meshes) {
        m_TotalVertexCount += static_cast<uint32>(mesh.Vertices.size());
        m_TotalIndexCount += static_cast<uint32>(mesh.Indices.size());
    }
}

void ModelAsset::GetBoundingBox(float minBounds[3], float maxBounds[3]) const {
    for (int i = 0; i < 3; ++i) {
        minBounds[i] = m_MinBounds[i];
        maxBounds[i] = m_MaxBounds[i];
    }
}

} // namespace GameEngine
