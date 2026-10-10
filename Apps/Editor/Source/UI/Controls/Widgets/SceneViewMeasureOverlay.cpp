#include "UI/Controls/Widgets/SceneViewMeasureOverlay.h"

#include "Components/Hierarchy.h"
#include "Components/Measure/MeasureComponent.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Components.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "Mathematics/MatrixOps.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "SceneViewController.h"
#include "SceneView/SceneViewEvents.h"
#include "SceneView/SceneViewProjection.h"
#include "Rendering/Text/FontAtlas.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIPrimitive.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace GameEngine
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;

float Dot3(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<float, 3> Sub3(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

float Len2(float x, float y)
{
    return std::sqrt(x * x + y * y);
}

float Len3(const std::array<float, 3>& v)
{
    return std::sqrt(Dot3(v, v));
}

void FormatFloat(char* out, size_t outSize, float value, int decimals)
{
    if (std::fabs(value) < 0.0005f)
        value = 0.0f;
    std::snprintf(out, outSize, "%.*f", decimals, value);
}

uint32_t TextColor()
{
    return UI::PackColor(0.92f, 0.92f, 0.92f, 1.0f);
}

uint32_t SecondaryTextColor()
{
    return UI::PackColor(0.74f, 0.74f, 0.74f, 1.0f);
}

uint32_t PillFillColor()
{
    return UI::PackColor(0.0f, 0.0f, 0.0f, 0.88f);
}

uint32_t SceneMeasureColor(float alphaScale = 1.0f)
{
    const uint32_t argb = Editor::SceneViewSettings::Get().GetMeasureColor();
    const float a = std::clamp(((argb >> 24) & 0xFF) / 255.0f * alphaScale, 0.0f, 1.0f);
    const float r = ((argb >> 16) & 0xFF) / 255.0f;
    const float g = ((argb >> 8) & 0xFF) / 255.0f;
    const float b = (argb & 0xFF) / 255.0f;
    return UI::PackColor(r, g, b, a);
}

uint32_t MeasureComponentColor(const Components::MeasureComponent& measure,
                               float alphaScale = 1.0f,
                               float brighten = 0.0f)
{
    const float mix = std::clamp(brighten, 0.0f, 1.0f);
    const float r = std::clamp(measure.Color[0], 0.0f, 1.0f);
    const float g = std::clamp(measure.Color[1], 0.0f, 1.0f);
    const float b = std::clamp(measure.Color[2], 0.0f, 1.0f);
    return UI::PackColor(r + (1.0f - r) * mix,
                         g + (1.0f - g) * mix,
                         b + (1.0f - b) * mix,
                         std::clamp(measure.Color[3] * alphaScale, 0.0f, 1.0f));
}

void EmitTextOutlined(UI::PrimitiveEmitContext& ctx,
                      const std::string& text,
                      float x,
                      float y,
                      float fontSize,
                      uint32_t color)
{
    if (!ctx.FontAtlas || text.empty())
        return;

    const uint32_t outline = UI::PackColor(0.0f, 0.0f, 0.0f, 0.95f);
    ctx.EmitText(text, x - 1.0f, y, fontSize, outline, ctx.FontAtlas);
    ctx.EmitText(text, x + 1.0f, y, fontSize, outline, ctx.FontAtlas);
    ctx.EmitText(text, x, y - 1.0f, fontSize, outline, ctx.FontAtlas);
    ctx.EmitText(text, x, y + 1.0f, fontSize, outline, ctx.FontAtlas);
    ctx.EmitText(text, x, y, fontSize, color, ctx.FontAtlas);
}

void EmitArc(UI::PrimitiveEmitContext& ctx,
             float cx,
             float cy,
             float radius,
             float startAngle,
             float endAngle,
             uint32_t color,
             float thickness)
{
    if (radius <= 0.5f)
        return;

    constexpr int kSegments = 8;
    float prevX = cx + std::cos(startAngle) * radius;
    float prevY = cy + std::sin(startAngle) * radius;
    for (int i = 1; i <= kSegments; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(kSegments);
        const float a = startAngle + (endAngle - startAngle) * t;
        const float px = cx + std::cos(a) * radius;
        const float py = cy + std::sin(a) * radius;
        ctx.Emit(UI::MakeLine(prevX, prevY, px, py, thickness, color));
        prevX = px;
        prevY = py;
    }
}

void EmitDottedLine(UI::PrimitiveEmitContext& ctx,
                    float x0,
                    float y0,
                    float x1,
                    float y1,
                    float thickness,
                    uint32_t color,
                    float spacing = 7.0f)
{
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float len = Len2(dx, dy);
    if (len <= 0.5f)
        return;

    const float ux = dx / len;
    const float uy = dy / len;
    const float dotLen = std::max(0.75f, thickness * 0.25f);
    for (float t = 0.0f; t <= len; t += spacing)
    {
        const float half = std::min(dotLen, len - t) * 0.5f;
        const float cx = x0 + ux * t;
        const float cy = y0 + uy * t;
        ctx.Emit(UI::MakeLine(cx - ux * half, cy - uy * half,
                              cx + ux * half, cy + uy * half,
                              thickness, color));
    }
}

float ClampLabelX(float x, float viewportW, float approxW)
{
    return std::clamp(x, 4.0f, std::max(4.0f, viewportW - approxW));
}

float ClampLabelY(float y, float viewportH)
{
    return std::clamp(y, 14.0f, std::max(14.0f, viewportH - 10.0f));
}

float MeasureTextWidth(Rendering::Text::FontAtlas* font,
                       std::string_view text,
                       float logicalFontSize,
                       float contentScale)
{
    if (!font || text.empty())
        return static_cast<float>(text.size()) * logicalFontSize * 0.55f;

    const float pixelSize = std::max(1.0f, logicalFontSize * contentScale);
    return font->MeasureUtf8(text, pixelSize).width / std::max(0.001f, contentScale);
}

struct TextInkBounds
{
    float MinX = 0.0f;
    float MinY = 0.0f;
    float W = 0.0f;
    float H = 0.0f;
};

TextInkBounds MeasureTextInkBounds(Rendering::Text::FontAtlas* font,
                                   std::string_view text,
                                   float logicalFontSize,
                                   float contentScale)
{
    TextInkBounds bounds{};
    if (!font || text.empty())
        return bounds;

    const float cs = std::max(0.001f, contentScale);
    const float pixelSize = std::max(1.0f, logicalFontSize * cs);
    static thread_local Rendering::Text::FontAtlas::ShapeResult s_TextShape;
    font->ShapeText(text, pixelSize, s_TextShape, 0xFFFFFFFFu);
    if (s_TextShape.glyphs.empty())
        return bounds;

    float minX = 1e30f;
    float minY = 1e30f;
    float maxX = -1e30f;
    float maxY = -1e30f;
    for (const auto& gp : s_TextShape.glyphs)
    {
        minX = std::min(minX, gp.x);
        minY = std::min(minY, gp.y);
        maxX = std::max(maxX, gp.x + gp.width);
        maxY = std::max(maxY, gp.y + gp.height);
    }

    bounds.MinX = minX / cs;
    bounds.MinY = minY / cs;
    bounds.W = std::max(0.0f, (maxX - minX) / cs);
    bounds.H = std::max(0.0f, (maxY - minY) / cs);
    return bounds;
}

void EmitTextCenteredInBox(UI::PrimitiveEmitContext& ctx,
                           const std::string& text,
                           float boxX,
                           float boxY,
                           float boxW,
                           float boxH,
                           float fontSize,
                           uint32_t color)
{
    if (!ctx.FontAtlas || text.empty())
        return;

    const TextInkBounds ink = MeasureTextInkBounds(ctx.FontAtlas, text, fontSize, ctx.ContentScale);
    if (ink.W <= 0.0f || ink.H <= 0.0f)
    {
        const float textW = MeasureTextWidth(ctx.FontAtlas, text, fontSize, ctx.ContentScale);
        ctx.EmitText(text, boxX + (boxW - textW) * 0.5f, boxY, fontSize, color, ctx.FontAtlas);
        return;
    }

    const float textX = boxX + (boxW - ink.W) * 0.5f - ink.MinX;
    const float textY = boxY + (boxH - ink.H) * 0.5f - ink.MinY;
    ctx.EmitText(text, textX, textY, fontSize, color, ctx.FontAtlas);
}

struct ValuePillLayout
{
    float W = 0.0f;
    float H = 0.0f;
    float PrimaryW = 0.0f;
    float PrimaryH = 0.0f;
    float SecondaryW = 0.0f;
    float SecondaryH = 0.0f;
};

ValuePillLayout MeasureValuePill(UI::PrimitiveEmitContext& ctx,
                                 const std::string& primaryText,
                                 const std::string& secondaryText,
                                 float fontSize)
{
    const float cs = std::max(0.001f, ctx.ContentScale);
    const float padX = 18.0f;
    const float padY = 6.0f;
    const float gapY = secondaryText.empty() ? 0.0f : 1.0f;
    ValuePillLayout layout{};
    layout.PrimaryW = MeasureTextWidth(ctx.FontAtlas, primaryText, fontSize, cs);
    layout.SecondaryW = secondaryText.empty()
        ? 0.0f
        : MeasureTextWidth(ctx.FontAtlas, secondaryText, fontSize - 1.0f, cs);

    layout.PrimaryH = fontSize + 2.0f;
    layout.SecondaryH = secondaryText.empty() ? 0.0f : fontSize + 1.0f;
    if (ctx.FontAtlas)
    {
        const auto primaryMetrics =
            ctx.FontAtlas->GetFontLineMetrics(std::max(1.0f, fontSize * cs));
        layout.PrimaryH = primaryMetrics.height / cs;
        if (!secondaryText.empty())
        {
            const auto secondaryMetrics =
                ctx.FontAtlas->GetFontLineMetrics(std::max(1.0f, (fontSize - 1.0f) * cs));
            layout.SecondaryH = secondaryMetrics.height / cs;
        }
    }

    layout.W = std::max(layout.PrimaryW, layout.SecondaryW) + padX * 2.0f;
    layout.H = padY * 2.0f + layout.PrimaryH + (secondaryText.empty() ? 0.0f : gapY + layout.SecondaryH);
    return layout;
}

ValuePillLayout EmitValuePill(UI::PrimitiveEmitContext& ctx,
                              const std::string& primaryText,
                              const std::string& secondaryText,
                              float centerX,
                              float centerY,
                              [[maybe_unused]] float viewportLeft,
                              [[maybe_unused]] float viewportTop,
                              [[maybe_unused]] float viewportRight,
                              [[maybe_unused]] float viewportBottom,
                              float fontSize)
{
    if (primaryText.empty())
        return {};

    const ValuePillLayout layout = MeasureValuePill(ctx, primaryText, secondaryText, fontSize);
    const float padY = 6.0f;
    const float gapY = secondaryText.empty() ? 0.0f : 1.0f;
    const float pillX = centerX - layout.W * 0.5f;
    const float pillY = centerY - layout.H * 0.5f;
    const float radius = layout.H * 0.5f;

    UI::UIPrimitive pill = UI::MakeRect(pillX, pillY, layout.W, layout.H,
                                        PillFillColor(),
                                        radius, radius, radius, radius);
    UI::AddShadow(pill, 0.0f, 4.0f, 12.0f, UI::PackColor(0.0f, 0.0f, 0.0f, 0.70f));
    UI::ExpandForEffects(pill);
    ctx.Emit(pill);

    if (!ctx.FontAtlas)
        return layout;

    const float primaryY = pillY + padY;
    EmitTextCenteredInBox(ctx,
                          primaryText,
                          pillX,
                          primaryY,
                          layout.W,
                          layout.PrimaryH,
                          fontSize,
                          0xFFF4F4F4u);

    if (!secondaryText.empty())
    {
        const float secondaryY = primaryY + layout.PrimaryH + gapY;
        EmitTextCenteredInBox(ctx,
                              secondaryText,
                              pillX,
                              secondaryY,
                              layout.W,
                              layout.SecondaryH,
                              fontSize - 1.0f,
                              0xFFD0D0D0u);
    }

    return layout;
}

std::string FormatWorldDistance(float meters, SceneViewMeasureOverlay::UnitSystem unitSystem)
{
    char buf[64];
    if (unitSystem == SceneViewMeasureOverlay::UnitSystem::Imperial)
    {
        const float feet = meters * 3.2808399f;
        if (std::fabs(feet) < 1.0f)
        {
            FormatFloat(buf, sizeof(buf), meters * 39.3700787f, 2);
            return std::string(buf) + " in";
        }
        FormatFloat(buf, sizeof(buf), feet, 3);
        return std::string(buf) + " ft";
    }

    FormatFloat(buf, sizeof(buf), meters, 3);
    return std::string(buf) + " m";
}

void EmitEndpointDot(UI::PrimitiveEmitContext& ctx, float cx, float cy, bool hovered = false)
{
    const float kRadius = hovered ? 5.0f : 4.0f;
    UI::UIPrimitive dot = UI::MakeRect(cx - kRadius, cy - kRadius,
                                       kRadius * 2.0f, kRadius * 2.0f,
                                       hovered
                                           ? UI::PackColor(0.04f, 0.04f, 0.04f, 1.0f)
                                           : UI::PackColor(0.0f, 0.0f, 0.0f, 0.92f),
                                       kRadius, kRadius, kRadius, kRadius);
    UI::AddShadow(dot,
                  0.0f,
                  hovered ? 3.0f : 2.0f,
                  hovered ? 9.0f : 6.0f,
                  UI::PackColor(0.0f, 0.0f, 0.0f, hovered ? 0.70f : 0.55f));
    UI::ExpandForEffects(dot);
    ctx.Emit(dot);
}

std::array<float, 2> ValuePillCenterForLine(UI::PrimitiveEmitContext& ctx,
                                            const std::string& primaryText,
                                            const std::string& secondaryText,
                                            float fontSize,
                                            float sx,
                                            float sy,
                                            float ex,
                                            float ey)
{
    const ValuePillLayout layout = MeasureValuePill(ctx, primaryText, secondaryText, fontSize);
    const float dx = ex - sx;
    const float dy = ey - sy;
    const float len = Len2(dx, dy);
    float cx = (sx + ex) * 0.5f;
    float cy = (sy + ey) * 0.5f;
    if (len <= 0.5f)
        return {cx, cy};

    const float ux = dx / len;
    const float uy = dy / len;
    const float projectedPill = std::fabs(ux) * layout.W + std::fabs(uy) * layout.H;
    if (len >= projectedPill + 12.0f)
        return {cx, cy};

    const float side = (std::fabs(uy) > 0.75f) ? (ux >= 0.0f ? 1.0f : -1.0f) : (uy >= 0.0f ? -1.0f : 1.0f);
    const float nx = -uy * side;
    const float ny = ux * side;
    const float offset = std::max(layout.H * 0.5f + 10.0f, 24.0f);
    return {cx + nx * offset, cy + ny * offset};
}

bool IsEntityEnabledInScene(ECS::World& world, ECS::EntityHandle entity)
{
    return entity.IsValid() && world.IsValid(entity) &&
           ECS::Entity(&world, entity).IsEnabledInHierarchy();
}

bool GetEntityWorldMatrix(ECS::World& world,
                          ECS::EntityHandle entity,
                          Mathematics::Matrix4x4& out,
                          int depth = 0)
{
    if (!entity.IsValid() || !world.IsValid(entity) || depth > 64)
        return false;

    if (const auto* transform = world.GetComponent<Components::Transform>(entity))
    {
        Mathematics::Matrix4x4 local = Mathematics::Matrix4x4::FromColumnMajor(transform->matrix);
        if (const auto* parent = world.GetComponent<Components::Parent>(entity);
            parent && parent->parent.IsValid())
        {
            Mathematics::Matrix4x4 parentWorld;
            if (GetEntityWorldMatrix(world, parent->parent, parentWorld, depth + 1))
            {
                out = parentWorld * local;
                return true;
            }
        }

        out = local;
        return true;
    }

    if (const auto* worldTransform = world.GetComponent<Components::WorldTransform>(entity))
    {
        out = Mathematics::Matrix4x4::FromColumnMajor(worldTransform->matrix);
        return true;
    }

    return false;
}

bool GetEntityWorldPosition(ECS::World& world, ECS::EntityHandle entity, std::array<float, 3>& out)
{
    if (!IsEntityEnabledInScene(world, entity))
        return false;

    Mathematics::Matrix4x4 worldMatrix;
    if (GetEntityWorldMatrix(world, entity, worldMatrix))
    {
        out = {worldMatrix[3].x, worldMatrix[3].y, worldMatrix[3].z};
        return true;
    }

    return false;
}

std::string Format2DUnitsLabel(SceneViewController* controller, float pixelLength)
{
    if (!controller)
        return {};

    const float grid = controller->GetGridSnapSize();
    if (!controller->IsGridSnapEnabled() || grid <= 0.001f)
        return {};

    char buf[64];
    FormatFloat(buf, sizeof(buf), pixelLength / grid, 2);
    return std::string(buf) + " units";
}
} // namespace

SceneViewMeasureOverlay::SceneViewMeasureOverlay()
{
    AddClass("scene-view-measure-overlay");
    AddClass("hidden");
}

void SceneViewMeasureOverlay::Tick()
{
    if (!m_Controller)
    {
        if (!HasClass("hidden"))
            AddClass("hidden");
        return;
    }

    if (HasClass("hidden"))
        RemoveClass("hidden");

    // An active measure follows the pointer — re-emit every tick. Committed
    // measures are world-anchored ECS entities: they only move on screen when
    // the camera pose, projection mode, or an endpoint entity moves. Hash
    // those inputs and re-emit on change; an unconditional per-frame mark
    // here kept every editor frame dirty and defeated the UI idle gate.
    if (m_Active)
    {
        MarkDirty(VisualDirty);
        return;
    }
    const uint64_t emitKey = ComputeEmitKey();
    if (emitKey != m_LastEmitKey)
    {
        m_LastEmitKey = emitKey;
        MarkDirty(VisualDirty);
    }
}

void SceneViewMeasureOverlay::SetMeasureToolEnabled(bool enabled)
{
    if (m_ToolEnabled == enabled)
        return;

    m_ToolEnabled = enabled;
    if (!enabled)
        m_Active = false;

    if (m_Controller)
        RemoveClass("hidden");

    MarkDirty(VisualDirty);
}

void SceneViewMeasureOverlay::SetUnitSystem(UnitSystem unitSystem)
{
    if (m_UnitSystem == unitSystem)
        return;

    m_UnitSystem = unitSystem;
    MarkDirty(VisualDirty);
}

void SceneViewMeasureOverlay::SetTwoDMode(TwoDMode mode)
{
    if (m_TwoDMode == mode)
        return;

    m_TwoDMode = mode;
    MarkDirty(VisualDirty);
}

void SceneViewMeasureOverlay::BeginMeasure(float viewX,
                                           float viewY,
                                           const std::array<float, 3>& world,
                                           bool is2D,
                                           bool showComponents)
{
    m_Active = true;
    m_Is2D = is2D;
    m_ShowComponents = showComponents;
    m_StartViewX = viewX;
    m_StartViewY = viewY;
    m_EndViewX = viewX;
    m_EndViewY = viewY;
    m_StartWorld = world;
    m_EndWorld = world;
    RemoveClass("hidden");
    MarkDirty(VisualDirty);
}

void SceneViewMeasureOverlay::UpdateMeasure(float viewX,
                                            float viewY,
                                            const std::array<float, 3>& world,
                                            bool showComponents)
{
    if (!m_Active)
        return;

    m_EndViewX = viewX;
    m_EndViewY = viewY;
    m_EndWorld = world;
    m_ShowComponents = showComponents;
    MarkDirty(VisualDirty);
}

void SceneViewMeasureOverlay::EndMeasure()
{
    m_Active = false;
    MarkDirty(VisualDirty);
}

void SceneViewMeasureOverlay::CancelMeasure()
{
    m_Active = false;
    MarkDirty(VisualDirty);
}

uint64_t SceneViewMeasureOverlay::ComputeEmitKey() const
{
    // FNV-1a over everything OnGeneratePrimitives reads for the committed
    // pills: each enabled measure's endpoints/color/mode, camera pose,
    // projection mode, FOV, grid-snap (feeds the 2D units label), and the
    // hovered entity (feeds the hover highlight). Layout-size and
    // unit-system changes mark dirty through their own paths (rect
    // write-back, setters).
    constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
    constexpr uint64_t kFnvPrime = 1099511628211ull;
    uint64_t h = kFnvOffsetBasis;
    auto mix = [&h](const void* p, size_t n)
    {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i)
        {
            h ^= b[i];
            h *= kFnvPrime;
        }
    };

    // Measures first: with none visible the overlay draws nothing, so the
    // camera-dependent inputs must not churn re-emits — return the basis
    // untouched and the key holds still through any camera motion.
    bool anyMeasure = false;
    if (ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld())
    {
        world->Query<ECS::Read<Components::MeasureComponent>>()
            .Each([&](ECS::EntityHandle entity, const Components::MeasureComponent& measure)
            {
                std::array<float, 3> start{};
                std::array<float, 3> end{};
                if (!GetEntityWorldPosition(*world, measure.StartEntity, start) ||
                    !GetEntityWorldPosition(*world, measure.EndEntity, end))
                    return;
                anyMeasure = true;
                mix(&entity, sizeof(entity));
                const bool measure2D = measure.Is2D;
                mix(&measure2D, sizeof(measure2D));
                mix(measure.Color, sizeof(measure.Color));
                mix(start.data(), sizeof(float) * 3);
                mix(end.data(), sizeof(float) * 3);
            });
    }
    if (!anyMeasure)
        return kFnvOffsetBasis;

    const SceneViewCameraPose pose = m_Controller->GetCameraPose();
    mix(&pose.Pos[0], sizeof(float) * 3);
    mix(&pose.YawDeg, sizeof(float));
    mix(&pose.PitchDeg, sizeof(float));
    mix(&pose.Distance, sizeof(float));
    const bool is2D = m_Controller->Is2DMode();
    const bool ortho = m_Controller->IsOrthographic();
    mix(&is2D, sizeof(is2D));
    mix(&ortho, sizeof(ortho));
    const float fovYDeg = Editor::SceneViewSettings::Get().GetFieldOfViewDeg();
    mix(&fovYDeg, sizeof(fovYDeg));
    const bool snapEnabled = m_Controller->IsGridSnapEnabled();
    const float snapSize = m_Controller->GetGridSnapSize();
    mix(&snapEnabled, sizeof(snapEnabled));
    mix(&snapSize, sizeof(snapSize));
    const ECS::EntityHandle hovered = m_Controller->GetHoveredEntity();
    mix(&hovered, sizeof(hovered));

    return h;
}

