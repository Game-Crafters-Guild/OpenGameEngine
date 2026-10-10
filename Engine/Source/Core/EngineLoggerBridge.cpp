#include "Core/EngineLoggerBridge.h"

namespace GameEngine
{

Logger::Log::LoggerState* GetEngineLoggerState()
{
    return Logger::Log::GetStateForSharing();
}

} // namespace GameEngine
