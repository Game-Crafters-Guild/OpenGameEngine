#include "Panels/ScriptEditorPanel.h"
#include "AssetCore/SharedFileRead.h"

#include "Editor/Settings/ScriptEditorSettings.h"
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "Panels/SaveSceneChangesModal.h"
#include "Panels/ScriptSyntax.h"
#include "Input/KeyCodes.h"
#include "Panels/CompletionPopup.h"
#include "Panels/OnDiskFileName.h"
#include "Panels/ScriptTextArea.h"
#include "UndoRedo/UndoRedoService.h"
#include "UndoRedo/IEditorCommand.h"
#include "UI/Controls/TextArea.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/EditorSearchBars.h"
#include "UI/Interaction/FocusIsInside.h"
#include "Types/StringId.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <set>

namespace GameEngine {

// CRT-safe ctype helpers: std::is* and std::tolower require unsigned-char or EOF.
static inline unsigned char ToUChar(char c) { return static_cast<unsigned char>(c); }
static inline bool IsSpace(char c) { return std::isspace(ToUChar(c)) != 0; }
static inline bool IsAlnum(char c) { return std::isalnum(ToUChar(c)) != 0; }
static inline char ToLowerChar(char c) { return static_cast<char>(std::tolower(ToUChar(c))); }

namespace
{
constexpr float kScriptEditorBaseFontSizePx = 14.0f;

const std::vector<int>& ScriptTextScaleOptions()
{
    static const std::vector<int> kOptions{50, 75, 90, 100, 110, 125, 150, 200};
    return kOptions;
}

int FindClosestTextScaleOptionIndex(float percent)
{
    const auto& options = ScriptTextScaleOptions();
    int bestIndex = 0;
    float bestDistance = std::numeric_limits<float>::max();
    for (int i = 0; i < static_cast<int>(options.size()); ++i)
    {
        const float distance = std::abs(static_cast<float>(options[static_cast<size_t>(i)]) - percent);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = i;
        }
    }
    return bestIndex;
}

std::vector<Dropdown::Option> BuildTextScaleDropdownOptions()
{
    std::vector<Dropdown::Option> options;
    for (int percent : ScriptTextScaleOptions())
    {
        const std::string value = std::to_string(percent);
        options.push_back(Dropdown::Option{value, value + " %"});
    }
    return options;
}
}

// Command for script text edits - integrates with the global UndoRedoService
class ScriptTextEditCommand : public Editor::IEditorCommand
{
public:
    ScriptTextEditCommand(ScriptTextArea* textArea, std::string beforeText, std::string afterText, 
                          bool* loadingFlag, std::function<void()> onUndoRedo)
        : m_TextArea(textArea)
        , m_BeforeText(std::move(beforeText))
        , m_AfterText(std::move(afterText))
        , m_LoadingFlag(loadingFlag)
        , m_OnUndoRedo(std::move(onUndoRedo))
    {
    }

    const char* GetName() const override { return "Script Edit"; }

    void Do() override
    {
        // Already applied when command was created
    }

    void Undo() override
    {
        if (m_TextArea && m_LoadingFlag)
        {
            *m_LoadingFlag = true;
            m_TextArea->SetValue(m_BeforeText);
            *m_LoadingFlag = false;
            if (m_OnUndoRedo) m_OnUndoRedo();
        }
    }

    void Redo() override
    {
        if (m_TextArea && m_LoadingFlag)
        {
            *m_LoadingFlag = true;
            m_TextArea->SetValue(m_AfterText);
            *m_LoadingFlag = false;
            if (m_OnUndoRedo) m_OnUndoRedo();
        }
    }

    // Allow merging consecutive character edits into a single command
    bool CanMergeWith(const IEditorCommand& other) const override
    {
        auto* otherCmd = dynamic_cast<const ScriptTextEditCommand*>(&other);
        if (!otherCmd) return false;
        // Merge if it's the same text area and the change is small (single character edit)
        if (m_TextArea != otherCmd->m_TextArea) return false;
        // Only merge if the difference between before and after is small
        int sizeDiff = std::abs((int)m_AfterText.size() - (int)otherCmd->m_BeforeText.size());
        if (sizeDiff > 1 || m_AfterText != otherCmd->m_BeforeText) return false;
        
        // Don't merge if the new edit contains a newline (Enter key creates new undo step)
        if (otherCmd->m_AfterText.size() > otherCmd->m_BeforeText.size())
        {
            // Character was added - check if it's a newline
            for (size_t i = 0; i < otherCmd->m_AfterText.size(); ++i)
            {
                if (i >= otherCmd->m_BeforeText.size() || otherCmd->m_AfterText[i] != otherCmd->m_BeforeText[i])
                {
                    if (otherCmd->m_AfterText[i] == '\n')
                        return false;
                    break;
                }
            }
        }
        
        return true;
    }

    bool MergeWith(const IEditorCommand& other) override
    {
        auto* otherCmd = dynamic_cast<const ScriptTextEditCommand*>(&other);
        if (!otherCmd) return false;
        // Keep our before state, take their after state
        m_AfterText = otherCmd->m_AfterText;
        return true;
    }

private:
    ScriptTextArea* m_TextArea;
    std::string m_BeforeText;
    std::string m_AfterText;
    bool* m_LoadingFlag;
    std::function<void()> m_OnUndoRedo;
};

ScriptEditorPanel::ScriptEditorPanel()
    : DockPanel("Script Editor")
{
    SetupUI();
}

ScriptEditorPanel::~ScriptEditorPanel()
{
    if (m_SearchField && m_SearchField->GetParent())
        EditorSearchBars::UnregisterFocusTarget(m_SearchField->GetParent());
}

