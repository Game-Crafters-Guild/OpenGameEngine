#pragma once

#include "Panels/ScriptSyntax.h"
#include "UI/Controls/TextArea.h"
#include "UI/ResolvedStyle.h"
#include "UI/SyntaxHighlightSettings.h"
#include <chrono>
#include <functional>
#include <vector>
#include <string>
#include <set>
#include <memory>
#include <cstdint>

namespace GameEngine {

class UIElement;
class Button;

/**
 * @brief TextArea with syntax highlighting support.
 *
 * Extends TextArea to add syntax highlighting (language chosen via
 * SetLanguage) with performance optimizations including cached tokenization.
 */
class ScriptTextArea : public TextArea {
public:
    ScriptTextArea();
    ~ScriptTextArea() override = default;

    bool OnChar(unsigned int codepoint) override;
    void SetValue(const std::string& value) override;

    // Highlighting language; the panel sets it from the opened file's path.
    void SetLanguage(Editor::ScriptLanguage language);

    // Line numbers
    void SetShowLineNumbers(bool show);
    bool GetShowLineNumbers() const { return m_ShowLineNumbers; }
    
    // Hover integration: hovering the line-number gutter should highlight the corresponding code line.
    void UpdateHoverFromMouse(float mouseX, float mouseY);
    void ClearHoverLine();
    void RefreshHoverFromLastMouse();

    // Search highlighting
    void SetSearchHighlights(const std::vector<size_t>& matchPositions, size_t matchLength, int currentMatchIndex = -1);
    void ClearSearchHighlights();
    
    // Variable highlighting (similar to search highlighting)
    void SetVariableHighlight(size_t position, size_t length);
    void ClearVariableHighlight();
    void SetDiagnosticHighlight(size_t position, size_t length);
    void ClearDiagnosticHighlight();

    // Reposition existing fold marker elements to match the current ScrollView scroll offsets.
    // This is invoked from the ScriptEditorPanel's ScrollView scroll handler so markers stay
    // perfectly synchronized with the text during scrolling (no 1-frame lag).
    void SyncFoldMarkerPositionsToScroll();

    // Returns the left gutter width in pixels (fold marker + line number columns).
    float GetGutterWidth() const;

    // Returns the actual rendered line advance in pixels: the resolved CSS line
    // box, or the font's `normal` line box when line-height is unset.
    float GetLineAdvancePx() const;

    // Converts a 0-based logical line number to its 0-based visual line number,
    // accounting for folded regions (folded lines reduce the visual count).
    size_t LogicalToVisualLine(size_t logicalLine) const { return GetVisualLineNumber(logicalLine); }

    // Expands any fold regions that contain bytePos so the line is visible.
    // Returns true if any region was expanded (visual line map is invalidated).
    bool EnsureBytePositionVisible(size_t bytePos);

    // Returns the 0-based visual line index and segment byte-start for bytePos,
    // correctly handling word-wrapped segments within a logical line.
    // outSegStart receives the byte offset of the start of that visual segment.
    size_t GetVisualLineForBytePos(size_t bytePos, size_t* outSegStart = nullptr) const;

    // Pixel rectangle of a byte position in element-local coordinates (relative to
    // this element's top-left, matching the rendered glyph layout via the actual font
    // caret map and line advance — DPI-correct). Not adjusted for an enclosing
    // ScrollView; callers map through GetLayoutX/GetLayoutY. outX/outY is the top-left,
    // outHeight is the line advance. Returns false if metrics are unavailable.
    bool GetBytePositionLocalRect(size_t bytePos, float& outX, float& outY, float& outHeight) const;

    // Called before TextArea's default OnEvent so the host can intercept keys (e.g. completion).
    // Return true to suppress the default handling for that key.
    using KeyFilter = std::function<bool(UIEvent&)>;
    void SetKeyFilter(KeyFilter filter) { m_KeyFilter = std::move(filter); }

    void OnEvent(UIEvent& e) override;

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

    void EmitTextGlyphs(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                        float originX, float originY, const TextAreaMetrics& met) override;

protected:
    // Override to position fold markers
    void OnPostLayout() override;
    
    // Override to handle fold marker clicks
    void OnPointerDown(float mouseX, float mouseY,
                       float x, float y, float W, float H,
                       const ResolvedStyle& style, Rendering::Text::FontAtlas* font) override;
    void OnPointerDrag(float mouseX, float mouseY,
                       float x, float y, float W, float H,
                       const ResolvedStyle& style, Rendering::Text::FontAtlas* font) override;

private:
    int HitTestVisibleCaret(float mouseX, float mouseY,
                            float x, float y, float W,
                            const ResolvedStyle& style) const;

    bool GetLogicalLineByteRangeFromMouseY(float mouseY, float x, float y, float W,
                                           const ResolvedStyle& style,
                                           size_t& outLineStart,
                                           size_t& outLineEndExclusive) const;

    bool IsPointerOverLineGlyphInk(float mouseX, float mouseY, float x, float y, float W,
                                   const ResolvedStyle& style) const;

    // Line/visual mapping caches for performance (avoid O(text) scans each frame).
    void InvalidateLineCaches() const;
    void EnsureLineStartCache(const std::string& text) const;
    void EnsureVisualLineMapCache(const std::string& text) const;
    size_t GetLineIndexForBytePos(size_t bytePos) const;

    KeyFilter m_KeyFilter;

    // Line numbers
    bool m_ShowLineNumbers = false;
    mutable int m_HoveredVisualLine = -1; // visual line index (after folding), or -1 if none
    mutable bool m_HasLastMouse = false;
    mutable float m_LastMouseX = 0.0f;
    mutable float m_LastMouseY = 0.0f;

