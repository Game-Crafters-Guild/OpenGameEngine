#pragma once

#include <functional>
#include <memory>
#include <filesystem>
#include <vector>
#include <string>
#include "UI/Controls/DockPanel.h"

namespace GameEngine {

class TextArea;
class ScriptTextArea;
class ScrollView;
class Button;
class Dropdown;
class Label;
class TextField;
class CompletionPopup;
class SaveSceneChangesModal;
namespace Editor { class UndoRedoService; }

/**
 * @brief Represents a variable/field extracted from a C# script.
 */
struct ScriptVariable {
    std::string Name;           // Variable name
    std::string TypeName;       // Type name (int, float, string, Vector3, etc.)
    std::string Value;          // Current value as string
    bool IsPublic = false;      // Is it a public field?
    bool HasSerializeField = false; // Has [SerializeField] attribute?
    size_t LineNumber = 0;      // Line number in source (1-based)
    size_t StartPos = 0;        // Start position in source
    size_t EndPos = 0;          // End position in source (for value replacement)
};

/**
 * @brief Represents a method/function found in a C# script, used for the members outline.
 */
struct ScriptMethod {
    std::string Name;       // Method name
    size_t LineNumber = 0;  // Line number in source (1-based)
};

/**
 * @brief Editor panel for editing C# scripts with syntax highlighting support.
 * 
 * ScriptEditorPanel provides a dockable panel that allows editing script files
 * directly in the editor. It supports:
 * - Multi-line code editing with TextArea
 * - Save functionality
 * - File path tracking
 * - Unsaved changes detection
 */
class ScriptEditorPanel : public DockPanel {
public:
    std::string_view DeclaredTabIconClass() const override { return "script-icon"; }

    ScriptEditorPanel();
    ~ScriptEditorPanel() override;

    // "Script Editor / Save" catalog shortcut. Handled only while focus is
    // inside the panel: the key is also offered to a merely-hovered panel, and
    // consuming it there would swallow the app's Save Scene binding.
    void OnEvent(UIEvent& e) override;

    /**
     * @brief Open a script file for editing.
     * @param scriptPath Path to the .cs script file
     * @return true if the file was successfully opened
     */
    bool OpenScript(const std::filesystem::path& scriptPath);
    void PromptSaveBeforeQuit(std::function<void()> onProceed);

    /**
     * @brief Save the current script to disk.
     * @return true if the file was successfully saved
     */
    bool SaveScript();

    /**
     * @brief Check if there are unsaved changes.
     * @return true if the script has been modified since last save
     */
    bool HasUnsavedChanges() const { return m_HasUnsavedChanges; }

    /**
     * @brief Get the currently open script path.
     * @return Path to the current script, or empty if none is open
     */
    const std::filesystem::path& GetCurrentScriptPath() const { return m_CurrentScriptPath; }

    /**
     * @brief Set callback for when a C# script has been written to disk.
     *
     * Only C# saves fire it: a saved shader reaches the material pipeline through
     * the file watcher and needs nothing from the app.
     */
    void SetOnCSharpScriptSaved(std::function<void(const std::filesystem::path&)> cb) {
        m_OnCSharpScriptSaved = std::move(cb);
    }

    /**
     * @brief Set callback for when a C# script is opened (for Inspector integration).
     *
     * Only C# opens fire it: the payload is the parsed serializable fields, and
     * a shader has none — announcing an empty list for one puts the C# "no
     * serializable fields" hint on a .glsl.
     *
     * @param cb Callback receiving the script path and parsed variables
     */
    void SetOnCSharpScriptOpened(std::function<void(const std::filesystem::path&, const std::vector<ScriptVariable>&)> cb) {
        m_OnCSharpScriptOpened = std::move(cb);
    }

    /**
     * @brief Set callback for when script variables change (text edited).
     */
    void SetOnScriptVariablesChanged(std::function<void(const std::vector<ScriptVariable>&)> cb) {
        m_OnScriptVariablesChanged = std::move(cb);
    }

    /**
     * @brief Set callback for when the script's method list changes.
     * @param cb Callback receiving the updated list of methods
     */
    void SetOnScriptMembersChanged(std::function<void(const std::vector<ScriptMethod>&)> cb) {
        m_OnScriptMembersChanged = std::move(cb);
    }
    
    /**
     * @brief Set callback when caret or selection moves in the script (to sync the Inspector).
     * @param cb Variable name if overlapping a serialized value, or empty if not.
     *           Second arg is true when there is a non-empty selection in that value (inspector should focus the field).
     */
    void SetOnCaretPositionChanged(std::function<void(const std::string& varName, bool focusInspectorField)> cb) {
        m_OnCaretPositionChanged = std::move(cb);
    }

