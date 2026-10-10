#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "UI/UIElement.h"
#include "UI/UIPrimitive.h"

namespace GameEngine
{

// Lightweight time-series chart for the debugger's Monitors / Profiler panels.
// Draws a single line series with auto-ranged Y axis and a sticky peak marker.
// Data is pulled each frame via a std::function provider, so panels can stream
// from a ring buffer (e.g. DebugMetrics::CopyHistory) without caching on the
// widget side.
//
// Header-only by design: renders purely by emitting UIPrimitives in
// OnGeneratePrimitives, no extra state, no CMake plumbing.
class ChartView : public UIElement
{
  public:
    enum class Mode : std::uint8_t
    {
        Line,
        Area,
        Bars,
    };

    using ProviderFn = std::function<void(std::vector<float>& samples)>;
    using FormatterFn = std::function<std::string(float value)>;

    ChartView()
    {
        AddClass("chart-view");
    }

    void SetMode(Mode mode) { m_Mode = mode; MarkDirty(VisualDirty); }
    void SetProvider(ProviderFn provider) { m_Provider = std::move(provider); MarkDirty(VisualDirty); }

    // Sample index highlighted as a vertical line; -1 disables the cursor.
    void SetCursorIndex(int index) { m_CursorIndex = index; MarkDirty(VisualDirty); }
    int GetCursorIndex() const { return m_CursorIndex; }

    // Axis label formatter. Receives a raw sample value and returns the
    // string to draw at the y-axis tick. Leave unset for the default "%.1f".
    void SetValueFormatter(FormatterFn formatter) { m_Formatter = std::move(formatter); MarkDirty(VisualDirty); }
    void SetShowAxisLabels(bool show) { m_ShowAxisLabels = show; MarkDirty(VisualDirty); }

    // Explicit Y range. Leave min==max to auto-range from the data.
    void SetYRange(float yMin, float yMax) { m_YMin = yMin; m_YMax = yMax; MarkDirty(VisualDirty); }

    // Horizontal reference line at a fixed value (e.g. a frame-time budget).
    // Drawn only when the value falls inside the visible Y range. Pass a value
    // <= 0 to clear it.
    void SetReferenceLine(float value, std::uint32_t color)
    {
        m_HasReferenceLine = value > 0.0f;
        m_ReferenceLineValue = value;
        m_ReferenceLineColor = color;
        MarkDirty(VisualDirty);
    }

    // Floor for the auto-ranged Y maximum (the "fit to budget" behaviour): the
    // axis never scales below this, so a reference line at/under it stays
    // visible and data is always read against the same baseline. Ignored when an
    // explicit SetYRange is active. 0 disables.
    void SetMinYMax(float value) { m_MinYMax = std::max(0.0f, value); MarkDirty(VisualDirty); }

    // Smooth the auto-ranged yMax so a single spike doesn't snap the axis back
    // down the moment it ages out of the sample window. The display yMax snaps
    // *up* immediately when new data exceeds it (so spikes are visible) and
    // decays back down toward the data's true yMax with the given half-life
    // (seconds). Set 0 to disable smoothing (default).
    void SetYMaxSmoothingHalfLife(float halfLifeSec)
    {
        m_YMaxHalfLife = std::max(0.0f, halfLifeSec);
    }

    // Line / area color (packed RGBA — see UI::PackColor).
    void SetLineColor(std::uint32_t color) { m_LineColor = color; MarkDirty(VisualDirty); }
    void SetFillColor(std::uint32_t color) { m_FillColor = color; MarkDirty(VisualDirty); }
    void SetGridColor(std::uint32_t color) { m_GridColor = color; MarkDirty(VisualDirty); }
    void SetAxisLabelColor(std::uint32_t color) { m_AxisLabelColor = color; MarkDirty(VisualDirty); }

