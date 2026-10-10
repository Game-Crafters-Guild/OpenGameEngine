#include "Scene/SceneSchemaRegistry.h"
#include "Scene/SceneValue.h"

#include "PathfindingECS/Components/NavigationAgent.h"
#include "PathfindingECS/Components/NavigationDebugSettings.h"
#include "PathfindingECS/Components/NavigationGrid.h"
#include "PathfindingECS/Components/NavigationMesh.h"
#include "PathfindingECS/Components/NavigationObstacle.h"
#include "ECS/ECSTemplates.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/NavigationGridRuntime.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

namespace GameEngine::Scene
{
namespace
{

// ---------------------------------------------------------------------------
// Helpers (same as BuiltInSceneSchemas.cpp local helpers)
// ---------------------------------------------------------------------------

static std::string FormatFloat(float v)
{
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

// The NavigationWorld map a component owns outlives the component's handle:
// loading a scene re-runs ApplyProperty on an entity that may already hold a
// built map and then clears the handle, and Remove drops the component
// outright. Both must hand the map back first or it stays live in
// NavigationWorld for the process lifetime. A cleared handle has generation 0,
// which RemoveMap rejects, so calling this twice is harmless.
static void ReleaseNavMap(uint32 mapIndex, uint32 mapGeneration)
{
    auto* navWorld = PathfindingECS::NavigationService::TryGet();
    if (!navWorld)
        return;
    navWorld->RemoveMap(Pathfinding::NavMapHandle{mapIndex, mapGeneration});
}

static bool ParseU32(std::string_view value, uint32_t& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind == SceneValueKind::Int)
    {
        if (v.IntValue < 0 || v.IntValue > 0xFFFFFFFFll)
        {
            if (outError)
                *outError = "Value out of range for uint32";
            return false;
        }
        out = static_cast<uint32_t>(v.IntValue);
        return true;
    }
    if (v.Kind == SceneValueKind::Float)
    {
        if (v.FloatValue < 0.0 || v.FloatValue > 4294967295.0)
        {
            if (outError)
                *outError = "Value out of range for uint32";
            return false;
        }
        out = static_cast<uint32_t>(v.FloatValue);
        return true;
    }
    if (outError)
        *outError = "Expected an integer value";
    return false;
}

static bool ParseF32(std::string_view value, float32& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind == SceneValueKind::Float)
    {
        out = (float32)v.FloatValue;
        return true;
    }
    if (v.Kind == SceneValueKind::Int)
    {
        out = (float32)v.IntValue;
        return true;
    }
    if (outError)
        *outError = "Expected a numeric value";
    return false;
}

static bool ParseI32(std::string_view value, int32_t& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind == SceneValueKind::Int)
    {
        constexpr int64_t kMin = (int64_t)std::numeric_limits<int32_t>::min();
        constexpr int64_t kMax = (int64_t)std::numeric_limits<int32_t>::max();
        if (v.IntValue < kMin || v.IntValue > kMax)
        {
            if (outError)
                *outError = "Value out of range for int32";
            return false;
        }
        out = static_cast<int32_t>(v.IntValue);
        return true;
    }
    if (v.Kind == SceneValueKind::Float)
    {
        constexpr double kMin = (double)std::numeric_limits<int32_t>::min();
        constexpr double kMax = (double)std::numeric_limits<int32_t>::max();
        if (v.FloatValue < kMin || v.FloatValue > kMax)
        {
            if (outError)
                *outError = "Value out of range for int32";
            return false;
        }
        out = static_cast<int32_t>(v.FloatValue);
        return true;
    }
    if (outError)
        *outError = "Expected an integer value";
    return false;
}

static bool ParseU8(std::string_view value, uint8& out, std::string* outError)
{
    uint32_t tmp = 0;
    if (!ParseU32(value, tmp, outError))
        return false;
    if (tmp > 0xFFu)
    {
        if (outError)
            *outError = "Value out of range for uint8";
        return false;
    }
    out = (uint8)tmp;
    return true;
}

static bool ParseBool(std::string_view value, bool& out, std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;
    if (v.Kind != SceneValueKind::Bool)
    {
        if (outError)
            *outError = "Expected true/false";
        return false;
    }
    out = v.BoolValue;
    return true;
}

static bool IsGuidZero(const uint8 guid[16])
{
    for (int i = 0; i < 16; ++i)
    {
        if (guid[i] != 0)
            return false;
    }
    return true;
}

