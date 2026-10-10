#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace GameEngine {

class AssetManager;
class Label;
class TextField;
class UIElement;
namespace Editor { class UndoRedoService; }

/// One inline-rename session in the Assets browser: the item's title label is
/// swapped for a text field pre-filled with the stem, the name is validated as
/// the user types (an invalid name is marked and carries the reason as its
/// tooltip), and Enter executes an undoable rename. Escape, or committing an
/// unchanged name, ends the session without touching disk; losing focus commits.
class AssetRenameController
{
public:
    AssetRenameController() = default;
    ~AssetRenameController();
    AssetRenameController(const AssetRenameController&) = delete;
    AssetRenameController& operator=(const AssetRenameController&) = delete;

    /// Fired after every apply of the rename — undo and redo included — with the
    /// path the asset has after that apply.
    using OnRenamed = std::function<void(const std::filesystem::path& renamedTo)>;

    /// `title` is the label the field replaces while editing; its parent hosts
    /// the field. Keyboard focus returns to `focusOnEnd` (the view the item lives
    /// in) when the session ends, so navigation keys keep working. A session
    /// already editing `itemPath` is left as it is; any other session in
    /// progress is cancelled first.
    void Begin(Label* title,
               UIElement* focusOnEnd,
               const std::filesystem::path& itemPath,
               Editor::UndoRedoService* undo,
               AssetManager* assets,
               OnRenamed onRenamed);

private:
    void Commit();
    void End();
    void ApplyValidation(const std::string& stem);

    Label* m_Title = nullptr;
    UIElement* m_FocusOnEnd = nullptr;
    TextField* m_Field = nullptr;
    std::filesystem::path m_ItemPath;
    Editor::UndoRedoService* m_Undo = nullptr;
    AssetManager* m_Assets = nullptr;
    OnRenamed m_OnRenamed;
};

} // namespace GameEngine
