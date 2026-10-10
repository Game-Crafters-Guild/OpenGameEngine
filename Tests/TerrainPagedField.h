#pragma once

// A paged height field for the resolve's oracles: a page cache driven a frame at a time, its page
// table and the cache texture's texels, filled from an analytic field. Shared by the CPU oracles
// (TerrainPageResidencyTests) and the device parity test (TerrainPageResolveGpuTests).

#include "TerrainECS/PagedHeightSampler.h"

#include "PageStreaming/PageCache.h"
#include "PageStreaming/PageStoreFormat.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace GameEngine::TerrainECS::Test
{

// The default field: 600 x 400 level-0 samples (four levels: 5 x 4, 3 x 2, 2 x 1 and 1 x 1 pages).
inline constexpr uint32 kFieldX = 600;
inline constexpr uint32 kFieldZ = 400;

// The height at level-0 sample position (x0, z0), rough enough that every level differs.
inline float32 FieldHeight(float32 x0, float32 z0)
{
    return 10.0f * std::sin(0.05f * x0) + 7.0f * std::cos(0.083f * z0) + 3.0f * std::sin(0.37f * (x0 + z0));
}

// The stored samples of a page: level L's lattice at its owned samples and apron, edge-clamped.
inline std::vector<float32> PageSamples(const PageStreaming::PageStoreLevel& shape, const PageStreaming::PageAddress& page)
{
    std::vector<float32> samples(PageStreaming::kPageSampleCount);
    for (uint32 r = 0; r < PageStreaming::kPageStrideSamples; ++r)
        for (uint32 c = 0; c < PageStreaming::kPageStrideSamples; ++c)
        {
            const int64 j = std::clamp<int64>(int64(page.X) * 128 + c - 1, 0, shape.SamplesX - 1);
            const int64 k = std::clamp<int64>(int64(page.Z) * 128 + r - 1, 0, shape.SamplesZ - 1);
            samples[r * PageStreaming::kPageStrideSamples + c] =
                FieldHeight(float32(j << page.Level), float32(k << page.Level));
        }
    return samples;
}

// The cache, its page table and its texels, driven a frame at a time: one stream's field.
struct PagedField
{
    static constexpr uint32 kStream = 0;

    PageStreaming::PageCache Cache;
    PageTable Table;
    PageCacheGeometry Geometry;
    std::vector<float32> Texels;
    uint64 Frame = 0;
    uint32 LastTableWrites = 0;

    explicit PagedField(uint32 slots, float32 fadeSeconds = 0.4f, uint32 samplesX = kFieldX, uint32 samplesZ = kFieldZ)
    {
        Cache.Configure(slots, 2, fadeSeconds);
        Table.Reset(samplesX, samplesZ, slots);
        Geometry = MakePageCacheGeometry(slots);
        Texels.assign(std::size_t(Geometry.CacheDim) * Geometry.CacheDim, 0.0f);
    }

    uint32 TopLevel() const { return uint32(Table.Levels.size()) - 1u; }

    void Step(const std::vector<PageStreaming::PageAddress>& wanted, float32 deltaSeconds = 1.0f / 60.0f, uint32 cap = 64)
    {
        std::vector<PageStreaming::PageWant> wants;
        for (const PageStreaming::PageAddress& page : wanted)
            wants.push_back({page, 0.0f, false, kStream, page.Level >= TopLevel()});
        Cache.Update(++Frame, deltaSeconds, wants, cap);
        for (const PageStreaming::PageUploadRequest& upload : Cache.Uploads())
        {
            const PageStreaming::PageAddress& address = upload.Page.Address;
            const std::vector<float32> samples = PageSamples(Table.Levels[address.Level], address);
            for (uint32 r = 0; r < PageStreaming::kPageStrideSamples; ++r)
                for (uint32 c = 0; c < PageStreaming::kPageStrideSamples; ++c)
                    Texels[std::size_t(Geometry.TexelZ(upload.Slot, r)) * Geometry.CacheDim +
                           Geometry.TexelX(upload.Slot, c)] = samples[r * PageStreaming::kPageStrideSamples + c];
        }
        LastTableWrites = UpdatePageTable(Cache, kStream, Table);
    }

    std::vector<PageStreaming::PageAddress> AllPages() const
    {
        std::vector<PageStreaming::PageAddress> pages;
        for (uint32 level = 0; level < Table.Levels.size(); ++level)
            for (uint32 z = 0; z < Table.Levels[level].PagesZ; ++z)
                for (uint32 x = 0; x < Table.Levels[level].PagesX; ++x)
                    pages.push_back(PageStreaming::PageAddress{0, uint8(level), x, z});
        return pages;
    }

    /// Page `address` of the field's stream, as its cache holds it.
    static PageStreaming::CachedPage Page(const PageStreaming::PageAddress& address) { return {kStream, address}; }

    PagedHeightSampler Sampler() const { return PagedHeightSampler{&Table, Geometry, Texels.data()}; }
};

} // namespace GameEngine::TerrainECS::Test