static std::string FormatGuid(const uint8 guid[16])
{
    std::string result;
    result.reserve(32);
    for (int i = 0; i < 16; ++i)
    {
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02x", guid[i]);
        result += hex;
    }
    return result;
}

static bool ParseGuid(std::string_view value, uint8 guid[16], std::string* outError)
{
    SceneValue v{};
    if (!ParseValue(value, v, outError))
        return false;

    // Accept identifier (bare hex string) or quoted string.
    std::string hexStr;
    if (v.Kind == SceneValueKind::Identifier || v.Kind == SceneValueKind::String)
        hexStr = v.StringValue;
    else if (v.Kind == SceneValueKind::Int)
        hexStr = std::to_string(v.IntValue);
    else
    {
        if (outError)
            *outError = "Expected a hex GUID string";
        return false;
    }

    if (hexStr.size() != 32)
    {
        if (outError)
            *outError = "GUID must be 32 hex characters";
        return false;
    }

    for (int i = 0; i < 16; ++i)
    {
        unsigned long byte = 0;
        try
        {
            byte = std::stoul(hexStr.substr(i * 2, 2), nullptr, 16);
        }
        catch (...)
        {
            if (outError)
                *outError = "Invalid hex in GUID";
            return false;
        }
        guid[i] = static_cast<uint8>(byte);
    }
    return true;
}

// ---------------------------------------------------------------------------
// NavigationGridSchema
// ---------------------------------------------------------------------------

class NavigationGridSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "NavigationGrid"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::NavigationGrid>(entity);
        if (!c)
            return;

        outLines.push_back(std::string("NavigationGrid.gridType = ") + std::to_string(static_cast<uint32>(c->GridType)));
        outLines.push_back(std::string("NavigationGrid.cellSize = ") + FormatFloat(c->CellSize));
        outLines.push_back(std::string("NavigationGrid.width = ") + std::to_string(c->Width));
        outLines.push_back(std::string("NavigationGrid.depth = ") + std::to_string(c->Depth));
        outLines.push_back(std::string("NavigationGrid.maxSlope = ") + FormatFloat(c->MaxSlope));
        outLines.push_back(std::string("NavigationGrid.maxStepHeight = ") + FormatFloat(c->MaxStepHeight));
        outLines.push_back(std::string("NavigationGrid.raycastOriginHeight = ") + FormatFloat(c->RaycastOriginHeight));
        outLines.push_back(std::string("NavigationGrid.bakeSource = ") + std::to_string(static_cast<uint32>(c->BakeSource)));
        outLines.push_back(std::string("NavigationGrid.bakeAgentRadius = ") + FormatFloat(c->BakeAgentRadius));
        outLines.push_back(std::string("NavigationGrid.bakeAgentHeight = ") + FormatFloat(c->BakeAgentHeight));

        if (!IsGuidZero(c->AssetGuid))
            outLines.push_back(std::string("NavigationGrid.assetGuid = ") + FormatGuid(c->AssetGuid));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::NavigationGrid c{};
        if (auto* existing = world.GetComponent<Components::NavigationGrid>(entity))
            c = *existing;

        // Clear runtime state on load.
        PathfindingECS::ResetNavigationGridMap(world, c);

        if (property == "gridtype")
        {
            uint8 v = 0;
            if (!ParseU8(value, v, outError))
                return false;
            c.GridType = static_cast<Pathfinding::GridType>(v);
        }
        else if (property == "cellsize")
        {
            if (!ParseF32(value, c.CellSize, outError))
                return false;
        }
        else if (property == "width")
        {
            if (!ParseU32(value, c.Width, outError))
                return false;
        }
        else if (property == "depth")
        {
            if (!ParseU32(value, c.Depth, outError))
                return false;
        }
        else if (property == "maxslope")
        {
            if (!ParseF32(value, c.MaxSlope, outError))
                return false;
        }
        else if (property == "maxstepheight")
        {
            if (!ParseF32(value, c.MaxStepHeight, outError))
                return false;
        }
        else if (property == "raycastoriginheight")
        {
            if (!ParseF32(value, c.RaycastOriginHeight, outError))
                return false;
        }
        else if (property == "bakesource")
        {
            uint8 v = 0;
            if (!ParseU8(value, v, outError))
                return false;
            c.BakeSource = static_cast<Components::NavigationBakeSource>(v);
        }
        else if (property == "bakeagentradius")
        {
            if (!ParseF32(value, c.BakeAgentRadius, outError))
                return false;
        }
        else if (property == "bakeagentheight")
        {
            if (!ParseF32(value, c.BakeAgentHeight, outError))
                return false;
        }
        else if (property == "assetguid")
        {
            if (!ParseGuid(value, c.AssetGuid, outError))
                return false;
        }
        else
        {
            if (outError)
                *outError = "Unknown NavigationGrid property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::NavigationGrid>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::NavigationGrid{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        if (auto* c = world.GetComponentForWrite<Components::NavigationGrid>(entity))
            PathfindingECS::ResetNavigationGridMap(world, *c);
        world.RemoveComponentImmediate<Components::NavigationGrid>(entity);
        return true;
    }
};

