#pragma once

#include "SceneView/SceneViewGizmos.h"
#include "SceneView/SceneViewTools.h"

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace GameEngine {

class SceneViewController;

namespace Editor { class UndoRedoService; }

namespace Editor::SceneTools {

// Paint modes for the brush
enum class NavGridBrushMode : uint8_t
{
    Cost,     // Set cell cost value
    Block,    // Toggle blocked state
    Unblock   // Clear blocked state
};

// Cell state captured for paint strokes and undo/redo commands.
struct NavGridBrushCellSnapshot
{
    uint32_t CellX;
    uint32_t CellZ;
    float Cost;
    bool Blocked;
};

// Gizmo that renders a brush preview (highlighted cells under cursor)
class NavGridBrushGizmo : public IGizmo
{
public:
    void Render(GizmoRenderContext& context) override;

    // Updated by the tool each frame
    float BrushCenterX = 0.0f;
    float BrushCenterY = 0.0f;
    float BrushCenterZ = 0.0f;
    uint32_t BrushRadius = 0; // 0 = single cell, 1 = 3x3, 2 = 5x5
    NavGridBrushMode Mode = NavGridBrushMode::Cost;
    bool Active = false;
};

class NavGridBrushTool : public ISceneTool
{
public:
    explicit NavGridBrushTool(SceneViewController& owner);

    const char* GetName() const override { return "NavGridBrushTool"; }
    void OnActivated() override;
    void OnDeactivated() override;
    void OnPointerEvent(const ScenePointerEvent& event) override;
    void OnKeyEvent(const SceneKeyEvent& event) override;
    void GatherGizmos(GizmoCollector& collector) override;

    // Save the current grid cost/blocked data to a .navgrid file via a native save dialog.
    void SaveGridToFile();

    // Optional editor undo/redo service (not owned).
    void SetUndoRedoService(Editor::UndoRedoService* undo) { m_UndoRedo = undo; }

private:
    // Raycast to find the grid cell under the mouse
    bool RaycastToGrid(const GizmoRay& ray, float& outWorldX, float& outWorldZ, float& outWorldY);

    // Paint cells within brush radius at the given world position
    void PaintAtPosition(float worldX, float worldZ);

    // Finalize the current paint stroke and push an undo command.
    void FinalizeStroke();

    SceneViewController& m_Owner;
    NavGridBrushGizmo m_BrushGizmo;
    NavGridBrushMode m_Mode = NavGridBrushMode::Cost;
    float m_CostValue = 5.0f;    // cost to paint in Cost mode
    uint32_t m_BrushRadius = 0;  // 0=1x1, 1=3x3, 2=5x5
    bool m_IsPainting = false;

    // Undo/redo service (not owned).
    Editor::UndoRedoService* m_UndoRedo = nullptr;

    // Stroke tracking for undo/redo.
    std::vector<NavGridBrushCellSnapshot> m_StrokeBeforeState;
    std::unordered_set<uint64_t> m_StrokeCellsModified;
};

} // namespace Editor::SceneTools
} // namespace GameEngine