    // Sign-based line coloring. When enabled, Line-mode segments above
    // `threshold` use `m_LineColor`, segments below use `negativeColor`.
    // Segments that cross the threshold are split at the crossing point.
    void SetNegativeLineColor(std::uint32_t color, float threshold = 0.0f)
    {
        m_NegativeLineColor = color;
        m_SignThreshold = threshold;
        m_HasNegativeColor = true;
        MarkDirty(VisualDirty);
    }

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                              const ResolvedStyle& /*style*/,
                              float x, float y, float w, float h) override
    {
        using namespace UI;
        if (w <= 1.0f || h <= 1.0f)
            return;

        // Gridlines: 10 vertical, 4 horizontal.
        for (int i = 0; i <= 10; ++i)
        {
            const float px = x + static_cast<float>(i) / 10.0f * w;
            ctx.Emit(MakeRect(px - 0.5f, y, 1.0f, h, m_GridColor));
        }
        for (int i = 0; i <= 4; ++i)
        {
            const float py = y + static_cast<float>(i) / 4.0f * h;
            ctx.Emit(MakeRect(x, py - 0.5f, w, 1.0f, m_GridColor));
        }

        if (!m_Provider)
            return;

        m_Samples.clear();
        m_Provider(m_Samples);
        if (m_Samples.empty())
            return;

        float yMin = m_YMin;
        float yMax = m_YMax;
        const bool autoRange = (yMin >= yMax);
        if (autoRange)
        {
            yMin = std::numeric_limits<float>::infinity();
            yMax = -std::numeric_limits<float>::infinity();
            for (float v : m_Samples)
            {
                if (v < yMin) yMin = v;
                if (v > yMax) yMax = v;
            }
            if (!std::isfinite(yMin) || !std::isfinite(yMax) || yMax <= yMin)
            {
                yMin = 0.0f;
                yMax = std::max(1.0f, yMax);
            }
            const float pad = (yMax - yMin) * 0.1f;
            yMax += pad;
            if (yMin > 0.0f)
                yMin = 0.0f;

            // Sticky-max with exponential decay. Snap UP on new highs (a fresh
            // spike should be visible immediately) but decay smoothly back DOWN
            // when the spike ages out, so the y-axis doesn't flicker between
            // wildly different scales as outlier samples enter and leave the
            // window.
            if (m_YMaxHalfLife > 0.0f)
            {
                const auto now = std::chrono::steady_clock::now();
                if (m_HasDisplayYMax)
                {
                    const float dt = std::chrono::duration<float>(now - m_LastDisplayUpdate).count();
                    if (yMax > m_DisplayYMax)
                    {
                        m_DisplayYMax = yMax;
                    }
                    else
                    {
                        const float decay = std::exp2f(-dt / m_YMaxHalfLife);
                        m_DisplayYMax = yMax + (m_DisplayYMax - yMax) * decay;
                    }
                }
                else
                {
                    m_DisplayYMax = yMax;
                    m_HasDisplayYMax = true;
                }
                m_LastDisplayUpdate = now;
                yMax = m_DisplayYMax;
            }
            // Fit-to-budget floor: never scale below the configured minimum so a
            // reference line at/under it stays on screen.
            if (m_MinYMax > 0.0f)
                yMax = std::max(yMax, m_MinYMax);
        }
        const float yRange = std::max(1e-6f, yMax - yMin);

        const std::size_t n = m_Samples.size();
        auto indexToPx = [&](std::size_t i) {
            return x + (n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) * w : w * 0.5f);
        };
        auto valueToPy = [&](float v) {
            const float ny = (v - yMin) / yRange;
            return y + h - std::clamp(ny, 0.0f, 1.0f) * h;
        };

        switch (m_Mode)
        {
        case Mode::Bars:
        {
            const float barWidth = std::max(1.0f, w / static_cast<float>(n) - 1.0f);
            for (std::size_t i = 0; i < n; ++i)
            {
                const float px = indexToPx(i) - barWidth * 0.5f;
                const float top = valueToPy(m_Samples[i]);
                const float barH = (y + h) - top;
                if (barH > 0.0f)
                    ctx.Emit(MakeRect(px, top, barWidth, barH, m_LineColor));
            }
            break;
        }
        case Mode::Area:
        {
            // Approximate area by stacking thin columns under each sample.
            for (std::size_t i = 0; i + 1 < n; ++i)
            {
                const float x0 = indexToPx(i);
                const float x1 = indexToPx(i + 1);
                const float colTop = valueToPy(std::max(m_Samples[i], m_Samples[i + 1]));
                const float colBot = y + h;
                if (colBot > colTop && x1 > x0)
                    ctx.Emit(MakeRect(x0, colTop, x1 - x0, colBot - colTop, m_FillColor));
            }
            [[fallthrough]];
        }
        case Mode::Line:
        default:
        {
            for (std::size_t i = 0; i + 1 < n; ++i)
            {
                const float x0 = indexToPx(i);
                const float x1 = indexToPx(i + 1);
                const float v0 = m_Samples[i];
                const float v1 = m_Samples[i + 1];
                const float y0 = valueToPy(v0);
                const float y1 = valueToPy(v1);
                if (!m_HasNegativeColor)
                {
                    ctx.Emit(MakeLine(x0, y0, x1, y1, 1.6f, m_LineColor));
                    continue;
                }
                const float d0 = v0 - m_SignThreshold;
                const float d1 = v1 - m_SignThreshold;
                const auto colorFor = [&](float d) { return d >= 0.0f ? m_LineColor : m_NegativeLineColor; };
                if ((d0 >= 0.0f) == (d1 >= 0.0f))
                {
                    ctx.Emit(MakeLine(x0, y0, x1, y1, 1.6f, colorFor(d0)));
                }
                else
                {
                    // Split the segment at the threshold crossing.
                    const float t = d0 / (d0 - d1);
                    const float xc = x0 + (x1 - x0) * t;
                    const float yc = valueToPy(m_SignThreshold);
                    ctx.Emit(MakeLine(x0, y0, xc, yc, 1.6f, colorFor(d0)));
                    ctx.Emit(MakeLine(xc, yc, x1, y1, 1.6f, colorFor(d1)));
                }
            }
            break;
        }
        }

