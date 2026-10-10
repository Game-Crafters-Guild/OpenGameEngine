#include "UI/Controls/TextArea.h"
#include "UI/TextSegmentation.h"
#include "UI/Utf8Helpers.h"
#include "UI/UIStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "UI/ResolvedStyle.h"
#include "UI/Controls/ScrollView.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Platform/SystemMetrics.h"

#include "UI/Parsers/CSSParser.h"
#include "UI/UIPrimitive.h"
#include "UI/GlyphRunEmitter.h"
#include "UI/TextSelectionFill.h"
#include "UI/UITextureRegistry.h"
#include "Types/ColorUtils.h"

#include "Rendering/Text/TextLayout.h"
#include "Rendering/Text/FontAtlas.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace GameEngine {

using namespace Rendering::Text;
using namespace Rendering::Geometry;

static constexpr int kDefaultPageLines = 10;

// =============================================================================
// Visual line computation via ShapeMultiline
// =============================================================================

using LineBreakInfo = TextLayout::LineBreakInfo;

static TextLayout::WordBreak MapWordBreak(const ResolvedStyle& style)
{
    switch (ResolveTextBreakPolicy(style.Visual.WordBreak, style.Visual.OverflowWrap)) {
    case WordBreak::BreakAll:  return TextLayout::WordBreak::BreakAll;
    case WordBreak::KeepAll:   return TextLayout::WordBreak::KeepAll;
    case WordBreak::BreakWord: return TextLayout::WordBreak::BreakWord;
    default:                   return TextLayout::WordBreak::Normal;
    }
}

static void GetVisualLines(const std::string& text,
                           FontAtlas* font,
                           float pixelSize,
                           float wrapWidth,
                           float lineBoxPx,
                           float letterSpacingPx,
                           TextLayout::WordBreak wordBreak,
                           std::vector<LineBreakInfo>& outLines)
{
    outLines.clear();

    if (text.empty()) {
        outLines.push_back({0, 0});
        return;
    }

    StyledRun run{};
    run.Font = font;
    run.PixelSize = pixelSize;
    run.LetterSpacingPx = letterSpacingPx;
    run.Text = text;

    auto result = TextLayout::ShapeMultiline(
        std::span<const StyledRun>(&run, 1), wrapWidth, lineBoxPx, wordBreak);

    outLines = std::move(result.LineBreaks);

    if (outLines.empty())
        outLines.push_back({0, text.size()});

    if (!text.empty() && text.back() == '\n')
        outLines.push_back({text.size(), text.size()});
}

// =============================================================================
// ScrollView parent lookup
// =============================================================================

static ScrollView* FindParentScrollView(UIElement* element)
{
    UIElement* p = element->GetParent();
    while (p) {
        if (auto* sv = dynamic_cast<ScrollView*>(p))
            return sv;
        p = p->GetParent();
    }
    return nullptr;
}

static bool IsInSubtreeOf(const UIElement* element, const UIElement* root)
{
    for (const UIElement* p = element; p; p = p->GetParent()) {
        if (p == root)
            return true;
    }
    return false;
}

// The scroll view whose offset moves this element: the nearest one holding it
// in its scroll content. A ScrollView's other children (its scrollbars, or an
// element appended beside the scroll row instead of through AddContent) are
// neither moved nor clipped by that view, so the search continues outward.
static const ScrollView* FindScrollingAncestor(const UIElement* element)
{
    for (const UIElement* p = element->GetParent(); p; p = p->GetParent()) {
        const auto* sv = dynamic_cast<const ScrollView*>(p);
        if (sv && IsInSubtreeOf(element, sv->GetViewport()))
            return sv;
    }
    return nullptr;
}

// =============================================================================
// Line break + per-line shaping cache
// =============================================================================

static constexpr uint64_t kFnv1aPrime = 0x100000001B3ull;

static uint64_t HashMix(uint64_t h, uint64_t v)
{
    h ^= v;
    h *= kFnv1aPrime;
    return h;
}

static uint64_t LineBreakCacheKey(const std::string& text, FontAtlas* font,
                                  float pixelSize, float wrapWidth,
                                  float lineBoxPx, float letterSpacingPx,
                                  TextLayout::WordBreak wb)
{
    uint64_t h = std::hash<std::string_view>{}(std::string_view(text));
    h = HashMix(h, reinterpret_cast<uintptr_t>(font));
    uint32_t bits;
    std::memcpy(&bits, &pixelSize, sizeof(float));
    h = HashMix(h, static_cast<uint64_t>(bits));
    std::memcpy(&bits, &wrapWidth, sizeof(float));
    h = HashMix(h, static_cast<uint64_t>(bits));
    std::memcpy(&bits, &lineBoxPx, sizeof(float));
    h = HashMix(h, static_cast<uint64_t>(bits));
    std::memcpy(&bits, &letterSpacingPx, sizeof(float));
    h = HashMix(h, static_cast<uint64_t>(bits));
    h = HashMix(h, static_cast<uint64_t>(wb));
    return h;
}

struct TextArea::TextRenderCache {
    uint64_t BreakKey = 0;
    std::vector<LineBreakInfo> Breaks;

    struct LineShape {
        uint64_t TextHash = 0;
        uintptr_t FontId = 0;
        float PixelSize = 0.0f;
        float LetterSpacing = 0.0f;
        uint32_t Color = 0;  // cache invalidates if textColor changes (hover/focus/selection)
        std::vector<FontAtlas::GlyphPlacement> Glyphs;
    };
    std::vector<LineShape> Shapes;

    void EnsureLineBreaks(const std::string& text, FontAtlas* font,
                          float pixelSize, float wrapWidth,
                          float lineBoxPx, float letterSpacingPx,
                          TextLayout::WordBreak wb)
    {
        uint64_t key =
            LineBreakCacheKey(text, font, pixelSize, wrapWidth, lineBoxPx, letterSpacingPx, wb);
        if (key == BreakKey && !Breaks.empty())
            return;

        BreakKey = key;
        Breaks.clear();
        Shapes.clear();

        if (text.empty()) {
            Breaks.push_back({0, 0});
            return;
        }

        if (wrapWidth <= 0.0f) {
            size_t lineStart = 0;
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\n') {
                    Breaks.push_back({lineStart, i});
                    lineStart = i + 1;
                }
            }
            Breaks.push_back({lineStart, text.size()});
            if (text.back() == '\n')
                Breaks.push_back({text.size(), text.size()});
            return;
        }

        GetVisualLines(text, font, pixelSize, wrapWidth, lineBoxPx, letterSpacingPx, wb, Breaks);
    }
};

TextArea::TextRenderCache& TextArea::EnsureTextRenderCache() const
{
    if (!m_TextRenderCache)
        m_TextRenderCache = std::make_unique<TextRenderCache>();
    return *m_TextRenderCache;
}

// =============================================================================
// Constructor / Destructor
// =============================================================================

