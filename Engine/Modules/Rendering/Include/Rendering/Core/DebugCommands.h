#pragma once

#include <string>
#include "Rendering/Core/Device.h"

namespace GameEngine::Rendering::Debug {

// Returns a human-readable string with pipeline cache statistics.
std::string GetPipelineCacheStatsString(IDevice& device);

// Logs pipeline cache statistics via stdout.
void PrintPipelineCacheStats(IDevice& device);

} // namespace GameEngine::Rendering::Debug
