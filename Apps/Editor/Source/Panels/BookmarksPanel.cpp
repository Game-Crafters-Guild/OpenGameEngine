#include "Panels/BookmarksPanel.h"
#include "EditorContext.h"
#include "Input/KeyCodes.h"
#include "Editor/DragDropPayloads.h"
#include "UndoRedo/BookmarkCommands.h"
#include "UndoRedo/UndoRedoService.h"
#include "Editor/Settings/SettingsStore.h"
#include "Editor/EditorPaths.h"
#include "Core/Engine.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "AssetCore/GUID.h"
#include "AssetCore/AssetTypes.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/Interaction/Payload.h"
#include "UI/Interaction/DragDropManager.h"
#include "UI/UIManager.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Button.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/PanelSearchBar.h"
#include "UI/StyleProperties.h"
#include "Panels/SettingsPanel.h"
#include "Panels/AssetsPanel.h"
#include "Assets/PolyhavenDownloadManager.h"
#include "Assets/PolyhavenService.h"
#include "Scheduler/Scheduler.h"
#include "Panels/HierarchyPanel.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "Logger/Logger.h"
#include "Scene/SceneIO.h"
#include "ECS/World.h"
#include "ECS/Entity.h"
#include "Components/Name.h"
#include "Components/SceneEntityTag.h"
#include "Scene/SceneDocumentManager.h"
#include "Input/InputSystem.h"
#include "Panels/ConfirmActionModal.h"
#include "ECS/ECS.h"
#include "Editor/Hierarchy/HierarchyEntityIcon.h"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <cmath>
#include <sstream>

namespace GameEngine {

// Static callback for adding bookmarks
std::function<void(const Bookmark&)> BookmarksPanel::s_OnAddBookmark = nullptr;

// Static drag state to track what's being dragged across panels
// Defined here, declared extern in AssetsBrowserController
struct DragState {
    bool active = false;
    std::filesystem::path assetPath;
    std::string scenePath;
    std::string entityId;
};
DragState g_DragState;

void BookmarksPanel::SetOnAddBookmark(std::function<void(const Bookmark&)> cb)
{
    s_OnAddBookmark = std::move(cb);
}

void BookmarksPanel::AddBookmarkStatic(const Bookmark& bookmark)
{
    if (s_OnAddBookmark)
        s_OnAddBookmark(bookmark);
}

BookmarksPanel::BookmarksPanel()
    : DockPanel("Bookmarks")
{
    BuildUI();
    
    // Initialize workspace root tracking (will be set when SetEditorContext is called)
    m_CurrentWorkspaceRoot = std::filesystem::path();
    
    // Don't load bookmarks here - wait for SetEditorContext to be called
    // This ensures we load the correct project's bookmarks
    
    // Register this instance as the callback target
    SetOnAddBookmark([this](const Bookmark& bm) { AddBookmark(bm); });
}

BookmarksPanel::~BookmarksPanel()
{
    if (m_SearchBar)
        SettingsPanel::UnregisterSearchBar(m_SearchBar);
}

void BookmarksPanel::SetEditorContext(EditorContext* context)
{
    m_Context = context;
    m_Undo = (context && context->UndoRedo) ? context->UndoRedo : nullptr;
    
    // Check if workspace root has changed (project changed)
    auto& engine = EngineCore::GetInstance();
    std::filesystem::path newWorkspaceRoot = engine.GetWorkspaceRoot();
    
    // Normalize paths for comparison (handle empty paths)
    bool workspaceChanged = false;
    if (m_CurrentWorkspaceRoot.empty() && !newWorkspaceRoot.empty())
    {
        // First time setting a workspace root
        workspaceChanged = true;
    }
    else if (!m_CurrentWorkspaceRoot.empty() && newWorkspaceRoot.empty())
    {
        // Workspace root was cleared
        workspaceChanged = true;
    }
    else if (!m_CurrentWorkspaceRoot.empty() && !newWorkspaceRoot.empty())
    {
        // Both are set, compare them using string comparison (more reliable)
        std::string currentStr = m_CurrentWorkspaceRoot.string();
        std::string newStr = newWorkspaceRoot.string();
        // Normalize paths by removing trailing slashes for comparison
        while (!currentStr.empty() && (currentStr.back() == '/' || currentStr.back() == '\\'))
            currentStr.pop_back();
        while (!newStr.empty() && (newStr.back() == '/' || newStr.back() == '\\'))
            newStr.pop_back();
        workspaceChanged = (currentStr != newStr);
    }
    
    // Always refresh bookmarks when workspace changes or when SetEditorContext is called
    // This ensures bookmarks are refreshed on project start/switch
    if (workspaceChanged || !newWorkspaceRoot.empty())
    {
        // Update workspace root tracking
        m_CurrentWorkspaceRoot = newWorkspaceRoot;
        
        // Load bookmarks for the new/current project
        LoadBookmarks();
        LoadRowHeight();
        
        // Refresh the UI to show loaded bookmarks
        // Defer UI update to ensure panel is ready
        if (GetOwnerManager())
        {
            PostAction([this]()
            {
                if (m_ListContainer)
                {
                    RebuildBookmarkRows();
                }
            });
        }
        else if (m_ListContainer)
        {
            // If no owner manager yet, update immediately (will be called again when attached)
            RebuildBookmarkRows();
        }
    }
}

void BookmarksPanel::OnPostLayout()
{
    // On first layout after being attached to a UI manager, refresh bookmarks
    // to ensure they're validated against the current asset registry
    if (!m_InitialRefreshDone && GetOwnerManager() && m_Context)
    {
        m_InitialRefreshDone = true;
        // Defer refresh to avoid mutating UI during layout
        PostAction([this]()
        {
            if (GetOwnerManager())
                RefreshBookmarks();
        });
    }
}

void BookmarksPanel::BuildUI()
{
    // Main container
    auto container = std::make_unique<UIElement>();
    container->AddClass("bookmarks-panel");
    
    // Search bar at the top
    auto built = BuildPanelSearchBar(
        "bookmarks-search-field",
        []() { return SettingsPanel::GetSearchBarsVisible(); },
        [this](const std::string& value)
        {
            m_SearchQuery = value;
            RebuildBookmarkRows();
        },
        [this](const std::string& value)
        {
            m_SearchQuery = value;
            RebuildBookmarkRows();
        },
        {{"all", "All fields"}, {"name", "Name"}, {"type", "Type"}, {"reference", "Reference"}},
        [this](const std::string& scope)
        {
            m_SearchFieldScope = scope;
            RebuildBookmarkRows();
        });
    m_SearchBar = built.RootPtr;
    m_SearchField = built.FieldPtr;
    container->AddChild(std::move(built.Root));
    SettingsPanel::RegisterSearchBar(m_SearchBar, built.IconPtr);
    
    // Scroll view for bookmark list
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->SetId("bookmarks-scroll");
    scrollView->AddClass("bookmarks-list");
    scrollView->Overrides().SetCustomNumber(StringId("--ui_scrollview_measure_horizontal"), 0.0f);
    m_ScrollView = scrollView.get();
    
    // Handle Ctrl+Scroll to adjust row height
    scrollView->RegisterEventHandler(kEventScroll, [this, scrollViewPtr = scrollView.get()](UIEvent& e)
    {
        // Only handle if this is the scroll view's event
        if (e.CurrentTarget != scrollViewPtr)
            return;
        
        // Check for Control modifier (Ctrl on Windows/Linux, Cmd on Mac uses kModSuper)
        const bool primaryMod = Input::IsPrimaryShortcutModifier(e.Mods);        
        if (primaryMod && e.ScrollY != 0.0f)
        {
            // Adjust row height (scroll up = increase, scroll down = decrease)
            // Note: scrollY is negative when scrolling up (wheel forward), positive when scrolling down (wheel back)
            const float step = 2.0f; // pixels per scroll step
            float newHeight = m_RowHeight - e.ScrollY * step; // Negative scrollY = scroll up = increase height
            
            // Clamp to reasonable range
            newHeight = std::max(20.0f, std::min(256.0f, newHeight));
            
            if (std::abs(newHeight - m_RowHeight) > 0.1f)
            {
                m_RowHeight = newHeight;
                
                // Save row height preference
                SaveRowHeight();
                
                // Rebuild rows with new height
                if (m_ListContainer)
                {
                    m_ListContainer->PostAction([this]()
                    {
                        RebuildBookmarkRows();
                    });
                }
            }
            
            e.Stop(); // Consume the event so it doesn't scroll
        }
    });
    
    // List container inside scroll view
    auto listContainer = std::make_unique<UIElement>();
    listContainer->SetId("bookmarks-list-container");
    listContainer->AddClass("bookmarks-list-container");
    m_ListContainer = listContainer.get();
    
    // Insertion indicator for drag-and-drop
    // Add it to the list container so it's positioned relative to the scrollable content
    auto indicator = std::make_unique<UIElement>();
    indicator->SetId("bookmarks-insertion-indicator");
    m_InsertionIndicator = indicator.get();
    listContainer->AddChild(std::move(indicator));
    
    scrollView->AddContent(std::move(listContainer));
    container->AddChild(std::move(scrollView));
    
    AddChild(std::move(container));
    
    // Drag start on panel (same as TreeView): when we have capture from row mousedown,
    // mouse move is dispatched to the panel; start drag when threshold exceeded.
    RegisterEventHandler(kEventMouseMove, [this](UIEvent& e)
    {
        if (!m_DragPending || m_DragPendingIndex >= m_Bookmarks.size())
            return;
        UIManager* ui = GetOwnerManager();
        if (!ui)
            return;
        UI::Interaction::DragDropManager* dd = ui->GetDragDropManager();
        if (!dd || dd->IsDragging())
            return;
        constexpr float kDragThresholdPx = 6.0f;
        const float dx = e.X - m_DragStartX;
        const float dy = e.Y - m_DragStartY;
        if ((dx * dx + dy * dy) < (kDragThresholdPx * kDragThresholdPx))
            return;
        const size_t index = m_DragPendingIndex;
        const Bookmark& bm = m_Bookmarks[index];
        Editor::BookmarkDragPayload payload;
        payload.bookmark = bm;
        payload.bookmarkIndex = index;
        payload.displayLabel = bm.Name;
        UI::Interaction::DragPayload dragPayload = UI::Interaction::DragPayload::Create(std::move(payload));
        dragPayload.DisplayLabel = bm.Name;

        // Set ghost icon/thumbnail for drag overlay (scene icon, asset thumbnail, or folder)
        if (bm.Type == BookmarkType::Scene || bm.Type == BookmarkType::Entity)
        {
            dragPayload.GhostIconKind = UI::Interaction::DragGhostIconKind::AssetFile;
            dragPayload.GhostThumbnailEngineName = "Icons/sceneicon.png";
        }
        else if (bm.Type == BookmarkType::Asset && m_Context)
        {
            std::filesystem::path assetPath;
            bool isFolder = false;
            try
            {
                if (std::filesystem::exists(bm.Reference) && std::filesystem::is_directory(bm.Reference))
                {
                    assetPath = std::filesystem::path(bm.Reference);
                    isFolder = true;
                }
            }
            catch (...)
            {
            }
            if (!isFolder)
            {
                auto& engine = EngineCore::GetInstance();
                AssetRegistry& registry = engine.GetAssetManager().GetRegistry();
                GUID guid(bm.Reference);
                if (!guid.IsNull())
                {
                    AssetMetadata metadata;
                    if (registry.TryGetAssetMetadata(guid, metadata))
                    {
                        assetPath = metadata.Path;
                        try
                        {
                            if (std::filesystem::exists(assetPath) && std::filesystem::is_directory(assetPath))
                                isFolder = true;
                        }
                        catch (...)
                        {
                        }
                    }
                }
            }
            if (isFolder)
            {
                dragPayload.GhostIconKind = UI::Interaction::DragGhostIconKind::AssetFolder;
            }
            else
            {
                dragPayload.GhostIconKind = UI::Interaction::DragGhostIconKind::AssetFile;
                if (!assetPath.empty())
                {
                    const std::string ext = assetPath.extension().string();
                    if (ext == ".scene")
                        dragPayload.GhostThumbnailEngineName = "Icons/sceneicon.png";
                    else if (m_Context->Thumbnails)
                    {
                        const std::string thumb = m_Context->Thumbnails->GetOrRequest(assetPath, 128, [](const std::string&) {});
                        if (!thumb.empty())
                            dragPayload.GhostThumbnailEngineName = thumb;
                    }
                }
            }
        }

        UI::Interaction::DragSessionContext ctx{};
        ctx.SourceWidgetId = GetInstanceId();
        dd->BeginDrag(std::move(dragPayload), ctx);
        m_DragPending = false;
        e.Stop();
    });

    // Mouse up on panel (we have capture after row mousedown): clear state and handle click vs drag.
    RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
    {
        if (e.Button != 0)
            return;
        m_DragSourceRow = nullptr;
        const bool wasClick = m_DragPending && m_DragPendingIndex < m_Bookmarks.size();
        const size_t index = m_DragPendingIndex;
        m_DragPending = false;
        if (wasClick)
        {
            const bool addToSelection = (m_DragPendingMods & (Input::kModControl | Input::kModSuper)) != 0;
            const bool rangeSelect = (m_DragPendingMods & Input::kModShift) != 0;
            if (addToSelection)
            {
                if (m_SelectedIndices.count(index))
                    m_SelectedIndices.erase(index);
                else
                    m_SelectedIndices.insert(index);
                m_SelectionAnchor = index;
                ApplySelectionVisuals();
            }
            else if (rangeSelect)
            {
                if (m_SelectionAnchor >= m_Bookmarks.size())
                    m_SelectionAnchor = index;
                const size_t lo = std::min(m_SelectionAnchor, index);
                const size_t hi = std::max(m_SelectionAnchor, index);
                m_SelectedIndices.clear();
                for (size_t i = lo; i <= hi; ++i)
                    m_SelectedIndices.insert(i);
                m_SelectionAnchor = index;
                ApplySelectionVisuals();
            }
            else
            {
                m_SelectedIndices = { index };
                m_SelectionAnchor = index;
                ApplySelectionVisuals();
                NavigateToBookmark(index);
            }
        }
        e.Stop();
    });

