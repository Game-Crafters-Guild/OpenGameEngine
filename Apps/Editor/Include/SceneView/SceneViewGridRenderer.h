#pragma once

#include <cstdint>

namespace GameEngine {

namespace Rendering {
struct CameraData;
namespace RenderGraph {
class RGContext;
}
} // namespace Rendering

// Renders the infinite editor grid for both 3D (Y=0 plane) and 2D (Z=0 / XY
// plane) scene view modes. Owns the lazily-created GPU pipelines and shader
// loading. Stateless: caller passes grid opacity, camera data, etc. per frame.
class SceneViewGridRenderer
{
public:
    // Record grid draw commands into the current gizmo overlay pass.
    // Call exactly once per frame, inside the gizmo overlay execute lambda.
    // gridColor is ARGB packed; only the rgb channels reach the shader (the
    // axis lines keep their hardcoded conventional colors).
    //
    // Pipeline variant resolved through the RenderGraph pass context (format key from
    // the pass's declared attachments — no sample-count member cache needed;
    // the device variant cache memoizes).
    void RecordRG(const Rendering::RenderGraph::RGContext& ctx,
                   const Rendering::CameraData* cam,
                   const float camPos[3],
                   bool is2DMode,
                   float camDistance,
                   float viewportH,
                   float gridOpacity,
                   uint32_t gridColor);
};

} // namespace GameEngine