void ScriptEditorPanel::SetupUI()
{
    m_TextScalePercent = static_cast<float>(
        ScriptTextScaleOptions()[static_cast<size_t>(
            FindClosestTextScaleOptionIndex(Editor::ScriptEditorSettings::Get().GetTextScalePercent()))]);

    // Create main container
    auto container = std::make_unique<UIElement>();
    container->AddClass("script-editor-container");
    container->SetId("ScriptEditorContainer");

    // Create toolbar
    auto toolbar = std::make_unique<UIElement>();
    toolbar->AddClass("script-editor-toolbar");

    // Left side: save button and unsaved indicator
    auto leftGroup = std::make_unique<UIElement>();
    leftGroup->AddClass("script-editor-toolbar-left");

    // Line numbers toggle button (icon only) - left-most icon
    auto lineNumbersButton = std::make_unique<Button>();
    lineNumbersButton->SetId("ScriptEditorLineNumbersButton");
    // Reuse the save icon button styling/states; only the icon image differs.
    lineNumbersButton->AddClass("script-editor-save-button");
    lineNumbersButton->AddClass("linenumbers-icon");
    lineNumbersButton->AddClass("icon-button");
    lineNumbersButton->AddClass("icon-active");
    lineNumbersButton->SetText("");
    lineNumbersButton->SetTooltip("Toggle line numbers");
    Button* lineNumbersButtonPtr = lineNumbersButton.get();
    lineNumbersButton->RegisterEventHandler(kEventButtonClick, [this, lineNumbersButtonPtr](UIEvent&) {
        if (!m_TextArea) return;
        const bool next = !m_TextArea->GetShowLineNumbers();
        m_TextArea->SetShowLineNumbers(next);
        if (next) lineNumbersButtonPtr->AddClass("icon-active");
        else lineNumbersButtonPtr->RemoveClass("icon-active");
    });
    leftGroup->AddChild(std::move(lineNumbersButton));

    // Word wrap toggle button. Wrap is on by default (active state); click to disable.
    auto wrapButton = std::make_unique<Button>();
    wrapButton->SetId("ScriptEditorWrapButton");
    wrapButton->AddClass("script-editor-save-button");
    wrapButton->AddClass("wrap-icon");
    wrapButton->AddClass("icon-button");
    wrapButton->AddClass("icon-active");
    wrapButton->SetText("");
    wrapButton->SetTooltip("Toggle word wrap");
    Button* wrapButtonPtr = wrapButton.get();
    wrapButton->RegisterEventHandler(kEventButtonClick, [this, wrapButtonPtr](UIEvent&) {
        if (!m_TextArea) return;
        const bool wrapOn = !m_TextArea->HasClass("nowrap");
        if (wrapOn) {
            m_TextArea->AddClass("nowrap");
            wrapButtonPtr->RemoveClass("icon-active");
        } else {
            m_TextArea->RemoveClass("nowrap");
            wrapButtonPtr->AddClass("icon-active");
        }
        m_TextArea->MarkDirty(StyleDirty | LayoutDirty | VisualDirty);
    });
    leftGroup->AddChild(std::move(wrapButton));

    // Text size percentage dropdown.
    auto textScaleDropdown = std::make_unique<Dropdown>();
    textScaleDropdown->SetId("ScriptEditorTextScaleDropdown");
    textScaleDropdown->AddClass("script-editor-text-scale-dropdown");
    textScaleDropdown->SetTooltip("Text size");
    textScaleDropdown->SetAutoWidthPopup(true);
    textScaleDropdown->SetOptions(BuildTextScaleDropdownOptions(), FindClosestTextScaleOptionIndex(m_TextScalePercent));
    m_TextScaleDropdown = textScaleDropdown.get();
    textScaleDropdown->SetOnValueChanged([this](const std::string& value) {
        if (m_SyncingTextScaleDropdown)
            return;
        for (int percent : ScriptTextScaleOptions())
        {
            if (value == std::to_string(percent))
            {
                ApplyTextScalePercent(static_cast<float>(percent), true);
                return;
            }
        }
    });
    leftGroup->AddChild(std::move(textScaleDropdown));

    // Save button (icon only, no text)
    auto saveButton = std::make_unique<Button>();
    saveButton->SetId("ScriptEditorSaveButton");
    saveButton->AddClass("script-editor-save-button");
    saveButton->AddClass("save-icon");
    saveButton->AddClass("icon-button");
    saveButton->SetText(""); // Icon-only button, no text
    saveButton->SetTooltip("Save script");
    saveButton->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
        OnSaveButtonClicked();
    });
    m_SaveButton = saveButton.get();
    leftGroup->AddChild(std::move(saveButton));

    // Unsaved changes indicator (asterisk) - after save icon
    auto unsavedIndicator = std::make_unique<Label>();
    unsavedIndicator->SetId("ScriptEditorUnsavedIndicator");
    unsavedIndicator->AddClass("script-editor-unsaved-indicator");
    unsavedIndicator->SetText("*");
    unsavedIndicator->AddClass("hidden"); // Hidden by default
    m_UnsavedIndicator = unsavedIndicator.get();
    leftGroup->AddChild(std::move(unsavedIndicator));

    toolbar->AddChild(std::move(leftGroup));

    // Center/Right: Search bar
    auto searchContainer = std::make_unique<UIElement>();
    searchContainer->AddClass("script-editor-search-container");
    
    auto searchField = std::make_unique<TextField>();
    m_SearchField = searchField.get();
    searchField->SetId("ScriptEditorSearchField");
    searchField->AddClass("script-editor-search-field");
    searchField->SetValue("");
    searchField->SetTooltip("Search (Enter/Down: next, Up: previous, click X: clear)");

    auto clearButton = std::make_unique<Button>();
    clearButton->AddClass("icon-button");
    clearButton->AddClass("script-editor-search-clear");
    clearButton->AddClass("xclose-icon");
    clearButton->AddClass("hidden");
    clearButton->SetFocusable(false);
    clearButton->SetTooltip("Clear search");
    Button* clearButtonPtr = clearButton.get();
    
    // Search icon overlay
    auto searchIcon = std::make_unique<UIElement>();
    searchIcon->AddClass("script-editor-search-icon");
    UIElement* searchIconPtr = searchIcon.get();
    UIElement* searchContainerPtr = searchContainer.get();
    EditorSearchBars::RegisterFocusTarget(searchContainerPtr);

    auto applySearchVisualState = [searchIconPtr, searchContainerPtr, clearButtonPtr](const std::string& value) {
        const bool hasText = !value.empty();
        if (hasText) {
            if (!searchContainerPtr->HasClass("has-query")) {
                searchContainerPtr->AddClass("has-query");
            }
            if (!searchContainerPtr->HasClass("active")) {
                searchContainerPtr->AddClass("active");
            }
            if (!searchIconPtr->HasClass("icon-active")) {
                searchIconPtr->AddClass("icon-active");
            }
            if (clearButtonPtr->HasClass("hidden")) {
                clearButtonPtr->RemoveClass("hidden");
            }
            return;
        }

        searchContainerPtr->RemoveClass("has-query");
        searchContainerPtr->RemoveClass("active");
        searchIconPtr->RemoveClass("icon-active");
        if (!clearButtonPtr->HasClass("hidden")) {
            clearButtonPtr->AddClass("hidden");
        }
    };

    // Search functionality - update results while typing (not just on Enter)
    m_SearchField->SetOnValueChanging([this, applySearchVisualState](const std::string& value) {
        OnSearchTextChanged(value);
        applySearchVisualState(value);
    });
    
    // Also update on final value change (for Enter key)
    m_SearchField->SetOnValueChanged([this, applySearchVisualState](const std::string& value) {
        OnSearchTextChanged(value);
        applySearchVisualState(value);
    });
    
    // Update icon state on focus
    m_SearchField->RegisterEventHandler(kEventFocusIn, [searchIconPtr, searchContainerPtr](UIEvent&) {
        if (!searchContainerPtr->HasClass("active")) {
            searchContainerPtr->AddClass("active");
        }
        if (!searchIconPtr->HasClass("icon-active")) {
            searchIconPtr->AddClass("icon-active");
        }
    });
    
    // Update icon state on blur (but keep active if text is present)
    m_SearchField->RegisterEventHandler(kEventFocusOut, [searchIconPtr, searchContainerPtr, clearButtonPtr, this](UIEvent&) {
        if (m_SearchField->GetValue().empty()) {
            searchContainerPtr->RemoveClass("active");
            searchIconPtr->RemoveClass("icon-active");
            if (!clearButtonPtr->HasClass("hidden")) {
                clearButtonPtr->AddClass("hidden");
            }
        }
    });
    
    // Handle up/down arrows for navigating search matches
    m_SearchField->RegisterEventHandler(kEventKeyDown, [this](UIEvent& e) {
        if (e.Key == Input::kKeyCode_Up) {
            NavigateToPreviousMatch();
            e.Stop();
        } else if (e.Key == Input::kKeyCode_Down) {
            NavigateToNextMatch();
            e.Stop();
        } else if (e.Key == Input::kKeyCode_Enter) {
            NavigateToNextMatch();
            e.Stop();
        }
    });
    
    // Right-click clears the search field
    m_SearchField->RegisterEventHandler(kEventMouseDown, [this, applySearchVisualState](UIEvent& e) {
        if (e.Button == 1) { // Right mouse button
            m_SearchField->SetValue("");
            OnSearchTextChanged("");
            applySearchVisualState("");
            e.Stop();
        }
    });

    clearButtonPtr->RegisterEventHandler(kEventButtonClick, [this, applySearchVisualState](UIEvent&) {
        if (!m_SearchField || m_SearchField->GetValue().empty()) {
            return;
        }
        m_SearchField->SetValue("");
        OnSearchTextChanged("");
        applySearchVisualState("");
    });
    
    searchContainer->AddChild(std::move(searchField));
    searchContainer->AddChild(std::move(clearButton));
    searchContainer->AddChild(std::move(searchIcon));
    
    toolbar->AddChild(std::move(searchContainer));

    container->AddChild(std::move(toolbar));

    // Create scroll view for text area
    auto scrollView = std::make_unique<ScrollView>();
    scrollView->AddClass("script-editor-scroll");
    scrollView->Overrides().SetCustomNumber(HashStringId("--ui_scrollview_primary_modifier_passthrough"), 1.0f);
    m_ScrollView = scrollView.get();
    
    // When scroll view is clicked (scrollbar, empty space), refresh Inspector to show script variables
    scrollView->RegisterEventHandler(kEventMouseDown, [this](UIEvent&) {
        // Defer callback to avoid reentrancy issues
        PostAction([this]() {
            // If we have a script open, refresh the Inspector to show its variables
            if (!m_CurrentScriptPath.empty() && !m_ScriptVariables.empty() && m_OnCSharpScriptOpened) {
                m_OnCSharpScriptOpened(m_CurrentScriptPath, m_ScriptVariables);
            }
        });
    });

    // Create syntax-highlighted text area for code editing
    auto textArea = std::make_unique<ScriptTextArea>();
    textArea->SetId("ScriptEditorTextArea");
    textArea->AddClass("script-editor-textarea");
    textArea->SetValue("");
    textArea->SetShowLineNumbers(true);
    textArea->SetFocusable(true);

    // Track text changes
    textArea->SetOnValueChanged([this](const std::string&) {
        OnTextChanged();
    });
    
    // Install completion key filter — runs inside TextArea::OnEvent before default handling
    // so Tab/Enter/Escape can be suppressed before inserting a character.
    textArea->SetKeyFilter([this](UIEvent& e) -> bool {
        if (!m_CompletionPopup || !m_CompletionPopup->IsVisible())
            return false;
        if (e.Key == Input::kKeyCode_Escape) { HideCompletions(); return true; }
        if (e.Key == Input::kKeyCode_Tab)    { AcceptCompletion(m_CompletionPopup->GetSelected()); return true; }
        if (e.Key == Input::kKeyCode_Enter)  { AcceptCompletion(m_CompletionPopup->GetSelected()); return true; }
        if (e.Key == Input::kKeyCode_Up)     { m_CompletionPopup->MoveSelection(-1); return true; }
        if (e.Key == Input::kKeyCode_Down)   { m_CompletionPopup->MoveSelection(1); return true; }
        return false;
    });

    // Track caret position changes (for Inspector highlighting)
    textArea->RegisterEventHandler(kEventKeyDown, [this](UIEvent&) {
        // Check caret position after a short delay to allow caret to update
        PostAction([this]() {
            CheckCaretPosition();
        });
    });

    textArea->RegisterEventHandler(kEventTextInput, [this](UIEvent&) {
        PostAction([this]() {
            ShowCompletions();
            CheckCaretPosition();
        });
    });

    textArea->RegisterEventHandler(kEventMouseDown, [this](UIEvent&) {
        HideCompletions();
        CheckCaretPosition();
        PostAction([this]() {
            CheckCaretPosition();
        });
    });

    textArea->RegisterEventHandler(kEventFocusOut, [this](UIEvent&) {
        PostAction([this]() { HideCompletions(); });
    });

    textArea->RegisterEventHandler(kEventMouseUp, [this](UIEvent&) {
        PostAction([this]() {
            CheckCaretPosition();
        });
    });
    
    // Check position on hover and mouse move
    textArea->RegisterEventHandler(kEventMouseMove, [this](UIEvent&) {
        // Check caret position on mouse move (for hover highlighting)
        // The caret might be set by the UI system when hovering
        CheckCaretPosition();
    });

    // Hovering the line-number gutter should also hover the corresponding code line.
    textArea->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e) {
        if (m_TextArea) {
            m_TextArea->UpdateHoverFromMouse(e.X, e.Y);
        }
    });
    textArea->RegisterEventHandler(kEventMouseLeave, [this](UIEvent&) {
        if (m_TextArea) {
            m_TextArea->ClearHoverLine();
        }
    });
    
    // When script editor gets focus, refresh Inspector to show script variables
    textArea->RegisterEventHandler(kEventFocusIn, [this](UIEvent&) {
        // Defer callback to avoid reentrancy issues
        PostAction([this]() {
            // If we have a script open, refresh the Inspector to show its variables
            if (!m_CurrentScriptPath.empty() && !m_ScriptVariables.empty() && m_OnCSharpScriptOpened) {
                m_OnCSharpScriptOpened(m_CurrentScriptPath, m_ScriptVariables);
            }
        });
    });

    m_TextArea = textArea.get();
    ApplyTextScalePercent(m_TextScalePercent, false);
    scrollView->AddContent(std::move(textArea));

    // IMPORTANT:
    // ScrollView scrolling is applied via a post-layout translation (no full geometry rebuild).
    // Our ScriptTextArea now virtualizes text rendering to visible lines, so it *must* be
    // notified when scroll offsets change (wheel, scrollbar drag, track click, etc.) so it can
    // rebuild the visible window.
    scrollView->SetOnScrollChanged([this](float /*scrollX*/, float /*scrollY*/) {
        if (!m_TextArea)
            return;
        m_TextArea->SyncFoldMarkerPositionsToScroll();
        m_TextArea->RefreshHoverFromLastMouse();
        m_TextArea->MarkDirty(VisualDirty);
    });

    // Keep fold icons perfectly in sync during scrolling.
    // ScrollView handles kEventScroll in OnEvent() first, then DispatchEvent() runs registered handlers.
    // That means this runs *after* ScrollBy() has updated scroll offsets, but *before* the next render pass.
    scrollView->RegisterEventHandler(kEventScroll, [this](UIEvent&) {
        if (m_TextArea) {
            m_TextArea->SyncFoldMarkerPositionsToScroll();
            m_TextArea->RefreshHoverFromLastMouse();
            // Ensure highlight rectangles update immediately during scroll.
            m_TextArea->MarkDirty(VisualDirty);
        }
    });

    scrollView->RegisterEventHandler(kEventScroll, [this](UIEvent& e) {
        if (!Input::IsPrimaryShortcutModifier(e.Mods) || e.ScrollY == 0.0f)
            return;
        AdjustTextScaleFromScroll(e.ScrollY);
        e.Stop();
    });

    container->AddChild(std::move(scrollView));

    // Completion popup — absolute-positioned overlay; added last so it renders on top.
    auto completionPopup = std::make_unique<CompletionPopup>();
    m_CompletionPopup = completionPopup.get();
    completionPopup->SetOnAccept([this](const std::string& s) { AcceptCompletion(s); });
    container->AddChild(std::move(completionPopup));

    AddChild(std::move(container));
    UpdateTitle();
    
    // Ensure save button is marked for proper rendering after setup
    if (m_SaveButton)
    {
        m_SaveButton->MarkDirty(VisualDirty | StyleDirty);
    }
}

