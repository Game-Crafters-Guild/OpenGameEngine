#include "Assets/ModelAsset.h"
#include "AssetCore/SharedFileRead.h"
#include "Assets/AlphaCutoffThreshold.h"
#include "Assets/AlphaUVFootprint.h"
#include "Assets/AnimationClip.h"
#include "Assets/AuthoredLodImport.h"
#include "Assets/FbxLoaderOptions.h"
#include "Assets/Textures/TextureMimeType.h"
#include "FbxAxisConversion.h"
#include "FbxEmission.h"
#include "SparseMorphTarget.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"

#include "ECSModules/Rendering/SkeletonStore.h"

#include <ufbx.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(GE_HAVE_STB)
  #include <stb_image.h>
  #include <stb_image_write.h>
#endif

namespace GameEngine
{

namespace
{

// Engine convention is +X right, +Y up, +Z forward, left-handed. FBX files
// can be Y-up or Z-up (and can disagree about which axis is "front"), so we
// load raw ufbx data and apply one consistent source->engine conversion to
// vertices, bone TRS, IBMs, and animation keys. Keeping the conversion on our
// side avoids ufbx splitting axis normalization between root nodes and
// geometry, which would mismatch during skinning evaluation.
//
// AnimationClip.cpp uses this same ModelImport conversion. Do not fork it.

using FbxAxisConversion = ModelImport::AxisConversion;

ufbx_vec3 ConvertFbxVec3(const ufbx_vec3& v, const FbxAxisConversion& c, float scale)
{
    return FbxImport::ConvertVec3(v, c, scale);
}

struct VertexDedupKey
{
    uint32_t pos = 0;
    uint32_t nrm = 0;
    uint32_t uv0 = 0;
    uint32_t uv1 = 0;
    uint32_t uv2 = 0;
    uint32_t uv3 = 0;
    uint32_t uv4 = 0;
    uint32_t uv5 = 0;
    uint32_t uv6 = 0;
    uint32_t uv7 = 0;
    uint32_t tangent = 0;
    uint32_t bitangent = 0;
    uint32_t color = 0;

    bool operator==(const VertexDedupKey& other) const
    {
        return pos == other.pos
            && nrm == other.nrm
            && uv0 == other.uv0
            && uv1 == other.uv1
            && uv2 == other.uv2
            && uv3 == other.uv3
            && uv4 == other.uv4
            && uv5 == other.uv5
            && uv6 == other.uv6
            && uv7 == other.uv7
            && tangent == other.tangent
            && bitangent == other.bitangent
            && color == other.color;
    }
};

struct VertexDedupKeyHash
{
    size_t operator()(const VertexDedupKey& key) const noexcept
    {
        size_t h = 0;
        auto mix = [&h](uint32_t v)
        {
            h ^= std::hash<uint32_t>{}(v) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        };
        mix(key.pos);
        mix(key.nrm);
        mix(key.uv0);
        mix(key.uv1);
        mix(key.uv2);
        mix(key.uv3);
        mix(key.uv4);
        mix(key.uv5);
        mix(key.uv6);
        mix(key.uv7);
        mix(key.tangent);
        mix(key.bitangent);
        mix(key.color);
        return h;
    }
};

ufbx_vec3 ConvertFbxScale(const ufbx_vec3& v, const FbxAxisConversion& c)
{
    return FbxImport::ConvertScale(v, c);
}

ufbx_quat ConvertFbxQuat(const ufbx_quat& q, const FbxAxisConversion& c)
{
    return FbxImport::ConvertQuat(q, c);
}

void StoreUfbxMatrixAsColumnMajorTargetSpace(const ufbx_matrix& m,
                                             const FbxAxisConversion& c,
                                             float s,
                                             float* out16)
{
    FbxImport::StoreMatrixColumnMajor(m, c, s, out16);
}

void StoreIdentityColumnMajor(float* out16)
{
    std::memset(out16, 0, 16 * sizeof(float));
    out16[0] = out16[5] = out16[10] = out16[15] = 1.0f;
}

struct SkeletonCleanupStats
{
    uint32 parentFixes = 0;
    uint32 translationFixes = 0;
    uint32 rotationFixes = 0;
    uint32 scaleFixes = 0;
    uint32 matrixFixes = 0;
};

SkeletonCleanupStats SanitizeImportedSkeleton(std::vector<int32>& parent,
                                               std::vector<float>& restT,
                                               std::vector<float>& restR,
                                               std::vector<float>& restS,
                                               std::vector<float>& restLocalRM,
                                               uint32 boneCount)
{
    SkeletonCleanupStats stats{};

    for (uint32 i = 0; i < boneCount && i < parent.size(); ++i)
    {
        const int32 p = parent[i];
        if (p < 0)
            continue;
        if (p == static_cast<int32>(i) || static_cast<uint32>(p) >= boneCount)
        {
            parent[i] = -1;
            ++stats.parentFixes;
            continue;
        }

        int32 cursor = p;
        uint32 depth = 0;
        bool cycle = false;
        while (cursor >= 0)
        {
            if (static_cast<uint32>(cursor) >= boneCount || depth++ > boneCount)
            {
                cycle = true;
                break;
            }
            if (cursor == static_cast<int32>(i))
            {
                cycle = true;
                break;
            }
            cursor = parent[static_cast<size_t>(cursor)];
        }
        if (cycle)
        {
            parent[i] = -1;
            ++stats.parentFixes;
        }
    }

    for (uint32 i = 0; i < boneCount; ++i)
    {
        for (uint32 c = 0; c < 3 && i * 3u + c < restT.size(); ++c)
        {
            float& v = restT[i * 3u + c];
            if (!std::isfinite(v))
            {
                v = 0.0f;
                ++stats.translationFixes;
            }
        }

        if (i * 4u + 3 < restR.size())
        {
            float* q = restR.data() + i * 4u;
            const float lenSq = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
            if (!std::isfinite(lenSq) || lenSq < 1e-12f)
            {
                q[0] = q[1] = q[2] = 0.0f;
                q[3] = 1.0f;
                ++stats.rotationFixes;
            }
            else
            {
                const float invLen = 1.0f / std::sqrt(lenSq);
                q[0] *= invLen;
                q[1] *= invLen;
                q[2] *= invLen;
                q[3] *= invLen;
            }
        }

        for (uint32 c = 0; c < 3 && i * 3u + c < restS.size(); ++c)
        {
            float& v = restS[i * 3u + c];
            if (!std::isfinite(v) || std::abs(v) < 1e-8f)
            {
                v = 1.0f;
                ++stats.scaleFixes;
            }
        }

        if (i * 16u + 15 < restLocalRM.size())
        {
            float* m = restLocalRM.data() + i * 16u;
            bool valid = true;
            for (uint32 c = 0; c < 16; ++c)
                valid = valid && std::isfinite(m[c]);
            if (!valid)
            {
                StoreIdentityColumnMajor(m);
                ++stats.matrixFixes;
            }
            else
            {
                m[3] = m[7] = m[11] = 0.0f;
                m[15] = 1.0f;
            }
        }
    }

    return stats;
}

void AddVertexInfluence(uint16_t* jointOut, float* weightOut, size_t maxInfluences, uint32 boneIndex, float w)
{
    if (w <= 1e-8f)
        return;

    size_t smallest = 0;
    for (size_t k = 1; k < maxInfluences; ++k)
    {
        if (weightOut[k] < weightOut[smallest])
            smallest = k;
    }

    float sum = 0.0f;
    for (size_t k = 0; k < maxInfluences; ++k)
        sum += weightOut[k];
    if (sum < 1e-6f)
    {
        for (size_t k = 0; k < maxInfluences; ++k)
        {
            if (weightOut[k] < 1e-6f)
            {
                jointOut[k] = static_cast<uint16_t>(boneIndex);
                weightOut[k] = w;
                return;
            }
        }
    }

    if (w > weightOut[smallest])
    {
        jointOut[smallest] = static_cast<uint16_t>(boneIndex);
        weightOut[smallest] = w;
    }
}

void NormalizeWeights(float* w, size_t maxInfluences)
{
    float sum = 0.0f;
    for (size_t k = 0; k < maxInfluences; ++k)
        sum += w[k];
    if (sum > 1e-6f)
    {
        const float inv = 1.0f / sum;
        for (size_t k = 0; k < maxInfluences; ++k)
            w[k] *= inv;
    }
    else
    {
        w[0] = 1.0f;
        for (size_t k = 1; k < maxInfluences; ++k)
            w[k] = 0.0f;
    }
}

// FBX texture paths are notoriously unreliable: most exporters embed the
// authoring machine's absolute path (Dropbox/network share/etc.) which won't
// exist on the build machine, plus a relative path that's correct only if
// the artist's working layout was preserved. Real-world game assets often
// store textures in a shared `Textures/` folder several directory levels
// away from the FBX itself (e.g. POLYGON_ElvenRealm/Textures/* with the
// FBXes in POLYGON_ElvenRealm/FBX/Characters/...).
//
// Resolution strategy, applied in order until a hit:
//   1. The absolute path written into the FBX (works on the authoring machine).
//   2. `<modelDir>/<relativePath>` and `<modelDir>/<ufbx-resolved-filename>`.
//   3. Walk up from `modelDir` toward the filesystem root and at each level
//      recursively scan known texture-bearing directory names (Textures/,
//      Materials/, Maps/) for the basename. Extension-swap so a `.psd`
//      authored reference picks up the shipped `.png/.jpg/.tga` instead.
//
// The walk-up + recursive-search step is bounded (max upward levels and max
// recursion depth) so a misconfigured load doesn't traverse the whole disk.

namespace
{
constexpr int    kTextureSearchMaxAscend = 8; // levels above modelDir to walk
constexpr int    kTextureSearchMaxRecurse = 4; // depth into Textures/ etc.
constexpr const char* const kTextureBearingDirs[] = {
    "Textures", "textures", "Texture", "texture",
    "Materials", "materials", "Material", "material",
    "Maps", "maps", "Map", "map",
};
constexpr const char* const kTextureExtensionFallbacks[] = {
    ".png", ".jpg", ".jpeg", ".tga", ".tif", ".tiff", ".bmp", ".hdr", ".exr",
};

bool BasenameMatchesIgnoringExtension(const std::filesystem::path& candidate,
                                      const std::string& wantStem)
{
    std::string stem = candidate.stem().string();
    if (stem.size() != wantStem.size())
        return false;
    for (size_t i = 0; i < stem.size(); ++i)
    {
        const unsigned char a = static_cast<unsigned char>(stem[i]);
        const unsigned char b = static_cast<unsigned char>(wantStem[i]);
        if (std::tolower(a) != std::tolower(b))
            return false;
    }
    return true;
}

std::filesystem::path FindTextureRecursive(const std::filesystem::path& root,
                                            const std::string& wantStem,
                                            int depthRemaining)
{
    if (depthRemaining < 0)
        return {};
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec))
        return {};

    // First pass: files in this directory.
    for (const auto& entry : std::filesystem::directory_iterator(root, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file(ec))
            continue;
        if (BasenameMatchesIgnoringExtension(entry.path(), wantStem))
            return entry.path();
    }
    // Second pass: descend into subdirectories.
    if (depthRemaining > 0)
    {
        for (const auto& entry : std::filesystem::directory_iterator(root, ec))
        {
            if (ec) break;
            if (!entry.is_directory(ec))
                continue;
            if (auto found = FindTextureRecursive(entry.path(), wantStem, depthRemaining - 1); !found.empty())
                return found;
        }
    }
    return {};
}

// Fallback: pick a file whose stem starts with `wantStem` followed by an
// underscore-separated suffix (e.g. wantStem="Wood_01" matches "Wood_01_A",
// "Wood_01_diffuse", "Wood_01_ashe"). Used as a last resort to handle the
// common artist convention where a referenced base name has shipped variants
// under suffixed names. Returns the alphabetically first match for
// determinism. Caller is responsible for calling this only when no exact
// stem match exists, and only for slots where guessing is acceptable
// (typically the base/diffuse texture).
bool IsTextureFileExtension(const std::filesystem::path& p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* known : kTextureExtensionFallbacks)
        if (ext == known) return true;
    return false;
}

