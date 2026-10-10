#pragma once

#include "UndoRedo/IEditorCommand.h"

#include "Assets/AssetRename.h"

#include <filesystem>
#include <functional>

namespace GameEngine
{
class AssetManager;
}

namespace GameEngine::Editor
{

// Undoable rename of one asset file plus, for a surface shader, its paired
// material: the pair keeps one stem and the material's `surfaceShader` path is
// rewritten so it still resolves. A material's `materialName` follows the stem
// it was created with. The registry rename keeps each GUID, so scene and
// material references to either file stay valid; a rewritten material is
// re-registered from disk in the same call so no new compile is enqueued
// against the shader's old name, and shader-error entries keyed to the old
// path are purged.
class RenameAssetFileCommand final : public IEditorCommand
{
  public:
    RenameAssetFileCommand(AssetRenamePlan plan,
                           AssetManager* assets,
                           std::function<void(const std::filesystem::path& renamedTo)> onRenamed);

    const char* GetName() const override { return "Rename Asset"; }

    void Do() override;
    void Undo() override;
    void Redo() override { Do(); }

  private:
    void Apply(const std::filesystem::path& from,
               const std::filesystem::path& to,
               const std::filesystem::path& companionFrom,
               const std::filesystem::path& companionTo);
    bool RenameOne(const std::filesystem::path& from, const std::filesystem::path& to);
    void ReloadMaterial(const std::filesystem::path& materialPath);
    void PurgeShaderErrors(const std::filesystem::path& oldPath);

    AssetRenamePlan m_Plan;
    AssetManager* m_Assets = nullptr;
    std::function<void(const std::filesystem::path&)> m_OnRenamed;
};

} // namespace GameEngine::Editor
