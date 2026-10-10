#include "Panels/TodoPanel.h"
#include "EditorContext.h"
#include "UndoRedo/TodoCommands.h"
#include "UndoRedo/UndoRedoService.h"
#include "Editor/Settings/SettingsStore.h"
#include "Editor/EditorPaths.h"
#include "Core/Engine.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/StyleProperties.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/TextField.h"
#include "UI/PanelSearchBar.h"
#include "Panels/SettingsPanel.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <cmath>
#include <sstream>
#include <chrono>

namespace GameEngine {

namespace {
constexpr const char* kNewTodoPlaceholder = "New todo item";
}

TodoPanel::TodoPanel()
    : DockPanel("Todo")
{
    BuildUI();
    m_CurrentWorkspaceRoot = std::filesystem::path();
}

TodoPanel::~TodoPanel()
{
    if (m_SearchBar)
        SettingsPanel::UnregisterSearchBar(m_SearchBar);
}

void TodoPanel::SetEditorContext(EditorContext* context)
{
    m_Context = context;
    m_Undo = (context && context->UndoRedo) ? context->UndoRedo : nullptr;
    
    auto& engine = EngineCore::GetInstance();
    std::filesystem::path newWorkspaceRoot = engine.GetWorkspaceRoot();
    
    bool workspaceChanged = false;
    if (m_CurrentWorkspaceRoot.empty() && !newWorkspaceRoot.empty())
    {
        workspaceChanged = true;
    }
    else if (!m_CurrentWorkspaceRoot.empty() && !newWorkspaceRoot.empty())
    {
        std::string currentStr = m_CurrentWorkspaceRoot.string();
        std::string newStr = newWorkspaceRoot.string();
        while (!currentStr.empty() && (currentStr.back() == '/' || currentStr.back() == '\\'))
            currentStr.pop_back();
        while (!newStr.empty() && (newStr.back() == '/' || newStr.back() == '\\'))
            newStr.pop_back();
        workspaceChanged = (currentStr != newStr);
    }
    
    if (workspaceChanged || !newWorkspaceRoot.empty())
    {
        m_CurrentWorkspaceRoot = newWorkspaceRoot;
        LoadTodos();
        LoadRowHeight();
        
        if (GetOwnerManager())
        {
            PostAction([this]()
            {
                RebuildTodoRows();
            });
        }
        else
        {
            RebuildTodoRows();
        }
    }
}

void TodoPanel::OnPostLayout()
{
    if (!m_InitialRefreshDone && GetOwnerManager() && m_Context)
    {
        m_InitialRefreshDone = true;
        LoadTodos();
        LoadRowHeight();
        m_NeedsRebuild = true;
    }
    
    // Handle deferred rebuild at a safe point (after layout, before next frame)
    if (m_NeedsRebuild && m_PendingContainer && m_CompletedContainer)
    {
        m_NeedsRebuild = false;
        RebuildTodoRows();
    }
    
    if (m_NeedsSave)
    {
        m_NeedsSave = false;
        SaveTodos();
    }
}

void TodoPanel::BuildUI()
{
    // Main container
    auto container = std::make_unique<UIElement>();
    container->AddClass("todo-panel");
    m_MainContainer = container.get();
    
    // Add new todo input area at the top
    auto addContainer = std::make_unique<UIElement>();
    addContainer->AddClass("todo-add-container");
    
    auto addField = std::make_unique<TextField>();
    addField->AddClass("todo-add-field");
    addField->AddClass("todo-add-placeholder");
    addField->SetValue(kNewTodoPlaceholder);
    m_AddFieldShowingPlaceholder = true;
    m_AddField = addField.get();

    addField->RegisterEventHandler(kEventFocusIn, [this](UIEvent&)
    {
        if (m_AddField && m_AddFieldShowingPlaceholder)
        {
            m_AddField->SetValue("");
            m_AddField->RemoveClass("todo-add-placeholder");
            m_AddFieldShowingPlaceholder = false;
        }
    });

    addField->RegisterEventHandler(kEventFocusOut, [this](UIEvent&)
    {
        if (m_AddField && m_AddField->GetValue().empty())
        {
            m_AddField->SetValue(kNewTodoPlaceholder);
            m_AddField->AddClass("todo-add-placeholder");
            m_AddFieldShowingPlaceholder = true;
        }
    });
    
    // Handle Enter key to add todo
    addField->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e)
    {
        if ((e.Key == Input::kKeyCode_Enter || e.Key == Input::kKeyCode_NumPadEnter) && m_AddField)
        {
            std::string text = m_AddField->GetValue();
            if (!m_AddFieldShowingPlaceholder && !text.empty())
            {
                AddTodo(text);
                m_AddField->SetValue(kNewTodoPlaceholder);
                m_AddField->AddClass("todo-add-placeholder");
                m_AddFieldShowingPlaceholder = true;
            }
            e.Stop();
        }
    });
    
    auto addBtn = std::make_unique<Button>();
    addBtn->AddClass("todo-add-button");
    addBtn->SetText(""); // Icon set via CSS
    addBtn->SetTooltip("Add a new todo item");
    addBtn->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e)
    {
        if (e.Button == 0 && m_AddField)
        {
            std::string text = m_AddField->GetValue();
            if (m_AddFieldShowingPlaceholder || text.empty())
                text = kNewTodoPlaceholder;

            if (!text.empty())
            {
                AddTodo(text);
                m_AddField->SetValue(kNewTodoPlaceholder);
                m_AddField->AddClass("todo-add-placeholder");
                m_AddFieldShowingPlaceholder = true;
            }
            e.Stop();
        }
    });
    
    addContainer->AddChild(std::move(addField));
    addContainer->AddChild(std::move(addBtn));
    container->AddChild(std::move(addContainer));
    
    // Search bar
    auto built = BuildPanelSearchBar(
        "todo-search-field",
        []() { return SettingsPanel::GetSearchBarsVisible(); },
        [this](const std::string& value)
        {
            m_SearchQuery = value;
            m_NeedsRebuild = true; // Deferred rebuild
        },
        [this](const std::string& value)
        {
            m_SearchQuery = value;
            m_NeedsRebuild = true; // Deferred rebuild
        },
        {{"all", "All fields"}, {"text", "Text"}, {"status", "Status"}},
        [this](const std::string& scope)
        {
            m_SearchFieldScope = scope;
            m_NeedsRebuild = true;
        });
    m_SearchBar = built.RootPtr;
    m_SearchField = built.FieldPtr;
    container->AddChild(std::move(built.Root));
    SettingsPanel::RegisterSearchBar(m_SearchBar, built.IconPtr);
    
    // Scroll view for todo lists
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->SetId("todo-scroll");
    scrollView->AddClass("todo-list");
    scrollView->Overrides().SetCustomNumber(StringId("--ui_scrollview_measure_horizontal"), 0.0f);
    m_ScrollView = scrollView.get();
    
    // Handle click on background to clear selection
    scrollView->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e)
    {
        if (e.Button == 0)
        {
            UIElement* target = e.Target;
            // Only handle if clicking directly on the scroll view or content container
            if (target && (target->HasClass("todo-list") || 
                          target->HasClass("todo-content") ||
                          target->HasClass("todo-list-container") ||
                          target->HasClass("todo-section")))
            {
                m_SelectedId.clear();
                m_NeedsRebuild = true;
                e.Stop();
            }
        }
    });
    
    // Handle Ctrl+Scroll to adjust row height
    scrollView->RegisterEventHandler(kEventScroll, [this, scrollViewPtr = scrollView.get()](UIEvent& e)
    {
        if (e.CurrentTarget != scrollViewPtr)
            return;
        
        const bool primaryMod = Input::IsPrimaryShortcutModifier(e.Mods);        
        if (primaryMod && e.ScrollY != 0.0f)
        {
            const float step = 2.0f;
            float newHeight = m_RowHeight - e.ScrollY * step;
            newHeight = std::max(20.0f, std::min(256.0f, newHeight));
            
            if (std::abs(newHeight - m_RowHeight) > 0.1f)
            {
                m_RowHeight = newHeight;
                SaveRowHeight();
                
                if (m_PendingContainer)
                {
                    m_PendingContainer->PostAction([this]()
                    {
                        RebuildTodoRows();
                    });
                }
            }
            
            e.Stop();
        }
    });
    
    // Content container inside scroll view
    auto contentContainer = std::make_unique<UIElement>();
    contentContainer->AddClass("todo-content");
    m_ContentContainer = contentContainer.get();
    
    // Pending todos section
    auto pendingSection = std::make_unique<UIElement>();
    pendingSection->AddClass("todo-section");
    pendingSection->AddClass("todo-section-pending");
    
    auto pendingHeader = std::make_unique<Label>();
    pendingHeader->AddClass("todo-section-header");
    pendingHeader->SetText("To Do");
    pendingSection->AddChild(std::move(pendingHeader));
    
    auto pendingContainer = std::make_unique<UIElement>();
    pendingContainer->SetId("todo-pending-container");
    pendingContainer->AddClass("todo-list-container");
    m_PendingContainer = pendingContainer.get();
    pendingSection->AddChild(std::move(pendingContainer));
    
    contentContainer->AddChild(std::move(pendingSection));
    
    // Completed todos section
    auto completedSection = std::make_unique<UIElement>();
    completedSection->AddClass("todo-section");
    completedSection->AddClass("todo-section-completed");
    
    auto completedHeader = std::make_unique<Label>();
    completedHeader->AddClass("todo-section-header");
    completedHeader->SetText("Completed");
    completedSection->AddChild(std::move(completedHeader));
    
    auto completedContainer = std::make_unique<UIElement>();
    completedContainer->SetId("todo-completed-container");
    completedContainer->AddClass("todo-list-container");
    m_CompletedContainer = completedContainer.get();
    completedSection->AddChild(std::move(completedContainer));
    
    contentContainer->AddChild(std::move(completedSection));
    
    // Insertion indicator for drag-and-drop
    auto indicator = std::make_unique<UIElement>();
    indicator->SetId("todo-insertion-indicator");
    indicator->Overrides().Set(Style::Display, DisplayMode::None);
    m_InsertionIndicator = indicator.get();
    contentContainer->AddChild(std::move(indicator));
    
    scrollView->AddContent(std::move(contentContainer));
    container->AddChild(std::move(scrollView));
    
    AddChild(std::move(container));
}

