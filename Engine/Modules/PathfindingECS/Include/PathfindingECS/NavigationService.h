#pragma once
#include "Pathfinding/NavigationWorld.h"
#include <memory>
#include <mutex>
#include <vector>

namespace GameEngine::PathfindingECS
{

struct NavDebugData
{
    struct Line { float32 X1, Y1, Z1, X2, Y2, Z2; float32 R, G, B, A; };
    struct Triangle { float32 X1, Y1, Z1, X2, Y2, Z2, X3, Y3, Z3; float32 R, G, B, A; };

    // Cached: regenerated only when grid topology changes
    std::vector<Line> CachedLines;
    std::vector<Triangle> CachedTriangles;

    // Dynamic: regenerated every frame (paths, agents, reservations)
    std::vector<Line> DynamicLines;

    // Combined output for renderer (merged from cached + dynamic each frame)
    std::vector<Line> Lines;
    std::vector<Triangle> Triangles;

    void Clear()
    {
        DynamicLines.clear();
        Lines.clear();
        Triangles.clear();
    }

    void ClearCached()
    {
        CachedLines.clear();
        CachedTriangles.clear();
    }

    void Finalize()
    {
        Lines.clear();
        Lines.reserve(CachedLines.size() + DynamicLines.size());
        Lines.insert(Lines.end(), CachedLines.begin(), CachedLines.end());
        Lines.insert(Lines.end(), DynamicLines.begin(), DynamicLines.end());
        Triangles = CachedTriangles;
    }
};

// Instance class that owns the navigation world and debug data.
// Can be used directly for multi-world scenarios in the future.
class NavigationServiceInstance
{
public:
    NavigationServiceInstance();
    ~NavigationServiceInstance();

    Pathfinding::NavigationWorld& GetWorld();
    Pathfinding::NavigationWorld* TryGetWorld();
    NavDebugData& GetDebugData();

private:
    std::unique_ptr<Pathfinding::NavigationWorld> m_World;
    NavDebugData m_DebugData;
};

// Static singleton API (delegates to a global NavigationServiceInstance).
// Kept for backward compatibility - all existing code continues to work.
class NavigationService
{
public:
    static void Initialize();
    static void Shutdown();
    static Pathfinding::NavigationWorld& Get();
    static Pathfinding::NavigationWorld* TryGet();
    static bool IsInitialized();
    static uint64 GetGeneration();
    static NavDebugData& GetDebugData();

private:
    static std::unique_ptr<NavigationServiceInstance> s_Instance;
    static uint64 s_Generation;
    static std::mutex s_Mutex;
};

} // namespace GameEngine::PathfindingECS
