#include "Ocean/OceanCaustics.h"

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace GameEngine::Ocean
{

using namespace ::GameEngine::Rendering;

namespace
{

// Hash a wrapped integer lattice cell to a feature point inside the cell. The
// lattice index is taken modulo the tile period so the pattern wraps seamlessly
// (the texture is sampled with Repeat at arbitrary world scale).
struct CellHash
{
    float OffsetX;
    float OffsetY;
};

float Fract(float v)
{
    return v - std::floor(v);
}

CellHash HashCell(int cx, int cy, int period)
{
    // Wrap the cell so the feature-point field is tileable.
    int wx = ((cx % period) + period) % period;
    int wy = ((cy % period) + period) % period;
    const float fx = static_cast<float>(wx);
    const float fy = static_cast<float>(wy);
    float a = std::sin(fx * 127.1f + fy * 311.7f) * 43758.5453f;
    float b = std::sin(fx * 269.5f + fy * 183.3f) * 43758.5453f;
    return {Fract(a), Fract(b)};
}

// Voronoi "edge" response at a point in lattice space, evaluated tileably over a
// grid of `period` cells. Caustics are the bright, thin webs where refracted
// light focuses — the ridge midway between two nearest Worley cells. We return a
// sharp edge term: ~1 on the ridge, falling to 0 inside the cells. F2 - F1 is the
// classic Worley edge distance; we shape it into a thin bright filament.
float VoronoiEdge(float px, float py, int period)
{
    const int ix = static_cast<int>(std::floor(px));
    const int iy = static_cast<int>(std::floor(py));
    const float fx = px - static_cast<float>(ix);
    const float fy = py - static_cast<float>(iy);

    float f1 = 1.0e9f;
    float f2 = 1.0e9f;
    for (int oy = -1; oy <= 1; ++oy)
    {
        for (int ox = -1; ox <= 1; ++ox)
        {
            const CellHash h = HashCell(ix + ox, iy + oy, period);
            const float rx = static_cast<float>(ox) + h.OffsetX - fx;
            const float ry = static_cast<float>(oy) + h.OffsetY - fy;
            const float d = rx * rx + ry * ry;
            if (d < f1)
            {
                f2 = f1;
                f1 = d;
            }
            else if (d < f2)
            {
                f2 = d;
            }
        }
    }
    // Edge band: small where F1≈F2 (on the ridge between two cells). Convert into
    // a thin bright filament with a smooth falloff.
    const float edge = std::sqrt(f2) - std::sqrt(f1);
    float ridge = 1.0f - edge / 0.35f; // 1 on the ridge, ramps to 0 away from it
    if (ridge < 0.0f)
        ridge = 0.0f;
    return ridge * ridge; // sharpen the filament
}

// Two octaves of the Voronoi edge at different scales/phases, the way real
// caustics overlap multiple focusing scales. Both octaves wrap (tileable).
float CausticIntensity(float u, float v)
{
    const int period1 = 6;
    const int period2 = 11;
    float a = VoronoiEdge(u * static_cast<float>(period1), v * static_cast<float>(period1), period1);
    float b = VoronoiEdge(u * static_cast<float>(period2) + 3.0f,
                          v * static_cast<float>(period2) + 7.0f, period2);
    float c = a * 0.7f + b * 0.55f;
    if (c > 1.0f)
        c = 1.0f;
    return c;
}

// Smooth tileable scalar field whose gradient feeds the distortion normal. A pair
// of wrapped sinusoids gives a gentle, seam-free undulation independent of the
// caustic web so the two effects don't visibly correlate.
float DistortionField(float u, float v)
{
    const float tau = 6.2831853f;
    return 0.5f + 0.25f * std::sin(u * tau * 2.0f) * std::cos(v * tau * 3.0f) +
           0.25f * std::sin((u + v) * tau * 2.0f);
}

// Box-downsample one RGBA8 mip level into the next. Tileable content means a
// straight 2x2 average is correct (no edge clamp needed for wrap sampling).
void DownsampleMip(const std::vector<uint8_t>& src, uint32_t srcW, uint32_t /*srcH*/,
                   std::vector<uint8_t>& dst, uint32_t dstW, uint32_t dstH)
{
    dst.resize(static_cast<size_t>(dstW) * dstH * 4u);
    for (uint32_t y = 0; y < dstH; ++y)
    {
        for (uint32_t x = 0; x < dstW; ++x)
        {
            const uint32_t sx = x * 2u;
            const uint32_t sy = y * 2u;
            for (uint32_t ch = 0; ch < 4u; ++ch)
            {
                const uint32_t s00 = src[(static_cast<size_t>(sy) * srcW + sx) * 4u + ch];
                const uint32_t s10 = src[(static_cast<size_t>(sy) * srcW + (sx + 1u)) * 4u + ch];
                const uint32_t s01 = src[(static_cast<size_t>(sy + 1u) * srcW + sx) * 4u + ch];
                const uint32_t s11 = src[(static_cast<size_t>(sy + 1u) * srcW + (sx + 1u)) * 4u + ch];
                dst[(static_cast<size_t>(y) * dstW + x) * 4u + ch] =
                    static_cast<uint8_t>((s00 + s10 + s01 + s11 + 2u) / 4u);
            }
        }
    }
}

uint32_t MipCount(uint32_t resolution)
{
    uint32_t mips = 1;
    while (resolution > 1u)
    {
        resolution >>= 1u;
        ++mips;
    }
    return mips;
}

} // namespace

bool OceanCaustics::Initialize(::GameEngine::Rendering::IDevice* device)
{
    if (m_Ready)
        return true;
    if (!device)
        return false;

    const uint32_t res = kResolution;
    const uint32_t mipLevels = MipCount(res);

    // --- Generate the base mip on the CPU ---
    // R,G = distortion normal (xy, [0,1]-encoded). B = caustic intensity. A = 255.
    std::vector<uint8_t> base(static_cast<size_t>(res) * res * 4u);
    const float texel = 1.0f / static_cast<float>(res);
    for (uint32_t y = 0; y < res; ++y)
    {
        for (uint32_t x = 0; x < res; ++x)
        {
            const float u = (static_cast<float>(x) + 0.5f) * texel;
            const float v = (static_cast<float>(y) + 0.5f) * texel;

            const float caustic = CausticIntensity(u, v);

            // Central-difference gradient of the smooth distortion field, sampled
            // with wrap so the encoded normal tiles. Scaled into a gentle slope.
            const float du =
                DistortionField(u + texel, v) - DistortionField(u - texel, v);
            const float dv =
                DistortionField(u, v + texel) - DistortionField(u, v - texel);
            const float nx = -du * 4.0f;
            const float ny = -dv * 4.0f;
            const float ex = 0.5f + 0.5f * std::fmax(-1.0f, std::fmin(1.0f, nx));
            const float ey = 0.5f + 0.5f * std::fmax(-1.0f, std::fmin(1.0f, ny));

            const size_t idx = (static_cast<size_t>(y) * res + x) * 4u;
            base[idx + 0] = static_cast<uint8_t>(ex * 255.0f + 0.5f);
            base[idx + 1] = static_cast<uint8_t>(ey * 255.0f + 0.5f);
            base[idx + 2] = static_cast<uint8_t>(caustic * 255.0f + 0.5f);
            base[idx + 3] = 255u;
        }
    }

    // --- Build the full mip chain on the CPU (no GPU GenerateMips API) ---
    std::vector<std::vector<uint8_t>> mips;
    mips.reserve(mipLevels);
    mips.push_back(std::move(base));
    uint32_t mw = res;
    uint32_t mh = res;
    for (uint32_t m = 1; m < mipLevels; ++m)
    {
        const uint32_t nw = std::max(1u, mw >> 1u);
        const uint32_t nh = std::max(1u, mh >> 1u);
        std::vector<uint8_t> next;
        DownsampleMip(mips[m - 1], mw, mh, next, nw, nh);
        mips.push_back(std::move(next));
        mw = nw;
        mh = nh;
    }

    // --- Create the GPU texture (RGBA8, full mip chain, sampleable) ---
    TextureDesc td{};
    td.width = res;
    td.height = res;
    td.depth = 1;
    td.mipLevels = mipLevels;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    td.format = static_cast<uint32_t>(TextureFormat::RGBA8_UNORM);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst);
    td.initialState = ResourceState::Undefined;
    td.debugName = "Ocean_Caustics";
    m_Texture = device->CreateTexture(td);
    if (!m_Texture.IsValid())
    {
        Logger::Log::Warning("OceanCaustics: texture creation failed; caustics disabled");
        return false;
    }

    // --- Upload each mip subresource via transient staging buffers ---
    struct MipStaging
    {
        BufferHandle Buffer;
        uint32_t Width;
        uint32_t Height;
        size_t RowPitch;
    };
    std::vector<MipStaging> staging(mipLevels);
    uint32_t lw = res;
    uint32_t lh = res;
    for (uint32_t m = 0; m < mipLevels; ++m)
    {
        const size_t rowPitch = static_cast<size_t>(lw) * 4u;
        const size_t size = rowPitch * lh;
        staging[m].Buffer = device->CreateUploadBuffer(size, "Ocean_Caustics_Staging");
        if (staging[m].Buffer.IsValid())
            device->UpdateBuffer(staging[m].Buffer, 0, size, mips[m].data());
        staging[m].Width = lw;
        staging[m].Height = lh;
        staging[m].RowPitch = rowPitch;
        lw = std::max(1u, lw >> 1u);
        lh = std::max(1u, lh >> 1u);
    }

    auto cl = device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    // Transition the whole mip chain (levelCount = mipLevels), not just mip 0.
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(m_Texture, ResourceState::Undefined,
                                                      ResourceState::CopyDest, 0u, mipLevels));
    for (uint32_t m = 0; m < mipLevels; ++m)
    {
        if (!staging[m].Buffer.IsValid())
            continue;
        cl->CopyBufferToTextureSubresource(staging[m].Buffer, m_Texture, m, 0, staging[m].Width,
                                           staging[m].Height, 0, staging[m].RowPitch, 1, 0);
    }
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(m_Texture, ResourceState::CopyDest,
                                                      ResourceState::ShaderResource, 0u, mipLevels));
    cl->End();
    CommandList* raw = cl.get();
    device->ExecuteCommandLists({raw});

    for (uint32_t m = 0; m < mipLevels; ++m)
        if (staging[m].Buffer.IsValid())
            device->DestroyBuffer(staging[m].Buffer);

    // Linear + linear-mip + repeat: the caustic web tiles seamlessly and the
    // mip chain gives the focal-depth blur the surface selects per fragment.
    m_Sampler = device->CreateSampler(SamplerDesc::MaterialLinearRepeat("Ocean_Caustics_Sampler"));
    if (!m_Sampler.IsValid())
    {
        Logger::Log::Warning("OceanCaustics: sampler creation failed; caustics disabled");
        device->DestroyTexture(m_Texture);
        m_Texture = {};
        return false;
    }

    m_Ready = true;
    Logger::Log::Info("OceanCaustics: procedural caustics texture generated");
    return true;
}

void OceanCaustics::Destroy(::GameEngine::Rendering::IDevice* device)
{
    if (!device)
        return;
    if (m_Texture.IsValid())
        device->DestroyTexture(m_Texture);
    if (m_Sampler.IsValid())
        device->DestroySampler(m_Sampler);
    m_Texture = {};
    m_Sampler = {};
    m_Ready = false;
}

} // namespace GameEngine::Ocean