void TodoPanel::RebuildTodoRows()
{
    if (!m_PendingContainer || !m_CompletedContainer)
        return;
    
    if (UIElement::IsInEventDispatch())
    {
        m_PendingContainer->PostAction([this]() { RebuildTodoRows(); });
        return;
    }
    
    // Increment generation to invalidate any captured pointers in event handlers
    ++m_RebuildGeneration;
    
    // Clear any active drag state since pointers will be invalidated
    m_DraggedRow = nullptr;
    m_DragActive = false;
    m_DragStartId.clear();
    m_DragTargetIndex = -1;
    
    // Clear existing rows
    auto clearContainer = [](UIElement* container) {
        std::vector<UIElement*> toRemove;
        for (const auto& child : container->GetChildren())
        {
            if (child->HasClass("todo-row"))
                toRemove.push_back(child.get());
        }
        for (UIElement* child : toRemove)
            container->RemoveChild(child);
    };
    
    clearContainer(m_PendingContainer);
    clearContainer(m_CompletedContainer);
    
    // Sort todos by order
    std::sort(m_Todos.begin(), m_Todos.end(),
        [](const TodoItem& a, const TodoItem& b) { return a.Order < b.Order; });
    
    // Filter by search query
    auto matchesSearch = [this](const TodoItem& todo) -> bool {
        if (m_SearchQuery.empty())
            return true;
        
        std::string searchLower = m_SearchQuery;
        std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(),
            [](unsigned char c) { return std::tolower(c); });
        
        const std::string status = todo.Completed ? "completed" : "pending";
        std::string searchable;
        if (m_SearchFieldScope == "status")
            searchable = status;
        else if (m_SearchFieldScope == "text")
            searchable = todo.Text;
        else
            searchable = todo.Text + " " + status;
        std::transform(searchable.begin(), searchable.end(), searchable.begin(),
            [](unsigned char c) { return std::tolower(c); });
        
        return searchable.find(searchLower) != std::string::npos;
    };
    
    // Create rows for pending todos
    for (const auto& todo : m_Todos)
    {
        if (!todo.Completed && matchesSearch(todo))
        {
            CreateTodoRow(todo, m_PendingContainer, false);
        }
    }
    
    // Create rows for completed todos
    for (const auto& todo : m_Todos)
    {
        if (todo.Completed && matchesSearch(todo))
        {
            CreateTodoRow(todo, m_CompletedContainer, true);
        }
    }
}

