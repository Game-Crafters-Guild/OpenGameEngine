#include "Panels/ScriptTextArea.h"
#include "Rendering/Text/FontAtlas.h"
#include "Rendering/Text/TextLayout.h"
#include "UI/UIStyle.h"
#include "UI/UIPrimitive.h"
#include "UI/GlyphRunEmitter.h"
#include "UI/TextSelectionFill.h"
#include "UI/UITextureRegistry.h"
#include "UI/UIManager.h"
#include "UI/ResolvedStyle.h"
#include "UI/Controls/ScrollView.h"
#include "UI/UIElement.h"

#include "UI/UIEvents.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Foldout.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Platform/SystemMetrics.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <unordered_set>

namespace GameEngine {

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

namespace
{
constexpr float kFoldGutterWidthPx = 28.0f;
constexpr float kFoldMarkerIndentPx = 10.0f;
constexpr float kFoldMarkerInsetPx = 6.0f;
constexpr float kLineNumberRightPaddingPx = 8.0f;
constexpr uint32_t kLineNumberColorArgb = 0xFF828282u;
constexpr uint32_t kActiveLineNumberColorArgb = 0xFFFFFFFFu;
constexpr uint32_t kCaretLineNumberColorArgb = 0xFF3670D4u;
constexpr uint32_t kFoldMarkerColorArgb = 0xFFFFFFFFu;
constexpr float kFoldMarkerBoldOffsetPx = 1.0f;
constexpr float kMultiClickTolerancePx = 5.0f;
constexpr float kMultiClickDragBreakThresholdPx = 5.0f;

static bool IsWordCharByte(unsigned char c)
{
    return std::isalnum(c) != 0 || c == '_' || c == '$' || c == '@';
}

static std::pair<size_t, size_t> FindWordBoundsAtByte(const std::string& text, size_t pos)
{
    if (text.empty())
        return {0, 0};
    if (pos > text.size())
        pos = text.size();

    // Past-end clicks often land on \\n or on \\n after \\r; those are not word chars and the
    // forward scan would pick the next line. Step back across line endings first.
    size_t effectivePos = pos;
    while (effectivePos > 0 && effectivePos < text.size())
    {
        const unsigned char c = static_cast<unsigned char>(text[effectivePos]);
        if (c == '\n' || c == '\r')
            --effectivePos;
        else
            break;
    }

    size_t anchor = effectivePos;
    if (effectivePos < text.size() && IsWordCharByte(static_cast<unsigned char>(text[effectivePos])))
        anchor = effectivePos;
    else if (effectivePos > 0 && IsWordCharByte(static_cast<unsigned char>(text[effectivePos - 1])))
        anchor = effectivePos - 1;
    else
    {
        size_t p = effectivePos;
        while (p < text.size() && !IsWordCharByte(static_cast<unsigned char>(text[p])))
            ++p;
        if (p >= text.size())
            return {effectivePos, effectivePos};
        anchor = p;
    }

    size_t start = anchor;
    while (start > 0 && IsWordCharByte(static_cast<unsigned char>(text[start - 1])))
        --start;
    size_t end = anchor;
    while (end < text.size() && IsWordCharByte(static_cast<unsigned char>(text[end])))
        ++end;
    return {start, end};
}

static int ClampCaretIndexToLineByteRange(int caret, size_t lineStart, size_t lineEndExclusive)
{
    if (lineEndExclusive <= lineStart)
        return static_cast<int>(lineStart);
    const int lo = static_cast<int>(lineStart);
    const int hi = static_cast<int>(lineEndExclusive) - 1;
    if (caret < lo)
        return lo;
    if (caret > hi)
        return hi;
    return caret;
}
}

void ScriptTextArea::InvalidateLineCaches() const
{
    m_LineStartCacheValid = false;
    m_VisualLineMapValid = false;
}

void ScriptTextArea::EnsureLineStartCache(const std::string& text) const
{
    if (m_LineStartCacheValid)
        return;

    m_LineStarts.clear();
    m_LineStarts.reserve(256);
    m_LineStarts.push_back(0);
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\n')
            m_LineStarts.push_back(i + 1);
    }
    if (m_LineStarts.empty())
        m_LineStarts.push_back(0);

    m_LineStartCacheValid = true;
    // Visual mapping depends on line starts.
    m_VisualLineMapValid = false;
}

size_t ScriptTextArea::GetLineIndexForBytePos(size_t bytePos) const
{
    if (!m_LineStartCacheValid || m_LineStarts.empty())
        return 0;

    auto it = std::upper_bound(m_LineStarts.begin(), m_LineStarts.end(), bytePos);
    if (it == m_LineStarts.begin())
        return 0;
    return (size_t)std::distance(m_LineStarts.begin(), it - 1);
}

static Rendering::Text::TextLayout::WordBreak MapScriptWordBreak(const ResolvedStyle& style)
{
    switch (ResolveTextBreakPolicy(style.Visual.WordBreak, style.Visual.OverflowWrap)) {
    case WordBreak::BreakAll:  return Rendering::Text::TextLayout::WordBreak::BreakAll;
    case WordBreak::KeepAll:   return Rendering::Text::TextLayout::WordBreak::KeepAll;
    case WordBreak::BreakWord: return Rendering::Text::TextLayout::WordBreak::BreakWord;
    default:                   return Rendering::Text::TextLayout::WordBreak::Normal;
    }
}

void ScriptTextArea::EnsureVisualLineMapCache(const std::string& text) const
{
    EnsureLineStartCache(text);

    if (!m_FoldRegionsValid)
        DetectFoldRegions(text);

    // Determine current wrap/font context so we can key the cache on it.
    const ResolvedStyle& rs = GetResolvedStyle();
    TextAreaMetrics met{};
    const bool haveMetrics = ComputeMetrics(rs, GetLayoutWidth(), met);

    // ComputeMetrics sets WrapWidth = 0 for NoWrap/Pre; otherwise it's the content width.
    const float effectiveWrap = haveMetrics ? met.WrapWidth : 0.0f;
    const auto wordBreak = MapScriptWordBreak(rs);
    const uintptr_t fontId = haveMetrics ? reinterpret_cast<uintptr_t>(met.Font) : 0;
    const float pixelSize = haveMetrics ? met.Px : 0.0f;
    const float lineBoxPx = haveMetrics ? met.LineBox : 0.0f;

    if (m_VisualLineMapValid &&
        m_LineMapFontId == fontId &&
        m_LineMapPixelSize == pixelSize &&
        m_LineMapWrapWidth == effectiveWrap &&
        m_LineMapLineBox == lineBoxPx &&
        m_LineMapWordBreak == static_cast<int>(wordBreak))
        return;

    const size_t totalLines = std::max<size_t>(1, m_LineStarts.size());

    std::vector<uint8_t> visible(totalLines, 1);
    for (const auto& region : m_FoldRegions)
    {
        if (!IsLineFolded(region.StartLine))
            continue;
        const size_t end = std::min(region.EndLine, totalLines - 1);
        for (size_t line = region.StartLine + 1; line <= end; ++line)
            visible[line] = 0;
    }

    m_VisualLines.clear();
    m_VisualLines.reserve(totalLines);
    m_ActualToVisualLine.assign(totalLines, 0);
    uint32_t maxVisibleLineNumber = 1;

    auto trimTrailingNewline = [&text](size_t start, size_t endExclusive) {
        while (endExclusive > start && endExclusive <= text.size() &&
               (text[endExclusive - 1] == '\n' || text[endExclusive - 1] == '\r'))
            --endExclusive;
        return endExclusive;
    };

    auto appendSegments = [&](uint32_t actualLine, size_t lineStart, size_t lineEndExclusive) {
        m_ActualToVisualLine[actualLine] = (uint32_t)m_VisualLines.size();

        // Empty source line → one empty visual segment so the row is rendered/navigable.
        if (lineStart >= lineEndExclusive) {
            m_VisualLines.push_back(VisualLineEntry{actualLine, (uint32_t)lineStart, (uint32_t)lineEndExclusive});
            return;
        }

        if (effectiveWrap <= 0.0f || !haveMetrics) {
            m_VisualLines.push_back(VisualLineEntry{actualLine, (uint32_t)lineStart, (uint32_t)lineEndExclusive});
            return;
        }

        Rendering::Text::StyledRun run{};
        run.Font = met.Font;
        run.PixelSize = met.Px;
        run.LetterSpacingPx = met.LetterSpacing;
        run.Text.assign(text, lineStart, lineEndExclusive - lineStart);

        auto result = Rendering::Text::TextLayout::ShapeMultiline(
            std::span<const Rendering::Text::StyledRun>(&run, 1),
            effectiveWrap, met.LineBox, wordBreak);

        if (result.LineBreaks.empty()) {
            m_VisualLines.push_back(VisualLineEntry{actualLine, (uint32_t)lineStart, (uint32_t)lineEndExclusive});
            return;
        }

        for (const auto& br : result.LineBreaks) {
            size_t segStart = lineStart + br.ByteStart;
            size_t segEnd = lineStart + br.ByteEnd;
            if (segEnd > lineEndExclusive) segEnd = lineEndExclusive;
            if (segStart > lineEndExclusive) segStart = lineEndExclusive;
            m_VisualLines.push_back(VisualLineEntry{actualLine, (uint32_t)segStart, (uint32_t)segEnd});
        }
    };

    for (size_t i = 0; i < totalLines; ++i)
    {
        if (!visible[i])
            continue;

        const size_t lineStart = m_LineStarts[i];
        size_t lineEnd = text.size();
        if (i + 1 < m_LineStarts.size())
            lineEnd = m_LineStarts[i + 1];
        lineEnd = trimTrailingNewline(lineStart, lineEnd);

        appendSegments((uint32_t)i, lineStart, lineEnd);
        maxVisibleLineNumber = (uint32_t)std::max<size_t>(maxVisibleLineNumber, i + 1);
    }
    if (m_VisualLines.empty())
    {
        m_VisualLines.push_back(VisualLineEntry{0, 0, 0});
        m_ActualToVisualLine[0] = 0;
        maxVisibleLineNumber = 1;
    }

    m_LineNumberWidthDigits = (int)std::to_string(maxVisibleLineNumber).size();
    m_VisualLineMapValid = true;
    m_LineMapFontId = fontId;
    m_LineMapPixelSize = pixelSize;
    m_LineMapWrapWidth = effectiveWrap;
    m_LineMapLineBox = lineBoxPx;
    m_LineMapWordBreak = static_cast<int>(wordBreak);
}

