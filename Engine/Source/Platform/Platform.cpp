#include "Platform/Platform.h"
#include "Logger/Logger.h"
#include <thread>

namespace GameEngine {

bool Platform::s_Initialized = false;

bool Platform::Initialize() {
    if (s_Initialized) {
        return true;
    }

    Logger::Log::Info("Initializing Platform systems");
    Logger::Log::Info("Platform: {}", GetPlatformName());
    Logger::Log::Info("CPU cores: {}", GetCPUCoreCount());
    
    // TODO: Initialize platform-specific systems
    
    s_Initialized = true;
    Logger::Log::Info("Platform systems initialized");
    return true;
}

void Platform::Shutdown() {
    if (!s_Initialized) {
        return;
    }

    Logger::Log::Info("Shutting down Platform systems");
    
    // TODO: Cleanup platform-specific systems
    
    s_Initialized = false;
    Logger::Log::Info("Platform systems shutdown complete");
}

String Platform::GetPlatformName() {
    return PLATFORM_NAME;
}

uint32 Platform::GetCPUCoreCount() {
    return std::thread::hardware_concurrency();
}

uint64 Platform::GetTotalSystemMemory() {
    // TODO: Implement platform-specific memory detection
    return 0;
}

} // namespace GameEngine