void TodoPanel::CreateTodoRow(const TodoItem& todo, UIElement* container, bool isCompleted)
{
    if (!container)
        return;
    
    // Row container
    auto row = std::make_unique<UIElement>();
    row->SetId("todo-row-" + todo.Id);
    row->AddClass("todo-row");
    if (isCompleted)
        row->AddClass("todo-completed");
    if (m_SelectedId == todo.Id)
        row->AddClass("selected");
    
    row->Overrides()
        .Set(Style::MinHeight, StyleLength::Px(m_RowHeight))
        .Set(Style::Height, StyleLength::Px(m_RowHeight));
    
    // Drag handle
    auto dragHandle = std::make_unique<UIElement>();
    dragHandle->AddClass("todo-drag-handle");
    
    // Checkbox button (toggle completion)
    auto checkbox = std::make_unique<Button>();
    checkbox->AddClass("todo-checkbox");
    if (isCompleted)
        checkbox->AddClass("todo-checkbox-checked");
    checkbox->SetText(""); // Icon set via CSS

    // Reduce checkbox size when row is at minimum height
    if (m_RowHeight <= 22.0f)
    {
        int checkboxSize = 18; // Smaller checkbox for compact rows
        checkbox->Overrides()
            .Set(Style::Width, StyleLength::Px((float)checkboxSize))
            .Set(Style::Height, StyleLength::Px((float)checkboxSize))
            .Set(Style::MinWidth, StyleLength::Px((float)checkboxSize))
            .Set(Style::MinHeight, StyleLength::Px((float)checkboxSize));
    }
    
    std::string todoId = todo.Id;
    checkbox->RegisterEventHandler(kEventMouseDown, [this, todoId](UIEvent& e)
    {
        if (e.Button == 0)
        {
            CancelDrag(); // Cancel any active drag before modifying
            ToggleTodoComplete(todoId);
            e.Stop();
        }
    });
    
    // Editable text label
    auto textField = std::make_unique<TextField>();
    textField->AddClass("todo-text");
    if (isCompleted)
        textField->AddClass("todo-text-completed");
    textField->SetValue(todo.Text);
    
    // Handle focus out (blur) to save
    textField->RegisterEventHandler(kEventFocusOut, [this, todoId](UIEvent& e)
    {
        if (auto* tf = dynamic_cast<TextField*>(e.Target))
        {
            UpdateTodoText(todoId, tf->GetValue());
        }
    });
    
    // Handle Enter key to save and unfocus
    textField->RegisterEventHandler(kEventKeyDown, [this, todoId](UIEvent& e)
    {
        if (e.Key == Input::kKeyCode_Enter || e.Key == Input::kKeyCode_NumPadEnter)
        {
            if (auto* tf = dynamic_cast<TextField*>(e.Target))
            {
                UpdateTodoText(todoId, tf->GetValue());
            }
        }
    });
    
    // Delete button
    auto deleteBtn = std::make_unique<Button>();
    deleteBtn->AddClass("todo-delete");
    deleteBtn->SetText("×");
    deleteBtn->RegisterEventHandler(kEventMouseDown, [this, todoId](UIEvent& e)
    {
        if (e.Button == 0)
        {
            CancelDrag(); // Cancel any active drag before modifying
            // Defer so we don't modify m_Todos/rebuild while still in the handler (fixes rapid-delete crash)
            auto it = std::find_if(m_Todos.begin(), m_Todos.end(),
                [&todoId](const TodoItem& t) { return t.Id == todoId; });
            if (it == m_Todos.end())
            {
                e.Stop();
                return;
            }
            const TodoItem item = *it;
            if (m_PendingContainer)
            {
                m_PendingContainer->PostAction([this, item]()
                {
                    auto found = std::find_if(m_Todos.begin(), m_Todos.end(),
                        [&item](const TodoItem& t) { return t.Id == item.Id; });
                    if (found == m_Todos.end())
                        return;
                    if (m_Undo)
                        m_Undo->Execute(std::make_unique<Editor::RemoveTodoCommand>(this, item));
                    else
                        RemoveTodo(item.Id);
                });
            }
            else
            {
                if (m_Undo)
                    m_Undo->Execute(std::make_unique<Editor::RemoveTodoCommand>(this, item));
                else
                    RemoveTodo(todoId);
            }
            e.Stop();
        }
    });
    
    // Content container
    auto contentContainer = std::make_unique<UIElement>();
    contentContainer->AddClass("todo-content-container");
    
    UIElement* dragHandlePtr = dragHandle.get();
    contentContainer->AddChild(std::move(dragHandle));
    contentContainer->AddChild(std::move(checkbox));
    contentContainer->AddChild(std::move(textField));
    
    // Delete column
    auto deleteColumn = std::make_unique<UIElement>();
    deleteColumn->AddClass("todo-delete-column");
    deleteColumn->AddChild(std::move(deleteBtn));
    
    row->AddChild(std::move(contentContainer));
    row->AddChild(std::move(deleteColumn));
    
    // Row selection on click
    row->RegisterEventHandler(kEventMouseDown, [this, todoId](UIEvent& e)
    {
        if (e.Button == 0)
        {
            UIElement* target = e.Target;
            if (target && (target->HasClass("todo-checkbox") || 
                          target->HasClass("todo-delete") ||
                          target->HasClass("todo-drag-handle") ||
                          target->HasClass("todo-text")))
                return;
            
            m_SelectedId = todoId;
            // Mark for deferred rebuild - will happen in OnPostLayout
            m_NeedsRebuild = true;
        }
    });
    
    SetupDragAndDrop(row.get(), dragHandlePtr, todo.Id);
    
    container->AddChild(std::move(row));
}

