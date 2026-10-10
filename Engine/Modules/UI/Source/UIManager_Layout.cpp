#include "UI/UIManager.h"
#include "UIManager_Internal.h"

#include "UI/Controls/Mount.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/WeightedPane.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Layout/YogaLayout.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/ITextMeasurable.h"
#include "UI/UIElement.h"
#include "UI/Internal/LayoutAccess.h"

#include "UI/UIEvents.h"
#include "UI/UIStyle.h"

#include "Core/CpuProfiler.h"
#include "Logger/Logger.h"
#include "Rendering/Text/FontAtlas.h"
#include "Rendering/Text/TextLayout.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string_view>

using namespace GameEngine::UIParsing;

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
#include <yoga/Yoga.h>
#endif

using namespace GameEngine;
using namespace GameEngine::Rendering::Text;
using namespace GameEngine::UILayout;

// TextMeasureCtx moved to UIManager.h as a public nested type; local alias
// keeps call-site brevity.
using TextMeasureCtx = GameEngine::UIManager::TextMeasureCtx;

// ---------------------------------------------------------------------------
// Yoga text measure callback
// ---------------------------------------------------------------------------

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
// Measurement + single-slot cache fill for a node with a resolved atlas. Shapes
// with the atlas's primary face. outShaped (optional) reports whether
// measurement work actually ran — false when the ctx's single-slot cache served
// the result.
YGSize MeasureTextWithAtlas(TextMeasureCtx* ctx, float wrapWidth,
                            bool* outShaped = nullptr);

YGSize MeasureTextFn(YGNodeConstRef node,
                     float width, YGMeasureMode widthMode,
                     float /*height*/, YGMeasureMode /*heightMode*/)
{
    auto* ctx = reinterpret_cast<TextMeasureCtx*>(YGNodeGetContext(node));
    if (!ctx)
        return YGSize{0, 0};

    // Yoga operates in logical (CSS) pixels; the font atlas works in physical pixels.
    // Scale the wrap-width constraint to physical before measuring, then divide all
    // returned dimensions back to logical so Yoga sees consistent units.
    const float scale    = ctx->ContentScale > 0.0f ? ctx->ContentScale : 1.0f;
    const float invScale = 1.0f / scale;

    float wrapWidth = 0.0f;
    if (ctx->AllowWrap && (widthMode == YGMeasureModeAtMost || widthMode == YGMeasureModeExactly))
    {
        wrapWidth = std::max(0.0f, width * scale); // logical → physical
    }

    // Record the solve-time constraint so the parallel pre-measure pass can
    // predict it next time (exact for fixed-width virtualized rows and any
    // steady-state element).
    ctx->LastWrapWidthPhysical = wrapWidth;
    ctx->HasMeasured = true;

    // If the font atlas is not yet available (e.g. host is resolving font bytes asynchronously),
    // return a conservative fallback size so Yoga layout doesn't collapse to 0x0 and cause
    // overlap/stacking. Once the atlas arrives, UIManager invalidates geometry and the next
    // frame will measure with real font metrics.
    if (!ctx->Atlas)
    {
        // No atlas yet, so no metrics: approximate "normal" as 1.2em. A
        // resolved line-height still lands exactly.
        const float resolvedBox =
            Rendering::Text::TextLayout::ResolveLineBoxPx(ctx->LineHeight, ctx->FontSize, scale);

        // Count lines and approximate the max glyph count per line using a UTF-8 byte scan
        // that counts codepoint starts (non-continuation bytes).
        const std::string& t = ctx->Text;
        auto countCodepoints = [](std::string_view s) -> size_t
        {
            size_t cps = 0;
            for (unsigned char b : s)
            {
                if ((b & 0xC0u) != 0x80u) // UTF-8 codepoint start
                    ++cps;
            }
            return cps;
        };

        const float px = std::max(1.0f, ctx->PixelSize);
        size_t lineCount = 0;
        size_t maxCpsInLine = 0;
        if (ctx->AllowWrap && wrapWidth > 0.0f)
        {
            const size_t maxCpsPerLine = std::max<size_t>(1, (size_t)std::floor(wrapWidth / (px * 0.6f)));
            size_t start = 0;
            while (start <= t.size())
            {
                size_t nl = t.find('\n', start);
                const size_t len = (nl == std::string::npos) ? (t.size() - start) : (nl - start);
                const size_t cps = countCodepoints(std::string_view(t.data() + start, len));
                const size_t linesForSeg = std::max<size_t>(1, (cps + maxCpsPerLine - 1) / maxCpsPerLine);
                lineCount += linesForSeg;
                maxCpsInLine = std::max(maxCpsInLine, std::min(cps, maxCpsPerLine));
                if (nl == std::string::npos)
                    break;
                start = nl + 1;
            }
        }
        else
        {
            size_t start = 0;
            while (start <= t.size())
            {
                size_t nl = t.find('\n', start);
                const size_t len = (nl == std::string::npos) ? (t.size() - start) : (nl - start);
                const size_t cps = countCodepoints(std::string_view(t.data() + start, len));
                maxCpsInLine = std::max(maxCpsInLine, cps);
                ++lineCount;
                if (nl == std::string::npos)
                    break;
                start = nl + 1;
            }
        }

        const float lineBox = (resolvedBox > 0.0f) ? resolvedBox : px * 1.2f;
        const float h = std::max(1.0f, (float)lineCount * lineBox);
        const float w = std::max(0.0f, (float)maxCpsInLine * px * 0.6f);
        ctx->MeasuredLineCount = std::max(1, (int)lineCount);
        return YGSize{w * invScale, h * invScale};
    }

    return MeasureTextWithAtlas(ctx, wrapWidth);
}

YGSize MeasureTextWithAtlas(TextMeasureCtx* ctx, float wrapWidth, bool* outShaped)
{
    if (outShaped)
        *outShaped = false;
    const float scale    = ctx->ContentScale > 0.0f ? ctx->ContentScale : 1.0f;
    const float invScale = 1.0f / scale;

    auto measureUtf8 = [&](std::string_view s)
    {
        return ctx->Atlas->MeasureUtf8(s, ctx->PixelSize, ctx->LetterSpacingPx);
    };
    auto fontLineMetrics = [&]
    {
        return ctx->Atlas->GetFontLineMetrics(ctx->PixelSize);
    };
    auto buildCaretMap = [&](std::string_view s, std::vector<float>& out)
    {
        return ctx->Atlas->BuildCaretMapUtf8(s, ctx->PixelSize, out, ctx->LetterSpacingPx);
    };

    // CSS line box: a declared line-height resolves against the font size or
    // as an exact length; unset/"normal" uses the font's metric height. One
    // definition for single-line, multi-line and rendered blocks alike.
    const float resolvedBox =
        Rendering::Text::TextLayout::ResolveLineBoxPx(ctx->LineHeight, ctx->FontSize, scale);
    auto lineBoxOrNormal = [&]() -> float
    {
        if (resolvedBox > 0.0f)
            return resolvedBox;
        auto lm = fontLineMetrics();
        return (lm.height > 0.0f) ? lm.height : std::max(1.0f, lm.ascender + lm.descender);
    };

    // Even when the string is empty, text controls (TextInput/TextField) need a
    // sensible intrinsic height so they remain visible and clickable. Treat
    // empty text as a single line with zero width.
    if (ctx->Text.empty())
    {
        ctx->MeasuredLineCount = 1;
        return YGSize{0.0f, lineBoxOrNormal() * invScale};
    }

    auto hashFloatBits = [](float v) -> std::uint32_t
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        return bits;
    };
    std::uint64_t key = ctx->TextHash;
    key ^= (std::uint64_t)hashFloatBits(ctx->PixelSize) * 1099511628211ull;
    key ^= (std::uint64_t)hashFloatBits(ctx->LetterSpacingPx) * 0x9E3779B97F4A7C15ull;
    key ^= (std::uint64_t)(ctx->HasNewlines ? 0xA5A5A5A5u : 0x5A5A5A5Au);
    key ^= (std::uint64_t)(ctx->AllowWrap ? 0xC3C3C3C3u : 0x3C3C3C3Cu);
    if (ctx->AllowWrap)
    {
        const std::uint32_t wbBits = (std::uint32_t)ctx->BreakPolicy;
        key ^= (std::uint64_t)wbBits * 2166136261u;
        if (wrapWidth > 0.0f)
            key ^= (std::uint64_t)hashFloatBits(wrapWidth) * 1099511628211ull;
    }
    key ^= (std::uint64_t)hashFloatBits(resolvedBox) * 14695981039346656037ull;
    // Content-derived atlas id, not the pointer: pointers survive in-place
    // font reloads and can be reused across destroy/recreate, serving sizes
    // measured with the old glyphs.
    key ^= (std::uint64_t)ctx->Atlas->GetAtlasId() * 0x9E3779B97F4A7C15ull;

    if (ctx->CachedValid && ctx->CachedKey == key)
    {
        // cachedW/H are stored in physical px; return logical to Yoga.
        return YGSize{ctx->CachedW * invScale, ctx->CachedH * invScale};
    }
    if (outShaped)
        *outShaped = true;

    const bool wrapEnabled = (ctx->AllowWrap && wrapWidth > 0.0f);

    // The intrinsic width reported to Yoga is the pure advance — the same box a
    // browser gives a shrink-wrapped inline. Slug's half-pixel quad dilation is
    // PAINT bleed and must not be added here: a box widened past its ink places
    // the run off-centre by half the surplus everywhere the box is positioned
    // FROM its intrinsic width (flex centering, flex-end, text-align on a
    // shrink-wrapped box), because emission left-anchors the run at the content
    // origin. Ink that spills a clip is clipped, exactly as CSS overflow does.

    // Multiline-aware measurement (Yoga needs correct height so our scissor doesn't
    // clip all but the first line; Label renders multiline when
    // it sees '\n', but FontAtlas::MeasureUtf8 does not account for newlines).
    if (!ctx->HasNewlines && !wrapEnabled)
    {
        auto metrics = measureUtf8(ctx->Text);
        ctx->CachedW = metrics.width; // stored in physical px
        ctx->CachedH = lineBoxOrNormal();
        ctx->MeasuredLineCount = 1;
        ctx->CachedKey = key;
        ctx->CachedValid = true;
        return YGSize{ctx->CachedW * invScale, ctx->CachedH * invScale};
    }

    auto utf8Next = [](std::string_view s, size_t i, uint32_t& outCp) -> size_t
    {
        if (i >= s.size())
        {
            outCp = 0;
            return s.size();
        }
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80)
        {
            outCp = c;
            return i + 1;
        }
        if ((c >> 5) == 0x6)
        {
            if (i + 1 >= s.size())
            {
                outCp = 0;
                return s.size();
            }
            outCp = ((uint32_t)(c & 0x1F) << 6) | ((uint32_t)(s[i + 1] & 0x3F));
            return i + 2;
        }
        if ((c >> 4) == 0xE)
        {
            if (i + 2 >= s.size())
            {
                outCp = 0;
                return s.size();
            }
            outCp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[i + 1] & 0x3F) << 6) | ((uint32_t)(s[i + 2] & 0x3F));
            return i + 3;
        }
        if ((c >> 3) == 0x1E)
        {
            if (i + 3 >= s.size())
            {
                outCp = 0;
                return s.size();
            }
            outCp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[i + 1] & 0x3F) << 12) | ((uint32_t)(s[i + 2] & 0x3F) << 6) | ((uint32_t)(s[i + 3] & 0x3F));
            return i + 4;
        }
        outCp = c;
        return i + 1;
    };

    auto isWrapWhitespace = [](uint32_t cp) -> bool
    {
        return (cp == ' ' || cp == '\t' || cp == '\r');
    };

    // UAX #14 class HY. Kept in step with TextLayout::ShapeMultiline: Yoga must
    // size a box for the same lines the renderer will draw into it.
    auto isWrapHyphen = [](uint32_t cp) -> bool { return cp == '-'; };

    float maxW = 0.0f;
    size_t lineCount = 0;

    const float lineBox = lineBoxOrNormal();

    const std::string& t = ctx->Text;
    size_t start = 0;
    while (start <= t.size())
    {
        size_t nl = t.find('\n', start);
        const size_t len = (nl == std::string::npos) ? (t.size() - start) : (nl - start);
        std::string_view seg(t.data() + start, len);

        if (seg.empty())
        {
            ++lineCount;
        }
        else if (!wrapEnabled)
        {
            auto m = measureUtf8(seg);
            maxW = std::max(maxW, m.width);
            ++lineCount;
        }
        else
        {
            std::vector<float> xByByte;
            if (!buildCaretMap(seg, xByByte))
            {
                auto m = measureUtf8(seg);
                maxW = std::max(maxW, m.width);
                ++lineCount;
            }
            else
            {
                std::vector<size_t> cpBounds;
                std::vector<size_t> breakBounds;
                cpBounds.reserve(seg.size() + 1);
                breakBounds.reserve(8);

                size_t i = 0;
                while (i < seg.size())
                {
                    uint32_t cp = 0;
                    size_t next = utf8Next(seg, i, cp);
                    if (next <= i)
                        next = i + 1;
                    cpBounds.push_back(next);
                    if (isWrapWhitespace(cp) || isWrapHyphen(cp))
                        breakBounds.push_back(next);
                    i = next;
                }

                // Whitespace that a soft wrap consumes hangs past the wrap
                // width and adds nothing to the line box, so every fit test
                // and every reported line width measures content only.
                auto hangWidth = [&](size_t start, size_t end) -> float
                {
                    size_t e = end;
                    while (e > start && isWrapWhitespace(static_cast<unsigned char>(seg[e - 1])))
                        --e;
                    return xByByte[end] - xByByte[e];
                };
                auto contentWidth = [&](size_t start, size_t end) -> float
                {
                    return (xByByte[end] - xByByte[start]) - hangWidth(start, end);
                };

                auto nextCpBoundary = [&](size_t pos) -> size_t
                {
                    auto it = std::upper_bound(cpBounds.begin(), cpBounds.end(), pos);
                    if (it == cpBounds.end())
                        return seg.size();
                    return *it;
                };
                // Invariant these fitters share with measure: the intrinsic
                // width reported to Yoga is exactly the widest fitted line
                // (pure advance, no halo margin), so a shrink-wrapped box
                // re-measures at its own max line width with zero slack.
                // Lines re-fit only because Yoga rounds the box up on the
                // 1/64 grid; anything that shrinks avail below the reported
                // width by even a fraction re-wraps the text.
                auto findFittingCpBoundary = [&](size_t pos, float avail) -> size_t
                {
                    size_t best = pos;
                    auto it = std::upper_bound(cpBounds.begin(), cpBounds.end(), pos);
                    for (; it != cpBounds.end(); ++it)
                    {
                        size_t end = *it;
                        if (contentWidth(pos, end) <= avail)
                            best = end;
                        else
                            break;
                    }
                    return best;
                };
                auto findFittingBreak = [&](size_t pos, float avail) -> size_t
                {
                    size_t best = std::string::npos;
                    auto it = std::upper_bound(breakBounds.begin(), breakBounds.end(), pos);
                    for (; it != breakBounds.end(); ++it)
                    {
                        size_t end = *it;
                        if (contentWidth(pos, end) <= avail)
                            best = end;
                        else
                            break;
                    }
                    return best;
                };
                auto nextBreakOrEnd = [&](size_t pos) -> size_t
                {
                    auto it = std::upper_bound(breakBounds.begin(), breakBounds.end(), pos);
                    if (it == breakBounds.end())
                        return seg.size();
                    return *it;
                };
                auto skipWhitespace = [&](size_t pos) -> size_t
                {
                    size_t p = pos;
                    while (p < seg.size())
                    {
                        uint32_t cp = 0;
                        size_t next = utf8Next(seg, p, cp);
                        if (!isWrapWhitespace(cp))
                            break;
                        p = next;
                    }
                    return p;
                };

                size_t pos = 0;
                while (pos < seg.size())
                {
                    const float remainingWidth = contentWidth(pos, seg.size());
                    if (remainingWidth <= wrapWidth)
                    {
                        maxW = std::max(maxW, remainingWidth);
                        ++lineCount;
                        break;
                    }

                    size_t breakPos = pos;
                    bool skipWs = false;
                    if (ctx->BreakPolicy == WordBreak::BreakAll)
                    {
                        breakPos = findFittingCpBoundary(pos, wrapWidth);
                        if (breakPos == pos)
                            breakPos = nextCpBoundary(pos);
                    }
                    else
                    {
                        size_t wsPos = findFittingBreak(pos, wrapWidth);
                        if (wsPos != std::string::npos && wsPos > pos)
                        {
                            breakPos = wsPos;
                            skipWs = true;
                        }
                        else if (ctx->BreakPolicy == WordBreak::BreakWord)
                        {
                            breakPos = findFittingCpBoundary(pos, wrapWidth);
                            if (breakPos == pos)
                                breakPos = nextCpBoundary(pos);
                        }
                        else
                        {
                            breakPos = nextBreakOrEnd(pos);
                            if (breakPos == pos)
                                breakPos = nextCpBoundary(pos);
                            skipWs = true;
                        }
                    }

                    const float lineW = contentWidth(pos, breakPos);
                    maxW = std::max(maxW, lineW);
                    ++lineCount;
                    pos = breakPos;
                    // Only a whitespace break swallows what follows; a hyphen
                    // break keeps the next character on the new line.
                    if (skipWs && breakPos > 0 &&
                        isWrapWhitespace(static_cast<unsigned char>(seg[breakPos - 1])))
                        pos = skipWhitespace(pos);
                }
            }
        }

        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }

    if (lineCount == 0)
        return YGSize{0, 0};

    const float h = lineBox * (float)lineCount;

    ctx->CachedW = maxW; // stored in physical px
    ctx->CachedH = h;
    ctx->MeasuredLineCount = (int)lineCount;
    ctx->CachedKey = key;
    ctx->CachedValid = true;
    return YGSize{ctx->CachedW * invScale, ctx->CachedH * invScale};
}

