#include "Pathfinding/ObstacleAvoidance.h"
#include "Pathfinding/DetourNavMap.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cstring>

#if GE_PATHFINDING_BACKEND_DETOUR
#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#endif

namespace GameEngine::Pathfinding
{

#if GE_PATHFINDING_BACKEND_DETOUR

struct CrowdManager::Impl
{
    dtCrowd* Crowd = nullptr;
    dtNavMesh* NavMesh = nullptr;
    dtNavMeshQuery* NavQuery = nullptr;

    ~Impl()
    {
        if (NavQuery)
        {
            dtFreeNavMeshQuery(NavQuery);
            NavQuery = nullptr;
        }
        if (Crowd)
        {
            dtFreeCrowd(Crowd);
            Crowd = nullptr;
        }
    }
};

CrowdManager::CrowdManager()
    : m_Impl(std::make_unique<Impl>())
{
}

CrowdManager::~CrowdManager() = default;
CrowdManager::CrowdManager(CrowdManager&&) noexcept = default;
CrowdManager& CrowdManager::operator=(CrowdManager&&) noexcept = default;

bool CrowdManager::Initialize(DetourNavMap& navMap, uint32 maxAgents)
{
    if (!navMap.IsBuilt())
        return false;

    // Clean up any previous initialization to prevent leaking the old dtCrowd.
    if (m_Impl->Crowd)
        Shutdown();

    m_Impl->NavMesh = static_cast<dtNavMesh*>(navMap.GetDetourNavMesh());
    if (!m_Impl->NavMesh)
        return false;

    m_Impl->NavQuery = dtAllocNavMeshQuery();
    if (!m_Impl->NavQuery)
        return false;

    if (dtStatusFailed(m_Impl->NavQuery->init(m_Impl->NavMesh, 2048)))
    {
        dtFreeNavMeshQuery(m_Impl->NavQuery);
        m_Impl->NavQuery = nullptr;
        return false;
    }

    m_Impl->Crowd = dtAllocCrowd();
    if (!m_Impl->Crowd)
    {
        dtFreeNavMeshQuery(m_Impl->NavQuery);
        m_Impl->NavQuery = nullptr;
        return false;
    }

    static constexpr float32 kDefaultMaxAgentRadius = 2.4f;
    if (!m_Impl->Crowd->init(static_cast<int32>(maxAgents), kDefaultMaxAgentRadius,
                              m_Impl->NavMesh))
    {
        dtFreeCrowd(m_Impl->Crowd);
        m_Impl->Crowd = nullptr;
        dtFreeNavMeshQuery(m_Impl->NavQuery);
        m_Impl->NavQuery = nullptr;
        return false;
    }

    // Configure obstacle avoidance params. All fields must be set — leaving
    // weightDesVel/weightCurVel/gridSize at zero causes the avoidance query
    // to produce zero velocity, making agents unable to move.
    dtObstacleAvoidanceParams avoidanceParams;

    // Low quality
    avoidanceParams.velBias = 0.5f;
    avoidanceParams.weightDesVel = 2.0f;
    avoidanceParams.weightCurVel = 0.75f;
    avoidanceParams.weightSide = 0.75f;
    avoidanceParams.weightToi = 2.5f;
    avoidanceParams.horizTime = 2.5f;
    avoidanceParams.gridSize = 33;
    avoidanceParams.adaptiveDivs = 5;
    avoidanceParams.adaptiveRings = 2;
    avoidanceParams.adaptiveDepth = 1;
    m_Impl->Crowd->setObstacleAvoidanceParams(0, &avoidanceParams);

    // Medium quality
    avoidanceParams.adaptiveDivs = 5;
    avoidanceParams.adaptiveRings = 2;
    avoidanceParams.adaptiveDepth = 2;
    m_Impl->Crowd->setObstacleAvoidanceParams(1, &avoidanceParams);

    // Good quality
    avoidanceParams.adaptiveDivs = 7;
    avoidanceParams.adaptiveRings = 2;
    avoidanceParams.adaptiveDepth = 3;
    m_Impl->Crowd->setObstacleAvoidanceParams(2, &avoidanceParams);

    // High quality
    avoidanceParams.adaptiveDivs = 7;
    avoidanceParams.adaptiveRings = 3;
    avoidanceParams.adaptiveDepth = 3;
    m_Impl->Crowd->setObstacleAvoidanceParams(3, &avoidanceParams);

    return true;
}

void CrowdManager::Shutdown()
{
    if (m_Impl->Crowd)
    {
        dtFreeCrowd(m_Impl->Crowd);
        m_Impl->Crowd = nullptr;
    }
    if (m_Impl->NavQuery)
    {
        dtFreeNavMeshQuery(m_Impl->NavQuery);
        m_Impl->NavQuery = nullptr;
    }
    m_Impl->NavMesh = nullptr;
}

bool CrowdManager::IsInitialized() const
{
    return m_Impl && m_Impl->Crowd != nullptr;
}

CrowdAgentHandle CrowdManager::AddAgent(float32 x, float32 y, float32 z, const CrowdAgentParams& params)
{
    if (!IsInitialized())
        return kInvalidCrowdAgent;

    const float32 pos[3] = {x, y, z};

    dtCrowdAgentParams agentParams = {};
    agentParams.radius = params.Radius;
    agentParams.height = params.Height;
    agentParams.maxAcceleration = params.MaxAcceleration;
    agentParams.maxSpeed = params.MaxSpeed;
    agentParams.collisionQueryRange = (params.CollisionQueryRange > 0.0f)
        ? params.CollisionQueryRange : params.Radius * 12.0f;
    agentParams.pathOptimizationRange = (params.PathOptimizationRange > 0.0f)
        ? params.PathOptimizationRange : params.Radius * 30.0f;
    agentParams.separationWeight = params.SeparationWeight;
    agentParams.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS |
                              DT_CROWD_OPTIMIZE_TOPO | DT_CROWD_OBSTACLE_AVOIDANCE |
                              DT_CROWD_SEPARATION;
    agentParams.obstacleAvoidanceType = static_cast<uint8>(params.AvoidanceQuality);
    agentParams.queryFilterType = 0;

    return static_cast<CrowdAgentHandle>(m_Impl->Crowd->addAgent(pos, &agentParams));
}

void CrowdManager::RemoveAgent(CrowdAgentHandle agent)
{
    if (!IsInitialized() || agent == kInvalidCrowdAgent)
        return;
    m_Impl->Crowd->removeAgent(static_cast<int32>(agent));
}

void CrowdManager::SetAgentTarget(CrowdAgentHandle agent, float32 x, float32 y, float32 z)
{
    if (!IsInitialized() || agent == kInvalidCrowdAgent)
        return;

    const float32 targetPos[3] = {x, y, z};

    const dtCrowdAgent* crowdAgent = m_Impl->Crowd->getAgent(static_cast<int32>(agent));
    const float32 agentRadius = (crowdAgent && crowdAgent->active) ? crowdAgent->params.radius : 0.6f;
    const float32 agentHeight = (crowdAgent && crowdAgent->active) ? crowdAgent->params.height : 2.0f;
    const float32 extent[3] = {
        std::max(2.0f, agentRadius * 4.0f),
        std::max(4.0f, agentHeight * 2.0f),
        std::max(2.0f, agentRadius * 4.0f)
    };

    dtPolyRef targetRef = 0;
    float32 nearestPos[3] = {x, y, z};
    dtStatus status = m_Impl->NavQuery->findNearestPoly(targetPos, extent,
                                                         m_Impl->Crowd->getFilter(0),
                                                         &targetRef, nearestPos);
    if (dtStatusFailed(status) || targetRef == 0)
    {
        Logger::Log::Warning("[CrowdManager] SetAgentTarget: findNearestPoly failed for target ({}, {}, {})",
                             x, y, z);
        return;
    }

    if (!m_Impl->Crowd->requestMoveTarget(static_cast<int32>(agent), targetRef, nearestPos))
    {
        Logger::Log::Warning("[CrowdManager] SetAgentTarget: requestMoveTarget failed for agent {} "
                             "to ({}, {}, {})", agent, nearestPos[0], nearestPos[1], nearestPos[2]);
    }
}

void CrowdManager::SetAgentParams(CrowdAgentHandle agent, const CrowdAgentParams& params)
{
    if (!IsInitialized() || agent == kInvalidCrowdAgent)
        return;

    const dtCrowdAgent* existingAgent = m_Impl->Crowd->getAgent(static_cast<int32>(agent));
    if (!existingAgent || !existingAgent->active)
        return;

    dtCrowdAgentParams agentParams = existingAgent->params;
    agentParams.radius = params.Radius;
    agentParams.height = params.Height;
    agentParams.maxAcceleration = params.MaxAcceleration;
    agentParams.maxSpeed = params.MaxSpeed;
    agentParams.separationWeight = params.SeparationWeight;
    agentParams.obstacleAvoidanceType = static_cast<uint8>(params.AvoidanceQuality);
    // Recompute derived ranges from the new radius to keep them consistent.
    agentParams.collisionQueryRange = (params.CollisionQueryRange > 0.0f)
        ? params.CollisionQueryRange : params.Radius * 12.0f;
    agentParams.pathOptimizationRange = (params.PathOptimizationRange > 0.0f)
        ? params.PathOptimizationRange : params.Radius * 30.0f;

    m_Impl->Crowd->updateAgentParameters(static_cast<int32>(agent), &agentParams);
}

void CrowdManager::GetAgentPosition(CrowdAgentHandle agent, float32& outX, float32& outY, float32& outZ) const
{
    outX = outY = outZ = 0.0f;

    if (!IsInitialized() || agent == kInvalidCrowdAgent)
        return;

    const dtCrowdAgent* crowdAgent = m_Impl->Crowd->getAgent(static_cast<int32>(agent));
    if (!crowdAgent || !crowdAgent->active)
        return;

    outX = crowdAgent->npos[0];
    outY = crowdAgent->npos[1];
    outZ = crowdAgent->npos[2];
}

void CrowdManager::GetAgentVelocity(CrowdAgentHandle agent, float32& outVx, float32& outVy, float32& outVz) const
{
    outVx = outVy = outVz = 0.0f;

    if (!IsInitialized() || agent == kInvalidCrowdAgent)
        return;

    const dtCrowdAgent* crowdAgent = m_Impl->Crowd->getAgent(static_cast<int32>(agent));
    if (!crowdAgent || !crowdAgent->active)
        return;

    outVx = crowdAgent->vel[0];
    outVy = crowdAgent->vel[1];
    outVz = crowdAgent->vel[2];
}

void CrowdManager::GetAgentTarget(CrowdAgentHandle agent, float32& outX, float32& outY, float32& outZ) const
{
    outX = outY = outZ = 0.0f;

    if (!IsInitialized() || agent == kInvalidCrowdAgent)
        return;

    const dtCrowdAgent* crowdAgent = m_Impl->Crowd->getAgent(static_cast<int32>(agent));
    if (!crowdAgent || !crowdAgent->active)
        return;

    outX = crowdAgent->targetPos[0];
    outY = crowdAgent->targetPos[1];
    outZ = crowdAgent->targetPos[2];
}

bool CrowdManager::IsAgentActive(CrowdAgentHandle agent) const
{
    if (!IsInitialized() || agent == kInvalidCrowdAgent)
        return false;

    const dtCrowdAgent* crowdAgent = m_Impl->Crowd->getAgent(static_cast<int32>(agent));
    return crowdAgent && crowdAgent->active;
}

void CrowdManager::Update(float32 deltaTime)
{
    if (!IsInitialized())
        return;
    m_Impl->Crowd->update(deltaTime, nullptr);
}

#else // !GE_PATHFINDING_BACKEND_DETOUR

struct CrowdManager::Impl
{
};

CrowdManager::CrowdManager()
    : m_Impl(std::make_unique<Impl>())
{
}

CrowdManager::~CrowdManager() = default;
CrowdManager::CrowdManager(CrowdManager&&) noexcept = default;
CrowdManager& CrowdManager::operator=(CrowdManager&&) noexcept = default;

bool CrowdManager::Initialize(DetourNavMap& /*navMap*/, uint32 /*maxAgents*/)
{
    return false;
}

void CrowdManager::Shutdown()
{
}

bool CrowdManager::IsInitialized() const
{
    return false;
}

CrowdAgentHandle CrowdManager::AddAgent(float32 /*x*/, float32 /*y*/, float32 /*z*/, const CrowdAgentParams& /*params*/)
{
    return kInvalidCrowdAgent;
}

void CrowdManager::RemoveAgent(CrowdAgentHandle /*agent*/)
{
}

void CrowdManager::SetAgentTarget(CrowdAgentHandle /*agent*/, float32 /*x*/, float32 /*y*/, float32 /*z*/)
{
}

void CrowdManager::SetAgentParams(CrowdAgentHandle /*agent*/, const CrowdAgentParams& /*params*/)
{
}

void CrowdManager::GetAgentPosition(CrowdAgentHandle /*agent*/, float32& outX, float32& outY, float32& outZ) const
{
    outX = outY = outZ = 0.0f;
}

void CrowdManager::GetAgentVelocity(CrowdAgentHandle /*agent*/, float32& outVx, float32& outVy, float32& outVz) const
{
    outVx = outVy = outVz = 0.0f;
}

void CrowdManager::GetAgentTarget(CrowdAgentHandle /*agent*/, float32& outX, float32& outY, float32& outZ) const
{
    outX = outY = outZ = 0.0f;
}

bool CrowdManager::IsAgentActive(CrowdAgentHandle /*agent*/) const
{
    return false;
}

void CrowdManager::Update(float32 /*deltaTime*/)
{
}

#endif // GE_PATHFINDING_BACKEND_DETOUR

} // namespace GameEngine::Pathfinding