void TodoPanel::SetupDragAndDrop(UIElement* row, UIElement* dragHandle, const std::string& todoId)
{
    if (!row || !dragHandle)
        return;
    
    // Capture generation to detect if rebuild invalidated our pointers
    uint32_t generation = m_RebuildGeneration;
    
    dragHandle->RegisterEventHandler(kEventMouseDown, [this, row, todoId, dragHandle, generation](UIEvent& e)
    {
        if (e.Button != 0)
            return;
        
        // If a rebuild happened since this handler was created, our pointers are invalid
        if (generation != m_RebuildGeneration)
            return;
        
        m_DraggedRow = row;
        m_DragStartId = todoId;
        m_DragTargetIndex = -1;
        m_DragActive = false;
        m_DragStartY = e.Y;
        
        // Determine which section we're dragging from
        auto it = std::find_if(m_Todos.begin(), m_Todos.end(),
            [&todoId](const TodoItem& t) { return t.Id == todoId; });
        if (it != m_Todos.end())
        {
            m_DragInCompletedSection = it->Completed;
        }
        
        e.Capture(dragHandle);
        e.Stop();
    });
    
    dragHandle->RegisterEventHandler(kEventMouseMove, [this, row, todoId, generation](UIEvent& e)
    {
        // If a rebuild happened, our pointers are invalid
        if (generation != m_RebuildGeneration || m_DraggedRow != row)
            return;
        
        // Get the container for the current section
        UIElement* container = m_DragInCompletedSection ? m_CompletedContainer : m_PendingContainer;
        if (!container)
            return;
        
        const float dragThreshold = 5.0f;
        if (!m_DragActive)
        {
            float deltaY = std::abs(e.Y - m_DragStartY);
            if (deltaY < dragThreshold)
                return;
            m_DragActive = true;
            row->AddClass("dragging");
        }
        
        // Mouse Y in layout coordinates
        float mouseY = e.Y;
        
        // Build list of todo rows sorted by visual position
        std::vector<std::pair<UIElement*, std::string>> todoRows;
        const auto& children = container->GetChildren();
        for (const auto& child : children)
        {
            if (child->HasClass("todo-row"))
            {
                std::string rowId = child->GetId();
                // Extract todo ID from row ID (format: "todo-row-{id}")
                size_t prefixLen = std::string("todo-row-").length();
                if (rowId.length() > prefixLen)
                {
                    std::string tid = rowId.substr(prefixLen);
                    todoRows.push_back({child.get(), tid});
                }
            }
        }
        
        // Sort by visual Y position
        std::sort(todoRows.begin(), todoRows.end(),
            [](const auto& a, const auto& b) {
                return a.first->GetLayoutY() < b.first->GetLayoutY();
            });
        
        if (todoRows.empty())
            return;
        
        // Find the visual index of the dragged row
        int draggedVisualIndex = -1;
        for (size_t i = 0; i < todoRows.size(); ++i)
        {
            if (todoRows[i].second == m_DragStartId)
            {
                draggedVisualIndex = static_cast<int>(i);
                break;
            }
        }
        
        // Find insertion point based on mouse Y relative to row midpoints
        int visualTargetIndex = static_cast<int>(todoRows.size()); // default: after last
        
        for (size_t i = 0; i < todoRows.size(); ++i)
        {
            UIElement* rowEl = todoRows[i].first;
            float rowY = rowEl->GetLayoutY();
            float rowH = rowEl->GetLayoutHeight();
            float rowMidY = rowY + rowH * 0.5f;
            
            if (mouseY < rowMidY)
            {
                visualTargetIndex = static_cast<int>(i);
                break;
            }
        }
        
        // Check for no-op cases
        bool isNoOp = (visualTargetIndex == draggedVisualIndex) || 
                      (visualTargetIndex == draggedVisualIndex + 1);
        
        if (isNoOp)
        {
            if (m_InsertionIndicator)
            {
                m_InsertionIndicator->Overrides().Set(Style::Display, DisplayMode::None);
            }
            m_DragTargetIndex = -1;
            e.Stop();
            return;
        }
        
        // Convert visual target to actual index after removal
        int targetIndex = visualTargetIndex;
        if (visualTargetIndex > draggedVisualIndex && draggedVisualIndex >= 0)
        {
            targetIndex--;
        }
        
        m_DragTargetIndex = targetIndex;
        
        // Calculate indicator position
        float indicatorAbsY = 0.0f;
        
        if (visualTargetIndex < static_cast<int>(todoRows.size()))
        {
            indicatorAbsY = todoRows[visualTargetIndex].first->GetLayoutY();
        }
        else
        {
            UIElement* lastRow = todoRows.back().first;
            indicatorAbsY = lastRow->GetLayoutY() + lastRow->GetLayoutHeight();
        }
        
        // Convert to relative position (relative to content container where indicator lives)
        float contentContainerY = m_ContentContainer ? m_ContentContainer->GetLayoutY() : 0.0f;
        float indicatorY = indicatorAbsY - contentContainerY;
        
        // Show insertion indicator
        if (m_InsertionIndicator)
        {
            m_InsertionIndicator->Overrides()
                .Set(Style::PositionTop, StyleLength::Px(indicatorY))
                .Set(Style::Display, DisplayMode::Block);
        }
        
        e.Stop();
    });
    
    dragHandle->RegisterEventHandler(kEventMouseUp, [this, row, todoId, generation](UIEvent& e)
    {
        // If a rebuild happened, our pointers are invalid - just clean up state
        if (generation != m_RebuildGeneration)
        {
            m_DraggedRow = nullptr;
            m_DragActive = false;
            m_DragStartId.clear();
            m_DragTargetIndex = -1;
            if (m_InsertionIndicator)
            {
                m_InsertionIndicator->Overrides().Set(Style::Display, DisplayMode::None);
            }
            return;
        }
        
        if (e.Button != 0 || m_DraggedRow != row)
            return;
        
        if (m_DragActive)
        {
            row->RemoveClass("dragging");
        }
        
        // Perform the reorder if we have a valid target
        if (m_DragActive && m_DragTargetIndex >= 0 && !m_DragStartId.empty())
        {
            // Compute from-index in section for undo (before applying the move)
            int fromIndexInSection = -1;
            int idx = 0;
            for (const auto& t : m_Todos)
            {
                if (t.Completed != m_DragInCompletedSection)
                    continue;
                if (t.Id == m_DragStartId)
                {
                    fromIndexInSection = idx;
                    break;
                }
                ++idx;
            }
            const std::string dragId = m_DragStartId;
            const int toIdx = m_DragTargetIndex;
            const bool inCompleted = m_DragInCompletedSection;
            ReorderTodos(dragId, toIdx, inCompleted);
            if (m_Undo && fromIndexInSection >= 0)
                m_Undo->CommitAlreadyApplied(std::make_unique<Editor::ReorderTodosCommand>(
                    this, dragId, fromIndexInSection, toIdx, inCompleted));
        }
        
        if (m_InsertionIndicator)
        {
            m_InsertionIndicator->Overrides().Set(Style::Display, DisplayMode::None);
        }

        m_DraggedRow = nullptr;
        m_DragActive = false;
        m_DragStartId.clear();
        m_DragTargetIndex = -1;
        
        e.Stop();
    });
}

