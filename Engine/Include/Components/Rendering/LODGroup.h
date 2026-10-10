#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

// Per-entity LOD bias consumed by the GPU selector (draw_command_scatter.comp
// ge_SelectLOD): screen coverage is scaled by exp2(Bias + global bias) before
// the per-mesh threshold compare. Positive = more detail retained at range,
// negative = coarser sooner. Extraction plumbs it into GPUInstance.lodBias
// through the dirty-range gate. Per-mesh thresholds live on the GPUMesh row
// (MeshGPURegistry) — this component is deliberately just the per-entity knob.
struct LODGroup {
    float32 Bias {0.0f};
};

} // namespace Components
} // namespace GameEngine

