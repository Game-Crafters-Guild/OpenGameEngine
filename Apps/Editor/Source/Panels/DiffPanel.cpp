#include "Panels/DiffPanel.h"
#include "VersionControl/SceneDiff.h"

#include "UI/Controls/TextArea.h"
#include "UI/ResolvedStyle.h"
#include "Rendering/Text/FontAtlas.h"
#include "Rendering/Text/TextLayout.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIPrimitive.h"
#include "UI/GlyphRunEmitter.h"
#include "UI/UITextureRegistry.h"
#include "Logger/Logger.h"
#include <sstream>
#include <algorithm>
#include <vector>

namespace GameEngine {

namespace {
// Diff line color constants
constexpr float kDiffColorUnchangedR = 0.7f;
constexpr float kDiffColorUnchangedG = 0.7f;
constexpr float kDiffColorUnchangedB = 0.7f;

constexpr float kDiffColorDeletedR = 1.0f;
constexpr float kDiffColorDeletedG = 0.3f;
constexpr float kDiffColorDeletedB = 0.3f;

constexpr float kDiffColorAddedR = 0.3f;
constexpr float kDiffColorAddedG = 1.0f;
constexpr float kDiffColorAddedB = 0.3f;
} // namespace

class DiffTextArea : public TextArea {
public:
    DiffTextArea(bool isLeftSide) : TextArea(), m_IsLeftSide(isLeftSide) {
        SetReadOnly(true);
    }

    void EmitTextGlyphs(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                        float originX, float originY, const TextAreaMetrics& met) override
    {
        using namespace UI;
        using namespace Rendering::Text;

        const std::string& text = GetValue();
        if (text.empty())
            return;

        auto lm = met.Font->GetLineMetrics(met.Px);
        const float lineH = std::max(1.0f, lm.height);
        // Clamped at zero: the diff steps a fixed LineAdvance per line, so a
        // line box tighter than the font would lift line 1 out of the top of
        // the content box while every later line stayed where the advance put it.
        const float glyphYOffset =
            std::max(0.0f, TextLayout::HalfLeadingPx(lineH, met.LineAdvance));

        static thread_local FontAtlas::ShapeResult scratchResult;

        static const std::vector<UITextureRegistry::SlugTextureIndices> kEmptySlugPagesDiff;
        const auto& slugPages = (ctx.Textures && met.Font)
            ? ctx.Textures->RegisterSlugTextures(*met.Font)
            : kEmptySlugPagesDiff;

        const UI::GlyphRunTarget runTarget = UI::MakeGlyphRunTarget(ctx, met.Font, slugPages);

        int lineIdx = 0;
        size_t cursor = 0;
        while (cursor <= text.size()) {
            size_t eol = text.find('\n', cursor);
            if (eol == std::string::npos) eol = text.size();

            std::string_view lineView(text.data() + cursor, eol - cursor);
            uint32_t lineColor = DiffLineColor(lineView, style);

            met.Font->ShapeText(lineView, met.Px, scratchResult, lineColor, met.LetterSpacing);

            float lineY = originY + lineIdx * met.LineAdvance + glyphYOffset;
            UI::EmitGlyphRun(scratchResult.glyphs, originX, lineY, lm.ascender, runTarget);

            cursor = eol + 1;
            ++lineIdx;
        }
    }

private:
    bool m_IsLeftSide;