void TodoPanel::CancelDrag()
{
    // Don't try to access m_DraggedRow - it might be a dangling pointer
    // The rebuild will create new rows without the "dragging" class anyway
    m_DraggedRow = nullptr;
    m_DragActive = false;
    m_DragStartId.clear();
    m_DragTargetIndex = -1;
    if (m_InsertionIndicator)
    {
        m_InsertionIndicator->Overrides().Set(Style::Display, DisplayMode::None);
    }
}

void TodoPanel::AddTodo(const std::string& text)
{
    if (text.empty())
        return;
    
    CancelDrag(); // Cancel any active drag before modifying
    
    TodoItem todo;
    todo.Id = GenerateUniqueId();
    todo.Text = text;
    todo.Completed = false;
    todo.Order = 0;
    
    // Shift existing pending todos down
    for (auto& t : m_Todos)
    {
        if (!t.Completed)
            t.Order += 1;
    }
    
    m_Todos.insert(m_Todos.begin(), todo);
    
    // Mark for deferred rebuild - will happen in OnPostLayout
    m_NeedsRebuild = true;
    m_NeedsSave = true;
}

void TodoPanel::AddTodoItem(const TodoItem& item)
{
    CancelDrag();
    m_Todos.push_back(item);
    // Reassign all orders (same as ReorderTodos)
    int pendingOrder = 0;
    int completedOrder = 0;
    for (auto& t : m_Todos)
    {
        if (t.Completed)
            t.Order = completedOrder++;
        else
            t.Order = pendingOrder++;
    }
    m_NeedsRebuild = true;
    m_NeedsSave = true;
}