bool SceneViewMeasureOverlay::ProjectWorldToView(const std::array<float, 3>& world,
                                                 float w,
                                                 float h,
                                                 float& outX,
                                                 float& outY) const
{
    if (!m_Controller || w <= 1.0f || h <= 1.0f)
        return false;

    Editor::SceneTools::ScenePointerEvent view{};
    view.viewW = w;
    view.viewH = h;
    m_Controller->PopulatePointerCameraState(view);

    Mathematics::Vector2 pixel;
    if (!Editor::SceneTools::ProjectWorldToView(view, Mathematics::Vector3(world[0], world[1], world[2]), pixel))
        return false;
    outX = pixel.x;
    outY = pixel.y;
    return true;
}

void SceneViewMeasureOverlay::RenderPersistentMeasurePills(UI::PrimitiveEmitContext& ctx,
                                                           float x, float y, float w, float h)
{
    if (!m_Controller || w <= 1.0f || h <= 1.0f)
        return;

    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (!world)
        return;

    constexpr float fontSize = 13.0f;
    char buf[96];

    world->Query<ECS::Read<Components::MeasureComponent>>()
        .Each([&](ECS::EntityHandle entity, const Components::MeasureComponent& measure)
        {
            std::array<float, 3> start{};
            std::array<float, 3> end{};
            if (!GetEntityWorldPosition(*world, measure.StartEntity, start) ||
                !GetEntityWorldPosition(*world, measure.EndEntity, end))
            {
                return;
            }

            float sx = 0.0f;
            float sy = 0.0f;
            float ex = 0.0f;
            float ey = 0.0f;
            if (!ProjectWorldToView(start, w, h, sx, sy) ||
                !ProjectWorldToView(end, w, h, ex, ey))
            {
                return;
            }

            sx += x;
            sy += y;
            ex += x;
            ey += y;

            const std::array<float, 3> deltaWorld = Sub3(end, start);
            if (measure.Is2D)
            {
                const ECS::EntityHandle hovered = m_Controller->GetHoveredEntity();
                const bool measureHovered = hovered == entity;
                const bool anyHovered =
                    measureHovered ||
                    hovered == measure.StartEntity ||
                    hovered == measure.EndEntity;
                const float hoverBrighten = anyHovered ? 0.35f : 0.0f;
                const uint32_t primary = MeasureComponentColor(measure, 1.0f, hoverBrighten);
                const uint32_t secondary = MeasureComponentColor(measure, 0.55f, hoverBrighten);
                EmitDottedLine(ctx, sx, sy, ex, ey, anyHovered ? 4.2f : 3.0f, primary);

                const float dx = ex - sx;
                const float dy = ey - sy;
                const float screenLength = Len2(dx, dy);
                const bool showTriangle = m_TwoDMode == TwoDMode::Triangle;
                const bool diagonal2D = showTriangle && std::fabs(dx) > 0.5f && std::fabs(dy) > 0.5f;
                if (diagonal2D)
                {
                    const float cx = sx;
                    const float cy = ey;
                    EmitDottedLine(ctx, sx, sy, cx, cy, anyHovered ? 2.0f : 1.4f, secondary, 6.0f);
                    EmitDottedLine(ctx, cx, cy, ex, ey, anyHovered ? 2.0f : 1.4f, secondary, 6.0f);

                    const float horizontalAngle = std::atan2(std::fabs(dy), std::fabs(dx));
                    const float verticalAngle = kPi * 0.5f - horizontalAngle;
                    const float arcRadius = std::min({50.0f,
                                                      screenLength * 0.10f,
                                                      std::fabs(dx),
                                                      std::fabs(dy)});

                    const float dirX = dx >= 0.0f ? 1.0f : -1.0f;
                    const float dirY = dy >= 0.0f ? 1.0f : -1.0f;
                    const float vStart = dirY > 0.0f ? kPi * 0.5f : -kPi * 0.5f;
                    const float vEnd = vStart - dirX * verticalAngle;
                    EmitArc(ctx, sx, sy, arcRadius, vStart, vEnd, primary, anyHovered ? 2.8f : 2.0f);

                    const float hStart = dirX > 0.0f ? kPi : 0.0f;
                    const float hEnd = hStart + dirY * horizontalAngle;
                    EmitArc(ctx, ex, ey, arcRadius, hStart, hEnd, primary, anyHovered ? 2.8f : 2.0f);
                }

                EmitEndpointDot(ctx, sx, sy, anyHovered);
                EmitEndpointDot(ctx, ex, ey, anyHovered);

                const float wx = std::fabs(deltaWorld[0]);
                const float wy = std::fabs(deltaWorld[1]);
                const float lengthPx = std::sqrt(wx * wx + wy * wy);
                FormatFloat(buf, sizeof(buf), lengthPx, 1);
                const std::string label = std::string(buf) + " px";
                const std::string unitsLabel = Format2DUnitsLabel(m_Controller, lengthPx);
                const auto labelCenter = ValuePillCenterForLine(ctx, label, unitsLabel, fontSize, sx, sy, ex, ey);
                EmitValuePill(ctx, label, unitsLabel,
                              labelCenter[0],
                              labelCenter[1],
                              x,
                              y,
                              x + w,
                              y + h,
                              fontSize);

                if (diagonal2D)
                {
                    const float cx = sx;
                    const float cy = ey;
                    FormatFloat(buf, sizeof(buf), wy, 1);
                    EmitValuePill(ctx, std::string(buf) + " px", {},
                                  sx + (sx < ex ? -42.0f : 42.0f),
                                  (sy + cy) * 0.5f,
                                  x,
                                  y,
                                  x + w,
                                  y + h,
                                  fontSize);

                    FormatFloat(buf, sizeof(buf), wx, 1);
                    EmitValuePill(ctx, std::string(buf) + " px", {},
                                  (cx + ex) * 0.5f,
                                  ey + (sy < ey ? 24.0f : -24.0f),
                                  x,
                                  y,
                                  x + w,
                                  y + h,
                                  fontSize);

                    const int hAngle =
                        static_cast<int>(std::round(std::atan2(wy, wx) * 180.0f / kPi));
                    const int vAngle = 90 - hAngle;
                    std::snprintf(buf, sizeof(buf), "%d deg", vAngle);
                    EmitValuePill(ctx, buf, {},
                                  sx,
                                  sy + (sy < ey ? 30.0f : -30.0f),
                                  x,
                                  y,
                                  x + w,
                                  y + h,
                                  fontSize);
                    std::snprintf(buf, sizeof(buf), "%d deg", hAngle);
                    EmitValuePill(ctx, buf, {},
                                  ex,
                                  ey + (sy < ey ? 32.0f : -32.0f),
                                  x,
                                  y,
                                  x + w,
                                  y + h,
                                  fontSize);
                }
                return;
            }

            const float dist = Len3(deltaWorld) < 0.001f ? 0.0f : Len3(deltaWorld);
            const std::string label = FormatWorldDistance(dist, m_UnitSystem);
            const auto labelCenter = ValuePillCenterForLine(ctx, label, {}, fontSize, sx, sy, ex, ey);
            EmitValuePill(ctx, label, {},
                          labelCenter[0],
                          labelCenter[1],
                          x,
                          y,
                          x + w,
                          y + h,
                          fontSize);
        });
}