    uint32_t DiffLineColor(std::string_view line, const ResolvedStyle& style) const
    {
        float r = kDiffColorUnchangedR;
        float g = kDiffColorUnchangedG;
        float b = kDiffColorUnchangedB;

        if (line.size() >= 2) {
            if (line[0] == '-' && line[1] == ' ' && m_IsLeftSide) {
                r = kDiffColorDeletedR; g = kDiffColorDeletedG; b = kDiffColorDeletedB;
            } else if (line[0] == '+' && line[1] == ' ' && !m_IsLeftSide) {
                r = kDiffColorAddedR; g = kDiffColorAddedG; b = kDiffColorAddedB;
            }
        }

        uint8_t ri = static_cast<uint8_t>(r * 255.0f);
        uint8_t gi = static_cast<uint8_t>(g * 255.0f);
        uint8_t bi = static_cast<uint8_t>(b * 255.0f);
        uint8_t ai = static_cast<uint8_t>(style.Visual.Opacity * 255.0f);
        return UI::PackFromARGB((uint32_t(ai) << 24) | (uint32_t(ri) << 16) | (uint32_t(gi) << 8) | uint32_t(bi));
    }
};

DiffPanel::DiffPanel()
    : DockPanel("Diff")
{
    SetupUI();
}

DiffPanel::~DiffPanel() = default;

void DiffPanel::SetupUI()
{
    // Create main container - fills entire panel
    auto container = std::make_unique<UIElement>();
    container->AddClass("diff-panel-container");
    container->SetId("DiffPanelContainer");
    m_Container = container.get();

    // Create horizontal split container - fills container, 50/50 split
    auto splitContainer = std::make_unique<UIElement>();
    splitContainer->AddClass("diff-split-container");

    // Left panel (original version) - exactly 50% width
    auto leftPanel = std::make_unique<UIElement>();
    leftPanel->AddClass("diff-left-panel");
    m_LeftPanel = leftPanel.get();

    // Left label
    auto leftLabel = std::make_unique<Label>();
    leftLabel->SetId("DiffLeftLabel");
    leftLabel->AddClass("diff-panel-label");
    leftLabel->SetText("Original (VCS)");
    m_LeftLabel = leftLabel.get();
    leftPanel->AddChild(std::move(leftLabel));

    // Left scroll view - fills remaining space
    auto leftScrollView = std::make_unique<ScrollView>();
    leftScrollView->AddClass("diff-scroll-view");
    m_LeftScrollView = leftScrollView.get();

    leftPanel->AddChild(std::move(leftScrollView));

    // Right panel (current version) - exactly 50% width
    auto rightPanel = std::make_unique<UIElement>();
    rightPanel->AddClass("diff-right-panel");
    m_RightPanel = rightPanel.get();

    // Right label
    auto rightLabel = std::make_unique<Label>();
    rightLabel->SetId("DiffRightLabel");
    rightLabel->AddClass("diff-panel-label");
    rightLabel->SetText("Current (Working Directory)");
    m_RightLabel = rightLabel.get();
    rightPanel->AddChild(std::move(rightLabel));

    // Right scroll view - fills remaining space
    auto rightScrollView = std::make_unique<ScrollView>();
    rightScrollView->AddClass("diff-scroll-view");
    m_RightScrollView = rightScrollView.get();

    rightPanel->AddChild(std::move(rightScrollView));

    splitContainer->AddChild(std::move(leftPanel));
    splitContainer->AddChild(std::move(rightPanel));

    container->AddChild(std::move(splitContainer));
    AddChild(std::move(container));
    SetupTextContent();
    
    // Don't register scroll handlers here - defer until panel is actually shown/attached
    // RegisterScrollHandlers() will be called from OnPostLayout or when needed
}

void DiffPanel::SetupTextContent()
{
    if (!m_LeftScrollView || !m_RightScrollView)
        return;

    m_LeftScrollView->GetViewport()->RemoveAllChildren();
    m_RightScrollView->GetViewport()->RemoveAllChildren();

    auto leftTextArea = std::make_unique<DiffTextArea>(true);
    leftTextArea->SetId("DiffLeftTextArea");
    leftTextArea->AddClass("diff-textarea");
    m_LeftTextArea = leftTextArea.get();
    m_LeftScrollView->AddContent(std::move(leftTextArea));

    auto rightTextArea = std::make_unique<DiffTextArea>(false);
    rightTextArea->SetId("DiffRightTextArea");
    rightTextArea->AddClass("diff-textarea");
    m_RightTextArea = rightTextArea.get();
    m_RightScrollView->AddContent(std::move(rightTextArea));
}

void DiffPanel::OnPostLayout()
{
    // Safety check: ensure panel has an owner manager before registering handlers
    if (!GetOwnerManager()) {
        return;
    }
    
    // Register scroll handlers after first layout when UI is fully attached
    if (!m_ScrollHandlersRegistered && m_LeftScrollView && m_RightScrollView) {
        RegisterScrollHandlers();
        m_ScrollHandlersRegistered = true;
    }
}

void DiffPanel::RegisterScrollHandlers()
{
    // Safety checks
    if (!m_LeftScrollView || !m_RightScrollView) {
        return;
    }
    
    // Ensure scroll views are still valid and attached
    if (!m_LeftScrollView->GetViewport() || !m_RightScrollView->GetViewport()) {
        return;
    }
    
    // Synchronize scrolling between both panels
    m_LeftScrollView->RegisterEventHandler(kEventScroll, [this](UIEvent&) {
        if (m_IsScrolling || !m_LeftScrollView || !m_RightScrollView) {
            return;
        }
        // Direct synchronization - scroll events are safe to handle synchronously
        m_IsScrolling = true;
        SynchronizeScroll(m_LeftScrollView, m_RightScrollView);
        m_IsScrolling = false;
    });

    m_RightScrollView->RegisterEventHandler(kEventScroll, [this](UIEvent&) {
        if (m_IsScrolling || !m_LeftScrollView || !m_RightScrollView) {
            return;
        }
        // Direct synchronization - scroll events are safe to handle synchronously
        m_IsScrolling = true;
        SynchronizeScroll(m_RightScrollView, m_LeftScrollView);
        m_IsScrolling = false;
    });
}

bool DiffPanel::OpenDiff(const std::filesystem::path& filePath,
                         const std::string& originalContent,
                         const std::string& currentContent)
{
    if (filePath.empty())
    {
        Logger::Log::Error("DiffPanel: Cannot open diff - path is empty");
        return false;
    }

    m_CurrentFilePath = filePath;
    m_OriginalContent = originalContent;
    m_CurrentContent = currentContent;
    const bool isSceneDiff = filePath.extension() == ".scene";

    if (isSceneDiff)
    {
        UpdateSceneDisplay();
    }
    else
    {
        SetupTextContent();
        CalculateDiff(originalContent, currentContent);
        UpdateDisplay();
    }

    // Update title
    std::string fileName = filePath.filename().string();
    SetTitle("Diff: " + fileName);

    // Update labels
    if (m_LeftLabel) {
        m_LeftLabel->SetText("Original (VCS): " + fileName);
    }
    if (m_RightLabel) {
        m_RightLabel->SetText("Current (Working): " + fileName);
    }

    return true;
}

namespace
{
const char* SceneDiffClass(Editor::SceneDiffState state)
{
    switch (state)
    {
        case Editor::SceneDiffState::Added: return "diff-scene-added";
        case Editor::SceneDiffState::Removed: return "diff-scene-removed";
        case Editor::SceneDiffState::Modified: return "diff-scene-modified";
        case Editor::SceneDiffState::Unchanged: return "diff-scene-unchanged";
    }
    return "diff-scene-unchanged";
}

std::unique_ptr<UIElement> MakeSceneRow(const std::string& key,
                                        const std::string& value,
                                        Editor::SceneDiffState state,
                                        bool objectRow,
                                        bool entity,
                                        bool present)
{
    auto row = std::make_unique<UIElement>();
    row->AddClass("diff-scene-row");
    row->AddClass(objectRow ? "diff-scene-object-row" : "diff-scene-property-row");
    row->AddClass(SceneDiffClass(state));
    if (!present)
        row->AddClass("diff-scene-placeholder");

    auto keyLabel = std::make_unique<Label>();
    keyLabel->AddClass("diff-scene-key");
    if (objectRow)
    {
        keyLabel->AddClass(entity ? "diff-scene-entity-key" : "diff-scene-section-key");
        keyLabel->SetText(entity ? ("Entity  " + key) : key);
    }
    else
    {
        keyLabel->SetText(key);
    }
    row->AddChild(std::move(keyLabel));

    if (!objectRow)
    {
        auto valueLabel = std::make_unique<Label>();
        valueLabel->AddClass("diff-scene-value");
        valueLabel->SetText(value);
        row->AddChild(std::move(valueLabel));
    }
    return row;
}
} // namespace

void DiffPanel::UpdateSceneDisplay()
{
    if (!m_LeftScrollView || !m_RightScrollView)
        return;

    m_LeftTextArea = nullptr;
    m_RightTextArea = nullptr;
    m_LeftScrollView->GetViewport()->RemoveAllChildren();
    m_RightScrollView->GetViewport()->RemoveAllChildren();

    auto leftRoot = std::make_unique<UIElement>();
    auto rightRoot = std::make_unique<UIElement>();
    leftRoot->AddClass("diff-scene-list");
    rightRoot->AddClass("diff-scene-list");

    const auto sceneDiff = Editor::BuildSceneDiff(m_OriginalContent, m_CurrentContent);
    for (const auto& object : sceneDiff)
    {
        const bool hasLeft = object.State != Editor::SceneDiffState::Added;
        const bool hasRight = object.State != Editor::SceneDiffState::Removed;
        leftRoot->AddChild(MakeSceneRow(object.OriginalLabel, {}, object.State,
                                        true, object.IsEntity, hasLeft));
        rightRoot->AddChild(MakeSceneRow(object.CurrentLabel, {}, object.State,
                                         true, object.IsEntity, hasRight));

        for (const auto& property : object.Properties)
        {
            const bool propertyHasLeft = property.State != Editor::SceneDiffState::Added;
            const bool propertyHasRight = property.State != Editor::SceneDiffState::Removed;
            leftRoot->AddChild(MakeSceneRow(property.Key, property.OriginalValue, property.State,
                                            false, object.IsEntity, propertyHasLeft));
            rightRoot->AddChild(MakeSceneRow(property.Key, property.CurrentValue, property.State,
                                             false, object.IsEntity, propertyHasRight));
        }
    }

    m_LeftScrollView->AddContent(std::move(leftRoot));
    m_RightScrollView->AddContent(std::move(rightRoot));
    m_LeftScrollView->SetScrollX(0.0f);
    m_LeftScrollView->SetScrollY(0.0f);
    m_RightScrollView->SetScrollX(0.0f);
    m_RightScrollView->SetScrollY(0.0f);
}

void DiffPanel::CalculateDiff(const std::string& originalContent, const std::string& currentContent)
{
    m_DiffLines.clear();

    // Split into lines
    std::vector<std::string> originalLines;
    std::vector<std::string> currentLines;

    std::istringstream origStream(originalContent);
    std::string line;
    while (std::getline(origStream, line)) {
        originalLines.push_back(line);
    }

    std::istringstream currStream(currentContent);
    while (std::getline(currStream, line)) {
        currentLines.push_back(line);
    }

    // Simple line-based diff using longest common subsequence approach
    // This is a simplified version - for production, consider using a proper diff library
    size_t origIdx = 0;
    size_t currIdx = 0;
    int origLineNum = 1;
    int currLineNum = 1;

    while (origIdx < originalLines.size() || currIdx < currentLines.size()) {
        if (origIdx >= originalLines.size()) {
            // Only current lines remain - all added
            while (currIdx < currentLines.size()) {
                DiffLine diffLine;
                diffLine.Type = DiffLineType::Added;
                diffLine.Content = currentLines[currIdx];
                diffLine.OriginalLineNumber = -1;
                diffLine.CurrentLineNumber = currLineNum++;
                m_DiffLines.push_back(diffLine);
                currIdx++;
            }
            break;
        }

        if (currIdx >= currentLines.size()) {
            // Only original lines remain - all deleted
            while (origIdx < originalLines.size()) {
                DiffLine diffLine;
                diffLine.Type = DiffLineType::Deleted;
                diffLine.Content = originalLines[origIdx];
                diffLine.OriginalLineNumber = origLineNum++;
                diffLine.CurrentLineNumber = -1;
                m_DiffLines.push_back(diffLine);
                origIdx++;
            }
            break;
        }

        // Check if lines match
        if (originalLines[origIdx] == currentLines[currIdx]) {
            // Unchanged line
            DiffLine diffLine;
            diffLine.Type = DiffLineType::Unchanged;
            diffLine.Content = originalLines[origIdx];
            diffLine.OriginalLineNumber = origLineNum++;
            diffLine.CurrentLineNumber = currLineNum++;
            m_DiffLines.push_back(diffLine);
            origIdx++;
            currIdx++;
        } else {
            // Lines differ - check if it's a modification or insertion/deletion
            // Look ahead to see if we can find a match
            bool foundMatch = false;

            // Check if next original line matches current line (deletion)
            if (origIdx + 1 < originalLines.size() && 
                originalLines[origIdx + 1] == currentLines[currIdx]) {
                // Original line was deleted
                DiffLine diffLine;
                diffLine.Type = DiffLineType::Deleted;
                diffLine.Content = originalLines[origIdx];
                diffLine.OriginalLineNumber = origLineNum++;
                diffLine.CurrentLineNumber = -1;
                m_DiffLines.push_back(diffLine);
                origIdx++;
                foundMatch = true;
            }
            // Check if next current line matches original line (insertion)
            else if (currIdx + 1 < currentLines.size() && 
                     originalLines[origIdx] == currentLines[currIdx + 1]) {
                // Current line was added
                DiffLine diffLine;
                diffLine.Type = DiffLineType::Added;
                diffLine.Content = currentLines[currIdx];
                diffLine.OriginalLineNumber = -1;
                diffLine.CurrentLineNumber = currLineNum++;
                m_DiffLines.push_back(diffLine);
                currIdx++;
                foundMatch = true;
            }
            // Check if both next lines match (modification)
            else if (origIdx + 1 < originalLines.size() && 
                     currIdx + 1 < currentLines.size() &&
                     originalLines[origIdx + 1] == currentLines[currIdx + 1]) {
                // Both lines changed (modified)
                DiffLine origDiffLine;
                origDiffLine.Type = DiffLineType::Modified;
                origDiffLine.Content = originalLines[origIdx];
                origDiffLine.OriginalLineNumber = origLineNum++;
                origDiffLine.CurrentLineNumber = -1;
                m_DiffLines.push_back(origDiffLine);

                DiffLine currDiffLine;
                currDiffLine.Type = DiffLineType::Modified;
                currDiffLine.Content = currentLines[currIdx];
                currDiffLine.OriginalLineNumber = -1;
                currDiffLine.CurrentLineNumber = currLineNum++;
                m_DiffLines.push_back(currDiffLine);
                origIdx++;
                currIdx++;
                foundMatch = true;
            }

            if (!foundMatch) {
                // Treat as modification (both lines changed)
                DiffLine origDiffLine;
                origDiffLine.Type = DiffLineType::Modified;
                origDiffLine.Content = originalLines[origIdx];
                origDiffLine.OriginalLineNumber = origLineNum++;
                origDiffLine.CurrentLineNumber = -1;
                m_DiffLines.push_back(origDiffLine);

                DiffLine currDiffLine;
                currDiffLine.Type = DiffLineType::Modified;
                currDiffLine.Content = currentLines[currIdx];
                currDiffLine.OriginalLineNumber = -1;
                currDiffLine.CurrentLineNumber = currLineNum++;
                m_DiffLines.push_back(currDiffLine);
                origIdx++;
                currIdx++;
            }
        }
    }
}

void DiffPanel::UpdateDisplay()
{
    if (!m_LeftTextArea || !m_RightTextArea) {
        return;
    }

    // Build left side (original) text with diff markers
    std::ostringstream leftStream;
    std::ostringstream rightStream;

    for (const auto& diffLine : m_DiffLines) {
        std::string marker;
        switch (diffLine.Type) {
            case DiffLineType::Unchanged:
                marker = "  ";
                break;
            case DiffLineType::Added:
                // Added lines don't appear in original
                continue;
            case DiffLineType::Deleted:
                marker = "- ";
                break;
            case DiffLineType::Modified:
                marker = "- ";
                break;
        }

        if (diffLine.OriginalLineNumber >= 0) {
            leftStream << marker << diffLine.Content << "\n";
        }
    }

    // Build right side (current) text with diff markers
    for (const auto& diffLine : m_DiffLines) {
        std::string marker;
        switch (diffLine.Type) {
            case DiffLineType::Unchanged:
                marker = "  ";
                break;
            case DiffLineType::Added:
                marker = "+ ";
                break;
            case DiffLineType::Deleted:
                // Deleted lines don't appear in current
                continue;
            case DiffLineType::Modified:
                marker = "+ ";
                break;
        }

        if (diffLine.CurrentLineNumber >= 0) {
            rightStream << marker << diffLine.Content << "\n";
        }
    }

    m_LeftTextArea->SetValue(leftStream.str());
    m_RightTextArea->SetValue(rightStream.str());
}

void DiffPanel::SynchronizeScroll(ScrollView* source, ScrollView* target)
{
    if (!source || !target) {
        return;
    }

    // Check if scroll views are valid and have been laid out
    if (!source->GetViewport() || !target->GetViewport()) {
        return;
    }

    float scrollY = source->GetScrollY();
    target->SetScrollY(scrollY);
}

} // namespace GameEngine