bool ScriptEditorPanel::OpenScript(const std::filesystem::path& scriptPath)
{
    if (scriptPath.empty() || !std::filesystem::exists(scriptPath))
    {
        Logger::Log::Error("ScriptEditorPanel: Cannot open script - path is empty or file does not exist: {}", 
                          scriptPath.string());
        return false;
    }

    m_IsCSharpScript = (scriptPath.extension() == ".cs");
    if (m_TextArea)
        m_TextArea->SetLanguage(Editor::ScriptLanguageForPath(scriptPath));

    HideCompletions();
    m_IsLoading = true;
    LoadScriptFromFile(scriptPath);
    m_CurrentScriptPath = scriptPath;
    m_HasUnsavedChanges = false;
    m_IsLoading = false;
    
    // Initialize undo tracking state
    if (m_TextArea) {
        m_LastUndoContent = m_TextArea->GetValue();
    }

    // Parse script variables for Inspector integration (C# only).
    m_ScriptVariables.clear();
    if (m_IsCSharpScript)
    {
        ParseScriptVariables();
    }

    // Notify listeners that a C# file was opened. A shader has no serializable
    // fields to show, and announcing its empty list drives the Inspector's C#
    // script view onto a .glsl.
    if (m_IsCSharpScript && m_OnCSharpScriptOpened)
    {
        m_OnCSharpScriptOpened(m_CurrentScriptPath, m_ScriptVariables);
    }

    UpdateTitle();
    return true;
}

void ScriptEditorPanel::PromptSaveBeforeQuit(std::function<void()> onProceed)
{
    if (!m_HasUnsavedChanges)
    {
        onProceed();
        return;
    }

    if (!m_QuitModal)
    {
        auto modal = std::make_unique<SaveSceneChangesModal>();
        m_QuitModal = modal.get();
        UIManager* ui = GetOwnerManager();
        UIElement* uiRoot = ui ? ui->GetRootElement() : nullptr;
        if (uiRoot)
            uiRoot->AddChild(std::move(modal));
        else
            AddChild(std::move(modal));
    }

    m_QuitModal->SetOnSave([this, onProceed]()
    {
        if (SaveScript())
            onProceed();
    });
    m_QuitModal->SetOnDontSave([onProceed]() { onProceed(); });
    m_QuitModal->SetOnCancel([]() {});

    const std::string filename = m_CurrentScriptPath.empty()
        ? "Untitled Script"
        : m_CurrentScriptPath.filename().string();
    m_QuitModal->Show(
        "Unsaved Script Changes",
        "\"" + filename + "\" has unsaved changes.\nSave before closing?");
}