    // Set up drop handlers on the panel's content (first child) so we receive MouseUp/MouseMove
    // when they bubble from list rows or scroll view.
    const auto& kids = GetChildren();
    UIElement* content = (!kids.empty()) ? kids[0].get() : nullptr;
    SetupDropHandlers(content);
}

void BookmarksPanel::RebuildBookmarkRows()
{
    if (!m_ListContainer)
        return;
    
    // When called from drop handler we want immediate refresh; defer only if re-entering
    static thread_local int s_RebuildDepth = 0;
    if (UIElement::IsInEventDispatch() && s_RebuildDepth > 0)
    {
        m_ListContainer->PostAction([this]() { RebuildBookmarkRows(); });
        return;
    }
    ++s_RebuildDepth;
    
    // Reset scroll position to top
    if (m_ScrollView)
    {
        m_ScrollView->SetScrollY(0.0f);
    }
    
    // Clear existing rows (but keep insertion indicator)
    const auto& children = m_ListContainer->GetChildren();
    std::vector<UIElement*> toRemove;
    for (const auto& child : children)
    {
        if (child->HasClass("bookmark-row") || child->HasClass("bookmark-empty-state"))
        {
            toRemove.push_back(child.get());
        }
    }
    for (UIElement* child : toRemove)
    {
        m_ListContainer->RemoveChild(child);
    }
    
    // Clamp selection and anchor to bounds
    const size_t n = m_Bookmarks.size();
    for (auto it = m_SelectedIndices.begin(); it != m_SelectedIndices.end(); )
    {
        if (*it >= n)
            it = m_SelectedIndices.erase(it);
        else
            ++it;
    }
    if (m_SelectionAnchor >= n)
        m_SelectionAnchor = m_SelectedIndices.empty() ? 0 : *m_SelectedIndices.begin();
    
    // Sort bookmarks by order (ascending: 0 = top, higher = below)
    // This ensures newest bookmarks (order 0) appear at the top
    std::sort(m_Bookmarks.begin(), m_Bookmarks.end(), 
        [](const Bookmark& a, const Bookmark& b) { return a.Order < b.Order; });
    
    // Filter bookmarks based on search query
    std::vector<size_t> visibleIndices;
    if (m_SearchQuery.empty())
    {
        // No search - show all
        for (size_t i = 0; i < m_Bookmarks.size(); ++i)
            visibleIndices.push_back(i);
    }
    else
    {
        // Filter by the selected bookmark field.
        std::string searchLower = m_SearchQuery;
        std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(),
            [](unsigned char c) { return std::tolower(c); });
        
        for (size_t i = 0; i < m_Bookmarks.size(); ++i)
        {
            const Bookmark& bm = m_Bookmarks[i];
            const char* typeName = bm.Type == BookmarkType::Asset ? "asset" :
                                   bm.Type == BookmarkType::Scene ? "scene" : "entity";
            std::string searchable;
            if (m_SearchFieldScope == "name")
                searchable = bm.Name;
            else if (m_SearchFieldScope == "type")
                searchable = typeName;
            else if (m_SearchFieldScope == "reference")
                searchable = bm.Reference;
            else
                searchable = bm.Name + " " + typeName + " " + bm.Reference;
            std::transform(searchable.begin(), searchable.end(), searchable.begin(),
                [](unsigned char c) { return std::tolower(c); });
            
            if (searchable.find(searchLower) != std::string::npos)
            {
                visibleIndices.push_back(i);
            }
        }
    }
    
    // Create rows for visible bookmarks
    // Note: CreateBookmarkRow uses the bookmark index (not display index) for callbacks
    // Validate indices before creating rows to prevent crashes
    for (size_t bookmarkIndex : visibleIndices)
    {
        if (bookmarkIndex < m_Bookmarks.size())
        {
            CreateBookmarkRow(bookmarkIndex, m_ListContainer);
        }
    }

    if (visibleIndices.empty())
    {
        auto emptyState = std::make_unique<UIElement>();
        emptyState->AddClass("bookmark-empty-state");

        auto card = std::make_unique<UIElement>();
        card->AddClass("bookmark-empty-card");

        auto icon = std::make_unique<UIElement>();
        icon->AddClass("bookmark-empty-icon");

        auto label = std::make_unique<Label>();
        label->AddClass("bookmark-empty-text");
        label->SetText(m_Bookmarks.empty()
            ? "Drag assets, scenes or entities here to add bookmarks"
            : "No bookmarks match the search");

        card->AddChild(std::move(icon));
        card->AddChild(std::move(label));
        emptyState->AddChild(std::move(card));
        m_ListContainer->AddChild(std::move(emptyState));
    }
    --s_RebuildDepth;
}

void BookmarksPanel::CreateBookmarkRow(size_t index, UIElement* container)
{
    if (!container)
        return;
    
    if (index >= m_Bookmarks.size())
    {
        Logger::Log::Warning("[BookmarksPanel] CreateBookmarkRow: index {} out of bounds (size: {})", index, m_Bookmarks.size());
        return;
    }
    
    const Bookmark& bookmark = m_Bookmarks[index];
    
    // Row container - use list-item class for consistent styling
    auto row = std::make_unique<UIElement>();
    row->SetId("bookmark-row-" + std::to_string(index));
    row->AddClass("bookmark-row");
    row->AddClass("list-item");
    row->AddClass("assets-list-row");
    if (m_SelectedIndices.count(index) != 0)
        row->AddClass("selected");
    // Apply custom row height (dynamic, needs inline style)
    row->Overrides().Set(Style::MinHeight, StyleLength::Px(m_RowHeight)).Set(Style::Height, StyleLength::Px(m_RowHeight));
    
    // Drag handle - use icon image (styled via CSS class)
    auto dragHandle = std::make_unique<UIElement>();
    dragHandle->AddClass("bookmark-drag-handle");
    
    // Icon - use list-item-icon class for consistent styling
    // Scale icon size with row height (dynamic, needs inline style)
    float iconSize = m_RowHeight;
    iconSize = std::max(24.0f, std::min(256.0f, iconSize)); // Clamp between 24px and 256px
    
    auto iconEl = std::make_unique<UIElement>();
    iconEl->SetId("bookmark-icon-" + std::to_string(index)); // Set unique ID for safe lookup
    iconEl->AddClass("bookmark-icon");
    iconEl->AddClass("list-item-icon");
    // Dynamic icon size needs inline style
    iconEl->Overrides().Set(Style::Width, StyleLength::Px(iconSize)).Set(Style::Height, StyleLength::Px(iconSize))
        .Set(Style::MinWidth, StyleLength::Px(iconSize)).Set(Style::MinHeight, StyleLength::Px(iconSize));
    UpdateBookmarkIcon(iconEl.get(), bookmark);
    
    // Name label - use list-label class for consistent styling (styled via CSS class)
    auto nameLabel = std::make_unique<Label>();
    nameLabel->AddClass("bookmark-name");
    nameLabel->AddClass("list-label");
    nameLabel->SetText(bookmark.Name);
    
    // Content container (left side: drag handle, icon, name)
    auto contentContainer = std::make_unique<UIElement>();
    contentContainer->AddClass("bookmark-content-container");
    
    // Remove button column (right side) (styled via CSS class)
    auto removeColumn = std::make_unique<UIElement>();
    removeColumn->AddClass("bookmark-remove-column");
    
    // Remove button (styled via CSS class)
    auto removeBtn = std::make_unique<Button>();
    removeBtn->AddClass("bookmark-remove");
    removeBtn->SetText("×");
    // Capture bookmark identity at row creation time (not at click time) so the
    // delete handler remains valid even if m_Bookmarks was mutated by a deferred
    // add/reorder between row creation and the click.
    const Bookmark capturedBookmark = m_Bookmarks[index];
    removeBtn->RegisterEventHandler(kEventMouseDown, [this, capturedBookmark](UIEvent& e)
    {
        if (e.Button == 0)
        {
            // Defer so we don't modify m_Bookmarks/rebuild while still in the handler (fixes rapid-delete crash)
            if (m_ListContainer)
            {
                m_ListContainer->PostAction([this, capturedBookmark]()
                {
                    size_t currentIndex = FindBookmarkIndex(capturedBookmark);
                    if (currentIndex >= m_Bookmarks.size())
                        return;
                    if (m_Undo)
                        m_Undo->Execute(std::make_unique<Editor::RemoveBookmarkCommand>(this, capturedBookmark, currentIndex));
                    else
                        RemoveBookmark(currentIndex);
                });
            }
            else
            {
                size_t currentIndex = FindBookmarkIndex(capturedBookmark);
                if (currentIndex < m_Bookmarks.size())
                {
                    if (m_Undo)
                        m_Undo->Execute(std::make_unique<Editor::RemoveBookmarkCommand>(this, capturedBookmark, currentIndex));
                    else
                        RemoveBookmark(currentIndex);
                }
            }
            e.Stop();
        }
    });
    
    removeColumn->AddChild(std::move(removeBtn));
    
    UIElement* dragHandlePtr = dragHandle.get();
    contentContainer->AddChild(std::move(dragHandle));
    contentContainer->AddChild(std::move(iconEl));
    contentContainer->AddChild(std::move(nameLabel));
    
    row->AddChild(std::move(contentContainer));
    row->AddChild(std::move(removeColumn));
    
    // Register drag first so mouse down can capture and stop before click handler runs.
    SetupDragAndDrop(row.get(), dragHandlePtr, index);
    
    // Click handler: runs on mouse up when no drag occurred (drag handler runs first on mouse down and stops).
    // When click is on remove or drag handle, this handler returns; remove/drag handle have their own handlers.
    row->RegisterEventHandler(kEventMouseDown, [this, index](UIEvent& e)
    {
        if (e.Button == 0)
        {
            UIElement* target = e.Target;
            while (target)
            {
                if (target->HasClass("bookmark-remove") || target->HasClass("bookmark-drag-handle"))
                return;
                target = target->GetParent();
            }
            const bool addToSelection = (e.Mods & (Input::kModControl | Input::kModSuper)) != 0;
            const bool rangeSelect = (e.Mods & Input::kModShift) != 0;
            if (addToSelection)
            {
                if (m_SelectedIndices.count(index))
                    m_SelectedIndices.erase(index);
                else
                    m_SelectedIndices.insert(index);
                m_SelectionAnchor = index;
                ApplySelectionVisuals();
                e.Stop();
                return;
            }
            if (rangeSelect)
            {
                if (m_SelectionAnchor >= m_Bookmarks.size())
                    m_SelectionAnchor = index;
                const size_t lo = std::min(m_SelectionAnchor, index);
                const size_t hi = std::max(m_SelectionAnchor, index);
                m_SelectedIndices.clear();
                for (size_t i = lo; i <= hi; ++i)
                    m_SelectedIndices.insert(i);
                m_SelectionAnchor = index;
                ApplySelectionVisuals();
                e.Stop();
                return;
            }
            m_SelectedIndices = { index };
            m_SelectionAnchor = index;
            ApplySelectionVisuals();
            NavigateToBookmark(index);
            e.Stop();
        }
    });
    
    container->AddChild(std::move(row));
}