void TodoPanel::RemoveTodo(const std::string& id)
{
    auto it = std::find_if(m_Todos.begin(), m_Todos.end(),
        [&id](const TodoItem& t) { return t.Id == id; });
    
    if (it == m_Todos.end())
        return;
    
    if (m_SelectedId == id)
        m_SelectedId.clear();
    
    m_Todos.erase(it);
    
    // Mark for deferred rebuild - will happen in OnPostLayout
    m_NeedsRebuild = true;
    m_NeedsSave = true;
}

void TodoPanel::ToggleTodoComplete(const std::string& id)
{
    auto it = std::find_if(m_Todos.begin(), m_Todos.end(),
        [&id](const TodoItem& t) { return t.Id == id; });
    
    if (it == m_Todos.end())
        return;
    
    it->Completed = !it->Completed;
    it->Order = 0; // Move to top of new section
    
    // Reassign orders within the section
    int pendingOrder = 0;
    int completedOrder = 0;
    for (auto& t : m_Todos)
    {
        if (t.Id == id)
            continue;
        if (t.Completed)
            t.Order = ++completedOrder;
        else
            t.Order = ++pendingOrder;
    }
    
    // Mark for deferred rebuild - will happen in OnPostLayout
    m_NeedsRebuild = true;
    m_NeedsSave = true;
}