TextArea::TextArea()
{
    SetFocusable(true);
    m_TextMeasurable = this;
}

TextArea::~TextArea() = default;

// =============================================================================
// Multi-cursor helpers
// =============================================================================

void TextArea::AddExtraCursor(int caret, int anchor) const
{
    // Don't duplicate if a cursor already exists here.
    if (caret == m_CaretIndex && anchor < 0) {
        // Primary already sits here without selection.
        return;
    }
    for (const auto& c : m_ExtraCursors) {
        if (c.caret == caret && c.anchor == anchor)
            return;
    }
    Cursor c;
    c.caret = caret;
    c.anchor = anchor;
    m_ExtraCursors.push_back(c);
}

void TextArea::MergeOverlappingCursors() const
{
    if (m_ExtraCursors.empty())
        return;

    // Collect all cursors as a single vector (primary first), sorted by caret.
    std::vector<Cursor> all;
    all.reserve(m_ExtraCursors.size() + 1);
    all.push_back(PrimaryAsCursor());
    for (const auto& c : m_ExtraCursors) all.push_back(c);

    std::sort(all.begin(), all.end(), [](const Cursor& a, const Cursor& b) {
        return a.caret < b.caret;
    });

    // Merge cursors whose selection ranges overlap (or that share a caret).
    std::vector<Cursor> merged;
    merged.reserve(all.size());
    for (const auto& c : all) {
        if (!merged.empty()) {
            Cursor& last = merged.back();
            int lastStart = last.SelStart();
            int lastEnd   = last.SelEnd();
            int curStart  = c.SelStart();
            int curEnd    = c.SelEnd();
            if (curStart <= lastEnd) {
                // Overlap: extend last's selection.
                int newStart = std::min(lastStart, curStart);
                int newEnd   = std::max(lastEnd,   curEnd);
                // Preserve direction from whichever cursor's caret is at the trailing edge.
                int newCaret  = (c.caret == curEnd || last.caret == lastEnd) ? newEnd : newStart;
                int newAnchor = (newCaret == newEnd) ? newStart : newEnd;
                if (newCaret == newAnchor) newAnchor = -1;
                last.caret    = newCaret;
                last.anchor   = newAnchor;
                last.preferredX = -1.0f;
                continue;
            }
        }
        merged.push_back(c);
    }

    // First entry becomes the primary; rest become extras.
    WritePrimaryFromCursor(merged.front());
    m_ExtraCursors.assign(merged.begin() + 1, merged.end());
}

std::vector<std::pair<int,int>> TextArea::CollectAllSelectionRanges(bool includeEmpty) const
{
    std::vector<std::pair<int,int>> ranges;
    auto push = [&](const Cursor& c) {
        if (c.HasSelection()) {
            ranges.emplace_back(c.SelStart(), c.SelEnd());
        } else if (includeEmpty) {
            ranges.emplace_back(c.caret, c.caret);
        }
    };
    push(PrimaryAsCursor());
    for (const auto& c : m_ExtraCursors) push(c);

    // Sort descending by start so callers can mutate from the tail forward.
    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) {
        return a.first > b.first;
    });
    return ranges;
}

void TextArea::ApplyMultiCursorEdit(
    std::string& s,
    const std::function<std::string(const Cursor&)>& insertFn,
    const std::function<void(const std::string&, Cursor&)>& expandEmpty) const
{
    std::vector<Cursor> all;
    all.reserve(m_ExtraCursors.size() + 1);
    all.push_back(PrimaryAsCursor());
    for (const auto& c : m_ExtraCursors) all.push_back(c);

    if (expandEmpty) {
        for (auto& c : all) {
            if (!c.HasSelection())
                expandEmpty(s, c);
        }
    }

    std::sort(all.begin(), all.end(), [](const Cursor& a, const Cursor& b) {
        return a.SelStart() > b.SelStart();
    });

    for (auto& c : all) {
        int start = c.SelStart();
        int end   = c.SelEnd();
        start = std::clamp(start, 0, (int)s.size());
        end   = std::clamp(end,   0, (int)s.size());
        if (end > start) s.erase((size_t)start, (size_t)(end - start));

        std::string ins = insertFn(c);
        if (!ins.empty()) s.insert((size_t)start, ins);

        c.caret = start + (int)ins.size();
        c.anchor = -1;
        c.preferredX = -1.0f;
    }

    std::sort(all.begin(), all.end(), [](const Cursor& a, const Cursor& b) {
        return a.caret < b.caret;
    });
    all.erase(std::unique(all.begin(), all.end(), [](const Cursor& a, const Cursor& b) {
        return a.caret == b.caret && a.anchor == b.anchor;
    }), all.end());

    WritePrimaryFromCursor(all.front());
    m_ExtraCursors.assign(all.begin() + 1, all.end());
}

// =============================================================================
// Shared metric computation
// =============================================================================

bool TextArea::ComputeMetrics(const ResolvedStyle& style, float elW, TextAreaMetrics& out) const
{
    UIManager* manager = GetOwnerManager();
    if (!manager) return false;

    FontAtlas* font = manager->ResolveFontForStyle(style);
    if (!font) return false;

    const float fsize = std::max(1.0f, style.Visual.FontSize);
    out.Font = font;
    out.Px = fsize;
    out.LetterSpacing = style.Visual.LetterSpacing;

    auto lm = font->GetLineMetrics(out.Px);
    const float lineH = std::max(1.0f, lm.height);
    // TextArea works in logical units, so the line box resolves with scale 1.
    out.LineBox = TextLayout::ResolveLineBoxPx(style.Visual.LineHeight, fsize, 1.0f);
    out.LineAdvance = (out.LineBox > 0.0f) ? out.LineBox : lineH;

    const float padL = style.Layout.Padding.Left;
    const float padR = style.Layout.Padding.Right;
    const float borderL = style.Layout.BorderWidth.Left;
    const float borderR = style.Layout.BorderWidth.Right;

    float contentW = elW - padL - padR - borderL - borderR;

    if (style.Visual.WhiteSpace == WhiteSpace::NoWrap ||
        style.Visual.WhiteSpace == WhiteSpace::Pre)
    {
        out.WrapWidth = 0.0f;
    }
    else
    {
        out.WrapWidth = (contentW > 0.0f) ? contentW : 0.0f;
    }

    return true;
}

bool TextArea::ComputeMetrics(TextAreaMetrics& out) const
{
    return ComputeMetrics(GetResolvedStyle(), GetLayoutWidth(), out);
}

float TextArea::GetResolvedLineAdvancePx() const
{
    TextAreaMetrics m{};
    return ComputeMetrics(m) ? m.LineAdvance : 0.0f;
}

