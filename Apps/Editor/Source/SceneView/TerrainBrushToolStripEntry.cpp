#include "SceneView/TerrainBrushToolStripEntry.h"

#include "SceneView/TerrainBrushTool.h"
#include "SceneView/TerrainPresence.h"
#include "SceneViewController.h"
#include "UI/EditorIcons.h"

#include <memory>
#include <utility>

namespace GameEngine::Editor
{

namespace
{

std::unique_ptr<SceneTools::ISceneTool> CreateTerrainBrushToolForView(SceneViewController& owner)
{
    auto tool = std::make_unique<SceneTools::TerrainBrushTool>(owner);
    tool->SetUndoRedoService(owner.GetUndoRedoService());
    tool->SetChangeNotifications(owner.GetChangeNotifications());
    return tool;
}

} // namespace

SceneViewToolStripEntry MakeTerrainBrushToolStripEntry()
{
    SceneViewToolStripEntry entry;
    entry.Id = "terrainBrush";
    entry.Tooltip = "Terrain Brush (sculpt/paint zones)";
    entry.Icon = EditorIcons::kTerrain;
    entry.CreateTool = &CreateTerrainBrushToolForView;
    entry.UnavailableReason = "Terrain Brush needs a terrain in the scene: add a Terrain entity first";
    entry.IsAvailable = [presence = std::make_shared<TerrainPresence>()](ECS::World* world) {
        return presence->HasTerrain(world);
    };
    return entry;
}

} // namespace GameEngine::Editor