bool ScriptEditorPanel::SaveScript()
{
    if (m_CurrentScriptPath.empty() || !m_TextArea)
    {
        Logger::Log::Warning("ScriptEditorPanel: Cannot save - no file is open");
        return false;
    }

    std::string content = m_TextArea->GetValue();
    if (WriteScriptToFile(m_CurrentScriptPath, content))
    {
        m_LastSavedContent = content;
        m_HasUnsavedChanges = false;
        UpdateTitle();

        if (m_IsCSharpScript && m_OnCSharpScriptSaved)
        {
            m_OnCSharpScriptSaved(m_CurrentScriptPath);
        }

        Logger::Log::Info("ScriptEditorPanel: Saved script: {}", m_CurrentScriptPath.string());
        return true;
    }

    return false;
}

void ScriptEditorPanel::OnTextChanged()
{
    if (m_IsLoading || m_IsUndoRedoOperation || !m_TextArea)
        return;

    std::string currentContent = m_TextArea->GetValue();
    m_HasUnsavedChanges = (currentContent != m_LastSavedContent);
    UpdateTitle();

    if (m_IsCSharpScript)
    {
        // Re-parse variables and notify Inspector if they changed
        std::vector<ScriptVariable> oldVariables = m_ScriptVariables;
        ParseScriptVariables();

        // Check if any variable values changed
        bool variablesChanged = false;
        if (oldVariables.size() != m_ScriptVariables.size()) {
            variablesChanged = true;
        } else {
            for (size_t i = 0; i < m_ScriptVariables.size(); ++i) {
                if (m_ScriptVariables[i].Value != oldVariables[i].Value) {
                    variablesChanged = true;
                    break;
                }
            }
        }

        // Notify Inspector if variables changed
        if (variablesChanged && m_OnScriptVariablesChanged) {
            m_OnScriptVariablesChanged(m_ScriptVariables);
        }

        // Check caret position to highlight in Inspector
        CheckCaretPosition();
    }

    // Push text edit to global undo/redo service
    if (m_UndoRedo && currentContent != m_LastUndoContent)
    {
        auto cmd = std::make_unique<ScriptTextEditCommand>(
            m_TextArea, 
            m_LastUndoContent, 
            currentContent,
            &m_IsUndoRedoOperation,
            [this]() {
                // Update unsaved indicator after undo/redo
                if (m_TextArea) {
                    std::string content = m_TextArea->GetValue();
                    m_HasUnsavedChanges = (content != m_LastSavedContent);
                    m_LastUndoContent = content;
                    UpdateTitle();

                    // Re-parse variables after undo/redo (C# only)
                    if (m_IsCSharpScript) {
                        ParseScriptVariables();
                        if (m_OnScriptVariablesChanged) {
                            m_OnScriptVariablesChanged(m_ScriptVariables);
                        }
                    }
                }
            }
        );
        m_UndoRedo->CommitAlreadyApplied(std::move(cmd));
        m_LastUndoContent = currentContent;
    }
}

void ScriptEditorPanel::OnEvent(UIEvent& e)
{
    // Chord first: it rejects almost every key with two int compares, while the
    // focus test walks the tree.
    if (e.Id == kEventKeyDown && !m_CurrentScriptPath.empty() &&
        Editor::MatchesCatalogShortcut("Script Editor", "Save", e.Key, e.Mods) &&
        UI::FocusIsInside(*this))
    {
        (void)SaveScript();
        // Handling the chord here is what keeps the app's Save Scene binding on
        // the same keystroke from also firing. Focus is what earns that: the
        // panel is also offered the key while it is merely hovered, and saving
        // there would swallow Save Scene from under the pointer.
        e.Stop();
    }
}

void ScriptEditorPanel::OnSaveButtonClicked()
{
    SaveScript();
}

void ScriptEditorPanel::UpdateTitle()
{
    std::string title = "Script Editor";
    
    if (!m_CurrentScriptPath.empty())
    {
        // The real on-disk name, not the caller's: registry-resolved paths arrive case-folded
        // on case-insensitive filesystems, so the same file would otherwise title its tab
        // differently depending on which entry point opened it.
        title = Editor::OnDiskFileName(m_CurrentScriptPath);

        if (m_HasUnsavedChanges)
        {
            title += " *";
        }
    }

    SetTitle(title);

    // Update unsaved changes indicator (asterisk)
    if (m_UnsavedIndicator)
    {
        if (m_HasUnsavedChanges && !m_CurrentScriptPath.empty())
        {
            m_UnsavedIndicator->RemoveClass("hidden");
        }
        else
        {
            m_UnsavedIndicator->AddClass("hidden");
        }
    }

    // Update save button state
    if (m_SaveButton)
    {
        // Could disable save button when no changes, but keeping it enabled is more user-friendly
    }
}

void ScriptEditorPanel::LoadScriptFromFile(const std::filesystem::path& path)
{
    try
    {
        GameEngine::String content;
        if (!GameEngine::ReadFileTextShared(path, content))
        {
            Logger::Log::Error("ScriptEditorPanel: Failed to open file for reading: {}", path.string());
            return;
        }

        if (m_TextArea)
        {
            m_TextArea->SetValue(content);
            m_LastSavedContent = content;
        }
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("ScriptEditorPanel: Exception while loading script: {}", e.what());
    }
}

bool ScriptEditorPanel::WriteScriptToFile(const std::filesystem::path& path, const std::string& content)
{
    try
    {
        // Create parent directories if they don't exist
        std::filesystem::path parentDir = path.parent_path();
        if (!parentDir.empty() && !std::filesystem::exists(parentDir))
        {
            std::filesystem::create_directories(parentDir);
        }

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            Logger::Log::Error("ScriptEditorPanel: Failed to open file for writing: {}", path.string());
            return false;
        }

        file.write(content.c_str(), content.size());
        file.close();

        return true;
    }
    catch (const std::exception& e)
    {
        Logger::Log::Error("ScriptEditorPanel: Exception while saving script: {}", e.what());
        return false;
    }
}

void ScriptEditorPanel::OnSearchTextChanged(const std::string& searchText)
{
    m_CurrentSearchText = searchText;
    
    if (searchText.empty()) {
        m_SearchMatches.clear();
        m_CurrentMatchIndex = -1;
        if (m_TextArea) {
            m_TextArea->ClearSearchHighlights();
        }
        return;
    }
    
    FindAllMatches(searchText);
    
    // Auto-navigate to first match if there are matches
    if (!m_SearchMatches.empty()) {
        NavigateToMatch(0);
    } else {
        m_CurrentMatchIndex = -1;
        if (m_TextArea) {
            m_TextArea->SetSearchHighlights({}, 0, -1);
        }
    }
}

void ScriptEditorPanel::FindAllMatches(const std::string& searchText)
{
    m_SearchMatches.clear();
    
    if (searchText.empty() || !m_TextArea)
        return;
    
    const std::string& text = m_TextArea->GetValue();
    if (text.empty())
        return;
    
    // Case-insensitive search
    std::string lowerText = text;
    std::string lowerSearch = searchText;
    std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), [](char c) { return ToLowerChar(c); });
    std::transform(lowerSearch.begin(), lowerSearch.end(), lowerSearch.begin(), [](char c) { return ToLowerChar(c); });
    
    size_t pos = 0;
    while ((pos = lowerText.find(lowerSearch, pos)) != std::string::npos) {
        m_SearchMatches.push_back(pos);
        pos += 1; // Move past this match to find overlapping matches
    }
}

float ScriptEditorPanel::GetScriptFontSizePx() const
{
    return kScriptEditorBaseFontSizePx * (m_TextScalePercent / 100.0f);
}