size_t TextArea::GetVisualLineCount() const
{
    const std::string& text = GetValue();
    TextAreaMetrics m{};
    if (!ComputeMetrics(m))
        return static_cast<size_t>(std::count(text.begin(), text.end(), '\n')) + 1;

    auto& tc = EnsureTextRenderCache();
    tc.EnsureLineBreaks(text, m.Font, m.Px, m.WrapWidth, m.LineBox, m.LetterSpacing,
                        MapWordBreak(GetResolvedStyle()));
    return std::max<size_t>(1, tc.Breaks.size());
}

// =============================================================================
// Pointer hit-test (shared by OnPointerDown / OnPointerDrag)
// =============================================================================

int TextArea::HitTestCaret(float mouseX, float mouseY, float x, float y, float w,
                           const ResolvedStyle& style, Rendering::Text::FontAtlas* /*font*/) const {
    TextAreaMetrics m{};
    if (!ComputeMetrics(style, w, m))
        return 0;

    const float padL = style.Layout.Padding.Left;
    const float padT = style.Layout.Padding.Top;
    const float borderL = style.Layout.BorderWidth.Left;
    const float borderT = style.Layout.BorderWidth.Top;

    float localX = std::max(0.0f, mouseX - (x + padL + borderL));
    float localY = std::max(0.0f, mouseY - (y + padT + borderT));

    const std::string& text = GetValue();

    auto& tc = EnsureTextRenderCache();
    tc.EnsureLineBreaks(text, m.Font, m.Px, m.WrapWidth, m.LineBox, m.LetterSpacing,
                        MapWordBreak(style));
    const auto& vLines = tc.Breaks;

    int lineIdx = std::clamp((int)std::floor(localY / m.LineAdvance), 0,
                              std::max(0, (int)vLines.size() - 1));

    const auto& vl = vLines[lineIdx];
    std::string lineText(text.data() + vl.ByteStart, vl.ByteEnd - vl.ByteStart);
    std::vector<float> xPositions;
    m.Font->BuildCaretMapUtf8(lineText, m.Px, xPositions, m.LetterSpacing);

    // Snap to the nearest caret position, so clicking the left half of a glyph
    // puts the caret before it and the right half after it. Taking the first
    // position at or past the click instead biased every click one character
    // to the right. TextInput::OnPointerDown resolves single-line clicks the
    // same way.
    if (xPositions.empty())
        return (int)vl.ByteStart;

    int best = (int)vl.ByteStart;
    float bestDistance = std::fabs(localX - xPositions[0]);
    for (size_t i = 1; i < xPositions.size(); ++i) {
        const float distance = std::fabs(localX - xPositions[i]);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = (int)(vl.ByteStart + i);
        }
    }

    return best;
}

void TextArea::ApplyShiftClickExtend(int oldCaret, int click)
{
    const std::string& str = GetValue();
    const int len = static_cast<int>(str.size());
    const int selStart = m_SelectionStart;
    const int selEnd = m_SelectionEnd;
    const bool hasSelection = selStart >= 0 && selEnd >= 0 && selStart != selEnd;
    int anchor = oldCaret;
    if (hasSelection)
    {
        const int a = std::min(selStart, selEnd);
        const int b = std::max(selStart, selEnd);
        if (click < a)
            anchor = b;
        else if (click > b)
            anchor = a;
        else
        {
            const int oldC = std::clamp(oldCaret, 0, len);
            anchor = (oldC == a) ? b : a;
        }
    }
    m_SelectionStart = std::min(anchor, click);
    m_SelectionEnd = std::max(anchor, click);
    m_CaretIndex = click;
}

// =============================================================================
// Pointer handlers
// =============================================================================

// Called from the interactions that move the caret (typing, keyboard
// navigation, pointer down/drag, focus gain) so it is never in its hidden half
// at the moment the user looks for where it went. The OS setting is read per
// bump rather than cached, for the same reason the render path reads the rate
// per frame: a caret setting changed while the editor runs takes effect
// without a restart.
void TextArea::BumpCaretForceVisible() {
    float now = 0.0f;
    if (UIManager* ui = GetOwnerManager())
        now = ui->GetTimeSeconds();
    m_CaretForceVisible.Bump(now, Platform::GetCaretBlinkHalfPeriod());
}

void TextArea::OnPointerDown(float mouseX, float mouseY,
                             float x, float y, float W, float H,
                             const ResolvedStyle& style, FontAtlas* font) {
    (void)H;
    BumpCaretForceVisible();
    if (!font) return;

    const int oldCaret = m_CaretIndex;
    const int click = HitTestCaret(mouseX, mouseY, x, y, W, style, font);

    int mods = 0;
    if (UIManager* owner = GetOwnerManager())
        mods = owner->GetModifierKeys();
    const bool shift = (mods & Input::kModShift) != 0;
    const bool primaryMod = Input::IsPrimaryShortcutModifier(mods);
    const bool altMod = (mods & Input::kModAlt) != 0;
    // VSCode adds a cursor on Alt+Click (all platforms) or on Ctrl/Cmd+Click.
    const bool addCursor = (primaryMod || altMod) && !shift;

    m_DragCursorIndex = -1;
    if (addCursor) {
        // Add extra cursor at click position with empty selection.
        AddExtraCursor(click, -1);
        // If we actually added one, track it as the drag target.
        if (!m_ExtraCursors.empty() &&
            m_ExtraCursors.back().caret == click &&
            m_ExtraCursors.back().anchor < 0) {
            m_DragCursorIndex = (int)m_ExtraCursors.size() - 1;
        }
    } else {
        m_CaretIndex = click;
        if (shift) {
            ApplyShiftClickExtend(oldCaret, click);
        } else {
            m_SelectionStart = m_SelectionEnd = m_CaretIndex;
            m_ExtraCursors.clear();
        }
    }
    MarkDirty(VisualDirty);
}

void TextArea::OnPointerDrag(float mouseX, float mouseY,
                             float x, float y, float W, float H,
                             const ResolvedStyle& style, FontAtlas* font) {
    (void)H;
    BumpCaretForceVisible();
    if (!font) return;

    const int click = HitTestCaret(mouseX, mouseY, x, y, W, style, font);

    if (m_DragCursorIndex >= 0 && m_DragCursorIndex < (int)m_ExtraCursors.size()) {
        // Extending an extra cursor's selection.
        Cursor& c = m_ExtraCursors[m_DragCursorIndex];
        if (c.anchor < 0) c.anchor = c.caret;
        c.caret = click;
    } else {
        m_CaretIndex = click;
        m_SelectionEnd = m_CaretIndex;
    }

    float padL = style.Layout.Padding.Left;
    m_NavPreferredX = std::max(0.0f, mouseX - (x + padL));
    EnsureCaretVisibleInScroll();
    MarkDirty(VisualDirty);
}

// =============================================================================
// Post-layout: expand bounds for scrolled content hit testing
// =============================================================================

