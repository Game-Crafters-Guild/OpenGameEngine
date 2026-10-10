#include "Ocean/OceanFFTCollisionAsset.h"

#include "AssetCore/SharedFileRead.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace GameEngine::Ocean
{
namespace
{
constexpr char kMagic[8] = {'G', 'E', 'O', 'F', 'F', 'T', 'C', 'L'};
constexpr uint32 kMaxResolution = 4096u;
constexpr uint32 kMaxFrames = 4096u;

void SetFFTCollisionError(std::string* error, const char* message)
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

float32 PositiveModulo(float32 value, float32 period)
{
    float32 result = std::fmod(value, period);
    return result < 0.0f ? result + period : result;
}

OceanFFTDisplacementSample Lerp(const OceanFFTDisplacementSample& a,
                                const OceanFFTDisplacementSample& b, float32 t)
{
    return {a.X + (b.X - a.X) * t,
            a.Y + (b.Y - a.Y) * t,
            a.Z + (b.Z - a.Z) * t};
}

uint64 Fnv1a64(const void* data, size_t bytes, uint64 seed = 1469598103934665603ull)
{
    const auto* values = static_cast<const uint8*>(data);
    uint64 hash = seed;
    for (size_t i = 0; i < bytes; ++i)
    {
        hash ^= static_cast<uint64>(values[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}
} // namespace

bool OceanFFTCollisionAsset::Reset(const OceanFFTCollisionAssetDesc& desc,
                                   std::vector<OceanFFTDisplacementSample> samples,
                                   std::string* error)
{
    if (desc.FormatVersion != kFileVersion)
    {
        SetFFTCollisionError(error, "unsupported ocean FFT collision format version");
        return false;
    }
    if (desc.Resolution < 2u || desc.Resolution > kMaxResolution ||
        desc.FrameCount < 2u || desc.FrameCount > kMaxFrames)
    {
        SetFFTCollisionError(error, "invalid ocean FFT collision resolution or frame count");
        return false;
    }
    if (!std::isfinite(desc.TimeResolution) || desc.TimeResolution <= 0.0f ||
        !std::isfinite(desc.LoopPeriod) || desc.LoopPeriod <= 0.0f ||
        !std::isfinite(desc.LoopLength) || desc.LoopLength <= 0.0f ||
        !std::isfinite(desc.SmallestWavelength) || desc.SmallestWavelength <= 0.0f ||
        !std::isfinite(desc.SpatialResolution) || desc.SpatialResolution <= 0.0f ||
        !std::isfinite(desc.SeaLevel))
    {
        SetFFTCollisionError(error, "invalid ocean FFT collision metadata");
        return false;
    }

    const uint64 grid = static_cast<uint64>(desc.Resolution) * desc.Resolution;
    const uint64 expected = grid * desc.FrameCount;
    if (expected > static_cast<uint64>(std::numeric_limits<size_t>::max()) ||
        samples.size() != static_cast<size_t>(expected))
    {
        SetFFTCollisionError(error, "ocean FFT collision sample count does not match metadata");
        return false;
    }
    for (const OceanFFTDisplacementSample& value : samples)
    {
        if (!std::isfinite(value.X) || !std::isfinite(value.Y) || !std::isfinite(value.Z))
        {
            SetFFTCollisionError(error, "ocean FFT collision data contains non-finite samples");
            return false;
        }
    }

    m_Desc = desc;
    m_Samples = std::move(samples);
    return true;
}

bool OceanFFTCollisionAsset::IsValid() const
{
    const uint64 expected = static_cast<uint64>(m_Desc.Resolution) * m_Desc.Resolution *
                            m_Desc.FrameCount;
    return m_Desc.FormatVersion == kFileVersion && m_Desc.Resolution >= 2u &&
           m_Desc.FrameCount >= 2u && m_Desc.LoopLength > 0.0f &&
           m_Desc.LoopPeriod > 0.0f && m_Samples.size() == expected;
}

bool OceanFFTCollisionAsset::IsCompatible(uint64 expectedSpectrumHash) const
{
    return IsValid() && expectedSpectrumHash != 0u && m_Desc.SpectrumHash == expectedSpectrumHash;
}

OceanFFTDisplacementSample OceanFFTCollisionAsset::SampleDisplacement(
    float32 worldX, float32 worldZ, float32 time) const
{
    if (!IsValid())
        return {};

    const uint32 resolution = m_Desc.Resolution;
    const size_t frameStride = static_cast<size_t>(resolution) * resolution;
    const float32 u = PositiveModulo(worldX, m_Desc.LoopLength) / m_Desc.LoopLength * resolution;
    const float32 v = PositiveModulo(worldZ, m_Desc.LoopLength) / m_Desc.LoopLength * resolution;
    const uint32 x0 = static_cast<uint32>(std::floor(u)) % resolution;
    const uint32 z0 = static_cast<uint32>(std::floor(v)) % resolution;
    const uint32 x1 = (x0 + 1u) % resolution;
    const uint32 z1 = (z0 + 1u) % resolution;
    const float32 tx = u - std::floor(u);
    const float32 tz = v - std::floor(v);

    const float32 loopTime = PositiveModulo(time, m_Desc.LoopPeriod);
    const float32 frameCoord = loopTime / m_Desc.LoopPeriod * m_Desc.FrameCount;
    const uint32 f0 = static_cast<uint32>(std::floor(frameCoord)) % m_Desc.FrameCount;
    const uint32 f1 = (f0 + 1u) % m_Desc.FrameCount;
    const float32 ft = frameCoord - std::floor(frameCoord);

    const auto sampleFrame = [&](uint32 frame) {
        const size_t base = static_cast<size_t>(frame) * frameStride;
        const auto fetch = [&](uint32 x, uint32 z) -> const OceanFFTDisplacementSample& {
            return m_Samples[base + static_cast<size_t>(z) * resolution + x];
        };
        const OceanFFTDisplacementSample a = Lerp(fetch(x0, z0), fetch(x1, z0), tx);
        const OceanFFTDisplacementSample b = Lerp(fetch(x0, z1), fetch(x1, z1), tx);
        return Lerp(a, b, tz);
    };

    return Lerp(sampleFrame(f0), sampleFrame(f1), ft);
}

float32 OceanFFTCollisionAsset::SampleHeightInverted(
    float32 worldX, float32 worldZ, float32 time,
    OceanFFTDisplacementSample* outDisplacement) const
{
    float32 baseX = worldX;
    float32 baseZ = worldZ;
    OceanFFTDisplacementSample displacement{};
    for (uint32 iteration = 0u; iteration < 6u; ++iteration)
    {
        displacement = SampleDisplacement(baseX, baseZ, time);
        baseX = worldX - displacement.X;
        baseZ = worldZ - displacement.Z;
    }
    displacement = SampleDisplacement(baseX, baseZ, time);
    if (outDisplacement)
        *outDisplacement = displacement;
    return m_Desc.SeaLevel + displacement.Y;
}

bool OceanFFTCollisionAsset::SampleSurface(float32 worldX, float32 worldZ, float32 time,
                                           OceanSurfaceSample& outSample) const
{
    if (!IsValid() || !std::isfinite(worldX) || !std::isfinite(worldZ) || !std::isfinite(time))
        return false;

    OceanFFTDisplacementSample displacement{};
    const float32 height = SampleHeightInverted(worldX, worldZ, time, &displacement);
    const float32 e = std::max(m_Desc.SpatialResolution, 0.05f);
    const float32 left = SampleHeightInverted(worldX - e, worldZ, time);
    const float32 right = SampleHeightInverted(worldX + e, worldZ, time);
    const float32 down = SampleHeightInverted(worldX, worldZ - e, time);
    const float32 up = SampleHeightInverted(worldX, worldZ + e, time);
    float32 nx = left - right;
    float32 ny = 2.0f * e;
    float32 nz = down - up;
    const float32 nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (nlen > 1e-6f)
    {
        nx /= nlen;
        ny /= nlen;
        nz /= nlen;
    }

    OceanParamsGPU params{};
    params.SeaLevel = m_Desc.SeaLevel;
    outSample = MakeOceanSurfaceSample(params, worldX, worldZ, height, nx, ny, nz,
                                       OceanQuerySource::BakedFFTCPU);
    outSample.DisplacementWS[0] = displacement.X;
    outSample.DisplacementWS[1] = displacement.Y;
    outSample.DisplacementWS[2] = displacement.Z;
    outSample.PositionWS[0] = worldX;
    outSample.PositionWS[2] = worldZ;

    const float32 dt = std::max(m_Desc.TimeResolution * 0.5f, 1e-3f);
    const OceanFFTDisplacementSample previous = SampleDisplacement(
        worldX - displacement.X, worldZ - displacement.Z, time - dt);
    const OceanFFTDisplacementSample next = SampleDisplacement(
        worldX - displacement.X, worldZ - displacement.Z, time + dt);
    outSample.VelocityWS[0] = (next.X - previous.X) / (2.0f * dt);
    outSample.VelocityWS[1] = (next.Y - previous.Y) / (2.0f * dt);
    outSample.VelocityWS[2] = (next.Z - previous.Z) / (2.0f * dt);
    return true;
}

bool OceanFFTCollisionAsset::SaveBinary(const std::filesystem::path& path,
                                        std::string* error) const
{
    if (!IsValid())
    {
        SetFFTCollisionError(error, "cannot save invalid ocean FFT collision asset");
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        SetFFTCollisionError(error, "failed to open ocean FFT collision asset for writing");
        return false;
    }
    out.write(kMagic, sizeof(kMagic));
    if (!WriteValue(out, m_Desc.FormatVersion) || !WriteValue(out, m_Desc.Resolution) ||
        !WriteValue(out, m_Desc.FrameCount) || !WriteValue(out, m_Desc.TimeResolution) ||
        !WriteValue(out, m_Desc.LoopPeriod) || !WriteValue(out, m_Desc.LoopLength) ||
        !WriteValue(out, m_Desc.SmallestWavelength) ||
        !WriteValue(out, m_Desc.SpatialResolution) || !WriteValue(out, m_Desc.SeaLevel) ||
        !WriteValue(out, m_Desc.SpectrumHash))
    {
        SetFFTCollisionError(error, "failed to write ocean FFT collision header");
        return false;
    }
    out.write(reinterpret_cast<const char*>(m_Samples.data()),
              static_cast<std::streamsize>(m_Samples.size() * sizeof(m_Samples[0])));
    if (!out)
    {
        SetFFTCollisionError(error, "failed to write ocean FFT collision samples");
        return false;
    }
    return true;
}

bool OceanFFTCollisionAsset::LoadBinary(const std::filesystem::path& path, std::string* error)
{
    SharedFileReader in(path);
    if (!in.IsOpen())
    {
        SetFFTCollisionError(error, "failed to open ocean FFT collision asset");
        return false;
    }
    char magic[sizeof(kMagic)]{};
    if (in.Read(magic, sizeof(magic)) != static_cast<int64>(sizeof(magic)) ||
        std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
    {
        SetFFTCollisionError(error, "invalid ocean FFT collision magic");
        return false;
    }
    OceanFFTCollisionAssetDesc desc{};
    if (!ReadValue(in, desc.FormatVersion) || !ReadValue(in, desc.Resolution) ||
        !ReadValue(in, desc.FrameCount) || !ReadValue(in, desc.TimeResolution) ||
        !ReadValue(in, desc.LoopPeriod) || !ReadValue(in, desc.LoopLength) ||
        !ReadValue(in, desc.SmallestWavelength) || !ReadValue(in, desc.SpatialResolution) ||
        !ReadValue(in, desc.SeaLevel) || !ReadValue(in, desc.SpectrumHash))
    {
        SetFFTCollisionError(error, "truncated ocean FFT collision header");
        return false;
    }
    if (desc.Resolution < 2u || desc.Resolution > kMaxResolution ||
        desc.FrameCount < 2u || desc.FrameCount > kMaxFrames)
    {
        SetFFTCollisionError(error, "invalid ocean FFT collision dimensions");
        return false;
    }
    const uint64 count = static_cast<uint64>(desc.Resolution) * desc.Resolution * desc.FrameCount;
    if (count > static_cast<uint64>(std::numeric_limits<size_t>::max() / sizeof(OceanFFTDisplacementSample)))
    {
        SetFFTCollisionError(error, "ocean FFT collision data is too large");
        return false;
    }
    std::vector<OceanFFTDisplacementSample> samples(static_cast<size_t>(count));
    const uint64 sampleBytes = samples.size() * sizeof(samples[0]);
    if (in.Read(samples.data(), sampleBytes) != static_cast<int64>(sampleBytes))
    {
        SetFFTCollisionError(error, "truncated ocean FFT collision samples");
        return false;
    }
    return Reset(desc, std::move(samples), error);
}

uint64 OceanFFTCollisionAsset::ComputeSpectrumHash(const OceanFFTParamsGPU& params)
{
    OceanFFTParamsGPU stable = params;
    stable.Time = 0.0f;
    return Fnv1a64(&stable, sizeof(stable));
}

bool BakeOceanFFTCollision(const OceanFFTCollisionAssetDesc& desc,
                           OceanFFTCollisionBakeSampleFn sample,
                           void* userData,
                           OceanFFTCollisionAsset& outAsset,
                           std::string* error)
{
    if (!sample)
    {
        SetFFTCollisionError(error, "ocean FFT collision bake needs a sample callback");
        return false;
    }
    if (desc.Resolution < 2u || desc.FrameCount < 2u || desc.LoopLength <= 0.0f ||
        desc.LoopPeriod <= 0.0f)
    {
        SetFFTCollisionError(error, "invalid ocean FFT collision bake metadata");
        return false;
    }
    const uint64 count = static_cast<uint64>(desc.Resolution) * desc.Resolution * desc.FrameCount;
    if (count > static_cast<uint64>(std::numeric_limits<size_t>::max()))
    {
        SetFFTCollisionError(error, "ocean FFT collision bake is too large");
        return false;
    }
    std::vector<OceanFFTDisplacementSample> values(static_cast<size_t>(count));
    for (uint32 frame = 0u; frame < desc.FrameCount; ++frame)
    {
        const float32 time = static_cast<float32>(frame) / desc.FrameCount * desc.LoopPeriod;
        for (uint32 z = 0u; z < desc.Resolution; ++z)
        {
            for (uint32 x = 0u; x < desc.Resolution; ++x)
            {
                const float32 worldX = static_cast<float32>(x) / desc.Resolution * desc.LoopLength;
                const float32 worldZ = static_cast<float32>(z) / desc.Resolution * desc.LoopLength;
                const size_t index = (static_cast<size_t>(frame) * desc.Resolution + z) *
                                     desc.Resolution + x;
                if (!sample(frame, time, worldX, worldZ, values[index], userData))
                {
                    SetFFTCollisionError(error, "ocean FFT collision bake callback failed");
                    return false;
                }
            }
        }
    }
    return outAsset.Reset(desc, std::move(values), error);
}

} // namespace GameEngine::Ocean