// Yoga's baseline for `align-items: baseline`. Without one a childless node
// reports its measured HEIGHT (yoga/algorithm/Baseline.cpp), which aligns box
// BOTTOMS — indistinguishable from correct only while every item on the line
// shares a font size and line-height. A text node reports the offset from its
// top edge to its first line's alphabetic baseline instead, so mixed sizes
// align on the glyphs.
float BaselineTextFn(YGNodeConstRef node, float /*width*/, float height)
{
    auto* ctx = reinterpret_cast<TextMeasureCtx*>(YGNodeGetContext(node));
    // No metrics without a face — the host may still be resolving font bytes.
    // Report what Yoga would have, rather than inventing an ascent; the atlas
    // arriving invalidates geometry and the next solve places the real one.
    if (!ctx || !ctx->Atlas)
        return height;

    const float scale = ctx->ContentScale > 0.0f ? ctx->ContentScale : 1.0f;

    // MeasureTextFn reports the CONTENT box, so padding is what separates the
    // node's top edge from the text. Border is absent by construction: nothing
    // calls YGNodeStyleSetBorder, so Yoga's box model has none to subtract.
    const float paddingTop = YGNodeLayoutGetPadding(node, YGEdgeTop);
    const float contentHeight =
        std::max(0.0f, height - paddingTop - YGNodeLayoutGetPadding(node, YGEdgeBottom));

    const float lineBox =
        Rendering::Text::TextLayout::ResolveLineBoxPx(ctx->LineHeight, ctx->FontSize, scale);
    const auto lineMetrics = ctx->Atlas->GetFontLineMetrics(ctx->PixelSize);
    const float baselineInContent = Rendering::Text::TextLayout::FirstBaselineInContentBoxPx(
        contentHeight * scale, lineMetrics, lineBox, ctx->MeasuredLineCount);
    return paddingTop + baselineInContent / scale;
}
#endif

// ---------------------------------------------------------------------------
// Yoga node lifecycle helpers
// ---------------------------------------------------------------------------

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
void DetachYogaFromParent(YGNodeRef node)
{
    assert(!CascadeComputeActive() &&
           "Yoga mutation inside cascade-compute region (MT-4.0 seam violation)");
    if (!node)
        return;
    if (YGNodeRef p = YGNodeGetParent(node))
    {
        YGNodeRemoveChild(p, node);
    }
}

void RemoveAllYogaChildren(YGNodeRef node)
{
    assert(!CascadeComputeActive() &&
           "Yoga mutation inside cascade-compute region (MT-4.0 seam violation)");
    if (!node)
        return;
    while (YGNodeGetChildCount(node) > 0)
    {
        YGNodeRef c = YGNodeGetChild(node, 0);
        if (!c)
            break;
        YGNodeRemoveChild(node, c);
    }
}

void ApplyWeightedPaneFlexGrowOverride(GameEngine::UIElement* el, YGNodeRef node)
{
    if (!node || !el || el->Kind() != UIElementKind::WeightedPane)
        return;
    auto* pane = static_cast<WeightedPane*>(el);
    YGNodeStyleSetFlexGrow(node, std::max(0.0f, pane->GetFlexWeight()));
}

void ApplyBlockFlowShrinkDefault(YGNodeRef node,
                                 const GameEngine::ResolvedStyle& style,
                                 const GameEngine::ResolvedStyle* parentStyle)
{
    if (!node)
        return;
    // ApplyStyle already pushed the flex-container answer, and it is the right
    // one everywhere except an undeclared shrink inside block flow.
    if (style.Layout.FlexShrink.has_value())
        return;
    if (!parentStyle || !EstablishesBlockFlow(parentStyle->Layout.DisplayMode))
        return;
    YGNodeStyleSetFlexShrink(node, 0.0f);
}

// Out-of-line destructor: detach this node from any Yoga parent, drop any
// remaining Yoga children, then destroy the YGNode. Defined here so it can
// reach YogaAdapter / DetachYogaFromParent / RemoveAllYogaChildren without
// pulling yoga/Yoga.h into UIManager.h.
//
// Idempotent on already-null `node` (default-constructed instance never
// allocated a YGNode). When called via ~UIElement: children destruct
// before this element's m_YogaState (m_YogaState is declared earlier in
// UIElement, so it lives longer), so by the time we reach here the
// children have already detached themselves — RemoveAllYogaChildren is a
// no-op in the steady-state and a safety net otherwise.
RetainedYogaNode::~RetainedYogaNode()
{
    if (!Node)
        return;
    DetachYogaFromParent(Node);
    RemoveAllYogaChildren(Node);
    YGNodeSetMeasureFunc(Node, nullptr);
    YGNodeSetBaselineFunc(Node, nullptr);
    YGNodeSetContext(Node, nullptr);
    YogaAdapter::DestroyNode(Node);
    Node = nullptr;
}

void UIManager::ResetRetainedYogaTree()
{
    // YGNode ownership lives on UIElement::m_YogaState now (Stage 7 step 8).
    // This function no longer walks per-element retained nodes — the YGNodes
    // get destroyed when their owning UIElements destruct (e.g. when m_Root
    // is reassigned, or when subtrees are torn down by callers like the
    // dockspace rebuild). Here we just reset the manager-side caches that
    // the next BuildYogaRecursive run rebuilds from scratch.

    // Interned sheet-set IDs referred to entries on now-stale RetainedYogaNodes.
    // Clear the interner so IDs don't bleed across scene reloads / stylesheet-
    // set resets.
    m_SheetSetInterner.Clear();

    // Bulk reset invalidates every surgical assumption — force a full
    // rebuild regardless of op queue state.
    m_ForceFullRebuildNextFrame = true;

    // Invalidate the FocusOrder skip gate. It compares against the
    // structure-change generation captured on the last run and would
    // otherwise wrongly reuse a stale m_FocusOrder vector against a fresh
    // ctx.nodes population on the first post-reset frame. (The matching
    // IndicesAndClips skip gate retired in Stage 7 step 7.2 alongside
    // RebuildNodeIndices.)
    m_FocusOrderPrimed = false;
}

#endif

void UIManager::NotifyRelayoutRequested(const UIElement* source)
{
    m_RequestRelayout = true;
    ++m_RelayoutRequestsSinceLastUpdate;
    if (source)
    {
        m_RelayoutLastSourceInstanceId = source->GetInstanceId();
    }
}

void UIManager::InvalidateRetainedLayout(UIElement* subtreeRoot)
{
    if (!subtreeRoot)
        return;

    std::vector<UIElement*> stack{subtreeRoot};
    while (!stack.empty())
    {
        UIElement* element = stack.back();
        stack.pop_back();
        if (!element)
            continue;

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
        if (element->m_YogaState && element->m_YogaState->Node)
            YogaAdapter::MarkDirtyAndPropagate(element->m_YogaState->Node);
#endif

        element->MarkDirty(UIElement::LayoutDirty);

        for (const auto& child : element->GetChildren())
            stack.push_back(child.get());
        if (element->Kind() == UIElementKind::Mount)
            stack.push_back(static_cast<Mount*>(element)->GetTarget());
    }
    NotifyRelayoutRequested(subtreeRoot);
}

// ---------------------------------------------------------------------------
// SheetSetInterner — persistent intern of merged (parent+local) sheet lists.
// ---------------------------------------------------------------------------

UIManager::SheetSetInterner::SheetSetInterner()
{
    // Reserve ID 0 for the empty set. Always present so callers can
    // rely on a total mapping of (possibly-empty) spans → nonzero IDs
    // for non-empty, 0 for empty.
    m_Entries.push_back(Entry{});
}

void UIManager::SheetSetInterner::Clear()
{
    m_Entries.clear();
    m_HashBuckets.clear();
    m_Entries.push_back(Entry{});
    ++m_Generation;
}

uint32_t UIManager::SheetSetInterner::Intern(
    std::span<const StylesheetHandle> handles,
    UIManager& mgr)
{
    // MT-4.1 seam: interning mutates manager-container state (the interner
    // entries + the rule-index cache via GetOrBuildRuleIndex). It must run in
    // the collect stage only — never from the cascade compute region. Verified:
    // no ComputeElementCascade / ResolveCascadeForElement / ComputeStyleInto
    // path reaches Intern (compute consumes pre-built sheet/index spans;
    // CollectYogaInputs interns first). The Debug seam assert catches a
    // regression at its source.
    assert(!CascadeComputeActive() &&
           "SheetSetInterner::Intern inside cascade-compute region (MT-4.1 seam violation)");
    if (handles.empty())
        return 0;

    // Hash from raw pointers (handle.get() identity). Stylesheets are
    // deduplicated by pointer identity, not by content.
    uint64_t h = 0;
    for (const StylesheetHandle& sh : handles)
        HashCombine(h, HashPtr64(sh.get()));

    auto it = m_HashBuckets.find(h);
    if (it != m_HashBuckets.end())
    {
        for (uint32_t id : it->second)
        {
            const Entry& e = m_Entries[id];
            if (e.Sheets.size() != handles.size())
                continue;
            bool match = true;
            for (size_t i = 0; i < handles.size(); ++i)
            {
                if (e.Sheets[i] != handles[i].get())
                {
                    match = false;
                    break;
                }
            }
            if (match)
                return id;
        }
    }

    const uint32_t newId = (uint32_t)m_Entries.size();
    Entry e{};
    e.Sheets.reserve(handles.size());
    e.Handles.reserve(handles.size());
    e.Indices.reserve(handles.size());
    for (const StylesheetHandle& sh : handles)
    {
        const Stylesheet* raw = sh.get();
        e.Sheets.push_back(raw);
        e.Handles.push_back(sh);  // shared_ptr copy — keeps Stylesheet alive
        e.Indices.push_back(mgr.GetOrBuildRuleIndex(raw));
    }
    e.Hash = h;
    m_Entries.push_back(std::move(e));
    m_HashBuckets[h].push_back(newId);
    return newId;
}

std::span<const Stylesheet* const>
UIManager::SheetSetInterner::GetSheets(uint32_t id) const
{
    if (id >= m_Entries.size())
        return {};
    const auto& s = m_Entries[id].Sheets;
    return {s.data(), s.size()};
}

std::span<const StylesheetHandle>
UIManager::SheetSetInterner::GetHandles(uint32_t id) const
{
    if (id >= m_Entries.size())
        return {};
    const auto& h = m_Entries[id].Handles;
    return {h.data(), h.size()};
}

std::span<const StylesheetRuleIndex* const>
UIManager::SheetSetInterner::GetIndices(uint32_t id) const
{
    if (id >= m_Entries.size())
        return {};
    const auto& s = m_Entries[id].Indices;
    return {s.data(), s.size()};
}

// ---------------------------------------------------------------------------
// InvalidateSheetSetSubtree — zero PersistentSheetSetId + mark StyleDirty
// across the subtree (children + Mount targets). Used by stylesheet
// attach/detach sites on UIElement.
// ---------------------------------------------------------------------------

void UIManager::InvalidateSheetSetSubtree(UIElement* root)
{
    if (!root)
        return;
    // Keep the analysis-seed set in sync on every stylesheet attach/detach (both
    // call here): an element that owns local (subtree-attached) sheets must seed
    // the dynamic style analysis each frame via m_LocalSheetElements, since the
    // subtree-skip fast path can otherwise drop those sheets from the per-frame
    // Yoga-walk collection and break :hover/:active. SetOwnerManager handles the
    // attach/detach transitions; this handles add/remove while attached.
    if (!root->GetStylesheets().empty())
        RegisterLocalSheetElement(root);
    else
        UnregisterLocalSheetElement(root);
    // Zero this element's interned ID if it has a retained node. Also
    // drop the subtree-skip gate flag — the merged sheet list above this
    // element is changing, so the gate's PrevLocalSheetsHash /
    // PrevParentSheetsHash check would still pass on the first
    // post-mutation frame (sheets only attach to a single element, but
    // every descendant inherits the change). Forcing the slow path
    // guarantees the cascade re-runs against the new sheet pool.
    if (root->m_YogaState)
    {
        root->m_YogaState->PersistentSheetSetId = 0;
        root->m_YogaState->SheetSetGeneration = 0;
        root->m_YogaState->CachedSubtreeValid = false;
    }
    // MarkDirty covers this element (ancestors too via SubtreeDirty
    // propagation). MarkDirtySubtree was already called by the attach/
    // detach site on the root — here we just need to ensure Mount target
    // descendants also receive StyleDirty since MarkDirtySubtree doesn't
    // walk Mount portals.
    root->MarkDirty(UIElement::StyleDirty);
    // Cascade-memoization Phase 2: stylesheet attach/detach changes the
    // candidate-rule pool for the entire subtree. Invalidate the rule
    // cache here so the next cascade rebuilds against the new sheet set.
    // The recursive walk below handles Mount portal targets too.
    root->InvalidateRuleCacheSubtree();
    for (const auto& ch : root->GetChildren())
        InvalidateSheetSetSubtree(ch.get());
    if (root->Kind() == UIElementKind::Mount)
    {
        if (UIElement* tgt = static_cast<Mount*>(root)->GetTarget())
            InvalidateSheetSetSubtree(tgt);
    }
}

// ---------------------------------------------------------------------------
// Dependency methods for BuildYogaRecursive (extracted from Update lambdas)
// ---------------------------------------------------------------------------

namespace
{
bool ChainIsShareSensitive(const SelectorChain& chain);

// P4 sibling sharing: true when a compound's own matchers can differ between
// two same-key siblings — positional pseudos, :empty, or attribute selectors
// (per-element values), including nested inside :is()/:not()/:where()
// (nested chains are scanned in full so `:is(.a + .b)` catches the sibling
// combinator too).
bool CompoundIsShareSensitive(const CompoundSelector& c)
{
    if (!c.Attributes.empty())
        return true;
    for (const PseudoClass& p : c.Pseudos)
    {
        switch (p.PseudoKind)
        {
        case PseudoClass::Kind::Empty:
        case PseudoClass::Kind::FirstChild:
        case PseudoClass::Kind::LastChild:
        case PseudoClass::Kind::OnlyChild:
        case PseudoClass::Kind::FirstOfType:
        case PseudoClass::Kind::LastOfType:
        case PseudoClass::Kind::OnlyOfType:
        case PseudoClass::Kind::NthChild:
        case PseudoClass::Kind::NthLastChild:
        case PseudoClass::Kind::NthOfType:
        case PseudoClass::Kind::NthLastOfType:
            return true;
        case PseudoClass::Kind::Is:
        case PseudoClass::Kind::Not:
        case PseudoClass::Kind::Where:
            for (const auto& nested : p.SelectorList)
                if (nested && ChainIsShareSensitive(*nested))
                    return true;
            break;
        default:
            break;
        }
    }
    return false;
}

// Sibling combinators anywhere make the whole chain depend on preceding
// siblings; otherwise only the rightmost compound can diverge between
// same-key siblings (all left-terms match the shared ancestor chain).
bool ChainIsShareSensitive(const SelectorChain& chain)
{
    if (chain.Terms.empty())
        return false;
    for (const SelectorTerm& term : chain.Terms)
    {
        if (term.Comb == SelectorTerm::Combinator::AdjacentSibling ||
            term.Comb == SelectorTerm::Combinator::GeneralSibling)
            return true;
    }
    return CompoundIsShareSensitive(chain.Terms.back().Selector);
}
} // namespace