void BookmarksPanel::ApplySelectionVisuals()
{
    if (!m_ListContainer)
        return;
    const std::string prefix("bookmark-row-");
    for (const auto& child : m_ListContainer->GetChildren())
    {
        if (!child->HasClass("bookmark-row"))
            continue;
        const std::string& id = child->GetId();
        if (id.size() <= prefix.size() || id.compare(0, prefix.size(), prefix) != 0)
            continue;
        size_t idx = 0;
        try
        {
            idx = static_cast<size_t>(std::stoull(id.substr(prefix.size())));
        }
        catch (...)
        {
            continue;
        }
        if (m_SelectedIndices.count(idx))
            child->AddClass("selected");
        else
            child->RemoveClass("selected");
    }
}

static bool IsOverRemoveArea(UIElement* target)
{
    while (target)
    {
        if (target->HasClass("bookmark-remove") || target->HasClass("bookmark-remove-column"))
            return true;
        target = target->GetParent();
    }
    return false;
}

void BookmarksPanel::SetupDragAndDrop(UIElement* row, UIElement* /*dragHandle*/, size_t index)
{
    if (!row)
        return;
    
    // Drag from whole row (except remove button): mouse down arms drag and captures panel (same as TreeView).
    // Panel receives mouse move when it has capture; drag starts in panel's kEventMouseMove handler.
    row->RegisterEventHandler(kEventMouseDown, [this, row, index](UIEvent& e)
    {
        if (e.Button != 0)
            return;
        if (IsOverRemoveArea(e.Target))
            return;
        m_DragPending = true;
        m_DragPendingIndex = index;
        m_DragStartX = e.X;
        m_DragStartY = e.Y;
        m_DragPendingMods = e.Mods;
        e.Capture(this); // Capture panel so we get mouse move (same as TreeView capturing itself)
        e.Stop();
    });
    
    // Row does not start the drag; panel's kEventMouseMove does when it has capture.
    row->RegisterEventHandler(kEventMouseMove, [this, row, index](UIEvent& e)
    {
        (void)row;
        (void)index;
        if (!m_DragPending)
            return;
        e.Stop();
    });
        
    row->RegisterEventHandler(kEventMouseUp, [this, row, index](UIEvent& e)
        {
        if (e.Button != 0)
                return;
        if (m_DragSourceRow == row)
            m_DragSourceRow = nullptr;
        const bool wasClick = m_DragPending && m_DragPendingIndex == index;
        m_DragPending = false;
        if (wasClick)
        {
            const bool addToSelection = (m_DragPendingMods & (Input::kModControl | Input::kModSuper)) != 0;
            const bool rangeSelect = (m_DragPendingMods & Input::kModShift) != 0;
            if (addToSelection)
            {
                if (m_SelectedIndices.count(index))
                    m_SelectedIndices.erase(index);
                else
                    m_SelectedIndices.insert(index);
                m_SelectionAnchor = index;
                ApplySelectionVisuals();
            }
            else if (rangeSelect)
            {
                if (m_SelectionAnchor >= m_Bookmarks.size())
                    m_SelectionAnchor = index;
                const size_t lo = std::min(m_SelectionAnchor, index);
                const size_t hi = std::max(m_SelectionAnchor, index);
                m_SelectedIndices.clear();
                for (size_t i = lo; i <= hi; ++i)
                    m_SelectedIndices.insert(i);
                m_SelectionAnchor = index;
                ApplySelectionVisuals();
            }
            else
            {
                m_SelectedIndices = { index };
                m_SelectionAnchor = index;
                ApplySelectionVisuals();
                NavigateToBookmark(index);
            }
        }
        e.Stop();
    });
}

bool BookmarksPanel::AcceptsPayload(UI::Interaction::PayloadTypeId typeId) const
{
    return typeId == UI::Interaction::GetPayloadTypeId<Editor::BookmarkReorderPayload>() ||
           typeId == UI::Interaction::GetPayloadTypeId<Editor::BookmarkDragPayload>() ||
           typeId == UI::Interaction::GetPayloadTypeId<Editor::AssetPathsDragPayload>() ||
           typeId == UI::Interaction::GetPayloadTypeId<Editor::OnlineAssetDragPayload>() ||
           typeId == UI::Interaction::GetPayloadTypeId<Editor::HierarchyEntityDragPayload>();
}

bool BookmarksPanel::HitTestDropTarget(float x, float y, UI::Interaction::DropHit& out) const
{
    // Match TreeView: only accept when pointer is inside this panel (absolute coords).
    const float px = GetLayoutX();
    const float py = GetLayoutY();
    const float pw = GetLayoutWidth();
    const float ph = GetLayoutHeight();
    if (x < px || x >= px + pw || y < py || y >= py + ph)
        return false;

    if (!m_ListContainer)
        return false;

    // Build list of rows (id, index) sorted by Y, like TreeView's row pool.
    std::vector<std::pair<UIElement*, size_t>> rows;
    for (const auto& child : m_ListContainer->GetChildren())
    {
        if (!child->HasClass("bookmark-row"))
            continue;
        std::string id = child->GetId();
        size_t dash = id.rfind('-');
        if (dash == std::string::npos || dash + 1 >= id.size())
            continue;
        try
        {
            size_t idx = static_cast<size_t>(std::stoull(id.substr(dash + 1)));
            rows.push_back({child.get(), idx});
        }
        catch (...)
        {
        }
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.first->GetLayoutY() < b.first->GetLayoutY(); });
    if (rows.empty())
    {
        // Empty list: accept drop at position 0
        out.TargetId = 0;
        out.Location = UI::Interaction::DropLocation::BeforeItem;
        out.IndentDepth = 0;
        return true;
    }

    // Find row under (x,y); first hit wins (like TreeView).
    UIElement* bestRow = nullptr;
    size_t bestId = 0;
    float bestRowY = 0.0f;
    float bestRowH = 0.0f;
    for (const auto& p : rows)
    {
        const float rx = p.first->GetLayoutX();
        const float ry = p.first->GetLayoutY();
        const float rw = p.first->GetLayoutWidth();
        const float rh = p.first->GetLayoutHeight();
        if (x >= rx && x < rx + rw && y >= ry && y < ry + rh)
        {
            bestRow = p.first;
            bestId = p.second;
            bestRowY = ry;
            bestRowH = rh;
                break;
            }
        }
        
    if (!bestRow)
    {
        // Inside panel but not over a row: first row Before or last row After.
        const float midY = rows[0].first->GetLayoutY() + rows[0].first->GetLayoutHeight() * 0.5f;
        if (y < midY)
        {
            out.TargetId = static_cast<UI::Interaction::ItemId>(rows[0].second);
            out.Location = UI::Interaction::DropLocation::BeforeItem;
        }
        else
        {
            out.TargetId = static_cast<UI::Interaction::ItemId>(rows.back().second);
            out.Location = UI::Interaction::DropLocation::AfterItem;
        }
        out.IndentDepth = 0;
        return true;
    }

    // Per-row hit: band like TreeView (top 25% = Before, bottom 25% = After).
    const float band = bestRowH * 0.25f;
    if (y < bestRowY + band)
        out.Location = UI::Interaction::DropLocation::BeforeItem;
    else if (y >= bestRowY + bestRowH - band)
        out.Location = UI::Interaction::DropLocation::AfterItem;
    else
        out.Location = UI::Interaction::DropLocation::AfterItem;
    out.TargetId = static_cast<UI::Interaction::ItemId>(bestId);
    out.IndentDepth = 0;
    return true;
}

