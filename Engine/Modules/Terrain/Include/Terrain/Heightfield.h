#pragma once

#include "Terrain/TerrainTypes.h"
#include "Mathematics/Vector3.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine::Terrain
{

/// The world-space base noise at one position: the fractal value noise every world-space fill
/// writes (FillRegionWithNoiseWorldSpace), `octaves` octaves from `frequency` doubling, each half
/// the amplitude of the one before, normalized to [0, amplitude].
///
/// An octave whose lattice cell (1 / its frequency, in meters) is shorter than
/// `minResolvedCellMeters` is replaced by its mean (0.5 of its amplitude) instead of being
/// evaluated: the band limit a coarser lattice samples at (its cell at least twice the lattice
/// spacing), which keeps the mean height of every level equal. 0 evaluates every octave, the
/// full-detail value the fills write, bit for bit.
float32 SampleWorldSpaceNoise(float32 worldX, float32 worldZ, float32 frequency, float32 amplitude, uint32 octaves,
                              uint32 seed, float32 minResolvedCellMeters = 0.0f);

/// A PNG heightmap's grid, from its header.
struct PngHeightmapSize
{
    uint32 Width = 0;
    uint32 Height = 0;
};

/// Reads the grid of the PNG at `file` from its header and checks it against the PNG decoder
/// (HeightfieldData::LoadFromPNG16), which decodes a PNG whole and refuses more than 2^30 samples
/// times channels (a palette image counts 4): about 1.07 billion grayscale samples, 32768 x 32767.
/// For .png files only; other image formats have no such check. Returns an empty string and fills
/// `out`, else the reason worded as the fix (export the heightmap as .r32, which has no size limit).
std::string ResolvePngHeightmapSize(const std::filesystem::path& file, PngHeightmapSize& out);

// CPU-side heightfield data stored as a flat float grid.
// Origin is at (0,0), extending to (Width-1, Height-1) in sample space.
// World-space mapping is handled externally via TerrainConfig.
class HeightfieldData
{
public:
    HeightfieldData() = default;
    HeightfieldData(uint32 width, uint32 height, float32 defaultValue = 0.0f);

    // ---- Dimensions ----

    uint32 GetWidth() const { return m_Width; }
    uint32 GetHeight() const { return m_Height; }
    std::size_t GetSampleCount() const { return m_Samples.size(); }
    bool IsEmpty() const { return m_Samples.empty(); }

    // ---- Direct sample access ----

    float32 GetSample(uint32 x, uint32 z) const;
    void SetSample(uint32 x, uint32 z, float32 value);

    // Raw pointer for bulk upload (row-major, Z-major order).
    const float32* GetRawSamples() const { return m_Samples.data(); }
    float32* GetMutableSamples() { return m_Samples.data(); }

    // ---- Interpolated sampling ----

    // Sample at normalized UV in [0,1]. Bilinear interpolation.
    float32 SampleBilinear(float32 u, float32 v) const;

    // ---- Normal computation ----

    // Compute surface normal at integer grid position using central differences.
    // `spacingX` and `spacingZ` are world-space distances between adjacent samples.
    Mathematics::Vector3 ComputeNormal(int32 x, int32 z,
                                       float32 spacingX, float32 spacingZ) const;

    // Compute normal with cross-tile neighbor data at edges.
    // Null neighbor pointers fall back to edge clamping (same as ComputeNormal).
    Mathematics::Vector3 ComputeNormalWithNeighbor(
        int32 x, int32 z, float32 spacingX, float32 spacingZ,
        const HeightfieldData* neighborLeft,
        const HeightfieldData* neighborRight,
        const HeightfieldData* neighborUp,
        const HeightfieldData* neighborDown) const;

    // ---- Min/max queries (for quadtree construction) ----

    // Get min/max height in a rectangular region [x0, x0+sizeX) x [z0, z0+sizeZ).
    void GetMinMax(int32 x0, int32 z0, int32 sizeX, int32 sizeZ,
                   float32& outMin, float32& outMax) const;

    // ---- I/O ----

    // Load from a 16-bit grayscale PNG. Heights are normalized to [0,1].
    bool LoadFromPNG16(const std::filesystem::path& path);

    // Load from a raw binary file (float32 per sample, row-major).
    bool LoadFromRawFloat(const std::filesystem::path& path, uint32 width, uint32 height);

    // Load from a raw binary file (uint16 per sample, little-endian, row-major).
    // Heights are normalized [0, 65535] -> [0, 1].
    bool LoadFromRawUInt16(const std::filesystem::path& path, uint32 width, uint32 height);

    // ---- Modification ----

    // Fill the entire heightfield with a constant value.
    void Fill(float32 value);

    // Fill with fractal noise. Heights output in [0, amplitude].
    void FillWithNoise(float32 frequency, float32 amplitude, uint32 octaves = 4, uint32 seed = 0);

    // Fill only the samples in [minX, maxX] x [minZ, maxZ] (inclusive, clamped)
    // with the same per-sample noise as FillWithNoise. The noise value depends
    // only on the sample coordinate and parameters, so a region fill is an
    // exact subset of a full fill — used for region-scoped modifier re-bakes.
    void FillRegionWithNoise(float32 frequency, float32 amplitude,
                             int32 minX, int32 minZ, int32 maxX, int32 maxZ,
                             uint32 octaves = 4, uint32 seed = 0);

    // World-space noise: samples are computed using world positions so adjacent
    // tiles with matching world origins produce seamless boundaries.
    void FillWithNoiseWorldSpace(float32 frequency, float32 amplitude,
                                  float32 worldOriginX, float32 worldOriginZ,
                                  float32 worldSizeX, float32 worldSizeZ,
                                  uint32 octaves = 4, uint32 seed = 0);

    // Fill only the samples in [minX, maxX] x [minZ, maxZ] (inclusive, clamped)
    // with the same per-sample world-space noise as FillWithNoiseWorldSpace.
    // The noise value depends only on the sample's world position, so a region
    // fill is an exact subset of a full fill — used by tiled region re-bakes.
    void FillRegionWithNoiseWorldSpace(float32 frequency, float32 amplitude,
                                        float32 worldOriginX, float32 worldOriginZ,
                                        float32 worldSizeX, float32 worldSizeZ,
                                        int32 minX, int32 minZ, int32 maxX, int32 maxZ,
                                        uint32 octaves = 4, uint32 seed = 0);

    // Resize and clear to a default value.
    void Resize(uint32 width, uint32 height, float32 defaultValue = 0.0f);

private:
    uint32 m_Width = 0;
    uint32 m_Height = 0;
    std::vector<float32> m_Samples;
};

} // namespace GameEngine::Terrain