std::filesystem::path FindTextureByStemPrefix(const std::filesystem::path& root,
                                                const std::string& wantStem,
                                                int depthRemaining)
{
    if (depthRemaining < 0)
        return {};
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec))
        return {};

    auto matchesPrefix = [&](const std::filesystem::path& p) -> bool
    {
        if (!IsTextureFileExtension(p))
            return false;
        const std::string stem = p.stem().string();
        if (stem.size() <= wantStem.size())
            return false;
        // Case-insensitive prefix check.
        for (size_t i = 0; i < wantStem.size(); ++i)
        {
            const unsigned char a = static_cast<unsigned char>(stem[i]);
            const unsigned char b = static_cast<unsigned char>(wantStem[i]);
            if (std::tolower(a) != std::tolower(b))
                return false;
        }
        // Require an underscore (or '.') separator after the prefix so we
        // don't match unrelated filenames that happen to share a prefix.
        const char sep = stem[wantStem.size()];
        return sep == '_' || sep == '-' || sep == '.';
    };

    std::filesystem::path best;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec))
    {
        if (ec) break;
        if (!entry.is_regular_file(ec))
            continue;
        if (matchesPrefix(entry.path()))
        {
            if (best.empty() || entry.path().filename().string() < best.filename().string())
                best = entry.path();
        }
    }
    if (!best.empty())
        return best;

    if (depthRemaining > 0)
    {
        for (const auto& entry : std::filesystem::directory_iterator(root, ec))
        {
            if (ec) break;
            if (!entry.is_directory(ec))
                continue;
            if (auto found = FindTextureByStemPrefix(entry.path(), wantStem, depthRemaining - 1); !found.empty())
                return found;
        }
    }
    return {};
}
} // namespace

std::filesystem::path ResolveTextureOnDisk(const ufbx_texture* tex,
                                            const std::filesystem::path& modelDir)
{
    if (!tex)
        return {};

    auto tryPath = [](const std::filesystem::path& p) -> std::filesystem::path
    {
        if (p.empty()) return {};
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
        return {};
    };

    auto stringPath = [](const ufbx_string& s) -> std::string
    {
        return std::string(s.data ? s.data : "", s.length);
    };

    // 1. Absolute path the FBX was exported with.
    if (tex->absolute_filename.length > 0)
    {
        if (auto p = tryPath(stringPath(tex->absolute_filename)); !p.empty())
            return p;
    }
    // 2. Relative path resolved against the model directory.
    if (!modelDir.empty() && tex->relative_filename.length > 0)
    {
        if (auto p = tryPath(modelDir / stringPath(tex->relative_filename)); !p.empty())
            return p;
    }
    // 3. ufbx's resolved `filename` (already relative to load opts).
    if (!modelDir.empty() && tex->filename.length > 0)
    {
        if (auto p = tryPath(modelDir / stringPath(tex->filename)); !p.empty())
            return p;
        if (auto p = tryPath(stringPath(tex->filename)); !p.empty())
            return p;
    }
    if (modelDir.empty() || tex->filename.length == 0)
        return {};

    const std::filesystem::path file = std::filesystem::path(stringPath(tex->filename)).filename();
    if (auto p = tryPath(modelDir / file); !p.empty())
        return p;

    // 4. Walk up the directory tree. At each level look for one of the
    // known texture-bearing directory names and recursively search inside.
    // Try the original filename first, then extension-swapped variants
    // (e.g. .psd → .png) since shipped assets are usually game-ready.
    const std::string wantStem = file.stem().string();
    std::filesystem::path level = modelDir;
    for (int up = 0; up <= kTextureSearchMaxAscend; ++up)
    {
        for (const char* sub : kTextureBearingDirs)
        {
            const std::filesystem::path texDir = level / sub;
            // Direct hit: <texDir>/<filename>
            if (auto p = tryPath(texDir / file); !p.empty())
                return p;
            // Extension-swap: <texDir>/<stem><ext> for each known extension
            for (const char* extAlt : kTextureExtensionFallbacks)
            {
                if (auto p = tryPath(texDir / (wantStem + extAlt)); !p.empty())
                    return p;
            }
            // Recursive fuzzy search by stem (any extension, any subdir).
            if (auto p = FindTextureRecursive(texDir, wantStem, kTextureSearchMaxRecurse); !p.empty())
                return p;
        }

        if (!level.has_parent_path() || level.parent_path() == level)
            break;
        level = level.parent_path();
    }

    // 5. Last-resort prefix match: many DCC / asset-pack workflows ship
    // texture *variants* (e.g. `Wood_01_A.png`, `Wood_01_B.png`) where the
    // FBX references either the un-suffixed name or a non-shipped variant.
    // We try two stem candidates in order of specificity:
    //   a. The full requested stem — e.g. FBX says `Wood_01`, files are
    //      `Wood_01_A`, `Wood_01_B`. Prefix-match picks any with a
    //      `Wood_01_*` name.
    //   b. The stem with its trailing `_<suffix>` stripped — e.g. FBX says
    //      `Wood_01_ashe`, but only `Wood_01_A` ships. Strip `_ashe` to get
    //      `Wood_01`, then prefix-match on that.
    // This is loose so it can pick up the wrong file in pathological cases;
    // it's the very last fallback before giving up.
    auto stripTrailingSuffix = [](const std::string& s) -> std::string
    {
        const auto lastUs = s.rfind('_');
        if (lastUs == std::string::npos || lastUs == 0)
            return {};
        return s.substr(0, lastUs);
    };

    std::string prefixCandidates[2] = { wantStem, stripTrailingSuffix(wantStem) };
    for (const std::string& candidateStem : prefixCandidates)
    {
        if (candidateStem.empty())
            continue;
        level = modelDir;
        for (int up = 0; up <= kTextureSearchMaxAscend; ++up)
        {
            for (const char* sub : kTextureBearingDirs)
            {
                const std::filesystem::path texDir = level / sub;
                if (auto p = FindTextureByStemPrefix(texDir, candidateStem, kTextureSearchMaxRecurse); !p.empty())
                    return p;
            }
            if (!level.has_parent_path() || level.parent_path() == level)
                break;
            level = level.parent_path();
        }
    }
    return {};
}

// Append (or reuse) an embedded image entry for `tex`. Returns the index in
// `outImages`, or -1 if nothing could be loaded.
int AppendEmbeddedTextureForUfbx(const ufbx_texture* tex,
                                  const std::filesystem::path& modelDir,
                                  Vector<EmbeddedImage>& outImages,
                                  std::unordered_map<const ufbx_texture*, int>& cache,
                                  std::unordered_map<std::string, int>& diskCache)
{
    if (!tex)
        return -1;
    if (auto it = cache.find(tex); it != cache.end())
        return it->second;

    // Embedded blob inside the FBX itself.
    if (tex->content.size > 0 && tex->content.data != nullptr)
    {
        EmbeddedImage ei{};
        const auto* bytes = static_cast<const uint8_t*>(tex->content.data);
        ei.Data.assign(bytes, bytes + tex->content.size);
        ei.MimeType = tex->filename.length > 0
            ? MimeFromTextureExtension(std::filesystem::path(std::string(tex->filename.data, tex->filename.length)))
            : "image/png";
        const int idx = static_cast<int>(outImages.size());
        outImages.push_back(std::move(ei));
        cache.emplace(tex, idx);
        return idx;
    }

    // External file on disk.
    const std::filesystem::path resolved = ResolveTextureOnDisk(tex, modelDir);
    if (resolved.empty())
    {
        cache.emplace(tex, -1);
        return -1;
    }

    std::error_code ec;
    const std::string canonical = std::filesystem::absolute(resolved, ec).string();
    if (auto it = diskCache.find(canonical); it != diskCache.end())
    {
        cache.emplace(tex, it->second);
        return it->second;
    }

    EmbeddedImage ei{};
    if (!ReadFileBytesShared(resolved, ei.Data))
    {
        Logger::Log::Warning("FBX: could not read external texture '{}'", resolved.string());
        cache.emplace(tex, -1);
        return -1;
    }
    if (ei.Data.empty())
    {
        Logger::Log::Warning("FBX: external texture is empty '{}'", resolved.string());
        cache.emplace(tex, -1);
        return -1;
    }
    ei.MimeType = MimeFromTextureExtension(resolved);
    const int idx = static_cast<int>(outImages.size());
    outImages.push_back(std::move(ei));
    cache.emplace(tex, idx);
    diskCache.emplace(canonical, idx);
    return idx;
}

#if defined(GE_HAVE_STB)
void WritePngBytes(void* context, void* data, int size)
{
    auto* bytes = static_cast<Vector<uint8>*>(context);
    const auto* src = static_cast<const uint8*>(data);
    bytes->insert(bytes->end(), src, src + size);
}

int MergeAlphaTextureIntoAlbedo(Vector<EmbeddedImage>& images,
                                int albedoIndex,
                                int alphaIndex,
                                AlphaMode& alphaMode)
{
    if (albedoIndex < 0 || alphaIndex < 0 ||
        albedoIndex >= static_cast<int>(images.size()) ||
        alphaIndex >= static_cast<int>(images.size()) ||
        albedoIndex == alphaIndex)
        return albedoIndex;

    int aw = 0, ah = 0, ac = 0;
    int mw = 0, mh = 0, mc = 0;
    stbi_uc* albedo = stbi_load_from_memory(
        images[static_cast<size_t>(albedoIndex)].Data.data(),
        static_cast<int>(images[static_cast<size_t>(albedoIndex)].Data.size()),
        &aw, &ah, &ac, 4);
    stbi_uc* mask = stbi_load_from_memory(
        images[static_cast<size_t>(alphaIndex)].Data.data(),
        static_cast<int>(images[static_cast<size_t>(alphaIndex)].Data.size()),
        &mw, &mh, &mc, 4);
    if (!albedo || !mask || aw <= 0 || ah <= 0 || aw != mw || ah != mh)
    {
        if (albedo) stbi_image_free(albedo);
        if (mask) stbi_image_free(mask);
        return albedoIndex;
    }

    bool onlyCutoutValues = true;
    const size_t pixelCount = static_cast<size_t>(aw) * static_cast<size_t>(ah);
    for (size_t i = 0; i < pixelCount; ++i)
    {
        const uint8 alpha = mask[i * 4u];
        albedo[i * 4u + 3u] = alpha;
        if (alpha != 0u && alpha != 255u)
            onlyCutoutValues = false;
    }

    Vector<uint8> encoded;
    stbi_write_png_to_func(WritePngBytes, &encoded, aw, ah, 4, albedo, aw * 4);
    stbi_image_free(albedo);
    stbi_image_free(mask);
    if (encoded.empty())
        return albedoIndex;

    EmbeddedImage merged{};
    merged.Data = std::move(encoded);
    merged.MimeType = "image/png";
    const int mergedIndex = static_cast<int>(images.size());
    images.push_back(std::move(merged));
    alphaMode = onlyCutoutValues ? AlphaMode::Mask : AlphaMode::Blend;
    return mergedIndex;
}
#endif

void AssignEmbeddedRef(String& outSlot, int embeddedIndex)
{
    if (embeddedIndex >= 0)
        outSlot = std::string(kEmbeddedTexturePrefix) + std::to_string(static_cast<unsigned>(embeddedIndex));
}

#if defined(GE_HAVE_STB)
// Footprint demotion for inferred Mask. MergeAlphaTextureIntoAlbedo classifies
// from whole-texture content, so a shared atlas with cutout regions (e.g.
// Synty foliage) forces Mask onto every material referencing it — including
// architecture whose meshes never touch a cutout texel. Alpha test that can
// never discard still pays discard-mode depth rasterization and is barred
// from the material-independent depth classes, so when every texel reachable
// through the meshes' diffuse-UV footprint is opaque above the cutoff, the
// material is demoted to Opaque. FBX-only on purpose: Mask here is inferred
// from texture content, while glTF alpha modes are authored intent.
//
// Conservative by construction: the footprint is dilated for filtered
// sampling, the threshold sits a safety margin above the cutoff, base-color
// alpha attenuation (and vertex-color alpha attenuation on a material that
// reads vertex color) disables demotion, and any analysis doubt
// keeps Mask. Residual: at heavy minification the Mask path could discard
// where atlas neighbors bleed into coarse mips (eroding opaque geometry at a
// distance); demoted materials no longer erode, which is the intended output
// for genuinely opaque surfaces.

constexpr float kFootprintDilationTexels = 4.0f;
constexpr float kOpaqueAlphaAttenuationMin = 0.999f;
constexpr float kMaxDemotableAlphaCutoff = 0.99f;

// Pre-existing margin, deliberately NOT re-derived here. The alpha-envelope
// measurement found source->cooked alpha drops of
// up to 29 steps on realistic content (and 105 on adversarial content) through
// the PNG -> stbir mips -> DirectXTex BC7 cook that feeds THIS site, so 8 does
// not bound it. Raising it changes model-import alpha modes project-wide,
// which is outside the branch that found it; recorded here as a known gap
// rather than silently inherited as validated.
constexpr uint32 kAlphaCutoffSafetyMargin = 8; // 8-bit alpha steps above the cutoff

int ParseEmbeddedTextureIndex(const String& textureRef)
{
    const std::string_view ref(textureRef.c_str(), textureRef.size());
    if (ref.size() <= kEmbeddedTexturePrefixLen ||
        ref.compare(0, kEmbeddedTexturePrefixLen, kEmbeddedTexturePrefix) != 0)
        return -1;
    int value = 0;
    for (const char ch : ref.substr(kEmbeddedTexturePrefixLen))
    {
        if (ch < '0' || ch > '9')
            return -1;
        value = value * 10 + (ch - '0');
    }
    return value;
}