const StylesheetRuleIndex* UIManager::GetOrBuildRuleIndex(const Stylesheet* s)
{
    // MT-4.1 seam: on a cache miss this builds and inserts into the manager-
    // owned rule-index cache. Collect-stage only (CollectYogaInputs /
    // ResetFlatSheetTable / Intern / root-sheet setup pre-warm every index the
    // compute stage consumes). The compute region receives already-built index
    // spans, so no cascade path can reach a miss here. The Debug seam assert
    // catches a regression at its source.
    assert(!CascadeComputeActive() &&
           "GetOrBuildRuleIndex inside cascade-compute region (MT-4.1 seam violation)");
    if (!s)
        return nullptr;
    auto it = m_StylesheetRuleIndexCache.find(s);
    if (it != m_StylesheetRuleIndexCache.end())
        return it->second.get();

    auto idx = std::make_unique<StylesheetRuleIndex>();
    idx->Universal.reserve(s->Rules.size() / 4);

    // P4: rarest-class bucketing. A rule keyed by class goes into the
    // LEAST-POPULATED class bucket of its rightmost compound, not the first
    // written one — theme-common prefix classes (.inspector-field, .tree-row)
    // otherwise absorb every compound rule and the fat bucket is re-matched
    // by every element carrying the common class. Two passes: count how many
    // rules would key to each class, then assign each rule to its rarest.
    // Lookup is unchanged (candidates already probe every element class).
    std::unordered_map<StringId, uint32_t> classKeyCounts;
    for (const CSSRule& rule : s->Rules)
    {
        if (rule.Selector.Terms.empty())
            continue;
        const CompoundSelector& c = rule.Selector.Terms.back().Selector;
        if (c.ResolvedId == 0)
            for (StringId cls : c.ResolvedClassIds)
                ++classKeyCounts[cls];
    }

    for (uint32_t i = 0; i < (uint32_t)s->Rules.size(); ++i)
    {
        const CSSRule& rule = s->Rules[i];
        if (rule.Selector.Terms.empty())
        {
            idx->Universal.push_back(i);
            continue;
        }
        const CompoundSelector& c = rule.Selector.Terms.back().Selector;
        // Share-sensitivity is bucketed alongside the rule so the sibling-
        // sharing probe can disqualify exactly the elements such a rule can
        // reach (id-keyed rules are irrelevant: id-carrying elements never
        // share).
        const bool sensitive = ChainIsShareSensitive(rule.Selector);
        if (c.ResolvedId != 0)
            idx->ById[c.ResolvedId].push_back(i);
        else if (!c.ResolvedClassIds.empty())
        {
            StringId rarest = c.ResolvedClassIds.front();
            uint32_t rarestCount = classKeyCounts[rarest];
            for (size_t ci = 1; ci < c.ResolvedClassIds.size(); ++ci)
            {
                const uint32_t n = classKeyCounts[c.ResolvedClassIds[ci]];
                if (n < rarestCount)
                {
                    rarest = c.ResolvedClassIds[ci];
                    rarestCount = n;
                }
            }
            idx->ByClass[rarest].push_back(i);
            if (sensitive)
                for (StringId cls : c.ResolvedClassIds)
                    idx->ShareSensitiveClasses.insert(cls);
        }
        else if (c.ResolvedTagId)
        {
            idx->ByTag[c.ResolvedTagId].push_back(i);
            if (sensitive)
                idx->ShareSensitiveTags.insert(c.ResolvedTagId);
        }
        else if (!c.Tag.empty() && !c.Universal)
            continue;
        else
        {
            idx->Universal.push_back(i);
            if (sensitive)
                idx->ShareSensitiveUniversal = true;
        }
    }
    auto [itIns, inserted] = m_StylesheetRuleIndexCache.emplace(s, std::move(idx));
    return itIns->second.get();
}

void UIManager::ResetFlatSheetTable(UpdateContext& ctx, UIManager& mgr)
{
    ctx.flatSheets.clear();
    ctx.flatHandles.clear();
    ctx.flatIndices.clear();
    ctx.sheetSetRefs.clear();
    SheetSetRef rootRef;
    rootRef.offset = 0;
    rootRef.count = (uint32_t)ctx.rootSheets.size();
    for (const StylesheetHandle& h : ctx.rootSheets)
    {
        ctx.flatSheets.push_back(h.get());
        ctx.flatHandles.push_back(h);
        ctx.flatIndices.push_back(mgr.GetOrBuildRuleIndex(h.get()));
    }
    ctx.sheetSetRefs.push_back(rootRef);
}

void UIManager::AddSheetForAnalysis(const Stylesheet* s, UpdateContext& ctx)
{
    if (!s)
        return;
    if (ctx.analysisSheets.insert(s).second)
        ctx.analysisSheetHash ^= HashPtr64(s);
}

void UIManager::ApplyElementStyleOverrides(UIElement* el, ResolvedStyle& cs, bool allowLiveTextureRegistration)
{
    if (!el)
        return;
    if (const auto* liveBg = el->GetBackgroundImageTextureOverride())
    {
        const std::string key = MakeInternalBgKey(el->GetInstanceId());
        BackgroundImageSource src{};
        src.Kind = BackgroundImageSource::SourceKind::ResourceName;
        src.Value = key;
        cs.Visual.BackgroundImage.Source = std::move(src);
        cs.Visual.BackgroundImage.HasImage = cs.Visual.BackgroundImage.Source.HasImage();

        // Live background-image textures resolve directly from the element's
        // override (GetBackgroundImageTextureOverride) during RenderGraph primitive
        // generation, so no name-keyed registry entry is needed here.
        (void)allowLiveTextureRegistration;
    }

    if (!el->InlineOverrides().IsEmpty())
        ApplyOverridesToResolvedStyle(cs, el->InlineOverrides());
}

// Thread-local buffers for BuildYogaRecursive (capacity retained across frames).
static thread_local std::vector<GameEngine::UILayout::YogaAdapter::ChildWithOrder> s_OrderedChildrenBuf;

// TextMeasureCtx used to live in a thread_local pool here; Slice 2 step 3
// moved per-element contexts onto RetainedYogaNode so fast-path appends
// keep Yoga's context pointer stable across frames. The stubs below keep
// the old API surface alive for the spike logger (ctx.textMeasureCtxUsed)
// and are always zero.
static thread_local size_t s_TextMeasureCtxUsed = 0;
size_t& GetTextMeasureCtxUsed() { return s_TextMeasureCtxUsed; }

void TrimTextMeasureCtxPool()
{
    // No-op after the pool was removed. Kept so existing callers compile.

    // Never resize the pool. Retained Yoga nodes may hold context pointers
    // to any pool entry across frames when computeLayoutSignaturesThisFrame
    // is false. Freeing entries causes use-after-free in MeasureTextFn.
}

// ---------------------------------------------------------------------------
// Style resolution helpers (shared by BuildYogaRecursive and ConvergePostLayout)
// ---------------------------------------------------------------------------

