#pragma once

#include "Types/Types.h"

#include <memory>

namespace GameEngine::Pathfinding
{

class DetourNavMap;

enum class ObstacleAvoidanceQuality : uint8
{
    Low = 0,
    Medium = 1,
    Good = 2,
    High = 3
};

struct CrowdAgentParams
{
    float32 Radius = 0.6f;
    float32 Height = 2.0f;
    float32 MaxAcceleration = 8.0f;
    float32 MaxSpeed = 3.5f;
    ObstacleAvoidanceQuality AvoidanceQuality = ObstacleAvoidanceQuality::High;
    float32 SeparationWeight = 2.0f;
    float32 CollisionQueryRange = 0.0f;    // 0 = auto (Radius * 12)
    float32 PathOptimizationRange = 0.0f;  // 0 = auto (Radius * 30)
};

using CrowdAgentHandle = int32;
static constexpr CrowdAgentHandle kInvalidCrowdAgent = -1;

// Thread safety: CrowdManager is NOT internally synchronized. All public methods
// (Initialize, Shutdown, AddAgent, RemoveAgent, SetAgentTarget, SetAgentParams,
// Update, and all getters) must be called from the same thread or externally
// synchronized. The goal is to make this thread-safe in the future — avoid
// adding shared mutable state without synchronization.
class CrowdManager
{
public:
    CrowdManager();
    ~CrowdManager();

    CrowdManager(const CrowdManager&) = delete;
    CrowdManager& operator=(const CrowdManager&) = delete;
    CrowdManager(CrowdManager&&) noexcept;
    CrowdManager& operator=(CrowdManager&&) noexcept;

    bool Initialize(DetourNavMap& navMap, uint32 maxAgents);
    void Shutdown();
    bool IsInitialized() const;

    CrowdAgentHandle AddAgent(float32 x, float32 y, float32 z, const CrowdAgentParams& params);
    void RemoveAgent(CrowdAgentHandle agent);

    void SetAgentTarget(CrowdAgentHandle agent, float32 x, float32 y, float32 z);
    void SetAgentParams(CrowdAgentHandle agent, const CrowdAgentParams& params);

    void GetAgentPosition(CrowdAgentHandle agent, float32& outX, float32& outY, float32& outZ) const;
    void GetAgentVelocity(CrowdAgentHandle agent, float32& outVx, float32& outVy, float32& outVz) const;
    void GetAgentTarget(CrowdAgentHandle agent, float32& outX, float32& outY, float32& outZ) const;
    bool IsAgentActive(CrowdAgentHandle agent) const;

    void Update(float32 deltaTime);

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace GameEngine::Pathfinding
