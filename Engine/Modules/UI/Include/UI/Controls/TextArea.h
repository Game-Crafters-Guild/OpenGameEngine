#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <memory>
#include "UI/CaretBlink.h"
#include "UI/Controls/BaseField.h"
#include "UI/ITextMeasurable.h"

namespace GameEngine {
namespace Rendering { namespace Text { class FontAtlas; } }

class TextArea : public BaseField, public ITextMeasurable {
public:
    TextArea();
    ~TextArea() override;

    bool OnChar(unsigned int codepoint) override;
    bool OnKey(int key, int mods, UI::IPlatformApi* platform) override;
    void OnPointerDown(float mouseX, float mouseY,
                       float x, float y, float W, float H,
                       const ResolvedStyle& style, Rendering::Text::FontAtlas* font) override;
    void OnPointerDrag(float mouseX, float mouseY,
                       float x, float y, float W, float H,
                       const ResolvedStyle& style, Rendering::Text::FontAtlas* font) override;

    void OnFocusChanged(bool focused) override;

    void OnPostLayout() override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

    // ITextMeasurable: intrinsic sizing from text content
    void GetTextMeasureInfo(ITextMeasurable::TextMeasureInfo& info,
                            const ResolvedStyle& style) const override
    {
        (void)style;
        info.HasText = !GetValue().empty();
        info.Text = GetValue();
        info.ExplicitFontSize = 0.0f;
    }

    bool AllowWrapForMeasure() const override { return true; }

    // Expose text content for rendering by UIManager
    const std::string& GetTextContent() const override { return GetValue(); }

    // TextArea handles its own text rendering with per-line caching and viewport culling.
    bool HandlesOwnTextRendering() const override { return true; }

    /// Submit-on-Enter, for a message box: Enter calls `submit` instead of inserting a line
    /// break, and Shift+Enter inserts the line break. Empty, the default, leaves Enter
    /// inserting one. `submit` may destroy the area. A read-only area ignores it.
    void SetOnSubmit(std::function<void()> submit) { m_OnSubmit = std::move(submit); }

    // Read-only mode: allows selection and copy but prevents all editing
    void SetReadOnly(bool v) { m_ReadOnly = v; }
    bool IsReadOnly() const { return m_ReadOnly; }

    // Selection API for programmatic control
    void SetSelection(int start, int end) { m_SelectionStart = start; m_SelectionEnd = end; m_CaretIndex = end; m_ExtraCursors.clear(); MarkDirty(VisualDirty); }
    int GetCaretIndex() const { return m_CaretIndex; }
    int GetSelectionStart() const { return m_SelectionStart; }
    int GetSelectionEnd() const { return m_SelectionEnd; }

    /// The distance between two lines' baselines in logical px, as the text is laid
    /// out now; 0 until the element has an owner manager and its font resolves.
    float GetResolvedLineAdvancePx() const;
    /// The lines the value occupies at the element's current width, wrapped lines
    /// included (a value ending in a newline counts the empty line after it); 1 for
    /// an empty value, and the newline count + 1 until the font resolves.
    size_t GetVisualLineCount() const;

    // VSCode-style secondary cursors. Each has its own caret, selection anchor
    // and preferred X. anchor == -1 means no selection for that cursor.
    struct Cursor {
        int caret = 0;
        int anchor = -1;
        float preferredX = -1.0f;
        bool HasSelection() const { return anchor >= 0 && anchor != caret; }
        int SelStart() const { return HasSelection() ? (caret < anchor ? caret : anchor) : caret; }
        int SelEnd()   const { return HasSelection() ? (caret > anchor ? caret : anchor) : caret; }
    };

protected:
    // Stamp the caret's "hold solid" deadline from the interaction that moved
    // it. Emission must only READ the deadline (CaretForceVisibleUntil) — one
    // re-derived while the caret is being drawn can never be overtaken by the
    // clock the shader compares it against.
    void BumpCaretForceVisible();
    float CaretForceVisibleUntil() const { return m_CaretForceVisible.Until(); }

    bool m_ReadOnly = false;

    // Primary caret/selection in UTF-8 byte indices. (mutable to allow precise
    // updates in const render paths.)  Secondary cursors are stored in
    // m_ExtraCursors below. All edits/movements are applied to every cursor.
    mutable int m_CaretIndex = 0;
    mutable int m_SelectionStart = -1;
    mutable int m_SelectionEnd = -1;
    // Preferred X (pixels from line start) for vertical navigation (Up/Down)
    mutable float m_NavPreferredX = -1.0f;
    mutable bool m_SwallowNextNewlineChar = false;

