#include "Assets/HlodSelect.h"

#include <algorithm>
#include <cmath>

namespace GameEngine {
namespace Hlod {

SwitchConfig MakeSwitchConfig(float switchCoverage, float switchHysteresis) {
    SwitchConfig config;
    config.EnterCoverage = std::max(switchCoverage, 0.0f);
    const float band = std::max(switchHysteresis, 0.0f);
    config.ExitCoverage = config.EnterCoverage * (1.0f + band);
    return config;
}

float ClusterCoverage(const ViewLod& view, const float sphereCenter[3], float sphereRadius) {
    if (view.ProjScaleY <= 0.0f)
        return kAlwaysMembersCoverage; // LOD disabled (ortho/shadow) -> keep members

    const float dx = view.CameraPos[0] - sphereCenter[0];
    const float dy = view.CameraPos[1] - sphereCenter[1];
    const float dz = view.CameraPos[2] - sphereCenter[2];
    const float dist = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1e-4f);

    float coverage = sphereRadius * view.ProjScaleY / dist;
    coverage *= std::exp2(view.LodBiasGlobal);
    return coverage;
}

bool UpdateProxyActive(bool wasProxyActive, float coverage, const SwitchConfig& config) {
    if (wasProxyActive) {
        // Stay proxy until coverage rises past the (larger) exit threshold.
        return coverage < config.ExitCoverage;
    }
    // Engage proxy only once coverage drops below the enter threshold.
    return coverage < config.EnterCoverage;
}

} // namespace Hlod
} // namespace GameEngine
