#pragma once

#include "UndoRedo/IEditorCommand.h"

#include "AssetCore/GUID.h"

#include <filesystem>
#include <functional>
#include <string>

namespace GameEngine::Editor
{

// One undoable write of an asset metadata key — the import settings an asset
// inspector edits. These rows write into a store file that ships with the
// asset's package and is tracked by that package's repository, so an edit made
// by mistake has to be reversible the same way every other inspector field is.
//
// Undo restores the key's previous value; a key the asset had no value for goes
// back to the empty string, which every reader of these keys treats the same
// way as absent (the parse fails and the caller keeps its default).
//
// The command holds the asset's GUID rather than the path it was built from:
// the registry answers where that asset is now, so a rename or a move between
// the edit and the undo still undoes the edit instead of writing to a path
// nothing answers to.
class SetAssetMetaValueCommand final : public IEditorCommand
{
  public:
    // `write` is the inspector's own write path for this key — the store write
    // plus whatever reload that setting needs — so undo goes back through
    // exactly the same path instead of a second one that could drift from it.
    // It is handed the asset's current path, which is what the metadata API
    // takes. `onWritten` re-presents the panel, so the controls show what the
    // store holds after an undo rather than the value the user last picked.
    SetAssetMetaValueCommand(const std::filesystem::path& assetPath,
                             std::string metaKey,
                             std::string newValue,
                             std::function<void(const std::filesystem::path&, const std::string&)> write,
                             std::function<void()> onWritten);

    const char* GetName() const override { return "Change Import Setting"; }
    const char* GetTypeName() const override { return "SetAssetMetaValueCommand"; }

    void Do() override;
    void Undo() override;

  private:
    void Write(const std::string& value);
    // Where the registry says this asset is now. False when the GUID no longer
    // resolves — an asset deleted between the edit and the undo — and the write
    // is skipped rather than aimed at a path nothing answers to.
    bool TryResolveCurrentPath(std::filesystem::path& outPath) const;

    GUID m_AssetGuid;
    std::string m_MetaKey;
    std::string m_NewValue;
    std::string m_PreviousValue;
    // The previous value is captured on the first Do() only: a redo must put
    // back the value this command wrote, not the one the undo just restored.
    bool m_PreviousValueCaptured = false;
    std::function<void(const std::filesystem::path&, const std::string&)> m_Write;
    std::function<void()> m_OnWritten;
};

} // namespace GameEngine::Editor