ScriptTextArea::ScriptTextArea()
    : TextArea()
{
}

int ScriptTextArea::HitTestVisibleCaret(float mouseX, float mouseY,
                                        float x, float y, float W,
                                        const ResolvedStyle& style) const
{
    TextAreaMetrics metrics{};
    if (!ComputeMetrics(style, W, metrics))
        return 0;

    const float padL = style.Layout.Padding.Left;
    const float padT = style.Layout.Padding.Top;
    const float borderL = style.Layout.BorderWidth.Left;
    const float borderT = style.Layout.BorderWidth.Top;

    const float localX = std::max(0.0f, mouseX - (x + padL + borderL));
    const float localY = std::max(0.0f, mouseY - (y + padT + borderT));

    const std::string& text = GetValue();
    EnsureVisualLineMapCache(text);
    if (m_VisualLines.empty())
        return 0;

    const int visualLine = std::clamp(static_cast<int>(std::floor(localY / metrics.LineAdvance)),
                                      0,
                                      static_cast<int>(m_VisualLines.size()) - 1);
    const auto& seg = m_VisualLines[visualLine];
    const size_t segByteStart = seg.ByteStart;
    const size_t segByteEnd = seg.ByteEndExclusive;

    std::string lineText(text.data() + segByteStart, segByteEnd - segByteStart);
    std::vector<float> xPositions;
    metrics.Font->BuildCaretMapUtf8(lineText, metrics.Px, xPositions, metrics.LetterSpacing);

    int best = static_cast<int>(segByteStart);
    for (size_t i = 0; i < xPositions.size(); ++i)
    {
        if (xPositions[i] >= localX)
        {
            best = static_cast<int>(segByteStart + i);
            break;
        }
        best = static_cast<int>(segByteStart + i);
    }
    if (!xPositions.empty() && localX > xPositions.back())
        best = static_cast<int>(segByteEnd);

    return best;
}

bool ScriptTextArea::GetLogicalLineByteRangeFromMouseY(float mouseY, float /*x*/, float y, float W,
                                                        const ResolvedStyle& style,
                                                        size_t& outLineStart,
                                                        size_t& outLineEndExclusive) const
{
    TextAreaMetrics metrics{};
    if (!ComputeMetrics(style, W, metrics))
        return false;

    const float padT = style.Layout.Padding.Top;
    const float borderT = style.Layout.BorderWidth.Top;
    const float localY = std::max(0.0f, mouseY - (y + padT + borderT));

    const std::string& text = GetValue();
    EnsureVisualLineMapCache(text);
    if (m_VisualLines.empty())
        return false;

    const int visualLine = std::clamp(static_cast<int>(std::floor(localY / metrics.LineAdvance)),
                                      0,
                                      static_cast<int>(m_VisualLines.size()) - 1);
    const auto& seg = m_VisualLines[visualLine];
    outLineStart = seg.ByteStart;
    outLineEndExclusive = seg.ByteEndExclusive;
    return true;
}

bool ScriptTextArea::IsPointerOverLineGlyphInk(float mouseX, float mouseY, float x, float y, float W,
                                               const ResolvedStyle& style) const
{
    TextAreaMetrics metrics{};
    if (!ComputeMetrics(style, W, metrics))
        return false;

    const float padL = style.Layout.Padding.Left;
    const float padT = style.Layout.Padding.Top;
    const float borderL = style.Layout.BorderWidth.Left;
    const float borderT = style.Layout.BorderWidth.Top;

    const float localX = std::max(0.0f, mouseX - (x + padL + borderL));
    const float localY = std::max(0.0f, mouseY - (y + padT + borderT));

    const std::string& text = GetValue();
    EnsureVisualLineMapCache(text);
    if (m_VisualLines.empty())
        return false;

    const int visualLine = std::clamp(static_cast<int>(std::floor(localY / metrics.LineAdvance)),
                                      0,
                                      static_cast<int>(m_VisualLines.size()) - 1);
    const auto& seg = m_VisualLines[visualLine];
    const size_t segByteStart = seg.ByteStart;
    const size_t segByteEnd = seg.ByteEndExclusive;

    const std::string lineText(text.data() + segByteStart, segByteEnd - segByteStart);
    if (lineText.empty())
        return false;

    std::vector<float> xPositions;
    metrics.Font->BuildCaretMapUtf8(lineText, metrics.Px, xPositions, metrics.LetterSpacing);
    if (xPositions.empty())
        return false;

    return localX <= xPositions.back();
}

void ScriptTextArea::SetShowLineNumbers(bool show)
{
    if (m_ShowLineNumbers == show)
        return;
    m_ShowLineNumbers = show;

    // CSS controls gutter sizing via this class.
    if (m_ShowLineNumbers)
        AddClass("show-line-numbers");
    else
        RemoveClass("show-line-numbers");

    // Fold markers depend on left gutter/padding.
    m_NeedsFoldMarkerUpdate = true;
    MarkDirty(StyleDirty | LayoutDirty | VisualDirty);

    // Ensure fold marker X positions update immediately after toggling (no need to scroll first).
    // We defer via PostAction so resolved style/padding reflects the new CSS class.
    PostAction([this]() {
        if (!GetOwnerManager())
            return;
        UpdateFoldMarkers();
        SyncFoldMarkerPositionsToScroll();
    });
}

void ScriptTextArea::UpdateHoverFromMouse(float mouseX, float mouseY)
{
    m_HasLastMouse = true;
    m_LastMouseX = mouseX;
    m_LastMouseY = mouseY;

    if (!m_ShowLineNumbers)
    {
        if (m_HoveredVisualLine != -1)
        {
            m_HoveredVisualLine = -1;
            MarkDirty(VisualDirty);
        }
        return;
    }

    auto* manager = GetOwnerManager();
    if (!manager)
        return;
    const auto& rs = GetResolvedStyle();

    constexpr float kBaseFoldGutterPx = 28.0f;
    const float kLineNumberGutterPx = std::max(0.0f, rs.Layout.Padding.Left - kBaseFoldGutterPx);

    const float localX = mouseX - GetLayoutX();
    const float localY = mouseY - GetLayoutY();

    if (localX < 0.0f || localX > kLineNumberGutterPx)
    {
        if (m_HoveredVisualLine != -1)
        {
            m_HoveredVisualLine = -1;
            MarkDirty(VisualDirty);
        }
        return;
    }

    TextAreaMetrics met{};
    if (!ComputeMetrics(met))
        return;

    const float lineH = met.LineAdvance;
    const float padT = rs.Layout.Padding.Top;

    const float contentY = (localY - padT);
    int nextHover = -1;
    if (contentY >= 0.0f)
        nextHover = (int)std::floor(contentY / lineH);

    if (nextHover != m_HoveredVisualLine)
    {
        m_HoveredVisualLine = nextHover;
        MarkDirty(VisualDirty);
    }
}

void ScriptTextArea::ClearHoverLine()
{
    m_HasLastMouse = false;
    if (m_HoveredVisualLine != -1)
    {
        m_HoveredVisualLine = -1;
        MarkDirty(VisualDirty);
    }
}

void ScriptTextArea::RefreshHoverFromLastMouse()
{
    if (!m_HasLastMouse)
        return;
    UpdateHoverFromMouse(m_LastMouseX, m_LastMouseY);
}

void ScriptTextArea::SetLanguage(Editor::ScriptLanguage language)
{
    if (m_Language == language)
        return;
    m_Language = language;
    InvalidateTokenCache();
    MarkDirty(VisualDirty);
}

void ScriptTextArea::SetValue(const std::string& value)
{
    TextArea::SetValue(value);

    InvalidateTokenCache();
    InvalidateLineCaches();
    m_FoldRegionsValid = false;
}

bool ScriptTextArea::OnChar(unsigned int codepoint)
{
    const bool consumed = TextArea::OnChar(codepoint);
    InvalidateTokenCache();
    InvalidateLineCaches();
    m_FoldRegionsValid = false; // Invalidate fold regions when text changes
    return consumed;
}

