#pragma once

#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <filesystem>
#include <functional>

namespace GameEngine::Editor
{
class UndoRedoService;
}

namespace GameEngine
{

// Stores one of a raw heightmap's two grid settings (`key` is TerrainECS::kRawHeightmapWidthKey or
// kRawHeightmapHeightKey; 0 is unset) as one undoable import-setting edit, and has every terrain
// using the heightmap decode it again, on the edit and on its undo and redo. A value the store
// already holds is not an edit: it would re-decode the whole heightmap and put a no-op on the undo
// stack, so nothing happens. A store that refuses the write leaves the terrains as they are.
// `onWritten` re-presents the inspector after each write. Without an undo service the write still
// lands, unrecorded.
void CommitRawHeightmapSampleCount(Editor::UndoRedoService* undo, const std::filesystem::path& path,
                                   const GUID& heightmap, const char* key, uint32 value,
                                   std::function<void()> onWritten);

} // namespace GameEngine
