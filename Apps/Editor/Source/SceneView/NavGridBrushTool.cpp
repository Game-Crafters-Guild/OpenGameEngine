#include "SceneView/NavGridBrushTool.h"

#include "SceneViewController.h"
#include "Core/Engine.h"
#include "ECS/ECS.h"
#include "ECS/Query.h"
#include "PathfindingECS/NavigationService.h"
#include "PathfindingECS/NavigationGridRuntime.h"
#include "PathfindingECS/Components/NavigationGrid.h"
#include "Pathfinding/NavigationWorld.h"
#include "Pathfinding/GridMap.h"
#include "Pathfinding/PathfindingTypes.h"
#include "Assets/NavGridAsset.h"
#include "Platform/Shell.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"
#include "Logger/Logger.h"
#include "Mathematics/Vector3.h"
#include "Types/Color.h"

#include <cmath>

namespace GameEngine::Editor::SceneTools
{

namespace
{

constexpr uint32_t kMaxBrushRadius = 4;
constexpr float kRayDirectionEpsilon = 1e-6f;

// Find the first initialized grid map via ECS NavigationGrid components.
Pathfinding::GridMap* FindFirstGridMap(ECS::World* ecsWorld)
{
    auto* navWorld = PathfindingECS::NavigationService::TryGet();
    if (!navWorld)
        return nullptr;

    if (!ecsWorld)
        return nullptr;

    Pathfinding::GridMap* result = nullptr;
    ecsWorld->Query<ECS::Read<Components::NavigationGrid>>().Each(
        [&](ECS::EntityHandle /*e*/, const Components::NavigationGrid& source)
        {
            if (result)
                return;
            result = PathfindingECS::ResolveNavigationGridMap(*ecsWorld, source);
        });

    return result;
}

class GridPaintCommand final : public Editor::IEditorCommand
{
public:
    GridPaintCommand(ECS::World& world, std::vector<NavGridBrushCellSnapshot> beforeState,
                     std::vector<NavGridBrushCellSnapshot> afterState)
        : m_World(&world), m_BeforeState(std::move(beforeState))
        , m_AfterState(std::move(afterState))
    {
    }

    const char* GetName() const override { return "Paint Navigation Grid"; }

    void Do() override { ApplyState(m_AfterState); }
    void Undo() override { ApplyState(m_BeforeState); }

private:
    void ApplyState(const std::vector<NavGridBrushCellSnapshot>& state)
    {
        auto* gridMap = FindFirstGridMap(m_World);
        if (!gridMap)
            return;
        for (const auto& cell : state)
        {
            gridMap->SetCellCost(cell.CellX, cell.CellZ, cell.Cost);
            gridMap->SetCellBlocked(cell.CellX, cell.CellZ, cell.Blocked);
        }
    }

