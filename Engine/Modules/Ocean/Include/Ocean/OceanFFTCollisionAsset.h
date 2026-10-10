#pragma once

#include "Ocean/OceanFFT.h"
#include "Ocean/OceanQuery.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine::Ocean
{

struct OceanFFTDisplacementSample
{
    float32 X = 0.0f;
    float32 Y = 0.0f;
    float32 Z = 0.0f;
};

// Metadata carried by a periodic CPU collision bake. Samples are stored as
// FrameCount square, periodic grids of Resolution x Resolution displacements.
// LoopLength is the world-space tile period; LoopPeriod is the time period.
struct OceanFFTCollisionAssetDesc
{
    uint32 FormatVersion = 1u;
    uint32 Resolution = 0u;
    uint32 FrameCount = 0u;
    float32 TimeResolution = 0.0f;
    float32 LoopPeriod = 0.0f;
    float32 LoopLength = 0.0f;
    float32 SmallestWavelength = 0.0f;
    float32 SpatialResolution = 0.0f;
    float32 SeaLevel = 0.0f;
    uint64 SpectrumHash = 0u;
};

class OceanFFTCollisionAsset
{
public:
    static constexpr uint32 kFileVersion = 1u;

    bool Reset(const OceanFFTCollisionAssetDesc& desc,
               std::vector<OceanFFTDisplacementSample> samples,
               std::string* error = nullptr);

    bool IsValid() const;
    bool IsCompatible(uint64 expectedSpectrumHash) const;
    const OceanFFTCollisionAssetDesc& GetDesc() const { return m_Desc; }
    const std::vector<OceanFFTDisplacementSample>& GetSamples() const { return m_Samples; }

    // Samples the displaced surface at a requested world XZ. Horizontal
    // displacement is inverted iteratively so the returned height/normal refer to
    // the visible surface above the query point, not the undisplaced FFT texel.
    bool SampleSurface(float32 worldX, float32 worldZ, float32 time,
                       OceanSurfaceSample& outSample) const;

    bool SaveBinary(const std::filesystem::path& path, std::string* error = nullptr) const;
    bool LoadBinary(const std::filesystem::path& path, std::string* error = nullptr);

    // Stable settings hash used by extraction and the baker. It deliberately
    // hashes the complete GPU spectrum packet, including per-octave controls.
    static uint64 ComputeSpectrumHash(const OceanFFTParamsGPU& params);

private:
    OceanFFTDisplacementSample SampleDisplacement(float32 worldX, float32 worldZ,
                                                  float32 time) const;
    float32 SampleHeightInverted(float32 worldX, float32 worldZ, float32 time,
                                 OceanFFTDisplacementSample* outDisplacement = nullptr) const;

    OceanFFTCollisionAssetDesc m_Desc{};
    std::vector<OceanFFTDisplacementSample> m_Samples;
};

// Callback-based clean-room bake entry point. The callback supplies one periodic
// displacement texel for a frame/time/world coordinate, making the asset writer
// usable with GPU readback, deterministic reference data, or offline tools.
using OceanFFTCollisionBakeSampleFn = bool (*)(uint32 frame, float32 time,
                                               float32 worldX, float32 worldZ,
                                               OceanFFTDisplacementSample& out,
                                               void* userData);

bool BakeOceanFFTCollision(const OceanFFTCollisionAssetDesc& desc,
                           OceanFFTCollisionBakeSampleFn sample,
                           void* userData,
                           OceanFFTCollisionAsset& outAsset,
                           std::string* error = nullptr);

} // namespace GameEngine::Ocean