void TextArea::OnPostLayout() {
    ScrollView* scrollView = FindParentScrollView(this);
    if (!scrollView) return;

    const float curH = GetLayoutHeight();

    const ResolvedStyle& rs = GetResolvedStyle();
    float padT = rs.Layout.Padding.Top;
    float padB = rs.Layout.Padding.Bottom;
    float borderT = rs.Layout.BorderWidth.Top;
    float borderB = rs.Layout.BorderWidth.Bottom;

    TextAreaMetrics m{};
    if (!ComputeMetrics(m))
        return;

    const std::string& text = GetValue();
    auto& tc = EnsureTextRenderCache();
    tc.EnsureLineBreaks(text, m.Font, m.Px, m.WrapWidth, m.LineBox, m.LetterSpacing,
                        MapWordBreak(rs));
    const auto& vLines = tc.Breaks;

    // Height override is a BORDER-box height — that is the box Yoga sizes —
    // so both insets have to be added back or the last line lands under the
    // bottom border. Compared against curH, which is the border box Yoga solved.
    float contentH = (int)vLines.size() * m.LineAdvance + padT + padB + borderT + borderB;
    if (contentH > curH)
    {
        const auto existing = Overrides().Get(Style::Height);
        if (!existing.has_value() || !existing->IsPx() || std::fabs(existing->Value - contentH) > 0.5f)
        {
            Overrides().Set(Style::Height, StyleLength::Px(contentH));
            MarkDirty(StyleDirty | LayoutDirty);
        }
    }
}

// =============================================================================
// Character input
// =============================================================================

bool TextArea::OnChar(unsigned int codepoint) {
    if (m_ReadOnly)
        return false;
    if (!Utf8::IsTextInputCodepoint(codepoint))
        return false;

    BumpCaretForceVisible();

    std::string ch;
    if (codepoint == 13u || codepoint == 10u) {
        // The Enter key already inserted this newline; absorbing the duplicate
        // still consumes it, or the keystroke would go on to reach gameplay.
        if (m_SwallowNextNewlineChar) { m_SwallowNextNewlineChar = false; return true; }
        ch = "\n";
    } else {
        ch = Utf8::Encode((uint32_t)codepoint);
    }

    std::string s = GetValue();
    ApplyMultiCursorEdit(s, [&](const Cursor&) { return ch; });
    m_NavPreferredX = -1.0f;
    SetValue(s);
    NotifyValueChanging();
    NotifyValueChanged();
    EnsureCaretVisibleInScroll();
    return true;
}

// =============================================================================
// Key input
// =============================================================================

// Per-cursor movement helpers. These mutate the passed Cursor in place and
// preserve / drop selection anchor as needed based on `shift`.
namespace {

void MoveHorizontal(TextArea::Cursor& c, const std::string& s,
                    bool goingRight, bool wordStep, bool shift)
{
    size_t pos = (size_t)std::clamp(c.caret, 0, (int)s.size());
    int newCaret = goingRight
        ? (int)(wordStep ? TextSegmentation::NextBoundary(s, pos) : Utf8::Next(s, pos))
        : (int)(wordStep ? TextSegmentation::PrevBoundary(s, pos) : Utf8::Prev(s, pos));

    if (shift) {
        if (c.anchor < 0) c.anchor = c.caret;
        c.caret = newCaret;
    } else {
        c.caret = newCaret;
        c.anchor = -1;
    }
    c.preferredX = -1.0f;
}

int FindLineContaining(const std::vector<LineBreakInfo>& lines, size_t byteIdx)
{
    for (int li = 0; li < (int)lines.size(); ++li) {
        if (byteIdx >= lines[li].ByteStart && byteIdx <= lines[li].ByteEnd)
            return li;
    }
    return 0;
}

float PreferredXForCaret(const std::vector<LineBreakInfo>& lines, int lineIdx,
                         size_t caret, const std::string& s,
                         Rendering::Text::FontAtlas* font, float px, float letterSpacingPx)
{
    const auto& vl = lines[lineIdx];
    std::string lineText(s.data() + vl.ByteStart, vl.ByteEnd - vl.ByteStart);
    std::vector<float> xByByte;
    font->BuildCaretMapUtf8(lineText, px, xByByte, letterSpacingPx);
    size_t localByte = caret - vl.ByteStart;
    return (localByte < xByByte.size())
        ? xByByte[localByte]
        : (xByByte.empty() ? 0.0f : xByByte.back());
}

int CaretFromPreferredX(const std::vector<LineBreakInfo>& lines, int lineIdx,
                        float targetX, const std::string& s,
                        Rendering::Text::FontAtlas* font, float px, float letterSpacingPx)
{
    const auto& tl = lines[lineIdx];
    std::string lineText(s.data() + tl.ByteStart, tl.ByteEnd - tl.ByteStart);
    std::vector<float> xByByte;
    font->BuildCaretMapUtf8(lineText, px, xByByte, letterSpacingPx);
    size_t rel = 0;
    while (rel < xByByte.size() && xByByte[rel] < targetX) rel++;
    rel = std::min(rel, (size_t)(tl.ByteEnd - tl.ByteStart));
    return (int)(tl.ByteStart + rel);
}

} // namespace

void TextArea::EnsureCaretVisibleInScroll() {
    ScrollView* sv = FindParentScrollView(this);
    if (!sv) return;

    const float viewportH = sv->GetViewportHeight();
    if (viewportH <= 0.0f) return;

    TextAreaMetrics met{};
    if (!ComputeMetrics(met)) return;
    if (met.LineAdvance <= 0.0f) return;

    const std::string& s = GetValue();
    auto& tc = EnsureTextRenderCache();
    tc.EnsureLineBreaks(s, met.Font, met.Px, met.WrapWidth, met.LineBox,
                        met.LetterSpacing, MapWordBreak(GetResolvedStyle()));
    const auto& navLines = tc.Breaks;
    if (navLines.empty()) return;

    size_t caret = (size_t)std::clamp(m_CaretIndex, 0, (int)s.size());
    int caretLine = FindLineContaining(navLines, caret);

    const float padT = GetResolvedStyle().Layout.Padding.Top;
    const float caretLineTop = padT + caretLine * met.LineAdvance;
    const float caretLineBottom = caretLineTop + met.LineAdvance;

    const float localTop = sv->GetLayoutY() - GetLayoutY();

    float dy = 0.0f;
    if (caretLineTop < localTop)
        dy = caretLineTop - localTop;
    else if (caretLineBottom > localTop + viewportH)
        dy = caretLineBottom - (localTop + viewportH);

    if (std::fabs(dy) > 0.5f)
        sv->ScrollBy(0.0f, dy);
}

