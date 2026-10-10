#pragma once

#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine::Ocean
{

// CPU representation of a saved ocean depth cache. Depth values are positive
// meters below calm sea level, laid out row-major over a world-space rectangle.
// The renderer uploads this as an R32F texture and the seabed-depth bake takes
// the shallowest value across the saved cache, analytic seabeds, and dynamic
// depth contributors.
struct OceanDepthCacheAssetDesc
{
    uint32 Width = 0;
    uint32 Height = 0;
    float32 OriginX = 0.0f;
    float32 OriginZ = 0.0f;
    float32 SizeX = 0.0f;
    float32 SizeZ = 0.0f;
    float32 DeepWaterDepth = 60000.0f;
};

class OceanDepthCacheAsset
{
public:
    static constexpr uint32 kFileVersion = 1;

    bool Reset(const OceanDepthCacheAssetDesc& desc, std::vector<float32> depths,
               std::string* error = nullptr);

    bool IsValid() const { return m_Desc.Width > 0 && m_Desc.Height > 0 && !m_Depths.empty(); }

    const OceanDepthCacheAssetDesc& GetDesc() const { return m_Desc; }
    const std::vector<float32>& GetDepths() const { return m_Depths; }

    bool SampleDepth(float32 worldX, float32 worldZ, float32& outDepth) const;

    bool SaveBinary(const std::filesystem::path& path, std::string* error = nullptr) const;
    bool LoadBinary(const std::filesystem::path& path, std::string* error = nullptr);

private:
    OceanDepthCacheAssetDesc m_Desc{};
    std::vector<float32> m_Depths;
};

struct OceanDepthCacheBakeDesc
{
    uint32 Width = 0;
    uint32 Height = 0;
    float32 OriginX = 0.0f;
    float32 OriginZ = 0.0f;
    float32 SizeX = 0.0f;
    float32 SizeZ = 0.0f;
    float32 SeaLevel = 0.0f;
    float32 DeepWaterDepth = 60000.0f;
};

using OceanDepthCacheHeightSampleFn =
    bool (*)(float32 worldX, float32 worldZ, float32& outFloorY, void* userData);

struct OceanDepthCacheHeightfieldSource
{
    const float32* Samples = nullptr; // Row-major height samples.
    uint32 Width = 0;
    uint32 Height = 0;
    float32 OriginX = 0.0f;
    float32 OriginZ = 0.0f;
    float32 SizeX = 0.0f;
    float32 SizeZ = 0.0f;
    float32 HeightScale = 1.0f;
    float32 WorldOriginY = 0.0f;
    uint32 LayerMask = 0xFFFFFFFFu;
    bool Enabled = true;
};

struct OceanDepthCacheMeshSource
{
    // Non-owning indexed triangle mesh. Positions may point to an interleaved
    // vertex buffer; VertexStride is measured in bytes.
    const Mathematics::Vector3* Positions = nullptr;
    uint32 VertexStride = sizeof(Mathematics::Vector3);
    uint32 VertexCount = 0;
    const uint32* Indices = nullptr;
    uint32 IndexCount = 0;

    // Optional column-major local-to-world transform. When disabled, positions
    // are treated as world space.
    bool UseLocalToWorld = false;
    float32 LocalToWorld[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};

    uint32 LayerMask = 0xFFFFFFFFu;
    bool Enabled = true;
};

// Builds a saved depth cache from a geometry/terrain height sampler. The sampler
// should return the highest floor/seabed Y under the ocean at each world XZ. A
// false return writes the deep-water sentinel for that texel.
bool BakeOceanDepthCache(const OceanDepthCacheBakeDesc& desc,
                         OceanDepthCacheHeightSampleFn sampleHeight,
                         void* userData,
                         OceanDepthCacheAsset& outCache,
                         std::string* error = nullptr);

// Builds a saved depth cache from one or more terrain/heightfield grids. This is
// the CPU terrain-capture path: sources can be layer-filtered, tiled, and
// overlapped; the highest sampled floor becomes the shallowest water depth.
bool BakeOceanDepthCacheFromHeightfields(const OceanDepthCacheBakeDesc& desc,
                                         const OceanDepthCacheHeightfieldSource* sources,
                                         uint32 sourceCount,
                                         uint32 sourceLayerMask,
                                         OceanDepthCacheAsset& outCache,
                                         std::string* error = nullptr);

// Builds a saved depth cache from real indexed triangle geometry. Sources are
// filtered by LayerMask and sampled top-down: the highest triangle surface at
// each texel's world XZ becomes the seabed height before conversion to depth.
bool BakeOceanDepthCacheFromMeshes(const OceanDepthCacheBakeDesc& desc,
                                   const OceanDepthCacheMeshSource* sources,
                                   uint32 sourceCount,
                                   uint32 sourceLayerMask,
                                   OceanDepthCacheAsset& outCache,
                                   std::string* error = nullptr);

// Composes multiple saved depth caches into one runtime cache over their union
// bounds. Overlap resolves to the shallowest water depth (minimum positive
// depth), matching the seabed-depth compute.
bool ComposeOceanDepthCaches(const OceanDepthCacheAsset* caches,
                             uint32 cacheCount,
                             OceanDepthCacheAsset& outCache,
                             std::string* error = nullptr);

} // namespace GameEngine::Ocean
