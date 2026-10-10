#pragma once

#include "Logger/Logger.h"

namespace GameEngine
{

// The Logger state owned by this module — Engine.dll when the engine is linked
// shared. Logger is a static library, so a host executable carries its own copy
// of the Log singleton: without adoption, engine-side logs only ever reach the
// engine copy's console sink and are invisible to the host's ring-buffer (MCP
// get_log), file, and Log-panel sinks. Hosts call, first thing in main():
//   Logger::Log::RedirectToSharedState(GameEngine::GetEngineLoggerState());
Logger::Log::LoggerState* GetEngineLoggerState();

} // namespace GameEngine