    ECS::World* const m_World;
    std::vector<NavGridBrushCellSnapshot> m_BeforeState;
    std::vector<NavGridBrushCellSnapshot> m_AfterState;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// NavGridBrushGizmo
// ---------------------------------------------------------------------------

void NavGridBrushGizmo::Render(GizmoRenderContext& context)
{
    if (!Active)
        return;

    auto* gridMap = FindFirstGridMap(context.GetWorld());
    if (!gridMap)
        return;

    const auto& settings = gridMap->GetSettings();
    const float cellSize = settings.CellSize;

    // Convert brush center to cell coordinates
    uint32_t centerCellX = 0;
    uint32_t centerCellZ = 0;
    if (!gridMap->WorldToCell(BrushCenterX, BrushCenterZ, centerCellX, centerCellZ))
        return;

    const int32_t radius = static_cast<int32_t>(BrushRadius);

    // Yellow for cost mode, red for block, green for unblock
    Color color(1.0f, 1.0f, 0.2f, 1.0f);
    if (Mode == NavGridBrushMode::Block)
        color = Color(1.0f, 0.2f, 0.2f, 1.0f);
    else if (Mode == NavGridBrushMode::Unblock)
        color = Color(0.2f, 1.0f, 0.2f, 1.0f);

    const float yOffset = 0.05f; // slight lift above grid surface

    for (int32_t dz = -radius; dz <= radius; ++dz)
    {
        for (int32_t dx = -radius; dx <= radius; ++dx)
        {
            const int32_t cx = static_cast<int32_t>(centerCellX) + dx;
            const int32_t cz = static_cast<int32_t>(centerCellZ) + dz;

            if (cx < 0 || cz < 0)
                continue;

            const uint32_t cellX = static_cast<uint32_t>(cx);
            const uint32_t cellZ = static_cast<uint32_t>(cz);

            if (!gridMap->IsValidCell(cellX, cellZ))
                continue;

            float worldX = 0.0f;
            float worldZ = 0.0f;
            gridMap->CellToWorld(cellX, cellZ, worldX, worldZ);

            const float halfCell = cellSize * 0.5f;
            const float cellY = gridMap->GetCellHeight(cellX, cellZ) + yOffset;

            // Draw wireframe box for this cell
            context.DrawWireBox(Mathematics::Vector3(worldX, cellY, worldZ),
                                Mathematics::Vector3(halfCell, 0.01f, halfCell),
                                color, 1.5f);
        }
    }
}

// ---------------------------------------------------------------------------
// NavGridBrushTool
// ---------------------------------------------------------------------------

NavGridBrushTool::NavGridBrushTool(SceneViewController& owner)
    : m_Owner(owner)
{
}

void NavGridBrushTool::OnActivated()
{
    m_IsPainting = false;
    m_BrushGizmo.Active = true;
}

void NavGridBrushTool::OnDeactivated()
{
    if (m_IsPainting)
        FinalizeStroke();
    m_IsPainting = false;
    m_BrushGizmo.Active = false;
}

bool NavGridBrushTool::RaycastToGrid(const GizmoRay& ray, float& outWorldX, float& outWorldZ, float& outWorldY)
{
    // Default to Y=0 plane; could be refined to use grid origin Y
    float planeY = 0.0f;

    auto* gridMap = FindFirstGridMap(&m_Owner.GetWorld());
    if (gridMap)
        planeY = gridMap->GetSettings().OriginY;

    // Avoid division by near-zero when looking horizontally
    if (std::abs(ray.direction.y) < kRayDirectionEpsilon)
        return false;

    const float t = (planeY - ray.origin.y) / ray.direction.y;
    if (t < 0.0f)
        return false;

    outWorldX = ray.origin.x + ray.direction.x * t;
    outWorldZ = ray.origin.z + ray.direction.z * t;
    outWorldY = planeY;
    return true;
}

void NavGridBrushTool::OnPointerEvent(const ScenePointerEvent& event)
{
    float hitX = 0.0f;
    float hitZ = 0.0f;
    float hitY = 0.0f;

    const bool hitGrid = RaycastToGrid(event.ray, hitX, hitZ, hitY);

    // Always update brush gizmo position for preview
    if (hitGrid)
    {
        m_BrushGizmo.BrushCenterX = hitX;
        m_BrushGizmo.BrushCenterY = hitY;
        m_BrushGizmo.BrushCenterZ = hitZ;
        m_BrushGizmo.BrushRadius = m_BrushRadius;
        m_BrushGizmo.Mode = m_Mode;
        m_BrushGizmo.Active = true;
    }
    else
    {
        m_BrushGizmo.Active = false;
    }

    switch (event.phase)
    {
    case PointerPhase::Down:
        if (event.button == PointerButton::Left && hitGrid)
        {
            m_StrokeBeforeState.clear();
            m_StrokeCellsModified.clear();
            m_IsPainting = true;
            PaintAtPosition(hitX, hitZ);
        }
        break;

    case PointerPhase::Move:
        if (m_IsPainting && hitGrid)
        {
            PaintAtPosition(hitX, hitZ);
        }
        break;

    case PointerPhase::Up:
        if (m_IsPainting)
        {
            FinalizeStroke();
            m_IsPainting = false;
        }
        break;
    }
}

void NavGridBrushTool::PaintAtPosition(float worldX, float worldZ)
{
    auto* gridMap = FindFirstGridMap(&m_Owner.GetWorld());
    if (!gridMap)
        return;

    uint32_t centerCellX = 0;
    uint32_t centerCellZ = 0;
    if (!gridMap->WorldToCell(worldX, worldZ, centerCellX, centerCellZ))
        return;

    const int32_t radius = static_cast<int32_t>(m_BrushRadius);

    for (int32_t dz = -radius; dz <= radius; ++dz)
    {
        for (int32_t dx = -radius; dx <= radius; ++dx)
        {
            const int32_t cx = static_cast<int32_t>(centerCellX) + dx;
            const int32_t cz = static_cast<int32_t>(centerCellZ) + dz;

            if (cx < 0 || cz < 0)
                continue;

            const uint32_t cellX = static_cast<uint32_t>(cx);
            const uint32_t cellZ = static_cast<uint32_t>(cz);

            if (!gridMap->IsValidCell(cellX, cellZ))
                continue;

            // Capture before-state for undo if this cell hasn't been touched this stroke.
            uint64_t cellKey = (static_cast<uint64_t>(cellZ) << 32) | cellX;
            if (m_StrokeCellsModified.find(cellKey) == m_StrokeCellsModified.end())
            {
                NavGridBrushCellSnapshot before;
                before.CellX = cellX;
                before.CellZ = cellZ;
                before.Cost = gridMap->GetCellCost(cellX, cellZ);
                before.Blocked = gridMap->IsCellBlocked(cellX, cellZ);
                m_StrokeBeforeState.push_back(before);
                m_StrokeCellsModified.insert(cellKey);
            }

            switch (m_Mode)
            {
            case NavGridBrushMode::Cost:
                gridMap->SetCellCost(cellX, cellZ, m_CostValue);
                break;
            case NavGridBrushMode::Block:
                gridMap->SetCellBlocked(cellX, cellZ, true);
                break;
            case NavGridBrushMode::Unblock:
                gridMap->SetCellBlocked(cellX, cellZ, false);
                break;
            }
        }
    }
}

void NavGridBrushTool::FinalizeStroke()
{
    if (m_StrokeCellsModified.empty())
    {
        m_StrokeBeforeState.clear();
        m_StrokeCellsModified.clear();
        return;
    }

    auto* gridMap = FindFirstGridMap(&m_Owner.GetWorld());
    if (gridMap && m_UndoRedo)
    {
        std::vector<NavGridBrushCellSnapshot> afterState;
        afterState.reserve(m_StrokeBeforeState.size());
        for (const auto& before : m_StrokeBeforeState)
        {
            NavGridBrushCellSnapshot after;
            after.CellX = before.CellX;
            after.CellZ = before.CellZ;
            after.Cost = gridMap->GetCellCost(before.CellX, before.CellZ);
            after.Blocked = gridMap->IsCellBlocked(before.CellX, before.CellZ);
            afterState.push_back(after);
        }

        auto cmd = std::make_unique<GridPaintCommand>(
            m_Owner.GetWorld(), std::move(m_StrokeBeforeState), std::move(afterState));
        m_UndoRedo->CommitAlreadyApplied(std::move(cmd));
    }

    m_StrokeBeforeState.clear();
    m_StrokeCellsModified.clear();
}

void NavGridBrushTool::SaveGridToFile()
{
    auto* gridMap = FindFirstGridMap(&m_Owner.GetWorld());
    if (!gridMap)
    {
        Logger::Log::Warning("[NavGridBrushTool] No grid map to save");
        return;
    }

    auto savePath = Platform::SaveFile({}, "Navigation Grid", "*.navgrid");
    if (savePath.empty())
        return;

    if (savePath.extension() != ".navgrid")
        savePath += ".navgrid";

    const auto& settings = gridMap->GetSettings();
    const uint32_t cellCount = gridMap->GetCellCount();

    bool ok = NavGridAsset::SaveToFile(savePath, settings,
                                       gridMap->GetCostData(),
                                       gridMap->GetBlockedData(),
                                       cellCount);
    if (ok)
        Logger::Log::Info("[NavGridBrushTool] Saved grid to {}", savePath.string());
    else
        Logger::Log::Error("[NavGridBrushTool] Failed to save grid to {}", savePath.string());
}

void NavGridBrushTool::OnKeyEvent(const SceneKeyEvent& event)
{
    if (!event.pressed)
        return;

    // Ctrl+S to save grid
    constexpr uint32_t kKeyS = 83;
    if (event.ctrl && event.keyCode == kKeyS)
    {
        SaveGridToFile();
        return;
    }

    // '[' = decrease brush radius, ']' = increase brush radius
    // Key codes: '[' = 91, ']' = 93
    constexpr uint32_t kKeyLeftBracket  = 91;
    constexpr uint32_t kKeyRightBracket = 93;
    constexpr uint32_t kKey1 = 49;
    constexpr uint32_t kKey2 = 50;
    constexpr uint32_t kKey3 = 51;

    switch (event.keyCode)
    {
    case kKeyLeftBracket:
        if (m_BrushRadius > 0)
            --m_BrushRadius;
        m_BrushGizmo.BrushRadius = m_BrushRadius;
        break;

    case kKeyRightBracket:
        if (m_BrushRadius < kMaxBrushRadius)
            ++m_BrushRadius;
        m_BrushGizmo.BrushRadius = m_BrushRadius;
        break;

    case kKey1:
        m_Mode = NavGridBrushMode::Cost;
        break;

    case kKey2:
        m_Mode = NavGridBrushMode::Block;
        break;

    case kKey3:
        m_Mode = NavGridBrushMode::Unblock;
        break;
    }
}

void NavGridBrushTool::GatherGizmos(GizmoCollector& collector)
{
    collector.AddGizmo(&m_BrushGizmo);
}

} // namespace GameEngine::Editor::SceneTools
