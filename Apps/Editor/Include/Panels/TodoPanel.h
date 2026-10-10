#pragma once

#include "UI/Controls/DockPanel.h"
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <cstddef>
#include <filesystem>

namespace GameEngine {

namespace Editor { class UndoRedoService; }

class UIElement;
class ScrollView;
class Label;
class Button;
class TextField;
struct EditorContext;

struct TodoItem {
    std::string Id;          // Unique identifier
    std::string Text;        // Todo text content
    bool Completed = false;  // Completion state
    int Order = 0;           // Sort order within section

    bool operator==(const TodoItem& other) const {
        return Id == other.Id;
    }
};

class TodoPanel : public DockPanel {
public:
    std::string_view DeclaredTabIconClass() const override { return "checkmark-icon"; }

    TodoPanel();
    ~TodoPanel() override;
    
    // Set editor context
    void SetEditorContext(EditorContext* context);

    // Set undo service for undoable remove/reorder (set by EditorApplication).
    void SetUndoRedoService(Editor::UndoRedoService* undo) { m_Undo = undo; }

    // Add a new todo item
    void AddTodo(const std::string& text);

    // Restore a todo item (used by undo of RemoveTodo).
    void AddTodoItem(const TodoItem& item);
    
    // Remove a todo by index (within its section)
    void RemoveTodo(const std::string& id);

    // Reorder todos (used by undo/redo and drag-and-drop).
    void ReorderTodos(const std::string& fromId, int toIndex, bool inCompletedSection);
    
    // Toggle completion state
    void ToggleTodoComplete(const std::string& id);
    
    // Update todo text
    void UpdateTodoText(const std::string& id, const std::string& newText);
    
    // Override to refresh on first layout
    void OnPostLayout() override;

private:
    void BuildUI();
    void RebuildTodoRows();
    void CreateTodoRow(const TodoItem& todo, UIElement* container, bool isCompleted);
    void SetupDragAndDrop(UIElement* row, UIElement* dragHandle, const std::string& todoId);
    void CancelDrag(); // Cancel any active drag operation
    void SaveTodos();
    void LoadTodos();
    void SaveRowHeight();
    void LoadRowHeight();
    std::string GenerateUniqueId();
    
    // UI elements
    UIElement* m_MainContainer = nullptr;
    UIElement* m_ContentContainer = nullptr;
    UIElement* m_PendingContainer = nullptr;
    UIElement* m_CompletedContainer = nullptr;
    ScrollView* m_ScrollView = nullptr;
    TextField* m_AddField = nullptr;
    UIElement* m_InsertionIndicator = nullptr;
    
    // Search
    UIElement* m_SearchBar = nullptr;
    TextField* m_SearchField = nullptr;
    std::string m_SearchQuery;
    std::string m_SearchFieldScope{"all"};
    
    // Data
    std::vector<TodoItem> m_Todos;
    EditorContext* m_Context = nullptr;
    Editor::UndoRedoService* m_Undo = nullptr;
    
    // Track current workspace root
    std::filesystem::path m_CurrentWorkspaceRoot;
    
    // Drag state
    UIElement* m_DraggedRow = nullptr;
    std::string m_DragStartId;
    int m_DragTargetIndex = -1;
    bool m_DragActive = false;
    float m_DragStartY = 0.0f;
    bool m_DragInCompletedSection = false;
    
    // Selection state
    std::string m_SelectedId;
    
    // Row height (adjustable via Ctrl+Scroll)
    float m_RowHeight = 32.0f;
    
    // Track if initial refresh has been done
    bool m_InitialRefreshDone = false;
    
    // ID counter for generating unique IDs
    int m_NextId = 1;
    
    // Guard against concurrent rebuilds
    bool m_RebuildPending = false;
    
    // Flag to request rebuild on next safe opportunity (OnPostLayout)
    bool m_NeedsRebuild = false;
    bool m_NeedsSave = false;
    bool m_AddFieldShowingPlaceholder = false;
    
    // Generation counter - increments on each rebuild to invalidate stale pointers
    uint32_t m_RebuildGeneration = 0;
};

} // namespace GameEngine