        // Budget / reference line (e.g. the 16.67 ms frame-time target), drawn
        // only while it sits inside the visible Y range.
        if (m_HasReferenceLine && m_ReferenceLineValue >= yMin && m_ReferenceLineValue <= yMax)
        {
            const float refPy = valueToPy(m_ReferenceLineValue);
            ctx.Emit(MakeRect(x, refPy - 0.5f, w, 1.0f, m_ReferenceLineColor));
        }

        // Cursor marker (vertical white line at the selected sample index).
        if (m_CursorIndex >= 0 && static_cast<std::size_t>(m_CursorIndex) < n)
        {
            const float cx = indexToPx(static_cast<std::size_t>(m_CursorIndex));
            ctx.Emit(MakeRect(cx - 0.5f, y, 1.0f, h, 0xFFFFFFFFu));
        }

        // Y-axis tick labels (top = yMax, middle = mid, bottom = yMin).
        if (m_ShowAxisLabels && ctx.FontAtlas)
        {
            const float fontSize = 10.0f;
            const float lineH = fontSize + 1.0f;
            const float labelX = x + 3.0f;
            auto emitLabel = [&](float value, float py) {
                const std::string text = m_Formatter ? m_Formatter(value) : DefaultFormat(value);
                if (text.empty())
                    return;
                ctx.EmitText(text, labelX, py, fontSize, m_AxisLabelColor, ctx.FontAtlas);
            };
            // Top label sits just below the plot top edge; bottom label sits just above the baseline.
            emitLabel(yMax, y + 1.0f);
            const float midVal = 0.5f * (yMax + yMin);
            emitLabel(midVal, y + h * 0.5f - lineH * 0.5f);
            emitLabel(yMin, y + h - lineH - 1.0f);
        }
    }

  private:
    static std::string DefaultFormat(float value)
    {
        char buf[32];
        if (std::abs(value) >= 1000.0f)
            std::snprintf(buf, sizeof(buf), "%.0f", value);
        else
            std::snprintf(buf, sizeof(buf), "%.1f", value);
        return std::string(buf);
    }

  public:

  private:
    Mode m_Mode = Mode::Line;
    ProviderFn m_Provider;
    FormatterFn m_Formatter;
    float m_YMin = 0.0f;
    float m_YMax = 0.0f; // min >= max => auto-range
    std::uint32_t m_LineColor = 0xFF5FBEFFu; // ABGR packed (orange-ish)
    std::uint32_t m_FillColor = 0x405FBEFFu;
    std::uint32_t m_GridColor = 0x1AFFFFFFu;
    std::uint32_t m_AxisLabelColor = 0xFF808080u;
    std::uint32_t m_NegativeLineColor = 0xFF5FBEFFu;
    float m_SignThreshold = 0.0f;
    bool m_HasNegativeColor = false;
    bool m_HasReferenceLine = false;
    float m_ReferenceLineValue = 0.0f;
    std::uint32_t m_ReferenceLineColor = 0xFFFFFFFFu;
    float m_MinYMax = 0.0f; // fit-to-budget floor for auto-range
    bool m_ShowAxisLabels = true;
    int m_CursorIndex = -1;

    // Auto-range smoothing state. Zero half-life disables smoothing; the
    // display yMax then exactly follows the data each frame (old behavior).
    float m_YMaxHalfLife = 0.0f;
    float m_DisplayYMax = 0.0f;
    bool m_HasDisplayYMax = false;
    std::chrono::steady_clock::time_point m_LastDisplayUpdate{};

    std::vector<float> m_Samples;
};

} // namespace GameEngine
