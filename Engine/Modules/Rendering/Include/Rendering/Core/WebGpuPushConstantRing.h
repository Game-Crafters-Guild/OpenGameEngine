// The browser WebGPU backend's push-constant emulation ring, as callers see it.

#pragma once

#include <cstdint>

namespace GameEngine::Rendering
{

// Browser WebGPU has no push constants: each draw or dispatch of a pipeline that
// declares a push-constant block takes one slot of a per-frame uniform ring. A
// frame that needs more slots than this has its excess draws refused (and
// reported once) rather than recorded with the block unbound.
inline constexpr uint32_t kWebGpuPushConstantSlotsPerFrame = 8192;

} // namespace GameEngine::Rendering