float ScriptTextArea::GetGutterWidth() const
{
    return GetResolvedStyle().Layout.Padding.Left;
}

float ScriptTextArea::GetLineAdvancePx() const
{
    TextAreaMetrics met;
    if (!ComputeMetrics(met) || met.LineAdvance <= 0.0f)
        return 14.0f; // fallback
    return met.LineAdvance;
}

size_t ScriptTextArea::GetVisualLineForBytePos(size_t bytePos, size_t* outSegStart) const
{
    const std::string& text = GetValue();
    EnsureVisualLineMapCache(text);
    for (size_t i = 0; i < m_VisualLines.size(); ++i)
    {
        const auto& seg = m_VisualLines[i];
        // Last segment of the file may have ByteEndExclusive == text.size() with no trailing newline
        if (bytePos >= seg.ByteStart && (bytePos < seg.ByteEndExclusive || i == m_VisualLines.size() - 1))
        {
            if (outSegStart) *outSegStart = seg.ByteStart;
            return i;
        }
    }
    // Fallback: last visual line
    if (!m_VisualLines.empty())
    {
        if (outSegStart) *outSegStart = m_VisualLines.back().ByteStart;
        return m_VisualLines.size() - 1;
    }
    if (outSegStart) *outSegStart = 0;
    return 0;
}

bool ScriptTextArea::GetBytePositionLocalRect(size_t bytePos, float& outX, float& outY, float& outHeight) const
{
    TextAreaMetrics met;
    if (!ComputeMetrics(met) || !met.Font || met.LineAdvance <= 0.0f)
        return false;

    const std::string& text = GetValue();
    const size_t pos = std::min(bytePos, text.size());

    size_t segStart = 0;
    const size_t visualLine = GetVisualLineForBytePos(pos, &segStart);

    size_t segEnd = text.size();
    if (visualLine < m_VisualLines.size())
        segEnd = m_VisualLines[visualLine].ByteEndExclusive;
    if (segEnd < segStart)
        segEnd = segStart;

    const std::string lineText = text.substr(segStart, segEnd - segStart);
    std::vector<float> xByByte;
    met.Font->BuildCaretMapUtf8(lineText, met.Px, xByByte, met.LetterSpacing);

    const size_t localByte = (pos >= segStart) ? (pos - segStart) : 0;
    const float glyphX = (localByte < xByByte.size())
                             ? xByByte[localByte]
                             : (xByByte.empty() ? 0.0f : xByByte.back());

    // Element-local, so the origin is the border box — the same padding+border
    // inset the glyph emitter and the hit-test apply.
    const ResolvedStyle& style = GetResolvedStyle();
    outX = style.Layout.Padding.Left + style.Layout.BorderWidth.Left + glyphX;
    outY = style.Layout.Padding.Top + style.Layout.BorderWidth.Top +
           static_cast<float>(visualLine) * met.LineAdvance;
    outHeight = met.LineAdvance;
    return true;
}

bool ScriptTextArea::EnsureBytePositionVisible(size_t bytePos)
{
    const std::string& text = GetValue();
    if (!m_FoldRegionsValid)
        DetectFoldRegions(text);

    std::vector<size_t> containing = FindFoldRegionsContainingPosition(bytePos);
    if (containing.empty())
        return false;

    // Expand outermost → innermost so parent regions open first
    for (size_t regionIndex : containing)
        ExpandFoldRegion(regionIndex);
    return true;
}

void ScriptTextArea::OnEvent(UIEvent& e)
{
    if (e.Id == kEventKeyDown && m_KeyFilter && m_KeyFilter(e))
    {
        e.Stop();
        return;
    }
    TextArea::OnEvent(e);
}

void ScriptTextArea::SetSearchHighlights(const std::vector<size_t>& matchPositions, size_t matchLength, int currentMatchIndex)
{
    m_SearchHighlightPositions = matchPositions;
    m_SearchHighlightLength = matchLength;
    m_CurrentSearchMatchIndex = currentMatchIndex;
    
    // Search results should always be visible, so expand folded regions that contain matches.
    if (!matchPositions.empty()) {
        // Ensure fold regions are detected
        const std::string& text = GetValue();
        if (!m_FoldRegionsValid) {
            DetectFoldRegions(text);
        }

        for (size_t matchPosition : matchPositions) {
            std::vector<size_t> containingRegions = FindFoldRegionsContainingPosition(matchPosition);

            // Expand the innermost region; ExpandFoldRegion also expands parent regions.
            if (!containingRegions.empty()) {
                ExpandFoldRegion(containingRegions.back());
            }
        }
    }
    
    MarkDirty(VisualDirty);
}

void ScriptTextArea::ClearSearchHighlights()
{
    m_SearchHighlightPositions.clear();
    m_SearchHighlightLength = 0;
    m_CurrentSearchMatchIndex = -1;
    MarkDirty(VisualDirty);
}

void ScriptTextArea::SetVariableHighlight(size_t position, size_t length)
{
    m_VariableHighlightPosition = position;
    m_VariableHighlightLength = length;
    MarkDirty(VisualDirty);
}

void ScriptTextArea::ClearVariableHighlight()
{
    m_VariableHighlightPosition = SIZE_MAX;
    m_VariableHighlightLength = 0;
    MarkDirty(VisualDirty);
}

void ScriptTextArea::SetDiagnosticHighlight(size_t position, size_t length)
{
    m_DiagnosticHighlightPosition = position;
    m_DiagnosticHighlightLength = length;
    MarkDirty(VisualDirty);
}

void ScriptTextArea::ClearDiagnosticHighlight()
{
    m_DiagnosticHighlightPosition = SIZE_MAX;
    m_DiagnosticHighlightLength = 0;
    MarkDirty(VisualDirty);
}

void ScriptTextArea::SyncFoldMarkerPositionsToScroll()
{
    if (m_UpdatingFoldMarkers)
        return;
    if (m_FoldMarkerElements.empty() || m_FoldRegions.empty())
        return;

    TextAreaMetrics met{};
    if (!ComputeMetrics(met))
        return;

    const auto& rs = GetResolvedStyle();
    float lineH = met.LineAdvance;
    float padT = rs.Layout.Padding.Top;
    constexpr float kFoldButtonW = 28.0f;
    constexpr int kFoldButtonWPx = 28;
    constexpr int kFoldButtonHPx = 16;

    const size_t n = std::min(m_FoldMarkerElements.size(), m_FoldRegions.size());
    for (size_t i = 0; i < n; ++i)
    {
        UIElement* buttonContainer = m_FoldMarkerElements[i];
        if (!buttonContainer)
            continue;

        const auto& region = m_FoldRegions[i];

        bool isInsideFoldedParent = IsRegionInsideFoldedParent(i);
        UI::Layout::SetElementHidden(*buttonContainer, isInsideFoldedParent);
        if (isInsideFoldedParent)
            continue;

        size_t visualLine = GetVisualLineNumber(region.StartLine);
        float lineY = padT + (visualLine * lineH);

        float markerX = std::max(0.0f, rs.Layout.Padding.Left - kFoldButtonW);
        float markerY = lineY + (lineH * 0.5f) - 8.0f;

        UI::Layout::SetAbsolutePosition(*buttonContainer,
                                        Mathematics::Rect{
                                            markerX,
                                            markerY,
                                            static_cast<float>(kFoldButtonWPx),
                                            static_cast<float>(kFoldButtonHPx),
                                        });
    }
}

void ScriptTextArea::OnPostLayout()
{
    TextArea::OnPostLayout();
    if (FindParentScrollView(this))
        RefreshContentHeightOverride();
}

void ScriptTextArea::RefreshContentHeightOverride()
{
    TextAreaMetrics metrics{};
    if (!ComputeMetrics(metrics))
        return;

    const auto& rs = GetResolvedStyle();
    const std::string& text = GetValue();
    EnsureVisualLineMapCache(text);

    // A height override is a BORDER-box height — that is the box Yoga sizes —
    // so the border has to be added alongside the padding, or the last line
    // lands outside the content box the renderer draws into.
    const float naturalHeight = static_cast<float>(std::max<size_t>(1, m_VisualLines.size())) * metrics.LineAdvance +
                                rs.Layout.Padding.Top + rs.Layout.Padding.Bottom +
                                rs.Layout.BorderWidth.Top + rs.Layout.BorderWidth.Bottom;

    // Allow scroll-past-end: keep the layout at least as tall as the current
    // scroll position plus one viewport. Without this, folding a region near
    // the bottom shrinks the natural content below `scrollY + viewportH` and
    // ScrollView clamps scrollY down to fill the viewport — which yanks the
    // text upward and moves the fold arrow out from under the cursor.
    float effectiveHeight = naturalHeight;
    if (ScrollView* sv = FindParentScrollView(this))
    {
        const float vph     = sv->GetViewportHeight();
        const float scrollY = sv->GetScrollY();
        effectiveHeight = std::max(effectiveHeight, scrollY + vph);
    }

    if (std::fabs(effectiveHeight - m_LastContentHeightOverride) > 0.5f)
    {
        m_LastContentHeightOverride = effectiveHeight;
        Overrides().Set(Style::Height, StyleLength::Px(effectiveHeight));
        MarkDirty(StyleDirty | LayoutDirty);
    }
}

