#pragma once

namespace GameEngine::Editor
{

// Registers DDGIVolume's editor-side traits (inspector section name + icon,
// hierarchy row icon). Called once at editor startup; see
// EditorComponentTraits.h for why the registry is the seam rather than another
// branch in the inspector/hierarchy built-in chains.
void RegisterDDGIVolumeComponentTraits();

} // namespace GameEngine::Editor