uint32 DemoteMaskMaterialsWithOpaqueUVFootprint(const Vector<Mesh>& meshes,
                                                Vector<ImportedMaterialData>& materials,
                                                const Vector<EmbeddedImage>& images)
{
    // Shared atlases are the norm (Synty ships one per pack): decode each
    // embedded image once and memoize its alpha plane + min alpha across the
    // material loop. MinAlpha answers every whole-plane question in O(1): a
    // fully-opaque plane accepts a material without rasterizing a single
    // triangle, and TriangleFootprintIsOpaque's whole-plane fallbacks stop
    // re-scanning the image per triangle.
    struct DecodedAlphaPlane
    {
        stbi_uc* Rgba = nullptr; // null: decode failed (material keeps Mask)
        AlphaPlaneView Plane;
        uint8 MinAlpha = 0;
    };
    std::unordered_map<int, DecodedAlphaPlane> decodedPlanes;
    auto decodedPlaneFor = [&images, &decodedPlanes](int imageIndex) -> const DecodedAlphaPlane&
    {
        auto [it, inserted] = decodedPlanes.try_emplace(imageIndex);
        DecodedAlphaPlane& entry = it->second;
        if (!inserted)
            return entry;
        const EmbeddedImage& image = images[static_cast<size_t>(imageIndex)];
        int width = 0;
        int height = 0;
        int components = 0;
        stbi_uc* rgba = stbi_load_from_memory(image.Data.data(),
                                              static_cast<int>(image.Data.size()),
                                              &width, &height, &components, 4);
        if (!rgba)
            return entry;
        if (width <= 0 || height <= 0)
        {
            stbi_image_free(rgba);
            return entry;
        }
        entry.Rgba = rgba;
        entry.Plane = AlphaPlaneView{rgba + 3,
                                     static_cast<uint32>(width),
                                     static_cast<uint32>(height),
                                     4u};
        entry.MinAlpha = PlaneMinAlpha(entry.Plane);
        return entry;
    };

    uint32 demoted = 0;
    for (size_t mi = 0; mi < materials.size(); ++mi)
    {
        ImportedMaterialData& material = materials[mi];
        // Only merge-inferred Mask qualifies: authored alpha modes (glTF) are
        // intent, and this pass also runs from PostLoad where all loaders'
        // materials are visible.
        if (material.AlphaMode != AlphaMode::Mask || !material.AlphaModeInferred)
            continue;
        // The shader multiplies texture alpha by base-color alpha, and by
        // vertex-color alpha only when the material reads vertex color; any
        // attenuation could pull an opaque texel under the cutoff.
        if (material.DiffuseColor[3] < kOpaqueAlphaAttenuationMin)
            continue;
        // Near-one cutoffs sit inside float-filtering error of fully-opaque
        // texels; parity with the material-registration gate, which refuses
        // to reason about them.
        if (std::clamp(material.AlphaCutoff, 0.0f, 1.0f) > kMaxDemotableAlphaCutoff)
            continue;
        const int imageIndex = ParseEmbeddedTextureIndex(material.DiffuseTexture);
        if (imageIndex < 0 || static_cast<size_t>(imageIndex) >= images.size())
            continue;

        bool anyMesh = false;
        bool analyzable = true;
        for (const Mesh& mesh : meshes)
        {
            if (mesh.MaterialIndex != static_cast<uint32>(mi))
                continue;
            anyMesh = true;
            if (mesh.PrimitiveTopology != MeshPrimitiveTopology::Triangles)
            {
                analyzable = false;
                break;
            }
            if (!mesh.HasValidLODColor0())
            {
                analyzable = false;
                break;
            }
            if (mesh.HasColor0() && !material.IgnoresVertexColor)
            {
                for (size_t v = 3; analyzable && v < mesh.Color0.size(); v += 4)
                    analyzable = mesh.Color0[v] >= kOpaqueAlphaAttenuationMin;
                for (const auto& colours : mesh.ExtraLODColor0)
                    for (size_t v = 3; analyzable && v < colours.size(); v += 4)
                        analyzable = colours[v] >= kOpaqueAlphaAttenuationMin;
                if (!analyzable)
                    break;
            }
        }
        if (!anyMesh || !analyzable)
            continue;

        const DecodedAlphaPlane& decoded = decodedPlaneFor(imageIndex);
        if (!decoded.Rgba)
            continue;

        // Shared with the material-registration probe so the two demotion
        // sites cannot drift apart again; nullopt means the cutoff is
        // unusable (non-finite, or so high the margin no longer fits under
        // 255) and the only safe answer is to keep Mask.
        const std::optional<uint32> threshold =
            AlphaCutoffOpaqueThreshold(material.AlphaCutoff, kAlphaCutoffSafetyMargin);
        if (!threshold)
            continue;
        const uint8 opaqueThreshold = static_cast<uint8>(*threshold);

        // Every texel of the plane clears the threshold: any footprint over it
        // is opaque — demote without walking a single triangle.
        if (decoded.MinAlpha >= opaqueThreshold)
        {
            material.AlphaMode = AlphaMode::Opaque;
            ++demoted;
            continue;
        }

        const std::array<float, 8>& st = material.DiffuseTextureTransform;

        bool footprintOpaque = true;
        for (const Mesh& mesh : meshes)
        {
            if (!footprintOpaque)
                break;
            if (mesh.MaterialIndex != static_cast<uint32>(mi))
                continue;

            auto transformedUV = [&st](const Vertex& vert, float out[2])
            {
                const float u = vert.TexCoords[0];
                const float v = vert.TexCoords[1];
                out[0] = st[0] * u + st[1] * v + st[2];
                out[1] = st[4] * u + st[5] * v + st[6];
            };
            auto indicesOpaque = [&](const Vector<uint32>& indices,
                                     const Vector<Vertex>& vertices)
            {
                for (size_t i = 0; i + 2 < indices.size(); i += 3)
                {
                    const uint32 i0 = indices[i];
                    const uint32 i1 = indices[i + 1];
                    const uint32 i2 = indices[i + 2];
                    if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
                        continue;
                    float a[2];
                    float b[2];
                    float c[2];
                    transformedUV(vertices[i0], a);
                    transformedUV(vertices[i1], b);
                    transformedUV(vertices[i2], c);
                    if (!TriangleFootprintIsOpaque(decoded.Plane, a, b, c, opaqueThreshold,
                                                   kFootprintDilationTexels, decoded.MinAlpha))
                        return false;
                }
                return true;
            };

            footprintOpaque = indicesOpaque(mesh.Indices, mesh.Vertices);
            // The pass runs from PostLoad AFTER authored-slot and generated/
            // cooked LOD resolution, so ExtraLODs here is the final LOD set:
            // index-only levels share the mesh's vertices, authored blocks
            // carry their own. Every drawn triangle contributes its footprint.
            for (size_t k = 0; footprintOpaque && k < mesh.ExtraLODs.size(); ++k)
            {
                const bool authored =
                    k < mesh.ExtraLODVertices.size() && !mesh.ExtraLODVertices[k].empty();
                footprintOpaque = indicesOpaque(
                    mesh.ExtraLODs[k], authored ? mesh.ExtraLODVertices[k] : mesh.Vertices);
            }
        }

        if (footprintOpaque)
        {
            material.AlphaMode = AlphaMode::Opaque;
            ++demoted;
        }
    }

    for (auto& [imageIndex, entry] : decodedPlanes)
        if (entry.Rgba)
            stbi_image_free(entry.Rgba);
    return demoted;
}
#endif // GE_HAVE_STB

// First mesh-bearing node in `node`'s subtree (inclusive, pre-order). LOD-group
// children are often intermediate transform (Null) nodes with the mesh a level
// below (Maya's LOD_<n> wrappers), so descend to the geometry. Returns null when
// the subtree holds no mesh.
const ufbx_node* FindPrimaryMeshNode(const ufbx_node* node)
{
    if (!node)
        return nullptr;
    if (node->mesh)
        return node;
    for (size_t i = 0; i < node->children.count; ++i)
        if (const ufbx_node* found = FindPrimaryMeshNode(node->children.data[i]))
            return found;
    return nullptr;
}

std::array<float, 8> ReadTextureTransform(const ufbx_texture* tex)
{
    std::array<float, 8> st{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    if (!tex || !tex->has_uv_transform)
        return st;

    // ufbx exposes FBX's UV transform as an affine matrix from source UV to
    // normalized texture coordinates. Preserve the 2D matrix so rotation is
    // not collapsed away into scale/offset.
    const ufbx_matrix& m = tex->uv_to_texture;
    st[0] = static_cast<float>(m.m00);
    st[1] = static_cast<float>(m.m01);
    st[2] = static_cast<float>(m.m03);
    st[4] = static_cast<float>(m.m10);
    st[5] = static_cast<float>(m.m11);
    st[6] = static_cast<float>(m.m13);
    return st;
}

// The default take of a static export animates no property, so it has nothing
// for AnimationClip to bake.
bool StackAnimatesAnyProperty(const ufbx_anim_stack& stack)
{
    for (size_t layerIndex = 0; layerIndex < stack.layers.count; ++layerIndex)
    {
        if (stack.layers.data[layerIndex]->anim_props.count > 0)
            return true;
    }
    return false;
}

} // namespace