// Returns whether the keystroke was the area's to answer. Unlike single-line
// TextInput this one owns vertical navigation too, so Up/Down and the page keys
// are claimed. A read-only area declines the keys that would edit it while still
// claiming the ones that move its caret.
bool TextArea::OnKey(int key, int mods, UI::IPlatformApi* platform) {
    // A modifier's own press carries no text and belongs to whoever reads held
    // state; function keys are application-level. Neither is the area's.
    if (Input::IsModifierKey(key) || Input::IsFunctionKey(key))
        return false;

    // Every key that reaches a focused text area either moves the caret, edits
    // around it, or is a shortcut the user is watching the caret through, so
    // the deadline is stamped once here rather than at each of the ~30 branches
    // below — a per-branch bump is exactly the shape that leaves one out.
    BumpCaretForceVisible();

    std::string s = GetValue();
    bool shift = (mods & Input::kModShift) != 0;
    bool primaryMod = Input::IsPrimaryShortcutModifier(mods);

    // Helper: apply a per-cursor movement function to primary + extras.
    auto moveAllCursors = [&](const std::function<void(Cursor&)>& move) {
        Cursor p = PrimaryAsCursor();
        move(p);
        WritePrimaryFromCursor(p);
        for (auto& c : m_ExtraCursors) move(c);
        MergeOverlappingCursors();
    };

    // Clipboard / selection shortcuts
    if (primaryMod) {
        if (key == Input::kKeyCode_A) {
            m_SelectionStart = 0;
            m_SelectionEnd = (int)s.size();
            m_CaretIndex = m_SelectionEnd;
            m_NavPreferredX = -1.0f;
            m_ExtraCursors.clear();
            EnsureCaretVisibleInScroll();
            MarkDirty(VisualDirty);
            return true;
        }

        if (key == Input::kKeyCode_C || key == Input::kKeyCode_X) {
            if (platform) {
                // Concatenate each cursor's selection (top-to-bottom) joined by \n.
                std::vector<Cursor> all;
                all.reserve(m_ExtraCursors.size() + 1);
                all.push_back(PrimaryAsCursor());
                for (const auto& c : m_ExtraCursors) all.push_back(c);
                std::sort(all.begin(), all.end(), [](const Cursor& a, const Cursor& b) {
                    return a.SelStart() < b.SelStart();
                });

                std::string clipText;
                bool any = false;
                for (size_t i = 0; i < all.size(); ++i) {
                    if (!all[i].HasSelection()) continue;
                    int a = std::clamp(all[i].SelStart(), 0, (int)s.size());
                    int b = std::clamp(all[i].SelEnd(),   0, (int)s.size());
                    if (any) clipText.append("\n");
                    clipText.append(s.substr((size_t)a, (size_t)(b - a)));
                    any = true;
                }
                if (any) {
                    platform->SetClipboardText(clipText.c_str());
                    if (key == Input::kKeyCode_X && !m_ReadOnly) {
                        ApplyMultiCursorEdit(s, [&](const Cursor&) { return std::string{}; });
                        SetValue(s);
                        NotifyValueChanging();
                        NotifyValueChanged();
                    }
                }
            }
            EnsureCaretVisibleInScroll();
            MarkDirty(VisualDirty);
            return true;
        }

        if (key == Input::kKeyCode_V) {
            if (m_ReadOnly) return false;
            if (platform) {
                std::string clip = platform->GetClipboardText();
                if (!clip.empty()) {
                    // If clipboard has N lines and we have N cursors, paste one line per cursor.
                    std::vector<std::string> clipLines;
                    {
                        size_t start = 0;
                        for (size_t i = 0; i <= clip.size(); ++i) {
                            if (i == clip.size() || clip[i] == '\n') {
                                clipLines.emplace_back(clip.substr(start, i - start));
                                start = i + 1;
                            }
                        }
                    }
                    const int cursorCount = 1 + (int)m_ExtraCursors.size();
                    const bool perCursorLines = (int)clipLines.size() == cursorCount && cursorCount > 1;

                    // Build ordered cursor list so we can match clipboard lines by visual order.
                    std::vector<Cursor> ordered;
                    ordered.reserve(cursorCount);
                    ordered.push_back(PrimaryAsCursor());
                    for (const auto& c : m_ExtraCursors) ordered.push_back(c);
                    std::sort(ordered.begin(), ordered.end(), [](const Cursor& a, const Cursor& b) {
                        return a.caret < b.caret;
                    });

                    ApplyMultiCursorEdit(s, [&](const Cursor& c) -> std::string {
                        if (!perCursorLines) return clip;
                        int idx = 0;
                        for (size_t i = 0; i < ordered.size(); ++i) {
                            if (ordered[i].caret == c.caret && ordered[i].anchor == c.anchor) {
                                idx = (int)i;
                                break;
                            }
                        }
                        return clipLines[std::clamp(idx, 0, (int)clipLines.size() - 1)];
                    });
                    m_NavPreferredX = -1.0f;
                    SetValue(s);
                    NotifyValueChanging();
                    NotifyValueChanged();
                }
            }
            EnsureCaretVisibleInScroll();
            MarkDirty(VisualDirty);
            return true;
        }
    }

    // Set false only by the branches that decline the key; the common tail
    // below returns it.
    bool consumed = true;

    switch (key) {
    case Input::kKeyCode_Escape:
    {
        // Only a multi-cursor session is Escape's to collapse here. With none,
        // the key belongs to the popup or play session behind the area.
        const bool hadExtraCursors = !m_ExtraCursors.empty();
        if (hadExtraCursors) {
            m_ExtraCursors.clear();
            MarkDirty(VisualDirty);
        }
        return hadExtraCursors;
    }

    case Input::kKeyCode_Enter:
    case Input::kKeyCode_NumPadEnter:
    {
        if (m_ReadOnly) return false;
        // In submit-on-Enter mode only Shift+Enter breaks the line. Shift is read from the
        // event and from the manager's held keys, as an injected key may carry no modifiers.
        const UIManager* owner = GetOwnerManager();
        const bool shiftHeld = shift || (owner && (owner->GetModifierKeys() & Input::kModShift) != 0);
        if (m_OnSubmit && !shiftHeld) {
            // The char the key sends after it is the submit's too. Copied first: the submit
            // may destroy this area.
            m_SwallowNextNewlineChar = true;
            const std::function<void()> submit = m_OnSubmit;
            submit();
            return true;
        }
        ApplyMultiCursorEdit(s, [](const Cursor&) { return std::string("\n"); });
        m_NavPreferredX = -1.0f;
        m_SwallowNextNewlineChar = true;
        SetValue(s);
        NotifyValueChanging();
        NotifyValueChanged();
        break;
    }

    case Input::kKeyCode_Backspace:
    {
        if (m_ReadOnly) return false;
        ApplyMultiCursorEdit(
            s,
            [](const Cursor&) { return std::string{}; },
            [](const std::string& buf, Cursor& c) {
                size_t pos = (size_t)std::clamp(c.caret, 0, (int)buf.size());
                if (pos > 0) {
                    size_t prev = Utf8::Prev(buf, pos);
                    c.anchor = (int)prev;
                }
            });
        m_NavPreferredX = -1.0f;
        SetValue(s); NotifyValueChanging(); NotifyValueChanged();
        break;
    }

    case Input::kKeyCode_Delete:
    {
        if (m_ReadOnly) return false;
        ApplyMultiCursorEdit(
            s,
            [](const Cursor&) { return std::string{}; },
            [](const std::string& buf, Cursor& c) {
                size_t pos = (size_t)std::clamp(c.caret, 0, (int)buf.size());
                if (pos < buf.size()) {
                    size_t next = Utf8::Next(buf, pos);
                    c.anchor = (int)next;
                }
            });
        m_NavPreferredX = -1.0f;
        SetValue(s); NotifyValueChanging(); NotifyValueChanged();
        break;
    }

    case Input::kKeyCode_Left:
    {
        moveAllCursors([&](Cursor& c) {
            MoveHorizontal(c, s, /*goingRight*/false, /*wordStep*/primaryMod, shift);
        });
        m_NavPreferredX = -1.0f;
        break;
    }

    case Input::kKeyCode_Right:
    {
        moveAllCursors([&](Cursor& c) {
            MoveHorizontal(c, s, /*goingRight*/true, /*wordStep*/primaryMod, shift);
        });
        m_NavPreferredX = -1.0f;
        break;
    }

    case Input::kKeyCode_Up:
    case Input::kKeyCode_Down:
    {
        TextAreaMetrics met{};
        // No metrics means no line breaks to navigate by, so the caret cannot
        // move. Declining keeps an unlaid-out area from swallowing the key.
        if (!ComputeMetrics(met)) { consumed = false; break; }

        auto& tc = EnsureTextRenderCache();
        tc.EnsureLineBreaks(s, met.Font, met.Px, met.WrapWidth, met.LineBox,
                            met.LetterSpacing, MapWordBreak(GetResolvedStyle()));
        const auto& navLines = tc.Breaks;
        const bool goingDown = (key == Input::kKeyCode_Down);

        moveAllCursors([&](Cursor& c) {
            size_t caret = (size_t)std::clamp(c.caret, 0, (int)s.size());
            int curLine = FindLineContaining(navLines, caret);

            if (c.preferredX < 0.0f)
                c.preferredX = PreferredXForCaret(navLines, curLine, caret, s, met.Font, met.Px, met.LetterSpacing);

            int targetLine = std::clamp(curLine + (goingDown ? 1 : -1),
                                        0, std::max(0, (int)navLines.size() - 1));
            int newCaret = (targetLine != curLine)
                ? CaretFromPreferredX(navLines, targetLine, c.preferredX, s, met.Font, met.Px, met.LetterSpacing)
                : c.caret;

            if (shift) {
                if (c.anchor < 0) c.anchor = c.caret;
                c.caret = newCaret;
            } else {
                c.caret = newCaret;
                c.anchor = -1;
            }
        });
        break;
    }

    case Input::kKeyCode_Home:
    {
        if (primaryMod) {
            moveAllCursors([&](Cursor& c) {
                if (shift) {
                    if (c.anchor < 0) c.anchor = c.caret;
                    c.caret = 0;
                } else {
                    c.caret = 0;
                    c.anchor = -1;
                }
                c.preferredX = -1.0f;
            });
        } else {
            TextAreaMetrics met{};
            // Same rule as Up/Down: without metrics there is no line to move to.
            if (!ComputeMetrics(met)) {
                consumed = false;
            } else {
                auto& tc = EnsureTextRenderCache();
                tc.EnsureLineBreaks(s, met.Font, met.Px, met.WrapWidth, met.LineBox,
                                    met.LetterSpacing, MapWordBreak(GetResolvedStyle()));
                const auto& navLines = tc.Breaks;
                moveAllCursors([&](Cursor& c) {
                    size_t caret = (size_t)std::clamp(c.caret, 0, (int)s.size());
                    int curLine = FindLineContaining(navLines, caret);
                    int next = (int)navLines[curLine].ByteStart;
                    if (shift) {
                        if (c.anchor < 0) c.anchor = c.caret;
                        c.caret = next;
                    } else {
                        c.caret = next;
                        c.anchor = -1;
                    }
                    c.preferredX = -1.0f;
                });
            }
        }
        m_NavPreferredX = -1.0f;
        break;
    }

    case Input::kKeyCode_End:
    {
        if (primaryMod) {
            moveAllCursors([&](Cursor& c) {
                if (shift) {
                    if (c.anchor < 0) c.anchor = c.caret;
                    c.caret = (int)s.size();
                } else {
                    c.caret = (int)s.size();
                    c.anchor = -1;
                }
                c.preferredX = -1.0f;
            });
        } else {
            TextAreaMetrics met{};
            // Same rule as Up/Down: without metrics there is no line to move to.
            if (!ComputeMetrics(met)) {
                consumed = false;
            } else {
                auto& tc = EnsureTextRenderCache();
                tc.EnsureLineBreaks(s, met.Font, met.Px, met.WrapWidth, met.LineBox,
                                    met.LetterSpacing, MapWordBreak(GetResolvedStyle()));
                const auto& navLines = tc.Breaks;
                moveAllCursors([&](Cursor& c) {
                    size_t caret = (size_t)std::clamp(c.caret, 0, (int)s.size());
                    int curLine = FindLineContaining(navLines, caret);
                    int next = (int)navLines[curLine].ByteEnd;
                    if (shift) {
                        if (c.anchor < 0) c.anchor = c.caret;
                        c.caret = next;
                    } else {
                        c.caret = next;
                        c.anchor = -1;
                    }
                    c.preferredX = -1.0f;
                });
            }
        }
        m_NavPreferredX = -1.0f;
        break;
    }

    case Input::kKeyCode_PageUp:
    case Input::kKeyCode_PageDown:
    {
        const bool goingDown = (key == Input::kKeyCode_PageDown);

        TextAreaMetrics met{};
        // No metrics means no line breaks to navigate by, so the caret cannot
        // move. Declining keeps an unlaid-out area from swallowing the key.
        if (!ComputeMetrics(met)) { consumed = false; break; }

        int pageLines = kDefaultPageLines;
        float viewportH = 0.0f;
        ScrollView* sv = FindParentScrollView(this);

        if (sv) {
            viewportH = sv->GetViewportHeight();
            if (viewportH > 1.0f)
                pageLines = std::max(1, (int)std::floor(viewportH / met.LineAdvance) - 1);
        }

        auto& tc = EnsureTextRenderCache();
        tc.EnsureLineBreaks(s, met.Font, met.Px, met.WrapWidth, met.LineBox,
                            met.LetterSpacing, MapWordBreak(GetResolvedStyle()));
        const auto& navLines = tc.Breaks;

        moveAllCursors([&](Cursor& c) {
            size_t caret = (size_t)std::clamp(c.caret, 0, (int)s.size());
            int curLine = FindLineContaining(navLines, caret);
            if (c.preferredX < 0.0f)
                c.preferredX = PreferredXForCaret(navLines, curLine, caret, s, met.Font, met.Px, met.LetterSpacing);

            int targetLine = std::clamp(curLine + (goingDown ? pageLines : -pageLines),
                                        0, std::max(0, (int)navLines.size() - 1));
            int nextCaret = CaretFromPreferredX(navLines, targetLine, c.preferredX, s, met.Font, met.Px, met.LetterSpacing);

            if (shift) {
                if (c.anchor < 0) c.anchor = c.caret;
                c.caret = nextCaret;
            } else {
                c.caret = nextCaret;
                c.anchor = -1;
            }
        });

        if (sv && viewportH > 1.0f) {
            const float dy = std::max(0.0f, viewportH - met.LineAdvance);
            if (dy > 0.0f)
                sv->ScrollBy(0.0f, goingDown ? dy : -dy);
        }
        break;
    }

    default:
        // The paired character event is what this area acts on, so a keystroke
        // that composes one is claimed here. Chords this area implements
        // (select-all, copy/cut/paste, word and document navigation) already
        // returned above; a chord that composes nothing — primary-modifier
        // anywhere, Alt+letter outside macOS — bubbles so the application's
        // binding for it still fires with the caret sitting here.
        consumed = Input::ComposesTextInput(key, mods);
        break;
    }

    EnsureCaretVisibleInScroll();
    MarkDirty(VisualDirty);
    return consumed;
}