namespace
{
// P4 sibling sharing: dynamic pseudo state packed into the share key.
uint8_t PackShareStateBits(const ElementState& st)
{
    uint8_t bits = 0;
    if (st.Disabled)     bits |= 1u << 0;
    if (st.Enabled)      bits |= 1u << 1;
    if (st.Checked)      bits |= 1u << 2;
    if (st.Hover)        bits |= 1u << 3;
    if (st.Focus)        bits |= 1u << 4;
    if (st.FocusVisible) bits |= 1u << 5;
    if (st.FocusWithin)  bits |= 1u << 6;
    if (st.Active)       bits |= 1u << 7;
    return bits;
}

uint64_t FoldShareHash(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

// True when any effective sheet holds a share-sensitive rule reachable from
// this element's buckets (see StylesheetRuleIndex). Conservative: a missing
// index disqualifies sharing outright.
bool ProbeShareSensitive(const UIElement* el, StringId tagId,
                         std::span<const StylesheetRuleIndex* const> indices)
{
    if (indices.empty())
        return true;
    for (const StylesheetRuleIndex* idx : indices)
    {
        if (!idx)
            return true;
        if (idx->ShareSensitiveUniversal)
            return true;
        if (tagId != 0 && !idx->ShareSensitiveTags.empty() &&
            idx->ShareSensitiveTags.count(tagId) != 0)
            return true;
        if (!idx->ShareSensitiveClasses.empty())
        {
            for (StringId cls : el->GetClassIds())
                if (idx->ShareSensitiveClasses.count(cls) != 0)
                    return true;
        }
    }
    return false;
}
} // namespace

// Build the per-element cascade state (pseudo-class predicates). Extracted
// from ResolveCascadeForElement (MT-4.2) so the parallel children builder can
// pre-compute the sibling-share key at collect time. Reads manager pointer
// members that are stable for the whole build pass.
UIParsing::ElementState UIManager::BuildCascadeElementState(UIElement* el) const
{
    ElementState st{};
    st.Disabled = !el->IsEnabled();
    st.Enabled = el->IsEnabled();
    st.Checked = el->IsPseudoChecked();
    st.Hover = (el == m_Hovered);
    st.Focus = el->IsFocusTargetForId(m_FocusId);
    st.FocusVisible = st.Focus && m_FocusViaKeyboard;
    // Stage 5 Block D: :focus-within is true for the focused element and all
    // its ancestors. The chain is precomputed by RebuildFocusWithinChain at
    // Update entry; this is an O(1) hash lookup.
    st.FocusWithin = m_FocusWithinChain.count(el) > 0;
    st.Active = m_MouseDown && (m_MouseCaptured ? (el == m_CaptureElement) : (el == m_Hovered));
    // Stage 5 Block C: copy the element's active custom-state ids into the
    // cascade's ElementState. MatchesSelectorChain looks up custom :my-state
    // selectors via ResolvedCustomNameId in this set.
    const auto& customIds = el->GetCustomStateIds();
    st.CustomStates.reserve(customIds.size());
    for (StringId id : customIds)
        st.CustomStates.insert(id);
    return st;
}

// Sibling-share eligibility + key, extracted from ResolveCascadeForElement
// (MT-4.2). Single source of truth: ResolveCascadeForElement calls this so the
// key the parallel builder groups by is byte-identical to the key the donor
// registers under. Reads only el + the passed hover-ancestor set (both stable
// during the pass; `hover` may be null when the hover-chain bit is disabled).
// Returns eligibility; out params are 0 when ineligible.
bool UIManager::ComputeCascadeShareKey(UIElement* el,
    std::span<const StylesheetRuleIndex* const> indices,
    const UIParsing::ElementState& st,
    const std::unordered_set<const UIElement*>* hover,
    uint64_t& outKey, uint16_t& outStateBits, uint32_t& outSheetSetId) const
{
    outKey = 0;
    outStateBits = 0;
    outSheetSetId = 0;
    const StringId tagId =
        UIRegistration::ElementFactoryRegistry::Instance().GetElementTagId(*el);
    const bool eligible = el->GetOwnerManager() == this &&
                          el->m_YogaState != nullptr &&
                          el->GetIdHash() == 0 &&
                          !el->HasSelectorAttributes() &&
                          st.CustomStates.empty() &&
                          el->Overrides().CustomCount() == 0 &&
                          el->InlineOverrides().CustomCount() == 0 &&
                          !ProbeShareSensitive(el, tagId, indices);
    if (!eligible)
        return false;

    outSheetSetId = el->m_YogaState->PersistentSheetSetId;
    uint16_t stateBits = PackShareStateBits(st);
    // Rightmost `:hover` matches the hovered element's whole ancestor chain
    // (ElementMatchesHover) — st.Hover alone would let a row whose CHILD is
    // hovered share its hover-matched style with (or steal the plain style of)
    // an identical sibling.
    if (hover && hover->count(el) != 0)
        stateBits |= 1u << 8;
    // `:root` matches parentless elements (incl. detached Mount targets)
    // regardless of any keyed field.
    if (el->GetParent() == nullptr)
        stateBits |= 1u << 9;
    outStateBits = stateBits;

    uint64_t h = FoldShareHash(0, reinterpret_cast<uintptr_t>(el->GetDfsParent()));
    h = FoldShareHash(h, reinterpret_cast<uintptr_t>(&typeid(*el)));
    h = FoldShareHash(h, outSheetSetId);
    h = FoldShareHash(h, stateBits);
    for (StringId cls : el->GetClassIds())
        h = FoldShareHash(h, cls);
    outKey = h;
    return true;
}

bool UIManager::ResolveCascadeForElement(UIElement* el,
    std::span<const Stylesheet* const> sheets,
    std::span<const StylesheetRuleIndex* const> indices,
    const ResolvedStyle* parentStyle,
    CascadeShareContext* shareCtx,
    TransitionRegistration& outTransition)
{
    outTransition = TransitionRegistration::None;
    ElementState st = BuildCascadeElementState(el);

    // P4 sibling style sharing: identically-shaped siblings (same DFS
    // parent, type, sheet set, classes, dynamic state — no id, no
    // attributes, no custom states, no custom override vars, no reachable
    // share-sensitive rule) resolve to identical cascade output. The first
    // one computed per build pass registers a snapshot; the rest copy it.
    // Entity-select inspector rebuilds collapse from ~one cascade per
    // element to ~one per distinct shape.
    bool sharedFromCache = false;
    uint64_t shareKey = 0;
    bool shareEligible = false;
    uint16_t shareStateBits = 0;
    uint32_t shareSheetSetId = 0;
    // MT-4.1: the share cache + active flag arrive via the explicit context
    // (nullptr / !Active == sharing disabled). Registering a donor into
    // shareCtx->Cache is per-context state, so it stays legal inside the
    // compute region (each MT-4.2 worker owns its own context).
    CascadeShareCache* shareCache =
        (shareCtx && shareCtx->Active) ? shareCtx->Cache : nullptr;
    if (shareCache)
    {
        shareEligible = ComputeCascadeShareKey(el, indices, st, shareCtx->Hover,
                                               shareKey, shareStateBits, shareSheetSetId);
        if (shareEligible)
        {
            auto itShare = shareCache->Entries.find(shareKey);
            if (itShare != shareCache->Entries.end())
            {
                const auto& e = itShare->second;
                // Hash hit must prove full equality — a collision degrades
                // to a miss (and the fresh cascade overwrites the entry),
                // never to a wrong style.
                if (e.Parent == el->GetDfsParent() &&
                    e.Type == &typeid(*el) &&
                    e.SheetSetId == shareSheetSetId &&
                    e.StateBits == shareStateBits &&
                    e.ClassIds == el->GetClassIds())
                {
                    el->GetMutableResolvedStyle() = e.Style;
                    el->m_MatchedPseudoStates = e.MatchedPseudoStates;
                    el->m_CachedMatchedRules = e.CachedRules;
                    el->m_RuleCacheValid = e.RuleCacheValid;
                    el->m_RuleCacheEpoch = e.RuleCacheEpoch;
                    sharedFromCache = true;
                }
            }
        }
    }

    if (!sharedFromCache)
    {
        // Stage 5 Block B: capture the per-element pseudo-state predicate as
        // a side output of the cascade. Mark-dirty sites consult
        // el->m_MatchedPseudoStates to skip pushes for elements whose style
        // cannot change for the given state flip.
        uint16_t matchedPseudoBits = 0;
        CSSParser::ComputeStyleInto(el->GetMutableResolvedStyle(), *el,
            sheets, indices, st, parentStyle, m_Hovered, &matchedPseudoBits);
        el->m_MatchedPseudoStates = matchedPseudoBits;

        if (shareEligible)
        {
            // Register the donor snapshot NOW — FinalizeElementStyle
            // mutates el's ResolvedStyle in place right after this call, and
            // per-element (non-custom) overrides must not leak into sharees.
            auto& e = shareCache->Entries[shareKey];
            e.Parent = el->GetDfsParent();
            e.Type = &typeid(*el);
            e.SheetSetId = shareSheetSetId;
            e.StateBits = shareStateBits;
            e.ClassIds = el->GetClassIds();
            e.Style = el->GetResolvedStyle();
            e.MatchedPseudoStates = el->m_MatchedPseudoStates;
            e.CachedRules = el->m_CachedMatchedRules;
            e.RuleCacheValid = el->m_RuleCacheValid;
            e.RuleCacheEpoch = el->m_RuleCacheEpoch;
        }
    }

    // Decide the declared-transition registry action; the CALLER applies it
    // (MT-4.1) so this function performs no manager-container mutation and can
    // run on an MT-4.2 worker. rs.Transitions changes only when a cascade
    // rewrites it, so this is the single decision point; TransitionEngine::
    // Advance iterates the registry instead of walking the tree every heavy
    // frame. The decision reads the freshly-cascaded style BEFORE overrides
    // are applied (identical to the pre-4.1 in-place site). Registration
    // requires ownership: the unregister hooks key off el's owner, and
    // BuildYogaRecursive can cascade a mount target before Mount::OnPostLayout
    // adopts it — that adoption re-marks the subtree, so the next cascade
    // registers it. An empty transition set always unregisters (ownership-
    // independent, matching the old unconditional erase).
    if (el->GetResolvedStyle().Transitions.IsEmpty())
        outTransition = TransitionRegistration::Unregister;
    else if (el->GetOwnerManager() == this)
        outTransition = TransitionRegistration::Register;

    el->ClearDirty(UIElement::StyleDirty);
    return sharedFromCache;
}

// The last thing that touches an element's ResolvedStyle before the Yoga push
// reads it: overrides land on top of the cascade, then the used values that no
// single declaration can decide are resolved (border-style and border-width
// arrive in declaration order, so neither property's applier can settle the
// pair on its own).
//
// All three product cascade paths — ReResolvePseudoStateElement,
// ComputeElementCascade, ConvergePostLayout — call this immediately after
// ResolveCascadeForElement, so an element whose border-width or border-style
// could have changed this frame passes through here; when neither the cascade
// nor the overrides ran, the style is byte-identical to last frame's, which
// already did. UIManager_CascadeProbe calls the cascade without this: it is a
// bench harness that times the cascade and never drives layout or paint.
//
// TransitionEngine::AdvanceSlots writes interpolated values straight into
// ResolvedStyle with no cascade, so it re-applies the used-value resolution
// itself rather than routing through here.
void UIManager::FinalizeElementStyle(UIElement* el)
{
    if (el->Overrides().PropCount() > 0 ||
        el->HasBackgroundImageTextureOverride() ||
        !el->InlineOverrides().IsEmpty())
    {
        ApplyOverridesToResolvedStyle(el->GetMutableResolvedStyle(), el->Overrides());
        ApplyElementStyleOverrides(el, el->GetMutableResolvedStyle(), /*allowLiveTextureRegistration=*/false);
    }

    ResolveUsedBorderWidths(el->GetMutableResolvedStyle());

    el->Overrides().ClearDirty();
}

void UIManager::ReResolvePseudoStateElement(UIElement* el)
{
    // Re-bake one element's FULL cascade in-frame (st.Hover, st.Active, focus, ...).
    // The frame's main cascade (applyDirtyStylesToYoga in SolveAndApplyLayout) bakes
    // pseudo state (e.g. st.Hover = el == m_Hovered, st.Active = m_MouseDown && ...) at
    // the TOP of the frame, BEFORE ProcessHoverChain updates m_Hovered / sees the button
    // edge. On the passive fast path that input is pre-updated so it's fine, but a heavy
    // frame bakes the PREVIOUS pseudo state -> the :hover / :active style paints one
    // transition late. ProcessHoverChain calls this right after it updates m_Hovered
    // (reusing that hit-test, no extra one) to re-bake just the element whose hover or
    // active state flipped, so the paint drain reads the current pseudo state this frame.
    // Mirrors applyDirtyStylesToYoga's per-element body.
    if (!el || !el->m_YogaState)
        return;

    const ResolvedStyle* parentStyle = nullptr;
    if (UIElement* dfsParent = el->GetDfsParent())
        parentStyle = &dfsParent->GetResolvedStyle();

    // Snapshot the CSS-inherited fields (the InheritProperties set in
    // UIManager_StyleResolve.cpp). If the re-bake changes any of them,
    // descendants inheriting the value need their primitives re-emitted:
    // ResolveStyles fixes the VALUES render-side every frame, but the drain
    // only re-emits queued elements — so queue the subtree. On heavy frames
    // the full regen usually subsumes this; on pointer-only frames it is the
    // only thing that repaints inheriting children.
    const VisualStyle inheritedBefore = el->GetResolvedStyle().Visual;

    // An id interned before the last interner clear resolves to nothing.
    // BuildYogaRecursive re-primes ids on visit but stops at display:none, so
    // a hidden subtree keeps stale ids while the hover walk still reaches it.
    // Hand such an element to the batch cascade, which derives the pool
    // top-down, instead of baking it against an empty pool.
    if (el->m_YogaState->SheetSetGeneration != m_SheetSetInterner.Generation())
    {
        el->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
        return;
    }
    const uint32_t sheetSetId = el->m_YogaState->PersistentSheetSetId;
    const auto sheetSpan = m_SheetSetInterner.GetSheets(sheetSetId);
    // Post-cascade pseudo re-resolve: share-inactive (nullptr context). Apply
    // the declared-transition decision inline — this caller runs on the UI
    // thread and does not flow through a YogaBuildItem.
    TransitionRegistration transitionDecision = TransitionRegistration::None;
    ResolveCascadeForElement(el,
                             sheetSpan,
                             m_SheetSetInterner.GetIndices(sheetSetId),
                             parentStyle,
                             /*shareCtx=*/nullptr,
                             transitionDecision);
    ApplyTransitionRegistration(el, transitionDecision);
    FinalizeElementStyle(el);

    {
        const VisualStyle& after = el->GetResolvedStyle().Visual;
        const bool inheritedChanged =
            inheritedBefore.Color != after.Color ||
            inheritedBefore.FontSize != after.FontSize ||
            inheritedBefore.FontWeight != after.FontWeight ||
            inheritedBefore.FontStyle != after.FontStyle ||
            inheritedBefore.FontVariant != after.FontVariant ||
            inheritedBefore.FontFamily != after.FontFamily ||
            inheritedBefore.TextAlign != after.TextAlign ||
            inheritedBefore.WordBreak != after.WordBreak ||
            inheritedBefore.OverflowWrap != after.OverflowWrap ||
            inheritedBefore.WhiteSpace != after.WhiteSpace ||
            inheritedBefore.LineHeight != after.LineHeight ||
            inheritedBefore.LetterSpacing != after.LetterSpacing ||
            inheritedBefore.Cursor != after.Cursor ||
            inheritedBefore.Visible != after.Visible ||
            inheritedBefore.PointerEvents != after.PointerEvents;
        if (inheritedChanged && !el->GetChildren().empty())
            el->MarkDirtySubtree(UIElement::VisualDirty);
    }

    // ResolveCascadeForElement cleared StyleDirty, which is correct for paint-only
    // pseudo states (the common case: :hover/:active background/color). If this state
    // changed a LAYOUT input (rare, e.g. :active { padding }), the element still needs a Yoga re-solve,
    // which only happens via the StyleDirty -> SolveAndApplyLayout path; re-mark it so
    // next frame's solve applies the layout (today's one-frame layout behavior),
    // instead of stranding the change with StyleDirty cleared.
    RetainedYogaNode* retained = el->m_YogaState.get();
    if (!retained->HasPrevInputs || retained->PrevLayout != el->GetResolvedStyle().Layout)
        el->MarkDirty(UIElement::StyleDirty);
}

// ---------------------------------------------------------------------------
// BuildYogaRecursive -- the recursive Yoga tree builder
// (extracted from the buildYoga lambda in Update())
// ---------------------------------------------------------------------------

BuiltNode UIManager::BuildYogaRecursive(UIElement* el, const ResolvedStyle* parentStyle,
                                        size_t childIndex, int32_t parentSheetSetIdx,
                                        UpdateContext& ctx, CascadeShareContext* shareCtx)
{
    (void)childIndex;

    if (!el)
    {
        // Defensive: UI trees should not contain null children, but avoid UAF/null deref
        // if a control hands us an empty slot.
        return BuiltNode{nullptr, 0};
    }

    // P4 sibling sharing (MT-4.1): the share cache + active flag travel in an
    // explicit CascadeShareContext instead of manager members. The root build
    // pass (parentStyle == nullptr) owns the context as a stack local and binds
    // it to the manager-owned cache (kept across frames for allocation reuse);
    // descendants receive the same pointer. Donors are valid only within one
    // root pass — the share key cannot see inherited CONTENT changes, which is
    // exactly what ConvergePostLayout re-resolves for — and the local's scope
    // is precisely that pass (covering early returns), so no reset flag is
    // needed. MT-4.2 gives each subtree worker its own context/cache.
    const bool isRootBuild = (parentStyle == nullptr);
    CascadeShareContext rootShareCtx;
    if (isRootBuild)
    {
        if (!m_CascadeShareCache)
            m_CascadeShareCache = std::make_unique<CascadeShareCache>();
        m_CascadeShareCache->Entries.clear();
        // Mirror ElementMatchesHover exactly: the hovered element and its
        // plain GetParent() ancestors all match rightmost `:hover`. Built on the
        // root cache; MT-4.2 chunks read it by pointer without copying (#215).
        // Lives on the cache (behind the unique_ptr) so the fix leaves
        // UIManager's member layout unchanged (ODR-safe for stale dependents).
        m_CascadeShareCache->HoverChain.clear();
        for (const UIElement* cur = m_Hovered; cur; cur = cur->GetParent())
            m_CascadeShareCache->HoverChain.insert(cur);
        rootShareCtx.Cache = m_CascadeShareCache.get();
        rootShareCtx.Hover = &m_CascadeShareCache->HoverChain;
        rootShareCtx.Active = true;
        shareCtx = &rootShareCtx;
    }

    // === MT-4.0 Stage 1: Collect (UI thread) ============================
    // Ensure the Yoga node exists, resolve + intern the effective sheet set,
    // and compute the sheet-identity hashes. A false return means node-create
    // OOM — the parent can't include us either way.
    YogaBuildItem item;
    if (!CollectYogaInputs(el, parentSheetSetIdx, ctx, item))
        return BuiltNode{nullptr, 0};

    // --- Subtree-skip gate (Stage 7 step 7 option 2) ---------------------
    // Early-return when every dependency is verifiably clean.
    BuiltNode skipBuilt;
    if (EvaluateSubtreeSkipGate(el, item, ctx, skipBuilt))
        return skipBuilt;

    // === MT-4.0 Stage 2: Compute (element-local cascade; Yoga-free) ======
    ComputeElementCascade(el, parentStyle, ctx, item, shareCtx);

    // === MT-4.0 Stage 3: Apply (UI thread; Yoga mutations + recursion) ===
    return ApplyElementBuild(el, parentStyle, ctx, item, shareCtx);
}

// Subtree-skip gate factored out of BuildYogaRecursive. Returns true when el's
// subtree is verifiably clean and the caller must early-out with outBuilt;
// false to proceed to compute+apply. Side effects on the false path (prof
// reason counters + ctx.subtreeStructureChanges bump) are byte-identical to the
// pre-extraction inline gate.
bool UIManager::EvaluateSubtreeSkipGate(UIElement* el, const YogaBuildItem& item,
                                        UpdateContext& ctx, BuiltNode& outBuilt)
{
    RetainedYogaNode* retained = item.Retained;
    const bool isFreshNode = item.IsFreshNode;
    const bool hadChildrenDirty = item.HadChildrenDirty;
    const uint64_t localSheetsHashNow = item.LocalSheetsHashNow;
    const uint64_t parentSheetsHashNow = item.ParentSheetsHashNow;

    // Mount targets are off-tree from m_Parent's perspective, but the dirty
    // walks (PropagateSubtreeDirtyIfNeeded / MarkDirtySubtree) follow
    // GetDfsParent(), which routes through the Mount host — so SubtreeDirty is
    // trustworthy for subtrees containing Mounts and needs no gate escape.
    //
    // The hash compares are against the previous slow-path's stored values;
    // the slow path overwrites them at exit.
    // Common clean checks (apply to both visible and stable display:none fast
    // paths). Element-local dirty bits, structural triggers, and global style
    // forces all force slow path regardless of visibility.
    const bool elementClean =
        !isFreshNode &&
        retained->Node != nullptr &&
        !ctx.forceGlobalStyle &&
        !ctx.computeLayoutSignaturesAll &&
        !hadChildrenDirty &&
        !el->IsDirty(UIElement::SubtreeDirty) &&
        !el->Overrides().NeedsCascadeRerun() &&
        !el->Overrides().IsLayoutDirty() &&
        !el->Overrides().IsVisualDirty();

    const bool isDisplayNone = el->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None;

    // Visible fast-path: cached state must match the current frame's sheet set
    // / generation so re-cascade isn't needed.
    const bool subtreeIsCleanVisible =
        elementClean &&
        !isDisplayNone &&
        retained->CachedSubtreeValid &&
        retained->PrevLocalSheetsHash == localSheetsHashNow &&
        retained->PrevParentSheetsHash == parentSheetsHashNow &&
        retained->CachedStylesheetContentGen == m_StylesheetContentGeneration;

    // Stable display:none fast-path: element was display:none last frame and is
    // still display:none with no dirty bits. Skip the slow-path entry's
    // RemoveAllYogaChildren / cache-invalidation work — those are idempotent on
    // a stable hidden element. A flip back to visible will mark StyleDirty (via
    // cascade), which fails elementClean above.
    const bool subtreeIsCleanStableNone =
        elementClean &&
        isDisplayNone &&
        retained->PrevDisplayModeNone;

    if ((subtreeIsCleanVisible || subtreeIsCleanStableNone) && m_SubtreeSkipEnabled)
    {
        if (ctx.profEnabled && ctx.prof)
            ++ctx.prof->SubtreeFastPathHits;
        outBuilt = BuiltNode{retained->Node, el->GetResolvedStyle().Layout.Order,
                             /*structureDirty=*/false};
        return true;
    }
    if (ctx.profEnabled && ctx.prof)
    {
        ++ctx.prof->SubtreeSlowPathWalks;
        // Attribute which gate clause(s) failed. Bits ORd into Union; counters
        // bumped per-clause to see dominant trigger.
        auto& prof = *ctx.prof;
        const auto bump = [&](uint32_t bit, int idx) {
            prof.SubtreeSlowPathReasonUnion |= bit;
            ++prof.SubtreeSlowPathReasonCounts[idx];
        };
        if (isFreshNode || retained->Node == nullptr)              bump(1u   , 0);
        if (ctx.forceGlobalStyle || ctx.computeLayoutSignaturesAll) bump(2u  , 1);
        if (hadChildrenDirty)                                       bump(4u  , 2);
        if (el->IsDirty(UIElement::SubtreeDirty))                   bump(8u  , 3);
        if (el->Overrides().NeedsCascadeRerun())                    bump(16u , 4);
        if (el->Overrides().IsLayoutDirty())                        bump(32u , 5);
        if (el->Overrides().IsVisualDirty())                        bump(64u , 6);
        if (el->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None) bump(128u, 7);
        if (!retained->CachedSubtreeValid)                          bump(256u, 8);
        if (retained->PrevLocalSheetsHash != localSheetsHashNow)    bump(512u, 9);
        if (retained->PrevParentSheetsHash != parentSheetsHashNow)  bump(1024u, 10);
        if (retained->CachedStylesheetContentGen != m_StylesheetContentGeneration) bump(2048u, 11);
    }

    // Falling through to the slow path. Bump the structure counter when
    // anything that would invalidate downstream caches changed since the last
    // visit (fresh node, ChildrenDirty, sheet-list mismatch). The hash writes
    // happen at the slow-path exit in ApplyElementBuild.
    if (isFreshNode || hadChildrenDirty ||
        retained->PrevLocalSheetsHash != localSheetsHashNow ||
        retained->PrevParentSheetsHash != parentSheetsHashNow)
    {
        ++ctx.subtreeStructureChanges;
    }
    return false;
}


bool UIManager::CollectYogaInputs(UIElement* el, int32_t parentSheetSetIdx,
                                  UpdateContext& ctx, YogaBuildItem& item)
{
    // Track dirty flags observed before they get cleared by build/geometry stages.
    const bool hadChildrenDirty = el->IsDirty(UIElement::ChildrenDirty);
    if (el->IsDirty(UIElement::StyleDirty))
        ++ctx.dirtyStyleSeen;
    if (el->IsDirty(UIElement::LayoutDirty))
        ++ctx.dirtyLayoutSeen;
    if (el->IsDirty(UIElement::VisualDirty))
        ++ctx.dirtyVisualSeen;
    if (hadChildrenDirty)
        ++ctx.dirtyChildrenSeen;
    item.HadChildrenDirty = hadChildrenDirty;

    // Determine effective stylesheet set for this element.
    // Most elements have no local stylesheets; reuse the parent's set index.
    int32_t localSheetSetIdx = parentSheetSetIdx;
    const auto& localHandles = el->GetStylesheets();
    if (!localHandles.empty())
    {
        // Build a new set: copy parent's handles, then merge local handles.
        // Derive raw pointers from the handles so SheetSetRef::count always
        // describes all three flat arrays equally.
        static thread_local std::vector<const Stylesheet*> mergedSheets;
        static thread_local std::vector<StylesheetHandle> mergedHandlesBuf;
        mergedSheets.clear();
        mergedHandlesBuf.clear();
        if (parentSheetSetIdx >= 0)
        {
            const auto& pref = ctx.sheetSetRefs[(size_t)parentSheetSetIdx];
            mergedHandlesBuf.insert(mergedHandlesBuf.end(),
                                    ctx.flatHandles.data() + pref.offset,
                                    ctx.flatHandles.data() + pref.offset + pref.count);
        }
        for (const StylesheetHandle& h : localHandles)
        {
            if (h)
                AddSheetForAnalysis(h.get(), ctx);
        }
        MergeSheetsHandles(mergedHandlesBuf, localHandles);
        mergedSheets.reserve(mergedHandlesBuf.size());
        for (const StylesheetHandle& h : mergedHandlesBuf)
        {
            if (h)
                mergedSheets.push_back(h.get());
        }

        SheetSetRef newRef;
        newRef.offset = (uint32_t)ctx.flatSheets.size();
        newRef.count = (uint32_t)mergedSheets.size();
        ctx.flatSheets.insert(ctx.flatSheets.end(), mergedSheets.begin(), mergedSheets.end());
        ctx.flatHandles.insert(ctx.flatHandles.end(), mergedHandlesBuf.begin(), mergedHandlesBuf.end());
        for (const Stylesheet* s : mergedSheets)
            ctx.flatIndices.push_back(GetOrBuildRuleIndex(s));
        localSheetSetIdx = (int32_t)ctx.sheetSetRefs.size();
        ctx.sheetSetRefs.push_back(newRef);
    }
    // Only localHandlesSpan is needed here (for the interner). The sheet and
    // index spans are re-derived from the set index in ComputeElementCascade;
    // spans are never held across a stage boundary because ctx.flatSheets can
    // reallocate as descendants push their own merged sets.
    item.LocalSheetSetIdx = localSheetSetIdx;
    const auto localHandlesSpan = ctx.GetHandleSpan(localSheetSetIdx);

    // Retained Yoga node lives on el->m_YogaState (Stage 7 step 8).
    // Allocated lazily on first visit; persists for the element's lifetime
    // and is freed by ~RetainedYogaNode when the element destructs. Pointer
    // reuse can't happen across element lifetimes since the state is owned
    // by the element itself.
    const bool isFreshNode = (!el->m_YogaState || !el->m_YogaState->Node);
    if (isFreshNode)
    {
        if (!el->m_YogaState)
            el->m_YogaState = std::make_unique<RetainedYogaNode>();
        YGNodeRef newNode = YogaAdapter::CreateNode();
        if (!newNode)
        {
            // CreateNode failure is effectively OOM; signal the orchestrator
            // to return a null node. Order is irrelevant since the parent
            // can't include us anyway.
            return false;
        }
        el->m_YogaState->Node = newNode;
        el->m_YogaState->InstanceId = el->GetInstanceId();
        el->m_YogaNode = newNode;   // keep fast-access shadow in sync
    }
    else
    {
        el->m_YogaNode = el->m_YogaState->Node;
    }
    RetainedYogaNode* retained = el->m_YogaState.get();
    item.Retained = retained;
    item.Node = retained->Node;
    item.IsFreshNode = isFreshNode;

    // Intern this element's effective sheet span to a stable ID persisted
    // on the retained node so the effective sheet list can be resolved
    // without a top-down walk.
    // ID 0 is reserved for "empty set" — also serves as the sentinel for
    // "root inherits parent's (empty) sheets" since in that case localSheets
    // is empty. Populated here (not at exit) so the retained node reflects
    // this frame's resolved state even if the rest of BuildYogaRecursive
    // early-returns via display:none.
    retained->PersistentSheetSetId = m_SheetSetInterner.Intern(localHandlesSpan, *this);
    retained->SheetSetGeneration = m_SheetSetInterner.Generation();
    // Reset the pre-cascade content-gate cache: BuildYogaRecursive interns
    // against ctx-built handles rather than the parent's PSSID directly,
    // so we don't have a parent-PSSID value to cache here. UINT32_MAX is
    // the sentinel that forces the next pre-cascade walk visit to re-
    // validate this element via the cache-miss path.
    retained->LastSeenParentPSSID = UINT32_MAX;

    // Sheet-list hashes for the gate predicate. Compared against the
    // previous slow-path's stored values; a delta means an attach/detach/
    // reorder above us or here, forcing the slow path so the cascade
    // re-runs against the new effective sheet list.
    auto foldSheetHash = [](auto begin, auto end) -> uint64_t
    {
        uint64_t h = 0;
        for (auto it = begin; it != end; ++it)
        {
            const uint64_t p = reinterpret_cast<uint64_t>(*it);
            h ^= p + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        }
        return h;
    };

    uint64_t localSheetsHashNow = 0;
    for (const StylesheetHandle& h : el->GetStylesheets())
    {
        const uint64_t p = reinterpret_cast<uint64_t>(h.get());
        localSheetsHashNow ^= p + 0x9e3779b97f4a7c15ULL +
                              (localSheetsHashNow << 6) +
                              (localSheetsHashNow >> 2);
    }

    uint64_t parentSheetsHashNow = 0;
    if (parentSheetSetIdx >= 0)
    {
        const auto span = ctx.GetSheetSpan(parentSheetSetIdx);
        parentSheetsHashNow = foldSheetHash(span.begin(), span.end());
    }
    item.LocalSheetsHashNow = localSheetsHashNow;
    item.ParentSheetsHashNow = parentSheetsHashNow;
    return true;
}


void UIManager::ComputeElementCascade(UIElement* el, const ResolvedStyle* parentStyle,
                                     UpdateContext& ctx, YogaBuildItem& item,
                                     CascadeShareContext* shareCtx, bool forceCascade)
{
    // forceCascade is the MT-4.2 escalation re-run: the tripped worker run
    // already cleared StyleDirty (ResolveCascadeForElement tail) and the
    // override dirty bits, so a plain re-run would compute needCascade=false
    // and repair nothing. Forcing rebuilds the element's style from scratch,
    // which is exactly what the tripwire needs to restore trusted state.
    const bool needCascade =
        forceCascade || ctx.forceGlobalStyle || item.IsFreshNode ||
        el->IsDirty(UIElement::StyleDirty) ||
        el->Overrides().NeedsCascadeRerun();
    item.NeedCascade = needCascade;

    // MT-4.1 seam: the cascade must stay Yoga-free AND manager-container-free
    // so MT-4.2 can run it on a worker thread. Yoga mutation trips the guard;
    // prof counters, the CascadeComputeMs timer, and the declared-transition
    // registry decision are all recorded onto `item` and merged by the caller
    // (ApplyElementBuild) on the UI thread. The share cache written here is
    // per-context state (shareCtx), which stays worker-local.
    struct ComputeGuardScope
    {
        ComputeGuardScope() { SetCascadeComputeActive(true); }
        ~ComputeGuardScope() { SetCascadeComputeActive(false); }
    } computeGuard;

    if (needCascade)
    {
        bool shared = false;
        {
            // Time into the item (merged into prof->CascadeComputeMs by the
            // caller); a worker can't touch the shared prof frame.
            ScopedSectionTimer _tCascade(ctx.profEnabled && ctx.prof != nullptr,
                                         &item.CascadeComputeMs);
            // Spans re-derived from the set index (never held across the
            // collect boundary; ctx.flatSheets can reallocate mid-pass).
            shared = ResolveCascadeForElement(el,
                                              ctx.GetSheetSpan(item.LocalSheetSetIdx),
                                              ctx.GetIndexSpan(item.LocalSheetSetIdx),
                                              parentStyle,
                                              shareCtx,
                                              item.TransitionDecision);
        }
        item.DidShareCascade = shared;
    }

    const bool overridesDirty =
        el->Overrides().IsLayoutDirty() || el->Overrides().IsVisualDirty();
    item.OverridesDirty = overridesDirty;

    if (needCascade || overridesDirty)
    {
        FinalizeElementStyle(el);
    }
}


BuiltNode UIManager::ApplyElementBuild(UIElement* el, const ResolvedStyle* parentStyle,
                                       UpdateContext& ctx, const YogaBuildItem& item,
                                       CascadeShareContext* shareCtx)
{
    // Local aliases: identical names/values to the pre-split locals so the
    // apply body below is byte-for-byte the original code.
    const ResolvedStyle& cs = el->GetResolvedStyle();
    YGNodeRef n = item.Node;
    RetainedYogaNode* retained = item.Retained;
    RetainedYogaNode& retainedNode = *retained;
    const bool isFreshNode = item.IsFreshNode;
    const bool hadChildrenDirty = item.HadChildrenDirty;
    const bool needCascade = item.NeedCascade;
    const bool overridesDirty = item.OverridesDirty;
    const int32_t localSheetSetIdx = item.LocalSheetSetIdx;
    const uint64_t localSheetsHashNow = item.LocalSheetsHashNow;
    const uint64_t parentSheetsHashNow = item.ParentSheetsHashNow;

    // MT-4.1: merge the compute stage's deferred effects on the UI thread, in
    // DFS/item order, before any child recurses — reproducing the exact pre-4.1
    // ordering (the counters were bumped and m_TransitioningElements mutated
    // inside the cascade, i.e. right here in pre-order, before descending).
    // needCascade gates the CascadeCalls/CascadeCallsBuildYoga pair; the timer
    // and share flag ride the item. Nothing reads these between compute and
    // now, so the merged result is bit-identical to the inline writes.
    if (ctx.profEnabled && ctx.prof)
    {
        if (needCascade)
        {
            ++ctx.prof->CascadeCalls;
            ++ctx.prof->CascadeCallsBuildYoga;
        }
        if (item.DidShareCascade)
            ++ctx.prof->CascadeShared;
        ctx.prof->CascadeComputeMs += item.CascadeComputeMs;
    }
    ApplyTransitionRegistration(el, item.TransitionDecision);

    // display:none ↔ visible transitions change ctx.nodes size
    // (descendants appear/disappear via the display:none early-return
    // branch below) without ChildrenDirty firing. Bump the structure
    // counter here so downstream skip gates (indicesAndClips) know the
    // node list changed. `cs` holds the currently-resolved style (whether
    // cascade ran this frame via needCascade above, or was inherited from
    // a prior frame) — either way it's the right value to compare.
    const bool nowDisplayNone = (cs.Layout.DisplayMode == DisplayMode::None);
    // When coming back from display:none, RemoveAllYogaChildren was called on
    // the previous hide pass, so Yoga children are detached. Force re-insertion
    // even if no UIElement ChildrenDirty fired (it was cleared by the hide pass).
    const bool becomingVisible = retainedNode.PrevDisplayModeNone && !nowDisplayNone;
    if (retainedNode.PrevDisplayModeNone != nowDisplayNone)
    {
        ++ctx.subtreeStructureChanges;
        // Display flip → bump tree-generation so the next Update sees
        // structural change and forces a full rebuild.
        NotifyTreeStructureChanged();
    }
    retainedNode.PrevDisplayModeNone = nowDisplayNone;

    // Whether this element establishes block flow lands on its CHILDREN's Yoga
    // input (ApplyBlockFlowShrinkDefault), and flipping it leaves each child's
    // own style untouched — so a child carrying no dirty bit of its own would
    // be served by the subtree-skip gate and never re-pushed. Mark them here,
    // before the child loop further down visits them. `PrevLayout` still holds
    // the previous push's value; the overwrite happens later in this function.
    if (retainedNode.HasPrevInputs &&
        EstablishesBlockFlow(retainedNode.PrevLayout.DisplayMode) !=
            EstablishesBlockFlow(cs.Layout.DisplayMode))
    {
        constexpr unsigned kFlipFlags = UIElement::StyleDirty | UIElement::LayoutDirty;
        for (const auto& childEntry : el->GetChildren())
        {
            if (childEntry)
                childEntry->MarkDirty(kFlipFlags);
        }
        // GetChildren() excludes a Mount's portal target, which is still a Yoga
        // child of this node and reads this style as its parent.
        if (el->Kind() == UIElementKind::Mount)
        {
            if (UIElement* tgt = static_cast<Mount*>(el)->GetTarget())
                tgt->MarkDirty(kFlipFlags);
        }
    }

    if (!ctx.computeLayoutSignaturesThisFrame && needCascade)
    {
        ++ctx.cssRecomputeWhileSignaturePassDisabled;
    }

    // Reported to the parent via BuiltNode so an order-only mutation (e.g.
    // an Overrides().Set(Style::Order), which raises just LayoutDirty)
    // still triggers the parent's child re-attach.
    bool orderChangedSinceLastBuild = false;

    if (ctx.computeLayoutSignaturesThisFrame || needCascade || overridesDirty)
    {
        const bool needLayoutSigForThisNode =
            ctx.computeLayoutSignaturesAll ||
            needCascade || overridesDirty ||
            !retainedNode.HasPrevInputs ||
            el->IsDirty(UIElement::LayoutDirty) ||
            el->IsDirty(UIElement::ChildrenDirty);

        if (needLayoutSigForThisNode)
        {
            // Intrinsic measurement (for text controls). This affects layout and
            // must be accounted for in the layout signature.
            bool hasMeasure = false;
            bool allowWrapForMeasure = false;
            uint64_t measureTextHash = 0;
            float measureFontSize = 0.0f;
            float measureLineHeight = 0.0f;
            // Text measurement is opt-in via ITextMeasurable so UIManager/Yoga can size
            // leaf text ctx.nodes without UIManager knowing about concrete control types.
            //
            // IMPORTANT: Do not gate this on font availability. When fonts are resolved
            // asynchronously by the host, we still need a conservative non-zero fallback
            // measure so Yoga layout doesn't collapse (stacking/overlap). MeasureTextFn
            // handles the atlas-missing case.
            const bool allowMeasure = el->GetChildren().empty();
            if (allowMeasure)
            {
                if (auto* textMeasurable = el->GetTextMeasurable())
                {
                    ITextMeasurable::TextMeasureInfo tm{};
                    textMeasurable->GetTextMeasureInfo(tm, cs);
                    if (tm.HasText)
                    {
                        // Defensive: retained ctx.nodes may still have children (e.g. from a stale entry).
                        // Yoga forbids children on ctx.nodes with measure functions.
                        if (YGNodeGetChildCount(n) > 0)
                        {
                            RemoveAllYogaChildren(n);
                        }

                        measureTextHash = HashString64(tm.Text);
                        const bool tmHasNewlines = (tm.Text.find('\n') != std::string::npos);
                        allowWrapForMeasure = textMeasurable->AllowWrapForMeasure();
                        if (cs.Visual.WhiteSpace == WhiteSpace::NoWrap ||
                            cs.Visual.WhiteSpace == WhiteSpace::Pre)
                        {
                            allowWrapForMeasure = false;
                        }

                        // Stable per-element measure context (Slice 2 step 3).
                        // Lives on the RetainedYogaNode; its address doesn't
                        // move across frames so YGNodeSetContext stays valid
                        // even if the fast path skips re-binding next frame.
                        if (!retained->MeasureCtx)
                            retained->MeasureCtx = std::make_unique<TextMeasureCtx>();
                        TextMeasureCtx* mctx = retained->MeasureCtx.get();

                        FontAtlas* measureAtlas = m_FontAtlas.get();
                        if (!measureAtlas && !m_FontAtlases.empty())
                        {
                            measureAtlas = m_FontAtlases.begin()->second.get();
                        }
                        if (cs.Visual.FontFamily && !cs.Visual.FontFamily->empty())
                        {
                            for (const auto& fam : *cs.Visual.FontFamily)
                            {
                                if (auto* fa = GetOrRequestFontFamilyInternal(fam, cs.Visual.FontWeight, cs.Visual.FontStyle, cs.Visual.FontVariant))
                                {
                                    measureAtlas = fa;
                                    break;
                                }
                            }
                        }
                        if (mctx)
                        {
                            mctx->Atlas = measureAtlas; // may be null; MeasureTextFn handles fallback
                            mctx->Text = std::move(tm.Text);
                            mctx->LineHeight = cs.Visual.LineHeight;
                            // Device px, like PixelSize — the FontAtlas unit.
                            mctx->LetterSpacingPx = cs.Visual.LetterSpacing * ctx.contentScale;
                            mctx->TextHash = measureTextHash;
                            mctx->HasNewlines = tmHasNewlines;
                            mctx->AllowWrap = allowWrapForMeasure;
                            mctx->BreakPolicy =
                                allowWrapForMeasure
                                    ? ResolveTextBreakPolicy(cs.Visual.WordBreak, cs.Visual.OverflowWrap)
                                    : WordBreak::Normal;
                        }
                        // Measure using either an explicit size from the control or
                        // the same font-size the element will render with.
                        measureFontSize = tm.ExplicitFontSize > 0.0f ? tm.ExplicitFontSize : cs.Visual.FontSize;
                        measureLineHeight = cs.Visual.LineHeight;
                        if (mctx)
                        {
                            // pixelSize is physical so the font atlas is queried at the
                            // correct resolution. MeasureTextFn divides results by
                            // contentScale to return logical dimensions to Yoga.
                            mctx->PixelSize = std::max(1.0f, measureFontSize * ctx.contentScale);
                            mctx->ContentScale = ctx.contentScale;
                            // A control's explicit size overrides the visual font-size
                            // for measurement; unitless line-height must resolve
                            // against the size actually measured with. Every control
                            // currently reports 0 here (meaning "use the style"), so
                            // this equals Visual.FontSize and matches what primitive
                            // generation resolves against. A control that starts
                            // reporting a real explicit size must also carry it into
                            // EmitTextPrimitives, or layout and rendering would size
                            // the same text differently.
                            mctx->FontSize = measureFontSize;
                        }
                        YGNodeSetContext(n, mctx);
                        YGNodeSetMeasureFunc(n, MeasureTextFn);
                        YGNodeSetBaselineFunc(n, BaselineTextFn);
                        hasMeasure = true;
                    }
                }
            }
            // Clear any stale measurement callback/context when measurement is not active.
            if (!hasMeasure)
            {
                YGNodeSetMeasureFunc(n, nullptr);
                YGNodeSetBaselineFunc(n, nullptr);
                YGNodeSetContext(n, nullptr);
            }

            MeasureInputs curMeasure{};
            curMeasure.HasMeasure = hasMeasure;
            if (hasMeasure)
            {
                curMeasure.TextHash = measureTextHash;
                curMeasure.FontSize = measureFontSize;
                curMeasure.LineHeight = measureLineHeight;
                curMeasure.LetterSpacing = cs.Visual.LetterSpacing;
                curMeasure.VisualFontSize = cs.Visual.FontSize;
                curMeasure.VisualLineHeight = cs.Visual.LineHeight;
                curMeasure.AllowWrap = allowWrapForMeasure;
                curMeasure.BreakPolicy =
                    allowWrapForMeasure
                        ? ResolveTextBreakPolicy(cs.Visual.WordBreak, cs.Visual.OverflowWrap)
                        : WordBreak::Normal;
                curMeasure.AtlasId = (retainedNode.MeasureCtx && retainedNode.MeasureCtx->Atlas)
                                         ? retainedNode.MeasureCtx->Atlas->GetAtlasId()
                                         : 0;
            }

            auto* wp = (el->Kind() == UIElementKind::WeightedPane) ? static_cast<WeightedPane*>(el) : nullptr;
            const float effectiveFlexGrow = (wp ? std::max(0.0f, wp->GetFlexWeight()) : cs.Layout.FlexGrow);

            // Not derivable from cs.Layout: the container decides it, and this
            // element's own style is unchanged when the container flips.
            const bool parentBlockFlow =
                parentStyle && EstablishesBlockFlow(parentStyle->Layout.DisplayMode);

            const bool inputsChanged =
                !retainedNode.HasPrevInputs ||
                retainedNode.PrevLayout != cs.Layout ||
                retainedNode.PrevMeasure != curMeasure ||
                retainedNode.PrevFlexGrowOverride != effectiveFlexGrow ||
                retainedNode.PrevParentBlockFlow != parentBlockFlow;

            orderChangedSinceLastBuild =
                retainedNode.HasPrevInputs && retainedNode.PrevLayout.Order != cs.Layout.Order;

            if (inputsChanged)
            {
                ScopedSectionTimer _tYogaApply(ctx.profEnabled && ctx.prof != nullptr,
                                               ctx.prof ? &ctx.prof->YogaApplyMs : nullptr);
                retainedNode.PrevLayout = cs.Layout;
                retainedNode.PrevMeasure = curMeasure;
                retainedNode.PrevFlexGrowOverride = effectiveFlexGrow;
                retainedNode.PrevParentBlockFlow = parentBlockFlow;
                retainedNode.HasPrevInputs = true;
                ctx.anyLayoutSignatureChanged = true;
                YogaAdapter::ApplyStyle(n, cs);
                ApplyBlockFlowShrinkDefault(n, cs, parentStyle);
                if (wp)
                {
                    YGNodeStyleSetFlexGrow(n, std::max(0.0f, effectiveFlexGrow));
                }
                // YGNodeSetMeasureFunc does not dirty the node when the same function
                // pointer is re-set (Yoga 3.x). Explicitly dirty the node so
                // CalculateLayout calls MeasureTextFn with the updated text context
                // rather than using the cached zero-width result from a previous pass
                // (e.g. when a pooled row transitions from empty to a real entity name).
                if (hasMeasure)
                {
                    YGNodeMarkDirty(n);
                }
            }
        }

        // We've consumed any layout-dirty request for this element (the signature
        // comparison above will decide whether a Yoga solve is needed).
        el->ClearDirty(UIElement::LayoutDirty);
    }

    if (el->IsDirty(UIElement::ChildrenDirty))
        ctx.anyChildrenDirty = true;

    // CSS semantics: display:none removes the entire subtree from layout/paint.
    // Virtualized controls frequently hide pooled rows by toggling display:none
    // on the row container. If we still traverse/build children here, their Yoga
    // layout can collapse to (0,0,0,0) while their paint style (e.g. folder icon
    // background-image on .tree-title) remains, causing a "ghost icon" drawn at
    // the origin that then gets cached. Fix by detaching Yoga children and
    // skipping subtree traversal when display:none is active.
    if (cs.Layout.DisplayMode == DisplayMode::None)
    {
        RemoveAllYogaChildren(n);
        el->ClearDirty(UIElement::ChildrenDirty);
        // Invalidate the subtree-skip gate — children are detached; on
        // re-show the element re-cascades from scratch and would otherwise
        // serve a stale fast-path skip if no descendant re-dirties.
        retained->CachedSubtreeValid = false;
        retained->CachedStylesheetContentGen = 0;
        // Clear SubtreeDirty on self so ancestors can clear theirs next frame.
        // Without this, a display:none element whose descendants were dirtied
        // while visible keeps SubtreeDirty set forever — the ancestor-chain
        // clear is gated on "no child still has SubtreeDirty", so one hidden
        // dropdown/panel is enough to force the whole ancestor chain to
        // slow-path every frame in perpetuity. Own StyleDirty/LayoutDirty
        // stay as-is; a transition back to visible marks StyleDirty anyway,
        // which re-enters the full slow path and re-cascades.
        //
        // INVARIANT CAVEAT: this clear makes "SubtreeDirty set => all DFS
        // ancestors set" hold only for VISIBLE chains. Descendants (and
        // Mount targets) of a hidden element keep their bits while the
        // hidden ancestor's is cleared, so new dirt below it early-stops at
        // the first stale bit and does not re-mark the chain. Safe today:
        // un-hiding forces the slow path, which descends unconditionally
        // and finds the persisted flags. Anything new that consumes
        // SubtreeDirty (dirty-range uploads, O(changed) commit walks) must
        // not assume the invariant holds through hidden subtrees.
        el->ClearDirty(UIElement::SubtreeDirty);
        return BuiltNode{n, cs.Layout.Order, isFreshNode, orderChangedSinceLastBuild};
    }

    const auto& ch = el->GetChildren();
    // Record start offset into the shared flat buffer; our entries live at
    // [childStart .. s_OrderedChildrenBuf.size()) and are popped when done.
    const size_t childStart = s_OrderedChildrenBuf.size();
    // Track whether any structural change occurred among children. If none did,
    // and our own node wasn't freshly created / force-rebuilt, we can skip the
    // InsertChildrenSortedByOrder call entirely (its internal fast-path is cheap
    // but still an O(n) comparison per element — summed across the tree it adds up).
    bool anyChildStructureChange = false;

    // Cascade each child in order. Donor-first sibling sharing flows through the
    // shared cascade context (shareCtx) — a same-key sibling copies the donor's
    // snapshot rather than re-resolving (P4d).
    const bool propagateSiblingDirty = m_StyleAnalysis.UsesSiblingCombinators;
    bool anyPriorSiblingStyleDirty = false;
    for (size_t i = 0; i < ch.size(); ++i)
    {
        UIElement* childEl = ch[i].get();
        if (!childEl)
            continue;
        if (propagateSiblingDirty && anyPriorSiblingStyleDirty && childEl)
        {
            // Conservative invalidation for sibling-combinator selectors (+/~):
            // changes to a preceding sibling can affect matching for following siblings.
            childEl->MarkDirty(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
        }
        // Capture StyleDirty BEFORE the recursive call (ResolveCascadeForElement
        // inside clears it). StyleDirty implies the child's CSS 'order' may have
        // changed, so our child list must be re-sorted/inserted.
        const bool childStyleDirtyBefore = childEl->IsDirty(UIElement::StyleDirty);
        if (propagateSiblingDirty && childStyleDirtyBefore)
        {
            anyPriorSiblingStyleDirty = true;
        }
        // Build the child's node and use its already-computed CSS 'order'
        // (computed with the child's effective stylesheet list, including
        // its own attached sheets) to drive insertion order.
        BuiltNode childBuilt = BuildYogaRecursive(childEl, &cs, i, localSheetSetIdx, ctx, shareCtx);
        if (childBuilt.node)
            s_OrderedChildrenBuf.push_back({childBuilt.node, childBuilt.order, i});
        if (childStyleDirtyBefore || childBuilt.structureDirty || childBuilt.orderChanged)
            anyChildStructureChange = true;
    }
    // Include Mount portal target as a Yoga child (always after normal children).
    if (el->Kind() == UIElementKind::Mount)
    {
        auto* m = static_cast<Mount*>(el);
        if (UIElement* tgt = m->GetTarget())
        {
            const bool tgtStyleDirtyBefore = tgt->IsDirty(UIElement::StyleDirty);
            BuiltNode tgtBuilt = BuildYogaRecursive(tgt, &cs, ch.size(), localSheetSetIdx, ctx, shareCtx);
            if (tgtBuilt.node)
            {
                s_OrderedChildrenBuf.push_back({tgtBuilt.node, std::numeric_limits<int>::max(), ch.size()});
            }
            if (tgtStyleDirtyBefore || tgtBuilt.structureDirty || tgtBuilt.orderChanged)
                anyChildStructureChange = true;
        }
    }

    // Only re-attach the Yoga child list if something that could affect it
    // actually changed. A freshly created node starts with zero Yoga children
    // so we must insert; force-global-style forces a rebuild everywhere.
    // becomingVisible: the element just came out of display:none, so its Yoga
    // children were detached on the hide pass (RemoveAllYogaChildren) and must
    // be re-attached now — ChildrenDirty was already cleared by that hide pass.
    const bool needInsert =
        hadChildrenDirty || isFreshNode || ctx.forceGlobalStyle || anyChildStructureChange || becomingVisible;
    if (needInsert)
    {
        YogaAdapter::InsertChildrenSortedByOrder(
            n,
            std::span<const YogaAdapter::ChildWithOrder>(
                s_OrderedChildrenBuf.data() + childStart,
                s_OrderedChildrenBuf.size() - childStart));
    }
    // Pop our entries (shared buffer acts as a stack across recursion levels).
    s_OrderedChildrenBuf.resize(childStart);
    el->ClearDirty(UIElement::ChildrenDirty);

    // Slow-path exit: prime the subtree-skip gate so the next visit can
    // early-out. We commit the freshly-observed sheet hashes + content
    // generation here so the gate's equality checks pass on the next
    // clean frame. CachedSubtreeValid is the load-bearing flag that
    // refuses to skip until at least one slow-path build has run.
    retained->CachedSubtreeValid = true;
    retained->CachedStylesheetContentGen = m_StylesheetContentGeneration;
    retained->PrevLocalSheetsHash = localSheetsHashNow;
    retained->PrevParentSheetsHash = parentSheetsHashNow;

    // Clear SubtreeDirty on this element if the whole subtree rooted here is
    // now clean. Without this clear the summary bit becomes "sticky": once any
    // descendant dirties, every ancestor keeps SubtreeDirty forever and the
    // fast path never fires. Eligibility: self has no layout-affecting dirty
    // flags remaining AND no UIElement child still has SubtreeDirty.
    {
        const unsigned kLayoutAffecting =
            UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::ChildrenDirty;
        const bool selfClean =
            (el->IsDirty(kLayoutAffecting) == false) &&
            !el->Overrides().NeedsCascadeRerun() &&
            !el->Overrides().IsLayoutDirty() &&
            !el->Overrides().IsVisualDirty();
        if (selfClean)
        {
            bool anyChildSubtreeDirty = false;
            for (const auto& childEntry : el->GetChildren())
            {
                if (childEntry && childEntry->IsDirty(UIElement::SubtreeDirty))
                {
                    anyChildSubtreeDirty = true;
                    break;
                }
            }
            // Portal-aware: a Mount's summary bit also covers its target
            // subtree. The target was visited above (post-order), so its
            // SubtreeDirty is already cleared when it converged — this only
            // holds the bit when the target genuinely didn't.
            if (!anyChildSubtreeDirty && el->Kind() == UIElementKind::Mount)
            {
                if (UIElement* tgt = static_cast<Mount*>(el)->GetTarget())
                    anyChildSubtreeDirty = tgt->IsDirty(UIElement::SubtreeDirty);
            }
            if (!anyChildSubtreeDirty)
            {
                el->ClearDirty(UIElement::SubtreeDirty);
            }
        }
    }

    return BuiltNode{n, cs.Layout.Order, isFreshNode, orderChangedSinceLastBuild};
}


// ---------------------------------------------------------------------------
// Clip / z-index / scroll helper methods (extracted from Update lambdas)
// ---------------------------------------------------------------------------

// Stage 7 step 7.2: RebuildNodeIndices was a write-only nop after every
// reader of nodeIndexByYG / nodeIndexByElement / nodesByInstanceId was
// retired. Function and forward declaration deleted.

void UIManager::RebuildTypedNodeLists(UpdateContext& ctx)
{
    ctx.scrollViews.clear();
    ctx.layoutOverrideNodes.clear();
    ctx.scrollViews.reserve(16);
    ctx.layoutOverrideNodes.reserve(16);
    if (!m_Root)
        return;

    // ScrollViews come from the typed registry (maintained by
    // OnOwnerManagerChanged) — no per-node dynamic_cast. The old walk
    // excluded scroll views inside display:none subtrees AND, as a
    // downward walk, anything detached from the root (inactive dock tabs
    // keep their owner, so they stay registered). Reproduce both with a
    // DFS-ancestor scan per entry (registry is small, chains shallow,
    // GetDfsParent crosses Mount portals). Consumers are
    // order-independent, so registration order is fine.
    for (ScrollView* sv : m_ScrollViewRegistry)
    {
        bool hidden = false;
        UIElement* top = sv;
        for (UIElement* p = sv; p; p = p->GetDfsParent())
        {
            top = p;
            if (p->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None)
            {
                hidden = true;
                break;
            }
        }
        if (!hidden && top == m_Root.get())
            ctx.scrollViews.push_back(sv);
    }

    // The layout-override predicate (position:absolute) is cascade output,
    // not a stable type — it stays a walk, skipping display:none subtrees
    // (their layout rects are stale and consumers gate on displayMode
    // anyway).
    std::function<void(UIElement*)> walk = [&](UIElement* el) {
        if (!el)
            return;
        if (el->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None)
            return;
        Mathematics::Rect absoluteRect{};
        if (UI::Layout::TryGetAbsolutePosition(*el, absoluteRect))
            ctx.layoutOverrideNodes.push_back({el});
        for (const auto& ch : el->GetChildren())
            walk(ch.get());
        if (el->Kind() == UIElementKind::Mount)
            if (UIElement* tgt = static_cast<Mount*>(el)->GetTarget())
                walk(tgt);
    };
    walk(m_Root.get());
}

void UIManager::CommitLayoutRects(UIElement* root, bool incremental)
{
#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
    // The single Yoga->element rect commit (slice E2 — replaces five
    // hand-rolled copies). Walks visible elements (children + Mount
    // targets), commits the absolute rect; skips display:none subtrees
    // (Yoga doesn't solve them; their stale rects are correct to keep).
    //
    // Incremental mode (the steady-state SolveAndApplyLayout caller)
    // routes rect changes to the primitive drain: elements with a slot
    // range get re-emitted in place, elements without one (newly visible)
    // escalate to a full regen. Blunt mode (post-rebuild / converge
    // callers) relies on the full regen those paths already trigger.
    if (!root)
        return;
    ++m_LayoutCommitGeneration;
    bool needsFullForLayout = false;
    // The parent's absolute origin is carried down the walk (root's parent
    // is 0,0), so each node's absolute rect is one add instead of the old
    // O(depth) ancestor walk per node. Elements without a Yoga node pass
    // the incoming origin through — their node-bearing descendants are
    // Yoga children of the nearest node-bearing ancestor, so relative
    // coordinates resolve against exactly that origin.
    auto commit = [&](auto& self, UIElement* el, float parentAbsX, float parentAbsY) -> void {
        if (!el)
            return;
        if (el->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None)
            return;
        YGNodeRef node = el->m_YogaState ? el->m_YogaState->Node : nullptr;
        float childAbsX = parentAbsX;
        float childAbsY = parentAbsY;
        if (node)
        {
            float lx = parentAbsX + YGNodeLayoutGetLeft(node);
            float ly = parentAbsY + YGNodeLayoutGetTop(node);
            // Whole-device-pixel snap for elements that consume a render target
            // 1:1 (world viewports). EDGES are rounded, never the size: rounding
            // left and right independently keeps the right edge where the solve
            // put it, so a snapped element cannot drift away from the unsnapped
            // siblings it shares a splitter with. Rects stay logical px — the
            // snap is exact in DEVICE px, which is the space the render target
            // extent and the background quad are both derived in.
            float snappedW = 0.0f;
            float snappedH = 0.0f;
            const bool snapRect = el->SnapsRectToDevicePixels();
            if (snapRect)
            {
                const float cs = std::max(0.01f, m_ContentScale);
                const float left = std::round(lx * cs);
                const float top = std::round(ly * cs);
                const float right = std::round((lx + YGNodeLayoutGetWidth(node)) * cs);
                const float bottom = std::round((ly + YGNodeLayoutGetHeight(node)) * cs);
                lx = left / cs;
                ly = top / cs;
                snappedW = std::max(0.0f, right - left) / cs;
                snappedH = std::max(0.0f, bottom - top) / cs;
            }

            // E3 prune. hasNewLayout=false proves the node's RELATIVE
            // layout (and its subtree's) is unchanged — Yoga repositions
            // clean cache-hit children from the parent side without
            // descending, so a false flag says nothing about ABSOLUTE
            // position. Prune only when the committed origin also matches:
            // unchanged relative layout + unchanged origin => every
            // absolute rect below is unchanged (induction — children would
            // fail their own origin check otherwise). A moved-but-clean
            // subtree recommits fully, which also keeps scroll/override
            // translation deltas uniform across their subtrees.
            // The snapped arm extends the prune with a SIZE test: turning the
            // snap on is not a solve change, so a viewport whose origin already
            // sat on a whole device pixel would otherwise keep its fractional
            // width until something else moved it.
            const bool hasNew = YGNodeGetHasNewLayout(node);
            const bool originMoved = el->GetLayoutX() != lx || el->GetLayoutY() != ly;
            const bool snappedSizeMoved =
                snapRect &&
                (el->GetLayoutWidth() != snappedW || el->GetLayoutHeight() != snappedH);
            if (!hasNew && !originMoved && !snappedSizeMoved)
                return;
            if (hasNew)
                YGNodeSetHasNewLayout(node, false);
            el->m_YogaState->LastCommitGen = m_LayoutCommitGeneration;

            const float nw = snapRect ? snappedW : YGNodeLayoutGetWidth(node);
            const float nh = snapRect ? snappedH : YGNodeLayoutGetHeight(node);
            // Percentage paddings resolve against the containing block's width
            // during the solve (all four edges, width — that is CSS, not a
            // Yoga quirk). The style struct keeps the raw percent number, so
            // the resolved lengths only exist on the node: commit them with
            // the rect so post-layout consumers never have to re-derive a
            // containing block Yoga already picked.
            UILayoutAccess::SetLayoutPadding(*el, Box4{YGNodeLayoutGetPadding(node, YGEdgeTop),
                                      YGNodeLayoutGetPadding(node, YGEdgeRight),
                                      YGNodeLayoutGetPadding(node, YGEdgeBottom),
                                      YGNodeLayoutGetPadding(node, YGEdgeLeft)});
            if (incremental)
            {
                const bool rectChanged =
                    originMoved ||
                    el->GetLayoutWidth() != nw || el->GetLayoutHeight() != nh;
                UILayoutAccess::SetLastLayoutRect(*el, lx, ly, nw, nh);
                if (rectChanged)
                {
                    if (el->m_PrimitiveRangeCap > 0)
                        PushPrimitiveDataDirty(el);
                    else
                        needsFullForLayout = true;
                }
            }
            else
            {
                UILayoutAccess::SetLastLayoutRect(*el, lx, ly, nw, nh);
            }
            childAbsX = lx;
            childAbsY = ly;
        }
        for (const auto& ch : el->GetChildren())
            self(self, ch.get(), childAbsX, childAbsY);
        if (UIElement* tgt = el->GetMountTarget())
            self(self, tgt, childAbsX, childAbsY);
    };
    commit(commit, root, 0.0f, 0.0f);
    if (needsFullForLayout)
        MarkPrimitivesNeedRegen(0x80u);
#else
    (void)root;
    (void)incremental;
#endif
}

bool UIManager::FlushScrollCallbacks(UpdateContext& ctx)
{
    // The flush runs SUBSCRIBER code — the member callback and, since scroll offsets became a
    // first-class event, every handler-table subscriber. Raise the dispatch depth across the
    // whole loop, because without it UIElement::RemoveChild does not defer: a handler removing
    // its ScrollView (or an ancestor of it) would destroy the element under
    // FlushPendingScrollChanged's own `this`, and leave the remaining raw pointers in
    // ctx.scrollViews dangling for the rest of the loop.
    //
    // Across the LOOP rather than per element: the subtree a handler removes can contain a
    // ScrollView this loop has not reached yet.
    //
    // ONE DELIBERATE SIDE EFFECT, on the virtualization hot path. ListView, GridView and
    // TreeView drive UpdateVirtualization from their m_OnScrollChanged, and they pass
    // `allowShrink = !UIElement::IsInEventDispatch()` — so raising the depth here defers pool
    // SHRINK (VirtualWindowCore::DestroyTailSlot) out of scroll flushes to the next safe update.
    // That is the conservative direction and it is what VirtualWindowCore.h:93-98 always said
    // this path did; before this guard the claim was simply not true during a flush. Binds,
    // rebinds and the shrink hysteresis counter are unaffected — only the destruction moves.
    // Armed from both ends: ControlValueEventTests' ScrollFlushRaisesTheDispatchDepth pins the
    // input, VirtualWindowCoreTests' ShrinkBlockedWhileInDispatchFiresOnNextSafeUpdate pins the
    // deferral it produces.
    UIElement::SetInEventDispatch(true);
    struct DispatchDepthScope
    {
        ~DispatchDepthScope() { UIElement::SetInEventDispatch(false); }
    } depthScope;

    bool anyFlushed = false;
    for (ScrollView* sv : ctx.scrollViews)
    {
        if (!sv)
            continue;
        if (sv->FlushPendingScrollChanged())
            anyFlushed = true;
    }
    return anyFlushed;
}

namespace
{
// Local clip-stack state for the RebuildClipCaches tree walk. Mirrors
// the HitClipState used by the hit-tester so the two passes converge
// on the same effective clip semantics.
struct DebugClipWalkState
{
    bool has = false;
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

void WalkDebugClipRecursive(UIElement* el, DebugClipWalkState inherited,
                            std::unordered_map<UIElement*, UIManager::DebugClipRect>& out)
{
    if (!el)
        return;
    const ResolvedStyle& rs = el->GetResolvedStyle();
    // Mirror BuildYogaRecursive's display:none early-return so the cache
    // doesn't pick up stale layout rects from collapsed subtrees.
    if (rs.Layout.DisplayMode == DisplayMode::None)
        return;

    // Overlay layers (Dropdown, Tooltip, Modal, DragPreview) paint outside
    // their tree parent's clip, so reset the inherited clip on entry — the
    // overlay element and its descendants start fresh.
    if (el->GetOverlayLayer() != OverlayLayer::None)
        inherited = DebugClipWalkState{};

    // Record the clip applied TO this element (matches NodeRec.hasClip
    // semantics: parent's effective clip after the parent-overflow + own
    // overlay-reset steps).
    if (inherited.has)
        out.insert_or_assign(el, UIManager::DebugClipRect{inherited.x, inherited.y, inherited.w, inherited.h});

    // Compute the clip propagated to children: inherit + own overflow:hidden
    // contribution. Mount portal target inherits the Mount host's child
    // clip — the same edge BuildYogaRecursive baked into NodeRec.parentIndex.
    DebugClipWalkState childClip = inherited;
    if (rs.Layout.Overflow == Overflow::Hidden)
    {
        // Report the region paint actually clips to: the padding box.
        const Box4& bw = rs.Layout.BorderWidth;
        const float ax = el->GetLayoutX() + bw.Left;
        const float ay = el->GetLayoutY() + bw.Top;
        const float aw = std::max(0.0f, el->GetLayoutWidth() - bw.Left - bw.Right);
        const float ah = std::max(0.0f, el->GetLayoutHeight() - bw.Top - bw.Bottom);
        if (!childClip.has)
        {
            childClip.has = true;
            childClip.x = ax;
            childClip.y = ay;
            childClip.w = aw;
            childClip.h = ah;
        }
        else
        {
            const float left = std::max(childClip.x, ax);
            const float top = std::max(childClip.y, ay);
            const float right = std::min(childClip.x + childClip.w, ax + aw);
            const float bottom = std::min(childClip.y + childClip.h, ay + ah);
            childClip.x = left;
            childClip.y = top;
            childClip.w = std::max(0.0f, right - left);
            childClip.h = std::max(0.0f, bottom - top);
        }
    }

    for (const auto& ch : el->GetChildren())
    {
        if (ch)
            WalkDebugClipRecursive(ch.get(), childClip, out);
    }
    if (UIElement* tgt = el->GetMountTarget())
        WalkDebugClipRecursive(tgt, childClip, out);
}
} // namespace

void UIManager::RebuildClipCaches()
{
    m_DebugClipCache.clear();
    if (!m_Root)
        return;
    WalkDebugClipRecursive(m_Root.get(), DebugClipWalkState{}, m_DebugClipCache);
}

void UIManager::ApplyLayoutOverrideRects(UpdateContext& ctx, bool /*allOverrides*/)
{
    bool anyPatched = false;
    for (const auto& rec : ctx.layoutOverrideNodes)
    {
        UIElement* element = rec.element;
        if (!element)
            continue;

        if (element->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None)
            continue;

        // Layout-override consumers (virtualized cells in ListView /
        // GridView / TreeView) are direct children of regular UIElement
        // parents (scroll content containers). Mount portal targets are
        // not used with the position:absolute override path in practice,
        // so the UIElement parent edge is the right anchor — same value
        // the legacy NodeRec.parentIndex chain produced for these calls.
        const UIElement* parentEl = element->GetParent();
        if (!parentEl)
            continue;

        Mathematics::Rect rect{};
        if (!UI::Layout::TryGetAbsolutePosition(*element, rect))
            continue;
        rect.Width = std::max(0.0f, rect.Width);
        rect.Height = std::max(0.0f, rect.Height);

        const float desiredX = parentEl->GetLayoutX() + rect.X;
        const float desiredY = parentEl->GetLayoutY() + rect.Y;
        const float desiredW = rect.Width;
        const float desiredH = rect.Height;

        const float curX = element->GetLayoutX();
        const float curY = element->GetLayoutY();
        const float curW = element->GetLayoutWidth();
        const float curH = element->GetLayoutHeight();

        const float dx = desiredX - curX;
        const float dy = desiredY - curY;
        const bool posChanged = (dx != 0.0f) || (dy != 0.0f);
        const bool sizeChanged = (curW != desiredW) || (curH != desiredH);
        if (!posChanged && !sizeChanged)
            continue;

        // Queue the patched cell subtree (including Mount targets) for drain
        // re-emission at the new rects instead of a full-window regen: the
        // drain reads current committed rects, handles count changes, rewrites
        // owned clip slots in place, and escalates to a full regen when a
        // previously clip-culled element becomes visible.
        if (posChanged)
            TranslateLayoutSubtree(element, dx, dy, /*markForDrain=*/!sizeChanged);
        UILayoutAccess::SetLastLayoutRect(*element, desiredX, desiredY, desiredW, desiredH);
        if (sizeChanged)
            MarkSubtreeForDrainReEmit(element);
        anyPatched = true;
    }

    if (anyPatched && m_DebugCaptureEnabled)
    {
        RebuildClipCaches();
    }

    if (ctx.profEnabled && ctx.prof)
        ++ctx.prof->LayoutOverridePatchCount;
}

void UIManager::FinalizeSolve(UpdateContext& ctx)
{
    ApplyLayoutOverrideRects(ctx, /*allOverrides=*/true);
}

void UIManager::ClampAllScrollOffsets(UpdateContext& ctx)
{
    for (ScrollView* sv : ctx.scrollViews)
    {
        if (sv)
            sv->ClampScroll();
    }
}

void UIManager::MarkSubtreeForDrainReEmit(UIElement* root)
{
    if (!root)
        return;
    // A pending full regen clears the drain queue wholesale — skip the pushes.
    if (m_PrimitivesNeedRegen.load(std::memory_order_relaxed))
        return;
    static thread_local std::vector<UIElement*> stack;
    stack.clear();
    stack.push_back(root);
    while (!stack.empty())
    {
        UIElement* cur = stack.back();
        stack.pop_back();
        if (!cur)
            continue;
        // Subtrees the DFS would skip must not be queued: the drain treats an
        // unvisited element that looks visible as a bail-to-full-regen
        // trigger, so a permanently-skipped subtree would force a full regen
        // on every patch. The gate is the DFS's own — note it lets a ZERO-SIZE
        // box through when that box does not clip its children away, because
        // the DFS descends into it and its children's baked primitives would
        // otherwise freeze at their pre-patch rects.
        if (!EmitWalkEntersSubtree(*cur))
            continue;
        cur->MarkDirty(UIElement::VisualDirty);
        for (const auto& ch : cur->GetChildren())
        {
            if (ch)
                stack.push_back(ch.get());
        }
        if (cur->Kind() == UIElementKind::Mount)
        {
            auto* m = static_cast<Mount*>(cur);
            if (UIElement* tgt = m->GetTarget())
                stack.push_back(tgt);
        }
    }
}

void UIManager::TranslateLayoutSubtree(UIElement* rootToTranslate, float dx, float dy,
                                       bool markForDrain)
{
    if (!rootToTranslate)
        return;
    if (dx == 0.0f && dy == 0.0f)
        return;
    // Marks only feed the E1 drain; a pending full regen clears the queue
    // wholesale, so the queue pushes would be pure discarded work.
    if (markForDrain && m_PrimitivesNeedRegen.load(std::memory_order_relaxed))
        markForDrain = false;
    struct Item
    {
        UIElement* El;
        bool Mark;
    };
    static thread_local std::vector<Item> stack;
    stack.clear();
    stack.reserve(64);
    stack.push_back({rootToTranslate, markForDrain});
    constexpr int kMaxNodes = 200000;
    int processed = 0;
    while (!stack.empty() && processed++ < kMaxNodes)
    {
        const Item item = stack.back();
        stack.pop_back();
        UIElement* cur = item.El;
        if (!cur)
            continue;
        UILayoutAccess::SetLastLayoutRect(*cur, cur->GetLayoutX() + dx, cur->GetLayoutY() + dy,
                               cur->GetLayoutWidth(), cur->GetLayoutHeight());
        bool mark = item.Mark;
        if (mark)
        {
            // Subtrees the DFS would skip must not be queued — the drain
            // treats unvisited elements that look visible as a bail-to-full-
            // regen trigger, so queueing a permanently-skipped subtree would
            // force a full regen on every scroll tick. The gate is the DFS's
            // own, and `mark` propagates to the children below, so a zero-size
            // box that does not clip its children away keeps them queued: the
            // DFS emits them, and their baked primitives have to follow this
            // translation like any other.
            mark = EmitWalkEntersSubtree(*cur);
            if (mark)
                cur->MarkDirty(UIElement::VisualDirty);
        }
        for (const auto& ch : cur->GetChildren())
        {
            if (ch)
                stack.push_back({ch.get(), mark});
        }
        if (cur->Kind() == UIElementKind::Mount)
        {
            auto* m = static_cast<Mount*>(cur);
            if (UIElement* tgt = m->GetTarget())
                stack.push_back({tgt, mark});
        }
    }
}

bool UIManager::ApplyScrollTransforms(UpdateContext& ctx)
{
    bool anyTranslated = false;
    for (ScrollView* sv : ctx.scrollViews)
    {
        if (!sv)
            continue;
        UIElement* scrollContent = sv->GetViewport();
        if (!scrollContent)
            continue;
        const uint64_t key = sv->GetInstanceId();
        const uint32_t contentGen =
            scrollContent->m_YogaState ? scrollContent->m_YogaState->LastCommitGen : 0u;

        // The stored offset is only still baked into the committed rect if
        // the content wasn't recommitted since it was applied. A newer
        // commit stamp means CommitLayoutRects rewrote the rect to the
        // Yoga base, so the full desired offset must be re-applied.
        // (Per-content stamps, not a global "solve happened" flag —
        // pruned commits leave untouched scroll content translated.)
        AppliedScrollOffset applied{};
        auto it = m_AppliedScrollByInstanceId.find(key);
        if (it != m_AppliedScrollByInstanceId.end() && it->second.ContentCommitGen == contentGen)
            applied = it->second;

        const float desiredX = sv->GetScrollX();
        const float desiredY = sv->GetScrollY();
        const float dScrollX = desiredX - applied.X;
        const float dScrollY = desiredY - applied.Y;
        if (dScrollX == 0.0f && dScrollY == 0.0f)
        {
            m_AppliedScrollByInstanceId[key] = {desiredX, desiredY, contentGen};
            continue;
        }
        // The fused walk queues the translated subtree (including Mount
        // targets) for drain re-emission at the new rects; the drain rewrites
        // owned clip slots in place and escalates to a full regen when a
        // previously clip-culled element scrolls back into view.
        TranslateLayoutSubtree(scrollContent, -dScrollX, -dScrollY, /*markForDrain=*/true);
        m_AppliedScrollByInstanceId[key] = {desiredX, desiredY, contentGen};
        anyTranslated = true;
    }
    return anyTranslated;
}

// ---------------------------------------------------------------------------
// ConvergePostLayout -- iterative post-layout convergence
// (extracted from the ConvergePostLayout lambda in Update())
// ---------------------------------------------------------------------------

void UIManager::ConvergePostLayout(YGNodeRef& rootNode, UpdateContext& ctx)
{
    if (!ctx.didSolveLayout)
        return;

    GE_CPU_PROFILE_SCOPE("UIManager.Update.PostLayout");
    ScopedSectionTimer _tPostLayout(ctx.profEnabled, &ctx.prof->PostLayoutMs);

    // Templated tree walkers used by every post-layout pass. Compared to
    // `std::function<void(UIElement*)>` the per-call overhead drops from
    // ~30-50ns to a few ns — material on the editor's ~5k-element tree
    // where each pass walks the tree multiple times.
    auto walkPreOrderVisible = [&](auto& self, UIElement* el, auto&& visitor) -> void {
        if (!el) return;
        if (el->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None)
            return;
        visitor(el);
        for (const auto& ch : el->GetChildren())
            self(self, ch.get(), visitor);
        if (el->Kind() == UIElementKind::Mount)
            if (UIElement* tgt = static_cast<Mount*>(el)->GetTarget())
                self(self, tgt, visitor);
    };
    auto walkPostOrderVisible = [&](auto& self, UIElement* el, auto&& visitor) -> void {
        if (!el) return;
        if (el->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None)
            return;
        for (const auto& ch : el->GetChildren())
            self(self, ch.get(), visitor);
        if (el->Kind() == UIElementKind::Mount)
            if (UIElement* tgt = static_cast<Mount*>(el)->GetTarget())
                self(self, tgt, visitor);
        visitor(el);
    };

    struct ApplyDirtyStyleResult
    {
        bool anyApplied = false;
        bool needsRebuildYogaTree = false; // required when display:none toggles (children are pruned/reattached)
    };

    auto applyDirtyStylesToYoga = [&]() -> ApplyDirtyStyleResult
    {
        ApplyDirtyStyleResult res{};
        // Walk every element in the tree (children + Mount portal targets);
        // pre-order so a parent's freshly-cascaded ResolvedStyle is visible
        // to descendants in the same pass.
        auto visitor = [&](UIElement* el) {
            YGNodeRef node = el->m_YogaState ? el->m_YogaState->Node : nullptr;
            if (node && (el->IsDirty(UIElement::StyleDirty) ||
                         el->Overrides().NeedsCascadeRerun()))
            {
                // See ReResolvePseudoStateElement: a stale id resolves to an
                // empty pool. Only a build walk re-primes it, so escalate to
                // the rebuild instead of cascading in place.
                if (el->m_YogaState->SheetSetGeneration != m_SheetSetInterner.Generation())
                {
                    el->MarkDirty(UIElement::StyleDirty | UIElement::LayoutDirty);
                    res.needsRebuildYogaTree = true;
                    return;
                }
                res.anyApplied = true;

                const ResolvedStyle* parentStyle = nullptr;
                if (UIElement* dfsParent = el->GetDfsParent())
                    parentStyle = &dfsParent->GetResolvedStyle();

                // Resolve effective sheets via the persistent interned ID;
                // BuildYogaRecursive primes it on every visit, so it's
                // current after the initial build.
                std::span<const Stylesheet* const> sheets;
                std::span<const StylesheetRuleIndex* const> indices;
                if (el->m_YogaState)
                {
                    const uint32_t id = el->m_YogaState->PersistentSheetSetId;
                    sheets = m_SheetSetInterner.GetSheets(id);
                    indices = m_SheetSetInterner.GetIndices(id);
                }
                if (ctx.profEnabled && ctx.prof)
                {
                    ++ctx.prof->CascadeCalls;
                    ++ctx.prof->CascadeCallsConverge;
                }
                // Post-layout re-resolve: share-inactive (nullptr context).
                // Apply the declared-transition decision inline — this caller
                // runs on the UI thread and has no YogaBuildItem to defer onto.
                TransitionRegistration transitionDecision = TransitionRegistration::None;
                ResolveCascadeForElement(el, sheets, indices, parentStyle,
                                         /*shareCtx=*/nullptr, transitionDecision);
                ApplyTransitionRegistration(el, transitionDecision);
                FinalizeElementStyle(el);

                const ResolvedStyle& cs2 = el->GetResolvedStyle();

                // Gate ApplyStyle (~40 YGNodeStyleSet* calls) on actual
                // layout-input change. Cascade often re-resolves visual-only
                // properties (color, opacity) that don't affect Yoga inputs;
                // re-applying the same layout values is wasted work.
                // Mirrors BuildYogaRecursive's `inputsChanged` gate.
                RetainedYogaNode* retained = el->m_YogaState.get();
                // Order changes can't be honored in place: ordering lives in
                // the parent's InsertChildrenSortedByOrder, not in Yoga node
                // style. Escalate to the retry rebuild, which re-attaches.
                if (retained && retained->HasPrevInputs &&
                    retained->PrevLayout.Order != cs2.Layout.Order)
                {
                    res.needsRebuildYogaTree = true;
                }
                // See BuildYogaRecursive: the container decides the effective
                // flex-shrink, so a container that flipped display moves this
                // element's Yoga input without touching its own style.
                const bool parentBlockFlow =
                    parentStyle && EstablishesBlockFlow(parentStyle->Layout.DisplayMode);
                const bool inputsChanged = !retained || !retained->HasPrevInputs ||
                                           retained->PrevLayout != cs2.Layout ||
                                           retained->PrevParentBlockFlow != parentBlockFlow;
                if (inputsChanged)
                {
                    if (retained)
                    {
                        retained->PrevLayout = cs2.Layout;
                        retained->PrevParentBlockFlow = parentBlockFlow;
                        retained->HasPrevInputs = true;
                    }
                    YogaAdapter::ApplyStyle(node, cs2);
                    ApplyBlockFlowShrinkDefault(node, cs2, parentStyle);
                }
                ApplyWeightedPaneFlexGrowOverride(el, node);

                const DisplayMode nextDisplay = cs2.Layout.DisplayMode;
                const uint32_t yogaChildCount = (uint32_t)YGNodeGetChildCount(node);
                const bool uiHasChildren = !el->GetChildren().empty();

                if (nextDisplay == DisplayMode::None)
                {
                    if (yogaChildCount != 0u)
                    {
                        // Element just flipped to display:none but its Yoga
                        // children are still attached from the previous
                        // (visible) frame. Force the retry path to detach
                        // Yoga children and re-cascade the now-hidden subtree
                        // cleanly.
                        res.needsRebuildYogaTree = true;
                    }
                }
                else if (uiHasChildren && yogaChildCount == 0u)
                {
                    // Coming out of display:none: surgically re-attach this
                    // element's existing child RetainedYogaNodes so Yoga's
                    // post-layout solve has the correct child list for THIS
                    // element's immediate Yoga children. Then mark
                    // ChildrenDirty so the retry path completes the rebuild.
                    static thread_local std::vector<YogaAdapter::ChildWithOrder> s_ReattachBuf;
                    s_ReattachBuf.clear();
                    size_t idx = 0;
                    for (const auto& childPtr : el->GetChildren())
                    {
                        UIElement* ch = childPtr.get();
                        if (!ch)
                        {
                            ++idx;
                            continue;
                        }
                        if (ch->m_YogaState && ch->m_YogaState->Node)
                        {
                            s_ReattachBuf.push_back({ch->m_YogaState->Node,
                                                     ch->GetResolvedStyle().Layout.Order, idx});
                        }
                        ++idx;
                    }
                    // Mount portal target: GetChildren() excludes the
                    // Mount target; append it after children with max
                    // order to mirror BuildYogaRecursive.
                    if (el->Kind() == UIElementKind::Mount)
                    {
                        auto* m = static_cast<Mount*>(el);
                        if (UIElement* tgt = m->GetTarget())
                        {
                            if (tgt->m_YogaState && tgt->m_YogaState->Node)
                            {
                                s_ReattachBuf.push_back({tgt->m_YogaState->Node,
                                                         std::numeric_limits<int>::max(),
                                                         el->GetChildren().size()});
                            }
                        }
                    }
                    if (!s_ReattachBuf.empty())
                    {
                        YogaAdapter::InsertChildrenSortedByOrder(node,
                            std::span<const YogaAdapter::ChildWithOrder>(s_ReattachBuf.data(),
                                                                         s_ReattachBuf.size()));
                    }
                    el->MarkDirty(UIElement::ChildrenDirty | UIElement::LayoutDirty);
                }
            }
        };
        walkPreOrderVisible(walkPreOrderVisible, m_Root.get(), visitor);
        return res;
    };

    // Retry path: rebuild ctx.nodes + Yoga from scratch and re-solve layout.
    // Triggered when OnPostLayout mutates the tree (ChildrenDirty) OR when
    // applyDirtyStylesToYoga detects a display:none flip to None that leaves
    // descendants in ctx.nodes with orphaned Yoga positions. Both cases need
    // identical handling: wipe ctx.nodes, rebuild, re-solve, commit rects.
    // computeLayoutSignaturesThisFrame is forced on so newly created nodes
    // receive styles/measure funcs even on frames that initially skipped them.
    auto rebuildYogaTreeAndResolveLayout = [&]() {
        ScopedSectionTimer _tRetry(ctx.profEnabled, &ctx.prof->PostLayoutRetryMs);
        if (ctx.profEnabled && ctx.prof) ++ctx.prof->PostLayoutRetryCount;
        ctx.computeLayoutSignaturesThisFrame = true;
        ResetFlatSheetTable(ctx, *this);

        {
            ScopedSectionTimer _tBuild2(ctx.profEnabled, &ctx.prof->BuildYogaMs);
            // Root build entry: BuildYogaRecursive binds its own share context.
            rootNode = BuildYogaRecursive(m_Root.get(), nullptr, 0, ctx.rootSheetSetIdx, ctx,
                                          /*shareCtx=*/nullptr).node;
        }
        RebuildTypedNodeLists(ctx);
        {
            ScopedSectionTimer _tYoga2(ctx.profEnabled, &ctx.prof->YogaMs);
            YogaAdapter::CalculateLayout(rootNode, (float)ctx.viewportW, (float)ctx.viewportH);
        }
        ctx.didSolveLayout = true;

        CommitLayoutRects(m_Root.get(), /*incremental=*/false);
        ApplyLayoutOverrideRects(ctx, /*allOverrides=*/true);
    };

    constexpr int kMaxConvergePasses = 3;
    for (int pass = 0; pass < kMaxConvergePasses; ++pass)
    {
        {
            // Let controls observe layout and potentially mutate style/
            // visibility. Post-order so children see settled rects before
            // their parents — preserves the legacy reverse-DFS semantics.
            ScopedSectionTimer _tDispatch(ctx.profEnabled, &ctx.prof->OnPostLayoutDispatchMs);
            // Scoped, not a matched pair: OnPostLayout runs control code, and one of them
            // throwing past a bare SetInEventDispatch(false) would leave the depth raised for
            // the rest of the process — every later RemoveChild deferring forever.
            UIElement::SetInEventDispatch(true);
            struct DispatchDepthScope
            {
                ~DispatchDepthScope() { UIElement::SetInEventDispatch(false); }
            } depthScope;
            walkPostOrderVisible(walkPostOrderVisible, m_Root.get(), [](UIElement* el) {
                el->OnPostLayout();
            });
        }

        // Controls may mutate the UI tree during OnPostLayout (virtualization
        // pools). ChildrenDirty propagates SubtreeDirty up the parent chain,
        // so we only need to descend into subtrees that have it; clean
        // subtrees can't contain a ChildrenDirty descendant.
        bool postLayoutChildrenDirty = false;
        auto probeImpl = [&](auto& self, UIElement* el) -> void {
            if (!el || postLayoutChildrenDirty) return;
            if (el->IsDirty(UIElement::ChildrenDirty))
            {
                postLayoutChildrenDirty = true;
                return;
            }
            if (!el->IsDirty(UIElement::SubtreeDirty))
                return;
            for (const auto& ch : el->GetChildren())
                self(self, ch.get());
            if (el->Kind() == UIElementKind::Mount)
                if (UIElement* tgt = static_cast<Mount*>(el)->GetTarget())
                    self(self, tgt);
        };
        probeImpl(probeImpl, m_Root.get());
        if (postLayoutChildrenDirty)
        {
            rebuildYogaTreeAndResolveLayout();
            continue;
        }

        ApplyDirtyStyleResult dirtyRes = applyDirtyStylesToYoga();
        if (dirtyRes.needsRebuildYogaTree)
        {
            rebuildYogaTreeAndResolveLayout();
            continue;
        }

        if (!dirtyRes.anyApplied)
            break;

        {
            GE_CPU_PROFILE_SCOPE("UIManager.Update.YogaSolve");
            ScopedSectionTimer _tYoga(ctx.profEnabled, &ctx.prof->YogaMs);
            YogaAdapter::CalculateLayout(rootNode, (float)ctx.viewportW, (float)ctx.viewportH);
        }
        CommitLayoutRects(m_Root.get(), /*incremental=*/false);
        ApplyLayoutOverrideRects(ctx, /*allOverrides=*/true);
    }

    // After post-layout convergence, auto-size absolutely positioned
    // containers based on their children when they use auto width and/or
    // height. Lets popups (dropdowns) sized position:absolute with auto
    // dimensions naturally fit their options. Children must already have
    // their rects committed (post-order ensures this).
    walkPostOrderVisible(walkPostOrderVisible, m_Root.get(), [](UIElement* el) {
        const ResolvedStyle& cs = el->GetResolvedStyle();
        if (cs.Layout.PositionType != PositionType::Absolute)
            return;
        const bool widthAuto = (cs.Layout.Width.Unit == StyleLength::UnitType::Auto);
        const bool heightAuto = (cs.Layout.Height.Unit == StyleLength::UnitType::Auto);
        if (!widthAuto && !heightAuto)
            return;
        float baseX = el->GetLayoutX();
        float baseY = el->GetLayoutY();
        float curW = el->GetLayoutWidth();
        float curH = el->GetLayoutHeight();
        float maxRight = baseX;
        float maxBottom = baseY;
        bool foundChild = false;
        for (const auto& ch : el->GetChildren())
        {
            if (!ch)
                continue;
            // display:none children keep their last committed rect
            // (CommitLayoutRects skips them, Yoga doesn't solve them), so a
            // hidden child must not hold the container open at its old size.
            if (ch->GetResolvedStyle().Layout.DisplayMode == DisplayMode::None)
                continue;
            float cw = ch->GetLayoutWidth();
            float chH = ch->GetLayoutHeight();
            if (cw <= 0.0f || chH <= 0.0f)
                continue;
            // Trailing margins belong to the content extent: a child with
            // `margin: 0 5px` must not sit 5px from the left edge but flush
            // against the right (the row-hover asymmetry this fixes). Percent
            // and auto margins carry bare numbers/zeros here — count only
            // resolved px edges.
            const auto& chLayout = ch->GetResolvedStyle().Layout;
            const float marginRight =
                (!chLayout.MarginIsPercent.Right && !chLayout.MarginIsAuto.Right)
                    ? chLayout.Margin.Right : 0.0f;
            const float marginBottom =
                (!chLayout.MarginIsPercent.Bottom && !chLayout.MarginIsAuto.Bottom)
                    ? chLayout.Margin.Bottom : 0.0f;
            float cx1 = ch->GetLayoutX() + cw + marginRight;
            float cy1 = ch->GetLayoutY() + chH + marginBottom;
            if (!foundChild)
            {
                maxRight = cx1;
                maxBottom = cy1;
                foundChild = true;
            }
            else
            {
                if (cx1 > maxRight)
                    maxRight = cx1;
                if (cy1 > maxBottom)
                    maxBottom = cy1;
            }
        }
        if (!foundChild)
            return;
        // baseX/baseY are the container's BORDER-box origin and the children
        // are inset from it by border AND padding, so the size being built here
        // is a border box and both trailing insets belong in it. Padding alone
        // leaves the box one border-width short on each of the two edges.
        // The trailing padding is the LAYOUT-resolved one — the leading inset
        // baked into the children's positions came from the same solve, and
        // `cs.Layout.Padding` would hand back a bare `10` for `padding: 10%`.
        const Box4& resolvedPadding = el->GetLayoutPadding();
        float newW = curW;
        float newH = curH;
        if (widthAuto)
            newW = std::max(0.0f, maxRight - baseX + resolvedPadding.Right +
                                      cs.Layout.BorderWidth.Right);
        if (heightAuto)
            newH = std::max(0.0f, maxBottom - baseY + resolvedPadding.Bottom +
                                      cs.Layout.BorderWidth.Bottom);
        if (newW <= 0.0f && newH <= 0.0f)
            return;
        UILayoutAccess::SetLastLayoutRect(*el, baseX, baseY, newW, newH);
    });
}

#if defined(GE_HAVE_YOGA) && GE_HAVE_YOGA
int UIManager::MeasureTextLineCount(const UIElement& element, const std::string& text, float widthLogical)
{
    const RetainedYogaNode* retained = element.m_YogaState.get();
    if (!retained || !retained->MeasureCtx || !retained->MeasureCtx->Atlas)
        return 0;
    // A copy of the element's own measure context: the same face and policy, its cache untouched.
    TextMeasureCtx probe = *retained->MeasureCtx;
    probe.Text = text;
    probe.HasNewlines = text.find('\n') != std::string::npos;
    probe.CachedValid = false;
    const float scale = probe.ContentScale > 0.0f ? probe.ContentScale : 1.0f;
    MeasureTextWithAtlas(&probe, probe.AllowWrap ? std::max(0.0f, widthLogical) * scale : 0.0f);
    return probe.MeasuredLineCount;
}

float UIManager::MeasureTextWidth(const UIElement& element, std::string_view text)
{
    const RetainedYogaNode* retained = element.m_YogaState.get();
    if (!retained || !retained->MeasureCtx || !retained->MeasureCtx->Atlas)
        return 0.0f;
    // A copy of the element's own measure context: the same face, its cache untouched.
    TextMeasureCtx probe = *retained->MeasureCtx;
    probe.Text = std::string(text);
    probe.HasNewlines = text.find('\n') != std::string_view::npos;
    probe.AllowWrap = false;
    probe.CachedValid = false;
    return MeasureTextWithAtlas(&probe, 0.0f).width;
}
#else
int UIManager::MeasureTextLineCount(const UIElement&, const std::string&, float)
{
    return 0;
}

float UIManager::MeasureTextWidth(const UIElement&, std::string_view)
{
    return 0.0f;
}
#endif