void ScriptTextArea::OnPointerDown(float mouseX, float mouseY,
                                    float x, float y, float W, float H,
                                    const ResolvedStyle& style, Rendering::Text::FontAtlas* font)
{
    // Stamped here rather than left to the base: the font branch below does not
    // delegate, so a bump inherited from TextArea::OnPointerDown would be
    // skipped on every ordinary click. Bumping twice on the fallback path is
    // harmless — the deadline keeps the furthest of the two.
    BumpCaretForceVisible();
    if (!font)
    {
        TextArea::OnPointerDown(mouseX, mouseY, x, y, W, H, style, font);
        return;
    }

    const std::string& text = GetValue();
    const int oldCaret = m_CaretIndex;
    EnsureVisualLineMapCache(text);

    m_ClickDragAnchorX = mouseX;
    m_ClickDragAnchorY = mouseY;
    m_SuppressSmallDragSelection = false;

    const float localX = mouseX - x;
    const float localY = mouseY - y;
    const float padT = style.Layout.Padding.Top;
    const float padL = style.Layout.Padding.Left;
    const float borderT = style.Layout.BorderWidth.Top;
    const float contentY = localY - padT - borderT;

    const float borderL = style.Layout.BorderWidth.Left;
    const float textColumnStartX = padL + borderL;
    const bool inTextColumn =
        contentY >= 0.0f && localX >= textColumnStartX && !m_VisualLines.empty();

    bool hitFoldChevron = false;
    if (contentY >= 0.0f && !m_VisualLines.empty())
    {
        TextAreaMetrics metrics{};
        if (ComputeMetrics(style, W, metrics))
        {
            const int visualLine = std::clamp(static_cast<int>(std::floor(contentY / metrics.LineAdvance)),
                                              0,
                                              static_cast<int>(m_VisualLines.size()) - 1);
            const size_t actualLine = m_VisualLines[visualLine].ActualLine;
            for (size_t regionIndex = 0; regionIndex < m_FoldRegions.size(); ++regionIndex)
            {
                const FoldRegion& region = m_FoldRegions[regionIndex];
                if (region.StartLine != actualLine || IsRegionInsideFoldedParent(regionIndex))
                    continue;

                const float foldGutterX = std::max(0.0f, padL - kFoldGutterWidthPx);
                const float markerX = foldGutterX + static_cast<float>(GetFoldRegionNestingLevel(regionIndex)) * kFoldMarkerIndentPx;
                if (localX >= markerX && localX <= markerX + kFoldGutterWidthPx)
                {
                    hitFoldChevron = true;
                    break;
                }
                break;
            }
            if (hitFoldChevron)
            {
                ToggleFold(actualLine);
                const auto now = std::chrono::steady_clock::now();
                m_LastPointerClickTime = now;
                m_LastPointerClickX = mouseX;
                m_LastPointerClickY = mouseY;
                m_ConsecutiveClickCount = 0;
                return;
            }
        }
    }

    const bool inMultiClickZone = inTextColumn;

    const auto now = std::chrono::steady_clock::now();
    if (inMultiClickZone)
    {
        const auto elapsed = now - m_LastPointerClickTime;
        const bool sameSpot = std::fabs(mouseX - m_LastPointerClickX) <= kMultiClickTolerancePx &&
                              std::fabs(mouseY - m_LastPointerClickY) <= kMultiClickTolerancePx;
        if (sameSpot && elapsed < Platform::GetDoubleClickInterval() && m_ConsecutiveClickCount > 0)
            ++m_ConsecutiveClickCount;
        else
            m_ConsecutiveClickCount = 1;
    }
    else
        m_ConsecutiveClickCount = 0;

    m_LastPointerClickTime = now;
    m_LastPointerClickX = mouseX;
    m_LastPointerClickY = mouseY;

    if (m_ConsecutiveClickCount == 3 && inMultiClickZone)
    {
        const int caret = HitTestVisibleCaret(mouseX, mouseY, x, y, W, style);
        EnsureLineStartCache(text);
        const size_t lineIdx = GetLineIndexForBytePos(static_cast<size_t>(std::max(0, caret)));
        const size_t lineStart = (lineIdx < m_LineStarts.size()) ? m_LineStarts[lineIdx] : 0;
        size_t lineEnd = text.size();
        if (lineIdx + 1 < m_LineStarts.size())
            lineEnd = m_LineStarts[lineIdx + 1];
        const int lineStartI = static_cast<int>(lineStart);
        const int lineEndI = static_cast<int>(lineEnd);
        SetSelection(lineStartI, lineEndI);
        m_CaretIndex = (lineEndI > lineStartI) ? (lineEndI - 1) : lineStartI;
        m_ConsecutiveClickCount = 0;
        m_SuppressSmallDragSelection = true;
        MarkDirty(VisualDirty);
        return;
    }

    if (m_ConsecutiveClickCount == 2 && inMultiClickZone)
    {
        if (!IsPointerOverLineGlyphInk(mouseX, mouseY, x, y, W, style))
        {
            m_ConsecutiveClickCount = 1;
            m_CaretIndex = HitTestVisibleCaret(mouseX, mouseY, x, y, W, style);
            m_SelectionStart = m_SelectionEnd = m_CaretIndex;
            MarkDirty(VisualDirty);
            return;
        }
        size_t lineStart = 0;
        size_t lineEndEx = 0;
        const int caretRaw = HitTestVisibleCaret(mouseX, mouseY, x, y, W, style);
        int caretForWord = caretRaw;
        if (GetLogicalLineByteRangeFromMouseY(mouseY, x, y, W, style, lineStart, lineEndEx))
            caretForWord = ClampCaretIndexToLineByteRange(caretRaw, lineStart, lineEndEx);
        const auto wordBounds =
            FindWordBoundsAtByte(text, static_cast<size_t>(std::max(0, caretForWord)));
        SetSelection(static_cast<int>(wordBounds.first), static_cast<int>(wordBounds.second));
        m_SuppressSmallDragSelection = true;
        MarkDirty(VisualDirty);
        return;
    }

    const int click = HitTestVisibleCaret(mouseX, mouseY, x, y, W, style);
    int mods = 0;
    if (UIManager* owner = GetOwnerManager())
        mods = owner->GetModifierKeys();
    const bool shiftHeld = (mods & Input::kModShift) != 0;
    const bool primaryMod = Input::IsPrimaryShortcutModifier(mods);
    const bool altMod = (mods & Input::kModAlt) != 0;
    const bool addCursor = inTextColumn && !shiftHeld && (primaryMod || altMod);

    m_DragCursorIndex = -1;
    if (addCursor) {
        AddExtraCursor(click, -1);
        if (!m_ExtraCursors.empty() &&
            m_ExtraCursors.back().caret == click &&
            m_ExtraCursors.back().anchor < 0)
        {
            m_DragCursorIndex = (int)m_ExtraCursors.size() - 1;
        }
    } else if (inTextColumn && shiftHeld) {
        m_CaretIndex = click;
        ApplyShiftClickExtend(oldCaret, click);
    } else {
        m_CaretIndex = click;
        m_SelectionStart = m_SelectionEnd = m_CaretIndex;
        m_ExtraCursors.clear();
    }
    MarkDirty(VisualDirty);
}

void ScriptTextArea::OnPointerDrag(float mouseX, float mouseY,
                                   float x, float y, float W, float H,
                                   const ResolvedStyle& style, Rendering::Text::FontAtlas* font)
{
    // Same reason as OnPointerDown: the font branch below does not delegate.
    BumpCaretForceVisible();
    if (!font)
    {
        TextArea::OnPointerDrag(mouseX, mouseY, x, y, W, H, style, font);
        return;
    }

    const float dx = mouseX - m_ClickDragAnchorX;
    const float dy = mouseY - m_ClickDragAnchorY;
    const float dragDistSq = dx * dx + dy * dy;
    const float breakThreshSq =
        kMultiClickDragBreakThresholdPx * kMultiClickDragBreakThresholdPx;

    if (m_SuppressSmallDragSelection)
    {
        if (dragDistSq <= breakThreshSq)
            return;
        m_SuppressSmallDragSelection = false;
    }

    if (dragDistSq > breakThreshSq)
        m_ConsecutiveClickCount = 0;

    const int click = HitTestVisibleCaret(mouseX, mouseY, x, y, W, style);
    if (m_DragCursorIndex >= 0 && m_DragCursorIndex < (int)m_ExtraCursors.size()) {
        Cursor& c = m_ExtraCursors[m_DragCursorIndex];
        if (c.anchor < 0) c.anchor = c.caret;
        c.caret = click;
    } else {
        m_CaretIndex = click;
        m_SelectionEnd = m_CaretIndex;
    }

    const float padL = style.Layout.Padding.Left;
    m_NavPreferredX = std::max(0.0f, mouseX - (x + padL));
    EnsureCaretVisibleInScroll();
    MarkDirty(VisualDirty);
}