// =============================================================================
// Focus
// =============================================================================

void TextArea::OnFocusChanged(bool focused) {
    if (focused) {
        BumpCaretForceVisible();
        // FocusIn is dispatched at the start of the frame *after* mousedown updates m_FocusId.
        // OnPointerDown already ran on the click frame and set caret/selection; resetting here
        // would clear m_SelectionStart and break the first drag-select until another mousedown.
        UIManager* owner = GetOwnerManager();
        const bool keyboardFocus = owner && owner->IsFocusViaKeyboard();
        if (keyboardFocus) {
            m_CaretIndex = (int)GetValue().size();
            m_SelectionStart = m_SelectionEnd = -1;
            m_LastAutoScrollCaret = -1;
            m_LastAutoScrollSelectionStart = -1;
            m_LastAutoScrollSelectionEnd = -1;
        }
    }
    // Intentionally keep selection when unfocused so it survives scrolling (e.g. scrollbar) and
    // remains visible as inactive highlighting until the user edits or selects elsewhere.
}

// =============================================================================
// Text rendering + caret and selection overlay
// =============================================================================

void TextArea::EmitTextGlyphs(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float originX, float originY, const TextAreaMetrics& met)
{
    using namespace UI;

    const std::string& text = GetValue();
    if (text.empty())
        return;

    auto& tc = EnsureTextRenderCache();
    tc.EnsureLineBreaks(text, met.Font, met.Px, met.WrapWidth, met.LineBox,
                        met.LetterSpacing, MapWordBreak(style));
    const auto& vLines = tc.Breaks;
    const int totalLines = static_cast<int>(vLines.size());

    // Only the lines inside the scrolling ancestor's clip viewport are drawn.
    // The band is measured from where this area's text actually sits against
    // that viewport, never from the scroll offset alone: the area can be one of
    // several stacked in the scroll content, and the scroll translation is
    // already in both layout rects. All four terms are logical px, the space
    // LineAdvance is in.
    int firstLine = 0;
    int lastLine = totalLines - 1;
    const ScrollView* sv = FindScrollingAncestor(this);
    const UIElement* clip = sv ? sv->GetClipViewport() : nullptr;
    const float viewportH = sv ? sv->GetViewportHeight() : 0.0f;
    if (clip && viewportH > 0.0f && met.LineAdvance > 0.0f) {
        const float textTop = GetLayoutY() + style.Layout.Padding.Top + style.Layout.BorderWidth.Top;
        const float bandTop = clip->GetLayoutY() - textTop;
        firstLine = std::max(0, (int)std::floor(bandTop / met.LineAdvance));
        lastLine = std::min(totalLines - 1, (int)std::ceil((bandTop + viewportH) / met.LineAdvance));
    }

    auto lm = met.Font->GetLineMetrics(met.Px);
    const float lineH = std::max(1.0f, lm.height);
    // Clamped at zero: this editor steps a fixed LineAdvance per line, so a line
    // box tighter than the font would lift line 1 out of the top of the content
    // box while every later line stayed where the advance put it.
    const float glyphYOffset = std::max(0.0f, TextLayout::HalfLeadingPx(lineH, met.LineAdvance));

    const uint32_t textColor = PackFromARGB(style.Visual.Color);

    if (static_cast<int>(tc.Shapes.size()) < totalLines)
        tc.Shapes.resize(totalLines);

    static thread_local FontAtlas::ShapeResult scratchResult;

    // Register slug textures ONCE for the whole TextArea (all lines share the
    // same font). Returns a const ref to avoid per-line vector copies.
    static const std::vector<UITextureRegistry::SlugTextureIndices> kEmptySlugPages;
    const auto& slugPages = (ctx.Textures && met.Font)
        ? ctx.Textures->RegisterSlugTextures(*met.Font)
        : kEmptySlugPages;

    const UI::GlyphRunTarget runTarget = UI::MakeGlyphRunTarget(ctx, met.Font, slugPages);

    for (int lineIdx = firstLine; lineIdx <= lastLine; ++lineIdx)
    {
        if (lineIdx < 0 || lineIdx >= totalLines) continue;
        const auto& lb = vLines[lineIdx];
        size_t lineLen = lb.ByteEnd - lb.ByteStart;

        std::string_view lineView{text.data() + lb.ByteStart, lineLen};
        uint64_t lineTextHash = std::hash<std::string_view>{}(lineView);

        auto& ls = tc.Shapes[lineIdx];

        // Cache key includes textColor so a CSS color change (hover/focus/
        // selection styling) invalidates stale per-glyph colors.
        if (ls.TextHash != lineTextHash ||
            ls.FontId != reinterpret_cast<uintptr_t>(met.Font) ||
            ls.PixelSize != met.Px ||
            ls.LetterSpacing != met.LetterSpacing ||
            ls.Color != textColor)
        {
            scratchResult.glyphs = std::move(ls.Glyphs);
            met.Font->ShapeText(lineView, met.Px, scratchResult, textColor, met.LetterSpacing);
            ls.TextHash = lineTextHash;
            ls.FontId = reinterpret_cast<uintptr_t>(met.Font);
            ls.PixelSize = met.Px;
            ls.LetterSpacing = met.LetterSpacing;
            ls.Color = textColor;
            ls.Glyphs = std::move(scratchResult.glyphs);
        }

        float lineY = originY + lineIdx * met.LineAdvance + glyphYOffset;
        UI::EmitGlyphRun(ls.Glyphs, originX, lineY, lm.ascender, runTarget);
    }
}

