#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "UI/EditorTags.h"
#include "UndoRedo/IEditorCommand.h"

namespace GameEngine::Editor
{
// Undoable "remove project tag" command. Do() removes the tag at index and optionally strips it
// from all assets' "tags" meta; Undo() re-adds the tag to the list and re-applies it to assets
// that had it (when getPathsWithTag and restoreTagToPaths are provided).
class RemoveTagCommand final : public IEditorCommand
{
public:
    /// getPathsWithTag: if set, called before stripping to record which paths had the tag (for Undo).
    /// onStripFromAssets: if set, called with the tag name to remove it from all assets' tags meta.
    /// restoreTagToPaths: if set, called in Undo with (tagName, paths) to re-apply the tag to those assets.
    RemoveTagCommand(GameEngine::EditorTagDefinition tag, size_t index,
                     std::function<void()> onChanged = nullptr,
                     std::function<void(const std::string&)> onStripFromAssets = nullptr,
                     std::function<std::vector<std::filesystem::path>(const std::string&)> getPathsWithTag = nullptr,
                     std::function<void(const std::string&, const std::vector<std::filesystem::path>&)> restoreTagToPaths = nullptr)
        : m_Tag(std::move(tag)), m_Index(index), m_OnChanged(std::move(onChanged)),
          m_OnStripFromAssets(std::move(onStripFromAssets)),
          m_GetPathsWithTag(std::move(getPathsWithTag)),
          m_RestoreTagToPaths(std::move(restoreTagToPaths))
    {
    }

    const char* GetName() const override { return "Remove Tag"; }

    void Do() override
    {
        if (m_GetPathsWithTag && !m_Tag.Name.empty())
            m_PathsThatHadTag = m_GetPathsWithTag(m_Tag.Name);
        std::vector<GameEngine::EditorTagDefinition> tags = GameEngine::EditorTags::Load();
        if (m_Index < tags.size())
        {
            tags.erase(tags.begin() + static_cast<std::ptrdiff_t>(m_Index));
            GameEngine::EditorTags::Save(tags);
        }
        if (m_OnStripFromAssets && !m_Tag.Name.empty())
            m_OnStripFromAssets(m_Tag.Name);
        // Do not call m_OnChanged here: caller refreshes after Execute() to avoid destroying
        // the delete button while still inside its click handler.
    }

    void Undo() override
    {
        std::vector<GameEngine::EditorTagDefinition> tags = GameEngine::EditorTags::Load();
        size_t insertAt = (m_Index <= tags.size()) ? m_Index : tags.size();
        tags.insert(tags.begin() + static_cast<std::ptrdiff_t>(insertAt), m_Tag);
        GameEngine::EditorTags::Save(tags);
        if (m_RestoreTagToPaths && !m_Tag.Name.empty() && !m_PathsThatHadTag.empty())
            m_RestoreTagToPaths(m_Tag.Name, m_PathsThatHadTag);
        if (m_OnChanged)
            m_OnChanged();
    }

    void Redo() override
    {
        std::vector<GameEngine::EditorTagDefinition> tags = GameEngine::EditorTags::Load();
        if (m_Index < tags.size())
        {
            tags.erase(tags.begin() + static_cast<std::ptrdiff_t>(m_Index));
            GameEngine::EditorTags::Save(tags);
        }
        if (m_OnStripFromAssets && !m_Tag.Name.empty())
            m_OnStripFromAssets(m_Tag.Name);
        if (m_OnChanged)
            m_OnChanged();
    }

private:
    GameEngine::EditorTagDefinition m_Tag;
    size_t m_Index = 0;
    std::vector<std::filesystem::path> m_PathsThatHadTag;
    std::function<void()> m_OnChanged;
    std::function<void(const std::string&)> m_OnStripFromAssets;
    std::function<std::vector<std::filesystem::path>(const std::string&)> m_GetPathsWithTag;
    std::function<void(const std::string&, const std::vector<std::filesystem::path>&)> m_RestoreTagToPaths;
};

} // namespace GameEngine::Editor