void ScriptEditorPanel::ApplyTextScalePercent(float percent, bool persist)
{
    const int optionIndex = FindClosestTextScaleOptionIndex(percent);
    const int snappedPercent = ScriptTextScaleOptions()[static_cast<size_t>(optionIndex)];
    m_TextScalePercent = static_cast<float>(snappedPercent);

    if (m_TextArea)
    {
        m_TextArea->Overrides().Set(Style::FontSize, StyleLength::Px(GetScriptFontSizePx()));
        m_TextArea->MarkDirty(StyleDirty | LayoutDirty | VisualDirty);
    }

    if (m_ScrollView)
    {
        m_ScrollView->MarkDirty(LayoutDirty | VisualDirty);
    }

    if (m_TextScaleDropdown)
    {
        const std::string value = std::to_string(snappedPercent);
        if (m_TextScaleDropdown->GetSelectedValue() != value)
        {
            m_SyncingTextScaleDropdown = true;
            m_TextScaleDropdown->SetSelectedValue(value);
            m_SyncingTextScaleDropdown = false;
        }
    }

    if (persist)
    {
        Editor::ScriptEditorSettings::Get().SetTextScalePercent(m_TextScalePercent);
    }
}

void ScriptEditorPanel::AdjustTextScaleFromScroll(float scrollY)
{
    const auto& options = ScriptTextScaleOptions();
    int index = FindClosestTextScaleOptionIndex(m_TextScalePercent);
    if (scrollY < 0.0f)
        index = std::min(index + 1, static_cast<int>(options.size()) - 1);
    else
        index = std::max(index - 1, 0);

    ApplyTextScalePercent(static_cast<float>(options[static_cast<size_t>(index)]), true);
}

void ScriptEditorPanel::NavigateToMatch(int index)
{
    if (m_SearchMatches.empty() || !m_TextArea || !m_ScrollView)
        return;
    
    // Wrap index
    if (index < 0) {
        index = (int)m_SearchMatches.size() - 1;
    } else if (index >= (int)m_SearchMatches.size()) {
        index = 0;
    }
    
    m_CurrentMatchIndex = index;
    size_t matchPos = m_SearchMatches[index];
    
    // Update highlights
    m_TextArea->SetSearchHighlights(m_SearchMatches, m_CurrentSearchText.size(), m_CurrentMatchIndex);
    
    // Scroll to make match visible
    // Calculate line and position of match
    const std::string& text = m_TextArea->GetValue();
    int lineIdx = 0;
    for (size_t i = 0; i < matchPos && i < text.size(); ++i) {
        if (text[i] == '\n') lineIdx++;
    }
    
    ScriptTextArea* sta = dynamic_cast<ScriptTextArea*>(m_TextArea);
    const float lineH = sta ? sta->GetLineAdvancePx()
                            : Editor::ScriptEditorSettings::Get().GetLineHeight() * GetScriptFontSizePx();
    const size_t visualLine = sta ? sta->GetVisualLineForBytePos(matchPos) : static_cast<size_t>(lineIdx);
    float matchY = static_cast<float>(visualLine) * lineH;
    
    // Scroll to show the match (with some padding)
    float viewportH = m_ScrollView->GetClipViewport() ? m_ScrollView->GetClipViewport()->GetLayoutHeight() : 300.0f;
    float currentScrollY = m_ScrollView->GetScrollY();
    
    // Check if match is outside visible area
    if (matchY < currentScrollY || matchY > currentScrollY + viewportH - lineH * 2) {
        // Center the match in the viewport
        float newScrollY = std::max(0.0f, matchY - viewportH / 2.0f);
        m_ScrollView->SetScrollY(newScrollY);
    }
}

void ScriptEditorPanel::NavigateToNextMatch()
{
    if (m_SearchMatches.empty())
        return;
    NavigateToMatch(m_CurrentMatchIndex + 1);
}

void ScriptEditorPanel::NavigateToPreviousMatch()
{
    if (m_SearchMatches.empty())
        return;
    NavigateToMatch(m_CurrentMatchIndex - 1);
}

void ScriptEditorPanel::ParseScriptVariables()
{
    if (!m_TextArea) {
        m_ScriptVariables.clear();
        return;
    }

    const std::string& source = m_TextArea->GetValue();
    m_ScriptVariables = ParseVariablesFromSource(source);
    ScanMethodNames(source);

    if (m_OnScriptMembersChanged)
        m_OnScriptMembersChanged(m_ScriptMethods);
}

std::vector<ScriptVariable> ScriptEditorPanel::ParseVariablesFromSource(const std::string& source)
{
    std::vector<ScriptVariable> variables;
    
    // Simple C# field parser
    // Looks for patterns like:
    // - public <type> <name> = <value>;
    // - [SerializeField] private <type> <name> = <value>;
    // - public <type> <name>;
    
    size_t pos = 0;
    size_t lineNumber = 1;
    bool hasSerializeFieldAttr = false;
    
    while (pos < source.size()) {
        // Track line numbers
        if (source[pos] == '\n') {
            lineNumber++;
            pos++;
            continue;
        }
        
        // Skip whitespace
        while (pos < source.size() && IsSpace(source[pos]) && source[pos] != '\n') {
            pos++;
        }
        
        if (pos >= source.size()) break;
        
        // Check for [SerializeField] attribute
        if (source[pos] == '[') {
            size_t attrStart = pos;
            size_t attrEnd = source.find(']', pos);
            if (attrEnd != std::string::npos) {
                std::string attr = source.substr(attrStart + 1, attrEnd - attrStart - 1);
                // Remove whitespace
                attr.erase(std::remove_if(attr.begin(), attr.end(), [](char c) { return IsSpace(c); }), attr.end());
                if (attr == "SerializeField") {
                    hasSerializeFieldAttr = true;
                }
                pos = attrEnd + 1;
                continue;
            }
        }
        
        // Check for access modifiers
        bool isPublic = false;
        bool isPrivate = false;
        
        if (source.compare(pos, 6, "public") == 0 && (pos + 6 >= source.size() || !IsAlnum(source[pos + 6]))) {
            isPublic = true;
            pos += 6;
        } else if (source.compare(pos, 7, "private") == 0 && (pos + 7 >= source.size() || !IsAlnum(source[pos + 7]))) {
            isPrivate = true;
            pos += 7;
        } else if (source.compare(pos, 9, "protected") == 0 && (pos + 9 >= source.size() || !IsAlnum(source[pos + 9]))) {
            pos += 9;
        } else {
            // Not a field declaration we care about - skip to next line
            while (pos < source.size() && source[pos] != '\n') pos++;
            hasSerializeFieldAttr = false;
            continue;
        }
        
        // Skip whitespace after access modifier
        while (pos < source.size() && IsSpace(source[pos]) && source[pos] != '\n') pos++;
        
        // Skip optional modifiers (static, readonly, const)
        bool isStatic = false;
        if (source.compare(pos, 6, "static") == 0 && (pos + 6 >= source.size() || !IsAlnum(source[pos + 6]))) {
            isStatic = true;
            pos += 6;
            while (pos < source.size() && IsSpace(source[pos]) && source[pos] != '\n') pos++;
        }
        if (source.compare(pos, 8, "readonly") == 0 && (pos + 8 >= source.size() || !IsAlnum(source[pos + 8]))) {
            pos += 8;
            while (pos < source.size() && IsSpace(source[pos]) && source[pos] != '\n') pos++;
        }
        if (source.compare(pos, 5, "const") == 0 && (pos + 5 >= source.size() || !IsAlnum(source[pos + 5]))) {
            pos += 5;
            while (pos < source.size() && IsSpace(source[pos]) && source[pos] != '\n') pos++;
        }
        
        // Skip static fields and methods
        if (isStatic) {
            while (pos < source.size() && source[pos] != '\n') pos++;
            hasSerializeFieldAttr = false;
            continue;
        }
        
        // Read type name
        size_t typeStart = pos;
        while (pos < source.size() && (IsAlnum(source[pos]) || source[pos] == '_' || source[pos] == '<' || source[pos] == '>' || source[pos] == '.')) {
            pos++;
        }
        std::string typeName = source.substr(typeStart, pos - typeStart);
        
        if (typeName.empty()) {
            while (pos < source.size() && source[pos] != '\n') pos++;
            hasSerializeFieldAttr = false;
            continue;
        }
        
        // Skip class, struct, interface, enum, delegate declarations
        if (typeName == "class" || typeName == "struct" || typeName == "interface" || 
            typeName == "enum" || typeName == "delegate" || typeName == "event" ||
            typeName == "void" || typeName == "override" || typeName == "virtual" ||
            typeName == "abstract" || typeName == "partial") {
            while (pos < source.size() && source[pos] != '\n') pos++;
            hasSerializeFieldAttr = false;
            continue;
        }
        
        // Skip whitespace
        while (pos < source.size() && IsSpace(source[pos]) && source[pos] != '\n') pos++;
        
        // Read variable name
        size_t nameStart = pos;
        while (pos < source.size() && (IsAlnum(source[pos]) || source[pos] == '_')) {
            pos++;
        }
        std::string varName = source.substr(nameStart, pos - nameStart);
        
        if (varName.empty()) {
            while (pos < source.size() && source[pos] != '\n') pos++;
            hasSerializeFieldAttr = false;
            continue;
        }
        
        // Skip whitespace
        while (pos < source.size() && IsSpace(source[pos]) && source[pos] != '\n') pos++;
        
        // Check for method declaration (parenthesis after name means it's a method)
        if (pos < source.size() && source[pos] == '(') {
            while (pos < source.size() && source[pos] != '\n') pos++;
            hasSerializeFieldAttr = false;
            continue;
        }
        
        // Read value if there's an assignment
        std::string value;
        size_t valueStartPos = 0;
        size_t valueEndPos = 0;
        
        if (pos < source.size() && source[pos] == '=') {
            pos++; // Skip '='
            while (pos < source.size() && IsSpace(source[pos]) && source[pos] != '\n') pos++;
            
            valueStartPos = pos;
            
            // Read until semicolon, handling strings and nested structures
            int braceDepth = 0;
            bool inString = false;
            char stringChar = 0;
            
            while (pos < source.size() && source[pos] != '\n') {
                if (!inString) {
                    if (source[pos] == '"' || source[pos] == '\'') {
                        inString = true;
                        stringChar = source[pos];
                    } else if (source[pos] == '{' || source[pos] == '(' || source[pos] == '[') {
                        braceDepth++;
                    } else if (source[pos] == '}' || source[pos] == ')' || source[pos] == ']') {
                        braceDepth--;
                    } else if (source[pos] == ';' && braceDepth == 0) {
                        break;
                    }
                } else {
                    if (source[pos] == stringChar && (pos == 0 || source[pos - 1] != '\\')) {
                        inString = false;
                    }
                }
                pos++;
            }
            
            valueEndPos = pos;
            value = source.substr(valueStartPos, valueEndPos - valueStartPos);
            // Trim whitespace
            while (!value.empty() && IsSpace(value.back())) value.pop_back();
        }
        
        // Only include public fields or fields with [SerializeField]
        if (isPublic || hasSerializeFieldAttr) {
            ScriptVariable var;
            var.Name = varName;
            var.TypeName = typeName;
            var.Value = value;
            var.IsPublic = isPublic;
            var.HasSerializeField = hasSerializeFieldAttr;
            var.LineNumber = lineNumber;
            var.StartPos = valueStartPos;
            var.EndPos = valueEndPos;
            
            Logger::Log::Info("[ScriptEditorPanel] Parsed variable: {} ({}) = '{}' at line {}", 
                              varName, typeName, value, lineNumber);
            
            variables.push_back(var);
        }
        
        hasSerializeFieldAttr = false;
        
        // Move to end of line
        while (pos < source.size() && source[pos] != '\n') pos++;
    }
    
    return variables;
}

