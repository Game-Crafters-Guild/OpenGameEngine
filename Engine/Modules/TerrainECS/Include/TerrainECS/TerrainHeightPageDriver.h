#pragma once

#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <span>
#include <string_view>

// One frame of the height pages, driven from the terrain extraction: per tiled terrain, the
// height-source decision (TerrainHeightIsPaged) and, when paged, its page requests; then one
// residency frame for every terrain, handed to the GPU side and the CBT's dirty rects.
namespace GameEngine::Rendering
{
class IDevice;
}

namespace GameEngine::TerrainECS
{

class TerrainService;
class TerrainRenderFeature;
class TerrainHeightPageFeature;
struct TiledTerrainData;
struct TiledRenderExtent;
struct TerrainHandle;

/// The height-source inputs of a tiled terrain and, when TerrainHeightIsPaged says so, its page
/// requests for `cameras` this frame. `renderTerrain` is the handle the CBT draws it with; `name` is
/// the entity's name for the log lines (empty: its render index);
/// `originY` and `heightScale` place its normalized heights in the world; `targetPixelError` is the
/// terrain's CBT split target; `focalScale` is the largest 1 / tan(vfov / 2) of the views (0 when
/// there is none: nothing is requested). `device` decides the CBT arm: the narrow arm
/// (CBTDeviceRunsNarrowArm) binds no page table. A terrain this does not request in a frame stops
/// being paged at that frame's UpdateHeightPages. Sets `tiled`'s PageOverlayByteCap (the
/// platform's cap when the terrain would page but for its modifiers, else zero) and logs
/// OverlayRefusal when its overlay is over the cap.
void RequestTiledTerrainHeightPages(TerrainService& service, const Rendering::IDevice& device,
                                    TiledTerrainData& tiled, const TiledRenderExtent& extent,
                                    const TerrainHandle& renderTerrain, std::string_view name, float32 originY,
                                    float32 heightScale,
                                    float32 targetPixelError, std::span<const Mathematics::Vector3> cameras,
                                    float32 focalScale);

/// After every terrain's requests: one residency frame (a terrain not requested this frame stops
/// being paged), its uploads and page tables published to `pages`, and the rects whose height
/// changed source published into the CBT's dirty-rect accumulator of `terrainFeature`.
void UpdateHeightPages(TerrainService& service, TerrainRenderFeature& terrainFeature, TerrainHeightPageFeature& pages,
                       uint64 frameIndex, float32 deltaSeconds, uint32 framesInFlight);

} // namespace GameEngine::TerrainECS
