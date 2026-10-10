#pragma once

#include "PageStreaming/PageCache.h"
#include "PageStreaming/PageStoreFormat.h"
#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <span>
#include <vector>

// The level rule of a terrain's height pages: which pyramid level each place on the terrain needs,
// from the CBT's own split rule, so a page is fetched at the level the geometry will use and no
// feedback pass is needed (design page-streaming §3.3).
//
// The planar CBT splits a facet while its projected edge exceeds t = TargetPixelError x H / 1080
// pixels, so the finest edge it asks for at distance d is d x t / f with f = (H / 2) / tan(vfov / 2).
// The geometry level is the continuous lambda = log2(d x t / (f x s0)), s0 the field's level-0
// texel: lambda 0 is level-0 detail, each whole step a level coarser. The CBT's own coarsening
// factors only make it coarser than this, so the rule over-fetches and never leaves a hole.
namespace GameEngine::TerrainECS
{

/// Every level whose texel is at least this many meters stays resident whatever the camera does
/// (design D5: a texel size, not a level number), so a fallback walk always ends on a page.
inline constexpr float32 kPinnedPageTexelMeters = 16.0f;

/// A pyramid of at most 1 / this of its height cache's slots is pinned whole (the owner's card 93,
/// option B: a small terrain keeps everything it has resident and never streams). The cap is a
/// fraction of the profile's per-field slot budget (design D5), so it follows the profile: what one
/// small terrain may pin is bounded the same way whether the field's cache is the terrain's own or,
/// as D5's per-profile budgets allow, shared by several terrains; a quarter keeps three quarters of
/// that budget for streamed working sets.
inline constexpr uint32 kPinWholePyramidCacheFraction = 4;

/// The rows TargetPixelError is stated at. The level rule depends on a view's rows only through
/// t / f, from which they cancel, so a caller without the render height states its view at these.
inline constexpr uint32 kPageLevelReferenceRows = 1080u;

/// The view constants of the level rule: f, the focal length in pixels, and t, the CBT's target edge
/// in pixels at this render height.
struct PageLevelView
{
    float32 FocalPixels = 0.0f;
    float32 TargetPixels = 0.0f;
};

/// The level rule's view at a render height (pixels), vertical field of view (radians) and the
/// terrain's TargetPixelError (pixels at 1080 rows).
PageLevelView MakePageLevelView(uint32 renderHeight, float32 verticalFovRadians, float32 targetPixelError);

/// The continuous geometry level at `distance` meters for a field whose level-0 texel is
/// `level0Texel` meters: log2(distance x t / (f x s0)), unclamped (negative = finer than level 0).
float32 GeometryPageLevel(float32 distance, float32 level0Texel, const PageLevelView& view);

/// The weight of the parent level in a sample at continuous level `level`: 0 over the first three
/// quarters of a level's ring, a linear ramp to 1 over the last quarter, so the surface reaches the
/// parent exactly where the ring ends and nothing pops when the camera moves across it.
float32 PageParentBlend(float32 level);

/// A terrain's height pyramid as the level rule sees it, in world meters.
struct PageRequestTerrain
{
    float32 OriginX = 0.0f;     ///< world position of level-0 sample (0, 0)
    float32 OriginZ = 0.0f;
    float32 Level0TexelX = 0.0f; ///< meters between level-0 samples along X
    float32 Level0TexelZ = 0.0f;
    uint32 SamplesX = 0;        ///< level-0 samples
    uint32 SamplesZ = 0;
    float32 MinHeight = 0.0f;   ///< world height range of the whole field
    float32 MaxHeight = 0.0f;
    uint32 CacheSlots = 0;      ///< the slots of the field's height cache (the profile's budget)
};

/// The first pinned level of a pyramid (whose levels are `levels`): 0, the whole pyramid, when its
/// page count is at most CacheSlots / kPinWholePyramidCacheFraction; else the finest level whose
/// texel is at least kPinnedPageTexelMeters, or the coarsest level when none is.
uint32 FirstPinnedPageLevel(const PageRequestTerrain& terrain, std::span<const PageStreaming::PageStoreLevel> levels);

/// The pages a terrain's geometry needs for its cameras, each frame. Holds the terrain's pyramid and
/// its scratch, so a frame allocates nothing once the request set's size has been seen.
class HeightPageRequester
{
public:
    /// (Re)configure for `terrain`: its pyramid and pinned levels.
    void Configure(const PageRequestTerrain& terrain);

    /// Every page of the pinned levels, then down the quadtree the children of each page some point
    /// of whose footprint needs a finer level than its own (lambda at the footprint's nearest point
    /// below the page's level). Each request's parent is requested too, so the set is closed under
    /// parents (the parent-first rule). Distance is to the footprint box (its rectangle and the
    /// field's height range), so every point of a page is at least that far away and the rule is
    /// conservative.
    void Build(std::span<const Mathematics::Vector3> cameras, const PageLevelView& view,
               std::vector<PageStreaming::PageWant>& out);

    uint32 FirstPinnedLevel() const { return m_FirstPinned; }

private:
    PageRequestTerrain m_Terrain;
    std::vector<PageStreaming::PageStoreLevel> m_Levels;
    std::vector<PageStreaming::PageAddress> m_Frontier;
    uint32 m_FirstPinned = 0;
};

} // namespace GameEngine::TerrainECS
