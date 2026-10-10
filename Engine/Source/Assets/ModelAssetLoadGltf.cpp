#include "ModelAssetLoadGltf.h"
#include "Assets/ModelAsset.h"
#include "AssetCore/SharedFileRead.h"
#include "Assets/AnimationClip.h"
#include "Assets/AssetManager.h"
#include "Assets/AuthoredLodImport.h"
#include "Assets/FbxLoaderOptions.h"
#include "Assets/ModelExtras.h"
#include "Assets/Textures/TextureMimeType.h"
#include "Core/Engine.h"
#include "GltfAccessorUnpack.h"
#include "GltfAllocationBudget.h"
#include "GltfAxisConversion.h"
#include "Logger/Logger.h"
#include "ModelAxisConversion.h"
#include "SparseMorphTarget.h"
#include "SubmeshElementLimits.h"
#include "Types/Fnv1a.h"
#include "Types/StringUtils.h"

#include "ECSModules/Rendering/SkeletonStore.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(GE_HAVE_CGLTF)
  #include <cgltf.h>
#endif

namespace GameEngine {

// A material texture reference for image `idx`: the registered asset's GUID
// when the image is one, else the "__embedded:<index>" form the texture
// service decodes from the model's own bytes.
static std::string ImageTextureRef(const Vector<EmbeddedImage>& images, int idx)
{
    if (idx < 0)
        return {};
    if (static_cast<size_t>(idx) < images.size() && !images[idx].AssetGuid.IsNull())
        return images[idx].AssetGuid.ToString();
    return kEmbeddedTexturePrefix + std::to_string(idx);
}

static void AssignEmbeddedTextureRefs(ImportedMaterialData& mat, const Vector<EmbeddedImage>& images,
                                       int albedoIdx, int normalIdx, int metalRoughIdx, int emissiveIdx,
                                       int occlusionIdx)
{
    if (albedoIdx >= 0)
        mat.DiffuseTexture = ImageTextureRef(images, albedoIdx);
    if (normalIdx >= 0)
        mat.NormalTexture = ImageTextureRef(images, normalIdx);
    if (metalRoughIdx >= 0)
        mat.SpecularTexture = ImageTextureRef(images, metalRoughIdx);
    if (emissiveIdx >= 0)
        mat.EmissiveTexture = ImageTextureRef(images, emissiveIdx);
    // Occlusion reads the image's R channel, so a file that packs occlusion, roughness and
    // metalness into one image (ORM) names that image here and for metal-roughness.
    if (occlusionIdx >= 0)
        mat.OcclusionTexture = ImageTextureRef(images, occlusionIdx);
}

// Merge static (non-skinned) primitives that share the same MaterialIndex into
// a single Mesh, transforming each source primitive's vertices by its
// SourceNodeTransform so the result is in model-local space. This replaces
// many tiny draws with one larger draw per material — critical for assets
// like architectural / city dumps with thousands of primitives sharing a
// small material set.
//
// Skinned meshes are kept untouched (they animate via the bone hierarchy).
// Caller must gate this on "model has no animations" — otherwise non-skinned
// meshes with SourceNodeIndex >= 0 may follow bones via AnimatedNodeRef and
// merging would freeze them in place.
//
// Below kMergeMinPrimitives we don't attempt merging (small models gain
// nothing and lose per-primitive picking granularity). Above that, we still
// require the merge to halve the draw count (kMergeMinReductionFactor) —
// otherwise the per-primitive material variation is too high for merging
// to pay off.
static constexpr size_t kMergeMinPrimitives = 32;
static constexpr size_t kMergeMinReductionFactor = 2;

static void MergeStaticPrimitivesByMaterial(Vector<Mesh>& meshes)
{
    if (meshes.size() < kMergeMinPrimitives)
        return;

    // Partition: skinned stays untouched; non-skinned is candidate for merging.
    Vector<Mesh> skinned;
    Vector<Mesh> staticMeshes;
    skinned.reserve(meshes.size());
    staticMeshes.reserve(meshes.size());
    for (auto& m : meshes)
    {
        // Authored / own-vertex LOD chains stay untouched alongside skinned/
        // morph/non-triangle meshes: the merge concatenates only LOD0 geometry,
        // so merging such a submesh would silently drop its
        // ExtraLODVertices/ExtraLODs chain. A mesh drawn by several nodes stays
        // too: merging bakes one placement into the vertices.
        if (m.IsSkinned() || m.HasMorphTargets() || !m.ExtraPlacements.empty()
            || m.PrimitiveTopology != MeshPrimitiveTopology::Triangles
            || m.HasAuthoredLODs() || m.HasOwnVertexLODs())
            skinned.push_back(std::move(m));
        else
            staticMeshes.push_back(std::move(m));
    }
    meshes.clear();

    if (staticMeshes.size() < kMergeMinPrimitives)
    {
        // Below threshold; restore originals and bail.
        for (auto& m : skinned)       meshes.push_back(std::move(m));
        for (auto& m : staticMeshes)  meshes.push_back(std::move(m));
        return;
    }

    // Group by MaterialIndex. Bail if the win is small (>2x reduction required).
    std::unordered_map<uint32, Vector<size_t>> byMat;
    for (size_t i = 0; i < staticMeshes.size(); ++i)
        byMat[staticMeshes[i].MaterialIndex].push_back(i);

    if (staticMeshes.size() <= kMergeMinReductionFactor * byMat.size())
    {
        for (auto& m : skinned)       meshes.push_back(std::move(m));
        for (auto& m : staticMeshes)  meshes.push_back(std::move(m));
        return;
    }

    // Stable output order: by ascending MaterialIndex.
    Vector<uint32> matKeys;
    matKeys.reserve(byMat.size());
    for (auto& kv : byMat) matKeys.push_back(kv.first);
    std::sort(matKeys.begin(), matKeys.end());

    Vector<Mesh> merged;
    merged.reserve(matKeys.size() + skinned.size());

    size_t totalSrcVerts = 0;
    size_t totalSrcIndices = 0;
    for (const auto& m : staticMeshes)
    {
        totalSrcVerts += m.Vertices.size();
        totalSrcIndices += m.Indices.size();
    }

    for (uint32 matIdx : matKeys)
    {
        const auto& group = byMat[matIdx];
        if (group.empty())
            continue;

        // Single-primitive group: pass through unchanged.
        if (group.size() == 1)
        {
            merged.push_back(std::move(staticMeshes[group[0]]));
            continue;
        }

        Mesh out;
        out.Name = "merged_mat_" + std::to_string(matIdx);
        out.MaterialIndex = matIdx;
        out.SourceNodeIndex = -1;
        // Identity SourceNodeTransform (default).
        out.MinBounds[0] = out.MinBounds[1] = out.MinBounds[2] = std::numeric_limits<float>::max();
        out.MaxBounds[0] = out.MaxBounds[1] = out.MaxBounds[2] = std::numeric_limits<float>::lowest();

        size_t outVertCount = 0;
        size_t outIndexCount = 0;
        bool outHasColor0 = false;
        bool outHasTexCoords1 = false;
        size_t outExtraUvCount = 0;
        for (size_t gi : group)
        {
            outVertCount += staticMeshes[gi].Vertices.size();
            outIndexCount += staticMeshes[gi].Indices.size();
            outHasColor0 = outHasColor0 || staticMeshes[gi].HasColor0();
            outHasTexCoords1 = outHasTexCoords1 || staticMeshes[gi].HasTexCoords1();
            outExtraUvCount = std::max(outExtraUvCount, staticMeshes[gi].ExtraTexCoords.size());
        }
        out.Vertices.reserve(outVertCount);
        out.Indices.reserve(outIndexCount);
        if (outHasColor0)
            out.Color0.reserve(outVertCount * 4u);
        if (outHasTexCoords1)
            out.TexCoords1.reserve(outVertCount * 2u);
        if (outExtraUvCount > 0)
        {
            out.ExtraTexCoords.resize(outExtraUvCount);
            for (auto& uvSet : out.ExtraTexCoords)
                uvSet.reserve(outVertCount * 2u);
        }

        for (size_t gi : group)
        {
            Mesh& src = staticMeshes[gi];
            const uint32 vertexBase = static_cast<uint32>(out.Vertices.size());
            const bool srcHasColor0 = src.HasColor0();
            const bool srcHasTexCoords1 = src.HasTexCoords1();

            // Build transforms. SourceNodeTransform is column-major.
            const glm::mat4 M = glm::make_mat4(src.SourceNodeTransform);
            const glm::mat3 R(M);
            const glm::mat3 Rit = glm::transpose(glm::inverse(R));
            const float det = glm::determinant(R);
            const bool flipWinding = (det < 0.0f);

            const bool isIdentity =
                src.SourceNodeTransform[0]  == 1.0f && src.SourceNodeTransform[1]  == 0.0f &&
                src.SourceNodeTransform[2]  == 0.0f && src.SourceNodeTransform[3]  == 0.0f &&
                src.SourceNodeTransform[4]  == 0.0f && src.SourceNodeTransform[5]  == 1.0f &&
                src.SourceNodeTransform[6]  == 0.0f && src.SourceNodeTransform[7]  == 0.0f &&
                src.SourceNodeTransform[8]  == 0.0f && src.SourceNodeTransform[9]  == 0.0f &&
                src.SourceNodeTransform[10] == 1.0f && src.SourceNodeTransform[11] == 0.0f &&
                src.SourceNodeTransform[12] == 0.0f && src.SourceNodeTransform[13] == 0.0f &&
                src.SourceNodeTransform[14] == 0.0f && src.SourceNodeTransform[15] == 1.0f;

            for (size_t svi = 0; svi < src.Vertices.size(); ++svi)
            {
                const Vertex& sv = src.Vertices[svi];
                Vertex dv = sv;
                if (!isIdentity)
                {
                    const glm::vec4 p(sv.Position[0], sv.Position[1], sv.Position[2], 1.0f);
                    const glm::vec4 pT = M * p;
                    dv.Position[0] = pT.x;
                    dv.Position[1] = pT.y;
                    dv.Position[2] = pT.z;

                    const glm::vec3 n(sv.Normal[0], sv.Normal[1], sv.Normal[2]);
                    const glm::vec3 nT = Rit * n;
                    const float nLen = std::sqrt(nT.x * nT.x + nT.y * nT.y + nT.z * nT.z);
                    if (nLen > 1e-8f)
                    {
                        const float inv = 1.0f / nLen;
                        dv.Normal[0] = nT.x * inv;
                        dv.Normal[1] = nT.y * inv;
                        dv.Normal[2] = nT.z * inv;
                    }

                    const glm::vec3 t(sv.Tangent[0], sv.Tangent[1], sv.Tangent[2]);
                    const float tLen2 = t.x * t.x + t.y * t.y + t.z * t.z;
                    if (tLen2 > 1e-12f)
                    {
                        const glm::vec3 tT = R * t;
                        const float tLen = std::sqrt(tT.x * tT.x + tT.y * tT.y + tT.z * tT.z);
                        if (tLen > 1e-8f)
                        {
                            const float inv = 1.0f / tLen;
                            dv.Tangent[0] = tT.x * inv;
                            dv.Tangent[1] = tT.y * inv;
                            dv.Tangent[2] = tT.z * inv;
                        }
                    }

                    // Tangent.w is bitangent handedness. A mirrored transform
                    // (det(R) < 0) flips the cross-product orientation, so flip
                    // the stored handedness too to keep `cross(N, T) * w`
                    // pointing in the same physical direction.
                    dv.Tangent[3] = flipWinding ? -sv.Tangent[3] : sv.Tangent[3];
                }

                for (int c = 0; c < 3; ++c)
                {
                    out.MinBounds[c] = std::min(out.MinBounds[c], dv.Position[c]);
                    out.MaxBounds[c] = std::max(out.MaxBounds[c], dv.Position[c]);
                }
                out.Vertices.push_back(dv);
                if (outHasColor0)
                {
                    if (srcHasColor0)
                    {
                        const float* rgba = src.Color0.data() + svi * 4u;
                        out.Color0.insert(out.Color0.end(), rgba, rgba + 4u);
                    }
                    else
                    {
                        out.Color0.insert(out.Color0.end(), {1.0f, 1.0f, 1.0f, 1.0f});
                    }
                }
                if (outHasTexCoords1)
                {
                    if (srcHasTexCoords1)
                    {
                        const float* uv = src.TexCoords1.data() + svi * 2u;
                        out.TexCoords1.insert(out.TexCoords1.end(), uv, uv + 2u);
                    }
                    else
                    {
                        out.TexCoords1.insert(out.TexCoords1.end(), {0.0f, 0.0f});
                    }
                }
                for (size_t uvi = 0; uvi < outExtraUvCount; ++uvi)
                {
                    if (uvi < src.ExtraTexCoords.size()
                        && src.ExtraTexCoords[uvi].size() == src.Vertices.size() * 2u)
                    {
                        const float* uv = src.ExtraTexCoords[uvi].data() + svi * 2u;
                        out.ExtraTexCoords[uvi].insert(out.ExtraTexCoords[uvi].end(), uv, uv + 2u);
                    }
                    else
                    {
                        out.ExtraTexCoords[uvi].insert(out.ExtraTexCoords[uvi].end(), {0.0f, 0.0f});
                    }
                }
            }

            // Indices: rebase + optional winding flip.
            const size_t triCount = src.Indices.size() / 3;
            for (size_t ti = 0; ti < triCount; ++ti)
            {
                const uint32 i0 = src.Indices[ti * 3 + 0] + vertexBase;
                const uint32 i1 = src.Indices[ti * 3 + 1] + vertexBase;
                const uint32 i2 = src.Indices[ti * 3 + 2] + vertexBase;
                if (flipWinding)
                {
                    out.Indices.push_back(i0);
                    out.Indices.push_back(i2);
                    out.Indices.push_back(i1);
                }
                else
                {
                    out.Indices.push_back(i0);
                    out.Indices.push_back(i1);
                    out.Indices.push_back(i2);
                }
            }

            // Free the source primitive's geometry now that it's been copied
            // into `out`. Keeps peak RAM at ~1x rather than ~2x the merged
            // output during the merge pass.
            Vector<Vertex>().swap(src.Vertices);
            Vector<uint32>().swap(src.Indices);
            Vector<float>().swap(src.Color0);
            Vector<float>().swap(src.TexCoords1);
            Vector<Vector<float>>().swap(src.ExtraTexCoords);
            Vector<uint16>().swap(src.Joints0);
            Vector<float>().swap(src.Weights0);
            Vector<uint16>().swap(src.Joints1);
            Vector<float>().swap(src.Weights1);
        }

        merged.push_back(std::move(out));
    }

    Logger::Log::Info(
        "[ModelLoad] Merged {} static primitives into {} per-material draws ({} verts, {} indices)",
        static_cast<uint32>(staticMeshes.size()),
        static_cast<uint32>(merged.size()),
        static_cast<uint32>(totalSrcVerts),
        static_cast<uint32>(totalSrcIndices));

    // Reassemble: skinned first (so any skeleton-bound rendering stays adjacent
    // for cache locality), then merged static groups.
    meshes.reserve(skinned.size() + merged.size());
    for (auto& m : skinned) meshes.push_back(std::move(m));
    for (auto& m : merged)  meshes.push_back(std::move(m));
}

#if defined(GE_HAVE_CGLTF)
namespace {
std::filesystem::path ExternalBufferKey(const std::filesystem::path& path)
{
    std::error_code error;
    auto resolved = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : resolved;
}

using ExternalBufferSizes = std::unordered_map<std::filesystem::path, cgltf_size>;

ExternalBufferSizes DeclaredExternalBufferSizes(const cgltf_data& document, const std::string& fullPath)
{
    ExternalBufferSizes sizes;
    if (fullPath.empty())
        return sizes;

    const size_t lastSeparator = fullPath.find_last_of("/\\");
    const std::string directory = lastSeparator == std::string::npos ? "" : fullPath.substr(0, lastSeparator + 1);
    for (cgltf_size index = 0; index < document.buffers_count; ++index) {
        const auto& buffer = document.buffers[index];
        if (!buffer.uri || std::strncmp(buffer.uri, "data:", 5) == 0 || std::strstr(buffer.uri, "://"))
            continue;
        std::string decoded(buffer.uri);
        decoded.resize(cgltf_decode_uri(decoded.data()));
        // cgltf concatenates the URI onto the directory, including a URI with a leading slash.
        auto& size = sizes[ExternalBufferKey(directory + decoded)];
        size = std::max(size, buffer.size);
    }
    return sizes;
}

struct GltfBufferReadContext
{
    GltfExternalBufferStorage& Backing;
    ExternalBufferSizes Sizes;
};

// Every external buffer keeps its declared byteLength. Buffers naming one file share the
// largest declared prefix, read directly into document-owned storage through the shared-open
// path, so a watched file remains replaceable while it is read (AssetCore/SharedFileRead.h).
cgltf_result CgltfReadFileShared(const cgltf_memory_options* /*memoryOptions*/,
                                 const cgltf_file_options* fileOptions,
                                 const char* path, cgltf_size* size, void** data)
{
    if (!fileOptions || !fileOptions->user_data || !path || !size || !data)
        return cgltf_result_invalid_options;

    try {
        auto& context = *static_cast<GltfBufferReadContext*>(fileOptions->user_data);
        const auto key = ExternalBufferKey(path);
        auto loaded = context.Backing.find(key);
        if (loaded == context.Backing.end()) {
            const auto declared = context.Sizes.find(key);
            if (declared == context.Sizes.end())
                return cgltf_result_file_not_found;
            SharedFileReader file{std::filesystem::path(path)};
            if (!file.IsOpen())
                return cgltf_result_file_not_found;
            const int64 fileSize = file.Size();
            if (fileSize < 0)
                return cgltf_result_io_error;
            if (static_cast<uint64>(fileSize) < declared->second)
                return cgltf_result_data_too_short;

            Vector<uint8> bytes(declared->second);
            const int64 read = file.Read(bytes.data(), bytes.size());
            if (read < 0)
                return cgltf_result_io_error;
            if (static_cast<uint64>(read) != bytes.size())
                return cgltf_result_data_too_short;
            loaded = context.Backing.emplace(key, std::move(bytes)).first;
        }
        if (loaded->second.size() < *size)
            return cgltf_result_data_too_short;
        *data = loaded->second.data();
        return cgltf_result_success;
    } catch (const std::bad_alloc&) {
        return cgltf_result_out_of_memory;
    }
}

void CgltfReleaseSharedFile(const cgltf_memory_options*, const cgltf_file_options*, void*)
{
    // cgltf releases each buffer separately; their shared backing belongs to GltfDocumentRelease.
}

// Extensions the engine implements: a file may list them in extensionsRequired
// and still load as authored, and an import does not report them as ignored.
// KHR_materials_emissive_strength: LoadGLTF multiplies the emission by it.
// KHR_mesh_quantization: every accessor is read through cgltf's typed readers,
// which convert each component type the extension allows. MSFT_lod: LoadGLTF
// assembles the authored LOD chains.
constexpr std::array<std::string_view, 3> kImplementedGltfExtensions = {
    "KHR_materials_emissive_strength",
    "KHR_mesh_quantization",
    "MSFT_lod",
};

std::string_view CgltfResultName(cgltf_result result)
{
    switch (result) {
        case cgltf_result_success: return "success";
        case cgltf_result_data_too_short: return "data_too_short";
        case cgltf_result_unknown_format: return "unknown_format";
        case cgltf_result_invalid_json: return "invalid_json";
        case cgltf_result_invalid_gltf: return "invalid_gltf";
        case cgltf_result_invalid_options: return "invalid_options";
        case cgltf_result_file_not_found: return "file_not_found";
        case cgltf_result_io_error: return "io_error";
        case cgltf_result_out_of_memory: return "out_of_memory";
        case cgltf_result_legacy_gltf: return "legacy_gltf";
        case cgltf_result_max_enum: break;
    }
    return "an unknown result";
}

// The names among the `count` extension names at `extensions` that
// kImplementedGltfExtensions lacks, comma separated; empty when the engine
// implements every one.
std::string UnimplementedExtensions(char* const* extensions, cgltf_size count)
{
    std::string names;
    for (cgltf_size i = 0; i < count; ++i) {
        const std::string_view name = extensions[i];
        if (std::find(kImplementedGltfExtensions.begin(), kImplementedGltfExtensions.end(), name) !=
            kImplementedGltfExtensions.end())
            continue;
        if (!names.empty())
            names += ", ";
        names += name;
    }
    return names;
}

// Lists, once per model import, the extensions a file uses without requiring
// that the engine ignores; the file loads without what they add.
void ReportIgnoredGltfExtensions(const cgltf_data& document, const std::filesystem::path& path)
{
    const std::string ignored = UnimplementedExtensions(document.extensions_used, document.extensions_used_count);
    if (!ignored.empty())
        Logger::Log::Info("glTF '{}' uses extensions this engine ignores: {}; it loads without what they add.",
                          path.string(), ignored);
}

// glTF bounds a buffer view's byteStride to 4 to 252 bytes in steps of 4.
constexpr cgltf_size kMinimumByteStride = 4;
constexpr cgltf_size kMaximumByteStride = 252;
constexpr cgltf_size kByteStrideStep = 4;

// True when offset + stride * (count - 1) + elementSize, the extent cgltf_validate compares with a
// buffer view in size_t arithmetic, is larger than a size_t holds, so that the comparison wraps.
bool ExtentWraps(cgltf_size offset, cgltf_size stride, cgltf_size count, cgltf_size elementSize)
{
    if (count == 0)
        return false;
    constexpr cgltf_size kLargest = std::numeric_limits<cgltf_size>::max();
    if (elementSize > kLargest - offset)
        return true;
    return stride != 0 && count - 1 > (kLargest - offset - elementSize) / stride;
}

// cgltf_validate checks no byteStride; its extent checks wrap, so an accessor whose extent passes the
// size of a size_t compares as a small one and passes; and its accessor loop scans each sparse block's
// indices from the loaded buffer before its buffer view loop checks that each view fits its buffer.
// The first buffer view with a byteStride outside the glTF range or running past its buffer, or
// accessor whose dense or sparse extent wraps, described for a refusal; empty when none is.
std::string StrideOrExtentOutOfRange(const cgltf_data& document)
{
    for (cgltf_size i = 0; i < document.buffer_views_count; ++i)
    {
        const cgltf_buffer_view& view = document.buffer_views[i];
        const cgltf_size stride = view.stride;
        if (stride != 0 && (stride < kMinimumByteStride || stride > kMaximumByteStride || stride % kByteStrideStep != 0))
            return "buffer view " + std::to_string(i) + " has byteStride " + std::to_string(stride) +
                   "; glTF allows 4 to 252 in steps of 4";
        const cgltf_buffer* buffer = view.buffer;
        if (buffer && (view.size > buffer->size || view.offset > buffer->size - view.size))
            return "buffer view " + std::to_string(i) + " holds " + std::to_string(view.size) + " bytes from byte " +
                   std::to_string(view.offset) + ", past the " + std::to_string(buffer->size) + " bytes of buffer " +
                   std::to_string(buffer - document.buffers);
    }
    for (cgltf_size i = 0; i < document.accessors_count; ++i)
    {
        const cgltf_accessor& accessor = document.accessors[i];
        const cgltf_size elementSize = cgltf_calc_size(accessor.type, accessor.component_type);
        if (accessor.buffer_view && ExtentWraps(accessor.offset, accessor.stride, accessor.count, elementSize))
            return "accessor " + std::to_string(i) + " declares " + std::to_string(accessor.count) + " elements " +
                   std::to_string(accessor.stride) + " bytes apart, more bytes than a buffer can hold";
        if (!accessor.is_sparse)
            continue;
        const cgltf_accessor_sparse& sparse = accessor.sparse;
        const cgltf_size indexSize = cgltf_component_size(sparse.indices_component_type);
        if (ExtentWraps(sparse.indices_byte_offset, indexSize, sparse.count, indexSize) ||
            ExtentWraps(sparse.values_byte_offset, elementSize, sparse.count, elementSize))
            return "accessor " + std::to_string(i) + " declares a sparse block of " + std::to_string(sparse.count) +
                   " entries, more bytes than a buffer can hold";
    }
    return {};
}

// A buffer view bounds its accessor's count by the bytes the file holds. An accessor without one reads
// zeros, or a sparse block's values over zeros, for whatever count it declares, and LoadGLTF and the
// clip path size their arrays by that count. Such an accessor holds no more elements than the HLOD
// cache allows a submesh (kMaxVerticesPerSubmesh and kMaxIndicesPerSubmesh): 2^27
// when primitives read it only as their indices, 2^26 otherwise.
constexpr cgltf_size kMaxElementsWithoutABufferView = cgltf_size{kMaxVerticesPerSubmesh};
constexpr cgltf_size kMaxIndicesWithoutABufferView = cgltf_size{kMaxIndicesPerSubmesh};

// The first accessor without a buffer view whose count is past its limit above, described for a
// refusal; empty when none is. An accessor that a primitive or morph target also reads as an attribute
// takes the smaller limit, because its count then sizes a mesh's vertices.
std::string AccessorWithoutABufferViewPastItsLimit(const cgltf_data& document)
{
    Vector<uint8> readAsIndices(document.accessors_count, 0u);
    Vector<uint8> readAsAttribute(document.accessors_count, 0u);
    for (cgltf_size meshIndex = 0; meshIndex < document.meshes_count; ++meshIndex)
    {
        const cgltf_mesh& mesh = document.meshes[meshIndex];
        for (cgltf_size primitiveIndex = 0; primitiveIndex < mesh.primitives_count; ++primitiveIndex)
        {
            const cgltf_primitive& primitive = mesh.primitives[primitiveIndex];
            if (primitive.indices)
                readAsIndices[primitive.indices - document.accessors] = 1u;
            for (cgltf_size attributeIndex = 0; attributeIndex < primitive.attributes_count; ++attributeIndex)
            {
                if (primitive.attributes[attributeIndex].data)
                    readAsAttribute[primitive.attributes[attributeIndex].data - document.accessors] = 1u;
            }
            for (cgltf_size targetIndex = 0; targetIndex < primitive.targets_count; ++targetIndex)
            {
                const cgltf_morph_target& target = primitive.targets[targetIndex];
                for (cgltf_size attributeIndex = 0; attributeIndex < target.attributes_count; ++attributeIndex)
                {
                    if (target.attributes[attributeIndex].data)
                        readAsAttribute[target.attributes[attributeIndex].data - document.accessors] = 1u;
                }
            }
        }
    }
    for (cgltf_size i = 0; i < document.accessors_count; ++i)
    {
        const bool onlyIndices = readAsIndices[i] && !readAsAttribute[i];
        const cgltf_size limit = onlyIndices ? kMaxIndicesWithoutABufferView : kMaxElementsWithoutABufferView;
        const cgltf_accessor& accessor = document.accessors[i];
        if (!accessor.buffer_view && accessor.count > limit)
            return "accessor " + std::to_string(i) + " declares " + std::to_string(accessor.count) +
                   " elements without a buffer view, past the limit of " + std::to_string(limit) +
                   (onlyIndices ? " for primitive indices" : " for an accessor that is not only primitive indices");
    }
    return {};
}

// cgltf_validate does not compare a skin's inverseBindMatrices accessor with its
// joints, and LoadGLTF reads one matrix per joint. The first skin that holds
// fewer matrices than joints, described for a refusal; empty when none does.
std::string SkinShortOfInverseBindMatrices(const cgltf_data& document)
{
    for (cgltf_size i = 0; i < document.skins_count; ++i) {
        const cgltf_skin& skin = document.skins[i];
        if (!skin.inverse_bind_matrices || skin.inverse_bind_matrices->count >= skin.joints_count)
            continue;
        return "skin " + std::to_string(i) + " has " + std::to_string(skin.joints_count) +
               " joints but its inverseBindMatrices accessor holds " +
               std::to_string(skin.inverse_bind_matrices->count) + " matrices";
    }
    return {};
}

// Every refusal of a glTF file is logged here, once, with the file and the reason.
void RefuseGltfFile(const std::filesystem::path& path, std::string_view reason)
{
    Logger::Log::Error("glTF '{}' is not loaded: {}", path.string(), reason);
}

// Reads one VEC3 morph target attribute (POSITION, NORMAL or TANGENT deltas) into `deltas`, three
// floats per element in engine axes, through UnpackAccessorFloats, which applies a sparse block (where
// cgltf_accessor_read_float reads nothing) and reads zeros for an accessor with neither a buffer view
// nor a sparse block. Empty when read; otherwise the reason, for a refusal.
std::string UnpackMorphDeltas(const cgltf_accessor& accessor, const ModelImport::AxisConversion& conversion,
                              Vector<float>& deltas, GltfSparseScratch& sparseScratch)
{
    assert(accessor.type == cgltf_type_vec3);
    std::string problem = UnpackAccessorFloats(accessor, deltas, sparseScratch);
    if (!problem.empty())
        return problem;
    for (size_t i = 0; i < deltas.size(); i += 3u) {
        const ModelImport::Vec3 delta =
            ModelImport::ConvertVec3({deltas[i], deltas[i + 1u], deltas[i + 2u]}, conversion, 1.0f);
        deltas[i] = delta.x;
        deltas[i + 1u] = delta.y;
        deltas[i + 2u] = delta.z;
    }
    return {};
}

// Mesh `meshIndex` of the file as a refusal names it: "mesh 2", followed by " ('Body')" when the file
// names it.
std::string MeshInRefusal(size_t meshIndex, const char* meshName)
{
    std::string mesh = "mesh " + std::to_string(meshIndex);
    if (meshName && meshName[0])
        mesh += " ('" + std::string(meshName) + "')";
    return mesh;
}

// The start of a refusal naming the `attribute` accessor of morph target `targetIndex` of mesh
// `meshIndex`.
std::string MorphAttributeRefusal(size_t targetIndex, std::string_view targetName, size_t meshIndex,
                                  const char* meshName, std::string_view attribute)
{
    return "morph target " + std::to_string(targetIndex) + " ('" + std::string(targetName) + "') of " +
           MeshInRefusal(meshIndex, meshName) + " has a " + std::string(attribute) + " accessor ";
}

// The start of a refusal naming the accessor for `accessorRole` ("indices" or an attribute name such as
// "POSITION") of primitive `primitiveIndex` of mesh `meshIndex`.
std::string PrimitiveAccessorRefusal(size_t primitiveIndex, size_t meshIndex, const char* meshName,
                                     std::string_view accessorRole)
{
    return "primitive " + std::to_string(primitiveIndex) + " of " + MeshInRefusal(meshIndex, meshName) +
           " has an accessor for " + std::string(accessorRole) + " ";
}

// The glTF type LoadGLTF reads base attribute `attribute` as: VEC3 for POSITION and NORMAL, VEC2 for
// TEXCOORD_0, VEC4 for TANGENT, JOINTS_0 and WEIGHTS_0, and for COLOR_0 its accessor's own type when that is
// VEC3 or VEC4 (the one base attribute with two legal types; VEC4 otherwise, which refuses it);
// cgltf_type_invalid for an attribute it does not read.
cgltf_type BaseAttributeType(const cgltf_attribute& attribute)
{
    switch (attribute.type) {
    case cgltf_attribute_type_position:
    case cgltf_attribute_type_normal:
        return cgltf_type_vec3;
    case cgltf_attribute_type_texcoord:
        return attribute.index == 0 ? cgltf_type_vec2 : cgltf_type_invalid;
    case cgltf_attribute_type_tangent:
    case cgltf_attribute_type_joints:
    case cgltf_attribute_type_weights:
        return attribute.index == 0 ? cgltf_type_vec4 : cgltf_type_invalid;
    case cgltf_attribute_type_color:
        if (attribute.index != 0)
            return cgltf_type_invalid;
        return attribute.data && attribute.data->type == cgltf_type_vec3 ? cgltf_type_vec3 : cgltf_type_vec4;
    default:
        return cgltf_type_invalid;
    }
}

// An index or attribute accessor has data when it has a buffer view or a sparse block; LoadGLTF skips
// one with neither (all zeros in glTF).
bool AccessorHasData(const cgltf_accessor* accessor)
{
    return accessor && (accessor->buffer_view || accessor->is_sparse);
}

// The name LoadGLTF gives morph target `targetIndex` of `mesh`: the file's extras.targetNames entry
// (cgltf's target_names), or "morph_<index>" when the file names none.
String MorphTargetName(const cgltf_mesh& mesh, size_t targetIndex)
{
    const bool named = mesh.target_names && targetIndex < mesh.target_names_count && mesh.target_names[targetIndex];
    return named ? String(mesh.target_names[targetIndex]) : "morph_" + std::to_string(targetIndex);
}

// The attribute of `primitive` with data and the most elements: LoadGLTF sizes the primitive's vertices
// by it, before it checks an attribute's name. Null when no attribute has data.
const cgltf_attribute* WidestAttributeWithData(const cgltf_primitive& primitive)
{
    const cgltf_attribute* widest = nullptr;
    for (size_t attributeIndex = 0; attributeIndex < primitive.attributes_count; ++attributeIndex) {
        const cgltf_attribute& attribute = primitive.attributes[attributeIndex];
        if (AccessorHasData(attribute.data) && (!widest || attribute.data->count > widest->data->count))
            widest = &attribute;
    }
    return widest;
}

// Charges `budget` with the morph targets LoadGLTF keeps for `primitive`: per element of each target,
// the vertex index and the position, normal and tangent deltas MakeSparseMorphTarget stores (a
// position delta always, normal and tangent deltas when the target has them). The first charge that
// does not fit, described for a refusal; empty when every one fits.
std::string ChargeMorphTargets(const cgltf_mesh& mesh, size_t meshIndex, const cgltf_primitive& primitive,
                               GltfAllocationBudget& budget)
{
    constexpr size_t kBytesPerDelta = 3u * sizeof(float);
    for (size_t targetIndex = 0; targetIndex < primitive.targets_count; ++targetIndex) {
        const cgltf_morph_target& target = primitive.targets[targetIndex];
        const cgltf_attribute* first = nullptr;
        size_t deltaArrays = 1;
        for (size_t attributeIndex = 0; attributeIndex < target.attributes_count; ++attributeIndex) {
            const cgltf_attribute& attribute = target.attributes[attributeIndex];
            const bool read = attribute.type == cgltf_attribute_type_position ||
                              attribute.type == cgltf_attribute_type_normal ||
                              attribute.type == cgltf_attribute_type_tangent;
            if (!read || !attribute.data)
                continue;
            if (!first)
                first = &attribute;
            if (attribute.type != cgltf_attribute_type_position)
                ++deltaArrays;
        }
        if (!first)
            continue;
        const std::string problem =
            budget.Charge(*first->data, first->data->count, sizeof(uint32) + deltaArrays * kBytesPerDelta);
        if (!problem.empty())
            return MorphAttributeRefusal(targetIndex, MorphTargetName(mesh, targetIndex), meshIndex, mesh.name,
                                         first->name) +
                   problem;
    }
    return {};
}

// Charges `budget` with the arrays LoadGLTF keeps for triangle primitive `primitiveIndex` of mesh
// `meshIndex`: its indices; JOINTS_0, WEIGHTS_0 and COLOR_0 as four components each (an RGB COLOR_0
// takes alpha 1); one Vertex per element of its widest attribute with data, and one index per vertex
// when it has no indices; and its morph targets. The first charge that does not fit, described for a
// refusal; empty when every one fits.
std::string ChargePrimitiveArrays(const cgltf_mesh& mesh, size_t meshIndex, size_t primitiveIndex,
                                  GltfAllocationBudget& budget)
{
    const cgltf_primitive& primitive = mesh.primitives[primitiveIndex];
    const bool indexed = AccessorHasData(primitive.indices) && primitive.indices->count > 0;
    if (indexed) {
        const std::string problem = budget.Charge(*primitive.indices, primitive.indices->count, sizeof(uint32));
        if (!problem.empty())
            return PrimitiveAccessorRefusal(primitiveIndex, meshIndex, mesh.name, "indices") + problem;
    }

    for (size_t attributeIndex = 0; attributeIndex < primitive.attributes_count; ++attributeIndex) {
        const cgltf_attribute& attribute = primitive.attributes[attributeIndex];
        if (!AccessorHasData(attribute.data) || attribute.index != 0)
            continue;
        size_t bytesPerElement = 0;
        if (attribute.type == cgltf_attribute_type_joints)
            bytesPerElement = 4u * sizeof(uint16);
        else if (attribute.type == cgltf_attribute_type_weights || attribute.type == cgltf_attribute_type_color)
            bytesPerElement = 4u * sizeof(float);
        if (bytesPerElement == 0)
            continue;
        const std::string problem = budget.Charge(*attribute.data, attribute.data->count, bytesPerElement);
        if (!problem.empty())
            return PrimitiveAccessorRefusal(primitiveIndex, meshIndex, mesh.name, attribute.name) + problem;
    }

    const cgltf_attribute* widest = WidestAttributeWithData(primitive);
    if (!widest)
        return {};
    const size_t bytesPerVertex = sizeof(Vertex) + (indexed ? 0u : sizeof(uint32));
    const std::string vertexProblem = budget.Charge(*widest->data, widest->data->count, bytesPerVertex);
    if (!vertexProblem.empty())
        return PrimitiveAccessorRefusal(primitiveIndex, meshIndex, mesh.name, widest->name) + vertexProblem;
    return ChargeMorphTargets(mesh, meshIndex, primitive, budget);
}

// The scratch LoadGLTF reads attributes and morph targets through, per vertex of the widest primitive:
// it is reused across primitives and grows to that primitive's elements. An attribute reads up to four
// floats (attributeFloats) or four integers (attributeIntegers), and a morph target three floats each
// of position, normal and tangent deltas.
constexpr size_t kScratchBytesPerVertex = 4u * sizeof(float) + 4u * sizeof(uint32) + 3u * 3u * sizeof(float);

// Charges `budget` with every triangle primitive's arrays, then once with the scratch for the widest
// one, before LoadGLTF sizes any. The first charge that does not fit, described for a refusal; empty
// when every one fits. Not charged: the merge of static primitives by material, which copies arrays
// already charged, so the peak during a load of 32 or more static primitives is up to twice their
// charge.
std::string PrimitiveArraysPastTheBudget(const cgltf_data& document, GltfAllocationBudget& budget)
{
    const cgltf_attribute* widestOfAll = nullptr;
    size_t widestMeshIndex = 0;
    size_t widestPrimitiveIndex = 0;
    for (size_t meshIndex = 0; meshIndex < document.meshes_count; ++meshIndex) {
        const cgltf_mesh& mesh = document.meshes[meshIndex];
        for (size_t primitiveIndex = 0; primitiveIndex < mesh.primitives_count; ++primitiveIndex) {
            const cgltf_primitive& primitive = mesh.primitives[primitiveIndex];
            if (primitive.type != cgltf_primitive_type_triangles)
                continue;
            const std::string problem = ChargePrimitiveArrays(mesh, meshIndex, primitiveIndex, budget);
            if (!problem.empty())
                return problem;
            const cgltf_attribute* widest = WidestAttributeWithData(primitive);
            if (widest && (!widestOfAll || widest->data->count > widestOfAll->data->count)) {
                widestOfAll = widest;
                widestMeshIndex = meshIndex;
                widestPrimitiveIndex = primitiveIndex;
            }
        }
    }
    if (!widestOfAll)
        return {};
    const std::string problem = budget.Charge(*widestOfAll->data, widestOfAll->data->count, kScratchBytesPerVertex);
    if (problem.empty())
        return {};
    const cgltf_mesh& mesh = document.meshes[widestMeshIndex];
    return PrimitiveAccessorRefusal(widestPrimitiveIndex, widestMeshIndex, mesh.name, widestOfAll->name) + problem;
}

// Charges one GltfAllocationBudget of the document, loaded from `path`, with what LoadGLTF sizes for
// it: every primitive's arrays, and the keys of every animation, which LoadGLTF loads as embedded clips
// that stay resident whether or not the model loads. The first charge that does not fit, described for
// a refusal; empty when every one fits.
std::string ModelArraysPastTheBudget(const cgltf_data& document, const std::filesystem::path& path)
{
    GltfAllocationBudget budget(document, path);
    const std::string primitiveProblem = PrimitiveArraysPastTheBudget(document, budget);
    if (!primitiveProblem.empty())
        return primitiveProblem;
    for (size_t animationIndex = 0; animationIndex < document.animations_count; ++animationIndex) {
        const std::string problem = ChargeAnimationKeys(document, animationIndex, budget);
        if (!problem.empty())
            return problem;
    }
    return {};
}

// cgltf_validate bounds the indices an index accessor's buffer view holds by the primitive's vertex
// count, but not the indices its sparse block writes. The first index at or past `vertexCount`, worded
// to follow "an accessor" in a refusal; empty when every index names a vertex.
std::string IndexPastTheVertices(const Vector<uint32>& indices, size_t vertexCount)
{
    for (size_t element = 0; element < indices.size(); ++element) {
        if (indices[element] >= vertexCount)
            return "whose element " + std::to_string(element) + " is vertex " + std::to_string(indices[element]) +
                   ", past the primitive's " + std::to_string(vertexCount) + " vertices; re-export it.";
    }
    return {};
}

// The bounds as the warnings state them in KiB beside their bytes.
constexpr size_t kBytesPerKibibyte = 1024;

// The kinds of object whose extras a model keeps, as a warning names them, indexed by ModelObjectKind.
constexpr std::array<std::string_view, kModelObjectKindCount> kModelObjectKindNames = {
    "scene", "node", "mesh", "material", "animation",
};

// Object `index` of `kind` as an extras warning names it: "node 3", followed by " ('Hand_L')" when the
// file names it.
std::string ObjectInExtrasWarning(ModelObjectKind kind, size_t index, const char* name)
{
    std::string object = std::string(kModelObjectKindNames[static_cast<size_t>(kind)]) + " " + std::to_string(index);
    if (name && name[0])
        object += " ('" + std::string(name) + "')";
    return object;
}

// " and of 3 later objects", or empty for none: the objects a warning counts after the one it names.
std::string AndOfLaterObjects(size_t count)
{
    if (count == 0)
        return {};
    return " and of " + std::to_string(count) + (count == 1 ? " later object" : " later objects");
}

// The blocks an import did not keep for the model bound or a shared name: each warning names the first
// and counts the rest, so one line reports any number of them.
struct ExtrasNotKept
{
    std::string FirstPastModelBound;
    size_t FirstPastModelBoundBytes = 0; // its extras and its name, what the model bound charges
    size_t PastModelBoundCount = 0;
    std::string FirstNameTaken;
    size_t NameTakenCount = 0;
};

// The raw text of `objectExtras`, empty when the object has none. cgltf copies the extras of scenes,
// nodes, materials and animations into `data`, but only locates a mesh's by its start and end offsets into
// the document's JSON text, so a mesh's text is read there. The offsets count from `document.json`, which
// in a GLB is the start of the JSON chunk, not of the file. jsmn tokenises exactly `json_size` bytes, so
// every offset cgltf records lies within them; the range check keeps that true if cgltf ever changes.
std::string_view ExtrasText(const cgltf_data& document, const cgltf_extras& objectExtras)
{
    if (objectExtras.data)
        return objectExtras.data;
    if (!document.json || objectExtras.end_offset <= objectExtras.start_offset ||
        objectExtras.end_offset > document.json_size)
        return {};
    return std::string_view(document.json + objectExtras.start_offset,
                            objectExtras.end_offset - objectExtras.start_offset);
}

// Keeps `text`, the extras of object `index` of `kind`, in `extras`. A block over the object bound is named
// in a warning of its own, which bounds those warnings to one per 256 KiB of the file; the blocks refused
// for the model bound or a shared name are counted in `notKept`.
void RetainObjectExtras(ModelExtras& extras, ExtrasNotKept& notKept, ModelObjectKind kind, size_t index,
                        const char* name, std::string_view text, const std::filesystem::path& path)
{
    if (text.empty())
        return;
    const std::string_view objectName = name ? name : "";
    switch (extras.Retain(kind, objectName, text)) {
    case ModelExtras::RetainResult::Retained:
        return;
    case ModelExtras::RetainResult::OverObjectBound:
        Logger::Log::Warning("glTF '{}': the extras of {} are not kept: they hold {} bytes, and an object keeps "
                             "at most {} bytes ({} KiB) of extras; the model loads without them.",
                             path.string(), ObjectInExtrasWarning(kind, index, name), text.size(),
                             ModelExtras::kObjectBoundBytes, ModelExtras::kObjectBoundBytes / kBytesPerKibibyte);
        return;
    case ModelExtras::RetainResult::OverModelBound:
        if (notKept.PastModelBoundCount++ == 0) {
            notKept.FirstPastModelBound = ObjectInExtrasWarning(kind, index, name);
            notKept.FirstPastModelBoundBytes = objectName.size() + text.size();
        }
        return;
    case ModelExtras::RetainResult::NameTaken:
        if (notKept.NameTakenCount++ == 0)
            notKept.FirstNameTaken = ObjectInExtrasWarning(kind, index, name);
        return;
    }
}

// RetainObjectExtras for each of the `count` objects of `kind` at `objects` in `document`, in file order.
template <typename GltfObject>
void RetainEachObjectExtras(ModelExtras& extras, ExtrasNotKept& notKept, const cgltf_data& document,
                            ModelObjectKind kind, const GltfObject* objects, cgltf_size count,
                            const std::filesystem::path& path)
{
    for (cgltf_size index = 0; index < count; ++index)
        RetainObjectExtras(extras, notKept, kind, index, objects[index].name,
                           ExtrasText(document, objects[index].extras), path);
}

// The raw extras text of the document's scenes, nodes, meshes, materials and animations, kept in that
// order and each in file order within ModelExtras' bounds, so the blocks the model bound refuses are the
// last ones in that order. Every block not kept is reported; the model loads either way.
ModelExtras RetainGltfExtras(const cgltf_data& document, const std::filesystem::path& path)
{
    ModelExtras extras;
    ExtrasNotKept notKept;
    RetainEachObjectExtras(extras, notKept, document, ModelObjectKind::Scene, document.scenes,
                           document.scenes_count, path);
    RetainEachObjectExtras(extras, notKept, document, ModelObjectKind::Node, document.nodes,
                           document.nodes_count, path);
    RetainEachObjectExtras(extras, notKept, document, ModelObjectKind::Mesh, document.meshes,
                           document.meshes_count, path);
    RetainEachObjectExtras(extras, notKept, document, ModelObjectKind::Material, document.materials,
                           document.materials_count, path);
    RetainEachObjectExtras(extras, notKept, document, ModelObjectKind::Animation, document.animations,
                           document.animations_count, path);

    if (notKept.PastModelBoundCount > 0)
        Logger::Log::Warning("glTF '{}': the extras of {} ({} bytes with its name){} are not kept: a model keeps at "
                             "most {} bytes ({} KiB) of extras and their objects' names, taken from its scenes, "
                             "nodes, meshes, materials and animations in that order; the model loads without them.",
                             path.string(), notKept.FirstPastModelBound, notKept.FirstPastModelBoundBytes,
                             AndOfLaterObjects(notKept.PastModelBoundCount - 1), ModelExtras::kModelBoundBytes,
                             ModelExtras::kModelBoundBytes / kBytesPerKibibyte);
    if (notKept.NameTakenCount > 0)
        Logger::Log::Warning("glTF '{}': the extras of {}{} are not readable: an earlier object of the same kind has "
                             "the same name, and a name reads the first; rename them to read their extras.",
                             path.string(), notKept.FirstNameTaken, AndOfLaterObjects(notKept.NameTakenCount - 1));
    return extras;
}
// cgltf's base64 decoder writes into the image's own byte vector: one owned allocation per image.
void* AllocateIntoImageBytes(void* imageBytes, cgltf_size size)
{
    auto& bytes = *static_cast<Vector<uint8>*>(imageBytes);
    bytes.resize(size);
    return bytes.data();
}

void ReleaseNothing(void* /*imageBytes*/, void* /*data*/) {}

// Decodes an image's base64 data URI ("data:<type>[;<parameter>];base64,<payload>") into `image`,
// taking the MIME type from the URI, lower-cased and without parameters, when the image declares
// none. cgltf decodes the data URIs of buffers itself and leaves those of images to the importer.
// The decoded bytes are three quarters of the payload the file already holds, so a data URI cannot
// size an allocation past the file. False, with `image` left empty, when the URI is not base64 or
// its payload does not decode.
bool DecodeDataUriImage(std::string_view uri, EmbeddedImage& image)
{
    constexpr std::string_view kScheme = "data:";
    constexpr std::string_view kBase64Suffix = ";base64";
    const size_t comma = uri.find(',');
    if (uri.substr(0, kScheme.size()) != kScheme || comma == std::string_view::npos)
        return false;
    const std::string_view header = uri.substr(kScheme.size(), comma - kScheme.size());
    if (header.size() < kBase64Suffix.size() || header.substr(header.size() - kBase64Suffix.size()) != kBase64Suffix)
        return false;
    std::string_view payload = uri.substr(comma + 1);
    while (!payload.empty() && payload.back() == '=')
        payload.remove_suffix(1);
    const cgltf_size decodedSize = payload.size() * 3 / 4;
    if (decodedSize == 0)
        return false;

    cgltf_options options{};
    options.memory.alloc_func = &AllocateIntoImageBytes;
    options.memory.free_func = &ReleaseNothing;
    options.memory.user_data = &image.Data;
    void* decoded = nullptr;
    if (cgltf_load_buffer_base64(&options, decodedSize, payload.data(), &decoded) != cgltf_result_success) {
        image.Data.clear();
        return false;
    }
    if (image.MimeType.empty())
        image.MimeType = ToLowerAscii(header.substr(0, header.find(';')));
    return true;
}

} // namespace

void GltfDocumentRelease::operator()(cgltf_data* document)
{
    cgltf_free(document);
    ExternalBuffers.reset();
}

GltfDocument OpenGltfDocument(const Vector<uint8>& bytes, const std::filesystem::path& path)
{
    cgltf_options options{};
    options.file.read = &CgltfReadFileShared;
    options.file.release = &CgltfReleaseSharedFile;
    cgltf_data* parsed = nullptr;
    const cgltf_result parseResult = cgltf_parse(&options, bytes.data(), bytes.size(), &parsed);
    GltfDocument document(parsed);
    if (parseResult != cgltf_result_success || !document) {
        RefuseGltfFile(path, "it does not parse (cgltf_parse: " + std::string(CgltfResultName(parseResult)) +
                                 "); re-export it as glTF 2.0.");
        return {};
    }

    const std::string unimplemented =
        UnimplementedExtensions(document->extensions_required, document->extensions_required_count);
    if (!unimplemented.empty()) {
        RefuseGltfFile(path, "it requires " + unimplemented +
                                 ", which this engine does not implement; export it without these extensions, "
                                 "or with them optional (in extensionsUsed only).");
        return {};
    }

    // External buffer URIs resolve relative to the full file path.
    const std::string fullPath = path.string();
    auto& backing = document.get_deleter().ExternalBuffers;
    backing = std::make_unique<GltfExternalBufferStorage>();
    GltfBufferReadContext reads{*backing, DeclaredExternalBufferSizes(*document, fullPath)};
    // Only this synchronous load uses the context; the document copied file options at parse time.
    options.file.user_data = &reads;
    const cgltf_result loadResult =
        cgltf_load_buffers(&options, document.get(), fullPath.empty() ? nullptr : fullPath.c_str());
    if (loadResult != cgltf_result_success) {
        RefuseGltfFile(path, "its buffers do not load (cgltf_load_buffers: " +
                                 std::string(CgltfResultName(loadResult)) +
                                 "); check that every buffer file sits beside it at its declared byteLength.");
        return {};
    }

    const std::string outOfRange = StrideOrExtentOutOfRange(*document);
    if (!outOfRange.empty()) {
        RefuseGltfFile(path, outOfRange + "; re-export it.");
        return {};
    }

    const std::string pastItsLimit = AccessorWithoutABufferViewPastItsLimit(*document);
    if (!pastItsLimit.empty()) {
        RefuseGltfFile(path, pastItsLimit + "; re-export it.");
        return {};
    }

    const cgltf_result validateResult = cgltf_validate(document.get());
    if (validateResult != cgltf_result_success) {
        RefuseGltfFile(path, "it fails validation (cgltf_validate: " + std::string(CgltfResultName(validateResult)) +
                                 "): an accessor, buffer view, sparse index, index or animation sampler "
                                 "does not fit the data the file holds; re-export it.");
        return {};
    }

    const std::string shortSkin = SkinShortOfInverseBindMatrices(*document);
    if (!shortSkin.empty()) {
        RefuseGltfFile(path, shortSkin + "; re-export it with one inverse bind matrix per joint.");
        return {};
    }
    return document;
}

std::string ChargeAnimationKeys(const cgltf_data& document, size_t animationIndex, GltfAllocationBudget& budget)
{
    const cgltf_animation& animation = document.animations[animationIndex];
    for (size_t channelIndex = 0; channelIndex < animation.channels_count; ++channelIndex) {
        const cgltf_animation_channel& channel = animation.channels[channelIndex];
        const cgltf_animation_sampler* sampler = channel.sampler;
        if (!sampler || !sampler->input || !sampler->output || !channel.target_node)
            continue;
        const std::string problem =
            budget.Charge(*sampler->input, sampler->input->count, sizeof(Animation::AnimKeyframe));
        if (!problem.empty())
            return "channel " + std::to_string(channelIndex) + " of animation " + std::to_string(animationIndex) +
                   " ('" + (animation.name ? animation.name : "<unnamed>") + "') has a key time accessor " + problem;
    }
    return {};
}
#endif // GE_HAVE_CGLTF

bool ModelAsset::LoadGLTF(const Vector<uint8>& data, const FbxLoaderOptions& loaderOptions) {
#if defined(GE_HAVE_CGLTF)
    m_ParseOptionsHash = Hashing::Fnv1a64Value(HashFbxLoaderOptions(loaderOptions), kGltfImportGeometryVersion);
    const ModelImport::AxisConversion axisConversion =
        ModelImport::MakeEngineConversion(GltfImport::SourceConversion(), loaderOptions.Axis);
    auto convertVec = [&](float x, float y, float z, float out[3]) {
        const ModelImport::Vec3 v = ModelImport::ConvertVec3({x, y, z}, axisConversion, 1.0f);
        out[0] = v.x;
        out[1] = v.y;
        out[2] = v.z;
    };
    auto convertQuat = [&](float x, float y, float z, float w, float out[4]) {
        const ModelImport::Quat q = ModelImport::ConvertQuat({x, y, z, w}, axisConversion);
        out[0] = q.x;
        out[1] = q.y;
        out[2] = q.z;
        out[3] = q.w;
    };

    const GltfDocument gltf = OpenGltfDocument(data, GetPath());
    if (!gltf)
        return false;
    const std::string pastTheBudget = ModelArraysPastTheBudget(*gltf, GetPath());
    if (!pastTheBudget.empty()) {
        RefuseGltfFile(GetPath(), pastTheBudget);
        return false;
    }
    ReportIgnoredGltfExtensions(*gltf, GetPath());
    m_Extras = RetainGltfExtras(*gltf, GetPath());

    // An index or attribute accessor without data (AccessorHasData) is skipped. UnpackAccessorFloats and
    // UnpackAccessorIntegers report a buffer that holds no data; OpenGltfDocument bounded the count of an
    // accessor without a buffer view, and ModelArraysPastTheBudget the arrays all primitives size.

    static int sSkippedNonTrianglePrimWarnBudget = 8;

    // The elements of the attribute being read, reused across attributes; the per-vertex morph deltas
    // of the target being read, reused across targets; and the scratch for every sparse block.
    Vector<float> attributeFloats;
    Vector<uint32> attributeIntegers;
    Vector<float> positionDeltas;
    Vector<float> normalDeltas;
    Vector<float> tangentDeltas;
    GltfSparseScratch sparseScratch;
    // COLOR_n past COLOR_0 is not read; the first one is reported, once per import.
    bool reportedExtraColorSet = false;

    auto nodeIndexOf = [&](const cgltf_node* n) -> int
    {
        if (!n)
            return -1;
        return static_cast<int>(n - gltf->nodes);
    };

    const size_t nodeCount = gltf->nodes_count;
    std::vector<int32> parent(nodeCount, -1);
    std::vector<float> restT(nodeCount * 3u, 0.0f);
    std::vector<float> restR(nodeCount * 4u, 0.0f);
    std::vector<float> restS(nodeCount * 3u, 1.0f);
    std::vector<float> restLocalRM(nodeCount * 16u, 0.0f);
    std::vector<glm::mat4> restWorld(nodeCount, glm::mat4(1.0f));
    // Every node that draws each mesh, in node order: the first places the engine Mesh, the rest
    // become its ExtraPlacements.
    std::vector<std::vector<int32>> meshSourceNodes(gltf->meshes_count);
    auto storeColumnMajor = [](const glm::mat4& matrix, float* out)
    {
        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 4; ++col)
                out[col * 4 + row] = matrix[col][row];
        }
    };
    if (nodeCount > 0)
    {
        for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
        {
            const cgltf_node& node = gltf->nodes[nodeIndex];
            convertVec(static_cast<float>(node.translation[0]),
                       static_cast<float>(node.translation[1]),
                       static_cast<float>(node.translation[2]),
                       &restT[nodeIndex * 3]);
            convertQuat(static_cast<float>(node.rotation[0]),
                        static_cast<float>(node.rotation[1]),
                        static_cast<float>(node.rotation[2]),
                        static_cast<float>(node.rotation[3]),
                        &restR[nodeIndex * 4]);
            const ModelImport::Vec3 convertedScale = ModelImport::ConvertScale(
                {static_cast<float>(node.scale[0]),
                 static_cast<float>(node.scale[1]),
                 static_cast<float>(node.scale[2])},
                axisConversion);
            restS[nodeIndex * 3 + 0] = convertedScale.x;
            restS[nodeIndex * 3 + 1] = convertedScale.y;
            restS[nodeIndex * 3 + 2] = convertedScale.z;

            float localMatrix[16];
            if (node.has_matrix)
            {
                for (int i = 0; i < 16; ++i)
                    localMatrix[i] = static_cast<float>(node.matrix[i]);
            }
            else
            {
                cgltf_node_transform_local(&gltf->nodes[nodeIndex], localMatrix);
            }
            float convertedLocal[16];
            ModelImport::ConvertColumnMajor16(localMatrix, axisConversion, 1.0f, convertedLocal);
            std::memcpy(&restLocalRM[nodeIndex * 16u], convertedLocal, 16 * sizeof(float));

            if (node.mesh)
            {
                const int meshIndex = static_cast<int>(node.mesh - gltf->meshes);
                if (meshIndex >= 0 && meshIndex < static_cast<int>(meshSourceNodes.size()))
                    meshSourceNodes[static_cast<size_t>(meshIndex)].push_back(static_cast<int32>(nodeIndex));
            }
        }

        for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
        {
            const cgltf_node& node = gltf->nodes[nodeIndex];
            for (size_t childIndex = 0; childIndex < node.children_count; ++childIndex)
            {
                const int childNodeIndex = nodeIndexOf(node.children[childIndex]);
                if (childNodeIndex >= 0)
                    parent[static_cast<size_t>(childNodeIndex)] = static_cast<int32>(nodeIndex);
            }
        }

        std::vector<uint8_t> built(nodeCount, 0u);
        std::function<void(size_t)> buildWorld = [&](size_t index)
        {
            if (built[index])
                return;
            const int parentIndex = parent[index];
            const glm::mat4 localMatrix = glm::make_mat4(&restLocalRM[index * 16u]);
            if (parentIndex >= 0)
            {
                buildWorld(static_cast<size_t>(parentIndex));
                restWorld[index] = restWorld[static_cast<size_t>(parentIndex)] * localMatrix;
            }
            else
            {
                restWorld[index] = localMatrix;
            }
            built[index] = 1u;
        };

        for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
            buildWorld(nodeIndex);
    }

    // Iterate meshes/primitives and convert into engine Mesh records.
    for (size_t mi = 0; mi < gltf->meshes_count; ++mi) {
        const cgltf_mesh& m = gltf->meshes[mi];
        for (size_t pi = 0; pi < m.primitives_count; ++pi) {
            const cgltf_primitive& p = m.primitives[pi];

            // Current runtime draw path is triangle-list only.
            if (p.type != cgltf_primitive_type_triangles) {
                if (sSkippedNonTrianglePrimWarnBudget-- > 0) {
                    Logger::Log::Warning(
                        "ModelAsset: skipping non-triangle glTF primitive '{}' (mesh='{}', primitiveIndex={}, type={}).",
                        GetName(),
                        m.name ? m.name : "<unnamed>",
                        static_cast<uint32>(pi),
                        static_cast<uint32>(p.type));
                }
                continue;
            }

            Mesh mesh;
            mesh.Name = m.name ? m.name : GetName();
            mesh.MaterialIndex = 0;
            if (mi < meshSourceNodes.size() && !meshSourceNodes[mi].empty())
                mesh.SourceNodeIndex = meshSourceNodes[mi].front();
            if (mesh.SourceNodeIndex >= 0 && static_cast<size_t>(mesh.SourceNodeIndex) < restWorld.size())
                storeColumnMajor(restWorld[static_cast<size_t>(mesh.SourceNodeIndex)], mesh.SourceNodeTransform);
            mesh.MinBounds[0] = mesh.MinBounds[1] = mesh.MinBounds[2] = std::numeric_limits<float>::max();
            mesh.MaxBounds[0] = mesh.MaxBounds[1] = mesh.MaxBounds[2] = std::numeric_limits<float>::lowest();

            // Indices and the attributes below are read whole through UnpackAccessorIntegers (indices and
            // JOINTS_0) or UnpackAccessorFloats, as the morph targets are: both convert every legal
            // component type (including UNSIGNED_BYTE) and apply a sparse block, with or without a buffer
            // view, where cgltf's per-element readers read nothing from a sparse accessor.
            if (AccessorHasData(p.indices)) {
                const std::string problem = UnpackAccessorIntegers(*p.indices, mesh.Indices, sparseScratch);
                if (!problem.empty()) {
                    RefuseGltfFile(GetPath(), PrimitiveAccessorRefusal(pi, mi, m.name, "indices") + problem);
                    m_Meshes.clear();
                    return false;
                }
            }

            // Attributes: POSITION required, others optional.
            bool havePositions = false;
            bool haveNormals = false;
            bool haveTexcoords = false;
            bool haveTangents = false;
            size_t positionVertexCount = 0;
            for (size_t ai = 0; ai < p.attributes_count; ++ai) {
                const cgltf_attribute& a = p.attributes[ai];
                if (!a.data)
                    continue;
                const cgltf_accessor* acc = a.data;
                if (!AccessorHasData(acc))
                    continue;
                const size_t count = acc->count;
                if (mesh.Vertices.size() < count)
                    mesh.Vertices.resize(count, Vertex{});

                const cgltf_type readType = BaseAttributeType(a);
                if (readType == cgltf_type_invalid) {
                    if (a.type == cgltf_attribute_type_color && !reportedExtraColorSet) {
                        reportedExtraColorSet = true;
                        Logger::Log::Info("glTF '{}' has vertex colour set {} on {}; only COLOR_0 is read.",
                                          GetPath().string(), a.name ? a.name : "COLOR_n", MeshInRefusal(mi, m.name));
                    }
                    continue;
                }
                // glTF allows COLOR_n only as FLOAT, normalized UNSIGNED_BYTE or normalized UNSIGNED_SHORT;
                // cgltf_validate does not check the component type, and any other one would import out of 0..1.
                if (a.type == cgltf_attribute_type_color &&
                    !(acc->component_type == cgltf_component_type_r_32f ||
                      (acc->normalized && (acc->component_type == cgltf_component_type_r_8u ||
                                           acc->component_type == cgltf_component_type_r_16u)))) {
                    RefuseGltfFile(GetPath(), PrimitiveAccessorRefusal(pi, mi, m.name, a.name) +
                                                  "that is not FLOAT, normalized UNSIGNED_BYTE or normalized "
                                                  "UNSIGNED_SHORT; re-export it.");
                    m_Meshes.clear();
                    return false;
                }
                if (acc->type != readType) {
                    const std::string expected = a.type == cgltf_attribute_type_color
                                                     ? std::string("VEC3 or VEC4")
                                                     : "VEC" + std::to_string(cgltf_num_components(readType));
                    RefuseGltfFile(GetPath(), PrimitiveAccessorRefusal(pi, mi, m.name, a.name) + "that is not " +
                                                  expected + "; re-export it.");
                    m_Meshes.clear();
                    return false;
                }
                const std::string problem = a.type == cgltf_attribute_type_joints
                                                ? UnpackAccessorIntegers(*acc, attributeIntegers, sparseScratch)
                                                : UnpackAccessorFloats(*acc, attributeFloats, sparseScratch);
                if (!problem.empty()) {
                    RefuseGltfFile(GetPath(), PrimitiveAccessorRefusal(pi, mi, m.name, a.name) + problem);
                    m_Meshes.clear();
                    return false;
                }

                if (a.type == cgltf_attribute_type_position) {
                    havePositions = true;
                    positionVertexCount = count;
                    for (size_t i = 0; i < count; ++i) {
                        const float* position = attributeFloats.data() + i * 3;
                        convertVec(position[0], position[1], position[2], mesh.Vertices[i].Position);
                    }
                } else if (a.type == cgltf_attribute_type_normal) {
                    haveNormals = true;
                    for (size_t i = 0; i < count; ++i) {
                        const float* normal = attributeFloats.data() + i * 3;
                        convertVec(normal[0], normal[1], normal[2], mesh.Vertices[i].Normal);
                    }
                } else if (a.type == cgltf_attribute_type_texcoord) {
                    haveTexcoords = true;
                    for (size_t i = 0; i < count; ++i) {
                        const float* texcoord = attributeFloats.data() + i * 2;
                        mesh.Vertices[i].TexCoords[0] = texcoord[0];
                        mesh.Vertices[i].TexCoords[1] = texcoord[1];
                    }
                } else if (a.type == cgltf_attribute_type_tangent) {
                    haveTangents = true;
                    for (size_t i = 0; i < count; ++i) {
                        const float* tangent = attributeFloats.data() + i * 4;
                        convertVec(tangent[0], tangent[1], tangent[2], mesh.Vertices[i].Tangent);
                        // reverseWinding (default Mirror X) flips the bitangent
                        // handedness, so Tangent.w is negated to keep
                        // cross(N, T) * w pointing along the surface.
                        mesh.Vertices[i].Tangent[3] =
                            axisConversion.reverseWinding ? -tangent[3] : tangent[3];
                    }
                } else if (a.type == cgltf_attribute_type_joints) {
                    if (mesh.Joints0.size() < count * 4)
                        mesh.Joints0.assign(count * 4, 0);
                    for (size_t i = 0; i < count * 4; ++i)
                        mesh.Joints0[i] = static_cast<uint16>(std::min(attributeIntegers[i], 0xFFFFu));
                } else if (a.type == cgltf_attribute_type_weights) {
                    if (mesh.Weights0.size() < count * 4)
                        mesh.Weights0.assign(count * 4, 0.0f);
                    for (size_t i = 0; i < count; ++i) {
                        float* weights = attributeFloats.data() + i * 4;
                        const float sum = weights[0] + weights[1] + weights[2] + weights[3];
                        if (sum > 0.0f) {
                            weights[0] /= sum;
                            weights[1] /= sum;
                            weights[2] /= sum;
                            weights[3] /= sum;
                        }
                        mesh.Weights0[i * 4 + 0] = weights[0];
                        mesh.Weights0[i * 4 + 1] = weights[1];
                        mesh.Weights0[i * 4 + 2] = weights[2];
                        mesh.Weights0[i * 4 + 3] = weights[3];
                    }
                } else if (a.type == cgltf_attribute_type_color) {
                    // COLOR_0 is linear; UnpackAccessorFloats decodes the normalized UNSIGNED_BYTE and
                    // UNSIGNED_SHORT forms to 0..1. An RGB colour takes alpha 1.
                    const size_t components = cgltf_num_components(acc->type);
                    mesh.Color0.assign(count * 4, 1.0f);
                    for (size_t i = 0; i < count; ++i)
                        std::copy_n(attributeFloats.data() + i * components, components, mesh.Color0.data() + i * 4);
                }
            }

            if (!havePositions || positionVertexCount == 0)
                continue;

            const std::string indexPastTheVertices = IndexPastTheVertices(mesh.Indices, mesh.Vertices.size());
            if (!indexPastTheVertices.empty()) {
                RefuseGltfFile(GetPath(), PrimitiveAccessorRefusal(pi, mi, m.name, "indices") + indexPastTheVertices);
                m_Meshes.clear();
                return false;
            }

            // Morph targets keep the file's order, empty ones included, so the engine's target i,
            // default weight i and name i are the file's target i, mesh.weights[i] and
            // extras.targetNames[i] (cgltf's target_names). cgltf_validate gave every target attribute
            // the vertex count of the primitive's attributes.
            mesh.MorphTargets.reserve(p.targets_count);
            mesh.MorphTargetDefaultWeights.reserve(p.targets_count);
            for (size_t targetIndex = 0; targetIndex < p.targets_count; ++targetIndex)
            {
                String targetName = MorphTargetName(m, targetIndex);
                positionDeltas.clear();
                normalDeltas.clear();
                tangentDeltas.clear();
                const cgltf_morph_target& cgltfTarget = p.targets[targetIndex];
                for (size_t attributeIndex = 0; attributeIndex < cgltfTarget.attributes_count; ++attributeIndex)
                {
                    const cgltf_attribute& attribute = cgltfTarget.attributes[attributeIndex];
                    Vector<float>* deltas = nullptr;
                    if (attribute.type == cgltf_attribute_type_position)
                        deltas = &positionDeltas;
                    else if (attribute.type == cgltf_attribute_type_normal)
                        deltas = &normalDeltas;
                    else if (attribute.type == cgltf_attribute_type_tangent)
                        deltas = &tangentDeltas;
                    else
                        continue;

                    if (attribute.data->type != cgltf_type_vec3) {
                        RefuseGltfFile(GetPath(),
                                       MorphAttributeRefusal(targetIndex, targetName, mi, m.name, attribute.name) +
                                           "of " + std::to_string(cgltf_num_components(attribute.data->type)) +
                                           " components; morph target attributes are VEC3, re-export it with "
                                           "three components per delta.");
                        m_Meshes.clear();
                        return false;
                    }
                    const std::string problem =
                        UnpackMorphDeltas(*attribute.data, axisConversion, *deltas, sparseScratch);
                    if (!problem.empty()) {
                        RefuseGltfFile(GetPath(),
                                       MorphAttributeRefusal(targetIndex, targetName, mi, m.name, attribute.name) +
                                           problem);
                        m_Meshes.clear();
                        return false;
                    }
                }
                mesh.MorphTargets.push_back(MakeSparseMorphTarget(std::move(targetName), mesh.Vertices.size(),
                                                                  positionDeltas, normalDeltas, tangentDeltas));
                mesh.MorphTargetDefaultWeights.push_back(
                    (m.weights && targetIndex < m.weights_count) ? static_cast<float>(m.weights[targetIndex])
                                                                  : 0.0f);
            }

            if (mesh.Indices.empty()) {
                // Non-indexed triangle list fallback.
                const uint32 triVertCount = static_cast<uint32>((mesh.Vertices.size() / 3u) * 3u);
                mesh.Indices.reserve(triVertCount);
                for (uint32 i = 0; i < triVertCount; ++i)
                    mesh.Indices.push_back(i);
            }

            // Every index names a vertex (bounded above), but the index count need not be a
            // multiple of three; a trailing partial triangle is dropped.
            if (mesh.Indices.size() % 3 != 0) {
                mesh.Indices.resize((mesh.Indices.size() / 3) * 3);
            }

            if (mesh.Indices.empty())
                continue;

            if (axisConversion.reverseWinding)
            {
                for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3)
                    std::swap(mesh.Indices[i + 1], mesh.Indices[i + 2]);
            }

            // Recompute bounds after attribute/index sanitization.
            mesh.MinBounds[0] = mesh.MinBounds[1] = mesh.MinBounds[2] = std::numeric_limits<float>::max();
            mesh.MaxBounds[0] = mesh.MaxBounds[1] = mesh.MaxBounds[2] = std::numeric_limits<float>::lowest();
            for (const auto& v : mesh.Vertices) {
                for (int c = 0; c < 3; ++c) {
                    mesh.MinBounds[c] = std::min(mesh.MinBounds[c], v.Position[c]);
                    mesh.MaxBounds[c] = std::max(mesh.MaxBounds[c], v.Position[c]);
                }
            }

            // If normals are missing, generate smooth per-vertex normals from geometry.
            if (!haveNormals) {
                for (auto& v : mesh.Vertices) {
                    v.Normal[0] = v.Normal[1] = v.Normal[2] = 0.0f;
                }
                const size_t triCount = mesh.Indices.size() / 3;
                for (size_t ti = 0; ti < triCount; ++ti) {
                    const uint32 i0 = mesh.Indices[3 * ti + 0];
                    const uint32 i1 = mesh.Indices[3 * ti + 1];
                    const uint32 i2 = mesh.Indices[3 * ti + 2];
                    const auto& v0 = mesh.Vertices[i0];
                    const auto& v1 = mesh.Vertices[i1];
                    const auto& v2 = mesh.Vertices[i2];
                    const glm::vec3 p0(v0.Position[0], v0.Position[1], v0.Position[2]);
                    const glm::vec3 p1(v1.Position[0], v1.Position[1], v1.Position[2]);
                    const glm::vec3 p2(v2.Position[0], v2.Position[1], v2.Position[2]);
                    const glm::vec3 e1 = p1 - p0;
                    const glm::vec3 e2 = p2 - p0;
                    const glm::vec3 n = glm::cross(e1, e2);
                    const float len2 = glm::dot(n, n);
                    if (len2 < 1e-12f)
                        continue;
                    mesh.Vertices[i0].Normal[0] += n.x;
                    mesh.Vertices[i0].Normal[1] += n.y;
                    mesh.Vertices[i0].Normal[2] += n.z;
                    mesh.Vertices[i1].Normal[0] += n.x;
                    mesh.Vertices[i1].Normal[1] += n.y;
                    mesh.Vertices[i1].Normal[2] += n.z;
                    mesh.Vertices[i2].Normal[0] += n.x;
                    mesh.Vertices[i2].Normal[1] += n.y;
                    mesh.Vertices[i2].Normal[2] += n.z;
                }
                for (auto& v : mesh.Vertices) {
                    const float nx = v.Normal[0];
                    const float ny = v.Normal[1];
                    const float nz = v.Normal[2];
                    const float len2 = nx * nx + ny * ny + nz * nz;
                    if (len2 > 0.0f) {
                        const float invLen = 1.0f / std::sqrt(len2);
                        v.Normal[0] = nx * invLen;
                        v.Normal[1] = ny * invLen;
                        v.Normal[2] = nz * invLen;
                    } else {
                        v.Normal[0] = 0.0f;
                        v.Normal[1] = 1.0f;
                        v.Normal[2] = 0.0f;
                    }
                }
            }

            // glTF leaves a missing TANGENT stream to the client. A normal map reads its tangent frame
            // from the stream, and a layout without one shades with the vertex normal and drops the map
            // (standard_pbr.glsl, HAS_TANGENT), so a normal-mapped primitive with UVs gets a generated
            // frame. It is built from the converted positions and UVs, in the engine's axes.
            // GenerateMeshTangents points the bitangent (cross(N, T) * w) along +dP/dV. glTF's
            // normal textures put green toward -V (their UV origin is the image's top-left), which
            // is where an authored glTF tangent points the bitangent, so the generated handedness
            // is negated to read the map as an authored frame would.
            if (loaderOptions.GenerateMissingTangents && !haveTangents && haveTexcoords && p.material &&
                p.material->normal_texture.texture)
            {
                GenerateMeshTangents(mesh);
                // The two conventions differ by this sign: the generator's w puts B along +dP/dV,
                // an authored glTF tangent's w puts it along -dP/dV. Without the negation every
                // generated glTF frame reads its normal map's green channel inverted
                // (GltfImport.GeneratedTangentsAgreeWithTheAuthoredTangentsOfTheSameMesh).
                for (Vertex& vertex : mesh.Vertices)
                    vertex.Tangent[3] = -vertex.Tangent[3];
            }

            // Determine material index from primitive (default 0).
            if (p.material) {
                const int idx = static_cast<int>(p.material - gltf->materials);
                mesh.MaterialIndex = (idx >= 0 && idx < static_cast<int>(gltf->materials_count))
                                       ? static_cast<uint32>(idx)
                                       : 0u;
            } else {
                mesh.MaterialIndex = 0u;
            }

            mesh.Skinned =
                (mesh.Joints0.size() == mesh.Vertices.size() * 4u) &&
                (mesh.Weights0.size() == mesh.Vertices.size() * 4u);

            if (!mesh.IsSkinned() && mi < meshSourceNodes.size() && meshSourceNodes[mi].size() > 1u) {
                mesh.ExtraPlacements.resize(meshSourceNodes[mi].size() - 1u);
                for (size_t placementIndex = 1; placementIndex < meshSourceNodes[mi].size(); ++placementIndex) {
                    MeshPlacement& placement = mesh.ExtraPlacements[placementIndex - 1u];
                    placement.SourceNodeIndex = meshSourceNodes[mi][placementIndex];
                    storeColumnMajor(restWorld[static_cast<size_t>(placement.SourceNodeIndex)],
                                     placement.SourceNodeTransform);
                }
            }

            m_Meshes.push_back(std::move(mesh));
        }
    }

    // Extract node hierarchy for animation sampling, and skin metadata when present.
    if (nodeCount > 0) {
        using namespace GameEngine::Engine::Renderer;
        SkeletonData* skeleton = RetainOrCreateSkeleton(static_cast<uint32>(nodeCount));
        if (skeleton) {
            skeleton->Parent = std::move(parent);
            skeleton->RestTranslation = std::move(restT);
            skeleton->RestRotation = std::move(restR);
            skeleton->RestScale = std::move(restS);
            skeleton->RestLocalMatrix = std::move(restLocalRM);
            skeleton->BoneCount = static_cast<uint32>(nodeCount);
            skeleton->SourceModelPath = GetPath();

            // Bone names for sample-time name resolution (cross-rig animation).
            skeleton->BoneNames.resize(nodeCount);
            for (size_t i = 0; i < nodeCount; ++i)
            {
                const cgltf_node& node = gltf->nodes[i];
                if (node.name && node.name[0])
                    skeleton->BoneNames[i] = node.name;
            }
            skeleton->BuildBoneNameLookup();

            skeleton->InverseBind.assign(nodeCount * 16u, 0.0f);
            for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
                const size_t offset = nodeIndex * 16u;
                skeleton->InverseBind[offset + 0] = 1.0f;
                skeleton->InverseBind[offset + 5] = 1.0f;
                skeleton->InverseBind[offset + 10] = 1.0f;
                skeleton->InverseBind[offset + 15] = 1.0f;
            }

            if (gltf->skins_count > 0) {
                const cgltf_skin& skin = gltf->skins[0];
                const uint32 jointCount = static_cast<uint32>(skin.joints_count);
                skeleton->SkinJointCount = jointCount;
                skeleton->JointNodes.resize(jointCount);
                // The inverse bind matrices are read per element through cgltf_accessor_read_float from
                // a buffer view whose buffer holds data; without one every joint keeps the identity. A
                // sparse block is not applied: the reader reads nothing from a sparse accessor and
                // leaves the matrix zero.
                const cgltf_buffer_view* inverseBindView =
                    skin.inverse_bind_matrices ? skin.inverse_bind_matrices->buffer_view : nullptr;
                if (inverseBindView && inverseBindView->buffer && inverseBindView->buffer->data) {
                    for (uint32 jointIndex = 0; jointIndex < jointCount; ++jointIndex) {
                        const cgltf_node* jointNode = skin.joints[jointIndex];
                        const int nodeIdx = nodeIndexOf(jointNode);
                        if (nodeIdx < 0)
                            continue;
                        skeleton->JointNodes[jointIndex] = static_cast<uint32>(nodeIdx);
                        float inverseBindMatrix[16]{};
                        cgltf_accessor_read_float(skin.inverse_bind_matrices, jointIndex, inverseBindMatrix, 16);
                        float convertedIbm[16];
                        ModelImport::ConvertColumnMajor16(inverseBindMatrix, axisConversion, 1.0f, convertedIbm);
                        const size_t offset = static_cast<size_t>(nodeIdx) * 16u;
                        std::memcpy(&skeleton->InverseBind[offset], convertedIbm, 16 * sizeof(float));
                    }
                } else {
                    for (uint32 jointIndex = 0; jointIndex < jointCount; ++jointIndex) {
                        const int nodeIdx = nodeIndexOf(skin.joints[jointIndex]);
                        if (nodeIdx >= 0)
                            skeleton->JointNodes[jointIndex] = static_cast<uint32>(nodeIdx);
                    }
                }

                if (skin.skeleton) {
                    const int skeletonRootIndex = nodeIndexOf(skin.skeleton);
                    if (skeletonRootIndex >= 0)
                        storeColumnMajor(restWorld[static_cast<size_t>(skeletonRootIndex)], skeleton->SkeletonRootWorld);
                }

                int meshRoot = -1;
                for (size_t nodeIndex = 0; nodeIndex < gltf->nodes_count; ++nodeIndex) {
                    const cgltf_node& node = gltf->nodes[nodeIndex];
                    if (node.mesh && node.skin == &skin) {
                        meshRoot = static_cast<int>(nodeIndex);
                        break;
                    }
                }
                skeleton->MeshRootNode = meshRoot;
                if (meshRoot >= 0)
                    storeColumnMajor(restWorld[static_cast<size_t>(meshRoot)], skeleton->MeshRootWorld);

                // Per-world runtime state (CompactSkinMatrices, atlas offset) is
                // lazily initialized by SkeletonData::GetOrCreateRuntime().
            }

            if (gltf->animations_count > 0)
            {
                m_HasAnimations = true;
                m_AnimationNames.reserve(gltf->animations_count);
                m_EmbeddedClipGuids.reserve(gltf->animations_count);
                for (size_t ai = 0; ai < gltf->animations_count; ++ai)
                {
                    const cgltf_animation& a = gltf->animations[ai];
                    if (a.name && a.name[0])
                        m_AnimationNames.emplace_back(a.name);
                    else
                        m_AnimationNames.emplace_back("Animation " + std::to_string(ai));

                    // Eagerly load + register the embedded clip into the
                    // runtime ClipStore under a stable derived GUID — same
                    // policy as the FBX loader. See ModelAssetLoadFbx.cpp.
                    const GUID clipGuid = MintEmbeddedClipGuid(static_cast<uint32>(ai));

                    auto clip = std::make_shared<AnimationClip>(clipGuid, GetPath());
                    clip->SetSelectedAnimationIndex(static_cast<uint32>(ai));
                    clip->SetSourceInfo(GetPath(), static_cast<uint32>(ai));
                    // Parse from the bytes this load already holds (see LoadFBX).
                    if (clip->LoadFromData(data) && clip->GetDuration() > 0.0f)
                    {
                        StageOrPublishRuntimeClip(clipGuid, std::move(clip));
                    }
                    else
                    {
                        Logger::Log::Warning("ModelAsset: failed to load embedded glTF clip {} of '{}'",
                                             ai, GetName());
                    }
                }
            }

            skeleton->ComputeTopologicalSort();
        }
    }

    // Extract images from glTF (GLB buffer views, data URIs, or external file URIs).
    m_EmbeddedImages.clear();
    if (gltf->images_count > 0) {
        const auto modelDir = GetPath().parent_path();
        m_EmbeddedImages.reserve(gltf->images_count);
        for (size_t i = 0; i < gltf->images_count; ++i) {
            const cgltf_image& img = gltf->images[i];
            EmbeddedImage ei{};
            ei.MimeType = img.mime_type ? img.mime_type : "";
            if (img.buffer_view && img.buffer_view->buffer && img.buffer_view->buffer->data) {
                // Embedded in GLB binary buffer
                const auto* bv = img.buffer_view;
                if (bv->offset + bv->size <= bv->buffer->size) {
                    const uint8_t* base = static_cast<const uint8_t*>(bv->buffer->data) + bv->offset;
                    ei.Data.assign(base, base + bv->size);
                }
            }
            else if (img.uri && img.uri[0] != '\0') {
                const std::string_view uriView(img.uri);
                if (uriView.substr(0, 5) == "data:") {
                    if (!DecodeDataUriImage(uriView, ei))
                        Logger::Log::Warning("glTF '{}': image {} has a data URI that is not standard base64 "
                                             "(data:<type>;base64,<payload>), so it imports without its bytes; "
                                             "re-export the file with base64-embedded or external images.",
                                             GetPath().generic_string(), i);
                }
                else {
                    // External file URI — resolve relative to the .gltf file directory
                    auto texPath = modelDir / img.uri;
                    std::error_code ec;
                    // A file the AssetDatabase already tracks is referenced as
                    // that asset, not copied into the model: its import settings
                    // (mips, compression, colour space, filtering) then apply,
                    // and one GPU texture serves every model that shares it.
                    if (AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager()) {
                        const GUID assetGuid = assetManager->ResolveAssetGuid(texPath);
                        if (!assetGuid.IsNull()) {
                            ei.AssetGuid = assetGuid;
                            m_EmbeddedImages.push_back(std::move(ei));
                            continue;
                        }
                    }
                    if (std::filesystem::exists(texPath, ec)) {
                        if (ReadFileBytesShared(texPath, ei.Data) && !ei.Data.empty()) {
                            Logger::Log::Info("LoadGLTF: loaded external texture '{}' ({} bytes)", texPath.string(), ei.Data.size());
                        }
                        if (ei.MimeType.empty())
                            ei.MimeType = MimeFromTextureExtension(texPath);
                    }
                    else {
                        Logger::Log::Warning("LoadGLTF: external texture not found: '{}' (modelDir='{}')", texPath.string(), modelDir.string());
                    }
                }
            }
            m_EmbeddedImages.push_back(std::move(ei));
        }
    }

    // Helper: get the image index for a cgltf_texture_view (returns -1 if none).
    auto imageIndexOf = [&](const cgltf_texture_view& tv) -> int {
        if (!tv.texture || !tv.texture->image) return -1;
        auto idx = static_cast<int>(tv.texture->image - gltf->images);
        if (idx < 0 || idx >= static_cast<int>(gltf->images_count)) return -1;
        return idx;
    };

    // Extract materials (glTF-level)
    m_Materials.clear();
    if (gltf->materials_count > 0) {
        m_Materials.reserve(gltf->materials_count);
        for (size_t i = 0; i < gltf->materials_count; ++i) {
            const cgltf_material& m = gltf->materials[i];
            ImportedMaterialData mat{};
            mat.Name = m.name ? m.name : "Material";
            mat.DoubleSided = m.double_sided;
            switch (m.alpha_mode) {
                case cgltf_alpha_mode_mask:  mat.AlphaMode = AlphaMode::Mask;  break;
                case cgltf_alpha_mode_blend: mat.AlphaMode = AlphaMode::Blend; break;
                default:                      mat.AlphaMode = AlphaMode::Opaque; break;
            }
            mat.AlphaCutoff = (float)m.alpha_cutoff;

            // Populate basic PBR parameters from glTF metallic-roughness block
            const cgltf_pbr_metallic_roughness& pbr = m.pbr_metallic_roughness;
            // baseColorFactor is RGBA
            mat.DiffuseColor[0] = (float)pbr.base_color_factor[0];
            mat.DiffuseColor[1] = (float)pbr.base_color_factor[1];
            mat.DiffuseColor[2] = (float)pbr.base_color_factor[2];
            mat.DiffuseColor[3] = (float)pbr.base_color_factor[3];
            mat.Metallic = (float)pbr.metallic_factor;
            mat.Roughness = (float)pbr.roughness_factor;

            // Emission per the glTF specification: emissiveFactor (default black) times
            // the sRGB emissiveTexture (white when absent, alpha ignored) times
            // KHR_materials_emissive_strength (default 1). The bridge maps 1.0 to
            // reference white.
            const float emissiveStrength = m.has_emissive_strength ? m.emissive_strength.emissive_strength : 1.0f;
            for (int c = 0; c < 3; ++c)
                mat.EmissiveColor[c] = m.emissive_factor[c] * emissiveStrength;

            // Texture references (stored as "__embedded:<imageIndex>").
            AssignEmbeddedTextureRefs(mat, m_EmbeddedImages,
                imageIndexOf(pbr.base_color_texture),
                imageIndexOf(m.normal_texture),
                imageIndexOf(pbr.metallic_roughness_texture),
                imageIndexOf(m.emissive_texture),
                imageIndexOf(m.occlusion_texture));
            // The surfaces have no occlusion strength input: the map applies at full strength.
            if (m.occlusion_texture.texture && m.occlusion_texture.scale != 1.0f)
                Logger::Log::Info("glTF '{}': material '{}' sets occlusion strength {}, which this engine ignores; "
                                  "its occlusion map applies at full strength.",
                                  GetPath().string(), mat.Name, m.occlusion_texture.scale);

            m_Materials.push_back(std::move(mat));
        }
    } else {
        ImportedMaterialData mat{}; mat.Name = "DefaultMaterial"; mat.DiffuseColor[0]=mat.DiffuseColor[1]=mat.DiffuseColor[2]=0.8f; mat.DiffuseColor[3]=1.0f; m_Materials.push_back(mat);
    }

    // Assemble MSFT_lod authored LOD chains (the highest-priority authored source)
    // BEFORE the by-material merge — which would otherwise flatten the co-located
    // LOD alternates into one mesh — and before ConsumeLodSuffixFamilies runs on
    // return (MSFT_lod wins the source priority). cgltf v1.15 does not decode
    // MSFT_lod, so each MSFT_lod node arrives as a raw-JSON extension blob; the
    // lower-detail nodes' submeshes are consumed into the LOD0 node's submeshes and
    // never emitted standalone. Coverage comes from the LOD0 node's extras.
    if (gltf->nodes_count > 0)
    {
        Vector<MsftLodGroup> msftGroups;
        for (size_t ni = 0; ni < gltf->nodes_count; ++ni)
        {
            const cgltf_node& node = gltf->nodes[ni];
            const cgltf_extension* lodExt = nullptr;
            for (size_t ei = 0; ei < node.extensions_count; ++ei)
                if (node.extensions[ei].name &&
                    std::strcmp(node.extensions[ei].name, "MSFT_lod") == 0)
                {
                    lodExt = &node.extensions[ei];
                    break;
                }
            if (!lodExt || !lodExt->data)
                continue;

            Vector<int32> ids;
            if (!ParseMsftLodIds(lodExt->data, ids))
            {
                Logger::Log::Warning("LoadGLTF[{}]: node {} MSFT_lod blob malformed or empty; "
                                     "skipping.", GetName(), ni);
                continue;
            }

            MsftLodGroup group;
            group.Lod0Node = static_cast<int32>(ni);
            group.LowerNodes = std::move(ids);
            if (node.extras.data)
                ParseMsftScreenCoverage(node.extras.data, group.Coverage);
            msftGroups.push_back(std::move(group));
        }
        if (!msftGroups.empty())
            AssembleMsftLodChains(m_Meshes, msftGroups, GetName());
    }

    // Merge static primitives sharing a material into single meshes when the
    // model has no skinning data and no embedded animations. Drops thousands
    // of tiny draws to ~one-per-material for high-fragmentation assets
    // (architectural / city dumps).
    //
    // Excluding skins_count > 0 (not just animations) is important: a glTF
    // exported with a leftover armature can have no animations but still
    // bind non-skinned meshes to nodes via AnimatedNodeRef in
    // ModelEntityFactory. Merging those drops the SourceNodeIndex link
    // and silently breaks any animator hot-attached later.
    if (gltf->animations_count == 0 && gltf->skins_count == 0)
        MergeStaticPrimitivesByMaterial(m_Meshes);

    return !m_Meshes.empty();
#else
    Logger::Log::Warning("GLTF/GLB loading requires cgltf. Falling back: {}", GetName());
    return false;
#endif
}

} // namespace GameEngine
