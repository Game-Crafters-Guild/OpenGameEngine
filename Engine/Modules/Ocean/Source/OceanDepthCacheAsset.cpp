#include "Ocean/OceanDepthCacheAsset.h"

#include "AssetCore/SharedFileRead.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>

namespace GameEngine::Ocean
{

namespace
{

constexpr char kMagic[8] = {'G', 'E', 'O', 'D', 'C', 'A', 'C', 'H'};
constexpr uint32 kMaxDepthCacheResolution = 8192u;

void SetOceanDepthCacheError(std::string* error, const char* message)
{
    if (error)
        *error = message;
}

template <typename T>
bool WriteValue(std::ofstream& out, const T& value)
{
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return static_cast<bool>(out);
}

template <typename T>
bool ReadValue(SharedFileReader& in, T& value)
{
    return in.Read(&value, sizeof(T)) == static_cast<int64>(sizeof(T));
}

bool IsValidHeightfieldSource(const OceanDepthCacheHeightfieldSource& source, uint32 layerMask)
{
    return source.Enabled &&
           source.Samples != nullptr &&
           source.Width > 0 &&
           source.Height > 0 &&
           source.SizeX > 0.0f &&
           source.SizeZ > 0.0f &&
           (source.LayerMask & layerMask) != 0u;
}

float32 SampleHeightfieldSource(const OceanDepthCacheHeightfieldSource& source, float32 worldX,
                                float32 worldZ)
{
    const float32 u = std::clamp((worldX - source.OriginX) / source.SizeX, 0.0f, 1.0f);
    const float32 v = std::clamp((worldZ - source.OriginZ) / source.SizeZ, 0.0f, 1.0f);
    const float32 gx = u * static_cast<float32>(source.Width > 1 ? source.Width - 1 : 0);
    const float32 gz = v * static_cast<float32>(source.Height > 1 ? source.Height - 1 : 0);
    const float32 fx = std::floor(gx);
    const float32 fz = std::floor(gz);
    const uint32 x0 = static_cast<uint32>(std::clamp<int32>(static_cast<int32>(fx), 0,
                                                            static_cast<int32>(source.Width) - 1));
    const uint32 z0 = static_cast<uint32>(std::clamp<int32>(static_cast<int32>(fz), 0,
                                                            static_cast<int32>(source.Height) - 1));
    const uint32 x1 = std::min(x0 + 1, source.Width - 1);
    const uint32 z1 = std::min(z0 + 1, source.Height - 1);
    const float32 tx = gx - fx;
    const float32 tz = gz - fz;

    const auto fetch = [&](uint32 x, uint32 z) -> float32 {
        return source.Samples[static_cast<size_t>(z) * source.Width + x];
    };

    const float32 h00 = fetch(x0, z0);
    const float32 h10 = fetch(x1, z0);
    const float32 h01 = fetch(x0, z1);
    const float32 h11 = fetch(x1, z1);
    const float32 h0 = h00 + (h10 - h00) * tx;
    const float32 h1 = h01 + (h11 - h01) * tx;
    return (h0 + (h1 - h0) * tz) * source.HeightScale + source.WorldOriginY;
}

struct HeightfieldBakeContext
{
    const OceanDepthCacheHeightfieldSource* Sources = nullptr;
    uint32 SourceCount = 0;
    uint32 SourceLayerMask = 0xFFFFFFFFu;
};

struct PreparedMeshTriangle
{
    float32 X0 = 0.0f;
    float32 Y0 = 0.0f;
    float32 Z0 = 0.0f;
    float32 X1 = 0.0f;
    float32 Y1 = 0.0f;
    float32 Z1 = 0.0f;
    float32 X2 = 0.0f;
    float32 Y2 = 0.0f;
    float32 Z2 = 0.0f;
    float32 MinX = 0.0f;
    float32 MaxX = 0.0f;
    float32 MinZ = 0.0f;
    float32 MaxZ = 0.0f;
    float32 Denom = 0.0f;
};

struct MeshBakeContext
{
    std::vector<PreparedMeshTriangle> Triangles;
};

bool IsValidMeshSource(const OceanDepthCacheMeshSource& source, uint32 layerMask)
{
    return source.Enabled &&
           source.Positions != nullptr &&
           source.VertexStride >= sizeof(Mathematics::Vector3) &&
           source.VertexCount > 0 &&
           source.Indices != nullptr &&
           source.IndexCount >= 3 &&
           (source.LayerMask & layerMask) != 0u;
}

Mathematics::Vector3 ReadMeshPosition(const OceanDepthCacheMeshSource& source, uint32 index)
{
    const auto* base = reinterpret_cast<const std::byte*>(source.Positions);
    const auto* pos = reinterpret_cast<const Mathematics::Vector3*>(
        base + static_cast<size_t>(index) * static_cast<size_t>(source.VertexStride));
    Mathematics::Vector3 p = *pos;
    if (!source.UseLocalToWorld)
        return p;

    const float32* m = source.LocalToWorld;
    return Mathematics::Vector3{
        m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
        m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
        m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}

void AddPreparedTriangle(const Mathematics::Vector3& v0,
                         const Mathematics::Vector3& v1,
                         const Mathematics::Vector3& v2,
                         std::vector<PreparedMeshTriangle>& triangles)
{
    if (!std::isfinite(v0.x) || !std::isfinite(v0.y) || !std::isfinite(v0.z) ||
        !std::isfinite(v1.x) || !std::isfinite(v1.y) || !std::isfinite(v1.z) ||
        !std::isfinite(v2.x) || !std::isfinite(v2.y) || !std::isfinite(v2.z))
    {
        return;
    }

    PreparedMeshTriangle tri{};
    tri.X0 = v0.x;
    tri.Y0 = v0.y;
    tri.Z0 = v0.z;
    tri.X1 = v1.x;
    tri.Y1 = v1.y;
    tri.Z1 = v1.z;
    tri.X2 = v2.x;
    tri.Y2 = v2.y;
    tri.Z2 = v2.z;
    tri.MinX = std::min({v0.x, v1.x, v2.x});
    tri.MaxX = std::max({v0.x, v1.x, v2.x});
    tri.MinZ = std::min({v0.z, v1.z, v2.z});
    tri.MaxZ = std::max({v0.z, v1.z, v2.z});

    tri.Denom = (tri.Z1 - tri.Z2) * (tri.X0 - tri.X2) +
                (tri.X2 - tri.X1) * (tri.Z0 - tri.Z2);
    if (std::abs(tri.Denom) <= 1e-7f)
        return;

    triangles.push_back(tri);
}

bool PrepareMeshBakeContext(const OceanDepthCacheMeshSource* sources,
                            uint32 sourceCount,
                            uint32 sourceLayerMask,
                            MeshBakeContext& outContext,
                            std::string* error)
{
    if (!sources || sourceCount == 0)
    {
        SetOceanDepthCacheError(error, "ocean depth cache mesh bake needs at least one source");
        return false;
    }
    if (sourceLayerMask == 0u)
    {
        SetOceanDepthCacheError(error, "ocean depth cache mesh bake has an empty source layer mask");
        return false;
    }

    bool hasMatchingSource = false;
    size_t triangleCapacity = 0;
    for (uint32 i = 0; i < sourceCount; ++i)
    {
        if (!IsValidMeshSource(sources[i], sourceLayerMask))
            continue;

        hasMatchingSource = true;
        triangleCapacity += static_cast<size_t>(sources[i].IndexCount / 3u);
    }

    if (!hasMatchingSource)
    {
        SetOceanDepthCacheError(error, "ocean depth cache mesh bake has no enabled matching sources");
        return false;
    }

    outContext.Triangles.clear();
    outContext.Triangles.reserve(triangleCapacity);
    for (uint32 si = 0; si < sourceCount; ++si)
    {
        const OceanDepthCacheMeshSource& source = sources[si];
        if (!IsValidMeshSource(source, sourceLayerMask))
            continue;

        const uint32 triangleCount = source.IndexCount / 3u;
        for (uint32 ti = 0; ti < triangleCount; ++ti)
        {
            const uint32 i0 = source.Indices[ti * 3u + 0u];
            const uint32 i1 = source.Indices[ti * 3u + 1u];
            const uint32 i2 = source.Indices[ti * 3u + 2u];
            if (i0 >= source.VertexCount || i1 >= source.VertexCount || i2 >= source.VertexCount)
                continue;

            AddPreparedTriangle(ReadMeshPosition(source, i0),
                                ReadMeshPosition(source, i1),
                                ReadMeshPosition(source, i2),
                                outContext.Triangles);
        }
    }

    if (outContext.Triangles.empty())
    {
        SetOceanDepthCacheError(error, "ocean depth cache mesh bake found no valid triangles");
        return false;
    }

    return true;
}

bool SampleHeightfields(float32 worldX, float32 worldZ, float32& outFloorY, void* userData)
{
    const auto* ctx = static_cast<const HeightfieldBakeContext*>(userData);
    if (!ctx || !ctx->Sources)
        return false;

    bool found = false;
    float32 highestFloor = std::numeric_limits<float32>::lowest();
    for (uint32 i = 0; i < ctx->SourceCount; ++i)
    {
        const OceanDepthCacheHeightfieldSource& source = ctx->Sources[i];
        if (!IsValidHeightfieldSource(source, ctx->SourceLayerMask))
            continue;

        if (worldX < source.OriginX || worldZ < source.OriginZ ||
            worldX > source.OriginX + source.SizeX ||
            worldZ > source.OriginZ + source.SizeZ)
            continue;

        const float32 height = SampleHeightfieldSource(source, worldX, worldZ);
        if (!std::isfinite(height))
            continue;

        highestFloor = found ? std::max(highestFloor, height) : height;
        found = true;
    }

    if (!found)
        return false;
    outFloorY = highestFloor;
    return true;
}

bool SampleMeshes(float32 worldX, float32 worldZ, float32& outFloorY, void* userData)
{
    const auto* ctx = static_cast<const MeshBakeContext*>(userData);
    if (!ctx)
        return false;

    constexpr float32 kEdgeEps = -1e-5f;
    bool found = false;
    float32 highestFloor = std::numeric_limits<float32>::lowest();
    for (const PreparedMeshTriangle& tri : ctx->Triangles)
    {
        if (worldX < tri.MinX || worldX > tri.MaxX ||
            worldZ < tri.MinZ || worldZ > tri.MaxZ)
        {
            continue;
        }

        const float32 a = ((tri.Z1 - tri.Z2) * (worldX - tri.X2) +
                           (tri.X2 - tri.X1) * (worldZ - tri.Z2)) / tri.Denom;
        const float32 b = ((tri.Z2 - tri.Z0) * (worldX - tri.X2) +
                           (tri.X0 - tri.X2) * (worldZ - tri.Z2)) / tri.Denom;
        const float32 c = 1.0f - a - b;
        if (a < kEdgeEps || b < kEdgeEps || c < kEdgeEps)
            continue;

        const float32 y = a * tri.Y0 + b * tri.Y1 + c * tri.Y2;
        if (!std::isfinite(y))
            continue;

        highestFloor = found ? std::max(highestFloor, y) : y;
        found = true;
    }

    if (!found)
        return false;
    outFloorY = highestFloor;
    return true;
}

} // anonymous namespace

bool OceanDepthCacheAsset::Reset(const OceanDepthCacheAssetDesc& desc,
                                 std::vector<float32> depths, std::string* error)
{
    if (desc.Width == 0 || desc.Height == 0 ||
        desc.Width > kMaxDepthCacheResolution || desc.Height > kMaxDepthCacheResolution)
    {
        SetOceanDepthCacheError(error, "invalid ocean depth cache resolution");
        return false;
    }
    if (desc.SizeX <= 0.0f || desc.SizeZ <= 0.0f)
    {
        SetOceanDepthCacheError(error, "invalid ocean depth cache world size");
        return false;
    }
    const size_t expected = static_cast<size_t>(desc.Width) * static_cast<size_t>(desc.Height);
    if (depths.size() != expected)
    {
        SetOceanDepthCacheError(error, "ocean depth cache data size does not match resolution");
        return false;
    }

    const float32 deep = std::isfinite(desc.DeepWaterDepth) && desc.DeepWaterDepth > 0.0f
                             ? desc.DeepWaterDepth
                             : 60000.0f;
    for (float32& d : depths)
    {
        if (!std::isfinite(d))
            d = deep;
        else
            d = std::clamp(d, 0.0f, deep);
    }

    m_Desc = desc;
    m_Desc.DeepWaterDepth = deep;
    m_Depths = std::move(depths);
    return true;
}

bool OceanDepthCacheAsset::SampleDepth(float32 worldX, float32 worldZ, float32& outDepth) const
{
    if (!IsValid())
        return false;

    const float32 u = (worldX - m_Desc.OriginX) / m_Desc.SizeX;
    const float32 v = (worldZ - m_Desc.OriginZ) / m_Desc.SizeZ;
    if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f)
        return false;