bool ScriptEditorPanel::UpdateVariableValue(const std::string& varName, const std::string& newValue)
{
    if (!m_TextArea) return false;
    
    // Find the variable
    ScriptVariable* targetVar = nullptr;
    for (auto& var : m_ScriptVariables) {
        if (var.Name == varName) {
            targetVar = &var;
            break;
        }
    }

    if (!targetVar || targetVar->StartPos == 0) {
        return false;
    }

    // Get current source
    std::string source = m_TextArea->GetValue();

    // Replace the value in the source
    if (targetVar->StartPos < source.size() && targetVar->EndPos <= source.size()) {
        std::string newSource = source.substr(0, targetVar->StartPos) + newValue + source.substr(targetVar->EndPos);

        m_IsLoading = true;
        m_TextArea->SetValue(newSource);
        m_IsLoading = false;

        // Update the variable's stored value
        targetVar->Value = newValue;
        
        // Re-parse to update positions
        ParseScriptVariables();
        
        // Re-highlight the value (in case it's currently highlighted)
        HighlightVariable(varName);
        
        // Mark as changed
        m_HasUnsavedChanges = true;
        UpdateTitle();
        
        return true;
    }
    
    return false;
}

void ScriptEditorPanel::HighlightVariable(const std::string& varName)
{
    Logger::Log::Info("[ScriptEditorPanel] HighlightVariable called for: {}", varName);
    
    if (!m_TextArea || !m_ScrollView) {
        Logger::Log::Warning("[ScriptEditorPanel] HighlightVariable: m_TextArea or m_ScrollView is null");
        return;
    }
    
    // Always clear any existing highlight first
    ClearVariableHighlight();
    
    // If varName is empty, we're just clearing (already done above)
    if (varName.empty()) {
        return;
    }
    
    // Find the variable
    const ScriptVariable* targetVar = nullptr;
    for (const auto& var : m_ScriptVariables) {
        if (var.Name == varName) {
            targetVar = &var;
            break;
        }
    }

    if (!targetVar) {
        Logger::Log::Warning("[ScriptEditorPanel] HighlightVariable: Variable '{}' not found", varName);
        return;
    }
    
    // Highlight the value (not the variable name)
    const std::string& source = m_TextArea->GetValue();
    
    // Use the stored value position from parsing
    size_t valueStartPos = targetVar->StartPos;
    size_t valueEndPos = targetVar->EndPos;
    
    // If we don't have valid positions, try to find the value manually
    if (valueStartPos == 0 || valueEndPos == 0 || valueStartPos >= source.size()) {
        // Find the variable declaration line
        size_t lineStart = 0;
        size_t currentLine = 1;
        
        // Find the start of the target line
        while (currentLine < targetVar->LineNumber && lineStart < source.size()) {
            if (source[lineStart] == '\n') {
                currentLine++;
            }
            lineStart++;
        }
        
        // Find the '=' sign after the variable name
        size_t equalsPos = source.find('=', lineStart);
        if (equalsPos != std::string::npos) {
            equalsPos++; // Skip '='
            // Skip whitespace after '='
            while (equalsPos < source.size() && IsSpace(source[equalsPos]) && source[equalsPos] != '\n') {
                equalsPos++;
            }
            valueStartPos = equalsPos;
            
            // Find the end of the value (semicolon or end of line)
            valueEndPos = source.find(';', valueStartPos);
            if (valueEndPos == std::string::npos) {
                size_t lineEnd = source.find('\n', valueStartPos);
                valueEndPos = (lineEnd != std::string::npos) ? lineEnd : source.size();
            }
        } else {
            Logger::Log::Warning("[ScriptEditorPanel] No '=' found for variable '{}'", varName);
            return;
        }
    }
    
    size_t valueLength = (valueEndPos > valueStartPos) ? (valueEndPos - valueStartPos) : 0;
    
    Logger::Log::Info("[ScriptEditorPanel] Highlighting value for variable '{}' at position {} length {} (line {})", 
                      varName, valueStartPos, valueLength, targetVar->LineNumber);
    
    // Use variable highlight (blue background like search highlights) - highlight the VALUE
    if (ScriptTextArea* scriptTextArea = dynamic_cast<ScriptTextArea*>(m_TextArea)) {
        scriptTextArea->SetVariableHighlight(valueStartPos, valueLength);
    } else {
        // Fallback to selection if not a ScriptTextArea
        m_TextArea->SetSelection(static_cast<int>(valueStartPos), static_cast<int>(valueEndPos));
    }
    
    // Scroll to make the line visible
    ScriptTextArea* sta = dynamic_cast<ScriptTextArea*>(m_TextArea);
    const float lineH = sta ? sta->GetLineAdvancePx()
                            : Editor::ScriptEditorSettings::Get().GetLineHeight() * GetScriptFontSizePx();
    float targetY = (targetVar->LineNumber - 1) * lineH;
    
    float viewportH = m_ScrollView->GetClipViewport() ? m_ScrollView->GetClipViewport()->GetLayoutHeight() : 300.0f;
    float currentScrollY = m_ScrollView->GetScrollY();
    
    // Check if target is outside visible area
    if (targetY < currentScrollY || targetY > currentScrollY + viewportH - lineH * 2) {
        // Center the line in the viewport
        float newScrollY = std::max(0.0f, targetY - viewportH / 3.0f);
        m_ScrollView->SetScrollY(newScrollY);
    }
}