void TextArea::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                                    const ResolvedStyle& style,
                                    float x, float y, float w, float /*h*/)
{
    using namespace UI;

    auto* manager = GetOwnerManager();
    if (!manager)
        return;

    // Metrics, per-line shaping, and caret maps all measure on the shared
    // primary face — UI-thread-only (see the OnGeneratePrimitives thread
    // contract). Runs before ComputeMetrics so even an empty text area
    // never touches the font from a JobSystem worker.
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }

    TextAreaMetrics m{};
    if (!ComputeMetrics(style, w, m))
        return;

    // (x, y) arrive in physical px; CSS padding/border values are logical.
    // Scale them to physical so the text origin matches the rect.
    const float cs = ctx.ContentScale;
    const float padL = style.Layout.Padding.Left * cs;
    const float padT = style.Layout.Padding.Top * cs;
    const float borderL = style.Layout.BorderWidth.Left * cs;
    const float borderT = style.Layout.BorderWidth.Top * cs;
    const float originX = x + padL + borderL;
    const float originY = y + padT + borderT;

    // Paint order below is fixed by CSS: a selection highlight is a background,
    // so it goes UNDER the glyphs; the caret goes over them. Emission order is
    // composite order, so the sequence here is selection -> glyphs -> carets.
    const bool isFocused = IsFocusTargetForId(manager->GetFocusedElementId());
    const std::string& text = GetValue();
    const int textLen = static_cast<int>(text.size());

    // Collect all cursors (primary + extras) for unified rendering.
    std::vector<Cursor> allCursors;
    allCursors.reserve(m_ExtraCursors.size() + 1);
    allCursors.push_back(PrimaryAsCursor());
    for (const auto& c : m_ExtraCursors) allCursors.push_back(c);

    bool anyHasSelection = false;
    for (const auto& c : allCursors) {
        if (c.HasSelection()) { anyHasSelection = true; break; }
    }

    const bool needOverlays =
        (isFocused || anyHasSelection) &&
        !(isFocused && !anyHasSelection && m_ReadOnly && m_ExtraCursors.empty());

    if (!needOverlays)
    {
        EmitTextGlyphs(ctx, style, originX, originY, m);
        return;
    }

    auto& tc = EnsureTextRenderCache();
    tc.EnsureLineBreaks(text, m.Font, m.Px, m.WrapWidth, m.LineBox, m.LetterSpacing,
                        MapWordBreak(style));
    const auto& vLines = tc.Breaks;
    const int totalLines = static_cast<int>(vLines.size());

    auto visualLineOfByte = [&](size_t byteIdx) -> int {
        int lo = 0, hi = totalLines - 1;
        while (lo <= hi)
        {
            int mid = lo + (hi - lo) / 2;
            if (byteIdx < vLines[mid].ByteStart)
                hi = mid - 1;
            else if (byteIdx > vLines[mid].ByteEnd)
                lo = mid + 1;
            else
            {
                // At wrap boundaries byteEnd[i] == byteStart[i+1]; prefer the earlier line.
                if (mid > 0 &&
                    byteIdx == vLines[mid].ByteStart &&
                    byteIdx <= vLines[mid - 1].ByteEnd)
                {
                    return mid - 1;
                }
                return mid;
            }
        }
        return std::max(0, totalLines - 1);
    };

    // Cache caret maps per visual line to avoid recomputing for every cursor.
    std::vector<std::vector<float>> caretMapCache(totalLines);
    std::vector<bool> caretMapValid(totalLines, false);

    auto xForByte = [&](size_t byteIdx, int vLine) -> float {
        if (vLine < 0 || vLine >= totalLines)
            return 0.0f;
        if (!caretMapValid[vLine]) {
            const auto& vl = vLines[vLine];
            std::string lineText(text.data() + vl.ByteStart, vl.ByteEnd - vl.ByteStart);
            m.Font->BuildCaretMapUtf8(lineText, m.Px, caretMapCache[vLine], m.LetterSpacing);
            caretMapValid[vLine] = true;
        }
        const auto& caretMap = caretMapCache[vLine];
        size_t localByte = byteIdx - vLines[vLine].ByteStart;
        if (localByte < caretMap.size())
            return caretMap[localByte];
        return caretMap.empty() ? 0.0f : caretMap.back();
    };

    const float fontSize = std::max(1.0f, style.Visual.FontSize);
    const uint32_t packedSelColor = UI::PackedTextSelectionFill(style.Visual, isFocused);

    // Emit selection rects for every cursor with a selection.
    for (const auto& c : allCursors) {
        if (!c.HasSelection()) continue;
        int a = std::clamp(c.SelStart(), 0, textLen);
        int b = std::clamp(c.SelEnd(),   0, textLen);

        int lineA = visualLineOfByte((size_t)a);
        int lineB = visualLineOfByte((size_t)b);

        for (int line = lineA; line <= lineB; ++line) {
            float startX = (line == lineA) ? xForByte((size_t)a, line) : 0.0f;
            float endX   = (line == lineB) ? xForByte((size_t)b, line)
                                           : xForByte(vLines[line].ByteEnd, line);

            float selW = std::max(0.0f, endX - startX);
            if (selW <= 0.0f && line != lineB)
                selW = fontSize * 0.5f;

            if (selW > 0.0f) {
                float ry = originY + (float)line * m.LineAdvance;
                UIPrimitive prim = MakeRect(originX + startX, ry, selW, m.LineAdvance, packedSelColor);
                ctx.Emit(prim);
            }
        }
    }

    // --- Text glyphs (visible lines only, per-line cached) ---
    EmitTextGlyphs(ctx, style, originX, originY, m);

    // Emit carets for every cursor when focused (and not read-only).
    if (isFocused && !m_ReadOnly) {
        constexpr float kCaretWidth = 1.5f;
        uint32_t caretColor = PackFromARGB(style.Visual.Color);

        for (const auto& c : allCursors) {
            int clamped = std::clamp(c.caret, 0, textLen);
            int caretLine = visualLineOfByte((size_t)clamped);
            float caretX = originX + xForByte((size_t)clamped, caretLine);
            float caretY = originY + (float)caretLine * m.LineAdvance;

            UIPrimitive prim{};
            prim.X = caretX;
            prim.Y = caretY;
            prim.W = kCaretWidth;
            prim.H = m.LineAdvance;
            prim.FillColor = caretColor;
            prim.Opacity = style.Visual.Opacity;
            // Read, never stamped: the deadline belongs to the input that moved
            // the caret (BumpCaretForceVisible). Re-deriving it here would put
            // it a whole window ahead of the clock on every emitted frame, so
            // the shader's `timeSeconds >= CaretTime` could never be true.
            prim.CaretTime = CaretForceVisibleUntil();
            prim.ModeAndFlags = MakeFlags(PrimitiveMode::Rect, UI::GradientMode::None,
                                           ctx.ClipIndex, false, true);
            ctx.Primitives.push_back(prim);
        }
    }
}

} // namespace GameEngine
