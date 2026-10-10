#pragma once

// A deterministic multi-octave relief height texture for the CBT real-device tests, and an RAII
// owner that binds it to every frame-params ring slot.
//
// Shared because the height regime is load-bearing for more than one suite: planar corners carry
// sampled height in .y (Kernel_VertexEval) scaled by TerrainSize[2] = heightScale, so at
// heightScale 0 every corner of a facet shares one y. Several geometric facts about LEB siblings
// are EXACT in that regime and only approximate with relief — a test that runs flat-only can assert
// the exact form and look green while saying nothing about production, which ships heightScale 60.

#include <cmath>
#include <cstdint>
#include <vector>

#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTLayout.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"

namespace GameEngine::CBTTerrain::Test
{

// Integer hash -> [0,1]. Deterministic across runs and platforms: the arms compare against
// measured bounds, so the relief must not move between runs.
inline float ReliefValueNoise(uint32_t x, uint32_t y)
{
    uint32_t h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xFFFFFFu) / static_cast<float>(0xFFFFFF);
}

inline float ReliefSmoothNoiseAt(float u, float v, uint32_t period)
{
    const float su = u * static_cast<float>(period), sv = v * static_cast<float>(period);
    const uint32_t x0 = static_cast<uint32_t>(su), y0 = static_cast<uint32_t>(sv);
    const float fx = su - static_cast<float>(x0), fy = sv - static_cast<float>(y0);
    const float wx = fx * fx * (3.0f - 2.0f * fx), wy = fy * fy * (3.0f - 2.0f * fy);
    const float n00 = ReliefValueNoise(x0, y0), n10 = ReliefValueNoise(x0 + 1u, y0);
    const float n01 = ReliefValueNoise(x0, y0 + 1u), n11 = ReliefValueNoise(x0 + 1u, y0 + 1u);
    return (n00 * (1.0f - wx) + n10 * wx) * (1.0f - wy) + (n01 * (1.0f - wx) + n11 * wx) * wy;
}

// Six octaves, so the height is non-linear along edges at every subdivision depth rather than only
// the coarse ones. A single smooth octave leaves the deep facets locally planar, which would
// understate exactly the residual these arms exist to measure.
inline Rendering::TextureHandle MakeReliefHeightTexture(Rendering::IDevice& device, uint32_t dim)
{
    using namespace Rendering;
    TextureDesc td{};
    td.width = dim;
    td.height = dim;
    td.format = static_cast<uint32_t>(TextureFormat::R32_FLOAT);
    td.usage = static_cast<uint32_t>(TextureUsage::ShaderResource) |
               static_cast<uint32_t>(TextureUsage::TransferDst);
    td.persistent = true;
    td.debugName = "CBT.Test.Relief";
    TextureHandle tex = device.CreateTexture(td);
    if (!tex.IsValid())
        return tex;

    std::vector<float> data(static_cast<size_t>(dim) * dim);
    for (uint32_t y = 0; y < dim; ++y)
        for (uint32_t x = 0; x < dim; ++x)
        {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(dim);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(dim);
            float amp = 0.5f, sum = 0.0f, norm = 0.0f;
            for (uint32_t octave = 0; octave < 6u; ++octave)
            {
                sum += amp * ReliefSmoothNoiseAt(u, v, 4u << octave);
                norm += amp;
                amp *= 0.5f;
            }
            data[static_cast<size_t>(y) * dim + x] = sum / norm;
        }

    const size_t bytes = data.size() * sizeof(float);
    const size_t rowPitch = static_cast<size_t>(dim) * sizeof(float);
    BufferHandle staging = device.CreateUploadBuffer(bytes, "CBT.Test.ReliefStaging");
    device.UpdateBuffer(staging, 0, bytes, data.data());
    auto cl = device.CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    cl->CopyBufferToTextureSubresource(staging, tex, 0, 0, dim, dim, 0, rowPitch, 1, 0, 0, 0,
                                       ResourceState::Undefined);
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(tex, ResourceState::CopyDest,
                                                      ResourceState::ShaderResource));
    cl->End();
    std::vector<CommandList*> lists{cl.get()};
    device.ExecuteCommandLists(lists);
    device.WaitForIdle();
    device.DestroyBuffer(staging);
    return tex;
}

// Owns the relief texture for one arm and binds it to every ring slot, so any frame index samples
// the real relief rather than frame 0 only. A heightScale of 0 binds nothing: CBT_SampleHeight
// returns texel * terrainSize.z + terrainSize.w, so the sampled value cannot move a corner's y.
class ReliefHeightSource
{
  public:
    ReliefHeightSource() = default;
    ReliefHeightSource(Rendering::IDevice& device, CBTInstance& instance, float heightScale,
                       uint32_t dim)
    {
        if (heightScale == 0.0f)
            return;
        m_Texture = MakeReliefHeightTexture(device, dim);
        if (!m_Texture.IsValid())
            return;
        m_Device = &device;
        for (uint32_t slot = 0; slot < kCBTFrameParamsRing; ++slot)
            instance.SetHeightSource(slot, m_Texture);
    }
    ReliefHeightSource(const ReliefHeightSource&) = delete;
    ReliefHeightSource& operator=(const ReliefHeightSource&) = delete;
    ReliefHeightSource(ReliefHeightSource&& o) noexcept
        : m_Device(o.m_Device), m_Texture(o.m_Texture)
    {
        o.m_Device = nullptr;
    }
    ReliefHeightSource& operator=(ReliefHeightSource&&) = delete;
    ~ReliefHeightSource()
    {
        if (m_Device && m_Texture.IsValid())
            m_Device->DestroyTexture(m_Texture);
    }

    bool IsArmed() const { return m_Device != nullptr; }

  private:
    Rendering::IDevice* m_Device = nullptr;
    Rendering::TextureHandle m_Texture;
};

} // namespace GameEngine::CBTTerrain::Test
