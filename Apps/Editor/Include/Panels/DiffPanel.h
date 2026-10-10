#pragma once

#include <memory>
#include <filesystem>
#include <vector>
#include <string>
#include "UI/Controls/DockPanel.h"

namespace GameEngine {

class TextArea;
class ScrollView;
class Label;
class UIElement;

/**
 * @brief Represents a line in a diff with its type and content.
 */
enum class DiffLineType {
    Unchanged,
    Added,
    Deleted,
    Modified
};

struct DiffLine {
    DiffLineType Type;
    std::string Content;
    int OriginalLineNumber;  // Line number in original file (-1 if added)
    int CurrentLineNumber;   // Line number in current file (-1 if deleted)
};

/**
 * @brief Editor panel for displaying file diffs side-by-side.
 * 
 * DiffPanel provides a dockable panel that displays two file versions
 * side-by-side with diff visualization including:
 * - Line-by-line highlighting (added/deleted/modified/unchanged)
 * - Insertion arrows
 * - Synchronized scrolling
 * - Line numbers
 */
class DiffPanel : public DockPanel {
public:
    std::string_view DeclaredTabIconClass() const override { return "dock-diff-icon"; }

    DiffPanel();
    ~DiffPanel() override;

    /**
     * @brief Open a diff view for a file.
     * @param filePath Path to the file to diff
     * @param originalContent Original version content from VCS
     * @param currentContent Current working directory version content
     * @return true if the diff was successfully opened
     */
    bool OpenDiff(const std::filesystem::path& filePath,
                  const std::string& originalContent,
                  const std::string& currentContent);

    /**
     * @brief Get the currently open file path.
     * @return Path to the current file, or empty if none is open
     */
    const std::filesystem::path& GetCurrentFilePath() const { return m_CurrentFilePath; }

    // Override to register scroll handlers after layout
    void OnPostLayout() override;

private:
    void SetupUI();
    void SetupTextContent();
    void RegisterScrollHandlers();
    void CalculateDiff(const std::string& originalContent, const std::string& currentContent);
    void UpdateDisplay();
    void UpdateSceneDisplay();
    void SynchronizeScroll(ScrollView* source, ScrollView* target);

    // UI elements (owned by UI tree via DockPanel::AddChild)
    UIElement* m_Container = nullptr;
    UIElement* m_LeftPanel = nullptr;
    UIElement* m_RightPanel = nullptr;
    ScrollView* m_LeftScrollView = nullptr;
    ScrollView* m_RightScrollView = nullptr;
    TextArea* m_LeftTextArea = nullptr;
    TextArea* m_RightTextArea = nullptr;
    Label* m_LeftLabel = nullptr;
    Label* m_RightLabel = nullptr;

    // State
    std::filesystem::path m_CurrentFilePath;
    std::string m_OriginalContent;
    std::string m_CurrentContent;
    std::vector<DiffLine> m_DiffLines;
    bool m_IsScrolling = false; // Prevent scroll feedback loops
    bool m_ScrollHandlersRegistered = false; // Track if handlers are registered
};

} // namespace GameEngine
