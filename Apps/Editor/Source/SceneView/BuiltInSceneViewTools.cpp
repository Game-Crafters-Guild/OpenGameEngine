#include "SceneView/BuiltInSceneViewTools.h"

#include "SceneView/SceneViewToolStripRegistry.h"
#include "SceneView/SplineToolStripEntry.h"
#include "SceneView/TerrainBrushToolStripEntry.h"

namespace GameEngine::Editor
{

void RegisterBuiltInSceneViewTools()
{
    SceneViewToolStripRegistry& registry = SceneViewToolStripRegistry::Get();
    registry.Register(MakeSplineToolStripEntry());
    registry.Register(MakeTerrainBrushToolStripEntry());
}

} // namespace GameEngine::Editor