void SceneViewMeasureOverlay::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
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

    if (!m_Controller || w <= 1.0f || h <= 1.0f)
        return;

    RenderPersistentMeasurePills(ctx, x, y, w, h);

    if (!m_Active)
        return;

    // ProjectWorldToView is driven by w/h and so yields physical px, which is
    // the space x/y are in. The live drag endpoints come from pointer events
    // and are logical CSS px — convert them before offsetting by the origin.
    float sx = m_StartViewX * ctx.ContentScale;
    float sy = m_StartViewY * ctx.ContentScale;
    float ex = m_EndViewX * ctx.ContentScale;
    float ey = m_EndViewY * ctx.ContentScale;
    if (!m_Is2D)
    {
        if (!ProjectWorldToView(m_StartWorld, w, h, sx, sy) ||
            !ProjectWorldToView(m_EndWorld, w, h, ex, ey))
            return;
    }

    sx += x;
    sy += y;
    ex += x;
    ey += y;

    const uint32_t primary = SceneMeasureColor(1.0f);
    const uint32_t secondary = SceneMeasureColor(0.55f);
    const uint32_t xColor = UI::PackColor(0.95f, 0.25f, 0.25f, 0.95f);
    const uint32_t yColor = UI::PackColor(0.25f, 0.85f, 0.35f, 0.95f);
    const uint32_t zColor = UI::PackColor(0.35f, 0.55f, 1.0f, 0.95f);

    EmitDottedLine(ctx, sx, sy, ex, ey, 3.0f, primary);

    const float dx = ex - sx;
    const float dy = ey - sy;
    const float screenLength = Len2(dx, dy);
    const bool showTriangle2D = m_TwoDMode == TwoDMode::Triangle;
    const bool diagonal2D = m_Is2D && showTriangle2D && std::fabs(dx) > 0.5f && std::fabs(dy) > 0.5f;

    if (diagonal2D)
    {
        const float cx = sx;
        const float cy = ey;
        EmitDottedLine(ctx, sx, sy, cx, cy, 1.4f, secondary, 6.0f);
        EmitDottedLine(ctx, cx, cy, ex, ey, 1.4f, secondary, 6.0f);

        const float horizontalAngle = std::atan2(std::fabs(dy), std::fabs(dx));
        const float verticalAngle = kPi * 0.5f - horizontalAngle;
        const float arcRadius = std::min({50.0f, screenLength * 0.10f, std::fabs(dx), std::fabs(dy)});

        const float dirX = dx >= 0.0f ? 1.0f : -1.0f;
        const float dirY = dy >= 0.0f ? 1.0f : -1.0f;
        const float vStart = dirY > 0.0f ? kPi * 0.5f : -kPi * 0.5f;
        const float vEnd = vStart - dirX * verticalAngle;
        EmitArc(ctx, sx, sy, arcRadius, vStart, vEnd, primary, 2.0f);

        const float hStart = dirX > 0.0f ? kPi : 0.0f;
        const float hEnd = hStart + dirY * horizontalAngle;
        EmitArc(ctx, ex, ey, arcRadius, hStart, hEnd, primary, 2.0f);
    }

    if (m_Is2D)
    {
        const bool startHovered = Len2(m_EndViewX - m_StartViewX, m_EndViewY - m_StartViewY) <= 8.0f;
        EmitEndpointDot(ctx, sx, sy, startHovered);
        EmitEndpointDot(ctx, ex, ey, true);
    }

    const float fontSize = 13.0f;
    const std::array<float, 3> deltaWorld = Sub3(m_EndWorld, m_StartWorld);
    char buf[96];

    if (m_Is2D)
    {
        const float wx = std::fabs(deltaWorld[0]);
        const float wy = std::fabs(deltaWorld[1]);
        const float lengthPx = std::sqrt(wx * wx + wy * wy);
        FormatFloat(buf, sizeof(buf), lengthPx, 1);
        std::string label = std::string(buf) + " px";

        std::string unitsLabel = Format2DUnitsLabel(m_Controller, lengthPx);
        const auto labelCenter = ValuePillCenterForLine(ctx, label, unitsLabel, fontSize, sx, sy, ex, ey);
        EmitValuePill(ctx, label, unitsLabel,
                      labelCenter[0],
                      labelCenter[1],
                      x,
                      y,
                      x + w,
                      y + h,
                      fontSize);

        if (diagonal2D)
        {
            const float cx = sx;
            const float cy = ey;
            FormatFloat(buf, sizeof(buf), wy, 1);
            EmitValuePill(ctx, std::string(buf) + " px", {},
                          sx + (sx < ex ? -42.0f : 42.0f),
                          (sy + cy) * 0.5f,
                          x,
                          y,
                          x + w,
                          y + h,
                          fontSize);

            FormatFloat(buf, sizeof(buf), wx, 1);
            EmitValuePill(ctx, std::string(buf) + " px", {},
                          (cx + ex) * 0.5f,
                          ey + (sy < ey ? 24.0f : -24.0f),
                          x,
                          y,
                          x + w,
                          y + h,
                          fontSize);

            const int hAngle = static_cast<int>(std::round(std::atan2(wy, wx) * 180.0f / kPi));
            const int vAngle = 90 - hAngle;
            std::snprintf(buf, sizeof(buf), "%d deg", vAngle);
            EmitValuePill(ctx, buf, {},
                          sx,
                          sy + (sy < ey ? 30.0f : -30.0f),
                          x,
                          y,
                          x + w,
                          y + h,
                          fontSize);
            std::snprintf(buf, sizeof(buf), "%d deg", hAngle);
            EmitValuePill(ctx, buf, {},
                          ex,
                          ey + (sy < ey ? 32.0f : -32.0f),
                          x,
                          y,
                          x + w,
                          y + h,
                          fontSize);
        }
    }
    else
    {
        const float dist = Len3(deltaWorld) < 0.001f ? 0.0f : Len3(deltaWorld);
        const std::string label = FormatWorldDistance(dist, m_UnitSystem);
        const bool startHovered = Len2(m_EndViewX - m_StartViewX, m_EndViewY - m_StartViewY) <= 8.0f;
        EmitEndpointDot(ctx, sx, sy, startHovered);
        EmitEndpointDot(ctx, ex, ey, true);
        const auto labelCenter = ValuePillCenterForLine(ctx, label, {}, fontSize, sx, sy, ex, ey);
        EmitValuePill(ctx, label, {},
                      labelCenter[0],
                      labelCenter[1],
                      x,
                      y,
                      x + w,
                      y + h,
                      fontSize);

        if (m_ShowComponents)
        {
            const std::array<float, 3> xPoint{m_EndWorld[0], m_StartWorld[1], m_StartWorld[2]};
            const std::array<float, 3> yPoint{m_EndWorld[0], m_EndWorld[1], m_StartWorld[2]};
            float x1 = 0.0f;
            float y1 = 0.0f;
            float x2 = 0.0f;
            float y2 = 0.0f;
            if (ProjectWorldToView(xPoint, w, h, x1, y1))
            {
                x1 += x;
                y1 += y;
                EmitDottedLine(ctx, sx, sy, x1, y1, 1.8f, xColor, 6.0f);
                if (ProjectWorldToView(yPoint, w, h, x2, y2))
                {
                    x2 += x;
                    y2 += y;
                    EmitDottedLine(ctx, x1, y1, x2, y2, 1.8f, yColor, 6.0f);
                    EmitDottedLine(ctx, x2, y2, ex, ey, 1.8f, zColor, 6.0f);
                }
                else
                {
                    EmitDottedLine(ctx, x1, y1, ex, ey, 1.8f, yColor, 6.0f);
                }
            }

            const float ax = std::fabs(deltaWorld[0]) < 0.001f ? 0.0f : std::fabs(deltaWorld[0]);
            const float ay = std::fabs(deltaWorld[1]) < 0.001f ? 0.0f : std::fabs(deltaWorld[1]);
            const float az = std::fabs(deltaWorld[2]) < 0.001f ? 0.0f : std::fabs(deltaWorld[2]);
            float labelY = (sy + ey) * 0.5f + 12.0f;
            if (ax > 0.0f)
            {
                const std::string component = "X: " + FormatWorldDistance(ax, m_UnitSystem);
                EmitTextOutlined(ctx, component, ClampLabelX((sx + ex) * 0.5f - 36.0f, x + w, 110.0f),
                                 ClampLabelY(labelY, y + h), fontSize, xColor);
                labelY += 16.0f;
            }
            if (ay > 0.0f)
            {
                const std::string component = "Y: " + FormatWorldDistance(ay, m_UnitSystem);
                EmitTextOutlined(ctx, component, ClampLabelX((sx + ex) * 0.5f - 36.0f, x + w, 110.0f),
                                 ClampLabelY(labelY, y + h), fontSize, yColor);
                labelY += 16.0f;
            }
            if (az > 0.0f)
            {
                const std::string component = "Z: " + FormatWorldDistance(az, m_UnitSystem);
                EmitTextOutlined(ctx, component, ClampLabelX((sx + ex) * 0.5f - 36.0f, x + w, 110.0f),
                                 ClampLabelY(labelY, y + h), fontSize, zColor);
            }
        }
    }
}

} // namespace GameEngine

namespace RegisterWidgets
{
static auto s_reg_sceneViewMeasureOverlay =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::SceneViewMeasureOverlay>(
        "SceneViewMeasureOverlay",
        []() { return std::make_unique<GameEngine::SceneViewMeasureOverlay>(); })
    .TagAlias("sceneviewmeasureoverlay");
} // namespace