void ScriptTextArea::DetectFoldRegions(const std::string& text) const
{
    // Don't detect fold regions while we're updating markers (to avoid invalidating regions we're iterating over)
    ScriptTextArea* self = const_cast<ScriptTextArea*>(this);
    if (self->m_UpdatingFoldMarkers) {
        return;
    }

    // Fold regions affect visual line mapping.
    self->m_VisualLineMapValid = false;

    m_FoldRegions.clear();
    if (text.empty()) {
        m_FoldRegionsValid = true;
        return;
    }

    // Track brace depth and positions
    std::vector<std::pair<size_t, size_t>> openBraces; // position, line number
    size_t currentLine = 0;
    bool inString = false;
    char stringChar = 0;
    bool inSingleLineComment = false;
    bool inMultiLineComment = false;
    
    // Pre-calculate line starts for efficiency
    std::vector<size_t> lineStarts;
    lineStarts.push_back(0);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            lineStarts.push_back(i + 1);
        }
    }
    
    for (size_t i = 0; i < text.size(); ++i) {
        // Track line numbers
        if (i > 0 && text[i - 1] == '\n') {
            currentLine++;
            inSingleLineComment = false; // Reset single-line comment on new line
        }
        
        // Track strings and comments properly
        if (!inString && !inSingleLineComment && !inMultiLineComment) {
            // Check for string start
            if (text[i] == '"' || text[i] == '\'') {
                inString = true;
                stringChar = text[i];
            }
            // Check for single-line comment
            else if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '/') {
                inSingleLineComment = true;
                i++; // Skip next character
            }
            // Check for multi-line comment start
            else if (i + 1 < text.size() && text[i] == '/' && text[i + 1] == '*') {
                inMultiLineComment = true;
                i++; // Skip next character
            }
        } else if (inString) {
            // Check for string end (not escaped)
            if (text[i] == stringChar && (i == 0 || text[i - 1] != '\\')) {
                inString = false;
            }
        } else if (inMultiLineComment) {
            // Check for multi-line comment end
            if (i + 1 < text.size() && text[i] == '*' && text[i + 1] == '/') {
                inMultiLineComment = false;
                i++; // Skip next character
            }
        }
        
        // Skip braces inside strings or comments
        if (inString || inSingleLineComment || inMultiLineComment) {
            continue;
        }
        
        // Track opening braces
        if (text[i] == '{') {
            openBraces.push_back({i, currentLine});
        } else if (text[i] == '}') {
            if (!openBraces.empty()) {
                auto [openPos, startLine] = openBraces.back();
                openBraces.pop_back();
                
                // Find end line number
                size_t endLine = currentLine;
                
                // Only create fold region if it spans multiple lines
                if (endLine > startLine) {
                    FoldRegion region;
                    region.StartLine = startLine;
                    region.EndLine = endLine;
                    region.StartPos = openPos;
                    region.EndPos = i;
                    
                    // Calculate indent level for the start line
                    size_t lineStart = (startLine < lineStarts.size()) ? lineStarts[startLine] : 0;
                    int indent = 0;
                    for (size_t j = lineStart; j < text.size() && j < lineStart + 200; ++j) {
                        if (text[j] == ' ') indent++;
                        else if (text[j] == '\t') indent += 4;
                        else break;
                    }
                    region.IndentLevel = indent;
                    
                    m_FoldRegions.push_back(region);
                }
            }
        }
    }
    
    m_FoldRegionsValid = true;
    // Note: UpdateFoldMarkers() should be called after DetectFoldRegions() from a non-const context
}

void ScriptTextArea::UpdateFoldMarkers()
{
    m_NeedsFoldMarkerUpdate = false;
    MarkDirty(VisualDirty);
}

bool ScriptTextArea::IsLineFolded(size_t lineIndex) const
{
    return m_FoldedLines.find(lineIndex) != m_FoldedLines.end();
}

void ScriptTextArea::ToggleFold(size_t lineIndex)
{
    // Capture a scroll anchor on the fold-trigger line itself so the arrow the
    // user clicked stays pinned to the same screen Y across fold/unfold. With
    // scroll-past-end already enabled in RefreshContentHeightOverride, the
    // viewport never has to clamp away from this position — repeated toggles
    // hit the same arrow.
    bool haveAnchor = false;
    size_t anchorActualLine = lineIndex;
    float  anchorScreenY = 0.0f;
    if (ScrollView* sv = FindParentScrollView(this))
    {
        TextAreaMetrics metAnchor{};
        if (ComputeMetrics(metAnchor) && metAnchor.LineAdvance > 0.0f)
        {
            EnsureVisualLineMapCache(GetValue());
            if (!m_VisualLines.empty())
            {
                const float oldScrollY  = sv->GetScrollY();
                const size_t triggerVis = GetVisualLineNumber(lineIndex);
                anchorScreenY = static_cast<float>(triggerVis) * metAnchor.LineAdvance - oldScrollY;
                haveAnchor = true;
            }
        }
    }

    if (IsLineFolded(lineIndex)) {
        m_FoldedLines.erase(lineIndex);
    } else {
        m_FoldedLines.insert(lineIndex);
        if (!m_FoldRegionsValid) {
            DetectFoldRegions(GetValue());
        }
        if (const FoldRegion* region = GetFoldRegionForLine(lineIndex)) {
            // Move primary caret onto the fold header if it sat inside the
            // collapsed body — otherwise the caret would render inside hidden
            // text. Clamp the selection range the same way so multi-line
            // selections that ended inside the fold collapse to its header.
            auto clampInsideFold = [&](int& byteIdx) {
                if (byteIdx < 0) return;
                const size_t line = GetLineIndexForBytePos(static_cast<size_t>(byteIdx));
                if (line > region->StartLine && line <= region->EndLine)
                    byteIdx = static_cast<int>(region->StartPos);
            };
            clampInsideFold(m_CaretIndex);
            clampInsideFold(m_SelectionStart);
            clampInsideFold(m_SelectionEnd);
            for (auto& c : m_ExtraCursors) {
                clampInsideFold(c.caret);
                if (c.anchor >= 0) clampInsideFold(c.anchor);
            }
            // Drop any selection that collapsed to a single point inside the fold.
            if (m_SelectionStart == m_SelectionEnd) {
                m_SelectionStart = -1;
                m_SelectionEnd = -1;
            }
        }
    }
    m_VisualLineMapValid = false;
    MarkDirty(VisualDirty | LayoutDirty);

    // Push the new visible-content height to Yoga so the parent ScrollView
    // updates its scroll extent on the same frame as the fold toggle. Without
    // this, the textarea's measure callback (which only sees raw lines) would
    // keep reporting the unfolded height, leaving the scrollbar stuck.
    RefreshContentHeightOverride();

    // Restore the trigger-line anchor on the next pass, after Yoga has applied
    // the new height and ScrollView has refreshed its content extent. SetScrollY
    // clamps to the current max — scroll-past-end (above) keeps the desired
    // value within range so the trigger lands at the same screen Y as before.
    if (haveAnchor)
    {
        const size_t anchorLineCopy = anchorActualLine;
        const float  anchorScreenYCopy = anchorScreenY;
        PostAction([this, anchorLineCopy, anchorScreenYCopy]() {
            ScrollView* sv = FindParentScrollView(this);
            if (!sv) return;
            TextAreaMetrics met{};
            if (!ComputeMetrics(met) || met.LineAdvance <= 0.0f) return;
            EnsureVisualLineMapCache(GetValue());
            const size_t newTriggerVis = GetVisualLineNumber(anchorLineCopy);
            const float newScrollY = static_cast<float>(newTriggerVis) * met.LineAdvance - anchorScreenYCopy;
            sv->SetScrollY(std::max(0.0f, newScrollY));
        });
    }

    // Defer marker update to avoid destroying UI elements while their event handlers are executing
    // The actual update will happen in OnPostLayout
    m_NeedsFoldMarkerUpdate = true;
}

const ScriptTextArea::FoldRegion* ScriptTextArea::GetFoldRegionForLine(size_t lineIndex) const
{
    for (const auto& region : m_FoldRegions) {
        if (region.StartLine == lineIndex) {
            return &region;
        }
    }
    return nullptr;
}

size_t ScriptTextArea::GetVisualLineNumber(size_t actualLineNumber) const
{
    const std::string& text = GetValue();
    EnsureVisualLineMapCache(text);
    if (actualLineNumber < m_ActualToVisualLine.size())
        return m_ActualToVisualLine[actualLineNumber];
    return m_VisualLines.empty() ? 0 : m_VisualLines.size() - 1;
}

size_t ScriptTextArea::GetFoldRegionNestingLevel(size_t regionIndex) const
{
    if (regionIndex >= m_FoldRegions.size()) {
        return 0;
    }
    
    const auto& targetRegion = m_FoldRegions[regionIndex];
    size_t nestingLevel = 0;
    
    // Count how many other regions contain this region
    for (size_t i = 0; i < m_FoldRegions.size(); ++i) {
        if (i == regionIndex) continue;
        
        const auto& otherRegion = m_FoldRegions[i];
        // Check if otherRegion contains targetRegion
        if (otherRegion.StartLine < targetRegion.StartLine && otherRegion.EndLine > targetRegion.EndLine) {
            nestingLevel++;
        }
    }
    
    return nestingLevel;
}