bool ModelAsset::LoadFBX(const Vector<uint8>& data,
                         const std::filesystem::path& modelPathForExternalTextures,
                         const FbxLoaderOptions& loaderOptions)
{
    // Parse options change the produced geometry, so they are part of the
    // cooked-LOD cache key (F1): capture the resolved-options hash here, at parse
    // time, so the cook and the runtime provably key on the same options.
    m_ParseOptionsHash = HashFbxLoaderOptions(loaderOptions);

    ufbx_load_opts opts{};
    // Load raw FBX data; we apply axis and unit (cm→m) conversion
    // ourselves below so vertex data, bone matrices, and animation keys all
    // land in a single, consistent engine target space. Letting ufbx do the
    // conversion via target_axes/target_unit_meters splits the conversion
    // between the root node and untouched geometry, which would mismatch
    // during skinning evaluation.
    opts.generate_missing_normals = true;
    opts.normalize_normals = true;
    opts.normalize_tangents = true;
    opts.clean_skin_weights = loaderOptions.CleanSkinWeights;
    opts.use_blender_pbr_material = true;
    opts.pivot_handling = loaderOptions.AdjustPivots
        ? UFBX_PIVOT_HANDLING_ADJUST_TO_PIVOT
        : UFBX_PIVOT_HANDLING_RETAIN;
    opts.geometry_transform_handling = loaderOptions.PreserveGeometryTransforms
        ? UFBX_GEOMETRY_TRANSFORM_HANDLING_PRESERVE
        : UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    opts.inherit_mode_handling = loaderOptions.PreserveGeometryTransforms
        ? UFBX_INHERIT_MODE_HANDLING_PRESERVE
        : UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
    opts.node_depth_limit = 512;
    opts.ignore_embedded = !loaderOptions.ImportEmbeddedTextures;
    opts.evaluate_skinning = false;         // we sample skinning ourselves at runtime
    opts.load_external_files = true;
    opts.ignore_missing_external_files = true;

    const std::string modelPathStr = modelPathForExternalTextures.string();
    if (!modelPathStr.empty())
    {
        opts.filename.data = modelPathStr.c_str();
        opts.filename.length = modelPathStr.size();
    }

    ufbx_error err{};
    ufbx_scene* scene = ufbx_load_memory(data.data(), data.size(), &opts, &err);
    if (!scene)
    {
        Logger::Log::Error("ufbx: failed to load FBX '{}': {}",
                           GetName(),
                           err.description.length > 0 ? std::string(err.description.data, err.description.length) : "(no message)");
        return false;
    }

    int warningCount[UFBX_WARNING_TYPE_COUNT] = {};
    int ignoredWarningCount = 0;
    constexpr int kMaxWarningsPerType = 10;
    for (const ufbx_warning& warning : scene->metadata.warnings)
    {
        const int warningType = static_cast<int>(warning.type);
        if (warningType < 0 || warningType >= UFBX_WARNING_TYPE_COUNT)
        {
            ++ignoredWarningCount;
            continue;
        }

        if (warningCount[warningType]++ >= kMaxWarningsPerType)
        {
            ++ignoredWarningCount;
            continue;
        }

        const std::string description = warning.description.length > 0
            ? std::string(warning.description.data, warning.description.length)
            : std::string("(no description)");
        if (warning.element_id != UFBX_NO_INDEX && warning.element_id < scene->elements.count)
        {
            const ufbx_element* element = scene->elements.data[warning.element_id];
            const std::string elementName = element && element->name.length > 0
                ? std::string(element->name.data, element->name.length)
                : std::string("(unnamed)");
            Logger::Log::Warning("ufbx: warning in '{}': {}{}", elementName, description,
                                 warning.count > 1 ? " (repeated)" : "");
        }
        else
        {
            Logger::Log::Warning("ufbx: warning: {}{}", description,
                                 warning.count > 1 ? " (repeated)" : "");
        }
    }
    if (ignoredWarningCount > 0)
        Logger::Log::Warning("ufbx: ignored {} further warnings", ignoredWarningCount);

    // Source unit-to-meters factor (cm → 0.01, m → 1.0). FBX defaults to
    // centimeters so most files report 0.01. Used as the scale component
    // of the engine-target conversion C below.
    const float unitScale = scene->settings.unit_meters > 0.0
        ? static_cast<float>(scene->settings.unit_meters)
        : 1.0f;
    FbxAxisConversion axisConversion =
        FbxImport::MakeEngineConversion(scene->settings.axes, loaderOptions);

    // ---------------------------------------------------------------
    // Skeleton: every ufbx node becomes a "bone" in the engine skeleton,
    // mirroring the prior loader's convention of indexing by node tree.
    // Mesh.Joints0 stores node indices and the GPU palette is sized to
    // `BoneCount` so non-bone nodes stay identity. (See
    // SkeletonStore::CreateRuntime fallback.)
    // ---------------------------------------------------------------
    const uint32 boneCount = static_cast<uint32>(scene->nodes.count);
    std::vector<int32> parent(boneCount, -1);
    std::vector<float> restT(boneCount * 3u, 0.0f);
    std::vector<float> restR(boneCount * 4u, 0.0f);
    std::vector<float> restS(boneCount * 3u, 1.0f);
    std::vector<float> restLocalRM(boneCount * 16u, 0.0f);
    std::unordered_map<const ufbx_node*, uint32> nodeToBoneIndex;
    std::unordered_map<std::string, uint32> firstBoneIndexByName;
    nodeToBoneIndex.reserve(boneCount * 2u);
    firstBoneIndexByName.reserve(boneCount * 2u);

    // Pass 1: register every node so parent lookups can resolve regardless
    // of ordering in scene->nodes.
    for (uint32 i = 0; i < boneCount; ++i)
    {
        const ufbx_node* node = scene->nodes.data[i];
        nodeToBoneIndex.emplace(node, i);
        if (node->name.length > 0)
        {
            const std::string nm(node->name.data, node->name.length);
            firstBoneIndexByName.emplace(nm, i);
        }
    }

    // Pass 2: populate parent indices and rest TRS.
    for (uint32 i = 0; i < boneCount; ++i)
    {
        const ufbx_node* node = scene->nodes.data[i];
        if (const ufbx_node* parentNode = node->parent)
        {
            const auto it = nodeToBoneIndex.find(parentNode);
            parent[i] = (it != nodeToBoneIndex.end()) ? static_cast<int32>(it->second) : -1;
        }

        // Apply source FBX axes -> engine axes conversion to local TRS.
        const ufbx_transform& lt = node->local_transform;
        const ufbx_vec3 convertedT = ConvertFbxVec3(lt.translation, axisConversion, unitScale);
        const ufbx_quat convertedR = ConvertFbxQuat(lt.rotation, axisConversion);
        const ufbx_vec3 convertedS = ConvertFbxScale(lt.scale, axisConversion);
        restT[i * 3 + 0] = static_cast<float>(convertedT.x);
        restT[i * 3 + 1] = static_cast<float>(convertedT.y);
        restT[i * 3 + 2] = static_cast<float>(convertedT.z);
        restR[i * 4 + 0] = static_cast<float>(convertedR.x);
        restR[i * 4 + 1] = static_cast<float>(convertedR.y);
        restR[i * 4 + 2] = static_cast<float>(convertedR.z);
        restR[i * 4 + 3] = static_cast<float>(convertedR.w);
        restS[i * 3 + 0] = static_cast<float>(convertedS.x);
        restS[i * 3 + 1] = static_cast<float>(convertedS.y);
        restS[i * 3 + 2] = static_cast<float>(convertedS.z);

        StoreUfbxMatrixAsColumnMajorTargetSpace(
            node->node_to_parent,
            axisConversion,
            unitScale,
            restLocalRM.data() + i * 16u);
    }

    const SkeletonCleanupStats cleanupStats =
        SanitizeImportedSkeleton(parent, restT, restR, restS, restLocalRM, boneCount);
    if (cleanupStats.parentFixes || cleanupStats.translationFixes || cleanupStats.rotationFixes
        || cleanupStats.scaleFixes || cleanupStats.matrixFixes)
    {
        Logger::Log::Warning(
            "ModelAsset::LoadFBX '{}': skeleton cleanup fixed parents={}, translations={}, rotations={}, scales={}, matrices={}",
            GetName(), cleanupStats.parentFixes, cleanupStats.translationFixes,
            cleanupStats.rotationFixes, cleanupStats.scaleFixes, cleanupStats.matrixFixes);
    }

    using namespace GameEngine::Engine::Renderer;
    SkeletonData* skeleton = RetainOrCreateSkeleton(boneCount > 0 ? boneCount : 1u);
    if (!skeleton)
    {
        ufbx_free_scene(scene);
        return false;
    }

    skeleton->Parent = std::move(parent);
    skeleton->RestTranslation = std::move(restT);
    skeleton->RestRotation = std::move(restR);
    skeleton->RestScale = std::move(restS);
    skeleton->RestLocalMatrix = std::move(restLocalRM);
    skeleton->BoneCount = boneCount;
    skeleton->SourceModelPath = GetPath();

    // Bone names enable name-based channel resolution at sample time, so
    // clips authored against a sibling rig with matching names still drive
    // the right bones (cross-rig animation).
    skeleton->BoneNames.resize(boneCount);
    for (uint32 i = 0; i < boneCount; ++i)
    {
        const ufbx_node* node = scene->nodes.data[i];
        if (node->name.length > 0)
            skeleton->BoneNames[i].assign(node->name.data, node->name.length);
    }
    skeleton->BuildBoneNameLookup();

    // Inverse bind matrices: identity by default, populated from skin
    // clusters below. ufbx exposes the bind-pose matrix that takes a
    // mesh-local vertex into bone-local space directly as
    // `ufbx_skin_cluster.geometry_to_bone` — that's the inverse bind
    // matrix the GPU palette wants.
    skeleton->InverseBind.assign(static_cast<size_t>(boneCount) * 16u, 0.0f);
    for (uint32 i = 0; i < boneCount; ++i)
        StoreIdentityColumnMajor(skeleton->InverseBind.data() + i * 16u);

    skeleton->SkinJointCount = 0;
    skeleton->JointNodes.clear();
    skeleton->MeshRootNode = -1;

    // ---------------------------------------------------------------
    // Scene extras: cameras, lights, and authored empty/helper nodes.
    // ---------------------------------------------------------------
    m_Cameras.clear();
    m_Lights.clear();
    m_SceneNodes.clear();

    auto nodeName = [](const ufbx_node* node, const char* fallback, size_t index) -> String
    {
        if (node && node->name.length > 0)
            return String(node->name.data, node->name.length);
        return String(fallback) + "_" + std::to_string(index);
    };

    auto nodeIndexOf = [&](const ufbx_node* node) -> int32
    {
        const auto it = nodeToBoneIndex.find(node);
        return (it != nodeToBoneIndex.end()) ? static_cast<int32>(it->second) : -1;
    };
    auto parentNodeIndexOf = [&](const ufbx_node* node) -> int32
    {
        return (node && node->parent && !node->parent->is_root) ? nodeIndexOf(node->parent) : -1;
    };
    auto storeNodeTransforms = [&](const ufbx_node* node, float* worldOut, float* localOut)
    {
        StoreUfbxMatrixAsColumnMajorTargetSpace(
            node->node_to_world, axisConversion, unitScale, worldOut);
        StoreUfbxMatrixAsColumnMajorTargetSpace(
            node->node_to_parent, axisConversion, unitScale, localOut);
    };

    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

    for (size_t i = 0; i < scene->nodes.count; ++i)
    {
        const ufbx_node* node = scene->nodes.data[i];
        if (!node)
            continue;

        if (!node->is_root && !node->bone)
        {
            ImportedSceneNodeData imported{};
            imported.Name = nodeName(node, "Node", m_SceneNodes.size());
            imported.SourceNodeIndex = nodeIndexOf(node);
            imported.ParentSourceNodeIndex = parentNodeIndexOf(node);
            storeNodeTransforms(node, imported.Transform, imported.LocalTransform);
            m_SceneNodes.push_back(imported);
        }

        if (const ufbx_camera* camera = node->camera)
        {
            ImportedCameraData imported{};
            imported.Name = nodeName(node, "Camera", m_Cameras.size());
            imported.SourceNodeIndex = nodeIndexOf(node);
            imported.ParentSourceNodeIndex = parentNodeIndexOf(node);
            storeNodeTransforms(node, imported.Transform, imported.LocalTransform);
            imported.Perspective = camera->projection_mode == UFBX_PROJECTION_MODE_PERSPECTIVE;
            imported.FovY = camera->field_of_view_deg.y > 0.0
                ? static_cast<float>(camera->field_of_view_deg.y)
                : 60.0f;
            imported.OrthographicSize = camera->orthographic_size.y > 0.0
                ? static_cast<float>(camera->orthographic_size.y * unitScale)
                : 10.0f;
            imported.NearZ = camera->near_plane > 0.0
                ? static_cast<float>(camera->near_plane * unitScale)
                : 0.01f;
            imported.FarZ = camera->far_plane > camera->near_plane
                ? static_cast<float>(camera->far_plane * unitScale)
                : 1000.0f;
            m_Cameras.push_back(imported);
        }

        if (const ufbx_light* light = node->light)
        {
            ImportedLightData imported{};
            imported.Name = nodeName(node, "Light", m_Lights.size());
            imported.SourceNodeIndex = nodeIndexOf(node);
            imported.ParentSourceNodeIndex = parentNodeIndexOf(node);
            storeNodeTransforms(node, imported.Transform, imported.LocalTransform);
            imported.Color[0] = static_cast<float>(light->color.x);
            imported.Color[1] = static_cast<float>(light->color.y);
            imported.Color[2] = static_cast<float>(light->color.z);
            imported.Intensity = static_cast<float>(light->intensity);
            imported.InnerAngle = static_cast<float>(light->inner_angle) * kDegToRad;
            imported.OuterAngle = static_cast<float>(light->outer_angle) * kDegToRad;
            imported.CastsLight = light->cast_light;
            imported.CastsShadows = light->cast_shadows;
            imported.AreaWidth = static_cast<float>(std::max<ufbx_real>(
                0.001, ufbx_find_real(&light->props, "AreaLightWidth", 1.0)) * unitScale);
            imported.AreaHeight = static_cast<float>(std::max<ufbx_real>(
                0.001, ufbx_find_real(&light->props, "AreaLightHeight", 1.0)) * unitScale);
            imported.AreaRadius = static_cast<float>(std::max(imported.AreaWidth, imported.AreaHeight) * 0.5f);
            switch (light->decay)
            {
            case UFBX_LIGHT_DECAY_NONE:      imported.Decay = 0.0f; break;
            case UFBX_LIGHT_DECAY_LINEAR:    imported.Decay = 1.0f; break;
            case UFBX_LIGHT_DECAY_CUBIC:     imported.Decay = 3.0f; break;
            case UFBX_LIGHT_DECAY_QUADRATIC:
            default:                         imported.Decay = 2.0f; break;
            }
            imported.AreaShape = light->area_shape == UFBX_LIGHT_AREA_SHAPE_SPHERE
                ? ImportedAreaLightShape::Sphere
                : ImportedAreaLightShape::Rectangle;
            switch (light->type)
            {
            case UFBX_LIGHT_DIRECTIONAL: imported.Type = ImportedLightType::Directional; break;
            case UFBX_LIGHT_SPOT:        imported.Type = ImportedLightType::Spot;        break;
            case UFBX_LIGHT_AREA:        imported.Type = ImportedLightType::Area;        break;
            case UFBX_LIGHT_VOLUME:      imported.Type = ImportedLightType::Volume;      break;
            case UFBX_LIGHT_POINT:
            default:                     imported.Type = ImportedLightType::Point;       break;
            }
            m_Lights.push_back(imported);
        }
    }

    // ---------------------------------------------------------------
    // Materials + textures.
    // ---------------------------------------------------------------
    m_EmbeddedImages.clear();
    m_Materials.clear();
    m_Materials.reserve(scene->materials.count);

    std::unordered_map<const ufbx_texture*, int> textureCache;
    std::unordered_map<std::string, int> diskCache;
    const std::filesystem::path modelDir = modelPathForExternalTextures.empty()
        ? std::filesystem::path{}
        : modelPathForExternalTextures.parent_path();

    auto resolveTextureRef = [&](const ufbx_texture* tex) -> int
    {
        return AppendEmbeddedTextureForUfbx(tex, modelDir, m_EmbeddedImages, textureCache, diskCache);
    };

    auto floatOr = [](const ufbx_material_map& map, float fallback) -> float
    {
        return map.has_value ? static_cast<float>(map.value_real) : fallback;
    };

    for (size_t mi = 0; mi < scene->materials.count; ++mi)
    {
        const ufbx_material* mat = scene->materials.data[mi];
        ImportedMaterialData out{};
        out.IgnoresVertexColor = true;

        out.Name = mat->name.length > 0
            ? std::string(mat->name.data, mat->name.length)
            : "Material_" + std::to_string(mi);

        // Base color: prefer the FBX legacy diffuse pipeline (DiffuseColor *
        // DiffuseFactor) for Lambert/Phong source materials — that's how
        // those shaders actually compute their albedo. Fall back to the PBR
        // base_color (* base_factor) for PBR source shaders. Either way,
        // multiply by the corresponding factor when present so authoring
        // tools that use the factor for tinting come through correctly.
        const bool isLegacyFbxShader =
            mat->shader_type == UFBX_SHADER_FBX_LAMBERT ||
            mat->shader_type == UFBX_SHADER_FBX_PHONG ||
            mat->shader_type == UFBX_SHADER_BLENDER_PHONG;

        auto readColorXmap = [](const ufbx_material_map& colorMap,
                                const ufbx_material_map& factorMap,
                                float (&outRgb)[3]) -> bool
        {
            if (!colorMap.has_value)
                return false;
            const float f = factorMap.has_value ? static_cast<float>(factorMap.value_real) : 1.0f;
            outRgb[0] = static_cast<float>(colorMap.value_vec3.x) * f;
            outRgb[1] = static_cast<float>(colorMap.value_vec3.y) * f;
            outRgb[2] = static_cast<float>(colorMap.value_vec3.z) * f;
            return true;
        };

        float rgb[3] = {0.8f, 0.8f, 0.8f};
        bool gotColor = false;
        if (isLegacyFbxShader)
        {
            gotColor = readColorXmap(mat->fbx.diffuse_color, mat->fbx.diffuse_factor, rgb)
                    || readColorXmap(mat->pbr.base_color, mat->pbr.base_factor, rgb);
        }
        else
        {
            gotColor = readColorXmap(mat->pbr.base_color, mat->pbr.base_factor, rgb)
                    || readColorXmap(mat->fbx.diffuse_color, mat->fbx.diffuse_factor, rgb);
        }

        // Defensive: a value of essentially-black usually means the
        // exporter wrote a placeholder default, not a deliberate "render
        // pure black" choice. Fall back to the engine default gray so the
        // mesh remains visible in the editor.
        if (gotColor && (rgb[0] + rgb[1] + rgb[2]) < 1e-4f)
            rgb[0] = rgb[1] = rgb[2] = 0.8f;

        out.DiffuseColor[0] = rgb[0];
        out.DiffuseColor[1] = rgb[1];
        out.DiffuseColor[2] = rgb[2];
        out.DiffuseColor[3] = 1.0f;

        // Specular color (legacy FBX path; PBR materials don't expose this directly).
        if (mat->fbx.specular_color.has_value)
        {
            out.SpecularColor[0] = static_cast<float>(mat->fbx.specular_color.value_vec3.x);
            out.SpecularColor[1] = static_cast<float>(mat->fbx.specular_color.value_vec3.y);
            out.SpecularColor[2] = static_cast<float>(mat->fbx.specular_color.value_vec3.z);
        }
        else
        {
            out.SpecularColor[0] = out.SpecularColor[1] = out.SpecularColor[2] = 0.2f;
        }
        out.Shininess = floatOr(mat->fbx.specular_exponent, 32.0f);

        out.Metallic  = floatOr(mat->pbr.metalness, mat->pbr.metalness.texture ? 1.0f : 0.0f);
        if (mat->pbr.roughness.has_value)
        {
            out.Roughness = static_cast<float>(mat->pbr.roughness.value_real);
        }
        else if (mat->pbr.roughness.texture)
        {
            out.Roughness = 1.0f;
        }
        else
        {
            // Karis: shininess → roughness fallback for legacy Phong materials.
            out.Roughness = std::clamp(std::sqrt(2.0f / (out.Shininess + 2.0f)), 0.04f, 1.0f);
        }

        // Opacity: prefer the explicit PBR opacity field. The legacy FBX
        // `TransparencyFactor` channel is unreliable — many exporters set it
        // to a default 1.0 (which would mean fully transparent under naive
        // interpretation) on every material regardless of intent — so we
        // ignore scalar-only legacy transparency. A linked opacity /
        // transparency texture is explicit authoring intent, though: imports
        // such as Toon Dinosaurs' eye-shine planes store alpha in the diffuse
        // texture and also wire that image to FBX TransparencyColor.
        if (mat->pbr.opacity.has_value)
        {
            const float opacity = std::clamp(
                static_cast<float>(mat->pbr.opacity.value_real), 0.0f, 1.0f);
            out.DiffuseColor[3] = opacity;
            if (opacity < 0.999f)
                out.AlphaMode = AlphaMode::Blend;
        }
        int alphaTextureIdx = -1;
        if (mat->pbr.opacity.texture || mat->fbx.transparency_color.texture)
        {
            if (const int idx = resolveTextureRef(mat->pbr.opacity.texture); idx >= 0)
            {
                alphaTextureIdx = idx;
                out.AlphaMode = AlphaMode::Blend;
            }
            else if (const int idx2 = resolveTextureRef(mat->fbx.transparency_color.texture); idx2 >= 0)
            {
                alphaTextureIdx = idx2;
                out.AlphaMode = AlphaMode::Blend;
            }
        }

        out.DoubleSided = mat->features.double_sided.enabled;

        // Texture references — PBR maps first, fall back to FBX maps for
        // exports that didn't write the PBR block.
        const ufbx_texture* diffuseTexture = mat->pbr.base_color.texture
            ? mat->pbr.base_color.texture
            : mat->fbx.diffuse_color.texture;
        int diffuseTextureIdx = resolveTextureRef(diffuseTexture);
#if defined(GE_HAVE_STB)
        if (diffuseTextureIdx >= 0 && alphaTextureIdx >= 0)
        {
            diffuseTextureIdx = MergeAlphaTextureIntoAlbedo(m_EmbeddedImages, diffuseTextureIdx, alphaTextureIdx, out.AlphaMode);
            // The merge classifies Mask from texture content alone (only 0/255
            // alpha values) — mark it inferred so PostLoad's footprint demotion
            // knows this Mask is not authored intent.
            out.AlphaModeInferred = out.AlphaMode == AlphaMode::Mask;
        }
#endif
        if (diffuseTextureIdx >= 0)
        {
            AssignEmbeddedRef(out.DiffuseTexture, diffuseTextureIdx);
            out.DiffuseTextureTransform = ReadTextureTransform(diffuseTexture);
        }

        const ufbx_texture* normalTexture = mat->pbr.normal_map.texture
            ? mat->pbr.normal_map.texture
            : (mat->fbx.normal_map.texture ? mat->fbx.normal_map.texture : mat->fbx.bump.texture);
        if (const int idx = resolveTextureRef(normalTexture); idx >= 0)
        {
            AssignEmbeddedRef(out.NormalTexture, idx);
            out.NormalTextureTransform = ReadTextureTransform(normalTexture);
        }

        const ufbx_texture* specularTexture = mat->pbr.metalness.texture
            ? mat->pbr.metalness.texture
            : mat->fbx.specular_color.texture;
        if (const int idx = resolveTextureRef(specularTexture); idx >= 0)
        {
            AssignEmbeddedRef(out.SpecularTexture, idx);
            out.SpecularTextureTransform = ReadTextureTransform(specularTexture);
        }

        const ufbx_texture* emissiveTexture = mat->pbr.emission_color.texture
            ? mat->pbr.emission_color.texture
            : mat->fbx.emission_color.texture;
        const int emissiveIndex = resolveTextureRef(emissiveTexture);
        if (emissiveIndex >= 0)
        {
            AssignEmbeddedRef(out.EmissiveTexture, emissiveIndex);
            out.EmissiveTextureTransform = ReadTextureTransform(emissiveTexture);
        }
        FbxImport::ReadEmissiveColor(*mat, emissiveIndex >= 0, out.EmissiveColor);

        const ufbx_texture* occlusionTexture = mat->pbr.ambient_occlusion.texture
            ? mat->pbr.ambient_occlusion.texture
            : mat->fbx.ambient_color.texture;
        if (const int idx = resolveTextureRef(occlusionTexture); idx >= 0)
        {
            AssignEmbeddedRef(out.OcclusionTexture, idx);
            out.OcclusionTextureTransform = ReadTextureTransform(occlusionTexture);
        }

        const ufbx_texture* roughnessTexture = mat->pbr.roughness.texture;
        if (const int idx = resolveTextureRef(roughnessTexture); idx >= 0)
        {
            AssignEmbeddedRef(out.RoughnessTexture, idx);
            out.RoughnessTextureTransform = ReadTextureTransform(roughnessTexture);
        }

        const ufbx_texture* metallicTexture = mat->pbr.metalness.texture;
        if (const int idx = resolveTextureRef(metallicTexture); idx >= 0)
        {
            AssignEmbeddedRef(out.MetallicTexture, idx);
            out.MetallicTextureTransform = ReadTextureTransform(metallicTexture);
        }

        m_Materials.push_back(std::move(out));
    }

    if (m_Materials.empty())
    {
        ImportedMaterialData defMat{};
        defMat.Name = "DefaultMaterial";
        defMat.DiffuseColor[0] = defMat.DiffuseColor[1] = defMat.DiffuseColor[2] = 0.8f;
        defMat.DiffuseColor[3] = 1.0f;
        defMat.IgnoresVertexColor = true;
        m_Materials.push_back(std::move(defMat));
    }

    // ---------------------------------------------------------------
    // Meshes — one engine Mesh per ufbx_mesh + material part. We split by
    // material so each draw call has a single material, matching the
    // existing batch-compactor / world-draw-builder expectations.
    // ---------------------------------------------------------------
    for (size_t mi = 0; mi < scene->meshes.count; ++mi)
    {
        const ufbx_mesh* uMesh = scene->meshes.data[mi];
        if (!uMesh || uMesh->num_faces == 0)
            continue;

        // The owning node positions this mesh in model space; a mesh instanced
        // under several nodes takes the first here and the rest as
        // ExtraPlacements below.
        const ufbx_node* meshNode = (uMesh->instances.count > 0)
            ? uMesh->instances.data[0]
            : nullptr;

        // Geometry->node transform for the owning node. We bake this into the
        // emitted vertices so they land in the node's LOCAL frame rather than
        // the raw geometry frame, then place the part with the node transform
        // (node_to_world / node_to_parent) stored below. This matters because
        // ufbx pivot adjustment folds a node's pivot offset into
        // geometry_to_node; a consumer that only has the node transform (a
        // Unity-scene converter emitting one entity per node, or the GPU node
        // pose in RigidAnimationSystem) rotates about the node origin, so
        // vertices left in geometry space swing out by (R-I)*pivot under any
        // authored rotation. Baking makes the vertices node-local so both the
        // spawn path and a converter-authored node transform agree. Identity
        // for a node with no geometry transform (the common case), so this is
        // a no-op for ordinary meshes and skinned characters.
        const ufbx_matrix geometryToNode = meshNode ? meshNode->geometry_to_node : ufbx_identity_matrix;

        // Track which skin cluster each ufbx joint corresponds to so we
        // can flatten clusters into the engine's bone palette.
        const ufbx_skin_deformer* skin = (uMesh->skin_deformers.count > 0)
            ? uMesh->skin_deformers.data[0]
            : nullptr;

        // Populate the inverse bind matrix for every cluster. This runs
        // once per mesh; clusters reference the same bone nodes that were
        // indexed during skeleton construction above.
        if (skin)
        {
            for (size_t ci = 0; ci < skin->clusters.count; ++ci)
            {
                const ufbx_skin_cluster* cluster = skin->clusters.data[ci];
                if (!cluster || !cluster->bone_node)
                    continue;
                const auto it = nodeToBoneIndex.find(cluster->bone_node);
                if (it == nodeToBoneIndex.end())
                    continue;
                // The cluster's inverse bind (geometry_to_bone) maps a
                // geometry-space vertex into bone space. We bake geometryToNode
                // into the vertices below, so compose the inverse (node->geometry)
                // on the right to keep the bind mapping node-local vertices. The
                // two folds cancel exactly in the skin matrix, so skinned output
                // is bitwise-unchanged regardless of the geometry transform.
                const ufbx_matrix nodeToGeometry = ufbx_matrix_invert(&geometryToNode);
                const ufbx_matrix nodeToBone =
                    ufbx_matrix_mul(&cluster->geometry_to_bone, &nodeToGeometry);
                StoreUfbxMatrixAsColumnMajorTargetSpace(
                    nodeToBone, axisConversion, unitScale,
                    skeleton->InverseBind.data() + static_cast<size_t>(it->second) * 16u);
            }

            if (skeleton->MeshRootNode < 0 && meshNode)
            {
                if (auto it = nodeToBoneIndex.find(meshNode); it != nodeToBoneIndex.end())
                {
                    skeleton->MeshRootNode = static_cast<int32>(it->second);
                    StoreUfbxMatrixAsColumnMajorTargetSpace(
                        meshNode->geometry_to_world, axisConversion, unitScale,
                        skeleton->MeshRootWorld);
                }
            }
        }

        // Triangulate face by face so we work with a polygon-agnostic
        // representation. The maximum triangle count per face is bounded
        // and the temporary buffer is reused across faces.
        std::vector<uint32_t> triBuf(std::max<size_t>(uMesh->max_face_triangles, 1u) * 3u);

        // Per-material parts (one engine Mesh per part) so each draw uses
        // a single material; this matches the previous loader's behavior
        // when Assimp would split a multi-material FBX mesh into pieces.
        const size_t partCount = uMesh->material_parts.count > 0 ? uMesh->material_parts.count : 1u;
        for (size_t partIdx = 0; partIdx < partCount; ++partIdx)
        {
            const ufbx_mesh_part* part = (uMesh->material_parts.count > 0)
                ? &uMesh->material_parts.data[partIdx]
                : nullptr;

            Mesh mesh;
            // Prefer the geometry element name; many FBX (e.g. Synty packs) leave
            // the geometry unnamed and carry the meaningful name on the owning
            // node, so fall back to that. This makes Mesh.Name equal the DCC/FBX
            // object name a scene importer sees, enabling submesh-by-name binding.
            if (uMesh->name.length > 0)
                mesh.Name = std::string(uMesh->name.data, uMesh->name.length);
            else if (meshNode && meshNode->name.length > 0)
                mesh.Name = std::string(meshNode->name.data, meshNode->name.length);
            else
                mesh.Name = "Mesh_" + std::to_string(mi);
            if (uMesh->material_parts.count > 1)
                mesh.Name += "_" + std::to_string(partIdx);

            const size_t partTriangles = part ? part->num_triangles : uMesh->num_triangles;
            const size_t partLines = part ? part->num_line_faces : uMesh->num_line_faces;
            const size_t partPoints = part ? part->num_point_faces : uMesh->num_point_faces;
            if (partTriangles == 0 && partLines > 0)
                mesh.PrimitiveTopology = MeshPrimitiveTopology::Lines;
            else if (partTriangles == 0 && partLines == 0 && partPoints > 0)
                mesh.PrimitiveTopology = MeshPrimitiveTopology::Points;
            if (part && partTriangles > 0 && (partLines > 0 || partPoints > 0))
            {
                Logger::Log::Warning(
                    "ModelAsset::LoadFBX '{}': mesh part '{}' mixes triangle and point/line faces; importing triangle topology for this part",
                    GetName(), mesh.Name);
            }

            mesh.MaterialIndex = 0;
            if (part && uMesh->materials.count > 0 && partIdx < uMesh->materials.count)
            {
                const ufbx_material* matRef = uMesh->materials.data[partIdx];
                if (matRef)
                {
                    // Map mesh-local material → scene-level material index
                    // (which is what `m_Materials` is indexed by).
                    for (size_t si = 0; si < scene->materials.count; ++si)
                    {
                        if (scene->materials.data[si] == matRef)
                        {
                            mesh.MaterialIndex = static_cast<uint32>(si);
                            break;
                        }
                    }
                }
            }

            mesh.MinBounds[0] = mesh.MinBounds[1] = mesh.MinBounds[2] = std::numeric_limits<float>::max();
            mesh.MaxBounds[0] = mesh.MaxBounds[1] = mesh.MaxBounds[2] = std::numeric_limits<float>::lowest();

            if (meshNode)
            {
                if (auto it = nodeToBoneIndex.find(meshNode); it != nodeToBoneIndex.end())
                {
                    mesh.SourceNodeIndex = static_cast<int32>(it->second);
                    mesh.SourceNodeParentIndex = parentNodeIndexOf(meshNode);
                    // Vertices are now node-local (geometryToNode baked in), so
                    // the placement transform is the node transform itself:
                    // node_to_world composes node_to_world*geometry_to_node =
                    // geometry_to_world, leaving SourceNodeTransform*vertex
                    // invariant for consumers that bake into world (ocean depth,
                    // nav geometry). The local transform is identity: a mesh
                    // parented under its own node's scene entity (which already
                    // carries node_to_parent) sits at the node origin.
                    StoreUfbxMatrixAsColumnMajorTargetSpace(
                        meshNode->node_to_world, axisConversion, unitScale,
                        mesh.SourceNodeTransform);
                    StoreIdentityColumnMajor(mesh.SourceNodeLocalTransform);
                }
            }

            // Resolve indices for this material part. ufbx gives us a list
            // of face indices; for each face we triangulate, translate the
            // resulting per-corner indices through the mesh's vertex
            // attribute layers, and emit a unique vertex per
            // (position, normal, uv, tangent, ...) tuple — matching the
            // way the engine consumes interleaved vertex buffers today.
            const ufbx_uint32_list& faceIndices = part
                ? part->face_indices
                : ufbx_uint32_list{};
            const size_t faceCount = part ? part->num_faces : uMesh->num_faces;

            // Cache vertex dedup so attribute-split corners coincide where
            // appropriate. The dedup key is a tuple of per-attribute
            // index-lookups so a vertex split across UV/normal seams gets
            // its own entry but shared corners are reused.
            std::unordered_map<VertexDedupKey, uint32, VertexDedupKeyHash> vertexCache;
            mesh.Vertices.reserve(uMesh->num_vertices);

            // Parallel to mesh.Vertices: the originating ufbx mesh vertex
            // (= index into vertex_position.values). Used below to look up
            // skinning weights via skin_deformer.vertices[srcVert].
            std::vector<uint32_t> vertSourceVertex;
            vertSourceVertex.reserve(uMesh->num_vertices);

            const bool hasNormal    = uMesh->vertex_normal.exists;
            const bool hasUV        = uMesh->vertex_uv.exists;
            const bool hasUV1       = uMesh->uv_sets.count >= 2u && uMesh->uv_sets.data[1].vertex_uv.exists;
            std::array<bool, 8> hasUVSet{};
            std::array<uint32_t, 8> uvSetToExtraIndex{};
            size_t extraUvCount = 0;
            for (size_t uvi = 2; uvi < 8u && uvi < uMesh->uv_sets.count; ++uvi)
            {
                if (uMesh->uv_sets.data[uvi].vertex_uv.exists)
                {
                    hasUVSet[uvi] = true;
                    uvSetToExtraIndex[uvi] = static_cast<uint32_t>(extraUvCount++);
                }
            }
            const bool hasTangent   = uMesh->vertex_tangent.exists;
            const bool hasBitangent = uMesh->vertex_bitangent.exists;
            const bool hasColor     = uMesh->vertex_color.exists;
            if (hasUV1)
                mesh.TexCoords1.reserve(uMesh->num_vertices * 2u);
            if (extraUvCount > 0)
            {
                mesh.ExtraTexCoords.resize(extraUvCount);
                for (Vector<float>& uvSet : mesh.ExtraTexCoords)
                    uvSet.reserve(uMesh->num_vertices * 2u);
            }
            if (hasColor)
                mesh.Color0.reserve(uMesh->num_vertices * 4u);

            auto getMeshIndex = [&](size_t f) -> ufbx_face
            {
                const uint32_t fi = part ? faceIndices.data[f] : static_cast<uint32_t>(f);
                return uMesh->faces.data[fi];
            };

            auto emitVertexForMeshIndex = [&](uint32_t srcIndex) -> uint32
            {
                const uint32_t posIdx  = uMesh->vertex_position.indices.data[srcIndex];
                const uint32_t nrmIdx  = hasNormal    ? uMesh->vertex_normal.indices.data[srcIndex]    : 0;
                const uint32_t uvIdx   = hasUV        ? uMesh->vertex_uv.indices.data[srcIndex]        : 0;
                const uint32_t uv1Idx  = hasUV1       ? uMesh->uv_sets.data[1].vertex_uv.indices.data[srcIndex] : 0;
                std::array<uint32_t, 8> uvSetIndices{};
                for (size_t uvi = 2; uvi < 8u; ++uvi)
                {
                    if (hasUVSet[uvi])
                        uvSetIndices[uvi] = uMesh->uv_sets.data[uvi].vertex_uv.indices.data[srcIndex];
                }
                const uint32_t tanIdx  = hasTangent   ? uMesh->vertex_tangent.indices.data[srcIndex]   : 0;
                const uint32_t btIdx   = hasBitangent ? uMesh->vertex_bitangent.indices.data[srcIndex] : 0;
                const uint32_t colIdx  = hasColor     ? uMesh->vertex_color.indices.data[srcIndex]     : 0;
                const VertexDedupKey key{
                    posIdx, nrmIdx, uvIdx, uv1Idx,
                    uvSetIndices[2], uvSetIndices[3], uvSetIndices[4],
                    uvSetIndices[5], uvSetIndices[6], uvSetIndices[7],
                    tanIdx, btIdx, colIdx};
                if (auto it = vertexCache.find(key); it != vertexCache.end())
                    return it->second;

                Vertex v{};
                const ufbx_vec3 p = ufbx_transform_position(&geometryToNode, uMesh->vertex_position.values.data[posIdx]);
                const ufbx_vec3 convertedP = ConvertFbxVec3(p, axisConversion, unitScale);
                v.Position[0] = static_cast<float>(convertedP.x);
                v.Position[1] = static_cast<float>(convertedP.y);
                v.Position[2] = static_cast<float>(convertedP.z);
                if (hasNormal)
                {
                    const ufbx_vec3 n = ufbx_transform_direction(&geometryToNode, uMesh->vertex_normal.values.data[nrmIdx]);
                    const ufbx_vec3 convertedN = ConvertFbxVec3(n, axisConversion, 1.0f);
                    v.Normal[0] = static_cast<float>(convertedN.x);
                    v.Normal[1] = static_cast<float>(convertedN.y);
                    v.Normal[2] = static_cast<float>(convertedN.z);
                }
                if (hasUV)
                {
                    const ufbx_vec2 uv = uMesh->vertex_uv.values.data[uvIdx];
                    v.TexCoords[0] = static_cast<float>(uv.x);
                    v.TexCoords[1] = 1.0f - static_cast<float>(uv.y);
                }

                for (int c = 0; c < 3; ++c)
                {
                    mesh.MinBounds[c] = std::min(mesh.MinBounds[c], v.Position[c]);
                    mesh.MaxBounds[c] = std::max(mesh.MaxBounds[c], v.Position[c]);
                }
                const uint32 newVertIndex = static_cast<uint32>(mesh.Vertices.size());
                mesh.Vertices.push_back(v);
                if (hasColor)
                {
                    const ufbx_vec4 color = uMesh->vertex_color.values.data[colIdx];
                    mesh.Color0.push_back(static_cast<float>(color.x));
                    mesh.Color0.push_back(static_cast<float>(color.y));
                    mesh.Color0.push_back(static_cast<float>(color.z));
                    mesh.Color0.push_back(static_cast<float>(color.w));
                }
                if (hasUV1)
                {
                    const ufbx_vec2 uv1 = uMesh->uv_sets.data[1].vertex_uv.values.data[uv1Idx];
                    mesh.TexCoords1.push_back(static_cast<float>(uv1.x));
                    mesh.TexCoords1.push_back(1.0f - static_cast<float>(uv1.y));
                }
                for (size_t uvi = 2; uvi < 8u; ++uvi)
                {
                    if (!hasUVSet[uvi])
                        continue;
                    const uint32_t extraIndex = uvSetToExtraIndex[uvi];
                    const ufbx_vertex_vec2& uvAttrib = uMesh->uv_sets.data[uvi].vertex_uv;
                    const ufbx_vec2 uv = uvAttrib.values.data[uvSetIndices[uvi]];
                    mesh.ExtraTexCoords[extraIndex].push_back(static_cast<float>(uv.x));
                    mesh.ExtraTexCoords[extraIndex].push_back(1.0f - static_cast<float>(uv.y));
                }
                vertSourceVertex.push_back(posIdx);
                vertexCache.emplace(key, newVertIndex);
                return newVertIndex;
            };

            for (size_t f = 0; f < faceCount; ++f)
            {
                const ufbx_face face = getMeshIndex(f);
                if (mesh.PrimitiveTopology == MeshPrimitiveTopology::Points)
                {
                    if (face.num_indices == 1)
                        mesh.Indices.push_back(emitVertexForMeshIndex(face.index_begin));
                    continue;
                }
                if (mesh.PrimitiveTopology == MeshPrimitiveTopology::Lines)
                {
                    if (face.num_indices == 2)
                    {
                        mesh.Indices.push_back(emitVertexForMeshIndex(face.index_begin + 0));
                        mesh.Indices.push_back(emitVertexForMeshIndex(face.index_begin + 1));
                    }
                    continue;
                }
                if (face.num_indices < 3)
                    continue;

                const uint32_t numTris = ufbx_triangulate_face(triBuf.data(), triBuf.size(), uMesh, face);
                for (uint32_t t = 0; t < numTris; ++t)
                {
                    uint32 outIdx[3] = {0, 0, 0};
                    for (int corner = 0; corner < 3; ++corner)
                    {
                        const uint32_t srcIndex = triBuf[t * 3 + corner];

                        const uint32_t posIdx  = uMesh->vertex_position.indices.data[srcIndex];
                        const uint32_t nrmIdx  = hasNormal    ? uMesh->vertex_normal.indices.data[srcIndex]    : 0;
                        const uint32_t uvIdx   = hasUV        ? uMesh->vertex_uv.indices.data[srcIndex]        : 0;
                        const uint32_t uv1Idx  = hasUV1       ? uMesh->uv_sets.data[1].vertex_uv.indices.data[srcIndex] : 0;
                        std::array<uint32_t, 8> uvSetIndices{};
                        for (size_t uvi = 2; uvi < 8u; ++uvi)
                        {
                            if (hasUVSet[uvi])
                                uvSetIndices[uvi] = uMesh->uv_sets.data[uvi].vertex_uv.indices.data[srcIndex];
                        }
                        const uint32_t tanIdx  = hasTangent   ? uMesh->vertex_tangent.indices.data[srcIndex]   : 0;
                        const uint32_t btIdx   = hasBitangent ? uMesh->vertex_bitangent.indices.data[srcIndex] : 0;
                        const uint32_t colIdx  = hasColor     ? uMesh->vertex_color.indices.data[srcIndex]     : 0;
                        const VertexDedupKey key{
                            posIdx, nrmIdx, uvIdx, uv1Idx,
                            uvSetIndices[2], uvSetIndices[3], uvSetIndices[4],
                            uvSetIndices[5], uvSetIndices[6], uvSetIndices[7],
                            tanIdx, btIdx, colIdx};

                        if (auto it = vertexCache.find(key); it != vertexCache.end())
                        {
                            outIdx[corner] = it->second;
                            continue;
                        }

                        Vertex v{};
                        // Bake the node's geometry transform, then apply the
                        // source FBX axes -> engine axes conversion so vertices
                        // land node-local. Direction vectors get no unit scaling.
                        const ufbx_vec3 p = ufbx_transform_position(&geometryToNode, uMesh->vertex_position.values.data[posIdx]);
                        const ufbx_vec3 convertedP = ConvertFbxVec3(p, axisConversion, unitScale);
                        v.Position[0] = static_cast<float>(convertedP.x);
                        v.Position[1] = static_cast<float>(convertedP.y);
                        v.Position[2] = static_cast<float>(convertedP.z);

                        if (hasNormal)
                        {
                            const ufbx_vec3 n = ufbx_transform_direction(&geometryToNode, uMesh->vertex_normal.values.data[nrmIdx]);
                            const ufbx_vec3 convertedN = ConvertFbxVec3(n, axisConversion, 1.0f);
                            v.Normal[0] = static_cast<float>(convertedN.x);
                            v.Normal[1] = static_cast<float>(convertedN.y);
                            v.Normal[2] = static_cast<float>(convertedN.z);
                        }
                        if (hasUV)
                        {
                            const ufbx_vec2 uv = uMesh->vertex_uv.values.data[uvIdx];
                            v.TexCoords[0] = static_cast<float>(uv.x);
                            // FBX V origin is at the bottom; flip to match
                            // Vulkan textures (row 0 at top).
                            v.TexCoords[1] = 1.0f - static_cast<float>(uv.y);
                        }
                        if (hasTangent)
                        {
                            const ufbx_vec3 tg = ufbx_transform_direction(&geometryToNode, uMesh->vertex_tangent.values.data[tanIdx]);
                            const ufbx_vec3 convertedTangent = ConvertFbxVec3(tg, axisConversion, 1.0f);
                            v.Tangent[0] = static_cast<float>(convertedTangent.x);
                            v.Tangent[1] = static_cast<float>(convertedTangent.y);
                            v.Tangent[2] = static_cast<float>(convertedTangent.z);

                            // Recover handedness from the supplied bitangent
                            // (FBX provides explicit B; glTF provides w directly).
                            // Sign of dot(cross(N, T), B) tells us whether the
                            // bitangent goes the same direction as cross(N, T).
                            // Default +1 when no bitangent is supplied.
                            float handedness = 1.0f;
                            if (hasBitangent && hasNormal)
                            {
                                const uint32_t bitangentIdx = uMesh->vertex_bitangent.indices.data[srcIndex];
                                const ufbx_vec3 bt = ufbx_transform_direction(&geometryToNode, uMesh->vertex_bitangent.values.data[bitangentIdx]);
                                const ufbx_vec3 convertedBt = ConvertFbxVec3(bt, axisConversion, 1.0f);
                                const float bx = static_cast<float>(convertedBt.x);
                                const float by = static_cast<float>(convertedBt.y);
                                const float bz = static_cast<float>(convertedBt.z);
                                const float cx = v.Normal[1] * v.Tangent[2] - v.Normal[2] * v.Tangent[1];
                                const float cy = v.Normal[2] * v.Tangent[0] - v.Normal[0] * v.Tangent[2];
                                const float cz = v.Normal[0] * v.Tangent[1] - v.Normal[1] * v.Tangent[0];
                                const float dot = cx * bx + cy * by + cz * bz;
                                handedness = (dot < 0.0f) ? -1.0f : 1.0f;
                            }
                            v.Tangent[3] = handedness;
                        }

                        for (int c = 0; c < 3; ++c)
                        {
                            mesh.MinBounds[c] = std::min(mesh.MinBounds[c], v.Position[c]);
                            mesh.MaxBounds[c] = std::max(mesh.MaxBounds[c], v.Position[c]);
                        }

                        const uint32 newVertIndex = static_cast<uint32>(mesh.Vertices.size());
                        mesh.Vertices.push_back(v);
                        if (hasColor)
                        {
                            const ufbx_vec4 color = uMesh->vertex_color.values.data[colIdx];
                            mesh.Color0.push_back(static_cast<float>(color.x));
                            mesh.Color0.push_back(static_cast<float>(color.y));
                            mesh.Color0.push_back(static_cast<float>(color.z));
                            mesh.Color0.push_back(static_cast<float>(color.w));
                        }
                        if (hasUV1)
                        {
                            const ufbx_vec2 uv1 = uMesh->uv_sets.data[1].vertex_uv.values.data[uv1Idx];
                            mesh.TexCoords1.push_back(static_cast<float>(uv1.x));
                            mesh.TexCoords1.push_back(1.0f - static_cast<float>(uv1.y));
                        }
                        for (size_t uvi = 2; uvi < 8u; ++uvi)
                        {
                            if (!hasUVSet[uvi])
                                continue;
                            const uint32_t extraIndex = uvSetToExtraIndex[uvi];
                            const ufbx_vertex_vec2& uvAttrib = uMesh->uv_sets.data[uvi].vertex_uv;
                            const ufbx_vec2 uv = uvAttrib.values.data[uvSetIndices[uvi]];
                            mesh.ExtraTexCoords[extraIndex].push_back(static_cast<float>(uv.x));
                            mesh.ExtraTexCoords[extraIndex].push_back(1.0f - static_cast<float>(uv.y));
                        }
                        vertSourceVertex.push_back(posIdx);
                        vertexCache.emplace(key, newVertIndex);
                        outIdx[corner] = newVertIndex;
                    }

                    if (axisConversion.reverseWinding)
                    {
                        mesh.Indices.push_back(outIdx[0]);
                        mesh.Indices.push_back(outIdx[2]);
                        mesh.Indices.push_back(outIdx[1]);
                    }
                    else
                    {
                        mesh.Indices.push_back(outIdx[0]);
                        mesh.Indices.push_back(outIdx[1]);
                        mesh.Indices.push_back(outIdx[2]);
                    }
                }
            }

            // Skinning weights per logical vertex — ufbx exposes
            // `skin_deformer.vertices[]` indexed by mesh vertex (not by
            // corner index). vertSourceVertex maps each emitted Vertex
            // back to its source mesh vertex.
            if (skin && skin->vertices.count > 0 && !mesh.Vertices.empty())
            {
                const size_t vcount = mesh.Vertices.size();
                const bool useEightWeights = skin->max_weights_per_vertex > 4u;
                const size_t maxInfluences = useEightWeights ? 8u : 4u;
                mesh.Joints0.assign(vcount * 4u, 0u);
                mesh.Weights0.assign(vcount * 4u, 0.0f);
                if (useEightWeights)
                {
                    mesh.Joints1.assign(vcount * 4u, 0u);
                    mesh.Weights1.assign(vcount * 4u, 0.0f);
                }

                for (size_t vi = 0; vi < vcount; ++vi)
                {
                    const uint32_t srcVert = vertSourceVertex[vi];
                    if (srcVert >= skin->vertices.count)
                        continue;
                    const ufbx_skin_vertex sv = skin->vertices.data[srcVert];
                    uint16_t j8[8] = {};
                    float w8[8] = {};
                    for (uint32_t wi = 0; wi < sv.num_weights; ++wi)
                    {
                        const ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + wi];
                        if (sw.cluster_index >= skin->clusters.count)
                            continue;
                        const ufbx_skin_cluster* cluster = skin->clusters.data[sw.cluster_index];
                        if (!cluster || !cluster->bone_node)
                            continue;
                        const auto it = nodeToBoneIndex.find(cluster->bone_node);
                        if (it == nodeToBoneIndex.end())
                            continue;
                        AddVertexInfluence(j8, w8, maxInfluences, it->second, static_cast<float>(sw.weight));
                    }
                    NormalizeWeights(w8, maxInfluences);

                    uint16_t* j0 = mesh.Joints0.data() + vi * 4u;
                    float*    w0 = mesh.Weights0.data() + vi * 4u;
                    for (size_t k = 0; k < 4u; ++k)
                    {
                        j0[k] = j8[k];
                        w0[k] = w8[k];
                    }
                    if (useEightWeights)
                    {
                        uint16_t* j1 = mesh.Joints1.data() + vi * 4u;
                        float*    w1 = mesh.Weights1.data() + vi * 4u;
                        for (size_t k = 0; k < 4u; ++k)
                        {
                            j1[k] = j8[k + 4u];
                            w1[k] = w8[k + 4u];
                        }
                    }
                }
                // Mark the mesh as skinned when the joint/weight arrays were
                // populated correctly. Note: IsSkinned() reads `Skinned`, so
                // the size check has to be evaluated manually here.
                mesh.Skinned = !mesh.Vertices.empty()
                            && mesh.Joints0.size() == mesh.Vertices.size() * 4u
                            && mesh.Weights0.size() == mesh.Vertices.size() * 4u;
            }

            if (!mesh.HasColor0())
                mesh.Color0.clear();
            if (!mesh.HasTexCoords1())
                mesh.TexCoords1.clear();
            for (auto it = mesh.ExtraTexCoords.begin(); it != mesh.ExtraTexCoords.end();)
            {
                if (it->size() != mesh.Vertices.size() * 2u)
                    it = mesh.ExtraTexCoords.erase(it);
                else
                    ++it;
            }

            // Per-vertex deltas of the blend shape being read; stored sparse.
            Vector<float> positionDeltas;
            Vector<float> normalDeltas;
            for (const ufbx_blend_deformer* blend : uMesh->blend_deformers)
            {
                if (!blend)
                    continue;
                for (const ufbx_blend_channel* channel : blend->channels)
                {
                    if (!channel || channel->keyframes.count == 0)
                        continue;

                    const ufbx_blend_shape* shape =
                        channel->target_shape
                            ? channel->target_shape
                            : channel->keyframes.data[channel->keyframes.count - 1u].shape;
                    if (!shape)
                        continue;

                    String targetName;
                    if (channel->name.length > 0)
                        targetName.assign(channel->name.data, channel->name.length);
                    else if (shape->name.length > 0)
                        targetName.assign(shape->name.data, shape->name.length);
                    else
                        targetName = "morph_" + std::to_string(mesh.MorphTargets.size());

                    positionDeltas.assign(mesh.Vertices.size() * 3u, 0.0f);
                    normalDeltas.clear();
                    if (shape->normal_offsets.count > 0)
                        normalDeltas.assign(mesh.Vertices.size() * 3u, 0.0f);

                    bool anyPosition = false;
                    bool anyNormal = false;
                    for (size_t vi = 0; vi < mesh.Vertices.size(); ++vi)
                    {
                        const uint32_t srcVert = vertSourceVertex[vi];
                        const uint32_t offsetIndex = ufbx_get_blend_shape_offset_index(shape, srcVert);
                        if (offsetIndex == UFBX_NO_INDEX)
                            continue;

                        if (offsetIndex < shape->position_offsets.count)
                        {
                            const ufbx_vec3 delta = ConvertFbxVec3(
                                shape->position_offsets.data[offsetIndex], axisConversion, unitScale);
                            float* outDelta = positionDeltas.data() + vi * 3u;
                            outDelta[0] = static_cast<float>(delta.x);
                            outDelta[1] = static_cast<float>(delta.y);
                            outDelta[2] = static_cast<float>(delta.z);
                            anyPosition = true;
                        }
                        if (offsetIndex < shape->normal_offsets.count && !normalDeltas.empty())
                        {
                            const ufbx_vec3 delta = ConvertFbxVec3(
                                shape->normal_offsets.data[offsetIndex], axisConversion, 1.0f);
                            float* outDelta = normalDeltas.data() + vi * 3u;
                            outDelta[0] = static_cast<float>(delta.x);
                            outDelta[1] = static_cast<float>(delta.y);
                            outDelta[2] = static_cast<float>(delta.z);
                            anyNormal = true;
                        }
                    }

                    if (!anyNormal)
                        normalDeltas.clear();
                    if (anyPosition || anyNormal)
                    {
                        mesh.MorphTargetDefaultWeights.push_back(static_cast<float>(channel->weight / 100.0));
                        mesh.MorphTargets.push_back(MakeSparseMorphTarget(
                            std::move(targetName), mesh.Vertices.size(), positionDeltas, normalDeltas, {}));
                    }
                }
            }

            if (loaderOptions.GenerateMissingTangents)
                GenerateMeshTangents(mesh);

            // The further nodes instancing this mesh. The vertices carry the first
            // node's geometry_to_node, so a further node draws them through
            // geometry_to_node(it) * inverse(geometry_to_node(first)) below its node.
            if (!mesh.IsSkinned() && mesh.SourceNodeIndex >= 0 && uMesh->instances.count > 1)
            {
                const ufbx_matrix firstNodeToGeometry = ufbx_matrix_invert(&geometryToNode);
                for (size_t instanceIndex = 1; instanceIndex < uMesh->instances.count; ++instanceIndex)
                {
                    const ufbx_node* instanceNode = uMesh->instances.data[instanceIndex];
                    const auto it = instanceNode ? nodeToBoneIndex.find(instanceNode) : nodeToBoneIndex.end();
                    if (it == nodeToBoneIndex.end())
                        continue;
                    const ufbx_matrix nodeLocal =
                        ufbx_matrix_mul(&instanceNode->geometry_to_node, &firstNodeToGeometry);
                    const ufbx_matrix nodeWorld = ufbx_matrix_mul(&instanceNode->node_to_world, &nodeLocal);
                    MeshPlacement& placement = mesh.ExtraPlacements.emplace_back();
                    placement.SourceNodeIndex = static_cast<int32>(it->second);
                    StoreUfbxMatrixAsColumnMajorTargetSpace(nodeWorld, axisConversion, unitScale,
                                                            placement.SourceNodeTransform);
                    StoreUfbxMatrixAsColumnMajorTargetSpace(nodeLocal, axisConversion, unitScale,
                                                            placement.SourceNodeLocalTransform);
                }
            }

            if (!mesh.Vertices.empty() && !mesh.Indices.empty())
                m_Meshes.push_back(std::move(mesh));
        }
    }

    // ---------------------------------------------------------------
    // FBX LOD groups (C2c). ufbx surfaces Maya/Max LOD groups first-class in
    // scene->lod_groups: each owns a node whose children are the LOD models in
    // order, with per-level switch distances. Resolve each group's per-level mesh
    // node (descending through Maya's intermediate LOD_<n> transform wrappers) and
    // switch distance, then consume the lower levels into LOD0's submeshes so they
    // never render standalone. Runs before ConsumeLodSuffixFamilies (the shared
    // post-load pass) so LOD groups outrank the _LOD name-suffix source.
    if (scene->lod_groups.count > 0)
    {
        Vector<FbxLodGroup> lodGroups;
        lodGroups.reserve(scene->lod_groups.count);
        for (size_t gi = 0; gi < scene->lod_groups.count; ++gi)
        {
            const ufbx_lod_group* lg = scene->lod_groups.data[gi];
            if (!lg || lg->instances.count == 0)
                continue;
            const ufbx_node* owner = lg->instances.data[0];
            const std::string groupName(owner->name.data, owner->name.length);
            const size_t levelCount = std::min(lg->lod_levels.count, owner->children.count);
            if (levelCount < 2u)
            {
                Logger::Log::Warning(
                    "ModelAsset::LoadFBX '{}': LOD group '{}' has fewer than 2 levels; "
                    "leaving its mesh standalone.", GetName(), groupName);
                continue;
            }

            // Child order is not level order: the FBX only guarantees lod_levels
            // runs parallel to children, and exporters do write them out of order
            // (LOD1, LOD2, LOD0 among others). Resolve the order from the group's
            // own data before anything reads a "first" child.
            Vector<FbxLodGroupChild> groupChildren;
            groupChildren.reserve(levelCount);
            for (size_t k = 0; k < levelCount; ++k)
            {
                FbxLodGroupChild child;
                child.SwitchDistance = static_cast<float>(lg->lod_levels.data[k].distance);
                if (const ufbx_node* childNode = owner->children.data[k])
                    child.Name.assign(childNode->name.data, childNode->name.length);
                groupChildren.push_back(std::move(child));
            }

            FbxLodGroupOrder resolved = ResolveFbxLodGroupOrder(groupChildren, lg->relative_distances);
            Vector<uint32>& order = resolved.Levels;
            if (order.empty())
            {
                Logger::Log::Warning(
                    "ModelAsset::LoadFBX '{}': LOD group '{}' carries neither distinct switch "
                    "distances nor _LOD-numbered child names, so its level order cannot be "
                    "derived; using the file's child order. Re-export with switch distances, or "
                    "name the level nodes '<mesh>_LOD0', '<mesh>_LOD1', ...",
                    GetName(), groupName);
                order.resize(levelCount);
                std::iota(order.begin(), order.end(), 0u);
            }
            else
            {
                const auto describe = [&](const Vector<uint32>& levels) {
                    std::string text;
                    for (uint32 childIndex : levels)
                    {
                        if (!text.empty())
                            text += ", ";
                        text += groupChildren[childIndex].Name;
                    }
                    return text;
                };
                if (!resolved.OverriddenDistanceOrder.empty())
                {
                    Logger::Log::Warning(
                        "ModelAsset::LoadFBX '{}': LOD group '{}' disagrees with itself — its level "
                        "names give the order [{}] and its switch distances give [{}]. Importing in "
                        "name order and falling back to the default switch thresholds. Re-export "
                        "with thresholds that match the level names to choose the distances.",
                        GetName(), groupName, describe(order),
                        describe(resolved.OverriddenDistanceOrder));
                }
                else
                {
                    bool reordered = false;
                    for (size_t k = 0; k < order.size(); ++k)
                        reordered = reordered || order[k] != static_cast<uint32>(k);
                    if (reordered)
                        Logger::Log::Info(
                            "ModelAsset::LoadFBX '{}': LOD group '{}' stores its levels out of order; "
                            "importing in the level order its own data gives.", GetName(), groupName);
                }
            }

            const ufbx_node* lod0Node = FindPrimaryMeshNode(owner->children.data[order[0]]);
            if (!lod0Node)
            {
                Logger::Log::Warning(
                    "ModelAsset::LoadFBX '{}': LOD group '{}' LOD0 has no mesh; leaving its "
                    "meshes standalone.", GetName(), groupName);
                continue;
            }

            FbxLodGroup group;
            group.Lod0Node = nodeIndexOf(lod0Node);
            group.RelativeDistances = lg->relative_distances;
            group.UnitScale = unitScale;
            for (size_t k = 1; k < order.size(); ++k)
            {
                const ufbx_node* levelNode = FindPrimaryMeshNode(owner->children.data[order[k]]);
                if (!levelNode)
                    break; // a gap ends the chain (contiguous from LOD1)
                group.LowerNodes.push_back(nodeIndexOf(levelNode));
                // Only the distances themselves can seed switch coverages. When the
                // level names decided the order, ufbx's positional entries — including
                // the synthetic level-0 one — do not describe these levels, so the
                // chain takes the default threshold table instead.
                if (resolved.FromSwitchDistances)
                    group.SwitchDistances.push_back(
                        static_cast<float>(lg->lod_levels.data[order[k]].distance));
            }
            if (!group.LowerNodes.empty())
                lodGroups.push_back(std::move(group));
        }
        if (!lodGroups.empty())
            AssembleFbxLodGroupChains(m_Meshes, lodGroups, GetName());
    }

    // ---------------------------------------------------------------
    // Animation metadata — names + presence flag. Curve data is loaded
    // separately by AnimationClip when the user selects a clip.
    // ---------------------------------------------------------------
    m_HasAnimations = scene->anim_stacks.count > 0;
    m_AnimationNames.reserve(scene->anim_stacks.count);
    m_EmbeddedClipGuids.reserve(scene->anim_stacks.count);
    for (size_t ai = 0; ai < scene->anim_stacks.count; ++ai)
    {
        const ufbx_anim_stack* stack = scene->anim_stacks.data[ai];
        if (stack->name.length > 0)
            m_AnimationNames.emplace_back(stack->name.data, stack->name.length);
        else
            m_AnimationNames.emplace_back("Animation " + std::to_string(ai));

        // Eagerly load + register each embedded clip into the runtime
        // ClipStore under a stable derived GUID. Consumers (Animator
        // component, inspector, preview) only ever resolve `clipGuid` →
        // ClipStore index; there is no separate "model index" path.
        const GUID clipGuid = MintEmbeddedClipGuid(static_cast<uint32>(ai));
        if (!StackAnimatesAnyProperty(*stack))
            continue;

        auto clip = std::make_shared<AnimationClip>(clipGuid, modelPathForExternalTextures);
        clip->SetSelectedAnimationIndex(static_cast<uint32>(ai));
        // Populate the clip's source-model path so cross-rig auto-bootstrap
        // (AnimationSystem.cpp:200-262) can fire its fast path. Without
        // this, the FBX-derived clips slip through the structural body-
        // bone fallback when target + source rigs share bone indices
        // (e.g., Synty BusinessMale + A_Walk_F_Masc).
        clip->SetSourceInfo(modelPathForExternalTextures, static_cast<uint32>(ai));
        // Parse from the bytes this load already holds: Load() would read the
        // file again and keep a copy of it for the clip's lifetime.
        if (clip->LoadFromData(data) && clip->GetDuration() > 0.0f)
        {
            StageOrPublishRuntimeClip(clipGuid, std::move(clip));
        }
        else
        {
            Logger::Log::Warning("ModelAsset: failed to load embedded clip {} of '{}'",
                                 ai, GetName());
        }
    }

    skeleton->ComputeTopologicalSort();

    size_t totalVerts = 0;
    size_t totalIndices = 0;
    for (const auto& mesh : m_Meshes)
    {
        totalVerts += mesh.Vertices.size();
        totalIndices += mesh.Indices.size();
    }
    Logger::Log::Info("ModelAsset::LoadFBX '{}' → meshes={}, verts={}, indices={}, bones={}, anims={}, cameras={}, lights={}, helpers={}",
                      GetName(), m_Meshes.size(), totalVerts, totalIndices, boneCount,
                      scene->anim_stacks.count, m_Cameras.size(), m_Lights.size(), m_SceneNodes.size());

    ufbx_free_scene(scene);

    // The asset is valid if it carried *anything* useful — geometry, an
    // armature, or animation curves. Animation-only FBXes (e.g. Synty
    // anim packs) ship no meshes; we still want them in the asset
    // registry so AnimationClip can read clips from them.
    return !m_Meshes.empty() || m_HasAnimations || boneCount > 0
        || !m_Cameras.empty() || !m_Lights.empty() || !m_SceneNodes.empty();
}

// Called from PostLoad AFTER authored-slot and generated/cooked LOD
// resolution: those append triangles whose UV footprints the load-time mesh
// set does not cover, so demoting any earlier could miss a cutout an LOD
// still reaches. Lives in this TU for the file-local footprint machinery;
// only FBX-merge-inferred Mask (AlphaModeInferred) is eligible, so the call
// is a no-op for every other loader.
void ModelAsset::DemoteInferredMaskMaterials()
{
#if defined(GE_HAVE_STB)
    if (const uint32 demoted =
            DemoteMaskMaterialsWithOpaqueUVFootprint(m_Meshes, m_Materials, m_EmbeddedImages))
    {
        Logger::Log::Info(
            "ModelAsset '{}': demoted {} inferred-Mask material(s) to Opaque "
            "(fully-opaque diffuse-UV footprint)",
            GetName(), demoted);
    }
#endif
}

} // namespace GameEngine