    mutable std::vector<Cursor> m_ExtraCursors;
    // During a drag gesture, -1 means the primary cursor is the one being
    // extended. >=0 is an index into m_ExtraCursors selected by the most
    // recent mousedown.
    mutable int m_DragCursorIndex = -1;

    // Convert primary state to a Cursor / write a Cursor back into primary state.
    Cursor PrimaryAsCursor() const {
        Cursor c;
        c.caret = m_CaretIndex;
        c.anchor = (m_SelectionStart >= 0 && m_SelectionEnd >= 0 && m_SelectionStart != m_SelectionEnd)
                       ? (m_SelectionStart == m_CaretIndex ? m_SelectionEnd : m_SelectionStart)
                       : -1;
        c.preferredX = m_NavPreferredX;
        return c;
    }
    void WritePrimaryFromCursor(const Cursor& c) const {
        m_CaretIndex = c.caret;
        if (c.HasSelection()) {
            m_SelectionStart = c.SelStart();
            m_SelectionEnd = c.SelEnd();
        } else {
            m_SelectionStart = -1;
            m_SelectionEnd = -1;
        }
        m_NavPreferredX = c.preferredX;
    }

    // Multi-cursor helpers.
    void ClearExtraCursors() const { m_ExtraCursors.clear(); }
    /// Add an extra cursor. If one already exists at (caret,anchor) it is not duplicated.
    void AddExtraCursor(int caret, int anchor = -1) const;
    /// After edits/movements, merge overlapping cursors (primary + extras) so we
    /// never end up with two cursors at the same position or overlapping ranges.
    void MergeOverlappingCursors() const;
    /// Return all selection ranges (primary + extras) sorted high-to-low by start
    /// so callers can mutate the buffer from the tail without invalidating earlier
    /// indices. Each pair is {start, end}. Empty selections are returned as
    /// {caret, caret} (used by callers that want to insert at every cursor).
    std::vector<std::pair<int,int>> CollectAllSelectionRanges(bool includeEmpty) const;

    /// Apply a per-cursor edit to \a s. Each cursor's selection is replaced by
    /// insertFn(cursor); a no-selection cursor gets insertFn inserted at its caret.
    /// If \a expandEmpty is provided it's called first on each empty-selection
    /// cursor to grow its selection (e.g. Backspace = one char backward).
    /// Cursors are processed high-to-low so earlier indices remain valid, then
    /// overlapping cursors are merged and written back to primary + extras.
    void ApplyMultiCursorEdit(
        std::string& s,
        const std::function<std::string(const Cursor&)>& insertFn,
        const std::function<void(const std::string&, Cursor&)>& expandEmpty = nullptr) const;

    // Auto-scroll gating:
    // Only auto-scroll to keep caret visible when the caret/selection changes.
    // This prevents "snap back" when the user scrolls via the scrollbar thumb.
    mutable int m_LastAutoScrollCaret = -1;
    mutable int m_LastAutoScrollSelectionStart = -1;
    mutable int m_LastAutoScrollSelectionEnd = -1;

    struct TextAreaMetrics {
        Rendering::Text::FontAtlas* Font = nullptr;
        float Px = 0.0f;
        float LineAdvance = 0.0f;
        // Resolved CSS line box in px; 0 = "normal" (the font's metric height,
        // in which case LineAdvance carries the resolved value).
        float LineBox = 0.0f;
        // CSS letter-spacing, logical px like Px — wrap, caret maps and glyph
        // emission must all use it or they disagree about line geometry.
        float LetterSpacing = 0.0f;
        float WrapWidth = 0.0f;
        float OriginX = 0.0f;
        float OriginY = 0.0f;
    };
    bool ComputeMetrics(const ResolvedStyle& style, float elW, TextAreaMetrics& out) const;
    bool ComputeMetrics(TextAreaMetrics& out) const;

    /// Extends or sets selection when shift-clicking; \a click is the hit-tested caret index.
    void ApplyShiftClickExtend(int oldCaret, int click);

    /// If inside a ScrollView, nudge the scroll so the primary caret's line is visible.
    void EnsureCaretVisibleInScroll();

    // Emits text glyphs for visible lines. Virtual so ScriptTextArea can add syntax coloring.
    virtual void EmitTextGlyphs(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                                float originX, float originY, const TextAreaMetrics& met);

    struct TextRenderCache;
    mutable std::unique_ptr<TextRenderCache> m_TextRenderCache;
    TextRenderCache& EnsureTextRenderCache() const;

private:
    UI::CaretForceVisibleDeadline m_CaretForceVisible;
    std::function<void()> m_OnSubmit;

    int HitTestCaret(float mouseX, float mouseY, float x, float y, float w,
                     const ResolvedStyle& style, Rendering::Text::FontAtlas* font) const;
};

} // namespace GameEngine