bool ScriptTextArea::IsRegionInsideFoldedParent(size_t regionIndex) const
{
    if (regionIndex >= m_FoldRegions.size()) {
        return false;
    }
    
    const auto& targetRegion = m_FoldRegions[regionIndex];
    
    // Check if any parent region (one that contains this region) is folded
    for (size_t i = 0; i < m_FoldRegions.size(); ++i) {
        if (i == regionIndex) continue;
        
        const auto& otherRegion = m_FoldRegions[i];
        // Check if otherRegion is a parent of targetRegion (contains it)
        if (otherRegion.StartLine < targetRegion.StartLine && otherRegion.EndLine > targetRegion.EndLine) {
            // Check if this parent region is folded
            if (IsLineFolded(otherRegion.StartLine)) {
                return true;
            }
        }
    }
    return false;
}

std::vector<size_t> ScriptTextArea::FindFoldRegionsContainingPosition(size_t bytePosition) const
{
    std::vector<size_t> containingRegions;
    
    for (size_t i = 0; i < m_FoldRegions.size(); ++i) {
        const auto& region = m_FoldRegions[i];
        // Check if the position is inside this region (between startPos and endPos, exclusive of startPos)
        if (bytePosition > region.StartPos && bytePosition < region.EndPos) {
            containingRegions.push_back(i);
        }
    }
    
    // Sort by nesting level (outermost first)
    std::sort(containingRegions.begin(), containingRegions.end(), 
        [this](size_t a, size_t b) {
            return GetFoldRegionNestingLevel(a) < GetFoldRegionNestingLevel(b);
        });
    
    return containingRegions;
}

void ScriptTextArea::ExpandFoldRegion(size_t regionIndex)
{
    if (regionIndex >= m_FoldRegions.size()) {
        return;
    }
    
    const auto& targetRegion = m_FoldRegions[regionIndex];
    
    // First, expand all parent regions (outermost first)
    for (size_t i = 0; i < m_FoldRegions.size(); ++i) {
        if (i == regionIndex) continue;
        
        const auto& otherRegion = m_FoldRegions[i];
        // Check if otherRegion is a parent of targetRegion
        if (otherRegion.StartLine < targetRegion.StartLine && otherRegion.EndLine > targetRegion.EndLine) {
            // This is a parent region - expand it if it's folded
            if (IsLineFolded(otherRegion.StartLine)) {
                m_FoldedLines.erase(otherRegion.StartLine);
            }
        }
    }
    
    // Finally, expand the target region itself if it's folded
    if (IsLineFolded(targetRegion.StartLine)) {
        m_FoldedLines.erase(targetRegion.StartLine);
    }
    
    MarkDirty(VisualDirty | LayoutDirty);
    UpdateFoldMarkers();
}

// ---------------------------------------------------------------------------
// Syntax-colored text rendering (visible lines only)
// ---------------------------------------------------------------------------

void ScriptTextArea::EmitTextGlyphs(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& /*style*/,
                                     float originX, float originY, const TextAreaMetrics& met)
{
    using namespace UI;
    using namespace Rendering::Text;

    const std::string& text = GetValue();
    if (text.empty())
        return;

    EnsureLineStartCache(text);
    EnsureVisualLineMapCache(text);

    if (!m_TokenCacheValid || m_LastTokenizedText != text) {
        Editor::TokenizeScript(m_Language, text, m_TokenCache);
        m_LastTokenizedText = text;
        m_TokenCacheValid = true;
    }

    const auto& visualLines = m_VisualLines;
    const int totalVisualLines = static_cast<int>(visualLines.size());

    int firstLine = 0;
    int lastLine = totalVisualLines - 1;
    ScrollView* sv = FindParentScrollView(this);
    if (sv) {
        const float viewportH = sv->GetViewportHeight();
        if (viewportH > 0.0f && met.LineAdvance > 0.0f) {
            // Virtualize against the textarea's own position within the scroll
            // viewport, not raw scrollY — the textarea may sit below other
            // elements inside the scroll content (path/type labels in the
            // script inspector), so scrollY alone would skip too many lines
            // and leave a gap where the visible lines belong.
            const float localTop = sv->GetLayoutY() - GetLayoutY();
            firstLine = std::max(0, (int)std::floor(localTop / met.LineAdvance));
            lastLine = std::min(totalVisualLines - 1,
                                (int)std::ceil((localTop + viewportH) / met.LineAdvance));
        }
    }

    auto lm = met.Font->GetLineMetrics(met.Px);
    const float lineH = std::max(1.0f, lm.height);
    // Clamped at zero: the script editor steps a fixed LineAdvance per line, so
    // a line box tighter than the font would lift line 1 out of the top of the
    // content box while every later line stayed where the advance put it.
    const float glyphYOffset =
        std::max(0.0f, Rendering::Text::TextLayout::HalfLeadingPx(lineH, met.LineAdvance));

    const auto& settings = SyntaxHighlightSettings::Get();
    const uint32_t defaultPacked = PackFromARGB(settings.DefaultColor);

    using Editor::SyntaxTokenType;
    auto packedTokenColor = [&](SyntaxTokenType type) -> uint32_t {
        switch (type) {
        case SyntaxTokenType::Keyword:      return PackFromARGB(settings.KeywordColor);
        case SyntaxTokenType::String:       return PackFromARGB(settings.StringColor);
        case SyntaxTokenType::Comment:      return PackFromARGB(settings.CommentColor);
        case SyntaxTokenType::Number:       return PackFromARGB(settings.NumberColor);
        case SyntaxTokenType::Type:         return PackFromARGB(settings.TypeColor);
        // `#version`-style lines read as directives; `@texture` tags inside the
        // surface-shader header comments read as declarations, not prose.
        case SyntaxTokenType::Preprocessor: return PackFromARGB(settings.KeywordColor);
        case SyntaxTokenType::Annotation:   return PackFromARGB(settings.TypeColor);
        default:                            return defaultPacked;
        }
    };

    static const std::vector<UI::UITextureRegistry::SlugTextureIndices> kEmptySlugPagesText;
    const auto& slugPages = (ctx.Textures && met.Font)
        ? ctx.Textures->RegisterSlugTextures(*met.Font)
        : kEmptySlugPagesText;

    const UI::GlyphRunTarget runTarget = UI::MakeGlyphRunTarget(ctx, met.Font, slugPages);

    // Every token run on a visual line shares baseY and the ascender, so each
    // run resolves the same snap delta and the line stays rigid.
    auto emitShapedGlyphs = [&](FontAtlas::ShapeResult& result, float baseX, float baseY) {
        UI::EmitGlyphRun(result.glyphs, baseX, baseY, lm.ascender, runTarget);
    };

    static thread_local FontAtlas::ShapeResult scratchResult;

    for (int visLineIdx = firstLine; visLineIdx <= lastLine; ++visLineIdx) {
        if (visLineIdx < 0 || visLineIdx >= totalVisualLines) continue;

        const auto& seg = visualLines[visLineIdx];
        size_t lineByteStart = seg.ByteStart;
        size_t lineByteEnd = seg.ByteEndExclusive;

        float lineY = originY + visLineIdx * met.LineAdvance + glyphYOffset;
        float penX = 0.0f;

        // Binary search for first token overlapping this line
        size_t tokenStart = 0;
        {
            size_t lo = 0, hi = m_TokenCache.size();
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (m_TokenCache[mid].Start + m_TokenCache[mid].Length <= lineByteStart)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            tokenStart = lo;
        }

        size_t cursor = lineByteStart;
        for (size_t ti = tokenStart; ti < m_TokenCache.size(); ++ti) {
            const auto& tok = m_TokenCache[ti];
            if (tok.Start >= lineByteEnd)
                break;

            size_t visStart = std::max(tok.Start, lineByteStart);
            size_t visEnd = std::min(tok.Start + tok.Length, lineByteEnd);
            if (visStart >= visEnd)
                continue;

            if (cursor < visStart) {
                std::string_view gapView{text.data() + cursor, visStart - cursor};
                met.Font->ShapeText(gapView, met.Px, scratchResult, defaultPacked, met.LetterSpacing);
                emitShapedGlyphs(scratchResult, originX + penX, lineY);
                penX += scratchResult.metrics.width;
            }

            uint32_t tokColor = packedTokenColor(tok.Type);
            std::string_view tokView{text.data() + visStart, visEnd - visStart};
            met.Font->ShapeText(tokView, met.Px, scratchResult, tokColor, met.LetterSpacing);
            emitShapedGlyphs(scratchResult, originX + penX, lineY);
            penX += scratchResult.metrics.width;
            cursor = visEnd;
        }

        if (cursor < lineByteEnd) {
            std::string_view gapView{text.data() + cursor, lineByteEnd - cursor};
            met.Font->ShapeText(gapView, met.Px, scratchResult, defaultPacked, met.LetterSpacing);
            emitShapedGlyphs(scratchResult, originX + penX, lineY);
        }
    }
}

// ---------------------------------------------------------------------------
// Primitive-based rendering: search + variable highlight overlays
// ---------------------------------------------------------------------------

