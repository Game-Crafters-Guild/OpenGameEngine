#pragma once

#include <cstddef>
#include <string>

#include "Panels/BookmarksPanel.h"
#include "UndoRedo/IEditorCommand.h"

namespace GameEngine::Editor
{
// Undoable "remove bookmark" command. Do() removes; Undo() re-inserts at the same index.
class RemoveBookmarkCommand final : public IEditorCommand
{
public:
    RemoveBookmarkCommand(BookmarksPanel* panel, Bookmark bookmark, size_t index)
        : m_Panel(panel), m_Bookmark(std::move(bookmark)), m_Index(index)
    {
    }

    const char* GetName() const override { return "Remove Bookmark"; }

    void Do() override
    {
        if (!m_Panel)
            return;
        // Prefer stored index when valid so delete always works (avoids FindBookmarkIndex failures).
        size_t idx = (m_Index < m_Panel->GetBookmarkCount()) ? m_Index : m_Panel->FindBookmarkIndex(m_Bookmark);
        if (idx >= m_Panel->GetBookmarkCount())
            return;
        m_Index = idx;
        m_Panel->RemoveBookmark(idx);
    }

    void Undo() override
    {
        if (m_Panel)
            m_Panel->AddBookmarkAt(m_Bookmark, m_Index);
    }

    void Redo() override
    {
        if (!m_Panel)
            return;
        // Prefer stored index when valid (bookmark was re-inserted at m_Index by Undo).
        size_t idx = (m_Index < m_Panel->GetBookmarkCount()) ? m_Index : m_Panel->FindBookmarkIndex(m_Bookmark);
        if (idx >= m_Panel->GetBookmarkCount())
            return;
        m_Index = idx;
        m_Panel->RemoveBookmark(idx);
    }

private:
    BookmarksPanel* m_Panel = nullptr;
    Bookmark m_Bookmark;
    size_t m_Index = 0;
};

// Undoable "reorder bookmarks" command. Applied already on drag-end; Undo/Redo swap direction.
class ReorderBookmarksCommand final : public IEditorCommand
{
public:
    ReorderBookmarksCommand(BookmarksPanel* panel, int fromIndex, int toIndex)
        : m_Panel(panel), m_FromIndex(fromIndex), m_ToIndex(toIndex)
    {
    }

    const char* GetName() const override { return "Reorder Bookmarks"; }

    void Do() override
    {
        // Already applied by the panel when drag ends; no-op.
    }

    void Undo() override
    {
        if (m_Panel)
            m_Panel->ReorderBookmarks(m_ToIndex, m_FromIndex);
    }

    void Redo() override
    {
        if (m_Panel)
            m_Panel->ReorderBookmarks(m_FromIndex, m_ToIndex);
    }

private:
    BookmarksPanel* m_Panel = nullptr;
    int m_FromIndex = -1;
    int m_ToIndex = -1;
};

} // namespace GameEngine::Editor
