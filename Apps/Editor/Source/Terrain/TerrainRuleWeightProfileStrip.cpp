#include "Terrain/TerrainRuleWeightProfileStrip.h"

#include "TerrainECS/TerrainSurfaceRuleEval.h"
#include "UI/StyleProperties.h"
#include "UI/UIPrimitive.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace GameEngine::Editor
{
namespace
{

constexpr float kCornerRadiusPx = 3.0f;
constexpr float kBorderWidthPx = 1.0f;

// Below this the strip is a sliver where a plot reads as noise; the band summary
// beside it still says everything true.
constexpr float kMinDrawHeightPx = 6.0f;

// Upper bound on sampled columns, so an inspector docked very wide cannot turn
// one row into thousands of triangles per drag frame.
constexpr int kMaxColumns = 512;

// The fill is ONE hue whose lightness and alpha ramp with weight. Two hues read
// as two quantities; a falloff is one quantity changing.
constexpr float kHueR = 0.42f;
constexpr float kHueG = 0.63f;
constexpr float kHueB = 1.0f;

uint32_t WeightColor(float weight)
{
    const float t = std::clamp(weight, 0.0f, 1.0f);
    // Lift from a dim floor to full so a shallow ramp still reads as filled, and
    // carry alpha with it so the curve's foot fades out rather than ending on a
    // hard line.
    const float lift = 0.45f + 0.55f * t;
    const float alpha = 0.35f + 0.60f * t;
    return UI::PackColor(kHueR * lift, kHueG * lift, kHueB * lift, alpha);
}

} // namespace

TerrainRuleWeightProfileStrip::TerrainRuleWeightProfileStrip()
{
    AddClass("terrain-rule-weight-profile");
    // Inert by construction: the slider above owns the domain's interaction.
    Overrides().Set(Style::PointerEvents, false);
}

void TerrainRuleWeightProfileStrip::SetCondition(
    const Components::TerrainRuleCondition& condition,
    TerrainRuleVocabulary::ConditionDomain domain)
{
    m_Condition = condition;
    m_Domain = domain;
    MarkDirty(VisualDirty);
}

void TerrainRuleWeightProfileStrip::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                                                         const ResolvedStyle&,
                                                         float x, float y, float w, float h)
{
    // Parallel-drain thread contract (UIElement.h): custom emission escalates to
    // the UI thread rather than running on a JobSystem worker.
    if (ctx.OffThread)
    {
        if (ctx.EscalateFlag)
            *ctx.EscalateFlag = true;
        return;
    }

    if (w <= 1.0f || h <= kMinDrawHeightPx)
        return;

    const float cs = std::max(ctx.ContentScale, 1e-6f);
    const float radius = kCornerRadiusPx * cs;

    UI::UIPrimitive track = UI::MakeRect(x, y, w, h, UI::PackColor(0.09f, 0.10f, 0.12f, 0.95f),
                                         radius, radius, radius, radius);
    UI::AddBorder(track, kBorderWidthPx * cs, UI::PackColor(0.26f, 0.29f, 0.33f, 0.9f));
    ctx.Emit(track);

    const float domainSpan = m_Domain.Max - m_Domain.Min;
    if (!(domainSpan > 0.0f) || !std::isfinite(domainSpan))
        return;

    const float inset = kBorderWidthPx * cs;
    const float plotX = x + inset;
    const float plotY = y + inset;
    const float plotW = w - 2.0f * inset;
    const float plotH = h - 2.0f * inset;
    if (plotW <= 0.0f || plotH <= 0.0f)
        return;

    // One column per PHYSICAL pixel. Wider columns leave a seam at every
    // boundary — constant weight then reads as a barcode — and quantize a ramp
    // into stair treads, so any feather narrower than one column collapses into
    // a hard edge and stops being visible at all.
    const int columns = std::clamp(static_cast<int>(std::lround(plotW)), 1, kMaxColumns);

    // Sampled at column BOUNDARIES, so neighbouring columns share an edge exactly
    // and the top of the fill is the curve rather than a staircase.
    std::vector<float> weights(static_cast<std::size_t>(columns) + 1u);
    for (int i = 0; i <= columns; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(columns);
        const float value = m_Domain.Min + t * domainSpan;
        weights[static_cast<std::size_t>(i)] =
            std::clamp(TerrainECS::TerrainRuleConditionWeightForValue(m_Condition, value),
                       0.0f, 1.0f);
    }

    const float bottom = plotY + plotH;
    const float columnW = plotW / static_cast<float>(columns);

    for (int i = 0; i < columns; ++i)
    {
        const float wLeft = weights[static_cast<std::size_t>(i)];
        const float wRight = weights[static_cast<std::size_t>(i) + 1u];
        if (wLeft <= 0.0f && wRight <= 0.0f)
            continue;

        const float x0 = plotX + static_cast<float>(i) * columnW;
        const float x1 = x0 + columnW;
        const float y0 = bottom - wLeft * plotH;
        const float y1 = bottom - wRight * plotH;

        // Trapezoid as two triangles, coloured by the column's mean weight. At
        // one pixel per column that reads as a continuous ramp instead of a hue
        // step at the band edge.
        const uint32_t fill = WeightColor(0.5f * (wLeft + wRight));
        ctx.Emit(UI::MakeTriangle(x0, bottom, x1, bottom, x0, y0, fill));
        ctx.Emit(UI::MakeTriangle(x1, bottom, x1, y1, x0, y0, fill));
    }
}

} // namespace GameEngine::Editor
