#pragma once

namespace GameEngine::Rendering
{

// Logs `feature` the first time an unimplemented WebGPU backend entry point is
// reached, then stays silent for that feature. Stubs call this so an unexpected
// consumer becomes visible without flooding the log — the capability report is
// what keeps the renderer off these paths in the first place.
void WebGpuLogUnsupportedOnce(const char* feature);

} // namespace GameEngine::Rendering
