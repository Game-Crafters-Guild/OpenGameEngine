#pragma once

#include <string>

#include "Panels/TodoPanel.h"
#include "UndoRedo/IEditorCommand.h"

namespace GameEngine::Editor
{
// Undoable "remove todo" command. Do() removes; Undo() re-inserts the item.
class RemoveTodoCommand final : public IEditorCommand
{
public:
    RemoveTodoCommand(TodoPanel* panel, TodoItem item)
        : m_Panel(panel), m_Item(std::move(item))
    {
    }

    const char* GetName() const override { return "Remove Todo"; }

    void Do() override
    {
        if (m_Panel)
            m_Panel->RemoveTodo(m_Item.Id);
    }

    void Undo() override
    {
        if (m_Panel)
            m_Panel->AddTodoItem(m_Item);
    }

    void Redo() override
    {
        if (m_Panel)
            m_Panel->RemoveTodo(m_Item.Id);
    }

private:
    TodoPanel* m_Panel = nullptr;
    TodoItem m_Item;
};

// Undoable "reorder todos" command. Applied already on drag-end; Undo/Redo swap direction.
// fromIndex and toIndex are indices within the section (pending or completed).
class ReorderTodosCommand final : public IEditorCommand
{
public:
    ReorderTodosCommand(TodoPanel* panel,
                        const std::string& todoId,
                        int fromIndexInSection,
                        int toIndexInSection,
                        bool inCompletedSection)
        : m_Panel(panel)
        , m_TodoId(todoId)
        , m_FromIndex(fromIndexInSection)
        , m_ToIndex(toIndexInSection)
        , m_InCompletedSection(inCompletedSection)
    {
    }

    const char* GetName() const override { return "Reorder Todos"; }

    void Do() override
    {
        // Already applied by the panel when drag ends; no-op.
    }

    void Undo() override
    {
        if (m_Panel)
            m_Panel->ReorderTodos(m_TodoId, m_FromIndex, m_InCompletedSection);
    }

    void Redo() override
    {
        if (m_Panel)
            m_Panel->ReorderTodos(m_TodoId, m_ToIndex, m_InCompletedSection);
    }

private:
    TodoPanel* m_Panel = nullptr;
    std::string m_TodoId;
    int m_FromIndex = -1;
    int m_ToIndex = -1;
    bool m_InCompletedSection = false;
};

} // namespace GameEngine::Editor