    const float32 gx = u * static_cast<float32>(m_Desc.Width) - 0.5f;
    const float32 gz = v * static_cast<float32>(m_Desc.Height) - 0.5f;
    const float32 fx = std::floor(gx);
    const float32 fz = std::floor(gz);
    const int32 x0 = static_cast<int32>(fx);
    const int32 z0 = static_cast<int32>(fz);
    const float32 tx = gx - fx;
    const float32 tz = gz - fz;

    const auto fetch = [&](int32 x, int32 z) -> float32 {
        x = std::clamp(x, 0, static_cast<int32>(m_Desc.Width) - 1);
        z = std::clamp(z, 0, static_cast<int32>(m_Desc.Height) - 1);
        return m_Depths[static_cast<size_t>(z) * m_Desc.Width + static_cast<size_t>(x)];
    };

    const float32 d00 = fetch(x0, z0);
    const float32 d10 = fetch(x0 + 1, z0);
    const float32 d01 = fetch(x0, z0 + 1);
    const float32 d11 = fetch(x0 + 1, z0 + 1);
    const float32 d0 = d00 + (d10 - d00) * tx;
    const float32 d1 = d01 + (d11 - d01) * tx;
    outDepth = d0 + (d1 - d0) * tz;
    return true;
}

bool OceanDepthCacheAsset::SaveBinary(const std::filesystem::path& path, std::string* error) const
{
    if (!IsValid())
    {
        SetOceanDepthCacheError(error, "cannot save invalid ocean depth cache");
        return false;
    }

    std::error_code ec;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::binary);
    if (!out)
    {
        SetOceanDepthCacheError(error, "failed to open ocean depth cache for writing");
        return false;
    }

