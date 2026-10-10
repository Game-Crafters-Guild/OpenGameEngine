#include "PageStreaming/HeightPageOverlay.h"

#include <algorithm>

namespace GameEngine::PageStreaming
{

namespace
{

uint32 ClampIndex(int64 index, uint32 count)
{
    return static_cast<uint32>(std::clamp<int64>(index, 0, static_cast<int64>(count) - 1));
}

// The samples of the coarser level whose tent reads a sample of `finer` (the coarser sample j
// reads finer samples 2j - 1 to 2j + 1), within that level's `samplesX` x `samplesZ`.
PageSampleRect CoarserRect(const PageSampleRect& finer, uint32 samplesX, uint32 samplesZ)
{
    PageSampleRect out;
    out.MinX = std::min(finer.MinX / 2u, samplesX - 1u);
    out.MinZ = std::min(finer.MinZ / 2u, samplesZ - 1u);
    out.MaxX = std::min((finer.MaxX + 1u) / 2u, samplesX - 1u);
    out.MaxZ = std::min((finer.MaxZ + 1u) / 2u, samplesZ - 1u);
    return out;
}

} // namespace

bool PageReadsLevel0Rect(const PageAddress& address, const PageSampleRect& level0Rect)
{
    // The page stores its level's samples 128p - 1 to 128p + 128; level-L sample s sits at level-0
    // sample s * 2^L, and a coarser sample is filtered from the level-0 samples within 2^L - 1 of it.
    const int64 reach = (int64{1} << address.Level) - 1;
    const int64 firstX = ((static_cast<int64>(address.X) * kPageOwnedSamples - kPageApronSamples) << address.Level) - reach;
    const int64 firstZ = ((static_cast<int64>(address.Z) * kPageOwnedSamples - kPageApronSamples) << address.Level) - reach;
    const int64 lastX = ((static_cast<int64>(address.X) * kPageOwnedSamples + kPageOwnedSamples) << address.Level) + reach;
    const int64 lastZ = ((static_cast<int64>(address.Z) * kPageOwnedSamples + kPageOwnedSamples) << address.Level) + reach;
    return lastX >= level0Rect.MinX && firstX <= level0Rect.MaxX && lastZ >= level0Rect.MinZ &&
           firstZ <= level0Rect.MaxZ;
}

HeightPageOverlay::HeightPageOverlay(std::span<const PageStoreLevel> levels, const PageSampleRect& level0Rect,
                                     std::vector<float32> level0Delta)
{
    if (levels.empty())
        return;
    m_Levels.resize(levels.size());
    PageSampleRect rect = level0Rect;
    for (std::size_t level = 0; level < levels.size(); ++level)
    {
        if (level > 0)
            rect = CoarserRect(rect, levels[level].SamplesX, levels[level].SamplesZ);
        m_Levels[level].Shape = levels[level];
        m_Levels[level].Rect = rect;
        m_Levels[level].Delta.assign(static_cast<std::size_t>(rect.Width()) * rect.Height(), 0.0f);
    }
    m_Levels.front().Delta = std::move(level0Delta);
    m_Levels.front().Delta.resize(static_cast<std::size_t>(level0Rect.Width()) * level0Rect.Height(), 0.0f);
    for (uint32 level = 1; level < m_Levels.size(); ++level)
        FilterLevel(level, m_Levels[level - 1].Rect);
}

uint64 HeightPageOverlay::Bytes(std::span<const PageStoreLevel> levels, const PageSampleRect& level0Rect)
{
    uint64 samples = 0;
    PageSampleRect rect = level0Rect;
    for (std::size_t level = 0; level < levels.size(); ++level)
    {
        if (level > 0)
            rect = CoarserRect(rect, levels[level].SamplesX, levels[level].SamplesZ);
        samples += static_cast<uint64>(rect.Width()) * rect.Height();
    }
    return samples * sizeof(float32);
}

void HeightPageOverlay::Rebake(const PageSampleRect& changed, std::span<const float32> changedDelta)
{
    if (m_Levels.empty())
        return;
    Level& base = m_Levels.front();
    for (uint32 z = changed.MinZ; z <= changed.MaxZ; ++z)
        for (uint32 x = changed.MinX; x <= changed.MaxX; ++x)
            base.Delta[static_cast<std::size_t>(z - base.Rect.MinZ) * base.Rect.Width() + (x - base.Rect.MinX)] =
                changedDelta[static_cast<std::size_t>(z - changed.MinZ) * changed.Width() + (x - changed.MinX)];
    PageSampleRect reach = changed;
    for (uint32 level = 1; level < m_Levels.size(); ++level)
    {
        FilterLevel(level, reach);
        reach = CoarserRect(reach, m_Levels[level].Shape.SamplesX, m_Levels[level].Shape.SamplesZ);
    }
}

float32 HeightPageOverlay::Delta(uint32 level, uint32 x, uint32 z) const
{
    if (level >= m_Levels.size())
        return 0.0f;
    const Level& l = m_Levels[level];
    if (!l.Rect.Contains(x, z))
        return 0.0f;
    return l.Delta[static_cast<std::size_t>(z - l.Rect.MinZ) * l.Rect.Width() + (x - l.Rect.MinX)];
}

void HeightPageOverlay::FilterLevel(uint32 level, const PageSampleRect& finerChanged)
{
    const Level& finer = m_Levels[level - 1];
    Level& coarser = m_Levels[level];
    const PageSampleRect rect = CoarserRect(finerChanged, coarser.Shape.SamplesX, coarser.Shape.SamplesZ);
    const uint32 finerX = finer.Shape.SamplesX;
    const uint32 finerZ = finer.Shape.SamplesZ;
    for (uint32 z = std::max(rect.MinZ, coarser.Rect.MinZ); z <= std::min(rect.MaxZ, coarser.Rect.MaxZ); ++z)
    {
        for (uint32 x = std::max(rect.MinX, coarser.Rect.MinX); x <= std::min(rect.MaxX, coarser.Rect.MaxX); ++x)
        {
            float32 sum = 0.0f;
            for (int32 dz = -1; dz <= 1; ++dz)
            {
                const uint32 fz = ClampIndex(2 * static_cast<int64>(z) + dz, finerZ);
                const float32 wz = dz == 0 ? 0.5f : 0.25f;
                const float32 row = 0.25f * Delta(level - 1, ClampIndex(2 * static_cast<int64>(x) - 1, finerX), fz) +
                                    0.5f * Delta(level - 1, ClampIndex(2 * static_cast<int64>(x), finerX), fz) +
                                    0.25f * Delta(level - 1, ClampIndex(2 * static_cast<int64>(x) + 1, finerX), fz);
                sum += wz * row;
            }
            coarser.Delta[static_cast<std::size_t>(z - coarser.Rect.MinZ) * coarser.Rect.Width() +
                          (x - coarser.Rect.MinX)] = sum;
        }
    }
}

bool HeightPageOverlay::ApplyToPage(const PageAddress& address, std::span<float32> samples) const
{
    if (m_Levels.empty() || address.Level >= m_Levels.size() || samples.size() != kPageSampleCount ||
        !PageReadsLevel0Rect(address, Level0Rect()))
        return false;
    const Level& l = m_Levels[address.Level];
    const int64 firstX = static_cast<int64>(address.X) * kPageOwnedSamples - kPageApronSamples;
    const int64 firstZ = static_cast<int64>(address.Z) * kPageOwnedSamples - kPageApronSamples;
    for (uint32 row = 0; row < kPageStrideSamples; ++row)
    {
        const uint32 z = ClampIndex(firstZ + row, l.Shape.SamplesZ);
        if (z < l.Rect.MinZ || z > l.Rect.MaxZ)
            continue;
        float32* out = samples.data() + static_cast<std::size_t>(row) * kPageStrideSamples;
        for (uint32 i = 0; i < kPageStrideSamples; ++i)
            out[i] += Delta(address.Level, ClampIndex(firstX + i, l.Shape.SamplesX), z);
    }
    return true;
}

} // namespace GameEngine::PageStreaming