void ScriptTextArea::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                                          const ResolvedStyle& style,
                                          float x, float y, float w, float /*h*/)
{
    // Parallel-drain thread contract: custom emission may touch shared
    // text/measure state, so it never runs on a JobSystem worker — escalate
    // and let the drain re-emit this element on the UI thread.
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }

    using namespace UI;

    if (m_UpdatingFoldMarkers)
        return;

    bool hasSearch     = !m_SearchHighlightPositions.empty() && m_SearchHighlightLength > 0;
    bool hasVariable   = m_VariableHighlightPosition != SIZE_MAX && m_VariableHighlightLength > 0;
    bool hasDiagnostic = m_DiagnosticHighlightPosition != SIZE_MAX && m_DiagnosticHighlightLength > 0;

    if (hasSearch || hasVariable || hasDiagnostic)
    {
        TextAreaMetrics met{};
        if (ComputeMetrics(style, w, met))
        {
            Rendering::Text::FontAtlas* font = met.Font;
            const float px = met.Px;
            const float padL = style.Layout.Padding.Left;
            const float padT = style.Layout.Padding.Top;
            float renderOriginX = x + padL;
            float renderOriginY = y + padT;
            float lineH = met.LineAdvance;

            const std::string& text = GetValue();
            EnsureLineStartCache(text);

            auto isPositionInFoldedRegion = [this](size_t pos) -> bool {
                for (const auto& region : m_FoldRegions)
                {
                    if (IsLineFolded(region.StartLine) &&
                        pos > region.StartPos && pos <= region.EndPos)
                        return true;
                }
                return false;
            };

            uint32_t accentArgb = style.Visual.BorderColor.Top;
            if ((accentArgb >> 24) == 0)
                accentArgb = 0xFF3A8FFF;
            uint32_t fillPacked = PackFromARGB((accentArgb & 0x00FFFFFFu) | 0x33000000u);
            uint32_t borderPacked = PackFromARGB(accentArgb);
            const float cs = ctx.ContentScale > 0.0f ? ctx.ContentScale : 1.0f;
            const float kPad = 4.f * cs;
            const float kBorderR = 4.f * cs;

            auto emitHighlightRect = [&](size_t matchStart, size_t matchLen, uint32_t fill, uint32_t border)
            {
                size_t matchEnd = matchStart + matchLen;
                if (matchStart >= text.size()) return;
                if (matchEnd > text.size()) matchEnd = text.size();
                if (isPositionInFoldedRegion(matchStart)) return;

                size_t segStart = 0;
                size_t visualLine = GetVisualLineForBytePos(matchStart, &segStart);

                std::string linePrefix = text.substr(segStart, matchStart - segStart);
                std::vector<float> xPositions;
                font->BuildCaretMapUtf8(linePrefix, px, xPositions, met.LetterSpacing);
                float startX = xPositions.empty() ? 0.0f : xPositions.back();

                std::string matchText = text.substr(matchStart, matchEnd - matchStart);
                auto meas = font->MeasureUtf8(matchText, px, met.LetterSpacing);

                float hlX = renderOriginX + startX - kPad;
                float hlY = renderOriginY + static_cast<float>(visualLine) * lineH - kPad * 0.5f;
                float hlW = meas.width + kPad * 2.f;
                float hlH = lineH + kPad;

                UIPrimitive p = MakeRect(hlX, hlY, hlW, hlH, fill,
                                         kBorderR, kBorderR, kBorderR, kBorderR);
                AddBorder(p, 1.f * cs, border);
                ctx.Emit(p);
            };

            if (hasSearch)
            {
                for (size_t pos : m_SearchHighlightPositions)
                    emitHighlightRect(pos, m_SearchHighlightLength, fillPacked, borderPacked);
            }

            if (hasVariable)
                emitHighlightRect(m_VariableHighlightPosition, m_VariableHighlightLength, fillPacked, borderPacked);

            if (hasDiagnostic)
            {
                constexpr uint32_t kDiagnosticAccentArgb = 0xFFFF9F1Au;
                const uint32_t diagnosticFill = PackFromARGB((kDiagnosticAccentArgb & 0x00FFFFFFu) | 0x4D000000u);
                const uint32_t diagnosticBorder = PackFromARGB(kDiagnosticAccentArgb);
                emitHighlightRect(m_DiagnosticHighlightPosition, m_DiagnosticHighlightLength, diagnosticFill, diagnosticBorder);
            }
        }
    }

    // Replicate the base TextArea::OnGeneratePrimitives, but with fold-aware
    // selection and caret rendering. The base draws selection rects using
    // wrap-line indices (`tc.Breaks`), which don't match the visible-line
    // geometry once fold regions hide rows above the selection. Doing the
    // overlay here lets us share m_VisualLines with the text glyph emitter.
    {
        TextAreaMetrics met{};
        if (ComputeMetrics(style, w, met))
        {
            using namespace UI;

            const float cs = ctx.ContentScale;
            const float padL_phys = style.Layout.Padding.Left * cs;
            const float padT_phys = style.Layout.Padding.Top * cs;
            const float borderL_phys = style.Layout.BorderWidth.Left * cs;
            const float borderT_phys = style.Layout.BorderWidth.Top * cs;
            const float originX = x + padL_phys + borderL_phys;
            const float originY = y + padT_phys + borderT_phys;

            // Paint order is fixed by CSS: a selection highlight is a
            // background and goes UNDER the glyphs; the caret goes over them.
            // Emission order is composite order, so the sequence has to be
            // selection -> glyphs -> carets.
            bool glyphsEmitted = false;

            if (auto* manager = GetOwnerManager())
            {
                const std::string& textRef = GetValue();
                EnsureVisualLineMapCache(textRef);

                const bool isFocused = IsFocusTargetForId(manager->GetFocusedElementId());
                const int textLen = static_cast<int>(textRef.size());

                std::vector<Cursor> allCursors;
                allCursors.reserve(m_ExtraCursors.size() + 1);
                allCursors.push_back(PrimaryAsCursor());
                for (const auto& c : m_ExtraCursors) allCursors.push_back(c);

                bool anyHasSelection = false;
                for (const auto& c : allCursors)
                {
                    if (c.HasSelection()) { anyHasSelection = true; break; }
                }

                const bool needOverlays =
                    (isFocused || anyHasSelection) &&
                    !(isFocused && !anyHasSelection && m_ReadOnly && m_ExtraCursors.empty());

                if (needOverlays && !m_VisualLines.empty())
                {
                    auto isActualLineHidden = [this](size_t actualLine) -> bool {
                        for (const auto& region : m_FoldRegions)
                        {
                            if (IsLineFolded(region.StartLine) &&
                                actualLine > region.StartLine && actualLine <= region.EndLine)
                                return true;
                        }
                        return false;
                    };

                    auto byteToVisualLine = [this, &isActualLineHidden](size_t byteIdx) -> int {
                        const size_t actualLine = GetLineIndexForBytePos(byteIdx);
                        if (isActualLineHidden(actualLine))
                            return -1;
                        return static_cast<int>(GetVisualLineNumber(actualLine));
                    };

                    auto xForByteOnLine = [&](size_t byteIdx, int visualLine) -> float {
                        if (visualLine < 0 || visualLine >= static_cast<int>(m_VisualLines.size()))
                            return 0.0f;
                        const auto& vl = m_VisualLines[visualLine];
                        if (byteIdx <= vl.ByteStart) return 0.0f;
                        const size_t clamped = std::min(static_cast<size_t>(byteIdx), static_cast<size_t>(vl.ByteEndExclusive));
                        const size_t len = clamped - vl.ByteStart;
                        std::string_view prefix(textRef.data() + vl.ByteStart, len);
                        static thread_local std::vector<float> caretMap;
                        std::string prefixCopy(prefix);
                        met.Font->BuildCaretMapUtf8(prefixCopy, met.Px, caretMap, met.LetterSpacing);
                        return caretMap.empty() ? 0.0f : caretMap.back();
                    };

                    // Selection rects, fold-aware.
                    if (anyHasSelection)
                    {
                        const uint32_t packedSelColor = UI::PackedTextSelectionFill(style.Visual, isFocused);
                        const float fontSize = std::max(1.0f, style.Visual.FontSize);

                        for (const auto& c : allCursors)
                        {
                            if (!c.HasSelection()) continue;
                            const int a = std::clamp(c.SelStart(), 0, textLen);
                            const int b = std::clamp(c.SelEnd(), 0, textLen);
                            if (a >= b) continue;

                            const size_t actualLineA = GetLineIndexForBytePos(static_cast<size_t>(a));
                            const size_t actualLineB = GetLineIndexForBytePos(static_cast<size_t>(b));

                            for (size_t actualLine = actualLineA; actualLine <= actualLineB; ++actualLine)
                            {
                                if (isActualLineHidden(actualLine))
                                    continue;
                                const int visualLine = static_cast<int>(GetVisualLineNumber(actualLine));

                                const size_t lineStart = (actualLine < m_LineStarts.size())
                                                            ? m_LineStarts[actualLine]
                                                            : 0;
                                size_t lineEnd = (actualLine + 1 < m_LineStarts.size())
                                                    ? m_LineStarts[actualLine + 1]
                                                    : textRef.size();
                                if (lineEnd > lineStart && textRef[lineEnd - 1] == '\n') --lineEnd;
                                if (lineEnd > lineStart && textRef[lineEnd - 1] == '\r') --lineEnd;

                                const size_t startByte = std::max(static_cast<size_t>(a), lineStart);
                                const size_t endByte   = std::min(static_cast<size_t>(b), lineEnd);

                                const float startX = xForByteOnLine(startByte, visualLine);
                                const float endX   = xForByteOnLine(endByte,   visualLine);
                                float selW = std::max(0.0f, endX - startX);
                                if (selW <= 0.0f && actualLine != actualLineB)
                                    selW = fontSize * 0.5f;

                                if (selW > 0.0f)
                                {
                                    const float ry = originY + static_cast<float>(visualLine) * met.LineAdvance;
                                    UIPrimitive prim = MakeRect(originX + startX, ry, selW, met.LineAdvance, packedSelColor);
                                    ctx.Emit(prim);
                                }
                            }
                        }
                    }

                    EmitTextGlyphs(ctx, style, originX, originY, met);
                    glyphsEmitted = true;

                    // Carets, fold-aware. A caret that landed inside a folded
                    // body is suppressed (ToggleFold also moves the primary
                    // caret to the fold header, so this only matters for
                    // multi-cursor edits or selections collapsed into a fold).
                    if (isFocused && !m_ReadOnly)
                    {
                        constexpr float kCaretWidth = 1.5f;
                        const uint32_t caretColor = PackFromARGB(style.Visual.Color);

                        for (const auto& c : allCursors)
                        {
                            const int clamped = std::clamp(c.caret, 0, textLen);
                            const int caretVisualLine = byteToVisualLine(static_cast<size_t>(clamped));
                            if (caretVisualLine < 0) continue;

                            const float caretX = originX + xForByteOnLine(static_cast<size_t>(clamped), caretVisualLine);
                            const float caretY = originY + static_cast<float>(caretVisualLine) * met.LineAdvance;

                            UIPrimitive prim{};
                            prim.X = caretX;
                            prim.Y = caretY;
                            prim.W = kCaretWidth;
                            prim.H = met.LineAdvance;
                            prim.FillColor = caretColor;
                            prim.Opacity = style.Visual.Opacity;
                            // Read, never stamped — the deadline belongs to the
                            // input that moved the caret. See TextArea.
                            prim.CaretTime = CaretForceVisibleUntil();
                            prim.ModeAndFlags = MakeFlags(PrimitiveMode::Rect, GradientMode::None,
                                                          ctx.ClipIndex, false, true);
                            ctx.Primitives.push_back(prim);
                        }
                    }
                }
            }

            if (!glyphsEmitted)
                EmitTextGlyphs(ctx, style, originX, originY, met);
        }
    }

    TextAreaMetrics metrics{};
    if (!ComputeMetrics(style, w, metrics))
        return;

    const std::string& text = GetValue();
    EnsureVisualLineMapCache(text);
    if (m_VisualLines.empty())
        return;

    const float padL = style.Layout.Padding.Left;
    const float padT = style.Layout.Padding.Top;
    const float borderL = style.Layout.BorderWidth.Left;
    const float borderT = style.Layout.BorderWidth.Top;
    const float lineNumberGutterWidth = m_ShowLineNumbers ? std::max(0.0f, padL - kFoldGutterWidthPx) : 0.0f;

    int firstLine = 0;
    int lastLine = static_cast<int>(m_VisualLines.size()) - 1;
    ScrollView* sv = FindParentScrollView(this);
    if (sv && metrics.LineAdvance > 0.0f)
    {
        const float viewportH = sv->GetViewportHeight();
        if (viewportH > 0.0f)
        {
            const float localTop = sv->GetLayoutY() - GetLayoutY();
            firstLine = std::max(0, static_cast<int>(std::floor(localTop / metrics.LineAdvance)));
            lastLine = std::min(static_cast<int>(m_VisualLines.size()) - 1,
                                static_cast<int>(std::ceil((localTop + viewportH) / metrics.LineAdvance)));
        }
    }

    const auto lm = metrics.Font->GetLineMetrics(metrics.Px);
    const float lineH = std::max(1.0f, lm.height);
    // Clamped at zero: the script editor steps a fixed LineAdvance per line, so
    // a line box tighter than the font would lift line 1 out of the top of the
    // content box while every later line stayed where the advance put it.
    const float glyphYOffset =
        std::max(0.0f, Rendering::Text::TextLayout::HalfLeadingPx(lineH, metrics.LineAdvance));
    const float foldGutterX = x + borderL + lineNumberGutterWidth;

    static const std::vector<UI::UITextureRegistry::SlugTextureIndices> kEmptySlugPagesFold;
    const auto& slugPagesFold = (ctx.Textures && metrics.Font)
        ? ctx.Textures->RegisterSlugTextures(*metrics.Font)
        : kEmptySlugPagesFold;

    const UI::GlyphRunTarget foldRunTarget =
        UI::MakeGlyphRunTarget(ctx, metrics.Font, slugPagesFold);

    auto emitShapedText = [&](std::string_view content, float baseX, float baseY, uint32_t color) {
        static thread_local Rendering::Text::FontAtlas::ShapeResult scratchResult;
        metrics.Font->ShapeText(content, metrics.Px, scratchResult, color, metrics.LetterSpacing);
        UI::EmitGlyphRun(scratchResult.glyphs, baseX, baseY, lm.ascender, foldRunTarget);
    };

    int caretLine = -1;
    if (auto* manager = GetOwnerManager())
    {
        if (IsFocusTargetForId(manager->GetFocusedElementId()))
            caretLine = static_cast<int>(GetLineIndexForBytePos(static_cast<size_t>(std::max(0, m_CaretIndex))));
    }

    if (m_ShowLineNumbers)
    {
        for (int visualLine = firstLine; visualLine <= lastLine; ++visualLine)
        {
            if (visualLine < 0 || visualLine >= static_cast<int>(m_VisualLines.size()))
                continue;

            const auto& seg = m_VisualLines[visualLine];
            const size_t actualLine = seg.ActualLine;

            // Only draw the line number on the first visual segment of each actual line
            // (subsequent segments are soft-wrap continuations and leave the gutter blank).
            const size_t firstSegByteStart = (actualLine < m_LineStarts.size()) ? m_LineStarts[actualLine] : 0;
            if (static_cast<size_t>(seg.ByteStart) != firstSegByteStart)
                continue;

            const std::string lineNumberText = std::to_string(actualLine + 1);
            const auto measure =
                metrics.Font->MeasureUtf8(lineNumberText, metrics.Px, metrics.LetterSpacing);

            uint32_t lineNumberColor = PackFromARGB(kLineNumberColorArgb);
            if (static_cast<int>(actualLine) == caretLine)
                lineNumberColor = PackFromARGB(kCaretLineNumberColorArgb);
            if (visualLine == m_HoveredVisualLine)
                lineNumberColor = PackFromARGB(kActiveLineNumberColorArgb);

            const float numberX = x + borderL + lineNumberGutterWidth - kLineNumberRightPaddingPx - measure.width;
            const float lineY = y + padT + borderT + static_cast<float>(visualLine) * metrics.LineAdvance + glyphYOffset;
            emitShapedText(lineNumberText, numberX, lineY, lineNumberColor);
        }
    }

    for (size_t regionIndex = 0; regionIndex < m_FoldRegions.size(); ++regionIndex)
    {
        if (IsRegionInsideFoldedParent(regionIndex))
            continue;

        const FoldRegion& region = m_FoldRegions[regionIndex];
        const size_t visualLine = GetVisualLineNumber(region.StartLine);
        if (visualLine < static_cast<size_t>(firstLine) || visualLine > static_cast<size_t>(lastLine))
            continue;

        // Cell occupied by the chevron in the fold gutter. Inset matches the
        // hit-test region in OnPointerDown so click feedback aligns with the icon.
        const float cellW = 12.0f;
        const float cellH = std::max(metrics.LineAdvance, 12.0f);
        const float cellX = foldGutterX +
                            static_cast<float>(GetFoldRegionNestingLevel(regionIndex)) * kFoldMarkerIndentPx +
                            kFoldMarkerInsetPx;
        const float cellY = y + padT + borderT + static_cast<float>(visualLine) * metrics.LineAdvance;

        // Triangle dimensions — sized like the editor's font height so the
        // chevron reads clearly without dominating the gutter. Slightly inset
        // from the cell box.
        const float triSide = std::min(8.0f, std::min(cellW - 2.0f, cellH - 4.0f));
        const float halfBase = triSide * 0.5f;
        const float midX = cellX + cellW * 0.5f;
        const float midY = cellY + cellH * 0.5f;
        const uint32_t foldIconColor = PackFromARGB(kFoldMarkerColorArgb);

        if (IsLineFolded(region.StartLine))
        {
            // Right-pointing chevron (block is collapsed): apex on the right,
            // base flush to the left.
            const float apexX = midX + triSide * 0.5f;
            const float baseX = midX - triSide * 0.5f;
            ctx.Emit(UI::MakeTriangle(baseX, midY - halfBase,
                                      baseX, midY + halfBase,
                                      apexX, midY,
                                      foldIconColor, 0.5f));
        }
        else
        {
            // Down-pointing chevron (block is expanded): apex on the bottom.
            const float apexY = midY + triSide * 0.5f;
            const float baseY = midY - triSide * 0.5f;
            ctx.Emit(UI::MakeTriangle(midX - halfBase, baseY,
                                      midX + halfBase, baseY,
                                      midX,            apexY,
                                      foldIconColor, 0.5f));
        }
    }
}

} // namespace GameEngine