void ScriptEditorPanel::ClearVariableHighlight()
{
    if (ScriptTextArea* scriptTextArea = dynamic_cast<ScriptTextArea*>(m_TextArea)) {
        scriptTextArea->ClearVariableHighlight();
        scriptTextArea->ClearDiagnosticHighlight();
    }
}

void ScriptEditorPanel::CheckCaretPosition()
{
    if (!m_TextArea || !m_OnCaretPositionChanged) {
        return;
    }

    const int caretIndex = m_TextArea->GetCaretIndex();
    if (caretIndex < 0) {
        m_OnCaretPositionChanged("", false);
        return;
    }

    const int selStart = m_TextArea->GetSelectionStart();
    const int selEnd = m_TextArea->GetSelectionEnd();
    const bool hasTextSelection = selStart >= 0 && selEnd >= 0 && selStart != selEnd;

    // Range [rangeStart, rangeEnd) for overlap with parsed value spans [startPos, endPos).
    int rangeStart = caretIndex;
    int rangeEnd = caretIndex + 1;
    if (hasTextSelection) {
        rangeStart = std::min(selStart, selEnd);
        rangeEnd = std::max(selStart, selEnd);
    }

    if (rangeStart < 0) {
        m_OnCaretPositionChanged("", false);
        return;
    }

    const size_t rs = static_cast<size_t>(rangeStart);
    const size_t re = static_cast<size_t>(rangeEnd);

    for (const auto& var : m_ScriptVariables) {
        if (var.EndPos <= var.StartPos) {
            continue;
        }
        if (rs < var.EndPos && re > var.StartPos) {
            m_OnCaretPositionChanged(var.Name, hasTextSelection);
            return;
        }
    }

    m_OnCaretPositionChanged("", false);
}

// ---------------------------------------------------------------------------
// Code completion
// ---------------------------------------------------------------------------

namespace {

static const char* kCSharpKeywords[] = {
    "abstract","as","base","bool","break","byte","case","catch","char",
    "checked","class","const","continue","decimal","default","delegate",
    "do","double","else","enum","event","explicit","extern","false",
    "finally","fixed","float","for","foreach","goto","if","implicit",
    "in","int","interface","internal","is","lock","long","namespace",
    "new","null","object","operator","out","override","params","private",
    "protected","public","readonly","ref","return","sbyte","sealed",
    "short","sizeof","stackalloc","static","string","struct","switch",
    "this","throw","true","try","typeof","uint","ulong","unchecked",
    "unsafe","ushort","using","virtual","void","volatile","while"
};

static const char* kCommonTypes[] = {
    "Action","Array","Boolean","Char","DateTime","Dictionary",
    "Double","Exception","Func","Guid","HashSet","ICollection",
    "IDictionary","IEnumerable","IList","Int32","Int64","List",
    "Math","Object","Queue","Random","Single","Stack","String",
    "StringBuilder","Task","Thread","Tuple"
};

static bool IsKeyword(const std::string& word)
{
    for (const char* kw : kCSharpKeywords)
        if (word == kw) return true;
    return false;
}

} // anonymous namespace

std::string ScriptEditorPanel::GetWordPrefixAtCaret(size_t* outWordStart) const
{
    if (!m_TextArea)
    {
        if (outWordStart) *outWordStart = 0;
        return {};
    }
    const int caretIdx = m_TextArea->GetCaretIndex();
    if (caretIdx <= 0)
    {
        if (outWordStart) *outWordStart = 0;
        return {};
    }
    const std::string& text = m_TextArea->GetValue();
    int start = caretIdx;
    while (start > 0 && (IsAlnum(text[start - 1]) || text[start - 1] == '_'))
        --start;
    if (outWordStart) *outWordStart = static_cast<size_t>(start);
    return text.substr(static_cast<size_t>(start), static_cast<size_t>(caretIdx - start));
}

std::vector<std::string> ScriptEditorPanel::BuildSuggestions(const std::string& prefix) const
{
    if (prefix.empty() || std::isdigit(ToUChar(prefix[0])))
        return {};

    std::string lowerPrefix(prefix.size(), '\0');
    for (size_t i = 0; i < prefix.size(); ++i)
        lowerPrefix[i] = ToLowerChar(prefix[i]);

    std::vector<std::string> result;

    auto tryAdd = [&](const std::string& s) {
        if (s.size() <= prefix.size()) return; // nothing to complete
        // Case-insensitive prefix check
        for (size_t i = 0; i < prefix.size(); ++i)
            if (ToLowerChar(s[i]) != lowerPrefix[i]) return;
        // Deduplicate
        for (const auto& existing : result)
            if (existing == s) return;
        result.push_back(s);
    };

    for (const char* kw : kCSharpKeywords) tryAdd(kw);
    for (const char* t  : kCommonTypes)    tryAdd(t);
    for (const auto& var : m_ScriptVariables) {
        tryAdd(var.Name);
        tryAdd(var.TypeName);
    }
    for (const auto& m : m_ScriptMethodNames) tryAdd(m);

    std::sort(result.begin(), result.end());
    if (result.size() > 20) result.resize(20);
    return result;
}

void ScriptEditorPanel::ScanMethodNames(const std::string& source)
{
    m_ScriptMethodNames.clear();
    m_ScriptMethods.clear();

    // Build line-start index for O(log n) line-number lookup
    std::vector<size_t> lineStarts;
    lineStarts.push_back(0);
    for (size_t k = 0; k < source.size(); ++k)
        if (source[k] == '\n') lineStarts.push_back(k + 1);

    // Keywords that precede a type instantiation or expression — not a return type
    static const char* kNotReturnType[] = {
        "new", "typeof", "nameof", "sizeof", "return", "throw", "case", "in", "as", "is"
    };

    for (size_t i = 1; i < source.size(); ++i)
    {
        if (source[i] != '(') continue;
        size_t j = i;
        while (j > 0 && IsSpace(source[j - 1])) --j;
        const size_t end = j;
        while (j > 0 && (IsAlnum(source[j - 1]) || source[j - 1] == '_')) --j;
        if (j == end) continue;
        std::string name = source.substr(j, end - j);
        if (name.empty() || std::isdigit(ToUChar(name[0]))) continue;
        if (IsKeyword(name)) continue;

        // A method declaration always has a return type (or modifier) directly before the name.
        // Filter out call sites: obj.Method(  Method(arg  = Call(  ,Call(  etc.
        if (j == 0) continue;
        size_t k2 = j;
        while (k2 > 0 && IsSpace(source[k2 - 1])) --k2;
        if (k2 == 0) continue;
        const char prevChar = source[k2 - 1];
        if (prevChar == '.' || prevChar == '(' || prevChar == ',' || prevChar == '=') continue;
        if (!IsAlnum(prevChar) && prevChar != '_' && prevChar != '>') continue;

        // Extract the preceding token to reject non-return-type keywords
        const size_t prevEnd = k2;
        size_t prevStart = prevEnd;
        while (prevStart > 0 && (IsAlnum(source[prevStart - 1]) || source[prevStart - 1] == '_'))
            --prevStart;
        const std::string prevToken = source.substr(prevStart, prevEnd - prevStart);
        bool skip = false;
        for (const char* kw : kNotReturnType)
            if (prevToken == kw) { skip = true; break; }
        if (skip) continue;

        // Compute 1-based line number for this identifier
        auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), j);
        if (it != lineStarts.begin()) --it;
        const size_t lineNumber = static_cast<size_t>(std::distance(lineStarts.begin(), it)) + 1;

        m_ScriptMethodNames.push_back(name);
        m_ScriptMethods.push_back({std::move(name), lineNumber});
    }

    // Sort method names for completion deduplication
    std::sort(m_ScriptMethodNames.begin(), m_ScriptMethodNames.end());
    m_ScriptMethodNames.erase(
        std::unique(m_ScriptMethodNames.begin(), m_ScriptMethodNames.end()),
        m_ScriptMethodNames.end());

    // Sort methods by line number; deduplicate by name keeping first occurrence
    std::sort(m_ScriptMethods.begin(), m_ScriptMethods.end(),
              [](const ScriptMethod& a, const ScriptMethod& b) { return a.LineNumber < b.LineNumber; });
    std::vector<ScriptMethod> deduped;
    deduped.reserve(m_ScriptMethods.size());
    std::set<std::string> seen;
    for (auto& m : m_ScriptMethods)
        if (seen.insert(m.Name).second) deduped.push_back(std::move(m));
    m_ScriptMethods = std::move(deduped);
}