    // Tokenization (language rules live in Panels/ScriptSyntax)
    void InvalidateTokenCache() { m_TokenCacheValid = false; }

    Editor::ScriptLanguage m_Language = Editor::ScriptLanguage::CSharp;

    // Cached tokenization (only re-tokenize when text changes)
    mutable std::vector<Editor::SyntaxToken> m_TokenCache;
    mutable std::string m_LastTokenizedText;
    mutable bool m_TokenCacheValid = false;

    // Cached line starts (byte indices) and visual-to-actual mapping for folded/wrapped views.
    // These are rebuilt only when text, fold state, wrap width, font, or word-break change.
    mutable bool m_LineStartCacheValid = false;
    mutable std::vector<size_t> m_LineStarts;              // size == totalLines, each is byte index of line start
    mutable bool m_VisualLineMapValid = false;

    // One visible visual line (post-fold, post-wrap). Byte range is the portion of the
    // source text drawn on that visual row; ByteEndExclusive excludes any trailing '\n'.
    struct VisualLineEntry {
        uint32_t ActualLine;
        uint32_t ByteStart;
        uint32_t ByteEndExclusive;
    };
    mutable std::vector<VisualLineEntry> m_VisualLines;
    mutable std::vector<uint32_t> m_ActualToVisualLine;    // actualLineIdx -> first visualIdx of that line
    mutable int m_LineNumberWidthDigits = 1;

    // Cache key for the visual-line map so wrap-width / font / size changes invalidate it.
    mutable uintptr_t m_LineMapFontId = 0;
    mutable float m_LineMapPixelSize = 0.0f;
    mutable float m_LineMapWrapWidth = -1.0f;
    mutable float m_LineMapLineBox = 0.0f;
    mutable int m_LineMapWordBreak = 0;
    
    // Search highlight state
    std::vector<size_t> m_SearchHighlightPositions;
    size_t m_SearchHighlightLength = 0;
    int m_CurrentSearchMatchIndex = -1;
    
    // Variable highlight state
    size_t m_VariableHighlightPosition = SIZE_MAX;
    size_t m_VariableHighlightLength = 0;

    // Diagnostic highlight state
    size_t m_DiagnosticHighlightPosition = SIZE_MAX;
    size_t m_DiagnosticHighlightLength = 0;
    
    // Code folding
    struct FoldRegion {
        size_t StartLine;      // Line where fold starts (line with opening brace)
        size_t EndLine;         // Line where fold ends (line with closing brace)
        size_t StartPos;        // Byte position of opening brace
        size_t EndPos;          // Byte position of closing brace
        int IndentLevel;        // Indentation level for nested folds
    };
    
    // Detect foldable regions (blocks with braces)
    void DetectFoldRegions(const std::string& text) const;
    
    // Check if a line is folded
    bool IsLineFolded(size_t lineIndex) const;
    
    // Toggle fold at a specific line
    void ToggleFold(size_t lineIndex);
    
    // Get the fold region for a line (if it starts a foldable region)
    const FoldRegion* GetFoldRegionForLine(size_t lineIndex) const;
    
    // Calculate visual line number accounting for folded regions
    // Returns the line number as it appears visually (with folded lines hidden)
    size_t GetVisualLineNumber(size_t actualLineNumber) const;
    
    // Calculate nesting level of a fold region (how many other regions contain it)
    size_t GetFoldRegionNestingLevel(size_t regionIndex) const;
    
    // Check if a fold region is inside a folded parent region
    bool IsRegionInsideFoldedParent(size_t regionIndex) const;
    
    // Find fold regions that contain a byte position
    std::vector<size_t> FindFoldRegionsContainingPosition(size_t bytePosition) const;
    
    // Expand a fold region and all its parent regions
    void ExpandFoldRegion(size_t regionIndex);
    
    // Update fold marker UI elements
    void UpdateFoldMarkers();

    // Push the current visible-content height to the element's Yoga style as
    // an explicit Style::Height override so the parent ScrollView's content
    // size (and scrollbar) shrinks/grows immediately when fold state changes.
    // Without this, Yoga's text measure callback only counts raw lines and the
    // scroll layout would stay stuck at the unfolded extent.
    void RefreshContentHeightOverride();
    float m_LastContentHeightOverride = -1.0f;
    
    // Foldable regions and state
    mutable std::vector<FoldRegion> m_FoldRegions;
    mutable std::set<size_t> m_FoldedLines;  // Set of start lines that are folded
    mutable bool m_FoldRegionsValid = false;
    
    // Fold marker UI elements (one per foldable region)
    // Note: These are owned by the parent (via AddChild), so we store raw pointers
    mutable std::vector<UIElement*> m_FoldMarkerElements;

    // Deferred fold marker update flag
    // Set to true when fold state changes; actual update happens in OnPostLayout
    // to avoid destroying UI elements while their event handlers are executing
    mutable bool m_NeedsFoldMarkerUpdate = false;

    // Guard to prevent recursive calls to UpdateFoldMarkers
    // (RemoveChild can trigger layout updates which call OnPostLayout)
    mutable bool m_UpdatingFoldMarkers = false;

    // Double/triple-click word and line selection (text column only; gutter clicks reset chain)
    std::chrono::steady_clock::time_point m_LastPointerClickTime{};
    float m_LastPointerClickX = 0.0f;
    float m_LastPointerClickY = 0.0f;
    int m_ConsecutiveClickCount = 0;
    float m_ClickDragAnchorX = 0.0f;
    float m_ClickDragAnchorY = 0.0f;
    bool m_SuppressSmallDragSelection = false;
};

} // namespace GameEngine