    out.write(kMagic, sizeof(kMagic));
    if (!WriteValue(out, kFileVersion) ||
        !WriteValue(out, m_Desc.Width) ||
        !WriteValue(out, m_Desc.Height) ||
        !WriteValue(out, m_Desc.OriginX) ||
        !WriteValue(out, m_Desc.OriginZ) ||
        !WriteValue(out, m_Desc.SizeX) ||
        !WriteValue(out, m_Desc.SizeZ) ||
        !WriteValue(out, m_Desc.DeepWaterDepth))
    {
        SetOceanDepthCacheError(error, "failed to write ocean depth cache header");
        return false;
    }

    const uint64 count = static_cast<uint64>(m_Depths.size());
    if (!WriteValue(out, count))
    {
        SetOceanDepthCacheError(error, "failed to write ocean depth cache data count");
        return false;
    }
    out.write(reinterpret_cast<const char*>(m_Depths.data()),
              static_cast<std::streamsize>(m_Depths.size() * sizeof(float32)));
    if (!out)
    {
        SetOceanDepthCacheError(error, "failed to write ocean depth cache data");
        return false;
    }
    return true;
}

bool OceanDepthCacheAsset::LoadBinary(const std::filesystem::path& path, std::string* error)
{
    SharedFileReader in(path);
    if (!in.IsOpen())
    {
        SetOceanDepthCacheError(error, "failed to open ocean depth cache for reading");
        return false;
    }

    char magic[8] = {};
    if (in.Read(magic, sizeof(magic)) != static_cast<int64>(sizeof(magic)) ||
        std::memcmp(magic, kMagic, sizeof(magic)) != 0)
    {
        SetOceanDepthCacheError(error, "invalid ocean depth cache magic");
        return false;
    }

    uint32 version = 0;
    OceanDepthCacheAssetDesc desc{};
    if (!ReadValue(in, version) ||
        !ReadValue(in, desc.Width) ||
        !ReadValue(in, desc.Height) ||
        !ReadValue(in, desc.OriginX) ||
        !ReadValue(in, desc.OriginZ) ||
        !ReadValue(in, desc.SizeX) ||
        !ReadValue(in, desc.SizeZ) ||
        !ReadValue(in, desc.DeepWaterDepth))
    {
        SetOceanDepthCacheError(error, "failed to read ocean depth cache header");
        return false;
    }
    if (version != kFileVersion)
    {
        SetOceanDepthCacheError(error, "unsupported ocean depth cache version");
        return false;
    }

    uint64 count = 0;
    if (!ReadValue(in, count))
    {
        SetOceanDepthCacheError(error, "failed to read ocean depth cache data count");
        return false;
    }
    const uint64 expected = static_cast<uint64>(desc.Width) * static_cast<uint64>(desc.Height);
    if (count != expected || count > static_cast<uint64>(std::numeric_limits<size_t>::max()))
    {
        SetOceanDepthCacheError(error, "invalid ocean depth cache data count");
        return false;
    }

    std::vector<float32> depths(static_cast<size_t>(count));
    const uint64 depthBytes = depths.size() * sizeof(float32);
    if (in.Read(depths.data(), depthBytes) != static_cast<int64>(depthBytes))
    {
        SetOceanDepthCacheError(error, "failed to read ocean depth cache data");
        return false;
    }

    return Reset(desc, std::move(depths), error);
}