UI::Interaction::DropFeedback BookmarksPanel::CanDrop(const UI::Interaction::DropRequest& request) const
{
    if (const Editor::AssetPathsDragPayload* assetPl = request.payload.TryGet<Editor::AssetPathsDragPayload>())
    {
        if (assetPl->paths.empty())
            return {false, "No asset"};
        if (!m_Context)
            return {false, "No context"};
        return {true, ""};
    }

    if (request.payload.Is<Editor::OnlineAssetDragPayload>())
    {
        if (!m_Context)
            return {false, "No context"};
        return {true, ""};
    }

    if (const Editor::HierarchyEntityDragPayload* entPl = request.payload.TryGet<Editor::HierarchyEntityDragPayload>())
    {
        if (entPl->treeIds.empty())
            return {false, "No entities"};
        return {true, ""};
    }

    const Editor::BookmarkReorderPayload* reorderPl = request.payload.TryGet<Editor::BookmarkReorderPayload>();
    const Editor::BookmarkDragPayload* dragPl = request.payload.TryGet<Editor::BookmarkDragPayload>();
    const size_t fromIdx = reorderPl ? reorderPl->bookmarkIndex : (dragPl ? dragPl->bookmarkIndex : m_Bookmarks.size());
    if (fromIdx >= m_Bookmarks.size())
        return {false, "Invalid payload"};
    int fromIndex = static_cast<int>(fromIdx);
    int toIndex = (request.hit.Location == UI::Interaction::DropLocation::AfterItem)
                      ? static_cast<int>(request.hit.TargetId) + 1
                      : static_cast<int>(request.hit.TargetId);
    toIndex = std::max(0, std::min(toIndex, static_cast<int>(m_Bookmarks.size())));
    if (fromIndex == toIndex || fromIndex + 1 == toIndex)
        return {false, "Same position"};
    return {true, ""};
}