void TodoPanel::UpdateTodoText(const std::string& id, const std::string& newText)
{
    auto it = std::find_if(m_Todos.begin(), m_Todos.end(),
        [&id](const TodoItem& t) { return t.Id == id; });
    
    if (it == m_Todos.end())
        return;
    
    if (it->Text == newText)
        return;

    it->Text = newText;
    SaveTodos();
}

void TodoPanel::ReorderTodos(const std::string& fromId, int toIndex, bool inCompletedSection)
{
    // Find and move the todo
    auto it = std::find_if(m_Todos.begin(), m_Todos.end(),
        [&fromId](const TodoItem& t) { return t.Id == fromId; });
    
    if (it == m_Todos.end())
        return;
    
    TodoItem moved = *it;
    moved.Completed = inCompletedSection;
    m_Todos.erase(it);
    
    // Build a list of todos in the target section (same completed state)
    std::vector<size_t> sectionIndices;
    for (size_t i = 0; i < m_Todos.size(); ++i)
    {
        if (m_Todos[i].Completed == inCompletedSection)
        {
            sectionIndices.push_back(i);
        }
    }
    
    // Determine where to insert
    size_t insertPos = m_Todos.size(); // Default: end
    if (toIndex >= 0 && toIndex < static_cast<int>(sectionIndices.size()))
    {
        insertPos = sectionIndices[toIndex];
    }
    else if (toIndex >= static_cast<int>(sectionIndices.size()) && !sectionIndices.empty())
    {
        // Insert after the last item in section
        insertPos = sectionIndices.back() + 1;
    }
    
    // Insert at the calculated position
    m_Todos.insert(m_Todos.begin() + insertPos, moved);
    
    // Reassign all orders
    int pendingOrder = 0;
    int completedOrder = 0;
    for (auto& t : m_Todos)
    {
        if (t.Completed)
            t.Order = completedOrder++;
        else
            t.Order = pendingOrder++;
    }
    
    // Mark for deferred rebuild - will happen in OnPostLayout
    m_NeedsRebuild = true;
    m_NeedsSave = true;
}