bool BakeOceanDepthCache(const OceanDepthCacheBakeDesc& desc,
                         OceanDepthCacheHeightSampleFn sampleHeight,
                         void* userData,
                         OceanDepthCacheAsset& outCache,
                         std::string* error)
{
    if (!sampleHeight)
    {
        SetOceanDepthCacheError(error, "ocean depth cache bake needs a height sampler");
        return false;
    }
    if (desc.Width == 0 || desc.Height == 0 || desc.SizeX <= 0.0f || desc.SizeZ <= 0.0f)
    {
        SetOceanDepthCacheError(error, "invalid ocean depth cache bake bounds");
        return false;
    }

    const float32 deep = std::isfinite(desc.DeepWaterDepth) && desc.DeepWaterDepth > 0.0f
                             ? desc.DeepWaterDepth
                             : 60000.0f;
    std::vector<float32> depths(static_cast<size_t>(desc.Width) * desc.Height, deep);
    const float32 texelX = desc.SizeX / static_cast<float32>(desc.Width);
    const float32 texelZ = desc.SizeZ / static_cast<float32>(desc.Height);

    for (uint32 z = 0; z < desc.Height; ++z)
    {
        const float32 worldZ = desc.OriginZ + (static_cast<float32>(z) + 0.5f) * texelZ;
        for (uint32 x = 0; x < desc.Width; ++x)
        {
            const float32 worldX = desc.OriginX + (static_cast<float32>(x) + 0.5f) * texelX;
            float32 floorY = 0.0f;
            if (!sampleHeight(worldX, worldZ, floorY, userData) || !std::isfinite(floorY))
                continue;

            const float32 depth = std::clamp(desc.SeaLevel - floorY, 0.0f, deep);
            depths[static_cast<size_t>(z) * desc.Width + x] = depth;
        }
    }

    OceanDepthCacheAssetDesc assetDesc{};
    assetDesc.Width = desc.Width;
    assetDesc.Height = desc.Height;
    assetDesc.OriginX = desc.OriginX;
    assetDesc.OriginZ = desc.OriginZ;
    assetDesc.SizeX = desc.SizeX;
    assetDesc.SizeZ = desc.SizeZ;
    assetDesc.DeepWaterDepth = deep;
    return outCache.Reset(assetDesc, std::move(depths), error);
}