void BookmarksPanel::PerformDrop(const UI::Interaction::DropRequest& request)
{
    if (const Editor::HierarchyEntityDragPayload* entPl = request.payload.TryGet<Editor::HierarchyEntityDragPayload>())
    {
        if (entPl->treeIds.empty())
            return;
        int insertIndex = (request.hit.Location == UI::Interaction::DropLocation::AfterItem)
                              ? static_cast<int>(request.hit.TargetId) + 1
                              : static_cast<int>(request.hit.TargetId);
        insertIndex = std::max(0, std::min(insertIndex, static_cast<int>(m_Bookmarks.size())));
        HandleEntityPayloadDrop(entPl->treeIds, insertIndex);
        return;
    }

    if (const Editor::AssetPathsDragPayload* assetPl = request.payload.TryGet<Editor::AssetPathsDragPayload>())
    {
        if (assetPl->paths.empty() || !m_Context)
            return;
        int insertIndex = (request.hit.Location == UI::Interaction::DropLocation::AfterItem)
                              ? static_cast<int>(request.hit.TargetId) + 1
                              : static_cast<int>(request.hit.TargetId);
        insertIndex = std::max(0, std::min(insertIndex, static_cast<int>(m_Bookmarks.size())));
        HandleAssetDrops(assetPl->paths, insertIndex);
        // Immediate rebuild + next-frame rebuild so first drop always shows.
        if (m_ListContainer)
        {
            RebuildBookmarkRows();
            m_ListContainer->MarkDirty(UIElement::ChildrenDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
            if (m_ScrollView)
                m_ScrollView->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
            MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
            RequestRelayout();
            Scheduler::IScheduler* sched = GetScheduler();
            if (sched)
            {
                sched->ScheduleNext([this]()
                {
                    if (!m_ListContainer)
                        return;
                    RebuildBookmarkRows();
                    m_ListContainer->MarkDirty(UIElement::ChildrenDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
                    if (m_ScrollView)
                        m_ScrollView->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
                    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
                    RequestRelayout();
                });
            }
        }
        return;
    }

    if (const Editor::OnlineAssetDragPayload* onlinePl = request.payload.TryGet<Editor::OnlineAssetDragPayload>())
    {
        if (!m_Context || m_Context->AssetsRoot.empty())
            return;
        int insertIndex = (request.hit.Location == UI::Interaction::DropLocation::AfterItem)
                              ? static_cast<int>(request.hit.TargetId) + 1
                              : static_cast<int>(request.hit.TargetId);
        insertIndex = std::max(0, std::min(insertIndex, static_cast<int>(m_Bookmarks.size())));

        const auto& assetsRoot = m_Context->AssetsRoot;

        // Collect items: single-item back-compat or multi-item entries.
        std::vector<Editor::OnlineAssetEntry> items;
        if (!onlinePl->entries.empty())
            items = onlinePl->entries;
        else if (!onlinePl->slug.empty())
            items.push_back({onlinePl->slug, onlinePl->name});

        for (size_t i = 0; i < items.size(); ++i)
        {
            const auto& item = items[i];
            const int at = insertIndex + static_cast<int>(i);

            // Check if already downloaded — if so, add as a normal asset bookmark.
            std::filesystem::path mainFilePath = PolyhavenService::FindDownloadedFile(item.slug, assetsRoot);
            if (mainFilePath.empty())
            {
                std::filesystem::path cachedFile = PolyhavenService::FindCachedDownloadFile(item.slug);
                if (!cachedFile.empty())
                {
                    std::filesystem::path projectDir = assetsRoot / "Polyhaven" / item.slug;
                    mainFilePath = PolyhavenService::MoveDownloadToProject(
                        cachedFile, cachedFile.parent_path(), projectDir);
                }
            }

            if (!mainFilePath.empty())
            {
                HandleAssetDrop(mainFilePath, at);
                continue;
            }

            // Not downloaded yet — add bookmark immediately as an online asset reference.
            // Use "polyhaven:<slug>" so we can resolve it later when downloaded.
            Bookmark bookmark;
            bookmark.Type = BookmarkType::Asset;
            bookmark.Reference = "polyhaven:" + item.slug;
            bookmark.Name = item.name.empty() ? item.slug : item.name;

            // Use cached Polyhaven thumbnail if available.
            std::filesystem::path thumbPath = PolyhavenService::GetCacheDir() / (item.slug + ".png");
            std::error_code ec;
            if (std::filesystem::exists(thumbPath, ec))
                bookmark.IconPath = thumbPath.string();

            AddBookmarkAt(bookmark, static_cast<size_t>(at));

            // Start background download so the asset becomes available later.
            if (m_Context->DownloadManager &&
                !m_Context->DownloadManager->IsDownloadingOrCompleted(item.slug))
            {
                m_Context->DownloadManager->StartEarlyDownload(item.slug, onlinePl->type, assetsRoot);
            }
        }

        RebuildBookmarkRows();
        if (m_ListContainer)
            m_ListContainer->MarkDirty(UIElement::ChildrenDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
        MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        RequestRelayout();
        return;
    }

    const Editor::BookmarkReorderPayload* reorderPl = request.payload.TryGet<Editor::BookmarkReorderPayload>();
    const Editor::BookmarkDragPayload* dragPl = request.payload.TryGet<Editor::BookmarkDragPayload>();
    const size_t fromIdx = reorderPl ? reorderPl->bookmarkIndex : (dragPl ? dragPl->bookmarkIndex : m_Bookmarks.size());
    if (fromIdx >= m_Bookmarks.size())
        return;
    int fromIndex = static_cast<int>(fromIdx);
    int toIndex = (request.hit.Location == UI::Interaction::DropLocation::AfterItem)
                      ? static_cast<int>(request.hit.TargetId) + 1
                      : static_cast<int>(request.hit.TargetId);
    toIndex = std::max(0, std::min(toIndex, static_cast<int>(m_Bookmarks.size())));
    if (fromIndex == toIndex || fromIndex + 1 == toIndex)
        return;
    ReorderBookmarks(fromIndex, toIndex);
    if (m_Undo)
        m_Undo->CommitAlreadyApplied(
            std::make_unique<Editor::ReorderBookmarksCommand>(this, fromIndex, toIndex));
}

void BookmarksPanel::SetDropPreview(const UI::Interaction::DropPreviewState& state)
{
    // Clear row drop classes from all bookmark rows (like ListView UpdateDropPreviewClasses).
    if (m_ListContainer)
    {
        for (const auto& child : m_ListContainer->GetChildren())
        {
            if (!child->HasClass("bookmark-row"))
                continue;
            child->RemoveClass("drop-hover");
            child->RemoveClass("drop-allowed");
            child->RemoveClass("drop-denied");
        }
    }

    // Match TreeView: show line only for Before/After, one inline style (green = allowed, red = denied).
    const bool wantsLine =
        m_InsertionIndicator && m_ListContainer && state.Visible &&
        (state.Hit.Location == UI::Interaction::DropLocation::BeforeItem ||
         state.Hit.Location == UI::Interaction::DropLocation::AfterItem);

    if (!wantsLine)
    {
        if (m_InsertionIndicator)
            m_InsertionIndicator->Overrides().Set(Style::Display, DisplayMode::None);
        return;
    }

    // Normalize to a single insertion index so we only highlight one row (fixes "red on two rows"
    // when the same gap is reported as "after row N" vs "before row N+1").
    const size_t targetId = static_cast<size_t>(state.Hit.TargetId);
    const int toIndex = (state.Hit.Location == UI::Interaction::DropLocation::AfterItem)
                            ? static_cast<int>(targetId) + 1
                            : static_cast<int>(targetId);
    const int insertIndex = std::max(0, std::min(toIndex, static_cast<int>(m_Bookmarks.size())));
    const size_t rowToHighlight = (insertIndex < static_cast<int>(m_Bookmarks.size()))
                                      ? static_cast<size_t>(insertIndex)
                                      : (m_Bookmarks.empty() ? 0 : m_Bookmarks.size() - 1);
    const bool lineBelowRow = (insertIndex >= static_cast<int>(m_Bookmarks.size()) && !m_Bookmarks.empty());

    UIElement* row = m_ListContainer->FindById("bookmark-row-" + std::to_string(rowToHighlight));
    if (!row)
    {
        if (m_InsertionIndicator)
            m_InsertionIndicator->Overrides().Set(Style::Display, DisplayMode::None);
        return;
    }

    // Paint only the one row at the insertion index (drop-hover / drop-allowed / drop-denied).
    row->AddClass("drop-hover");
    row->AddClass(state.Allowed ? "drop-allowed" : "drop-denied");

    // Draw insertion line above that row, or below when dropping at end (single line for this insertion point).
    const float ry = row->GetLayoutY();
    const float rh = row->GetLayoutHeight();
    const float containerY = m_ListContainer->GetLayoutY();
    const float topPx = (lineBelowRow ? (ry + rh) : ry) - containerY;

    const uint32_t lineArgb = state.Allowed ? 0xF200C864u : 0xF2FF5050u; // rgba(0,200,100,0.95) / rgba(255,80,80,0.95)
    m_InsertionIndicator->Overrides()
        .Set(Style::PositionLeft, StyleLength::Px(0.0f))
        .Set(Style::PositionTop, StyleLength::Px(topPx))
        .Set(Style::Width, StyleLength::Percent(100.0f))
        .Set(Style::Height, StyleLength::Px(2.0f))
        .Set(Style::BackgroundColor, (uint32_t)lineArgb)
        .Set(Style::ZIndex, 9999)
        .Set(Style::Display, DisplayMode::Block);
}

void BookmarksPanel::AddBookmark(const Bookmark& bookmark)
{
    AddBookmarkAt(bookmark, 0);
}

namespace
{
// Normalize reference for duplicate check so path variants and GUID formats match.
std::string NormalizeReferenceForDuplicateCheck(const Bookmark& bm)
{
    const std::string& ref = bm.Reference;
    if (ref.empty())
        return ref;
    // Path-like: contains slash or backslash
    if (ref.find('/') != std::string::npos || ref.find('\\') != std::string::npos)
    {
        try
        {
            return std::filesystem::path(ref).generic_string();
        }
        catch (...)
        {
            return ref;
        }
    }
    // Asset and Scene use GUID; canonicalize so different string forms (braces, case) match.
    if (bm.Type == BookmarkType::Asset || bm.Type == BookmarkType::Scene)
    {
        std::string clean = ref;
        if (clean.size() >= 2 && clean.front() == '{' && clean.back() == '}')
            clean = clean.substr(1, clean.size() - 2);
        GUID guid(clean);
        if (!guid.IsNull())
            return guid.ToString();
    }
    return ref; // Entity or other - compare as-is
}
} // namespace

void BookmarksPanel::AddBookmarkAt(const Bookmark& bookmark, size_t insertIndex)
{
    // Reject duplicates: same type + same logical reference (normalize paths so C:/foo and C:\foo match).
    const std::string newRefNorm = NormalizeReferenceForDuplicateCheck(bookmark);
    for (const Bookmark& existing : m_Bookmarks)
    {
        if (existing.Type != bookmark.Type)
            continue;
        if (NormalizeReferenceForDuplicateCheck(existing) == newRefNorm)
            return; // Already bookmarked; do not add duplicate
    }
    
    size_t index = insertIndex;
    if (index > m_Bookmarks.size())
        index = m_Bookmarks.size();
    
    Bookmark newBookmark = bookmark;
    // Store canonical reference so future duplicate checks and lookups are reliable
    if (!newRefNorm.empty() && (bookmark.Reference.find('/') != std::string::npos || bookmark.Reference.find('\\') != std::string::npos))
        newBookmark.Reference = newRefNorm;
    m_Bookmarks.insert(m_Bookmarks.begin() + index, newBookmark);
    
    // Reassign orders so they match vector order
    for (size_t i = 0; i < m_Bookmarks.size(); ++i)
        m_Bookmarks[i].Order = static_cast<int>(i);
    
    SaveBookmarks();
    
    // Rebuild UI so the added item shows. When in event dispatch (e.g. drop), RemoveChild
    // defers in the UI layer, so defer the full rebuild to run right after the event.
    if (m_ListContainer)
    {
        if (UIElement::IsInEventDispatch())
        {
            // Schedule rebuild at the start of the NEXT frame so layout/paint run after our
            // changes. PostAction runs after event dispatch in the same frame, but layout/paint
            // have already run for this frame, so the new bookmark only appears after focusing
            // the panel. ScheduleNext runs in the next Update()'s first drain, before layout/paint.
            Scheduler::IScheduler* sched = GetScheduler();
            if (sched)
            {
                sched->ScheduleNext([this]()
                {
                    if (!m_ListContainer)
                        return;
            RebuildBookmarkRows();
                    if (m_ListContainer)
                        m_ListContainer->MarkDirty(UIElement::ChildrenDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
                    if (m_ScrollView)
                        m_ScrollView->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
                    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
                    RequestRelayout();
        });
    }
    else
    {
                PostAction([this]()
                {
                    if (!m_ListContainer)
                        return;
        RebuildBookmarkRows();
                    if (m_ListContainer)
                        m_ListContainer->MarkDirty(UIElement::ChildrenDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
                    if (m_ScrollView)
                        m_ScrollView->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
                    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
                    RequestRelayout();
                });
            }
        }
        else
        {
            RebuildBookmarkRows();
        }
    }
}

void BookmarksPanel::RemoveBookmark(size_t index)
{
    if (index >= m_Bookmarks.size())
        return;
    
    // Update selection: remove this index and shift indices above
    m_SelectedIndices.erase(index);
    std::set<size_t> adjusted;
    for (size_t i : m_SelectedIndices)
    {
        if (i > index)
            adjusted.insert(i - 1);
        else
            adjusted.insert(i);
    }
    m_SelectedIndices = std::move(adjusted);
    if (m_SelectionAnchor == index)
        m_SelectionAnchor = m_SelectedIndices.empty() ? 0 : *m_SelectedIndices.begin();
    else if (m_SelectionAnchor > index)
        --m_SelectionAnchor;
    
    // Remove the bookmark
    m_Bookmarks.erase(m_Bookmarks.begin() + index);
    
    // Defer UI rebuild and save to avoid blocking
    if (m_ListContainer)
    {
        // Capture current state to avoid issues if panel is destroyed
        UIElement* listContainer = m_ListContainer;
        listContainer->PostAction([this, listContainer]()
        {
            // Double-check that we still have a valid list container and it matches
            if (m_ListContainer == listContainer && m_ListContainer)
            {
                RebuildBookmarkRows();
                SaveBookmarks();
            }
        });
    }
    else
    {
        RebuildBookmarkRows();
        SaveBookmarks();
    }
}

void BookmarksPanel::ReorderBookmarks(int fromIndex, int toIndex)
{
    if (fromIndex < 0 || fromIndex >= static_cast<int>(m_Bookmarks.size()) ||
        toIndex < 0 || toIndex > static_cast<int>(m_Bookmarks.size()))
        return;
    
    if (fromIndex == toIndex)
        return;
    
    // Move bookmark: erase first, then insert. When moving down (fromIndex < toIndex), erasing
    // shrinks the vector so the insert position becomes toIndex - 1 (avoid out-of-bounds).
    Bookmark moved = m_Bookmarks[fromIndex];
    m_Bookmarks.erase(m_Bookmarks.begin() + fromIndex);
    const int insertPos = (fromIndex < toIndex) ? (toIndex - 1) : toIndex;
    m_Bookmarks.insert(m_Bookmarks.begin() + insertPos, moved);
    
    // Reassign orders
    for (size_t i = 0; i < m_Bookmarks.size(); ++i)
        m_Bookmarks[i].Order = static_cast<int>(i);
    
    // Defer UI rebuild and save to avoid blocking
    if (m_ListContainer)
    {
        m_ListContainer->PostAction([this]()
        {
            RebuildBookmarkRows();
            SaveBookmarks();
        });
    }
    else
    {
        RebuildBookmarkRows();
        SaveBookmarks();
    }
}

void BookmarksPanel::NavigateToBookmark(size_t index)
{
    if (index >= m_Bookmarks.size())
        return;
    NavigateToBookmark(m_Bookmarks[index]);
}
    
void BookmarksPanel::NavigateToBookmark(const Bookmark& bookmark)
{
    switch (bookmark.Type)
    {
    case BookmarkType::Asset:
    {
        // Online asset reference (not yet downloaded or needs resolution).
        if (bookmark.Reference.rfind("polyhaven:", 0) == 0)
        {
            const std::string slug = bookmark.Reference.substr(10);
            if (m_Context && !m_Context->AssetsRoot.empty())
            {
                // Check if it has been downloaded since bookmarking.
                std::filesystem::path filePath = PolyhavenService::FindDownloadedFile(slug, m_Context->AssetsRoot);
                if (!filePath.empty())
                {
                    // Upgrade bookmark to a real asset reference and navigate.
                    HandleAssetDrop(filePath, -1); // -1 = append; won't duplicate because we navigate
                    // TODO: could replace the polyhaven: reference with the real GUID here
                    if (m_AssetsPanel)
                        m_AssetsPanel->NavigateToAndSelectAsset(filePath);
                }
            }
            break;
        }
        if (std::filesystem::exists(bookmark.Reference) && std::filesystem::is_directory(bookmark.Reference))
        {
            if (m_AssetsPanel)
                m_AssetsPanel->NavigateToAndSelectAsset(std::filesystem::path(bookmark.Reference));
        }
        else
            NavigateToAsset(bookmark.Reference);
        break;
    }
    case BookmarkType::Scene:
        NavigateToScene(bookmark.Reference);
        break;
    case BookmarkType::Entity:
    {
        size_t sep = bookmark.Reference.find('|');
        if (sep != std::string::npos)
        {
            std::string scenePath = bookmark.Reference.substr(0, sep);
            std::string entityId = bookmark.Reference.substr(sep + 1);
            NavigateToEntity(scenePath, entityId);
        }
        break;
    }
    }
}

void BookmarksPanel::NavigateToAsset(const std::string& guidStr)
{
    if (!m_AssetsPanel || guidStr.empty())
        return;
    
    // Convert GUID string to GUID
    GUID guid(guidStr);
    if (guid.IsNull())
        return;
    
    // Get asset path from GUID
    auto& engine = EngineCore::GetInstance();
    AssetRegistry& registry = engine.GetAssetManager().GetRegistry();
    AssetMetadata metadata;
    if (!registry.TryGetAssetMetadata(guid, metadata))
        return;
    
    if (metadata.Path.empty())
        return;
    
    // Navigate to and select the asset
    m_AssetsPanel->NavigateToAndSelectAsset(metadata.Path);
}

void BookmarksPanel::NavigateToScene(const std::string& guidStr)
{
    if (!m_AssetsPanel || guidStr.empty())
        return;
    
    // Convert GUID string to GUID
    GUID guid(guidStr);
    if (guid.IsNull())
        return;
    
    // Get scene path from GUID
    auto& engine = EngineCore::GetInstance();
    AssetRegistry& registry = engine.GetAssetManager().GetRegistry();
    AssetMetadata metadata;
    if (!registry.TryGetAssetMetadata(guid, metadata))
        return;
    
    if (metadata.Path.empty())
        return;
    
    // Navigate to and select the scene (same as asset)
    m_AssetsPanel->NavigateToAndSelectAsset(metadata.Path);
}

void BookmarksPanel::NavigateToEntity(const std::string& scenePath, const std::string& entityId)
{
    if (!m_HierarchyPanel || entityId.empty())
        return;

    // If the bookmark has a scene path, check whether it matches the currently loaded scene.
    if (!scenePath.empty() && m_GetCurrentScenePath && m_OnOpenScene)
    {
        auto currentOpt = m_GetCurrentScenePath();
        bool sameScene = false;
        if (currentOpt.has_value())
        {
            try
            {
                sameScene = std::filesystem::equivalent(*currentOpt, std::filesystem::path(scenePath));
            }
            catch (...)
            {
                sameScene = (currentOpt->string() == scenePath);
            }
        }

        if (!sameScene)
        {
            // Lazily create the confirm modal (child UIElement, not owned by shared_ptr)
            if (!m_OpenSceneConfirm)
            {
                auto modal = std::make_unique<ConfirmActionModal>();
                m_OpenSceneConfirm = modal.get();
                AddChild(std::move(modal));
                m_OpenSceneConfirm->SetOnCancel([this]()
                {
                    m_PendingEntityNavScenePath.clear();
                    m_PendingEntityNavEntityId.clear();
                });
            }

            m_PendingEntityNavScenePath = std::filesystem::path(scenePath);
            m_PendingEntityNavEntityId = entityId;

            m_OpenSceneConfirm->SetOnConfirm([this]()
            {
                const std::filesystem::path sp = m_PendingEntityNavScenePath;
                const std::string eid = m_PendingEntityNavEntityId;
                m_PendingEntityNavScenePath.clear();
                m_PendingEntityNavEntityId.clear();
                if (m_OnOpenScene)
                {
                    m_OnOpenScene(sp);
                    // Schedule selection for the next 2 frames to let the scene load
                    Scheduler::IScheduler* sched = GetScheduler();
                    if (sched)
                    {
                        sched->ScheduleNext([this, eid]()
                        {
                            Scheduler::IScheduler* s = GetScheduler();
                            if (s)
                                s->ScheduleNext([this, eid]() { SelectEntityById(eid); });
                        });
                    }
                }
            });

            const std::string sceneName = std::filesystem::path(scenePath).filename().string();
            m_OpenSceneConfirm->Show(
                "Open Scene",
                "The entity belongs to \"" + sceneName + "\".\nOpen this scene?",
                "Open Scene");
            return;
        }
    }

    SelectEntityById(entityId);
}

void BookmarksPanel::SelectEntityById(const std::string& entityId)
{
    if (!m_HierarchyPanel || entityId.empty())
        return;

    ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    if (!world)
        return;

    size_t colonPos = entityId.find(':');
    ECS::EntityIndex index = 0;
    ECS::EntityVersion version = 0;

    if (colonPos != std::string::npos)
    {
        index = static_cast<ECS::EntityIndex>(std::stoul(entityId.substr(0, colonPos)));
        version = static_cast<ECS::EntityVersion>(std::stoul(entityId.substr(colonPos + 1)));
    }
    else
    {
        index = static_cast<ECS::EntityIndex>(std::stoul(entityId));
    }

    ECS::EntityHandle entity(index, version);
    if (!world->IsValid(entity))
        return;

    m_HierarchyPanel->SelectEntity(entity);
}

void BookmarksPanel::UpdateBookmarkIcon(UIElement* iconEl, const Bookmark& bookmark)
{
    if (!iconEl)
        return;
    
    // Check if iconPath is explicitly set (e.g., for folders)
    if (!bookmark.IconPath.empty())
    {
        UI::Layout::SetBackgroundPath(*iconEl, bookmark.IconPath);
        return;
    }
    
    if (bookmark.Type == BookmarkType::Asset && m_Context && m_Context->Thumbnails)
    {
        // Online asset reference — use cached Polyhaven thumbnail or default icon.
        if (bookmark.Reference.rfind("polyhaven:", 0) == 0)
        {
            const std::string slug = bookmark.Reference.substr(10);
            std::filesystem::path thumbPath = PolyhavenService::GetCacheDir() / (slug + ".png");
            std::error_code ec;
            if (std::filesystem::exists(thumbPath, ec))
                UI::Layout::SetBackgroundPath(*iconEl, thumbPath.string());
            else
                iconEl->AddClass(GetDefaultIconForType(bookmark.Type));
            return;
        }

        std::filesystem::path assetPath;
        bool isFolder = false;

        // Check if reference is a folder path (exists as directory)
        try
        {
            if (std::filesystem::exists(bookmark.Reference) && std::filesystem::is_directory(bookmark.Reference))
            {
                // It's a folder - use the path directly
                assetPath = std::filesystem::path(bookmark.Reference);
                isFolder = true;
            }
        }
        catch (...)
        {
            // Path check failed, try GUID approach
        }
        
        if (!isFolder)
        {
            // Try to get asset path from GUID
            auto& engine = EngineCore::GetInstance();
            AssetRegistry& registry = engine.GetAssetManager().GetRegistry();
            
            GUID guid(bookmark.Reference);
            if (!guid.IsNull())
            {
                AssetMetadata metadata;
                if (registry.TryGetAssetMetadata(guid, metadata))
                {
                    assetPath = metadata.Path;
                    // Check if the asset path is actually a directory
                    try
                    {
                        if (std::filesystem::exists(assetPath) && std::filesystem::is_directory(assetPath))
                        {
                            isFolder = true;
                        }
                    }
                    catch (...)
                    {
                        // Ignore filesystem errors
                    }
                }
            }
        }
        
        if (!assetPath.empty())
        {
            // For folders, use folder icon instead of requesting thumbnail
            if (isFolder)
            {
                // Use folder icon class
                iconEl->AddClass("bookmark-icon-folder");
                return;
            }
            
            // Request thumbnail for files - use row height for thumbnail size
            int thumbnailSize = static_cast<int>(std::max(24.0f, std::min(256.0f, m_RowHeight)));
            
            // Store instance ID for safe async access instead of raw pointer
            uint64_t iconInstanceId = iconEl->GetInstanceId();
            std::string iconId = iconEl->GetId();
            
            // Set fallback icon first (will be replaced when thumbnail loads)
            std::string fallbackClass = GetDefaultIconForBookmark(bookmark);
            if (!fallbackClass.empty())
            {
                iconEl->AddClass(fallbackClass);
            }
            
            auto immediate = m_Context->Thumbnails->GetOrRequest(
                assetPath, thumbnailSize,
                [this, iconInstanceId, iconId, assetPath, fallbackClass, post = GetPostHandle()](const std::string& rel)
                {
                    post.Post([this, iconInstanceId, iconId, rel, fallbackClass]()
                    {
                        // Safety check: ensure panel still has owner manager (not destroyed)
                        if (!GetOwnerManager())
                            return;
                        
                        // Find element by instance ID or ID to ensure it still exists
                        UIElement* iconEl = nullptr;
                        if (iconInstanceId != 0)
                        {
                            iconEl = GetOwnerManager()->FindElementByInstanceId(iconInstanceId);
                        }
                        else if (!iconId.empty())
                        {
                            iconEl = FindById(iconId);
                        }
                        
                        // Verify element is still valid.
                        if (iconEl)
                        {
                            if (!rel.empty())
                            {
                                // Remove fallback class now that we have a real thumbnail
                                if (!fallbackClass.empty())
                                {
                                    iconEl->RemoveClass(fallbackClass);
                                }
                                if (iconEl)
                                {
                                    // Check for engine texture (3D models return "engine:..." paths)
                                    constexpr const char* kEnginePrefix = "engine:";
                                    constexpr size_t kEnginePrefixLen = 7;
                                    if (rel.rfind(kEnginePrefix, 0) == 0)
                                    {
                                        UI::Layout::SetBackgroundResourceName(*iconEl, rel.substr(kEnginePrefixLen));
                                    }
                                    else
                                    {
                                        UI::Layout::SetBackgroundPath(*iconEl, rel);
                                    }
                                }
                            }
                            // If rel is empty, keep the fallback icon that was already set
                        }
                    });
                });
            
            if (!immediate.empty() && iconEl)
            {
                // Remove fallback class since we have immediate thumbnail
                if (!fallbackClass.empty())
                {
                    iconEl->RemoveClass(fallbackClass);
                }
                if (iconEl)
                {
                    // Check for engine texture (3D models return "engine:..." paths)
                    constexpr const char* kEnginePrefix = "engine:";
                    constexpr size_t kEnginePrefixLen = 7;
                    if (immediate.rfind(kEnginePrefix, 0) == 0)
                    {
                        UI::Layout::SetBackgroundResourceName(*iconEl, immediate.substr(kEnginePrefixLen));
                    }
                    else
                    {
                        UI::Layout::SetBackgroundPath(*iconEl, immediate);
                    }
                }
            }
            return;
        }
    }
    
    // For entity bookmarks: try to decode the entity and apply hierarchy entity icon classes
    // so the icon matches what the hierarchy panel shows (e.g. sphere, camera, light, etc.)
    if (bookmark.Type == BookmarkType::Entity)
    {
        size_t sep = bookmark.Reference.find('|');
        if (sep != std::string::npos)
        {
            const std::string entityId = bookmark.Reference.substr(sep + 1);
            size_t colon = entityId.find(':');
            try
            {
                ECS::EntityIndex idx = static_cast<ECS::EntityIndex>(std::stoul(entityId.substr(0, colon)));
                ECS::EntityVersion ver = (colon != std::string::npos)
                    ? static_cast<ECS::EntityVersion>(std::stoul(entityId.substr(colon + 1)))
                    : 0;
                ECS::EntityHandle handle(idx, ver);
                ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
                if (world && world->IsValid(handle))
                {
                    Editor::ApplyHierarchyEntityIconClassesToIcon(world, iconEl, handle);
                    return;
                }
            }
            catch (...) {}
        }
        // Entity not in current scene — show generic entity icon
        iconEl->AddClass("bookmark-icon-entity");
        return;
    }

    // Fallback to default icon (for scenes, or assets without thumbnails context)
    std::string iconClass = GetDefaultIconForBookmark(bookmark);
    if (!iconClass.empty())
    {
        iconEl->AddClass(iconClass);
    }
}

std::string BookmarksPanel::GetDefaultIconForType(BookmarkType type) const
{
    switch (type)
    {
    case BookmarkType::Asset:
        return "bookmark-icon-asset";
    case BookmarkType::Scene:
        return "bookmark-icon-scene";
    case BookmarkType::Entity:
        return "bookmark-icon-entity";
    }
    return "";
}

std::string BookmarksPanel::GetDefaultIconForBookmark(const Bookmark& bookmark) const
{
    if (bookmark.Type == BookmarkType::Asset && m_Context)
    {
        // Online asset references don't have a GUID to look up.
        if (bookmark.Reference.rfind("polyhaven:", 0) == 0)
            return GetDefaultIconForType(bookmark.Type);

        auto& engine = EngineCore::GetInstance();
        AssetRegistry& registry = engine.GetAssetManager().GetRegistry();
        GUID guid(bookmark.Reference);
        if (!guid.IsNull())
        {
            AssetMetadata metadata;
            if (registry.TryGetAssetMetadata(guid, metadata))
            {
                if (metadata.Type == AssetType::Scene)
                    return "bookmark-icon-scene";
                if (metadata.Type == AssetType::Script)
                    return "bookmark-icon-script";
            }
        }
    }
    return GetDefaultIconForType(bookmark.Type);
}

size_t BookmarksPanel::FindBookmarkIndex(const Bookmark& bookmark) const
{
    const std::string refNorm = NormalizeReferenceForDuplicateCheck(bookmark);
    for (size_t i = 0; i < m_Bookmarks.size(); ++i)
    {
        if (m_Bookmarks[i].Type != bookmark.Type)
            continue;
        if (NormalizeReferenceForDuplicateCheck(m_Bookmarks[i]) == refNorm)
            return i;
    }
    return m_Bookmarks.size();
}

size_t BookmarksPanel::FindBookmarkIndex(const Bookmark& bookmark, size_t preferredIndex) const
{
    const std::string refNorm = NormalizeReferenceForDuplicateCheck(bookmark);
    if (preferredIndex < m_Bookmarks.size() && m_Bookmarks[preferredIndex].Type == bookmark.Type
        && NormalizeReferenceForDuplicateCheck(m_Bookmarks[preferredIndex]) == refNorm)
        return preferredIndex;
    return FindBookmarkIndex(bookmark);
}

void BookmarksPanel::SaveBookmarks()
{
    // Save is already deferred by the caller, so execute directly
    // Save to project-specific file like smart folders do
    auto projectPaths = Editor::GetCurrentEditorProjectPaths();
    if (projectPaths.projectEditorRoot.empty())
    {
        Logger::Log::Warning("[BookmarksPanel] Cannot save bookmarks: no project root");
        return;
    }
    
    auto bookmarksPath = projectPaths.projectEditorRoot / BookmarksKeys::kFileName;
    Editor::SettingsStore store(bookmarksPath);
    
    nlohmann::json bookmarksJson = nlohmann::json::array();
    for (const auto& bookmark : m_Bookmarks)
    {
        nlohmann::json bmJson;
        bmJson["type"] = static_cast<int>(bookmark.Type);
        bmJson["reference"] = bookmark.Reference;
        bmJson["name"] = bookmark.Name;
        bmJson["iconPath"] = bookmark.IconPath;
        bmJson["order"] = bookmark.Order;
        bookmarksJson.push_back(bmJson);
    }
    
    store.SetJson(BookmarksKeys::kBookmarksKey, bookmarksJson);
    
    std::string err;
    if (!store.Save(&err))
    {
        Logger::Log::Error("[BookmarksPanel] Failed to save bookmarks: {}", err);
    }
}

void BookmarksPanel::LoadBookmarks()
{
    m_Bookmarks.clear();
    
    // Load from project-specific file like smart folders do
    auto projectPaths = Editor::GetCurrentEditorProjectPaths();
    if (projectPaths.projectEditorRoot.empty())
    {
        // No project root, nothing to load
        return;
    }
    
    auto bookmarksPath = projectPaths.projectEditorRoot / BookmarksKeys::kFileName;
    Editor::SettingsStore store(bookmarksPath);
    
    std::string err;
    if (!store.Load(&err))
    {
        // No bookmarks file is OK for new projects
        return;
    }
    
    if (!store.Contains(BookmarksKeys::kBookmarksKey))
        return;
    
    try
    {
        const auto& bookmarksJson = store.Json()[BookmarksKeys::kBookmarksKey];
        if (!bookmarksJson.is_array())
            return;
        
        auto& engine = EngineCore::GetInstance();
        
        for (const auto& bmJson : bookmarksJson)
        {
            Bookmark bookmark;
            bookmark.Type = static_cast<BookmarkType>(bmJson.value("type", 0));
            bookmark.Reference = bmJson.value("reference", "");
            bookmark.Name = bmJson.value("name", "");
            bookmark.IconPath = bmJson.value("iconPath", "");
            bookmark.Order = bmJson.value("order", 0);
            
            // Validate bookmark still exists
            bool isValid = true;
            const bool isOnlineRef = bookmark.Reference.rfind("polyhaven:", 0) == 0;

            // Upgrade polyhaven: references to real GUIDs if the asset has been downloaded.
            if (isOnlineRef && m_Context && !m_Context->AssetsRoot.empty())
            {
                const std::string slug = bookmark.Reference.substr(10);
                std::filesystem::path filePath = PolyhavenService::FindDownloadedFile(slug, m_Context->AssetsRoot);
                if (!filePath.empty())
                {
                    // Resolve through AssetManager, which registers the downloaded file
                    // when nothing has yet: a raw registry lookup finds nothing for an
                    // unregistered path, and the bookmark would stay on its polyhaven:
                    // reference.
                    GUID guid = engine.GetAssetManager().ResolveAssetGuid(filePath);
                    if (!guid.IsNull())
                    {
                        bookmark.Reference = guid.ToString();
                        bookmark.IconPath.clear(); // Let thumbnail system resolve icon
                    }
                }
            }

            if (!isOnlineRef && (bookmark.Type == BookmarkType::Asset || bookmark.Type == BookmarkType::Scene))
            {
                GUID guid(bookmark.Reference);
                if (!guid.IsNull())
                {
                    AssetRegistry& registry = engine.GetAssetManager().GetRegistry();
                    AssetMetadata metadata;
                    if (!registry.TryGetAssetMetadata(guid, metadata))
                    {
                        isValid = false;
                    }
                }
            }
            
            if (isValid)
            {
                // Skip duplicates (same type + normalized reference); do not allow duplicates
                const std::string refNorm = NormalizeReferenceForDuplicateCheck(bookmark);
                bool isDuplicate = false;
                for (const Bookmark& existing : m_Bookmarks)
                {
                    if (existing.Type != bookmark.Type)
                        continue;
                    if (NormalizeReferenceForDuplicateCheck(existing) == refNorm)
                    {
                        isDuplicate = true;
                        break;
                    }
                }
                if (!isDuplicate)
                {
                    if (!refNorm.empty() && (bookmark.Reference.find('/') != std::string::npos || bookmark.Reference.find('\\') != std::string::npos))
                        bookmark.Reference = refNorm;
                m_Bookmarks.push_back(bookmark);
                }
            }
        }
        
        // After loading, sort by order to ensure correct display order
        // (order 0 = top, higher orders = below)
        std::sort(m_Bookmarks.begin(), m_Bookmarks.end(), 
            [](const Bookmark& a, const Bookmark& b) { return a.Order < b.Order; });
    }
    catch (const std::exception& e)
    {
        Logger::Log::Warning("[BookmarksPanel] Failed to parse bookmarks: {}", e.what());
    }
    
    RebuildBookmarkRows();
}

void BookmarksPanel::SaveRowHeight()
{
    // Save to project-specific file
    auto projectPaths = Editor::GetCurrentEditorProjectPaths();
    if (projectPaths.projectEditorRoot.empty())
    {
        Logger::Log::Warning("[BookmarksPanel] Cannot save row height: no project root");
        return;
    }
    
    auto bookmarksPath = projectPaths.projectEditorRoot / BookmarksKeys::kFileName;
    Editor::SettingsStore store(bookmarksPath);
    
    std::string err;
    if (!store.Load(&err))
    {
        // File doesn't exist yet, that's OK
    }
    
    store.SetDouble(BookmarksKeys::kRowHeightKey, static_cast<double>(m_RowHeight));
    
    if (!store.Save(&err))
    {
        Logger::Log::Error("[BookmarksPanel] Failed to save row height: {}", err);
    }
}

void BookmarksPanel::LoadRowHeight()
{
    // Load from project-specific file
    auto projectPaths = Editor::GetCurrentEditorProjectPaths();
    if (projectPaths.projectEditorRoot.empty())
    {
        // No project root, use default
        return;
    }
    
    auto bookmarksPath = projectPaths.projectEditorRoot / BookmarksKeys::kFileName;
    Editor::SettingsStore store(bookmarksPath);
    
    std::string err;
    if (!store.Load(&err))
    {
        // No bookmarks file is OK, use default
        return;
    }
    
    double rowHeight = 32.0;
    if (store.TryGetDouble(BookmarksKeys::kRowHeightKey, rowHeight))
    {
        m_RowHeight = static_cast<float>(rowHeight);
        // Clamp to reasonable range
        m_RowHeight = std::max(20.0f, std::min(256.0f, m_RowHeight));
    }
}

void BookmarksPanel::SetupDropHandlers(UIElement* container)
{
    if (!container)
        return;
    
    // Track if mouse is over the panel (use static to persist across calls)
    static bool mouseOverPanel = false;
    
    // Mouse enter - track when mouse enters the panel
    container->RegisterEventHandler(kEventMouseEnter, [](UIEvent&)
    {
        mouseOverPanel = true;
    });
    
    // Mouse leave - track when mouse leaves the panel; hide asset-drop insertion indicator
    container->RegisterEventHandler(kEventMouseLeave, [this](UIEvent&)
    {
        mouseOverPanel = false;
        m_AssetDropInsertIndex = -1;
        if (m_InsertionIndicator)
            m_InsertionIndicator->Overrides().Set(Style::Display, DisplayMode::None);
    });

    // Mouse move - show insertion line when dragging an asset (g_DragState) or reordering bookmarks (DragDropManager)
    container->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e)
    {
        UIElement* listContainer = m_ListContainer;
        UIElement* insertionIndicator = m_InsertionIndicator;
        if (!listContainer || !insertionIndicator)
        {
            m_AssetDropInsertIndex = -1;
            return;
        }

        UIManager* ui = GetOwnerManager();
        UI::Interaction::DragDropManager* dd = ui ? ui->GetDragDropManager() : nullptr;
        const bool reorderDrag = dd && dd->IsDragging() &&
            (dd->GetPayload().TypeId == UI::Interaction::GetPayloadTypeId<Editor::BookmarkReorderPayload>() ||
             dd->GetPayload().TypeId == UI::Interaction::GetPayloadTypeId<Editor::BookmarkDragPayload>());

        // Bookmark reorder: use same normalized insertion index as SetDropPreview (one line, one row).
        // Only show the reorder line when this panel is the active drop target; otherwise the
        // DragDropManager's SetDropPreview handles it and we'd fight with its visibility state.
        if (reorderDrag)
        {
            // If the DragDropManager's current target is NOT this panel (e.g. user is dragging
            // a bookmark over the scene view or hierarchy), hide any stale indicator and bail.
            const bool isCurrentTarget = (dd->GetCurrentTargetElement() == this);
            if (!isCurrentTarget)
            {
                insertionIndicator->Overrides().Set(Style::Display, DisplayMode::None);
                return;
            }
            UI::Interaction::DropHit hit{};
            if (HitTestDropTarget(e.X, e.Y, hit) &&
                (hit.Location == UI::Interaction::DropLocation::BeforeItem ||
                 hit.Location == UI::Interaction::DropLocation::AfterItem))
            {
                const size_t targetId = static_cast<size_t>(hit.TargetId);
                const int toIndex = (hit.Location == UI::Interaction::DropLocation::AfterItem)
                                        ? static_cast<int>(targetId) + 1
                                        : static_cast<int>(targetId);
                const int insertIndex = std::max(0, std::min(toIndex, static_cast<int>(m_Bookmarks.size())));
                const size_t rowIdx = (insertIndex < static_cast<int>(m_Bookmarks.size()))
                                          ? static_cast<size_t>(insertIndex)
                                          : (m_Bookmarks.empty() ? 0 : m_Bookmarks.size() - 1);
                const bool lineBelowRow = (insertIndex >= static_cast<int>(m_Bookmarks.size()) && !m_Bookmarks.empty());

                UIElement* row = m_ListContainer->FindById("bookmark-row-" + std::to_string(rowIdx));
                if (row)
                {
                    UI::Interaction::DropRequest req{};
                    req.payload = dd->GetPayload();
                    req.hit = hit;
                    req.mods = 0;
                    UI::Interaction::DropFeedback fb = CanDrop(req);
                    const float ry = row->GetLayoutY();
                    const float rh = row->GetLayoutHeight();
                    const float yLine = lineBelowRow ? (ry + rh) : ry;
                    const float containerY = m_ListContainer->GetLayoutY();
                    const float topPx = yLine - containerY;
                    const uint32_t lineArgb = fb.Allowed ? 0xF200C864u : 0xF2FF5050u;
                    insertionIndicator->Overrides()
                        .Set(Style::PositionLeft, StyleLength::Px(0.0f))
                        .Set(Style::PositionTop, StyleLength::Px(topPx))
                        .Set(Style::Width, StyleLength::Percent(100.0f))
                        .Set(Style::Height, StyleLength::Px(2.0f))
                        .Set(Style::BackgroundColor, (uint32_t)lineArgb)
                        .Set(Style::ZIndex, 9999)
                        .Set(Style::Display, DisplayMode::Block);
                    return;
                }
            }
            insertionIndicator->Overrides().Set(Style::Display, DisplayMode::None);
            return;
        }

        if (!g_DragState.active || g_DragState.assetPath.empty())
        {
            m_AssetDropInsertIndex = -1;
            insertionIndicator->Overrides().Set(Style::Display, DisplayMode::None);
            return;
        }

        float mouseY = e.Y;
        std::vector<std::pair<UIElement*, int>> bookmarkRows;
        const auto& children = listContainer->GetChildren();
        for (const auto& child : children)
        {
            if (!child->HasClass("bookmark-row"))
                continue;
            std::string rowId = child->GetId();
            size_t dashPos = rowId.find_last_of('-');
            if (dashPos == std::string::npos || dashPos + 1 >= rowId.size())
                continue;
            std::string suffix = rowId.substr(dashPos + 1);
            int bookmarkIdx = -1;
            try
            {
                bookmarkIdx = std::stoi(suffix);
            }
            catch (...)
            {
                continue;
            }
            bookmarkRows.push_back({child.get(), bookmarkIdx});
        }
        std::sort(bookmarkRows.begin(), bookmarkRows.end(),
            [](const auto& a, const auto& b) {
                return a.first->GetLayoutY() < b.first->GetLayoutY();
            });
        
        int visualTargetIndex = static_cast<int>(bookmarkRows.size());
        for (size_t i = 0; i < bookmarkRows.size(); ++i)
        {
            UIElement* rowEl = bookmarkRows[i].first;
            float rowY = rowEl->GetLayoutY();
            float rowH = rowEl->GetLayoutHeight();
            float rowMidY = rowY + rowH * 0.5f;
            if (mouseY < rowMidY)
            {
                visualTargetIndex = static_cast<int>(i);
                break;
            }
        }
        
        m_AssetDropInsertIndex = visualTargetIndex;
        
        float containerY = listContainer->GetLayoutY();
        float indicatorAbsY = containerY;
        const int rowCount = static_cast<int>(bookmarkRows.size());
        if (visualTargetIndex >= 0 && visualTargetIndex < rowCount)
            indicatorAbsY = bookmarkRows[visualTargetIndex].first->GetLayoutY();
        else if (!bookmarkRows.empty())
        {
            UIElement* lastRow = bookmarkRows.back().first;
            indicatorAbsY = lastRow->GetLayoutY() + lastRow->GetLayoutHeight();
        }
        float indicatorY = indicatorAbsY - containerY;
        insertionIndicator->Overrides().Set(Style::PositionTop, StyleLength::Px(indicatorY)).Set(Style::Display, DisplayMode::Block);
    });
    
    // Mouse up - handle drop when mouse is released over the panel
    container->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
    {
        if (e.Button != 0)
            return;
        
        int insertIdx = m_AssetDropInsertIndex;
        m_AssetDropInsertIndex = -1;
        if (m_InsertionIndicator)
            m_InsertionIndicator->Overrides().Set(Style::Display, DisplayMode::None);

        // Force immediate rebuild + schedule next-frame rebuild so first drop always shows.
        auto forceRefresh = [this]()
        {
            if (!m_ListContainer)
                return;
            // Immediate rebuild
            RebuildBookmarkRows();
            m_ListContainer->MarkDirty(UIElement::ChildrenDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
            if (m_ScrollView)
                m_ScrollView->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
            MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
            RequestRelayout();
            // Also schedule next-frame rebuild to catch any deferred layout
            Scheduler::IScheduler* sched = GetScheduler();
            if (sched)
            {
                sched->ScheduleNext([this]()
                {
                    if (!m_ListContainer)
                        return;
                    RebuildBookmarkRows();
                    m_ListContainer->MarkDirty(UIElement::ChildrenDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
                    if (m_ScrollView)
                        m_ScrollView->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
                    MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
                    RequestRelayout();
                });
            }
        };
        
        if (g_DragState.active)
        {
            if (!g_DragState.assetPath.empty() && std::filesystem::exists(g_DragState.assetPath))
            {
                std::filesystem::path path = g_DragState.assetPath;
                g_DragState.active = false;
                g_DragState.assetPath.clear();
                if (m_Context)
                {
                    HandleAssetDrop(path, insertIdx);
                    forceRefresh();
                }
                e.Stop();
                return;
            }
            else if (!g_DragState.scenePath.empty() && !g_DragState.entityId.empty())
            {
                std::string scenePath = g_DragState.scenePath;
                std::string entityId = g_DragState.entityId;
                g_DragState.active = false;
                g_DragState.scenePath.clear();
                g_DragState.entityId.clear();
                HandleEntityDrop(scenePath, entityId);
                forceRefresh();
                e.Stop();
                return;
            }
        }
        
        g_DragState.active = false;
        g_DragState.assetPath.clear();
    });
}

void BookmarksPanel::HandleAssetDrop(const std::filesystem::path& assetPath)
{
    HandleAssetDrop(assetPath, -1);
}

void BookmarksPanel::HandleAssetDrops(const std::vector<std::filesystem::path>& assetPaths)
{
    HandleAssetDrops(assetPaths, -1);
}

void BookmarksPanel::HandleAssetDrops(const std::vector<std::filesystem::path>& assetPaths, int insertIndex)
{
    if (assetPaths.empty() || !m_Context)
        return;
    int at = (insertIndex >= 0) ? insertIndex : static_cast<int>(m_Bookmarks.size());
    for (size_t i = 0; i < assetPaths.size(); ++i)
        HandleAssetDrop(assetPaths[i], at + static_cast<int>(i));
}

void BookmarksPanel::HandleAssetDrop(const std::filesystem::path& assetPath, int insertIndex)
{
    if (assetPath.empty() || !m_Context)
        return;
    
    if (!std::filesystem::exists(assetPath))
        return;
    
    size_t at = (insertIndex >= 0) ? static_cast<size_t>(insertIndex) : m_Bookmarks.size();
    if (at > m_Bookmarks.size())
        at = m_Bookmarks.size();
    
    // Handle folders specially - they're not registered as assets
    if (std::filesystem::is_directory(assetPath))
    {
        // For folders, use the path directly as reference (not GUID-based)
        // We'll store it as a special asset type bookmark
        Bookmark bookmark;
        bookmark.Type = BookmarkType::Asset;
        // Store full absolute path for folders to ensure we can detect it later
        bookmark.Reference = std::filesystem::absolute(assetPath).string();
        bookmark.Name = assetPath.filename().string();
        if (bookmark.Name.empty())
        {
            // If filename is empty (root directory), use the last component of the path
            std::string pathStr = assetPath.string();
            // Remove trailing slash if present
            if (!pathStr.empty() && (pathStr.back() == '/' || pathStr.back() == '\\'))
                pathStr.pop_back();
            // Extract last component
            size_t lastSep = pathStr.find_last_of("/\\");
            if (lastSep != std::string::npos && lastSep + 1 < pathStr.length())
                bookmark.Name = pathStr.substr(lastSep + 1);
            else
                bookmark.Name = pathStr;
        }
        bookmark.IconPath = "Icons/Folder@32px.png"; // Set folder icon path explicitly
        
        AddBookmarkAt(bookmark, at);
        return;
    }
    
    // Handle regular files (assets)
    auto& engine = EngineCore::GetInstance();
    AssetManager& assets = engine.GetAssetManager();
    AssetRegistry& registry = assets.GetRegistry();

    // Single register-and-resolve entry point; the RegisterAsset + raw
    // GetAssetGUID pair it replaces performed the same sequence.
    const GUID guid = assets.ResolveAssetGuid(assetPath);
    if (guid.IsNull())
        return;
    
    // Get asset metadata
    AssetMetadata metadata;
    if (!registry.TryGetAssetMetadata(guid, metadata))
        return;
    
    // Determine bookmark type
    BookmarkType type = BookmarkType::Asset;
    if (metadata.Type == AssetType::Scene)
    {
        type = BookmarkType::Scene;
    }
    
    // Create bookmark
    Bookmark bookmark;
    bookmark.Type = type;
    bookmark.Reference = guid.ToString();
    bookmark.Name = metadata.Name.empty() ? assetPath.stem().string() : metadata.Name;
    bookmark.IconPath = ""; // Will be set by UpdateBookmarkIcon
    
    AddBookmarkAt(bookmark, at);
}

void BookmarksPanel::RefreshBookmarks()
{
    // Reload bookmarks from disk (re-validates against current asset registry)
    LoadBookmarks();
    LoadRowHeight();
    
    // Rebuild the UI
    if (m_ListContainer)
    {
        if (GetOwnerManager())
        {
            PostAction([this]()
            {
                if (m_ListContainer)
                {
                    RebuildBookmarkRows();
                }
            });
        }
        else
        {
            RebuildBookmarkRows();
        }
    }
}

void BookmarksPanel::HandleEntityDrop(const std::string& scenePath, const std::string& entityId)
{
    if (scenePath.empty() || entityId.empty())
        return;

    std::string entityName = entityId;
    ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    if (world)
    {
        size_t colon = entityId.find(':');
        ECS::EntityIndex idx = static_cast<ECS::EntityIndex>(std::stoul(entityId.substr(0, colon)));
        ECS::EntityVersion ver = (colon != std::string::npos)
            ? static_cast<ECS::EntityVersion>(std::stoul(entityId.substr(colon + 1)))
            : 0;
        ECS::EntityHandle h(idx, ver);
        if (world->IsValid(h))
        {
            if (auto* n = world->GetComponent<Components::Name>(h))
                if (!n->View().empty())
                    entityName = std::string(n->View());
        }
    }

    Bookmark bookmark;
    bookmark.Type = BookmarkType::Entity;
    bookmark.Reference = scenePath + "|" + entityId;
    bookmark.Name = entityName;
    bookmark.IconPath = "";

    AddBookmark(bookmark);
}

void BookmarksPanel::HandleEntityPayloadDrop(const std::vector<std::uint64_t>& treeIds, int insertIndex)
{
    std::string scenePath;
    if (m_GetCurrentScenePath)
    {
        auto opt = m_GetCurrentScenePath();
        if (opt.has_value())
            scenePath = opt->string();
    }

    ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    int at = (insertIndex >= 0) ? insertIndex : static_cast<int>(m_Bookmarks.size());

    for (size_t i = 0; i < treeIds.size(); ++i)
    {
        // Decode packed TreeId: same encoding as HierarchyDataProvider::Encode
        uint32_t raw = static_cast<uint32_t>(treeIds[i]);
        ECS::EntityIndex idx = static_cast<ECS::EntityIndex>(raw & ECS::kEntityIndexMask);
        ECS::EntityVersion ver = static_cast<ECS::EntityVersion>((raw >> ECS::kEntityIndexBits) & ECS::kEntityVersionMask);
        ECS::EntityHandle handle(idx, ver);

        std::string entityName = "Entity";
        if (world && world->IsValid(handle))
        {
            if (auto* n = world->GetComponent<Components::Name>(handle))
                if (!n->View().empty())
                    entityName = std::string(n->View());
        }

        std::string entityId = std::to_string(idx) + ":" + std::to_string(ver);

        Bookmark bookmark;
        bookmark.Type = BookmarkType::Entity;
        bookmark.Reference = scenePath + "|" + entityId;
        bookmark.Name = entityName;
        bookmark.IconPath = "";

        AddBookmarkAt(bookmark, static_cast<size_t>(at + static_cast<int>(i)));
    }
}

} // namespace GameEngine
