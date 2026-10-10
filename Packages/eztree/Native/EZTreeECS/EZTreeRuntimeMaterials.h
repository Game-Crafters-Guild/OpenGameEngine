#pragma once

// Default Tree Generator material documents. Extraction uses these when a
// tree has no authored material; keep them aligned with the package
// .material files so bark, leaves, and wind match either path.

#include "Rendering/Materials/MaterialDocument.h"

namespace GameEngine::EZTreeECS
{

MaterialDocument MakeBarkRuntimeMaterialShape();
MaterialDocument MakeLeafRuntimeMaterialShape();
MaterialDocument MakeTrellisRuntimeMaterialShape();

} // namespace GameEngine::EZTreeECS