// ---------------------------------------------------------------------------
// NavigationAgentSchema
// ---------------------------------------------------------------------------

class NavigationAgentSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "NavigationAgent"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::NavigationAgent>(entity);
        if (!c)
            return;

        outLines.push_back(std::string("NavigationAgent.speed = ") + FormatFloat(c->Speed));
        outLines.push_back(std::string("NavigationAgent.acceleration = ") + FormatFloat(c->Acceleration));
        outLines.push_back(std::string("NavigationAgent.stoppingDistance = ") + FormatFloat(c->StoppingDistance));
        outLines.push_back(std::string("NavigationAgent.radius = ") + FormatFloat(c->Radius));
        outLines.push_back(std::string("NavigationAgent.height = ") + FormatFloat(c->Height));
        outLines.push_back(std::string("NavigationAgent.smoothPaths = ") + (c->SmoothPaths ? "true" : "false"));
        outLines.push_back(std::string("NavigationAgent.avoidanceEnabled = ") + (c->AvoidanceEnabled ? "true" : "false"));
        outLines.push_back(std::string("NavigationAgent.avoidanceQuality = ") + std::to_string(c->AvoidanceQuality));
        outLines.push_back(std::string("NavigationAgent.separationWeight = ") + FormatFloat(c->SeparationWeight));
        outLines.push_back(std::string("NavigationAgent.replanInterval = ") + FormatFloat(c->ReplanInterval));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::NavigationAgent c{};
        if (auto* existing = world.GetComponent<Components::NavigationAgent>(entity))
            c = *existing;

        // Clear runtime state on load.
        c.NavMapIndex = 0;
        c.NavMapGeneration = 0;
        c.AgentId = 0;

        if (property == "speed")
        {
            if (!ParseF32(value, c.Speed, outError))
                return false;
        }
        else if (property == "acceleration")
        {
            if (!ParseF32(value, c.Acceleration, outError))
                return false;
        }
        else if (property == "stoppingdistance")
        {
            if (!ParseF32(value, c.StoppingDistance, outError))
                return false;
        }
        else if (property == "radius")
        {
            if (!ParseF32(value, c.Radius, outError))
                return false;
        }
        else if (property == "height")
        {
            if (!ParseF32(value, c.Height, outError))
                return false;
        }
        else if (property == "smoothpaths")
        {
            if (!ParseBool(value, c.SmoothPaths, outError))
                return false;
        }
        else if (property == "avoidanceenabled")
        {
            if (!ParseBool(value, c.AvoidanceEnabled, outError))
                return false;
        }
        else if (property == "avoidancequality")
        {
            if (!ParseU8(value, c.AvoidanceQuality, outError))
                return false;
        }
        else if (property == "separationweight")
        {
            if (!ParseF32(value, c.SeparationWeight, outError))
                return false;
        }
        else if (property == "replaninterval")
        {
            if (!ParseF32(value, c.ReplanInterval, outError))
                return false;
        }
        else
        {
            if (outError)
                *outError = "Unknown NavigationAgent property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::NavigationAgent>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::NavigationAgent{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::NavigationAgent>(entity);
        return true;
    }
};

// ---------------------------------------------------------------------------
// NavigationMeshSchema
// ---------------------------------------------------------------------------

class NavigationMeshSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "NavigationMesh"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::NavigationMesh>(entity);
        if (!c)
            return;

        outLines.push_back(std::string("NavigationMesh.cellSize = ") + FormatFloat(c->CellSize));
        outLines.push_back(std::string("NavigationMesh.cellHeight = ") + FormatFloat(c->CellHeight));
        outLines.push_back(std::string("NavigationMesh.agentRadius = ") + FormatFloat(c->AgentRadius));
        outLines.push_back(std::string("NavigationMesh.agentHeight = ") + FormatFloat(c->AgentHeight));
        outLines.push_back(std::string("NavigationMesh.agentMaxClimb = ") + FormatFloat(c->AgentMaxClimb));
        outLines.push_back(std::string("NavigationMesh.agentMaxSlope = ") + FormatFloat(c->AgentMaxSlope));
        outLines.push_back(std::string("NavigationMesh.regionMinSize = ") + FormatFloat(c->RegionMinSize));
        outLines.push_back(std::string("NavigationMesh.regionMergeSize = ") + FormatFloat(c->RegionMergeSize));
        outLines.push_back(std::string("NavigationMesh.edgeMaxLen = ") + FormatFloat(c->EdgeMaxLen));
        outLines.push_back(std::string("NavigationMesh.edgeMaxError = ") + FormatFloat(c->EdgeMaxError));
        outLines.push_back(std::string("NavigationMesh.detailSampleDist = ") + FormatFloat(c->DetailSampleDist));
        outLines.push_back(std::string("NavigationMesh.detailSampleMaxError = ") + FormatFloat(c->DetailSampleMaxError));
        outLines.push_back(std::string("NavigationMesh.vertsPerPoly = ") + std::to_string(c->VertsPerPoly));

        if (!IsGuidZero(c->AssetGuid))
            outLines.push_back(std::string("NavigationMesh.assetGuid = ") + FormatGuid(c->AssetGuid));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::NavigationMesh c{};
        if (auto* existing = world.GetComponent<Components::NavigationMesh>(entity))
            c = *existing;

        // Clear runtime state on load.
        if (c.Initialized)
            ReleaseNavMap(c.NavMapIndex, c.NavMapGeneration);
        c.NavMapIndex = 0;
        c.NavMapGeneration = 0;
        c.Initialized = false;
        c.NeedsRebuild = true;

        if (property == "cellsize")
        {
            if (!ParseF32(value, c.CellSize, outError))
                return false;
        }
        else if (property == "cellheight")
        {
            if (!ParseF32(value, c.CellHeight, outError))
                return false;
        }
        else if (property == "agentradius")
        {
            if (!ParseF32(value, c.AgentRadius, outError))
                return false;
        }
        else if (property == "agentheight")
        {
            if (!ParseF32(value, c.AgentHeight, outError))
                return false;
        }
        else if (property == "agentmaxclimb")
        {
            if (!ParseF32(value, c.AgentMaxClimb, outError))
                return false;
        }
        else if (property == "agentmaxslope")
        {
            if (!ParseF32(value, c.AgentMaxSlope, outError))
                return false;
        }
        else if (property == "regionminsize")
        {
            if (!ParseF32(value, c.RegionMinSize, outError))
                return false;
        }
        else if (property == "regionmergesize")
        {
            if (!ParseF32(value, c.RegionMergeSize, outError))
                return false;
        }
        else if (property == "edgemaxlen")
        {
            if (!ParseF32(value, c.EdgeMaxLen, outError))
                return false;
        }
        else if (property == "edgemaxerror")
        {
            if (!ParseF32(value, c.EdgeMaxError, outError))
                return false;
        }
        else if (property == "detailsampledist")
        {
            if (!ParseF32(value, c.DetailSampleDist, outError))
                return false;
        }
        else if (property == "detailsamplemaxerror")
        {
            if (!ParseF32(value, c.DetailSampleMaxError, outError))
                return false;
        }
        else if (property == "vertsperpoly")
        {
            if (!ParseI32(value, c.VertsPerPoly, outError))
                return false;
        }
        else if (property == "assetguid")
        {
            if (!ParseGuid(value, c.AssetGuid, outError))
                return false;
        }
        else
        {
            if (outError)
                *outError = "Unknown NavigationMesh property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::NavigationMesh>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::NavigationMesh{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        if (const auto* c = world.GetComponent<Components::NavigationMesh>(entity))
        {
            if (c->Initialized)
                ReleaseNavMap(c->NavMapIndex, c->NavMapGeneration);
        }
        world.RemoveComponentImmediate<Components::NavigationMesh>(entity);
        return true;
    }
};

// ---------------------------------------------------------------------------
// NavigationObstacleSchema
// ---------------------------------------------------------------------------

class NavigationObstacleSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "NavigationObstacle"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::NavigationObstacle>(entity);
        if (!c)
            return;

        outLines.push_back(std::string("NavigationObstacle.radius = ") + FormatFloat(c->Radius));
        outLines.push_back(std::string("NavigationObstacle.height = ") + FormatFloat(c->Height));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::NavigationObstacle c{};
        if (auto* existing = world.GetComponent<Components::NavigationObstacle>(entity))
            c = *existing;

        if (property == "radius")
        {
            if (!ParseF32(value, c.Radius, outError))
                return false;
        }
        else if (property == "height")
        {
            if (!ParseF32(value, c.Height, outError))
                return false;
        }
        else
        {
            if (outError)
                *outError = "Unknown NavigationObstacle property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::NavigationObstacle>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::NavigationObstacle{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::NavigationObstacle>(entity);
        return true;
    }
};

// ---------------------------------------------------------------------------
// NavigationDebugSettingsSchema
// ---------------------------------------------------------------------------

class NavigationDebugSettingsSchema final : public ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "NavigationDebugSettings"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, [[maybe_unused]] const SceneSaveContext& ctx, std::vector<std::string>& outLines) const override
    {
        const auto* c = world.GetComponent<Components::NavigationDebugSettings>(entity);
        if (!c)
            return;

        outLines.push_back(std::string("NavigationDebugSettings.showGrid = ") + (c->ShowGrid ? "true" : "false"));
        outLines.push_back(std::string("NavigationDebugSettings.showNavMesh = ") + (c->ShowNavMesh ? "true" : "false"));
        outLines.push_back(std::string("NavigationDebugSettings.showPaths = ") + (c->ShowPaths ? "true" : "false"));
        outLines.push_back(std::string("NavigationDebugSettings.showAgentRadii = ") + (c->ShowAgentRadii ? "true" : "false"));
        outLines.push_back(std::string("NavigationDebugSettings.showCellCosts = ") + (c->ShowCellCosts ? "true" : "false"));
        outLines.push_back(std::string("NavigationDebugSettings.showReservations = ") + (c->ShowReservations ? "true" : "false"));
        outLines.push_back(std::string("NavigationDebugSettings.gridLineThickness = ") + FormatFloat(c->GridLineThickness));
        outLines.push_back(std::string("NavigationDebugSettings.pathLineThickness = ") + FormatFloat(c->PathLineThickness));
    }

    bool ApplyProperty(ECS::World& world,
                       ECS::EntityHandle entity,
                       [[maybe_unused]] const SceneLoadContext& ctx,
                       std::string_view property,
                       std::string_view value,
                       std::string* outError) const override
    {
        Components::NavigationDebugSettings c{};
        if (auto* existing = world.GetComponent<Components::NavigationDebugSettings>(entity))
            c = *existing;

        if (property == "showgrid")
        {
            if (!ParseBool(value, c.ShowGrid, outError))
                return false;
        }
        else if (property == "shownavmesh")
        {
            if (!ParseBool(value, c.ShowNavMesh, outError))
                return false;
        }
        else if (property == "showpaths")
        {
            if (!ParseBool(value, c.ShowPaths, outError))
                return false;
        }
        else if (property == "showagentradii")
        {
            if (!ParseBool(value, c.ShowAgentRadii, outError))
                return false;
        }
        else if (property == "showcellcosts")
        {
            if (!ParseBool(value, c.ShowCellCosts, outError))
                return false;
        }
        else if (property == "showreservations")
        {
            if (!ParseBool(value, c.ShowReservations, outError))
                return false;
        }
        else if (property == "gridlinethickness")
        {
            if (!ParseF32(value, c.GridLineThickness, outError))
                return false;
        }
        else if (property == "pathlinethickness")
        {
            if (!ParseF32(value, c.PathLineThickness, outError))
                return false;
        }
        else
        {
            if (outError)
                *outError = "Unknown NavigationDebugSettings property";
            return false;
        }

        world.AddComponentImmediate(entity, c);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string* /*outError*/) const override
    {
        if (world.GetComponent<Components::NavigationDebugSettings>(entity))
            return true;
        world.AddComponentImmediate(entity, Components::NavigationDebugSettings{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::NavigationDebugSettings>(entity);
        return true;
    }
};

GE_REGISTER_SCENE_SCHEMA(NavigationGridSchema)
GE_REGISTER_SCENE_SCHEMA(NavigationAgentSchema)
GE_REGISTER_SCENE_SCHEMA(NavigationMeshSchema)
GE_REGISTER_SCENE_SCHEMA(NavigationObstacleSchema)
GE_REGISTER_SCENE_SCHEMA(NavigationDebugSettingsSchema)

} // namespace
} // namespace GameEngine::Scene
