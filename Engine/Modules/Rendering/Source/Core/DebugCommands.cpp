#include "Rendering/Core/DebugCommands.h"
#include <iostream>
#include <sstream>

namespace GameEngine::Rendering::Debug {

std::string GetPipelineCacheStatsString(IDevice& device) {
    const auto stats = device.GetPipelineCacheStats();
    std::ostringstream oss;
    oss << "PipelineCache Stats:\n";
    oss << "  hits:        " << stats.Hits << "\n";
    oss << "  misses:      " << stats.Misses << "\n";
    oss << "  inserts:     " << stats.Inserts << "\n";
    oss << "  evictions:   " << stats.Evictions << "\n";
    oss << "  clears:      " << stats.Clears << "\n";
    oss << "  pinned:      " << stats.PinnedCount;
    return oss.str();
}

void PrintPipelineCacheStats(IDevice& device) {
    std::cout << GetPipelineCacheStatsString(device) << std::endl;
}

} // namespace GameEngine::Rendering::Debug