void ScriptEditorPanel::ShowCompletions()
{
    if (!m_TextArea || !m_CompletionPopup || !m_ScrollView) return;

    size_t wordStart = 0;
    const std::string prefix = GetWordPrefixAtCaret(&wordStart);
    if (prefix.size() < 1)
    {
        HideCompletions();
        return;
    }

    const std::vector<std::string> suggestions = BuildSuggestions(prefix);
    if (suggestions.empty())
    {
        HideCompletions();
        return;
    }

    m_CompletionWordStart = wordStart;

    // Anchor the popup just below the start of the word being completed, using the
    // text area's actual glyph layout (DPI-correct; accounts for folding and wrap).
    ScriptTextArea* sta = dynamic_cast<ScriptTextArea*>(m_TextArea);
    float caretX = 0.0f, caretY = 0.0f, lineH = 0.0f;
    if (!sta || !sta->GetBytePositionLocalRect(wordStart, caretX, caretY, lineH))
    {
        HideCompletions();
        return;
    }

    // Map from text-area-local coordinates into the popup parent's coordinate space
    // via absolute layout positions (m_TextArea's layout origin already includes the
    // ScrollView scroll offset, so no manual scroll subtraction is needed).
    UIElement* popupParent = m_CompletionPopup->GetParent();
    const float baseX = popupParent ? popupParent->GetLayoutX() : 0.0f;
    const float baseY = popupParent ? popupParent->GetLayoutY() : 0.0f;
    const float popupX = m_TextArea->GetLayoutX() + caretX - baseX;
    const float popupY = m_TextArea->GetLayoutY() + caretY + lineH - baseY;

    // Match the list to the editor's current text size 1:1 (font + row height).
    m_CompletionPopup->Show(suggestions, popupX, popupY, GetScriptFontSizePx(), lineH);
}

void ScriptEditorPanel::HideCompletions()
{
    if (m_CompletionPopup)
        m_CompletionPopup->Hide();
}

void ScriptEditorPanel::AcceptCompletion(const std::string& text)
{
    HideCompletions();
    if (!m_TextArea || text.empty()) return;

    const int caretIdx = m_TextArea->GetCaretIndex();
    if (caretIdx < 0 || m_CompletionWordStart > static_cast<size_t>(caretIdx)) return;

    std::string source = m_TextArea->GetValue();
    const size_t ci = static_cast<size_t>(caretIdx);
    const std::string newSource =
        source.substr(0, m_CompletionWordStart) + text + source.substr(ci);

    m_TextArea->SetValue(newSource); // goes through normal undo pipeline
    const int newCaret = static_cast<int>(m_CompletionWordStart + text.size());
    m_TextArea->SetSelection(newCaret, newCaret);
}

void ScriptEditorPanel::ScrollToLine(size_t lineNumber, const std::string& highlightName, size_t diagnosticColumn)
{
    if (!m_TextArea || !m_ScrollView || lineNumber == 0) return;

    // Find the byte offset for the start of the requested line
    const std::string& text = m_TextArea->GetValue();
    size_t currentLine = 1;
    size_t bytePos = 0;
    while (bytePos < text.size() && currentLine < lineNumber)
    {
        if (text[bytePos] == '\n') ++currentLine;
        ++bytePos;
    }

    // Expand any fold regions that hide this line before computing the visual position
    ScriptTextArea* sta = dynamic_cast<ScriptTextArea*>(m_TextArea);
    if (sta) sta->EnsureBytePositionVisible(bytePos);

    m_TextArea->SetSelection(static_cast<int>(bytePos), static_cast<int>(bytePos));

    // Get visual segment containing bytePos (handles folding + word-wrap correctly)
    const size_t visualLine = sta ? sta->GetVisualLineForBytePos(bytePos) : (lineNumber - 1);
    const float lineH     = sta ? sta->GetLineAdvancePx() : GetScriptFontSizePx();
    const float padT      = m_TextArea->GetResolvedStyle().Layout.Padding.Top;
    const float lineY     = padT + static_cast<float>(visualLine) * lineH;
    const float viewportH = m_ScrollView->GetClipViewport()
                                ? m_ScrollView->GetClipViewport()->GetLayoutHeight()
                                : 300.0f;
    m_ScrollView->SetScrollY(std::max(0.0f, lineY - viewportH / 2.0f));

    // Highlight the function name on this line.
    // When a name is provided search for it directly; otherwise fall back to
    // scanning for the identifier immediately before the first '(' on the line.
    ClearVariableHighlight();
    if (sta)
    {
        const size_t lineEnd   = text.find('\n', bytePos);
        const size_t searchEnd = (lineEnd != std::string::npos) ? lineEnd : text.size();

        size_t hlStart = std::string::npos;
        size_t hlLen   = 0;

        if (!highlightName.empty())
        {
            // Search for the exact name within this line
            size_t pos = bytePos;
            while (pos < searchEnd)
            {
                size_t found = text.find(highlightName, pos);
                if (found == std::string::npos || found >= searchEnd) break;
                // Ensure it's a whole identifier (not part of a longer name)
                bool okBefore = (found == 0 || (!IsAlnum(text[found - 1]) && text[found - 1] != '_'));
                size_t afterEnd = found + highlightName.size();
                bool okAfter  = (afterEnd >= text.size() || (!IsAlnum(text[afterEnd]) && text[afterEnd] != '_'));
                if (okBefore && okAfter)
                {
                    hlStart = found;
                    hlLen   = highlightName.size();
                    break;
                }
                pos = found + 1;
            }
        }

        if (hlStart == std::string::npos && highlightName.empty())
        {
            hlStart = bytePos;
            hlLen = searchEnd > bytePos ? searchEnd - bytePos : 0;
            if (hlLen == 0 && bytePos < text.size())
                hlLen = 1;
        }

        if (hlStart == std::string::npos)
        {
            // Fallback: identifier immediately before first '(' on this line
            size_t parenPos = text.find('(', bytePos);
            if (parenPos != std::string::npos && parenPos < searchEnd)
            {
                size_t j = parenPos;
                while (j > bytePos && IsSpace(text[j - 1])) --j;
                const size_t nameEnd = j;
                while (j > bytePos && (IsAlnum(text[j - 1]) || text[j - 1] == '_')) --j;
                if (j < nameEnd)
                {
                    hlStart = j;
                    hlLen   = nameEnd - j;
                }
            }
        }

        if (hlStart != std::string::npos && hlLen > 0)
            sta->SetVariableHighlight(hlStart, hlLen);

        sta->ClearDiagnosticHighlight();
        if (diagnosticColumn > 0 && searchEnd > bytePos)
        {
            size_t diagPos = bytePos + diagnosticColumn - 1;
            if (diagPos >= searchEnd)
                diagPos = searchEnd - 1;
            while (diagPos < searchEnd && IsSpace(text[diagPos]))
                ++diagPos;

            if (diagPos < searchEnd)
            {
                size_t diagStart = diagPos;
                size_t diagEnd = diagPos + 1;
                auto isTokenChar = [](char c) {
                    return IsAlnum(c) || c == '_';
                };

                if (isTokenChar(text[diagPos]))
                {
                    while (diagStart > bytePos && isTokenChar(text[diagStart - 1]))
                        --diagStart;
                    while (diagEnd < searchEnd && isTokenChar(text[diagEnd]))
                        ++diagEnd;
                }
                else
                {
                    while (diagEnd < searchEnd && !IsSpace(text[diagEnd]) && !isTokenChar(text[diagEnd]))
                        ++diagEnd;
                }

                if (diagEnd > diagStart)
                    sta->SetDiagnosticHighlight(diagStart, diagEnd - diagStart);
            }
        }
    }
}

} // namespace GameEngine
