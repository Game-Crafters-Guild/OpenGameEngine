#include "TerrainECS/PageLevelRule.h"


#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine::TerrainECS
{
namespace
{

constexpr float32 kReferenceRows = static_cast<float32>(kPageLevelReferenceRows);
constexpr float32 kBlendStart = 0.75f;

// Distance from `camera` to the box of a page's footprint: its world rectangle and the field's
// height range.
float32 DistanceToFootprint(const PageRequestTerrain& terrain, const PageStreaming::PageAddress& address,
                            const Mathematics::Vector3& camera)
{
    const float32 span = static_cast<float32>(PageStreaming::kPageOwnedSamples << address.Level);
    const float32 minX = terrain.OriginX + static_cast<float32>(address.X) * span * terrain.Level0TexelX;
    const float32 minZ = terrain.OriginZ + static_cast<float32>(address.Z) * span * terrain.Level0TexelZ;
    const float32 maxX = minX + span * terrain.Level0TexelX;
    const float32 maxZ = minZ + span * terrain.Level0TexelZ;
    const float32 dx = std::max({minX - camera.x, 0.0f, camera.x - maxX});
    const float32 dz = std::max({minZ - camera.z, 0.0f, camera.z - maxZ});
    const float32 dy = std::max({terrain.MinHeight - camera.y, 0.0f, camera.y - terrain.MaxHeight});
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float32 NearestDistance(const PageRequestTerrain& terrain, const PageStreaming::PageAddress& address,
                        std::span<const Mathematics::Vector3> cameras)
{
    float32 nearest = std::numeric_limits<float32>::max();
    for (const Mathematics::Vector3& camera : cameras)
        nearest = std::min(nearest, DistanceToFootprint(terrain, address, camera));
    return nearest;
}

} // namespace

PageLevelView MakePageLevelView(uint32 renderHeight, float32 verticalFovRadians, float32 targetPixelError)
{
    const float32 rows = static_cast<float32>(renderHeight);
    PageLevelView view;
    view.FocalPixels = 0.5f * rows / std::tan(0.5f * verticalFovRadians);
    view.TargetPixels = targetPixelError * rows / kReferenceRows;
    return view;
}

float32 GeometryPageLevel(float32 distance, float32 level0Texel, const PageLevelView& view)
{
    const float32 edge = std::max(distance, std::numeric_limits<float32>::min()) * view.TargetPixels;
    return std::log2(edge / (view.FocalPixels * level0Texel));
}

float32 PageParentBlend(float32 level)
{
    const float32 inRing = level - std::floor(level);
    return std::clamp((inRing - kBlendStart) / (1.0f - kBlendStart), 0.0f, 1.0f);
}

uint32 FirstPinnedPageLevel(const PageRequestTerrain& terrain, std::span<const PageStreaming::PageStoreLevel> levels)
{
    if (levels.empty())
        return 0;
    const uint64 pages = static_cast<uint64>(levels.back().FirstEntry) + levels.back().PagesX * levels.back().PagesZ;
    if (pages <= terrain.CacheSlots / kPinWholePyramidCacheFraction)
        return 0;
    const float32 texel = std::max(terrain.Level0TexelX, terrain.Level0TexelZ);
    for (uint32 level = 0; level < levels.size(); ++level)
        if (texel * static_cast<float32>(1u << level) >= kPinnedPageTexelMeters)
            return level;
    return static_cast<uint32>(levels.size()) - 1u;
}

void HeightPageRequester::Configure(const PageRequestTerrain& terrain)
{
    m_Terrain = terrain;
    m_Levels = PageStreaming::BuildPageStoreLevels(terrain.SamplesX, terrain.SamplesZ);
    m_FirstPinned = FirstPinnedPageLevel(terrain, m_Levels);
}

void HeightPageRequester::Build(std::span<const Mathematics::Vector3> cameras, const PageLevelView& view,
                                std::vector<PageStreaming::PageWant>& out)
{
    out.clear();
    if (m_Levels.empty() || cameras.empty())
        return;
    const float32 texel = std::max(m_Terrain.Level0TexelX, m_Terrain.Level0TexelZ);

    // Every page of the pinned levels, coarsest first; the walk below descends from the finest
    // pinned level, and a page enters the list only after its parent (closed under parents).
    m_Frontier.clear();
    for (uint32 level = static_cast<uint32>(m_Levels.size()); level-- > m_FirstPinned;)
    {
        for (uint32 z = 0; z < m_Levels[level].PagesZ; ++z)
        {
            for (uint32 x = 0; x < m_Levels[level].PagesX; ++x)
            {
                const PageStreaming::PageAddress address{0, static_cast<uint8>(level), x, z};
                out.push_back(PageStreaming::PageWant{address, NearestDistance(m_Terrain, address, cameras), true});
                if (level == m_FirstPinned)
                    m_Frontier.push_back(address);
            }
        }
    }

    while (!m_Frontier.empty())
    {
        const PageStreaming::PageAddress page = m_Frontier.back();
        m_Frontier.pop_back();
        if (page.Level == 0)
            continue;
        const float32 distance = NearestDistance(m_Terrain, page, cameras);
        if (GeometryPageLevel(distance, texel, view) >= static_cast<float32>(page.Level))
            continue; // the whole footprint is content with this level or coarser
        const PageStreaming::PageStoreLevel& finer = m_Levels[page.Level - 1u];
        for (uint32 dz = 0; dz < 2u; ++dz)
        {
            for (uint32 dx = 0; dx < 2u; ++dx)
            {
                const PageStreaming::PageAddress child{0, static_cast<uint8>(page.Level - 1u), 2u * page.X + dx,
                                                       2u * page.Z + dz};
                if (child.X >= finer.PagesX || child.Z >= finer.PagesZ)
                    continue;
                out.push_back(PageStreaming::PageWant{child, NearestDistance(m_Terrain, child, cameras), false});
                m_Frontier.push_back(child);
            }
        }
    }
}

} // namespace GameEngine::TerrainECS
