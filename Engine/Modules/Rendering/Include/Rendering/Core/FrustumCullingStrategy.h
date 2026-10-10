#pragma once

#include "Rendering/Core/CullingStrategy.h"

namespace GameEngine
{
namespace Rendering
{

// Default strategy. Builds a `ViewCullingInput` from the per-view context
// and submits it to the shared `GPUCullingPipeline`, which batches all
// submitted views into a single `frustum_culling.comp` dispatch wave at
// `EndFrame`. The pipeline writes per-view visibility slices that the
// bucketer scheduler reads to gate indirect draws.
//
// Used by every camera-backed view in the main render path. Editor
// thumbnails opt out via `NoneCullingStrategy` (no frustum cull when the
// preview scene is small enough that cull cost &gt; cull save).
class FrustumCullingStrategy final : public ICullingStrategy
{
public:
    void ScheduleCulling(const ViewCullingContext& ctx) override;
};

} // namespace Rendering
} // namespace GameEngine