bool BakeOceanDepthCacheFromHeightfields(const OceanDepthCacheBakeDesc& desc,
                                         const OceanDepthCacheHeightfieldSource* sources,
                                         uint32 sourceCount,
                                         uint32 sourceLayerMask,
                                         OceanDepthCacheAsset& outCache,
                                         std::string* error)
{
    if (!sources || sourceCount == 0)
    {
        SetOceanDepthCacheError(error, "ocean depth cache heightfield bake needs at least one source");
        return false;
    }
    if (sourceLayerMask == 0u)
    {
        SetOceanDepthCacheError(error, "ocean depth cache heightfield bake has an empty source layer mask");
        return false;
    }

    bool hasUsableSource = false;
    for (uint32 i = 0; i < sourceCount; ++i)
    {
        if (IsValidHeightfieldSource(sources[i], sourceLayerMask))
        {
            hasUsableSource = true;
            break;
        }
    }
    if (!hasUsableSource)
    {
        SetOceanDepthCacheError(error, "ocean depth cache heightfield bake has no enabled matching sources");
        return false;
    }

    HeightfieldBakeContext ctx{};
    ctx.Sources = sources;
    ctx.SourceCount = sourceCount;
    ctx.SourceLayerMask = sourceLayerMask;
    return BakeOceanDepthCache(desc, &SampleHeightfields, &ctx, outCache, error);
}

bool BakeOceanDepthCacheFromMeshes(const OceanDepthCacheBakeDesc& desc,
                                   const OceanDepthCacheMeshSource* sources,
                                   uint32 sourceCount,
                                   uint32 sourceLayerMask,
                                   OceanDepthCacheAsset& outCache,
                                   std::string* error)
{
    MeshBakeContext ctx;
    if (!PrepareMeshBakeContext(sources, sourceCount, sourceLayerMask, ctx, error))
        return false;

    return BakeOceanDepthCache(desc, &SampleMeshes, &ctx, outCache, error);
}

