#include "WebGpuUnsupported.h"

#include "Logger/Logger.h"

#include <mutex>
#include <string>
#include <unordered_set>

namespace GameEngine::Rendering
{

void WebGpuLogUnsupportedOnce(const char* feature)
{
    if (feature == nullptr)
    {
        return;
    }

    static std::mutex mutex;
    static std::unordered_set<std::string> reported;

    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!reported.emplace(feature).second)
        {
            return;
        }
    }

    Logger::Log::Warning("WebGPU backend: '{}' is not implemented; the call was ignored", feature);
}

} // namespace GameEngine::Rendering
