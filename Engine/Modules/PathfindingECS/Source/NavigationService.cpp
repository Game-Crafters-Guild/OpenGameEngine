#include "PathfindingECS/NavigationService.h"
#include "Logger/Logger.h"

#include <cassert>

namespace GameEngine::PathfindingECS
{

// ---------------------------------------------------------------------------
// NavigationServiceInstance
// ---------------------------------------------------------------------------

NavigationServiceInstance::NavigationServiceInstance()
    : m_World(std::make_unique<Pathfinding::NavigationWorld>())
{
}

NavigationServiceInstance::~NavigationServiceInstance() = default;

Pathfinding::NavigationWorld& NavigationServiceInstance::GetWorld()
{
    return *m_World;
}

Pathfinding::NavigationWorld* NavigationServiceInstance::TryGetWorld()
{
    return m_World.get();
}

NavDebugData& NavigationServiceInstance::GetDebugData()
{
    return m_DebugData;
}

// ---------------------------------------------------------------------------
// NavigationService (static singleton delegates to instance)
// ---------------------------------------------------------------------------

std::unique_ptr<NavigationServiceInstance> NavigationService::s_Instance;
uint64 NavigationService::s_Generation = 0;
std::mutex NavigationService::s_Mutex;

void NavigationService::Initialize()
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    if (s_Instance)
    {
        Logger::Log::Warning("[PathfindingECS] NavigationService already initialized; ignoring duplicate Initialize()");
        return;
    }
    s_Instance = std::make_unique<NavigationServiceInstance>();
    ++s_Generation;
    Logger::Log::Info("[PathfindingECS] NavigationService initialized (generation={})", s_Generation);
}

void NavigationService::Shutdown()
{
    std::lock_guard<std::mutex> lock(s_Mutex);
    if (!s_Instance)
    {
        Logger::Log::Warning("[PathfindingECS] NavigationService not initialized; ignoring Shutdown()");
        return;
    }
    s_Instance.reset();
    ++s_Generation;
    Logger::Log::Info("[PathfindingECS] NavigationService shut down (generation={})", s_Generation);
}

Pathfinding::NavigationWorld& NavigationService::Get()
{
    assert(s_Instance && "NavigationService::Get() called before Initialize()");
    return s_Instance->GetWorld();
}

Pathfinding::NavigationWorld* NavigationService::TryGet()
{
    return s_Instance ? s_Instance->TryGetWorld() : nullptr;
}

bool NavigationService::IsInitialized()
{
    return s_Instance != nullptr;
}

uint64 NavigationService::GetGeneration()
{
    return s_Generation;
}

NavDebugData& NavigationService::GetDebugData()
{
    assert(s_Instance && "NavigationService::GetDebugData() called before Initialize()");
    return s_Instance->GetDebugData();
}

} // namespace GameEngine::PathfindingECS