bool ComposeOceanDepthCaches(const OceanDepthCacheAsset* caches,
                             uint32 cacheCount,
                             OceanDepthCacheAsset& outCache,
                             std::string* error)
{
    if (!caches || cacheCount == 0)
    {
        SetOceanDepthCacheError(error, "ocean depth cache composition needs at least one cache");
        return false;
    }

    uint32 validCount = 0;
    uint32 firstValid = 0;
    float32 minX = std::numeric_limits<float32>::max();
    float32 minZ = std::numeric_limits<float32>::max();
    float32 maxX = std::numeric_limits<float32>::lowest();
    float32 maxZ = std::numeric_limits<float32>::lowest();
    float32 texelX = std::numeric_limits<float32>::max();
    float32 texelZ = std::numeric_limits<float32>::max();
    float32 deep = 0.0f;

    for (uint32 i = 0; i < cacheCount; ++i)
    {
        if (!caches[i].IsValid())
            continue;

        if (validCount == 0)
            firstValid = i;
        ++validCount;

        const OceanDepthCacheAssetDesc& desc = caches[i].GetDesc();
        minX = std::min(minX, desc.OriginX);
        minZ = std::min(minZ, desc.OriginZ);
        maxX = std::max(maxX, desc.OriginX + desc.SizeX);
        maxZ = std::max(maxZ, desc.OriginZ + desc.SizeZ);
        texelX = std::min(texelX, desc.SizeX / static_cast<float32>(desc.Width));
        texelZ = std::min(texelZ, desc.SizeZ / static_cast<float32>(desc.Height));
        deep = std::max(deep, desc.DeepWaterDepth);
    }

    if (validCount == 0)
    {
        SetOceanDepthCacheError(error, "ocean depth cache composition has no valid caches");
        return false;
    }
    if (validCount == 1)
    {
        outCache = caches[firstValid];
        return true;
    }

    const float32 sizeX = maxX - minX;
    const float32 sizeZ = maxZ - minZ;
    if (sizeX <= 0.0f || sizeZ <= 0.0f || texelX <= 0.0f || texelZ <= 0.0f)
    {
        SetOceanDepthCacheError(error, "invalid ocean depth cache composition bounds");
        return false;
    }

    const uint32 outWidth = static_cast<uint32>(std::ceil(sizeX / texelX));
    const uint32 outHeight = static_cast<uint32>(std::ceil(sizeZ / texelZ));
    if (outWidth == 0 || outHeight == 0 ||
        outWidth > kMaxDepthCacheResolution || outHeight > kMaxDepthCacheResolution)
    {
        SetOceanDepthCacheError(error, "composed ocean depth cache resolution is too large");
        return false;
    }

    if (!std::isfinite(deep) || deep <= 0.0f)
        deep = 60000.0f;

    std::vector<float32> depths(static_cast<size_t>(outWidth) * outHeight, deep);
    const float32 outTexelX = sizeX / static_cast<float32>(outWidth);
    const float32 outTexelZ = sizeZ / static_cast<float32>(outHeight);
    for (uint32 z = 0; z < outHeight; ++z)
    {
        const float32 worldZ = minZ + (static_cast<float32>(z) + 0.5f) * outTexelZ;
        for (uint32 x = 0; x < outWidth; ++x)
        {
            const float32 worldX = minX + (static_cast<float32>(x) + 0.5f) * outTexelX;
            float32 depth = deep;
            for (uint32 i = 0; i < cacheCount; ++i)
            {
                float32 candidate = 0.0f;
                if (caches[i].SampleDepth(worldX, worldZ, candidate))
                    depth = std::min(depth, candidate);
            }
            depths[static_cast<size_t>(z) * outWidth + x] = depth;
        }
    }

    OceanDepthCacheAssetDesc outDesc{};
    outDesc.Width = outWidth;
    outDesc.Height = outHeight;
    outDesc.OriginX = minX;
    outDesc.OriginZ = minZ;
    outDesc.SizeX = sizeX;
    outDesc.SizeZ = sizeZ;
    outDesc.DeepWaterDepth = deep;
    return outCache.Reset(outDesc, std::move(depths), error);
}

} // namespace GameEngine::Ocean
