#include "Terrain/RawHeightmapSettingsCommit.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"
#include "TerrainECS/RawHeightmap.h"
#include "TerrainECS/TerrainService.h"
#include "UndoRedo/SetAssetMetaValueCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace GameEngine
{
namespace
{

// The store write plus the re-decode, the one path the edit, its undo and its redo all take.
void WriteSampleCount(const std::filesystem::path& path, const GUID& heightmap, const char* key,
                      const std::string& value)
{
    if (!EngineCore::GetInstance().GetAssetManager().GetRegistry().SetMetaValue(path, key, value))
        return;
    if (auto* terrain = TerrainECS::TerrainService::TryGet())
        terrain->EnqueueAssetInvalidation(heightmap);
}

} // namespace

void CommitRawHeightmapSampleCount(Editor::UndoRedoService* undo, const std::filesystem::path& path,
                                   const GUID& heightmap, const char* key, uint32 value,
                                   std::function<void()> onWritten)
{
    // Compared as sample counts, so "", "0" and absent are all the same unset value.
    std::string storedText;
    std::optional<std::string> stored;
    if (EngineCore::GetInstance().GetAssetManager().GetRegistry().TryGetMetaValue(path, key, storedText))
        stored = std::move(storedText);
    uint32 storedCount = 0;
    if (TerrainECS::ParseRawHeightmapSampleCount(key, stored, storedCount).empty() && storedCount == value)
        return;

    const std::string text = std::to_string(value);
    if (!undo)
    {
        WriteSampleCount(path, heightmap, key, text);
        if (onWritten)
            onWritten();
        return;
    }
    undo->Execute(std::make_unique<Editor::SetAssetMetaValueCommand>(
        path, key, text,
        [heightmap, key](const std::filesystem::path& current, const std::string& v)
        { WriteSampleCount(current, heightmap, key, v); },
        std::move(onWritten)));
}

} // namespace GameEngine
