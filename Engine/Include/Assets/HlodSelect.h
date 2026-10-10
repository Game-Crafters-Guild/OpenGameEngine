#pragma once

// HLOD runtime switch decision (design v0.2 §5.2). Pure functions the
// HLODSelectSystem uses to decide, once per frame per cluster, whether a
// cluster shows its members or its merged proxy. Kept free of ECS/GPU so the
// screen-coverage math (which MUST agree with the GPU per-instance LOD pick in
// draw_command_scatter.comp's ge_SelectLOD, so the CPU cluster switch and the
// GPU mesh LOD are commensurate at the boundary) and the hysteresis state
// machine are unit-tested directly.

#include "Types/Types.h"

#include <cstdint>

namespace GameEngine {
namespace Hlod {

// Main-view LOD inputs, mirroring GPUDrawStreamBuilder::ViewLODParams /
// MakeViewLODParams. ProjScaleY is |proj[1][1]|; a value <= 0 (ortho / shadow /
// 2D editor view) means LOD selection is disabled and the cluster must stay on
// its members (matching ge_SelectLOD returning LOD0).
struct ViewLod {
    float CameraPos[3] = {0.0f, 0.0f, 0.0f};
    float ProjScaleY = 0.0f;
    float LodBiasGlobal = 0.0f;
};

// Per-volume switch thresholds resolved from HLODVolume (SwitchCoverage +
// SwitchHysteresis). EnterCoverage is the proxy-engage threshold; ExitCoverage
// (>= EnterCoverage) is the proxy-disengage threshold — the hysteresis band.
struct SwitchConfig {
    float EnterCoverage = 0.08f;
    float ExitCoverage = 0.10f;
};

// Build a SwitchConfig from the volume's coverage + hysteresis fraction.
SwitchConfig MakeSwitchConfig(float switchCoverage, float switchHysteresis);

// Screen-space coverage of a cluster bounding sphere for the given view — the
// ge_SelectLOD metric: worldRadius * projScaleY / distance, scaled by
// exp2(lodBiasGlobal). Returns a large value (kAlwaysMembersCoverage) when LOD
// is disabled (ProjScaleY <= 0) so the caller keeps members.
float ClusterCoverage(const ViewLod& view, const float sphereCenter[3], float sphereRadius);

// Coverage returned when LOD is disabled — above any EnterCoverage, so the
// cluster stays on members.
inline constexpr float kAlwaysMembersCoverage = 1.0e9f;

// Hysteresis update: given the previous proxy-active state and this frame's
// coverage, return the new state. Far (coverage below Enter) -> proxy; near
// (coverage above Exit) -> members; in the band -> unchanged.
bool UpdateProxyActive(bool wasProxyActive, float coverage, const SwitchConfig& config);

} // namespace Hlod
} // namespace GameEngine
