#include "UI/Controls/Widgets/SceneViewRulerOverlay.h"

#include "Editor/Settings/SceneViewSettings.h"
#include "Logger/Logger.h"
#include "Panels/SceneViewPanel.h"
#include "Rendering/Text/FontAtlas.h"
#include "SceneViewController.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIPrimitive.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace GameEngine
{
namespace
{
// Layout constants.
constexpr float kRulerBand     = 18.0f; // band thickness for top + left rulers
constexpr float kTopRulerNudge = -1.0f; // 1px upward bias on the top ruler so its baseline aligns with the panel chrome
constexpr float kFontSize      = 10.0f; // physical pixels
constexpr float kCornerFont    = 9.0f;

// Major tick spacing in pixels stays in this range as the user zooms.
constexpr float kMinMajorPx  = 64.0f;
constexpr float kMaxMajorPx  = 130.0f;

// Pick a "nice" world-unit step for the ruler so a major tick spans at least
// kMinMajorPx and at most kMaxMajorPx pixels at the current zoom. Steps follow
// the standard 1, 2, 5, 10, 20, 50, 100, ... progression (and 0.5, 0.2, 0.1
// at small scales). worldPerPixel must be > 0.
float ChooseMajorStepWorld(float worldPerPixel)
{
    if (!(worldPerPixel > 0.0f))
        return 1.0f;

    float step = 1.0f;
    auto pxOf = [&]() { return step / worldPerPixel; };

    int guard = 64;
    while (pxOf() < kMinMajorPx && guard-- > 0)
    {
        const float exp10 = std::pow(10.0f, std::floor(std::log10(step)));
        const float mant  = step / exp10;
        if      (mant < 1.5f) step = 2.0f  * exp10;
        else if (mant < 3.5f) step = 5.0f  * exp10;
        else                  step = 10.0f * exp10;
    }
    guard = 64;
    while (pxOf() > kMaxMajorPx && guard-- > 0)
    {
        const float exp10 = std::pow(10.0f, std::floor(std::log10(step)));
        const float mant  = step / exp10;
        if      (mant > 7.0f) step = 5.0f * exp10;
        else if (mant > 3.0f) step = 2.0f * exp10;
        else if (mant > 1.5f) step = 1.0f * exp10;
        else                  step = 5.0f * (exp10 * 0.1f);
    }
    return step;
}

// Format a world-unit value for ruler labels. Picks a precision that keeps
// labels short while still showing useful sub-unit detail at high zoom.
void FormatNumber(float v, char* out, size_t outSize)
{
    const float av = std::abs(v);
    if (av < 1e-4f)
    {
        std::snprintf(out, outSize, "0");
        return;
    }
    if (std::fabs(v - std::round(v)) < 1e-3f * std::max(1.0f, av))
    {
        std::snprintf(out, outSize, "%d", static_cast<int>(std::round(v)));
        return;
    }
    if (av >= 100.0f)     std::snprintf(out, outSize, "%.0f", v);
    else if (av >= 10.0f) std::snprintf(out, outSize, "%.1f", v);
    else if (av >= 1.0f)  std::snprintf(out, outSize, "%.2f", v);
    else                  std::snprintf(out, outSize, "%.3f", v);
}

// Width of a string in physical pixels at the given (physical) font size.
float MeasureTextWidth(Rendering::Text::FontAtlas* font, std::string_view text, float physicalSize)
{
    if (!font || text.empty()) return 0.0f;
    static thread_local Rendering::Text::FontAtlas::ShapeResult s_Shape;
    font->ShapeText(text, physicalSize, s_Shape, 0xFFFFFFFFu);
    float maxX = 0.0f;
    for (const auto& gp : s_Shape.glyphs)
        maxX = std::max(maxX, gp.x + gp.width);
    return maxX;
}
} // namespace

SceneViewRulerOverlay::SceneViewRulerOverlay()
{
    AddClass("scene-view-ruler-overlay");
}

void SceneViewRulerOverlay::SetCursor(float localX, float localY, bool inside)
{
    if (inside == m_CursorInside &&
        std::fabs(localX - m_CursorX) < 0.5f &&
        std::fabs(localY - m_CursorY) < 0.5f)
        return;

    m_CursorX     = localX;
    m_CursorY     = localY;
    m_CursorInside = inside;

    if (m_LastShown)
        MarkDirty(VisualDirty);
}

void SceneViewRulerOverlay::Tick()
{
    if (!m_Controller)
    {
        if (!HasClass("hidden"))
            AddClass("hidden");
        if (m_LastShown)
        {
            m_LastShown = false;
            MarkDirty(VisualDirty);
        }
        return;
    }

    const auto& settings = Editor::SceneViewSettings::Get();
    const bool show = m_Controller->Is2DMode() && settings.GetShowRulers();

    static bool s_LoggedOnce = false;
    if (!s_LoggedOnce)
    {
        s_LoggedOnce = true;
        Logger::Log::Info("SceneViewRulerOverlay: first tick — Is2DMode={} ShowRulers={} viewport={}x{}",
                          m_Controller->Is2DMode(),
                          settings.GetShowRulers(),
                          GetLayoutWidth(),
                          GetLayoutHeight());
    }

    if (!show)
    {
        if (!HasClass("hidden"))
            AddClass("hidden");
        if (m_LastShown)
        {
            m_LastShown = false;
            MarkDirty(VisualDirty);
        }
        return;
    }

    if (HasClass("hidden"))
        RemoveClass("hidden");

    const auto pose = m_Controller->GetCameraPose();
    const float vw  = GetLayoutWidth();
    const float vh  = GetLayoutHeight();
    const int  scale = settings.GetPixelPerfectScale();
    const bool pp    = settings.GetPixelPerfect2D();
    const float op   = settings.GetRulerOpacity();
    const uint32_t indCol = settings.GetRulerIndicatorColor();
    const float    indTh  = settings.GetRulerIndicatorThickness();

    if (!m_LastShown
        || pose.Pos[0]  != m_LastCamX
        || pose.Pos[1]  != m_LastCamY
        || pose.Distance != m_LastDistance
        || vw            != m_LastViewW
        || vh            != m_LastViewH
        || scale         != m_LastPixelScale
        || pp            != m_LastPixelPerfect
        || std::fabs(op - m_LastOpacity) > 0.001f
        || indCol        != m_LastIndicatorColor
        || std::fabs(indTh - m_LastIndicatorThickness) > 0.01f)
    {
        m_LastCamX        = pose.Pos[0];
        m_LastCamY        = pose.Pos[1];
        m_LastDistance    = pose.Distance;
        m_LastViewW       = vw;
        m_LastViewH       = vh;
        m_LastPixelScale  = scale;
        m_LastPixelPerfect = pp;
        m_LastOpacity     = op;
        m_LastIndicatorColor     = indCol;
        m_LastIndicatorThickness = indTh;
        m_LastShown       = true;
        MarkDirty(VisualDirty);
    }
}

void SceneViewRulerOverlay::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                                                 const ResolvedStyle& /*style*/,
                                                 float x, float y, float w, float h)
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

    if (!m_Controller || !m_Controller->Is2DMode())
        return;
    if (w <= 4.0f || h <= 4.0f)
        return;
    if (!Editor::SceneViewSettings::Get().GetShowRulers())
        return;

    const auto pose = m_Controller->GetCameraPose();
    const float distance = std::max(0.0001f, pose.Distance);

    // 2D camera basis: right=+X, up=+Y, forward=+Z. m_CamDistance is the
    // orthographic visible height in world units.
    const float halfH  = distance * 0.5f;
    const float aspect = (h > 0.0f) ? (w / h) : 1.0f;
    const float halfW  = halfH * aspect;

    const float wppX = (2.0f * halfW) / std::max(1.0f, w);
    const float wppY = (2.0f * halfH) / std::max(1.0f, h);

    // Top-ruler vertical bias: the band is nudged up by kTopRulerNudge so its
    // baseline visually aligns with the surrounding panel chrome.
    const float topY = y + kTopRulerNudge;

    // User-configurable transparency multiplier (0..1). Scales the alpha of
    // every ruler primitive so the whole overlay can be made more see-through
    // without rebalancing the per-element opacities individually.
    const float rulerOpacity = std::clamp(
        Editor::SceneViewSettings::Get().GetRulerOpacity(), 0.0f, 1.0f);

    // Color palette — neutral dark with a warm cursor accent.
    const uint32_t bg        = PackColor(0.10f, 0.10f, 0.10f, 0.85f * rulerOpacity);
    const uint32_t corner    = PackColor(0.06f, 0.06f, 0.06f, 0.95f * rulerOpacity);
    const uint32_t tickMinor = PackColor(0.55f, 0.55f, 0.55f, 0.90f * rulerOpacity);
    const uint32_t tickMajor = PackColor(0.85f, 0.85f, 0.85f, 1.00f * rulerOpacity);
    const uint32_t labelCol  = PackColor(0.78f, 0.78f, 0.78f, 1.00f * rulerOpacity);

    // Cursor indicator color and thickness come from settings. The configured
    // ARGB alpha is honored, then scaled by rulerOpacity for the global fade.
    const uint32_t indicatorArgb = Editor::SceneViewSettings::Get().GetRulerIndicatorColor();
    const float indicatorThickness =
        std::max(0.5f, Editor::SceneViewSettings::Get().GetRulerIndicatorThickness());
    uint32_t cursorCol;
    {
        const uint8_t cr = static_cast<uint8_t>((indicatorArgb >> 16) & 0xFF);
        const uint8_t cg = static_cast<uint8_t>((indicatorArgb >>  8) & 0xFF);
        const uint8_t cb = static_cast<uint8_t>((indicatorArgb      ) & 0xFF);
        const uint8_t ca = static_cast<uint8_t>((indicatorArgb >> 24) & 0xFF);
        const uint8_t scaledA = static_cast<uint8_t>(
            std::clamp(static_cast<int>(std::round(static_cast<float>(ca) * rulerOpacity)), 0, 255));
        cursorCol = PackColorU8(cr, cg, cb, scaledA);
    }

    // ---------- Background bands ----------
    ctx.Emit(MakeRect(x, topY, w, kRulerBand, bg));                         // top
    ctx.Emit(MakeRect(x, y + kRulerBand, kRulerBand, h - kRulerBand, bg));  // left

    // ---------- Horizontal ruler ticks ----------
    {
        const float majorWorld = ChooseMajorStepWorld(wppX);
        const float minorWorld = majorWorld * 0.1f;

        const float startScreenX = x + kRulerBand;
        const float endScreenX   = x + w;

        auto WorldXToLocalPx = [&](float worldX) -> float {
            const float ndcX = (worldX - pose.Pos[0]) / std::max(1e-6f, halfW);
            return x + (ndcX + 1.0f) * 0.5f * w;
        };
        auto LocalPxToWorldX = [&](float screenPx) -> float {
            const float pxFromLeft = screenPx - x;
            const float ndcX = (2.0f * pxFromLeft) / std::max(1.0f, w) - 1.0f;
            return pose.Pos[0] + ndcX * halfW;
        };

        const float leftWorld  = LocalPxToWorldX(startScreenX);
        const float rightWorld = LocalPxToWorldX(endScreenX);
        const long long firstIdx = static_cast<long long>(std::floor(leftWorld  / minorWorld));
        const long long lastIdx  = static_cast<long long>(std::ceil (rightWorld / minorWorld));

        for (long long i = firstIdx; i <= lastIdx; ++i)
        {
            const float worldVal = static_cast<float>(i) * minorWorld;
            const float sx       = std::round(WorldXToLocalPx(worldVal));
            if (sx < startScreenX - 1.0f || sx > endScreenX + 1.0f)
                continue;

            const bool isMajor = (i % 10 == 0);
            const bool isMid   = !isMajor && (i % 5 == 0);
            const float top = isMajor ? topY
                            : isMid   ? (topY + kRulerBand * 0.33f)
                                      : (topY + kRulerBand * 0.66f);
            ctx.Emit(MakeLine(sx, top, sx, topY + kRulerBand, 1.0f,
                              isMajor ? tickMajor : tickMinor));

            if (isMajor && ctx.FontAtlas)
            {
                char buf[32];
                FormatNumber(worldVal, buf, sizeof(buf));
                const float logical = (ctx.ContentScale > 0.0f)
                                      ? (kFontSize / ctx.ContentScale) : kFontSize;
                ctx.EmitText(buf, sx + 2.0f, topY + 1.0f, logical, labelCol, ctx.FontAtlas);
            }
        }
    }

    // ---------- Vertical ruler ticks ----------
    {
        const float majorWorld = ChooseMajorStepWorld(wppY);
        const float minorWorld = majorWorld * 0.1f;

        const float startScreenY = y + kRulerBand;
        const float endScreenY   = y + h;

        // Screen Y is Y-down, world Y is Y-up: ndcY = 1 - 2*localY/h.
        auto WorldYToLocalPx = [&](float worldY) -> float {
            const float ndcY = (worldY - pose.Pos[1]) / std::max(1e-6f, halfH);
            return y + (1.0f - ndcY) * 0.5f * h;
        };
        auto LocalPxToWorldY = [&](float screenPx) -> float {
            const float pxFromTop = screenPx - y;
            const float ndcY = 1.0f - (2.0f * pxFromTop) / std::max(1.0f, h);
            return pose.Pos[1] + ndcY * halfH;
        };

        const float topWorld    = LocalPxToWorldY(startScreenY);
        const float bottomWorld = LocalPxToWorldY(endScreenY);
        const float minWorld = std::min(topWorld, bottomWorld);
        const float maxWorld = std::max(topWorld, bottomWorld);
        const long long firstIdx = static_cast<long long>(std::floor(minWorld / minorWorld));
        const long long lastIdx  = static_cast<long long>(std::ceil (maxWorld / minorWorld));

        for (long long i = firstIdx; i <= lastIdx; ++i)
        {
            const float worldVal = static_cast<float>(i) * minorWorld;
            const float sy       = std::round(WorldYToLocalPx(worldVal));
            if (sy < startScreenY - 1.0f || sy > endScreenY + 1.0f)
                continue;

            const bool isMajor = (i % 10 == 0);
            const bool isMid   = !isMajor && (i % 5 == 0);
            const float left = isMajor ? x
                              : isMid   ? (x + kRulerBand * 0.33f)
                                        : (x + kRulerBand * 0.66f);
            ctx.Emit(MakeLine(left, sy, x + kRulerBand, sy, 1.0f,
                              isMajor ? tickMajor : tickMinor));

            if (isMajor && ctx.FontAtlas)
            {
                char buf[32];
                FormatNumber(worldVal, buf, sizeof(buf));
                // Right-align the label inside the band so digits stack neatly.
                const float advance = MeasureTextWidth(ctx.FontAtlas, buf, kFontSize);
                const float tx = x + std::max(1.0f, kRulerBand - advance - 2.0f);
                const float ty = sy + 1.0f;
                const float logical = (ctx.ContentScale > 0.0f)
                                      ? (kFontSize / ctx.ContentScale) : kFontSize;
                ctx.EmitText(buf, tx, ty, logical, labelCol, ctx.FontAtlas);
            }
        }
    }

    // ---------- Cursor indicators ----------
    if (m_CursorInside)
    {
        // SetCursor is fed from pointer events, which are logical CSS px; the
        // rest of this overlay works in physical px, so convert on the way in.
        const float cursorX = m_CursorX * ctx.ContentScale;
        const float cursorY = m_CursorY * ctx.ContentScale;
        if (cursorX >= kRulerBand && cursorX <= w)
        {
            const float sx = x + std::round(cursorX);
            ctx.Emit(MakeLine(sx, topY, sx, topY + kRulerBand, indicatorThickness, cursorCol));
        }
        if (cursorY >= kRulerBand && cursorY <= h)
        {
            const float sy = y + std::round(cursorY);
            ctx.Emit(MakeLine(x, sy, x + kRulerBand, sy, indicatorThickness, cursorCol));
        }
    }

    // ---------- Corner square + magnification readout ----------
    ctx.Emit(MakeRect(x, topY, kRulerBand, kRulerBand, corner));

    if (ctx.FontAtlas)
    {
        char buf[24] = {0};
        const auto& settings = Editor::SceneViewSettings::Get();
        if (settings.GetPixelPerfect2D())
        {
            const int s = std::clamp(settings.GetPixelPerfectScale(), 1, 64);
            std::snprintf(buf, sizeof(buf), "%dx", s);
        }
        else
        {
            // Pixels per world unit along the (vertical) axis. Equivalent to
            // viewportH / orthoHeight.
            const float ppu = (wppY > 0.0f) ? (1.0f / wppY) : 1.0f;
            if (ppu >= 100.0f)     std::snprintf(buf, sizeof(buf), "%dpx", static_cast<int>(std::round(ppu)));
            else if (ppu >= 10.0f) std::snprintf(buf, sizeof(buf), "%.1fpx", ppu);
            else if (ppu >= 1.0f)  std::snprintf(buf, sizeof(buf), "%.2fpx", ppu);
            else                   std::snprintf(buf, sizeof(buf), "%.3fpx", ppu);
        }
        const float advance = MeasureTextWidth(ctx.FontAtlas, buf, kCornerFont);
        const float tx = x + std::max(1.0f, (kRulerBand - advance) * 0.5f);
        const float ty = topY + std::max(1.0f, (kRulerBand - kCornerFont) * 0.5f);
        const float logical = (ctx.ContentScale > 0.0f)
                              ? (kCornerFont / ctx.ContentScale) : kCornerFont;
        ctx.EmitText(buf, tx, ty, logical, labelCol, ctx.FontAtlas);
    }
}

} // namespace GameEngine

namespace RegisterWidgets
{
static auto s_reg_sceneViewRulerOverlay =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::SceneViewRulerOverlay>(
        "SceneViewRulerOverlay",
        []() { return std::make_unique<GameEngine::SceneViewRulerOverlay>(); })
    .TagAlias("sceneviewruleroverlay");
}