std::string TodoPanel::GenerateUniqueId()
{
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    return "todo_" + std::to_string(ms) + "_" + std::to_string(m_NextId++);
}

void TodoPanel::SaveTodos()
{
    auto projectPaths = Editor::GetCurrentEditorProjectPaths();
    if (projectPaths.projectEditorRoot.empty())
    {
        Logger::Log::Warning("[TodoPanel] Cannot save todos: no project root");
        return;
    }
    
    auto todosPath = projectPaths.projectEditorRoot / "Todos.json";
    Editor::SettingsStore store(todosPath);
    
    nlohmann::json todosJson = nlohmann::json::array();
    for (const auto& todo : m_Todos)
    {
        nlohmann::json todoJson;
        todoJson["id"] = todo.Id;
        todoJson["text"] = todo.Text;
        todoJson["completed"] = todo.Completed;
        todoJson["order"] = todo.Order;
        todosJson.push_back(todoJson);
    }
    
    store.SetJson("todos", todosJson);
    store.SetInt64("nextId", static_cast<int64_t>(m_NextId));
    
    std::string err;
    if (!store.Save(&err))
    {
        Logger::Log::Error("[TodoPanel] Failed to save todos: {}", err);
    }
}

void TodoPanel::LoadTodos()
{
    m_Todos.clear();
    
    auto projectPaths = Editor::GetCurrentEditorProjectPaths();
    if (projectPaths.projectEditorRoot.empty())
        return;
    
    auto todosPath = projectPaths.projectEditorRoot / "Todos.json";
    Editor::SettingsStore store(todosPath);
    
    std::string err;
    if (!store.Load(&err))
        return;
    
    if (store.Contains("nextId"))
    {
        int64_t nextId = 1;
        if (store.TryGetInt64("nextId", nextId))
            m_NextId = static_cast<int>(nextId);
    }
    
    if (!store.Contains("todos"))
        return;
    
    try
    {
        const auto& todosJson = store.Json()["todos"];
        if (!todosJson.is_array())
            return;
        
        for (const auto& todoJson : todosJson)
        {
            TodoItem todo;
            todo.Id = todoJson.value("id", "");
            todo.Text = todoJson.value("text", "");
            todo.Completed = todoJson.value("completed", false);
            todo.Order = todoJson.value("order", 0);
            
            if (!todo.Id.empty() && !todo.Text.empty())
            {
                m_Todos.push_back(todo);
            }
        }
        
        std::sort(m_Todos.begin(), m_Todos.end(),
            [](const TodoItem& a, const TodoItem& b) { return a.Order < b.Order; });
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning("[TodoPanel] Failed to parse todos: {}", e.what());
    }
}

void TodoPanel::SaveRowHeight()
{
    auto projectPaths = Editor::GetCurrentEditorProjectPaths();
    if (projectPaths.projectEditorRoot.empty())
        return;
    
    auto todosPath = projectPaths.projectEditorRoot / "Todos.json";
    Editor::SettingsStore store(todosPath);
    
    std::string err;
    store.Load(&err); // Load existing data
    
    store.SetDouble("rowHeight", static_cast<double>(m_RowHeight));
    
    if (!store.Save(&err))
    {
        Logger::Log::Error("[TodoPanel] Failed to save row height: {}", err);
    }
}

void TodoPanel::LoadRowHeight()
{
    auto projectPaths = Editor::GetCurrentEditorProjectPaths();
    if (projectPaths.projectEditorRoot.empty())
        return;
    
    auto todosPath = projectPaths.projectEditorRoot / "Todos.json";
    Editor::SettingsStore store(todosPath);
    
    std::string err;
    if (!store.Load(&err))
        return;
    
    double rowHeight = 32.0;
    if (store.TryGetDouble("rowHeight", rowHeight))
    {
        m_RowHeight = static_cast<float>(rowHeight);
        m_RowHeight = std::max(20.0f, std::min(256.0f, m_RowHeight));
    }
}

} // namespace GameEngine