    /**
     * @brief Get the currently parsed script variables.
     */
    const std::vector<ScriptVariable>& GetScriptVariables() const { return m_ScriptVariables; }

    /**
     * @brief Get the currently scanned script methods (for the members outline).
     */
    const std::vector<ScriptMethod>& GetScriptMethods() const { return m_ScriptMethods; }

    /**
     * @brief Scroll the script editor to a specific line and move the caret there.
     * @param lineNumber 1-based line number
     */
    void ScrollToLine(size_t lineNumber, const std::string& highlightName = {}, size_t diagnosticColumn = 0);

    /**
     * @brief Update a variable's value in the script source.
     * @param varName Variable name to update
     * @param newValue New value as string
     * @return true if the variable was found and updated
     */
    bool UpdateVariableValue(const std::string& varName, const std::string& newValue);
    
    /**
     * @brief Highlight and scroll to a variable in the script editor.
     * @param varName Variable name to highlight
     */
    void HighlightVariable(const std::string& varName);
    
    /**
     * @brief Clear variable highlighting in the script editor.
     */
    void ClearVariableHighlight();

    /**
     * @brief Set the editor's undo/redo service for text edit history.
     */
    void SetUndoRedoService(Editor::UndoRedoService* undo) { m_UndoRedo = undo; }

private:
    void SetupUI();
    void OnTextChanged();
    void OnSaveButtonClicked();
    void UpdateTitle();
    void LoadScriptFromFile(const std::filesystem::path& path);
    bool WriteScriptToFile(const std::filesystem::path& path, const std::string& content);
    void OnSearchTextChanged(const std::string& searchText);
    void FindAllMatches(const std::string& searchText);
    void NavigateToMatch(int index);
    void NavigateToNextMatch();
    void NavigateToPreviousMatch();
    void ApplyTextScalePercent(float percent, bool persist);
    void AdjustTextScaleFromScroll(float scrollY);
    float GetScriptFontSizePx() const;
    
    // Variable parsing
    void ParseScriptVariables();
    std::vector<ScriptVariable> ParseVariablesFromSource(const std::string& source);

    // Code completion
    void ShowCompletions();
    void HideCompletions();
    std::string GetWordPrefixAtCaret(size_t* outWordStart) const;
    std::vector<std::string> BuildSuggestions(const std::string& prefix) const;
    void ScanMethodNames(const std::string& source);
    void AcceptCompletion(const std::string& text);

    SaveSceneChangesModal* m_QuitModal = nullptr;

    // UI elements (owned by UI tree via DockPanel::AddChild)
    ScrollView* m_ScrollView = nullptr;
    ScriptTextArea* m_TextArea = nullptr;
    Button* m_SaveButton = nullptr;
    Dropdown* m_TextScaleDropdown = nullptr;
    Label* m_UnsavedIndicator = nullptr; // Asterisk indicator for unsaved changes
    TextField* m_SearchField = nullptr;

    // State
    std::filesystem::path m_CurrentScriptPath;
    // C# gets Inspector variable parsing, the members outline and the saved
    // hook; every other language (GLSL, plain text) is edit-and-save only.
    bool m_IsCSharpScript = false;
    std::string m_LastSavedContent;
    std::string m_LastUndoContent; // For tracking undo states
    bool m_HasUnsavedChanges = false;
    bool m_IsLoading = false;
    bool m_IsUndoRedoOperation = false;
    bool m_SyncingTextScaleDropdown = false;
    float m_TextScalePercent = 100.0f;
    
    // Search state
    std::vector<size_t> m_SearchMatches; // Byte positions of matches
    int m_CurrentMatchIndex = -1;
    std::string m_CurrentSearchText;

    // Undo/Redo service (not owned)
    Editor::UndoRedoService* m_UndoRedo = nullptr;

    // Parsed script variables
    std::vector<ScriptVariable> m_ScriptVariables;

    // Completion state
    CompletionPopup* m_CompletionPopup = nullptr;
    size_t           m_CompletionWordStart = 0;
    std::vector<std::string> m_ScriptMethodNames;

    // Members outline (populated alongside m_ScriptMethodNames)
    std::vector<ScriptMethod> m_ScriptMethods;

    // Callbacks
    std::function<void(const std::filesystem::path&)> m_OnCSharpScriptSaved;
    std::function<void(const std::filesystem::path&, const std::vector<ScriptVariable>&)> m_OnCSharpScriptOpened;
    std::function<void(const std::vector<ScriptVariable>&)> m_OnScriptVariablesChanged;
    std::function<void(const std::vector<ScriptMethod>&)> m_OnScriptMembersChanged;
    std::function<void(const std::string& varName, bool focusInspectorField)> m_OnCaretPositionChanged;
    
    // Helper to check if caret is in a variable value
    void CheckCaretPosition();
};

} // namespace GameEngine
