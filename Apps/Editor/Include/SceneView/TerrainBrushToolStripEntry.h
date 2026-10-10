#pragma once

#include "SceneView/SceneViewToolStripRegistry.h"

#include <functional>

namespace GameEngine::Editor
{

// The terrain brush's tool strip entry (id "terrainBrush"), available while the world
// supplied by the hosting view holds a terrain. The answer is cached per world and structural change,
// so the strip's per-frame read costs a few loads until an entity or component is added
// or removed.
SceneViewToolStripEntry MakeTerrainBrushToolStripEntry();

} // namespace GameEngine::Editor
