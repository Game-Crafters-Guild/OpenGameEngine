#include "Graph/GraphCanvas.h"

#include "Graph/GraphConnectionRouting.h"
#include "Graph/GraphRouteSolver.h"

#include "Graph/NodeColorSettings.h"
#include "Graph/GraphNodeOverlap.h"
#include "Platform/SystemMetrics.h"
#include "Editor/Settings/SettingsStore.h"
#include "Graph/GraphCanvasEditKeys.h"
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "Graph/GraphModel.h"
#include "Graph/GraphNodePool.h"
#include "Graph/GraphNodeRegistry.h"
#include "Graph/GraphOverlay.h"
#include "Graph/GraphPort.h"
#include "Graph/GraphPortedNode.h"
#include "Mathematics/Rect.h"
#include "Mathematics/Vector2.h"
#include "Types/StringId.h"
#include "UI/Internal/LayoutAccess.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"
#include "UI/UIPrimitive.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "Rendering/Geometry/ShapeBuilder.h"
#include "Types/ColorUtils.h"
#include "Logger/Logger.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace GameEngine {

using namespace Rendering::Geometry;

namespace
{
float g_NodeCornerRadius = GraphCanvas::kDefaultNodeCornerRadius;
bool g_NodeCornerRadiusLoaded = false;
bool g_ConnectionRoundedCorners = true;
bool g_ConnectionRoundedCornersLoaded = false;
bool g_NodeDropShadows = true;
bool g_NodeDropShadowsLoaded = false;
bool g_NodeDropShadowStyleLoaded = false;
float g_NodeDropShadowOffsetX = GraphCanvas::kDefaultNodeDropShadowOffsetX;
float g_NodeDropShadowOffsetY = GraphCanvas::kDefaultNodeDropShadowOffsetY;
float g_NodeDropShadowBlur = GraphCanvas::kDefaultNodeDropShadowBlur;
float g_NodeDropShadowOpacity = GraphCanvas::kDefaultNodeDropShadowOpacity;
std::string g_NodeHeaderAlignment = "left";
bool g_NodeHeaderAlignmentLoaded = false;
std::string g_SelectedNodeWireEmphasis = "glow";
bool g_SelectedNodeWireEmphasisLoaded = false;
std::unordered_set<GraphCanvas*> g_LiveGraphCanvases;

void LoadNodeDropShadowStyleIfNeeded()
{
    if (g_NodeDropShadowStyleLoaded)
        return;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    auto loadFloat = [&prefs](const char* key, float& out)
    {
        double stored = static_cast<double>(out);
        if (prefs.TryGetDouble(key, stored))
            out = static_cast<float>(stored);
    };
    loadFloat(GraphCanvas::kNodeDropShadowOffsetXPreference, g_NodeDropShadowOffsetX);
    loadFloat(GraphCanvas::kNodeDropShadowOffsetYPreference, g_NodeDropShadowOffsetY);
    loadFloat(GraphCanvas::kNodeDropShadowBlurPreference, g_NodeDropShadowBlur);
    loadFloat(GraphCanvas::kNodeDropShadowOpacityPreference, g_NodeDropShadowOpacity);
    g_NodeDropShadowBlur = std::max(0.0f, g_NodeDropShadowBlur);
    g_NodeDropShadowOpacity = std::clamp(g_NodeDropShadowOpacity, 0.0f, 1.0f);
    g_NodeDropShadowStyleLoaded = true;
}

std::string NormalizeNodeHeaderAlignment(std::string value)
{
    if (value == "center" || value == "right")
        return value;
    return "left";
}

/** Node-layer class per alignment. The stylesheet owns what each one does to
    the title; index with NodeHeaderAlignmentIndex. */
constexpr const char* kTitleAlignClasses[3] = {
    "gn-title-align--left", "gn-title-align--center", "gn-title-align--right"};

int NodeHeaderAlignmentIndex(const std::string& alignment)
{
    if (alignment == "center")
        return 1;
    if (alignment == "right")
        return 2;
    return 0;
}

std::string NormalizeSelectedNodeWireEmphasis(std::string value)
{
    if (value == "off" || value == "bright" || value == "halo")
        return value;
    return "glow";
}

/* Packed RGBA helpers for the wire emphasis: the wire keeps its port colour,
   the emphasis only lifts it toward white or fades it into a halo. */
uint32_t WireColorWithAlpha(uint32_t rgba, float alpha)
{
    const uint32_t a = static_cast<uint32_t>(std::clamp(alpha, 0.f, 1.f) * 255.f + 0.5f);
    return (rgba & 0x00FFFFFFu) | (a << 24);
}

uint32_t WireColorBrightened(uint32_t rgba, float amount)
{
    auto lift = [amount](uint32_t channel)
    {
        const float value = static_cast<float>(channel);
        return static_cast<uint32_t>(std::min(255.f, value + (255.f - value) * amount) + 0.5f);
    };
    const uint32_t r = lift(rgba & 0xFFu);
    const uint32_t g = lift((rgba >> 8) & 0xFFu);
    const uint32_t b = lift((rgba >> 16) & 0xFFu);
    return r | (g << 8) | (b << 16) | (rgba & 0xFF000000u);
}
}

struct GraphCanvas::EventTokens {
    UIElement::EventHandlerToken mouseDown;
    UIElement::EventHandlerToken mouseUp;
    UIElement::EventHandlerToken mouseMove;
    UIElement::EventHandlerToken mouseLeave;
    UIElement::EventHandlerToken scroll;
    UIElement::EventHandlerToken keyDown;
};

float GraphCanvas::GetNodeCornerRadius()
{
    if (!g_NodeCornerRadiusLoaded)
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        double stored = static_cast<double>(g_NodeCornerRadius);
        if (prefs.TryGetDouble(kNodeCornerRadiusPreference, stored))
            g_NodeCornerRadius = static_cast<float>(stored);
        g_NodeCornerRadius = std::clamp(g_NodeCornerRadius, kMinNodeCornerRadius, kMaxNodeCornerRadius);
        g_NodeCornerRadiusLoaded = true;
    }
    return g_NodeCornerRadius;
}

void GraphCanvas::SetNodeCornerRadius(float radius)
{
    g_NodeCornerRadius = std::clamp(radius, kMinNodeCornerRadius, kMaxNodeCornerRadius);
    g_NodeCornerRadiusLoaded = true;
    MarkLiveCanvasesDirty();
}

bool GraphCanvas::GetConnectionRoundedCorners()
{
    if (!g_ConnectionRoundedCornersLoaded)
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        bool stored = g_ConnectionRoundedCorners;
        if (prefs.TryGetBool(kConnectionRoundedCornersPreference, stored))
            g_ConnectionRoundedCorners = stored;
        g_ConnectionRoundedCornersLoaded = true;
    }
    return g_ConnectionRoundedCorners;
}

void GraphCanvas::SetConnectionRoundedCorners(bool enabled)
{
    g_ConnectionRoundedCorners = enabled;
    g_ConnectionRoundedCornersLoaded = true;
    MarkLiveCanvasesDirty();
}

bool GraphCanvas::GetNodeDropShadows()
{
    if (!g_NodeDropShadowsLoaded)
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        bool stored = g_NodeDropShadows;
        if (prefs.TryGetBool(kNodeDropShadowsPreference, stored))
            g_NodeDropShadows = stored;
        g_NodeDropShadowsLoaded = true;
    }
    return g_NodeDropShadows;
}

void GraphCanvas::SetNodeDropShadows(bool enabled)
{
    g_NodeDropShadows = enabled;
    g_NodeDropShadowsLoaded = true;
    MarkLiveCanvasesDirty();
}

void GraphCanvas::SetNodeDropShadowStyle(float offsetX, float offsetY, float blur, float opacity)
{
    g_NodeDropShadowOffsetX = offsetX;
    g_NodeDropShadowOffsetY = offsetY;
    g_NodeDropShadowBlur = std::max(0.0f, blur);
    g_NodeDropShadowOpacity = std::clamp(opacity, 0.0f, 1.0f);
    g_NodeDropShadowStyleLoaded = true;
    MarkLiveCanvasesDirty();
}

std::string GraphCanvas::GetNodeHeaderAlignment()
{
    if (!g_NodeHeaderAlignmentLoaded)
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        std::string stored = g_NodeHeaderAlignment;
        if (prefs.TryGetString(kNodeHeaderAlignmentPreference, stored))
            g_NodeHeaderAlignment = NormalizeNodeHeaderAlignment(std::move(stored));
        else
            g_NodeHeaderAlignment = "left";
        g_NodeHeaderAlignmentLoaded = true;
    }
    return g_NodeHeaderAlignment;
}

void GraphCanvas::SetNodeHeaderAlignment(const std::string& alignment)
{
    g_NodeHeaderAlignment = NormalizeNodeHeaderAlignment(alignment);
    g_NodeHeaderAlignmentLoaded = true;
    MarkLiveCanvasesDirty();
}

std::string GraphCanvas::GetSelectedNodeWireEmphasis()
{
    if (!g_SelectedNodeWireEmphasisLoaded)
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        std::string stored = g_SelectedNodeWireEmphasis;
        if (prefs.TryGetString(kSelectedNodeWireEmphasisPreference, stored))
            g_SelectedNodeWireEmphasis = NormalizeSelectedNodeWireEmphasis(std::move(stored));
        g_SelectedNodeWireEmphasisLoaded = true;
    }
    return g_SelectedNodeWireEmphasis;
}

void GraphCanvas::SetSelectedNodeWireEmphasis(const std::string& emphasis)
{
    g_SelectedNodeWireEmphasis = NormalizeSelectedNodeWireEmphasis(emphasis);
    g_SelectedNodeWireEmphasisLoaded = true;
    MarkLiveCanvasesDirty();
}

void GraphCanvas::MarkLiveCanvasesDirty()
{
    for (GraphCanvas* canvas : g_LiveGraphCanvases)
    {
        if (!canvas)
            continue;
        canvas->ApplyNodeDropShadows();
        canvas->ApplyNodeTitleAlignment();
        canvas->ApplyNodeLayerZoomClass();
        /* Node colours ride on per-node style variables published during bind,
           so a settings change only reaches the canvas through a rebind. */
        canvas->RebindVisibleNodes();
        canvas->MarkDirty(UIElement::VisualDirty);
    }
}

void GraphCanvas::ApplyNodeDropShadows()
{
    if (!m_NodePool)
        return;

    LoadNodeDropShadowStyleIfNeeded();

    /* The settings own the numbers; node-graph.css owns which elements wear
       them and what a drop target does instead. Published on the canvas so the
       whole node layer inherits one value rather than a per-slot override. */
    char shadow[96];
    if (GetNodeDropShadows())
        std::snprintf(shadow, sizeof(shadow), "%gpx %gpx %gpx rgba(0, 0, 0, %g)",
                      static_cast<double>(g_NodeDropShadowOffsetX),
                      static_cast<double>(g_NodeDropShadowOffsetY),
                      static_cast<double>(g_NodeDropShadowBlur),
                      static_cast<double>(std::clamp(g_NodeDropShadowOpacity, 0.0f, 1.0f)));
    else
        std::snprintf(shadow, sizeof(shadow), "none");
    Overrides().SetCustom(HashStringId("--graph-node-shadow"), std::string(shadow));
}

float GraphCanvas::SnapGraphCoordinate(float value)
{
    return GraphNodeOverlap::SnapCoordinate(value);
}

void GraphCanvas::SnapGraphPosition(float& x, float& y)
{
    x = SnapGraphCoordinate(x);
    y = SnapGraphCoordinate(y);
}

namespace {

bool ArePortTypesCompatible(const std::string& sourceType, const std::string& targetType)
{
    if (sourceType.empty() || targetType.empty() || sourceType == "any" || targetType == "any")
        return true;
    if (sourceType == targetType)
        return true;
    return (sourceType == "float3" && targetType == "float4") ||
           (sourceType == "float4" && targetType == "float3");
}

bool IsGenericOutputPortId(const std::string& portId)
{
    return portId == "value" || portId == "result" || portId == "out";
}

size_t FindPortIndex(const Graph::Node& node, const std::string& portId)
{
    for (size_t i = 0; i < node.Ports.size(); ++i)
    {
        if (node.Ports[i].Id == portId)
            return i;
    }

    if (!IsGenericOutputPortId(portId))
        return node.Ports.size();

    for (size_t i = 0; i < node.Ports.size(); ++i)
    {
        const Graph::Port& port = node.Ports[i];
        if (port.Direction == Graph::PortDirection::Out && IsGenericOutputPortId(port.Id))
            return i;
    }

    size_t soleOutput = node.Ports.size();
    int outputCount = 0;
    for (size_t i = 0; i < node.Ports.size(); ++i)
    {
        if (node.Ports[i].Direction != Graph::PortDirection::Out)
            continue;
        soleOutput = i;
        ++outputCount;
    }
    return outputCount == 1 ? soleOutput : node.Ports.size();
}

float GetNodeWidth(const Graph::Node& node)
{
    return GraphCanvas::GetNodeWidth(node);
}

/* Pure moves take the fast path inside SetGraphRect; committed child layout
   is offset alongside so the drag paints this frame instead of waiting for
   the next solve. */
void OffsetLayoutSubtree(UIElement* el, float dx, float dy)
{
    if (!el || (dx == 0.0f && dy == 0.0f))
        return;
    UILayoutAccess::SetLastLayoutRect(*el,
                                      el->GetLayoutX() + dx,
                                      el->GetLayoutY() + dy,
                                      el->GetLayoutWidth(),
                                      el->GetLayoutHeight());
    el->MarkDirty(UIElement::VisualDirty);
    for (const auto& child : el->GetChildren())
        OffsetLayoutSubtree(child.get(), dx, dy);
}

void ApplyNodeWidgetRect(GraphPortedNode& slot, const Graph::Node& node, float zoom,
                         float nodeHeightGraph)
{
    const Mathematics::Rect next{
        node.PositionX * zoom,
        node.PositionY * zoom,
        GetNodeWidth(node) * zoom,
        nodeHeightGraph * zoom
    };
    Mathematics::Rect prev{};
    const bool hadPrev = UI::Layout::TryGetAbsolutePosition(slot, prev);
    /* Drag-only fast path: pure moves skip layout dirt, and the committed
       child rects (ring, ports, hosts) are offset right here so the whole
       widget paints at the new spot this frame. Every other caller goes
       through SetGraphRect's full layout path. */
    if (!UI::Layout::SetAbsolutePosition(slot, next, /*positionOnlyFastPath=*/true))
        return;
    if (hadPrev && (slot.GetLayoutWidth() > 0.0f || slot.GetLayoutHeight() > 0.0f))
        OffsetLayoutSubtree(&slot, next.X - prev.X, next.Y - prev.Y);
}

void HideTooltip(GraphCanvas* canvas)
{
    if (!canvas)
        return;
    canvas->SetTooltip({});
    if (UIManager* ui = canvas->GetOwnerManager())
        ui->DismissActiveTooltip();
}

std::string NormalizedPortType(std::string type)
{
    std::transform(type.begin(), type.end(), type.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (type == "float2")
        return "vec2";
    if (type == "float3")
        return "vec3";
    if (type == "float4")
        return "vec4";
    return type;
}

uint32_t PortTypeColorArgb(const std::string& dataType)
{
    const std::string type = NormalizedPortType(dataType);
    if (type.empty() || type == "any")
        return 0xFF9A9A9Au;
    if (type == "flow" || type == "exec" || type == "event")
        return 0xFFE7E7E7u;
    if (type == "bool")
        return 0xFFFF5E73u;
    if (type == "int" || type == "uint")
        return 0xFF7BD88Fu;
    if (type == "float" || type == "double")
        return 0xFF7FB7FFu;
    if (type == "vec2")
        return 0xFF7BD8D8u;
    if (type == "vec3")
        return 0xFFFFC857u;
    if (type == "vec4" || type == "color")
        return 0xFFFF8BD1u;
    if (type.find("texture") != std::string::npos || type.find("sampler") != std::string::npos ||
        type.find("cubemap") != std::string::npos)
        return 0xFFB98BFFu;
    if (type == "string")
        return 0xFFFFA657u;
    return 0xFFA6A6A6u;
}

/* Gap a wire keeps from a node body it is not attached to, in graph units. */
constexpr float kWireClearanceGraph = 16.f;
/* And from the two it is attached to. Smaller, because the wire has to reach
   their ports, but not nothing: a wire flush against its own node reads as part
   of the node's border. */
constexpr float kOwnBodyGapGraph = 10.f;
/* Port circles are centred on the node edge and stand out past it, so the gap a
   wire keeps from a node it passes is measured from the ports, not the body. */
constexpr float kPortFootprintGraph = 6.f;

/* Separation between wires that would otherwise share a column or a row, in
   graph units like every other routing distance: a wire's shape is a property
   of the graph, so a zoom change rescales the picture without redrawing it. */
constexpr float kWireSeparationGraph = 12.f;

float ConnectionStubOffsetPx(float zoom)
{
    constexpr float kMinStubPx = 8.f;
    constexpr float kStubAtUnitZoomPx = 14.f;
    return std::max(kMinStubPx, kStubAtUnitZoomPx * zoom);
}

/* Second press on the same target inside the double-click interval, stamping it
   as the last one either way. Ports and node bodies share the tracker: the empty
   port id is the body, so a click on one clears a pending double on the other. */
bool ClickedTwice(GraphCanvas::PressTracker& tracker, const std::string& nodeId,
                  const std::string& portId)
{
    const auto now = std::chrono::steady_clock::now();
    const bool twice = nodeId == tracker.NodeId && portId == tracker.PortId &&
                       (now - tracker.At) < GameEngine::Platform::GetDoubleClickInterval();
    tracker.NodeId = nodeId;
    tracker.PortId = portId;
    tracker.At = now;
    return twice;
}

bool WireAuditEnabled()
{
    static const bool enabled = []
    {
        const char* value = std::getenv("GE_GRAPH_WIRE_AUDIT");
        return value && value[0] == '1';
    }();
    return enabled;
}

struct ScreenRect
{
    float Left = 0.f;
    float Top = 0.f;
    float Right = 0.f;
    float Bottom = 0.f;
};

bool VerticalSegmentIntersectsRect(float x, float y0, float y1, const ScreenRect& rect, float margin)
{
    const float yMin = std::min(y0, y1);
    const float yMax = std::max(y0, y1);
    return x >= rect.Left - margin && x <= rect.Right + margin &&
           yMax >= rect.Top - margin && yMin <= rect.Bottom + margin;
}

bool VerticalRiserBlocked(float riserX, float y0, float y1,
                          const std::vector<ScreenRect>& obstacles, float margin)
{
    if (std::abs(y1 - y0) < 1.f)
        return false;

    for (const ScreenRect& rect : obstacles)
    {
        if (VerticalSegmentIntersectsRect(riserX, y0, y1, rect, margin))
            return true;
    }
    return false;
}

float ShiftRiserXLeftUntilClear(float riserX, float exitX, float sy0, float sy1,
                                const std::vector<ScreenRect>& obstacles,
                                float margin, float trackOffsetPx)
{
    float x = riserX;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        if (!VerticalRiserBlocked(x, sy0, sy1, obstacles, margin))
            return x;
        x -= trackOffsetPx;
        if (x < exitX)
            return riserX;
    }
    return riserX;
}

/* A wire crosses a body when any of its runs passes inside the body's
   clearance box. Endpoint runs are exempt at their own ends: ports sit on the
   node edge, so the first and last stub always start inside one. */
bool RouteCrossesObstacle(const std::vector<Mathematics::Vector2>& points,
                          const std::vector<GraphRouting::Obstacle>& obstacles, float clearance)
{
    for (size_t i = 1; i < points.size(); ++i)
    {
        const bool vertical = std::abs(points[i].x - points[i - 1].x) < 0.5f;
        const bool isStub = i == 1 || i + 1 == points.size();
        for (const GraphRouting::Obstacle& obstacle : obstacles)
        {
            if (obstacle.PortRowsExempt && isStub)
                continue; // the stub leaves through its own node's gap by design
            const float gap = obstacle.Clearance >= 0.f ? obstacle.Clearance : clearance;
            const Mathematics::Rect box = obstacle.Body.Inflated(gap);
            const float left = box.X;
            const float right = box.Right();
            const float top = box.Y;
            const float bottom = box.Bottom();
            if (vertical)
            {
                if (points[i].x <= left || points[i].x >= right)
                    continue;
                if (std::max(points[i - 1].y, points[i].y) > top &&
                    std::min(points[i - 1].y, points[i].y) < bottom)
                    return true;
            }
            else
            {
                if (points[i].y <= top || points[i].y >= bottom)
                    continue;
                if (std::max(points[i - 1].x, points[i].x) > left &&
                    std::min(points[i - 1].x, points[i].x) < right)
                    return true;
            }
        }
    }
    return false;
}

float ChooseDetourLaneY(float sy0, float sy1, float riserX,
                        const std::vector<ScreenRect>& obstacles,
                        float clearance, int trackIdx, float trackOffsetPx)
{
    const float yMin = std::min(sy0, sy1);
    const float yMax = std::max(sy0, sy1);
    float blockBottom = yMin;
    float blockTop = yMax;
    for (const ScreenRect& rect : obstacles)
    {
        if (!VerticalSegmentIntersectsRect(riserX, sy0, sy1, rect, clearance))
            continue;
        blockBottom = std::max(blockBottom, rect.Bottom);
        blockTop = std::min(blockTop, rect.Top);
    }

    const float belowY = blockBottom + clearance + static_cast<float>(trackIdx) * trackOffsetPx;
    const float aboveY = blockTop - clearance - static_cast<float>(trackIdx) * trackOffsetPx;
    const float midY = yMin + (yMax - yMin) * 0.5f;
    const float belowCost = std::abs(belowY - sy0) + std::abs(sy1 - belowY);
    const float aboveCost = std::abs(sy0 - aboveY) + std::abs(aboveY - sy1);
    const float midCost = std::abs(midY - sy0) + std::abs(sy1 - midY);

    float laneY = midY;
    float bestCost = midCost;
    if (belowCost < bestCost)
    {
        laneY = belowY;
        bestCost = belowCost;
    }
    if (aboveCost < bestCost)
        laneY = aboveY;

    // Keep the detour lane between the ports when possible so the wire does not
    // dip below/above and hook back into the target port.
    if (laneY < yMin - clearance)
        laneY = yMin - clearance - static_cast<float>(trackIdx) * trackOffsetPx;
    if (laneY > yMax + clearance)
        laneY = yMax + clearance + static_cast<float>(trackIdx) * trackOffsetPx;
    return laneY;
}

float ConnectionCornerRadiusPx(float zoom)
{
    return std::max(4.f, 6.f * zoom);
}

void EmitRoundedPolyline(const std::vector<float>& xs, const std::vector<float>& ys,
                         float thickness, uint32_t color, float cornerRadius,
                         const std::function<void(float, float, float, float)>& emitLine,
                         const std::function<void(float, float, float, float, float, float, float, float)>& emitBezier)
{
    const size_t n = xs.size();
    if (n < 2 || ys.size() != n)
        return;
    if (n == 2)
    {
        emitLine(xs[0], ys[0], xs[1], ys[1]);
        return;
    }

    constexpr float kBezierArcK = 0.5522847498f;
    float sx = xs[0], sy = ys[0];

    for (size_t i = 0; i + 1 < n; ++i)
    {
        const float ex = xs[i + 1], ey = ys[i + 1];
        const bool hasNext = i + 2 < n;

        const float dx = ex - sx, dy = ey - sy;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4f)
            continue;

        const float ux = dx / len, uy = dy / len;

        if (!hasNext)
        {
            emitLine(sx, sy, ex, ey);
            break;
        }

        const float nx = xs[i + 2], ny = ys[i + 2];
        const float d2x = nx - ex, d2y = ny - ey;
        const float len2 = std::sqrt(d2x * d2x + d2y * d2y);
        if (len2 < 1e-4f)
        {
            emitLine(sx, sy, ex, ey);
            sx = ex;
            sy = ey;
            continue;
        }

        const float ux2 = d2x / len2, uy2 = d2y / len2;
        const float dot = ux * ux2 + uy * uy2;
        if (dot > 0.999f)
        {
            emitLine(sx, sy, ex, ey);
            sx = ex;
            sy = ey;
            continue;
        }
        if (dot < -0.999f)
        {
            // Skip reversal points (U-turn collinear overlap); the route should not
            // include them, but ignore the backtrack segment if it does.
            sx = ex;
            sy = ey;
            continue;
        }

        const float cross = ux * uy2 - uy * ux2;
        if (std::abs(cross) < 1e-4f)
        {
            emitLine(sx, sy, ex, ey);
            sx = ex;
            sy = ey;
            continue;
        }

        const float r = std::min({cornerRadius, len * 0.5f, len2 * 0.5f});
        const float trimX = ex - ux * r, trimY = ey - uy * r;
        emitLine(sx, sy, trimX, trimY);

        const float startX = ex + ux2 * r, startY = ey + uy2 * r;
        const float alpha = r * kBezierArcK;
        emitBezier(trimX, trimY,
                   trimX + alpha * ux, trimY + alpha * uy,
                   startX - alpha * ux2, startY - alpha * uy2,
                   startX, startY);

        sx = startX;
        sy = startY;
    }
}

void ComputeConnectionBezierControls(float sx0, float sy0, float sx1, float sy1, float zoom,
                                     float& cx0, float& cy0, float& cx1, float& cy1)
{
    const float offset = GraphConnectionRouting::BezierControlOffset(sx1 - sx0, sy1 - sy0, zoom);
    cx0 = sx0 + offset;
    cy0 = sy0;
    cx1 = sx1 - offset;
    cy1 = sy1;
}

} // namespace

GraphCanvas::GraphCanvas(Graph::Model* model)
    : m_Model(model)
{
    g_LiveGraphCanvases.insert(this);
    if (m_Model)
    {
        m_PanX = m_Model->Viewport.PanX;
        m_PanY = m_Model->Viewport.PanY;
        m_Zoom = std::max(kMinZoom, std::min(kMaxZoom, m_Model->Viewport.Zoom));
        SnapPanToDevicePixels();
    }
    AddClass("node-canvas");
    m_Tokens = std::make_unique<EventTokens>();
    RegisterEventHandlers();
    EnsureNodeChrome();
    RebuildNodeWidgets();
    MaybeArmSpikeDummyNodes();
}

GraphCanvas::~GraphCanvas()
{
    g_LiveGraphCanvases.erase(this);
    UnregisterEventHandlers();
}

namespace {

/* CSS `.graph-zK { font-size }` is 13px × kMinZoom × kZoomTierRatio^K so type
   tracks the node rect across the 10 zoom tiers. */
constexpr int kZoomTierCount = 10;
constexpr float kZoomTierBase = GraphCanvas::kMinZoom;
constexpr float kZoomTierRatio = 1.196f;
constexpr const char* kZoomTierClasses[kZoomTierCount] = {
    "graph-z0", "graph-z1", "graph-z2", "graph-z3", "graph-z4",
    "graph-z5", "graph-z6", "graph-z7", "graph-z8", "graph-z9"
};

int PickZoomTier(float zoom)
{
    int best = 0;
    float bestDist = 1.0e9f;
    float scale = kZoomTierBase;
    for (int k = 0; k < kZoomTierCount; ++k)
    {
        const float d = std::abs(scale - zoom);
        if (d < bestDist)
        {
            bestDist = d;
            best = k;
        }
        scale *= kZoomTierRatio;
    }
    return best;
}

} // namespace

void GraphCanvas::EnsureNodeChrome()
{
    auto layer = std::make_unique<UIElement>();
    layer->AddClass("graph-node-layer");
    layer->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(m_PanX))
        .Set(Style::PositionTop, StyleLength::Px(m_PanY))
        .Set(Style::PositionRight, StyleLength::Auto())
        .Set(Style::PositionBottom, StyleLength::Auto())
        .Set(Style::Width, StyleLength::Px(0.0f))
        .Set(Style::Height, StyleLength::Px(0.0f))
        .Set(Style::PointerEvents, false)
        .Set(Style::OverflowProp, Overflow::Visible);
    m_NodeLayer = layer.get();
    AddChild(std::move(layer));

    auto overlay = std::make_unique<GraphOverlay>();
    m_Overlay = overlay.get();
    AddChild(std::move(overlay));

    m_NodePool = std::make_unique<GraphNodePool>(m_NodeLayer, this);
    ApplyNodeLayerZoomClass();
    // A fresh layer wears no class yet, so the cached index cannot stand.
    m_NodeTitleAlignIndex = -1;
    ApplyNodeTitleAlignment();
}

void GraphCanvas::MaybeArmSpikeDummyNodes()
{
    const char* env = std::getenv("GE_GRAPH_SPIKE_NODES");
    if (!env || env[0] == '\0')
        return;
    const int count = std::atoi(env);
    if (count > 0)
        SpikeSpawnDummyNodes(count);
}

void GraphCanvas::ApplyNodeTitleAlignment()
{
    if (!m_NodeLayer)
        return;
    const int index = NodeHeaderAlignmentIndex(GetNodeHeaderAlignment());
    if (index == m_NodeTitleAlignIndex)
        return;
    if (m_NodeTitleAlignIndex >= 0)
        m_NodeLayer->RemoveClass(kTitleAlignClasses[m_NodeTitleAlignIndex]);
    m_NodeLayer->AddClass(kTitleAlignClasses[index]);
    m_NodeTitleAlignIndex = index;
}

void GraphCanvas::ApplyNodeLayerZoomClass()
{
    if (!m_NodeLayer)
        return;
    const int tier = PickZoomTier(m_Zoom);
    if (tier != m_NodeLayerZoomTier)
    {
        if (m_NodeLayerZoomTier >= 0 && m_NodeLayerZoomTier < kZoomTierCount)
            m_NodeLayer->RemoveClass(kZoomTierClasses[m_NodeLayerZoomTier]);
        m_NodeLayer->AddClass(kZoomTierClasses[tier]);
        m_NodeLayerZoomTier = tier;
    }

    /* Continuous zoom for the stylesheet — published on every zoom change,
       not per tier, or calc()-scaled insets and the radii would freeze
       between tier switches. --length-scale drives the calc() lengths; the
       corner radii publish as already-multiplied px so the user's
       corner-radius setting stays live. The ring radius wraps the shell by
       half the 2-unit ring line. */
    const float publishedRadius = GetNodeCornerRadius();
    if (m_PublishedZoom != m_Zoom || m_PublishedCornerRadius != publishedRadius)
    {
        m_PublishedZoom = m_Zoom;
        m_PublishedCornerRadius = publishedRadius;
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%.4f", m_Zoom);
        static const StringId kLengthScaleVar = HashStringId("--length-scale");
        m_NodeLayer->Overrides().SetCustom(kLengthScaleVar, buf);

        const float radiusPx = publishedRadius * m_Zoom;
        static const StringId kNodeRadiusVar = HashStringId("--graph-node-radius");
        static const StringId kValueRadiusVar = HashStringId("--graph-value-radius");
        static const StringId kRingRadiusVar = HashStringId("--graph-ring-radius");
        std::snprintf(buf, sizeof(buf), "%.2fpx", radiusPx);
        m_NodeLayer->Overrides().SetCustom(kNodeRadiusVar, buf);
        m_NodeLayer->Overrides().SetCustom(kValueRadiusVar, buf);
        std::snprintf(buf, sizeof(buf), "%.2fpx", radiusPx + std::max(1.0f, m_Zoom));
        m_NodeLayer->Overrides().SetCustom(kRingRadiusVar, buf);
        static const StringId kRingWidthVar = HashStringId("--graph-ring-width");
        /* Never thinner than the 2px base: a ~1px border on the fractional
           edges a zoomed-out layout produces drops below pixel coverage on
           some sides and the selection ring renders partially. Selection
           chrome keeps screen weight at zoom-out (it scales up with zoom-in
           as before). */
        std::snprintf(buf, sizeof(buf), "%.2fpx", std::max(2.0f, 2.0f * m_Zoom));
        m_NodeLayer->Overrides().SetCustom(kRingWidthVar, buf);
        m_NodeLayer->MarkDirty(StyleDirty);
    }
}

void GraphCanvas::SyncNodeLayerTransform()
{
    if (!m_NodeLayer)
        return;

    // Layout impact: Yoga must restyle left/top this frame. SetPositionOnlyPx is
    // visual-only, so committed node rects stay put while the overlay (which
    // draws from m_PanX) moves — edges slide, nodes jump on the next solve.
    m_NodeLayer->Overrides()
        .Set(Style::PositionLeft, StyleLength::Px(m_PanX))
        .Set(Style::PositionTop, StyleLength::Px(m_PanY));

    ApplyNodeLayerZoomClass();
    if (m_LaidOutZoom != m_Zoom)
    {
        m_LaidOutZoom = m_Zoom;
        LayoutSpikeDummyNodes();
        RebuildNodeWidgets();
    }
    m_NodeLayer->MarkDirty(VisualDirty);
}

void GraphCanvas::LayoutSpikeDummyNodes()
{
    if (!m_NodePool || m_SpikeDummyCount <= 0)
        return;

    constexpr int kCols = 6;
    constexpr float kGapX = 40.0f;
    constexpr float kGapY = 40.0f;
    bool anyChanged = false;
    for (int i = 0; i < m_SpikeDummyCount; ++i)
    {
        GraphPortedNode* node = m_NodePool->FindByModelId("spike-" + std::to_string(i));
        if (!node)
            continue;
        const int col = i % kCols;
        const int row = i / kCols;
        const float gx = static_cast<float>(col) * (kNodeWidth + kGapX);
        const float gy = static_cast<float>(row) * (kNodeHeight + kGapY);
        const Mathematics::Rect rect{
            gx * m_Zoom,
            gy * m_Zoom,
            kNodeWidth * m_Zoom,
            kNodeHeight * m_Zoom
        };
        if (node->SetGraphRect(rect))
            anyChanged = true;
    }
    if (anyChanged)
        MarkDirty(VisualDirty);
}

void GraphCanvas::SpikeSpawnDummyNodes(int count)
{
    if (count <= 0 || !m_NodePool)
        return;

    m_SpikeDummyCount = count;
    for (int i = 0; i < count; ++i)
    {
        const std::string spikeId = "spike-" + std::to_string(i);
        GraphPortedNode* node = m_NodePool->FindByModelId(spikeId);
        if (!node)
            node = m_NodePool->Acquire();
        if (!node)
            continue;
        node->Reset();
        node->SetModelNodeId(spikeId);
        node->EnsurePortCount(2, 2);
        for (size_t p = 0; p < 2; ++p)
        {
            if (GraphPort* port = node->InputPortAt(p))
                UI::Layout::SetElementHidden(*port, false);
            if (GraphPort* port = node->OutputPortAt(p))
                UI::Layout::SetElementHidden(*port, false);
        }
        node->SetTitleText("Dummy " + std::to_string(i));
        node->SetValueText("spike");
        node->ApplyVisualState({i == 0, i == 1, i == 2, i == 3, false});
    }
    m_LaidOutZoom = m_Zoom;
    LayoutSpikeDummyNodes();
    Logger::Log::Info("[GraphCanvas] SpikeSpawnDummyNodes count={}", count);
}

void GraphCanvas::SetModel(Graph::Model* model)
{
    m_PendingHiddenNodeIds.clear();
    m_PendingHiddenLinkIds.clear();
    m_DeferredModelMutations.clear();
    m_DeferredModelMutationSkipOnce = false;
    const bool graphChanged = m_Model != model;
    m_Model = model;
    /* Only when a different graph arrives. Metrics change between sessions
       (wider nodes, taller rows), so positions saved under old sizes can
       collide on load, and that is worth correcting once. Re-running it every
       time the panel re-sets the same pointer walks the author's layout across
       the canvas a little further on each pass. */
    if (m_Model && graphChanged)
        ResolveAllNodeOverlaps();
    RebuildNodeWidgets();
}

void GraphCanvas::ResolveAllNodeOverlaps()
{
    if (!m_Model)
        return;
    const auto getWidth = [](const Graph::Node& n) { return GetNodeWidth(n); };
    const auto getHeight = [this](const Graph::Node& n) { return GetNodeRectHeight(n); };
    GraphNodeOverlap::ResolveAllNodeOverlaps(*m_Model, getWidth, getHeight);
}

float GraphCanvas::GetNodeRectHeight(const Graph::Node& node) const
{
    const bool drawsInlineEditors =
        m_NodePool ? m_NodePool->DrawsInlinePortEditors(node) : true;
    const bool hasCentralEditors =
        m_NodePool ? m_NodePool->HasCentralEditors(node) : false;
    const float extraBlock =
        m_NodePool ? m_NodePool->ReservedBlockHeight(node, m_ExpandedNodes) : 0.f;
    const int extraRows =
        m_NodePool ? m_NodePool->SyntheticDetailRowCount(node, m_ExpandedNodes) : 0;
    return GraphNodeMetrics::GetNodeHeight(node, drawsInlineEditors, hasCentralEditors,
                                                m_ExpandedNodes, extraBlock, extraRows);
}

void GraphCanvas::RefreshBoundNodeValues()
{
    RebuildNodeWidgets();
}

void GraphCanvas::SetExpandedNodes(bool expanded)
{
    if (m_ExpandedNodes == expanded)
        return;
    m_ExpandedNodes = expanded;
    if (expanded && m_Model)
    {
        /* Rects grow: placements that cleared each other at base height can
           collide now. Resolve in model order so the shuffle is deterministic. */
        ResolveAllNodeOverlaps();
    }
    // Node rects change, so every widget needs the full layout path, not the
    // drag fast path — and the overlay's wire obstacles are stale too.
    RebuildNodeWidgets();
    MarkDirty(VisualDirty);
}

void GraphCanvas::SetNodeFactory(NodeFactoryFn factory)
{
    if (!m_NodePool)
        return;
    m_NodePool->SetFactory(std::move(factory));
    RebuildNodeWidgets();
}

void GraphCanvas::SetNodeSearchFilter(const std::string& filter)
{
    m_NodeSearchFilter = filter;
    RebuildNodeWidgets();
    MarkDirty(VisualDirty);
}

void GraphCanvas::SetNodeSearchField(const std::string& field)
{
    m_NodeSearchField = field;
    RebuildNodeWidgets();
    MarkDirty(VisualDirty);
}

bool GraphCanvas::NodeMatchesSearch(const Graph::Node& node) const
{
    if (m_NodeSearchFilter.empty() || !m_Model)
        return false;
    const NodeTypeMeta* meta = GraphNodeRegistry::Get().FindNodeMeta(m_Model->KindId, node.TypeId);
    std::string displayName = meta ? meta->DisplayName : node.TypeId;
    std::string query = m_NodeSearchFilter;
    std::string typeId = node.TypeId;
    auto toLower = [](std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    };
    toLower(query);
    toLower(displayName);
    toLower(typeId);
    if (m_NodeSearchField == "name")
        return displayName.find(query) != std::string::npos;
    if (m_NodeSearchField == "type")
        return typeId.find(query) != std::string::npos;
    return displayName.find(query) != std::string::npos || typeId.find(query) != std::string::npos;
}

void GraphCanvas::ApplyLiveEdit(const std::string& nodeId,
                               const std::function<void(Graph::Node&)>& write)
{
    if (!m_Model || !write)
        return;
    Graph::Node* node = m_Model->FindNode(nodeId);
    if (!node)
        return;
    /* Remember once per drag, on the first step. A second drag on another node
       replaces it: only one can be in flight. */
    if (!m_HasLiveEdit || m_LiveEditNodeId != nodeId)
    {
        m_LiveEditNodeId = nodeId;
        m_LiveEditParams = node->Parameters;
        m_HasLiveEdit = true;
    }
    write(*node);
    NotifyGraphChanged();
}

void GraphCanvas::RestoreStateBeforeLiveEdit()
{
    if (!m_HasLiveEdit)
        return;
    /* Silent: no notification. The scope's own mutation moves it forward once. */
    if (m_Model)
    {
        if (Graph::Node* node = m_Model->FindNode(m_LiveEditNodeId))
            node->Parameters = m_LiveEditParams;
    }
    m_LiveEditParams = Graph::GraphObject{};
    m_LiveEditNodeId.clear();
    m_HasLiveEdit = false;
}

void GraphCanvas::FillNodeEditHost(GraphNodeEditHost& host)
{
    host.Model = m_Model;
    /* Restore first: the drag already moved the model, so the scope would take
       the finished state as its own "before" and record a no-op. */
    host.Undo = [this](const std::string& name, std::function<void()> mutate)
    {
        RestoreStateBeforeLiveEdit();
        if (m_UndoScope)
            m_UndoScope(name, std::move(mutate));
    };
    host.EditLive = [this](const std::string& nodeId,
                           const std::function<void(Graph::Node&)>& write)
    { ApplyLiveEdit(nodeId, write); };
    host.ExpandedView = m_ExpandedNodes;
    host.Assets = m_AssetRegistry;
    host.Thumbnails = m_ThumbnailProvider;
    host.OnChanged = [this]() { NotifyGraphChanged(); };
    host.IsInputConnected = [this](const std::string& nodeId, const std::string& portId)
    {
        if (!m_Model)
            return false;
        for (const Graph::Edge& link : m_Model->Links)
        {
            if (IsLinkHiddenPendingDelete(link))
                continue;
            if (link.TargetNodeId == nodeId && link.TargetPortId == portId)
                return true;
        }
        return false;
    };
}

bool GraphCanvas::IsLinkHiddenPendingDelete(const Graph::Edge& link) const
{
    if (m_PendingHiddenLinkIds.count(link.Id) != 0)
        return true;
    return m_PendingHiddenNodeIds.count(link.SourceNodeId) != 0
        || m_PendingHiddenNodeIds.count(link.TargetNodeId) != 0;
}

bool GraphCanvas::BindLiveNode(const Graph::Node& node)
{
    if (!m_NodePool || !m_Model || node.Id.empty())
        return false;
    GraphPortedNode* slot = m_NodePool->FindByModelId(node.Id);
    if (!slot)
        slot = m_NodePool->Acquire();
    if (!slot)
        return false;
    GraphNodeEditHost host;
    FillNodeEditHost(host);
    GraphNodeVisualState visual;
    visual.Selected = IsNodeSelected(node.Id);
    visual.SearchMatch = NodeMatchesSearch(node);
    visual.RuntimeActive = m_RuntimeActiveNodeIds.count(node.Id) != 0;
    visual.HasError = m_ErrorNodeIds.count(node.Id) != 0;
    slot->BindModel(node, m_Model->KindId, visual, &host);
    const Mathematics::Rect rect{
        node.PositionX * m_Zoom,
        node.PositionY * m_Zoom,
        GetNodeWidth(node) * m_Zoom,
        GetNodeRectHeight(node) * m_Zoom
    };
    return slot->SetGraphRect(rect);
}

void GraphCanvas::RebuildNodeWidgets()
{
    if (!m_NodePool || m_SyncingModelNodes)
        return;
    struct SyncGuard
    {
        bool& Flag;
        explicit SyncGuard(bool& flag) : Flag(flag) { Flag = true; }
        ~SyncGuard() { Flag = false; }
    } syncGuard(m_SyncingModelNodes);

    std::unordered_set<std::string> liveIds;
    bool anyChanged = false;
    if (m_Model)
    {
        for (const Graph::Node& node : m_Model->Nodes)
        {
            if (node.Id.empty() || m_PendingHiddenNodeIds.count(node.Id) != 0)
                continue;
            liveIds.insert(node.Id);
            if (BindLiveNode(node))
                anyChanged = true;
        }
    }

    for (size_t i = 0; i < m_NodePool->SlotCount(); ++i)
    {
        GraphPortedNode* slot = m_NodePool->SlotAt(i);
        if (!slot)
            continue;
        const std::string& id = slot->GetModelNodeId();
        if (id.empty() || liveIds.count(id) != 0)
            continue;
        if (id.rfind("spike-", 0) == 0)
            continue;
        m_NodePool->Recycle(slot);
    }

    if (anyChanged)
        MarkDirty(VisualDirty);
    ApplyNodeDropShadows();
    RaiseSelectedNodes();
    UpdateCompatiblePortHighlights();
}

void GraphCanvas::ApplyNodeVisualStates()
{
    if (!m_NodePool)
        return;

    for (size_t i = 0; i < m_NodePool->SlotCount(); ++i)
    {
        GraphPortedNode* slot = m_NodePool->SlotAt(i);
        if (!slot)
            continue;
        const std::string& id = slot->GetModelNodeId();
        if (id.empty() || id.rfind("spike-", 0) == 0)
            continue;
        bool searchMatch = false;
        if (m_Model)
        {
            if (const Graph::Node* node = m_Model->FindNode(id))
                searchMatch = NodeMatchesSearch(*node);
        }
        GraphNodeVisualState visual;
        visual.Selected = IsNodeSelected(id);
        visual.SearchMatch = searchMatch;
        visual.RuntimeActive = m_RuntimeActiveNodeIds.count(id) != 0;
        visual.HasError = m_ErrorNodeIds.count(id) != 0;
        slot->ApplyVisualState(visual);
    }
    RaiseSelectedNodes();
}

void GraphCanvas::RaiseSelectedNodes()
{
    if (!m_NodeLayer)
        return;
    auto& children = m_NodeLayer->GetMutableChildren();
    if (children.size() < 2)
        return;

    bool seenSelected = false;
    bool needsMove = false;
    for (const auto& child : children)
    {
        auto* node = dynamic_cast<GraphPortedNode*>(child.get());
        const bool isSelected = node && node->HasClass("selected");
        if (isSelected)
            seenSelected = true;
        else if (seenSelected)
        {
            needsMove = true;
            break;
        }
    }
    if (!needsMove)
        return;

    std::vector<std::unique_ptr<UIElement>> unselected;
    std::vector<std::unique_ptr<UIElement>> selected;
    unselected.reserve(children.size());
    selected.reserve(children.size());
    for (auto& child : children)
    {
        auto* node = dynamic_cast<GraphPortedNode*>(child.get());
        if (node && node->HasClass("selected"))
            selected.push_back(std::move(child));
        else
            unselected.push_back(std::move(child));
    }
    children.clear();
    for (auto& child : unselected)
        children.push_back(std::move(child));
    for (auto& child : selected)
        children.push_back(std::move(child));
    m_NodeLayer->MarkDirty(ChildrenDirty | VisualDirty);
}

void GraphCanvas::NotifyGraphChanged()
{
    RebuildNodeWidgets();
    if (m_OnGraphChanged)
        m_OnGraphChanged();
}

void GraphCanvas::RebindNodes(const std::unordered_set<std::string>& nodeIds)
{
    if (!m_Model || nodeIds.empty())
        return;
    bool anyChanged = false;
    for (const std::string& id : nodeIds)
    {
        const Graph::Node* node = m_Model->FindNode(id);
        if (node && BindLiveNode(*node))
            anyChanged = true;
    }
    if (anyChanged)
        MarkDirty(VisualDirty);
}

void GraphCanvas::RebindVisibleNodes()
{
    if (!m_Model || !m_NodePool)
        return;
    bool anyChanged = false;
    for (const Graph::Node& node : m_Model->Nodes)
    {
        /* Bound slots only: BindLiveNode acquires one when a node has none, so
           walking the whole model here would materialize an offscreen graph. */
        GraphPortedNode* slot = m_NodePool->FindByModelId(node.Id);
        if (!slot)
            continue;
        if (BindLiveNode(node))
            anyChanged = true;
        /* The colours reach the node through custom properties, which only take
           effect when the cascade re-runs — the same reason ApplyAccentColorToUI
           follows its stylesheet swap with MarkStyleDirtyAll. This is that,
           scoped to the node instead of the whole UI. */
        slot->MarkDirty(UIElement::StyleDirty | UIElement::VisualDirty);
    }
    if (anyChanged)
        MarkDirty(VisualDirty);
}

void GraphCanvas::UpdateCompatiblePortHighlights()
{
    if (!m_NodePool)
        return;

    const std::optional<ConnectionDrag> attempt = PendingConnection();
    if (attempt)
        AddClass("connecting");
    else
        RemoveClass("connecting");

    for (size_t i = 0; i < m_NodePool->SlotCount(); ++i)
    {
        GraphPortedNode* slot = m_NodePool->SlotAt(i);
        if (!slot)
            continue;

        const Graph::Node* node = nullptr;
        if (attempt && !slot->GetModelNodeId().empty())
            node = m_Model->FindNode(slot->GetModelNodeId());

        const size_t inputCount = slot->InputPortCount();
        const size_t outputCount = slot->OutputPortCount();
        if (!node)
        {
            for (size_t p = 0; p < inputCount; ++p)
            {
                if (GraphPort* port = slot->InputPortAt(p))
                    port->RemoveClass("valid-target");
            }
            for (size_t p = 0; p < outputCount; ++p)
            {
                if (GraphPort* port = slot->OutputPortAt(p))
                    port->RemoveClass("valid-target");
            }
            continue;
        }

        size_t inputIndex = 0;
        size_t outputIndex = 0;
        for (const Graph::Port& port : node->Ports)
        {
            GraphPort* widget = port.Direction == Graph::PortDirection::In
                ? slot->InputPortAt(inputIndex++)
                : slot->OutputPortAt(outputIndex++);
            if (!widget)
                continue;
            const bool valid = attempt && attempt->Accepts(*node, port);
            if (valid)
                widget->AddClass("valid-target");
            else
                widget->RemoveClass("valid-target");
        }
        for (; inputIndex < inputCount; ++inputIndex)
        {
            if (GraphPort* port = slot->InputPortAt(inputIndex))
                port->RemoveClass("valid-target");
        }
        for (; outputIndex < outputCount; ++outputIndex)
        {
            if (GraphPort* port = slot->OutputPortAt(outputIndex))
                port->RemoveClass("valid-target");
        }
    }
}

void GraphCanvas::HideSelectedNodeWidgets()
{
    if (!m_NodePool)
        return;
    bool hid = false;
    for (const std::string& id : m_SelectedNodeIds)
    {
        if (GraphPortedNode* slot = m_NodePool->FindByModelId(id))
        {
            m_NodePool->Recycle(slot);
            hid = true;
        }
    }
    if (hid)
        MarkDirty(VisualDirty);
}

void GraphCanvas::DeleteSelectedNodes()
{
    if (!m_Model || m_SelectedNodeIds.empty())
        return;

    const std::unordered_set<std::string> toRemove = m_SelectedNodeIds;
    m_PendingHiddenNodeIds.insert(toRemove.begin(), toRemove.end());
    HideSelectedNodeWidgets();
    std::unordered_set<std::string> neighbors;
    for (const Graph::Edge& link : m_Model->Links)
    {
        if (toRemove.count(link.SourceNodeId) != 0 && toRemove.count(link.TargetNodeId) == 0)
            neighbors.insert(link.TargetNodeId);
        else if (toRemove.count(link.TargetNodeId) != 0 && toRemove.count(link.SourceNodeId) == 0)
            neighbors.insert(link.SourceNodeId);
    }
    MarkDirty(VisualDirty);

    DeferModelMutation([this, toRemove, neighbors = std::move(neighbors)]()
    {
        auto doDelete = [this, toRemove, neighbors]()
        {
            auto& nodes = m_Model->Nodes;
            auto& links = m_Model->Links;
            nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                [&toRemove](const Graph::Node& n) { return toRemove.count(n.Id) != 0; }), nodes.end());
            links.erase(std::remove_if(links.begin(), links.end(),
                [&toRemove](const Graph::Edge& l) {
                    return toRemove.count(l.SourceNodeId) != 0 || toRemove.count(l.TargetNodeId) != 0;
                }), links.end());
            for (const std::string& id : toRemove)
                m_PendingHiddenNodeIds.erase(id);
            m_SelectedNodeIds.clear();
            m_DragNodeStarts.clear();
            if (!m_SelectedLinkId.empty())
            {
                m_SelectedLinkId.clear();
                if (m_OnLinkSelected)
                    m_OnLinkSelected({});
            }
            RebindNodes(neighbors);
            MarkDirty(VisualDirty);
            NotifyGraphChanged();
            if (m_OnSelectionChanged)
                m_OnSelectionChanged(std::string());
        };
        if (m_UndoScope)
            m_UndoScope("Delete Nodes", doDelete);
        else
            doDelete();
    });
}

void GraphCanvas::DeleteSelectedLink()
{
    if (!m_Model || m_SelectedLinkId.empty())
        return;
    HideAndDeferDeleteLink(m_SelectedLinkId, "Delete Link");
}

void GraphCanvas::HideAndDeferDeleteLink(const std::string& linkId, const char* undoName)
{
    if (!m_Model || linkId.empty())
        return;
    m_PendingHiddenLinkIds.insert(linkId);
    MarkDirty(VisualDirty);
    DeferModelMutation([this, linkId, undoName]()
    {
        auto removeLink = [this, linkId]()
        {
            auto& links = m_Model->Links;
            links.erase(std::remove_if(links.begin(), links.end(),
                [&linkId](const Graph::Edge& l) { return l.Id == linkId; }), links.end());
            m_PendingHiddenLinkIds.erase(linkId);
            if (m_SelectedLinkId == linkId)
            {
                m_SelectedLinkId.clear();
                if (m_OnLinkSelected)
                    m_OnLinkSelected({});
            }
            NotifyGraphChanged();
        };
        if (m_UndoScope)
            m_UndoScope(undoName, removeLink);
        else
            removeLink();
    });
}

void GraphCanvas::DeferModelMutation(std::function<void()> action)
{
    m_DeferredModelMutations.push_back(std::move(action));
    m_DeferredModelMutationSkipOnce = true;
}

void GraphCanvas::FlushDeferredModelMutation()
{
    if (m_DeferredModelMutations.empty())
        return;
    if (m_DeferredModelMutationSkipOnce)
    {
        /* Give the hide one painted frame before the model changes under it. */
        m_DeferredModelMutationSkipOnce = false;
        return;
    }
    std::vector<std::function<void()>> actions;
    actions.swap(m_DeferredModelMutations);
    for (auto& action : actions)
    {
        if (m_Model && action)
            action();
    }
}

void GraphCanvas::SetRuntimeActiveNodes(const std::unordered_set<std::string>& nodeIds)
{
    m_RuntimeActiveNodeIds = nodeIds;
    ApplyNodeVisualStates();
    MarkDirty(VisualDirty);
}

void GraphCanvas::SetErrorNodes(const std::unordered_set<std::string>& nodeIds)
{
    if (m_ErrorNodeIds == nodeIds)
        return;
    m_ErrorNodeIds = nodeIds;
    ApplyNodeVisualStates();
    MarkDirty(VisualDirty);
}

void GraphCanvas::AddRuntimeTransitionPulses(const std::vector<std::string>& linkIds)
{
    bool added = false;
    for (const std::string& linkId : linkIds)
    {
        if (linkId.empty())
            continue;
        if (m_RuntimeTransitionPulses.find(linkId) != m_RuntimeTransitionPulses.end())
            continue;
        RuntimeTransitionPulse& pulse = m_RuntimeTransitionPulses[linkId];
        pulse.Phase = 0.0f;
        pulse.Strength = 1.0f;
        added = true;
    }
    if (added)
        MarkDirty(VisualDirty);
}

void GraphCanvas::TickRuntimeTransitionAnimation(float deltaSeconds)
{
    if (m_RuntimeTransitionPulses.empty())
        return;

    const float dt = std::max(0.0f, deltaSeconds);
    for (auto it = m_RuntimeTransitionPulses.begin(); it != m_RuntimeTransitionPulses.end();)
    {
        it->second.Phase = std::fmod(it->second.Phase + dt * 1.35f, 1.0f);
        it->second.Strength -= dt * 0.85f;
        if (it->second.Strength <= 0.0f)
            it = m_RuntimeTransitionPulses.erase(it);
        else
            ++it;
    }
    MarkDirty(VisualDirty);
}

void GraphCanvas::ClearRuntimeVisualization()
{
    m_RuntimeActiveNodeIds.clear();
    m_RuntimeTransitionPulses.clear();
    ApplyNodeVisualStates();
    MarkDirty(VisualDirty);
}

std::string GraphCanvas::GetSelectedNodeId() const
{
    if (m_SelectedNodeIds.empty())
        return {};
    return *m_SelectedNodeIds.begin();
}

void GraphCanvas::SetSelectedNodeId(const std::string& id)
{
    const bool clearedLink = !m_SelectedLinkId.empty();
    m_SelectedNodeIds.clear();
    if (!id.empty())
        m_SelectedNodeIds.insert(id);
    if (clearedLink)
        m_SelectedLinkId.clear();
    ApplyNodeVisualStates();
    MarkDirty(VisualDirty);
    if (clearedLink && m_OnLinkSelected)
        m_OnLinkSelected({});
}

void GraphCanvas::SetSelectedNodeIds(const std::unordered_set<std::string>& ids)
{
    const bool clearedLink = !ids.empty() && !m_SelectedLinkId.empty();
    m_SelectedNodeIds = ids;
    if (clearedLink)
        m_SelectedLinkId.clear();
    ApplyNodeVisualStates();
    MarkDirty(VisualDirty);
    if (clearedLink && m_OnLinkSelected)
        m_OnLinkSelected({});
}

void GraphCanvas::SetSelectedLinkId(const std::string& id)
{
    if (m_SelectedLinkId == id)
        return;
    m_SelectedLinkId = id;
    if (!id.empty())
        m_SelectedNodeIds.clear();
    ApplyNodeVisualStates();
    MarkDirty(VisualDirty);
    if (m_OnLinkSelected)
        m_OnLinkSelected(m_SelectedLinkId);
}

void GraphCanvas::ReconcileStateWithModel()
{
    if (!m_Model)
    {
        m_SelectedNodeIds.clear();
        m_DragNodeStarts.clear();
        m_PendingNodeDragId.clear();
        m_PendingLinkRemoveId.clear();
        m_RightMouseDragged = false;
        m_HoveredLinkId.clear();
        if (!m_SelectedLinkId.empty())
        {
            m_SelectedLinkId.clear();
            if (m_OnLinkSelected)
                m_OnLinkSelected(std::string());
        }
        m_HoveredNodeId.clear();
        m_HoveredPortId.clear();
        SetTooltip({});
        m_RuntimeActiveNodeIds.clear();
        m_RuntimeTransitionPulses.clear();
        m_ConnectionSourceNodeId.clear();
        m_ConnectionSourcePortId.clear();
        m_ConnectionTargetNodeId.clear();
        m_ConnectionTargetPortId.clear();
        m_DraggingLinkToExistingInput = false;
        m_ReroutingLinkId.clear();
        m_LastPress = {};
        m_PendingNodeDoubleClick = false;
        m_DraggingNode = false;
        m_DraggingLink = false;
        RebuildNodeWidgets();
        MarkDirty(VisualDirty);
        return;
    }
    std::unordered_set<std::string> validNodeIds;
    for (const auto& n : m_Model->Nodes)
        validNodeIds.insert(n.Id);
    if (!m_PendingNodeDragId.empty() && validNodeIds.count(m_PendingNodeDragId) == 0)
    {
        m_PendingNodeDragId.clear();
        m_PendingNodeDoubleClick = false;
    }
    std::unordered_set<std::string> validLinkIds;
    for (const auto& l : m_Model->Links)
        validLinkIds.insert(l.Id);

    for (auto it = m_SelectedNodeIds.begin(); it != m_SelectedNodeIds.end();)
    {
        if (validNodeIds.count(*it) == 0)
            it = m_SelectedNodeIds.erase(it);
        else
            ++it;
    }
    for (auto it = m_DragNodeStarts.begin(); it != m_DragNodeStarts.end();)
    {
        if (validNodeIds.count(it->first) == 0)
            it = m_DragNodeStarts.erase(it);
        else
            ++it;
    }
    for (auto it = m_RuntimeActiveNodeIds.begin(); it != m_RuntimeActiveNodeIds.end();)
    {
        if (validNodeIds.count(*it) == 0)
            it = m_RuntimeActiveNodeIds.erase(it);
        else
            ++it;
    }
    if (!m_HoveredLinkId.empty() && validLinkIds.count(m_HoveredLinkId) == 0)
        m_HoveredLinkId.clear();
    if (!m_SelectedLinkId.empty() && validLinkIds.count(m_SelectedLinkId) == 0)
        SetSelectedLinkId({});
    for (auto it = m_RuntimeTransitionPulses.begin(); it != m_RuntimeTransitionPulses.end();)
    {
        if (validLinkIds.count(it->first) == 0)
            it = m_RuntimeTransitionPulses.erase(it);
        else
            ++it;
    }
    if (!m_HoveredNodeId.empty() && validNodeIds.count(m_HoveredNodeId) == 0)
    {
        m_HoveredNodeId.clear();
        m_HoveredPortId.clear();
    }
    if (!m_ConnectionSourceNodeId.empty() && validNodeIds.count(m_ConnectionSourceNodeId) == 0)
    {
        m_ConnectionSourceNodeId.clear();
        m_ConnectionSourcePortId.clear();
        m_ConnectionTargetNodeId.clear();
        m_ConnectionTargetPortId.clear();
        m_DraggingLinkToExistingInput = false;
        m_ReroutingLinkId.clear();
        m_DraggingLink = false;
    }
    if (!m_ConnectionTargetNodeId.empty() && validNodeIds.count(m_ConnectionTargetNodeId) == 0)
    {
        m_ConnectionSourceNodeId.clear();
        m_ConnectionSourcePortId.clear();
        m_ConnectionTargetNodeId.clear();
        m_ConnectionTargetPortId.clear();
        m_DraggingLinkToExistingInput = false;
        m_ReroutingLinkId.clear();
        m_DraggingLink = false;
    }
    if (!m_ReroutingLinkId.empty() && validLinkIds.count(m_ReroutingLinkId) == 0)
        m_ReroutingLinkId.clear();
    if (m_DraggingNode)
    {
        bool anyLeft = false;
        for (const auto& kv : m_DragNodeStarts)
        {
            if (validNodeIds.count(kv.first) != 0)
            {
                anyLeft = true;
                break;
            }
        }
        if (!anyLeft)
        {
            m_DraggingNode = false;
            m_DragNodeStarts.clear();
        }
    }
    RebuildNodeWidgets();
    MarkDirty(VisualDirty);
}

bool GraphCanvas::WouldNodeOverlapAny(const Graph::Node& node, float x, float y,
                                            const std::unordered_set<std::string>& ignoreNodeIds) const
{
    if (!m_Model)
        return false;
    const auto getWidth = [](const Graph::Node& n) { return GetNodeWidth(n); };
    const auto getHeight = [this](const Graph::Node& n) { return GetNodeRectHeight(n); };
    return GraphNodeOverlap::WouldOverlapAny(*m_Model, node, Mathematics::Vector2(x, y),
                                             ignoreNodeIds, getWidth, getHeight);
}

void GraphCanvas::FindNearestNonOverlappingPosition(const Graph::Node& node, float desiredX, float desiredY,
                                                          const std::unordered_set<std::string>& ignoreNodeIds,
                                                          float& outX, float& outY) const
{
    if (!m_Model)
    {
        outX = desiredX;
        outY = desiredY;
        return;
    }
    const auto getWidth = [](const Graph::Node& n) { return GetNodeWidth(n); };
    const auto getHeight = [this](const Graph::Node& n) { return GetNodeRectHeight(n); };
    Mathematics::Vector2 out(desiredX, desiredY);
    GraphNodeOverlap::FindNearestNonOverlappingPosition(
        *m_Model, node, Mathematics::Vector2(desiredX, desiredY), ignoreNodeIds, out, getWidth,
        getHeight);
    outX = out.x;
    outY = out.y;
}

void GraphCanvas::ResolveNodeOverlaps(const std::vector<std::string>& nodeIds)
{
    if (!m_Model)
        return;
    const auto getWidth = [](const Graph::Node& n) { return GetNodeWidth(n); };
    const auto getHeight = [this](const Graph::Node& n) { return GetNodeRectHeight(n); };
    GraphNodeOverlap::ResolveNodeOverlaps(*m_Model, nodeIds, getWidth, getHeight);
}


void GraphCanvas::SnapPanToDevicePixels()
{
    /* The pan accumulates fractional mouse deltas, but glyph rasterization
       snaps to device pixels — under a fractional pan every label re-renders
       at a new subpixel phase and the graph text visibly shimmers while
       panning. Keeping the pan on the device-pixel grid moves the whole
       canvas by whole pixels instead, so panning is rasterization-invariant. */
    const UIManager* ui = GetOwnerManager();
    const float cs = std::max(0.01f, ui ? ui->GetContentScale() : 1.f);
    m_PanX = std::round(m_PanX * cs) / cs;
    m_PanY = std::round(m_PanY * cs) / cs;
}
void GraphCanvas::SetPanZoom(float panX, float panY, float zoom)
{
    const float clampedZoom = std::max(GraphCanvas::kMinZoom, std::min(GraphCanvas::kMaxZoom, zoom));
    if (panX == m_PanX && panY == m_PanY && clampedZoom == m_Zoom)
        return;
    HideTooltip(this);
    m_PanX = panX;
    m_PanY = panY;
    m_Zoom = clampedZoom;
    SnapPanToDevicePixels();
    if (m_Model)
    {
        m_Model->Viewport.PanX = m_PanX;
        m_Model->Viewport.PanY = m_PanY;
        m_Model->Viewport.Zoom = m_Zoom;
    }
    SyncNodeLayerTransform();
    MarkDirty(VisualDirty);
}

void GraphCanvas::GetPanZoom(float& panX, float& panY, float& zoom) const
{
    panX = m_PanX;
    panY = m_PanY;
    zoom = m_Zoom;
}

void GraphCanvas::FrameNodesToFit()
{
    if (!m_Model || m_Model->Nodes.empty())
        return;
    float visibleW = GetLayoutWidth();
    float visibleH = GetLayoutHeight();
    constexpr float kMinVisible = 100.f;
    if (visibleW < kMinVisible || visibleH < kMinVisible)
        return;

    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (const auto& node : m_Model->Nodes)
    {
        minX = std::min(minX, node.PositionX);
        minY = std::min(minY, node.PositionY);
        maxX = std::max(maxX, node.PositionX + GetNodeWidth(node));
        maxY = std::max(maxY, node.PositionY + GetNodeRectHeight(node));
    }
    constexpr float kPadding = 48.f;
    float boundsW = (maxX - minX) + kPadding * 2.f;
    float boundsH = (maxY - minY) + kPadding * 2.f;
    if (boundsW < 1.f)
        boundsW = 1.f;
    if (boundsH < 1.f)
        boundsH = 1.f;

    float zoom = std::min(visibleW / boundsW, visibleH / boundsH);
    zoom = std::max(GraphCanvas::kMinZoom, std::min(1.f, zoom));

    float centerX = (minX + maxX) * 0.5f;
    float centerY = (minY + maxY) * 0.5f;
    float panX = visibleW * 0.5f - centerX * zoom;
    float panY = visibleH * 0.5f - centerY * zoom;

    SetPanZoom(panX, panY, zoom);
}

void GraphCanvas::ScreenToGraph(float screenX, float screenY, float canvasX, float canvasY,
                                       float /*canvasW*/, float /*canvasH*/,
                                       float& outGraphX, float& outGraphY) const
{
    float localX = screenX - canvasX;
    float localY = screenY - canvasY;
    outGraphX = (localX - m_PanX) / m_Zoom;
    outGraphY = (localY - m_PanY) / m_Zoom;
}

void GraphCanvas::GraphToScreen(float graphX, float graphY, float canvasX, float canvasY,
                                      float& outScreenX, float& outScreenY) const
{
    outScreenX = canvasX + m_PanX + graphX * m_Zoom;
    outScreenY = canvasY + m_PanY + graphY * m_Zoom;
}

namespace {

void ComputeFormulaPortCenter(const Graph::Node& node, size_t portIndex, float& outX, float& outY,
                            float nodeHeightGraph)
{
    if (portIndex >= node.Ports.size())
    {
        outX = node.PositionX;
        outY = node.PositionY + nodeHeightGraph * 0.5f;
        return;
    }
    bool isOutput = node.Ports[portIndex].Direction == Graph::PortDirection::Out;
    int sameDirIndex = 0;
    {
        int scan = 0;
        for (size_t i = 0; i < node.Ports.size(); ++i)
        {
            if ((node.Ports[i].Direction == Graph::PortDirection::Out) != isOutput)
                continue;
            if (i == portIndex)
                sameDirIndex = scan;
            ++scan;
        }
    }
    float nx = node.PositionX;
    float ny = node.PositionY;
    float nw = GetNodeWidth(node);
    const float portAreaTop = ny + GraphCanvas::kNodeTitleRowHeightGraph + GraphCanvas::kNodePortAreaTopPaddingGraph;
    /* Same top-anchored lattice as GraphNodeMetrics::PortTopPercent. */
    const float portY =
        portAreaTop + GraphCanvas::kPortSpacing * static_cast<float>(sameDirIndex);
    outX = isOutput ? (nx + nw) : nx;
    outY = portY;
}

} // namespace

void GraphCanvas::GetPortCenterInGraph(const Graph::Node& node, size_t portIndex, float& outX, float& outY) const
{
    if (m_NodePool && portIndex < node.Ports.size() && m_Zoom != 0.0f)
    {
        GraphPortedNode* ported = m_NodePool->FindByModelId(node.Id);
        if (ported && ported->m_YogaState && ported->m_YogaState->LastCommitGen != 0)
        {
            GraphPort* port = ported->FindPort(node.Ports[portIndex].Id);
            if (port && port->GetLayoutWidth() > 0.0f && port->GetLayoutHeight() > 0.0f)
            {
                const float canvasX = GetLayoutX();
                const float canvasY = GetLayoutY();
                const float layoutCenterX = port->GetLayoutX() + port->GetLayoutWidth() * 0.5f;
                const float layoutCenterY = port->GetLayoutY() + port->GetLayoutHeight() * 0.5f;
                outX = (layoutCenterX - canvasX - m_PanX) / m_Zoom;
                outY = (layoutCenterY - canvasY - m_PanY) / m_Zoom;
                return;
            }
        }
    }
    ComputeFormulaPortCenter(node, portIndex, outX, outY, GetNodeRectHeight(node));
}

bool GraphCanvas::HitTestNodeOrPort(float graphX, float graphY, const Graph::Node& node,
                                         std::string& outPortId, bool& outIsOutput) const
{
    float nx = node.PositionX;
    float ny = node.PositionY;
    float nw = GetNodeWidth(node);
    float nh = GetNodeRectHeight(node);

    /* Test ports first (port circles extend outside the node rect, so allow hit outside node bounds) */
    size_t portIdx = 0;
    for (const auto& port : node.Ports)
    {
        float px, py;
        GetPortCenterInGraph(node, portIdx, px, py);
        float dx = graphX - px;
        float dy = graphY - py;
        if (dx * dx + dy * dy <= kPortHitRadius * kPortHitRadius)
        {
            outPortId = port.Id;
            outIsOutput = (port.Direction == Graph::PortDirection::Out);
            return true;
        }
        portIdx++;
    }

    /* No port hit; only count as node body hit if inside the node rect */
    if (graphX < nx || graphX > nx + nw || graphY < ny || graphY > ny + nh)
        return false;
    outPortId.clear();
    outIsOutput = false;
    return true;
}

void GraphCanvas::FindHit(float graphX, float graphY, std::string& outNodeId,
                                std::string& outPortId, bool& outIsOutput) const
{
    outNodeId.clear();
    outPortId.clear();
    outIsOutput = false;
    if (!m_Model)
        return;
    for (int i = static_cast<int>(m_Model->Nodes.size()) - 1; i >= 0; --i)
    {
        const auto& node = m_Model->Nodes[i];
        std::string portId;
        bool isOut;
        if (HitTestNodeOrPort(graphX, graphY, node, portId, isOut))
        {
            outNodeId = node.Id;
            outPortId = portId;
            outIsOutput = isOut;
            return;
        }
    }
}

const Graph::Port* GraphCanvas::FindPort(const Graph::Node& node, const std::string& portId)
{
    const size_t index = FindPortIndex(node, portId);
    return index < node.Ports.size() ? &node.Ports[index] : nullptr;
}

std::string GraphCanvas::BuildLinkTooltip(const Graph::Edge& link) const
{
    (void)link;
    return "Right click to delete";
}

/* The drag as a thing that can be asked a question, rather than a spread of
   member flags each caller has to reassemble. */
bool GraphCanvas::ConnectionDrag::Accepts(const Graph::Node& node, const Graph::Port& port) const
{
    if (!Model)
        return false;

    /* The loose end seeks the opposite kind of port to the one being held. */
    const Graph::PortDirection wanted =
        HeldOnInput ? Graph::PortDirection::Out : Graph::PortDirection::In;
    if (port.Direction != wanted)
        return false;

    const Graph::Node* heldNode = Model->FindNode(HeldNodeId);
    if (!heldNode)
        return false;
    const Graph::Port* heldPort = GraphCanvas::FindPort(*heldNode, HeldPortId);
    if (!heldPort || heldPort->Direction == wanted)
        return false;

    const Graph::Port& source = HeldOnInput ? port : *heldPort;
    const Graph::Port& target = HeldOnInput ? *heldPort : port;
    if (!ArePortTypesCompatible(source.DataType, target.DataType))
        return false;

    /* One wire per input: whichever end lands on the input, that input must be
       free. */
    const std::string& targetNodeId = HeldOnInput ? HeldNodeId : node.Id;
    const std::string& targetPortId = HeldOnInput ? HeldPortId : port.Id;
    for (const Graph::Edge& link : Model->Links)
    {
        if (Ignores && Ignores(link))
            continue;
        if (link.TargetNodeId == targetNodeId && link.TargetPortId == targetPortId)
            return false;
    }
    return true;
}

std::optional<GraphCanvas::ConnectionDrag> GraphCanvas::PendingConnection() const
{
    if (!m_DraggingLink || !m_Model)
        return std::nullopt;

    ConnectionDrag attempt;
    attempt.Model = m_Model;
    attempt.HeldOnInput = m_DraggingLinkToExistingInput;
    attempt.HeldNodeId = m_DraggingLinkToExistingInput ? m_ConnectionTargetNodeId
                                                       : m_ConnectionSourceNodeId;
    attempt.HeldPortId = m_DraggingLinkToExistingInput ? m_ConnectionTargetPortId
                                                       : m_ConnectionSourcePortId;
    attempt.Ignores = [this](const Graph::Edge& link)
    {
        return IsLinkHiddenPendingDelete(link) ||
               (!m_ReroutingLinkId.empty() && link.Id == m_ReroutingLinkId);
    };
    return attempt;
}



void GraphCanvas::ComputeConnectionTracks(std::vector<float> segSx0, std::vector<float> segSy0,
                                                std::vector<float> segSx1, std::vector<float> segSy1,
                                                float canvasX, float canvasY,
                                                std::vector<int>& outTrack, std::vector<int>& outNTracks) const
{
    const size_t numLinks = segSx0.size();
    outTrack.assign(numLinks, 0);
    outNTracks.assign(numLinks, 1);
    if (numLinks == 0 || !m_Model)
        return;

    /* Same graph-frame discipline as ComputeStraightRoute: cluster decisions
       must be a pure function of the graph, or panning's float noise and
       zooming's rescaled distances re-assign tracks and every affected wire
       visibly jumps. Convert once to snapped graph units; the thresholds
       below then read as graph units (their zoom-1 px values). Degenerate
       all-zero rows are the unrouted sentinel — leave them untouched. */
    {
        const float originX = canvasX + m_PanX;
        const float originY = canvasY + m_PanY;
        const float invZoom = 1.f / m_Zoom;
        auto toGraph = [&](float v, float origin)
        { return std::round((v - origin) * invZoom * 16.f) * (1.f / 16.f); };
        for (size_t i = 0; i < numLinks; ++i)
        {
            if (segSx0[i] == 0.f && segSy0[i] == 0.f && segSx1[i] == 0.f && segSy1[i] == 0.f)
                continue;
            segSx0[i] = toGraph(segSx0[i], originX);
            segSy0[i] = toGraph(segSy0[i], originY);
            segSx1[i] = toGraph(segSx1[i], originX);
            segSy1[i] = toGraph(segSy1[i], originY);
        }
    }

    constexpr float kOverlapDistThreshold = 14.f;
    constexpr float kParallelDotThreshold = 0.92f;

    std::vector<int> parent(numLinks);
    for (size_t i = 0; i < numLinks; ++i)
        parent[i] = static_cast<int>(i);
    auto find = [&parent](int i) -> int {
        while (parent[static_cast<size_t>(i)] != i)
            i = parent[static_cast<size_t>(i)];
        return i;
    };
    auto unite = [&parent, &find](int a, int b) {
        a = find(a);
        b = find(b);
        if (a != b)
            parent[static_cast<size_t>(b)] = a;
    };

    auto isDegenerate = [&](size_t i) {
        return segSx0[i] == 0.f && segSy0[i] == 0.f && segSx1[i] == 0.f && segSy1[i] == 0.f;
    };

    auto ptSegDist = [](float px, float py, float ax, float ay, float bx, float by) -> float {
        float dx = bx - ax, dy = by - ay;
        float len2 = dx * dx + dy * dy;
        if (len2 < 1e-10f)
            return std::sqrt((px - ax) * (px - ax) + (py - ay) * (py - ay));
        float t = std::max(0.f, std::min(1.f, ((px - ax) * dx + (py - ay) * dy) / len2));
        float qx = ax + t * dx, qy = ay + t * dy;
        return std::sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy));
    };

    /* Enter column per link: wires land at the target's left edge, so targets
       stacked at the same X share one column even when the links are neither
       parallel nor bound for the same node. Precomputed — the pair loop below
       would otherwise be a model lookup per pair. */
    std::vector<float> tgtLeftGraph(numLinks, 0.f);
    for (size_t i = 0; i < numLinks; ++i)
    {
        if (isDegenerate(i))
            continue;
        if (const Graph::Node* tgt = m_Model->FindNode(m_Model->Links[i].TargetNodeId))
            tgtLeftGraph[i] = tgt->PositionX;
    }
    constexpr float kEnterColumnThreshold = 10.f;

    for (size_t i = 0; i < numLinks; ++i)
    {
        if (isDegenerate(i))
            continue;
        float dax = segSx1[i] - segSx0[i], day = segSy1[i] - segSy0[i];
        float la = std::sqrt(dax * dax + day * day);
        if (la < 1e-6f)
            continue;
        float uax = dax / la, uay = day / la;
        float maX = (segSx0[i] + segSx1[i]) * 0.5f;
        float maY = (segSy0[i] + segSy1[i]) * 0.5f;
        for (size_t j = i + 1; j < numLinks; ++j)
        {
            if (isDegenerate(j))
                continue;
            // Links feeding the same target node share a riser column on the
            // input side, so always fan them apart even when not parallel.
            if (m_Model->Links[i].TargetNodeId == m_Model->Links[j].TargetNodeId)
            {
                unite(static_cast<int>(i), static_cast<int>(j));
                continue;
            }
            /* Different targets, same left edge: the two wires would run down
               one column and box around each other on the way in. */
            if (std::abs(tgtLeftGraph[i] - tgtLeftGraph[j]) <= kEnterColumnThreshold)
            {
                const float yMinI = std::min(segSy0[i], segSy1[i]);
                const float yMaxI = std::max(segSy0[i], segSy1[i]);
                const float yMinJ = std::min(segSy0[j], segSy1[j]);
                const float yMaxJ = std::max(segSy0[j], segSy1[j]);
                if (yMaxI >= yMinJ - kEnterColumnThreshold &&
                    yMaxJ >= yMinI - kEnterColumnThreshold)
                {
                    unite(static_cast<int>(i), static_cast<int>(j));
                    continue;
                }
            }
            float dbx = segSx1[j] - segSx0[j], dby = segSy1[j] - segSy0[j];
            float lb = std::sqrt(dbx * dbx + dby * dby);
            if (lb < 1e-6f)
                continue;
            float dot = uax * (dbx / lb) + uay * (dby / lb);
            if (dot < kParallelDotThreshold && dot > -kParallelDotThreshold)
                continue;
            float mbX = (segSx0[j] + segSx1[j]) * 0.5f;
            float mbY = (segSy0[j] + segSy1[j]) * 0.5f;
            float d1 = ptSegDist(maX, maY, segSx0[j], segSy0[j], segSx1[j], segSy1[j]);
            float d2 = ptSegDist(mbX, mbY, segSx0[i], segSy0[i], segSx1[i], segSy1[i]);
            if (d1 < kOverlapDistThreshold && d2 < kOverlapDistThreshold)
                unite(static_cast<int>(i), static_cast<int>(j));
        }
    }

    std::vector<std::vector<size_t>> comps(numLinks);
    for (size_t i = 0; i < numLinks; ++i)
    {
        if (isDegenerate(i))
            continue;
        comps[static_cast<size_t>(find(static_cast<int>(i)))].push_back(i);
    }
    for (auto& comp : comps)
    {
        if (comp.size() <= 1)
            continue;
        // Order by target-side vertical position so adjacent input ports get
        // adjacent risers, then stagger each link by its index in the group.
        std::sort(comp.begin(), comp.end(), [&](size_t a, size_t b) {
            if (segSy1[a] != segSy1[b])
                return segSy1[a] < segSy1[b];
            return segSy0[a] < segSy0[b];
        });
        int groupSize = static_cast<int>(comp.size());
        for (size_t k = 0; k < comp.size(); ++k)
        {
            outTrack[comp[k]] = static_cast<int>(k);
            outNTracks[comp[k]] = groupSize;
        }
    }

    // Fan apart lone wires that would otherwise share the same default riser column.
    struct RiserSlot
    {
        size_t LinkIdx = 0;
        float RiserX = 0.f;
        float YMin = 0.f;
        float YMax = 0.f;
    };
    std::vector<RiserSlot> loneLinks;
    loneLinks.reserve(numLinks);
    for (size_t i = 0; i < numLinks; ++i)
    {
        if (isDegenerate(i) || outNTracks[i] > 1)
            continue;
        loneLinks.push_back({
            i,
            (segSx0[i] + segSx1[i]) * 0.5f,
            std::min(segSy0[i], segSy1[i]),
            std::max(segSy0[i], segSy1[i]),
        });
    }

    if (loneLinks.size() > 1)
    {
        std::vector<int> loneParent(loneLinks.size());
        for (size_t i = 0; i < loneLinks.size(); ++i)
            loneParent[i] = static_cast<int>(i);
        auto loneFind = [&loneParent](int i) -> int {
            while (loneParent[static_cast<size_t>(i)] != i)
                i = loneParent[static_cast<size_t>(i)];
            return i;
        };
        auto loneUnite = [&](int a, int b) {
            a = loneFind(a);
            b = loneFind(b);
            if (a != b)
                loneParent[static_cast<size_t>(b)] = a;
        };

        constexpr float kRiserClusterThreshold = 10.f;
        for (size_t i = 0; i < loneLinks.size(); ++i)
        {
            for (size_t j = i + 1; j < loneLinks.size(); ++j)
            {
                if (std::abs(loneLinks[i].RiserX - loneLinks[j].RiserX) > kRiserClusterThreshold)
                    continue;
                if (loneLinks[i].YMax < loneLinks[j].YMin - kRiserClusterThreshold ||
                    loneLinks[j].YMax < loneLinks[i].YMin - kRiserClusterThreshold)
                    continue;
                loneUnite(static_cast<int>(i), static_cast<int>(j));
            }
        }

        std::vector<std::vector<size_t>> loneComps(loneLinks.size());
        for (size_t i = 0; i < loneLinks.size(); ++i)
            loneComps[static_cast<size_t>(loneFind(static_cast<int>(i)))].push_back(i);

        for (auto& comp : loneComps)
        {
            if (comp.size() <= 1)
                continue;
            std::sort(comp.begin(), comp.end(), [&](size_t a, size_t b) {
                if (loneLinks[a].YMin != loneLinks[b].YMin)
                    return loneLinks[a].YMin < loneLinks[b].YMin;
                return loneLinks[a].LinkIdx < loneLinks[b].LinkIdx;
            });
            const int groupSize = static_cast<int>(comp.size());
            for (size_t k = 0; k < comp.size(); ++k)
            {
                const size_t linkIdx = loneLinks[comp[k]].LinkIdx;
                outTrack[linkIdx] = static_cast<int>(k);
                outNTracks[linkIdx] = groupSize;
            }
        }
    }

    /* The same fan across the other axis: wires still on one track after the
       riser pass can share a horizontal run just as easily as a vertical one,
       and a shared row reads as a single wire exactly the same way. */
    struct LaneSlot
    {
        size_t LinkIdx = 0;
        float LaneY = 0.f;
        float XMin = 0.f;
        float XMax = 0.f;
    };
    std::vector<LaneSlot> loneLanes;
    loneLanes.reserve(numLinks);
    for (size_t i = 0; i < numLinks; ++i)
    {
        if (isDegenerate(i) || outNTracks[i] > 1)
            continue;
        loneLanes.push_back({
            i,
            (segSy0[i] + segSy1[i]) * 0.5f,
            std::min(segSx0[i], segSx1[i]),
            std::max(segSx0[i], segSx1[i]),
        });
    }

    if (loneLanes.size() > 1)
    {
        constexpr float kLaneClusterThreshold = 10.f;
        std::vector<int> laneParent(loneLanes.size());
        for (size_t i = 0; i < loneLanes.size(); ++i)
            laneParent[i] = static_cast<int>(i);
        auto laneFind = [&laneParent](int i) -> int {
            while (laneParent[static_cast<size_t>(i)] != i)
                i = laneParent[static_cast<size_t>(i)];
            return i;
        };
        for (size_t i = 0; i < loneLanes.size(); ++i)
        {
            for (size_t j = i + 1; j < loneLanes.size(); ++j)
            {
                if (std::abs(loneLanes[i].LaneY - loneLanes[j].LaneY) > kLaneClusterThreshold)
                    continue;
                if (loneLanes[i].XMax < loneLanes[j].XMin - kLaneClusterThreshold ||
                    loneLanes[j].XMax < loneLanes[i].XMin - kLaneClusterThreshold)
                    continue;
                const int a = laneFind(static_cast<int>(i));
                const int b = laneFind(static_cast<int>(j));
                if (a != b)
                    laneParent[static_cast<size_t>(b)] = a;
            }
        }

        std::vector<std::vector<size_t>> laneComps(loneLanes.size());
        for (size_t i = 0; i < loneLanes.size(); ++i)
            laneComps[static_cast<size_t>(laneFind(static_cast<int>(i)))].push_back(i);

        for (auto& comp : laneComps)
        {
            if (comp.size() <= 1)
                continue;
            std::sort(comp.begin(), comp.end(), [&](size_t a, size_t b) {
                if (loneLanes[a].XMin != loneLanes[b].XMin)
                    return loneLanes[a].XMin < loneLanes[b].XMin;
                return loneLanes[a].LinkIdx < loneLanes[b].LinkIdx;
            });
            const int groupSize = static_cast<int>(comp.size());
            for (size_t k = 0; k < comp.size(); ++k)
            {
                const size_t linkIdx = loneLanes[comp[k]].LinkIdx;
                outTrack[linkIdx] = static_cast<int>(k);
                outNTracks[linkIdx] = groupSize;
            }
        }
    }
}

namespace
{
/* Index of `portId` among the node's ports of its own direction, and how many
   there are — the fan below spreads stub columns across that count. */
void PortSlotAndCount(const Graph::Node& node, const std::string& portId, int& outSlot, int& outCount)
{
    outSlot = -1;
    outCount = 0;
    const Graph::Port* self = nullptr;
    for (const Graph::Port& port : node.Ports)
    {
        if (port.Id == portId)
        {
            self = &port;
            break;
        }
    }
    if (!self)
        return;
    for (const Graph::Port& port : node.Ports)
    {
        if (port.Direction != self->Direction)
            continue;
        if (port.Id == portId)
            outSlot = outCount;
        ++outCount;
    }
}
} // namespace

void GraphCanvas::ComputeStraightRoute(const Graph::Node& srcNode, const Graph::Node& tgtNode,
                                             float sx0, float sy0, float sx1, float sy1,
                                             float canvasX, float canvasY,
                                             int trackIdx, int trackCount, float trackOffsetPx,
                                             float srcPortBias, float tgtPortBias,
                                             std::vector<Mathematics::Vector2>& outPoints) const
{
    outPoints.clear();

    /* Route in GRAPH units and apply the view transform only when emitting:
       the route is then a pure function of the graph, so neither panning nor
       zooming can change a wire's shape. Screen coordinates carry origin+pan,
       and grid-snapped layouts park many wires exactly on the router's
       decision boundaries (clearances, riser-blocked tests) — the float
       rounding a large translation or a zoom factor introduces flipped those
       comparisons and wires visibly re-routed under a mere view change. The
       recovered port positions still carry ~1e-4 of that noise, so they are
       snapped to a 1/16-unit lattice (layout sits on far coarser steps),
       making every decision bit-identical at any pan and zoom. */
    const float originX = canvasX + m_PanX;
    const float originY = canvasY + m_PanY;
    const float invZoom = 1.f / m_Zoom;
    auto toGraph = [&](float v) { return std::round(v * 16.f) * (1.f / 16.f); };
    sx0 = toGraph((sx0 - originX) * invZoom);
    sy0 = toGraph((sy0 - originY) * invZoom);
    sx1 = toGraph((sx1 - originX) * invZoom);
    sy1 = toGraph((sy1 - originY) * invZoom);

    /* Graph units throughout, endpoints included, so a zoom change rescales the
       picture without re-deciding any branch or moving any wire. */
    const float stubPx = 14.f;
    constexpr float kClearancePx = kWireClearanceGraph;
    /* The fan and the stagger may narrow a stub column but never close it. */
    constexpr float kMinPortStubPx = GraphConnectionRouting::kMinPortStubGraph;

    /* Graph units throughout (the endpoints are converted above); widths are
       per-node, and the height is the expanded-aware rect so a grown node
       blocks wires over its detail rows too. */
    const float srcLeft = srcNode.PositionX;
    const float srcTop = srcNode.PositionY;
    const float srcRight = srcLeft + GetNodeWidth(srcNode);
    const float srcBottom = srcTop + GetNodeRectHeight(srcNode);

    const float tgtLeft = tgtNode.PositionX;
    const float tgtTop = tgtNode.PositionY;
    const float tgtBottom = tgtTop + GetNodeRectHeight(tgtNode);

    const float tgtRight = tgtLeft + GetNodeWidth(tgtNode);
    const GraphConnectionRouting::OrthogonalStubs stubs =
        GraphConnectionRouting::ForwardAwareStubs(sx0, sx1, srcLeft, srcRight, tgtLeft, tgtRight,
                                                  stubPx);
    float exitX = stubs.ExitX;
    float enterX = stubs.EnterX;
    /* Fan the stub columns apart per port. Only inside the forward band, and
       clamped so the ordering the stubs established cannot flip: a backtracking
       route already separates on its lane. */
    if (enterX >= exitX && (srcPortBias != 0.f || tgtPortBias != 0.f))
    {
        const float biasedExit = std::min(exitX + srcPortBias, enterX);
        const float biasedEnter = std::max(enterX - tgtPortBias, biasedExit);
        exitX = std::max(biasedExit, sx0 + kMinPortStubPx);
        enterX = std::max(std::min(biasedEnter, sx1 - kMinPortStubPx), exitX);
    }

    std::vector<ScreenRect> obstacles;
    std::vector<GraphRouting::Obstacle> solidBodies;
    if (m_Model)
    {
        obstacles.reserve(m_Model->Nodes.size());
        solidBodies.reserve(m_Model->Nodes.size());
        for (const Graph::Node& node : m_Model->Nodes)
        {
            const float left = node.PositionX;
            const float top = node.PositionY;
            const float width = GetNodeWidth(node);
            const float height = GetNodeRectHeight(node);
            /* The wire begins and ends on its own two nodes: those keep a
               smaller gap, and none at all on the rows their ports sit on. */
            const bool own = node.Id == srcNode.Id || node.Id == tgtNode.Id;
            if (own)
            {
                solidBodies.push_back({{left, top, width, height}, kOwnBodyGapGraph, true});
                continue;
            }
            /* Port circles are centred on the node edge and stand out past it,
               so the gap is measured from the ports, not the body. */
            solidBodies.push_back(
                {{left - kPortFootprintGraph, top, width + 2.f * kPortFootprintGraph, height},
                 -1.f, false});
            obstacles.push_back({left, top, left + width, top + height});
        }
    }

    /* Nodes stacked at the same left edge share an enter column, so wires
       landing on them run down the same line whatever their riser does. Both
       route shapes below nudge that column by the cluster's stagger. */
    const float stagger = (trackCount > 1)
        ? (static_cast<float>(trackIdx) - static_cast<float>(trackCount - 1) * 0.5f) * trackOffsetPx
        : 0.f;

    /* Route in graph units, then convert once at the end: the obstacle tests
       below share the frame the obstacles are expressed in. */
    std::vector<Mathematics::Vector2> route;
    auto push = [&](float x, float y) { route.push_back({x, y}); };
    push(sx0, sy0);
    push(exitX, sy0);

    if (enterX >= exitX)
    {
        // Clamped inside the band so the horizontal run into the port cannot invert.
        if (stagger != 0.f)
            enterX = std::max(exitX, std::min(enterX - stagger, sx1 - kMinPortStubPx));
        float riserX = (exitX + enterX) * 0.5f + stagger;
        riserX = std::max(exitX, std::min(riserX, enterX));

        const float portSpanY = std::abs(sy1 - sy0);
        const float kFlatLinkThresholdPx = std::max(stubPx * 1.5f, 18.f);

        if (portSpanY <= kFlatLinkThresholdPx)
        {
            // Nearly level: route horizontally into the port without dipping below it.
            if (riserX > exitX + 0.5f)
                push(riserX, sy0);
            if (enterX > riserX + 0.5f)
                push(enterX, sy0);
            if (std::abs(sy1 - sy0) > 0.5f)
                push(enterX, sy1);
        }
        else
        {
            riserX = ShiftRiserXLeftUntilClear(riserX, exitX, sy0, sy1, obstacles,
                                               kClearancePx, trackOffsetPx);

            if (VerticalRiserBlocked(riserX, sy0, sy1, obstacles, kClearancePx))
            {
                float laneY = ChooseDetourLaneY(sy0, sy1, riserX, obstacles, kClearancePx,
                                                trackIdx, trackOffsetPx);
                /* Same per-port bias as the columns, so two detours around one
                   node do not land on the same row. */
                laneY += (laneY >= std::max(sy0, sy1)) ? std::abs(srcPortBias) : -std::abs(srcPortBias);
                push(exitX, laneY);
                push(riserX, laneY);
                push(riserX, sy1);
            }
            else
            {
                if (riserX > exitX + 0.5f)
                    push(riserX, sy0);
                push(riserX, sy1);
            }

            if (enterX > riserX + 0.5f)
                push(enterX, sy1);
        }
    }
    else
    {
        /* Backtracking link: route around both bodies. A direct drop from the
           exit stub to the target row would cut through whatever sits between
           them — the lane is what keeps the wire outside the nodes. The lane row
           takes the same per-port bias the stub columns take, so two wires
           wrapping the same node do not share one horizontal run. */
        /* The wrap comes down the enter column, so wires bound for nodes stacked
           at one X share that line even though their lanes differ. Stagger it,
           keeping it clear of the target body and left of the exit column. */
        if (stagger != 0.f)
            enterX = std::min({enterX - stagger, sx1 - kMinPortStubPx, exitX - 1.f});
        const float top = std::min(srcTop, tgtTop) - kClearancePx;
        const float bottom = std::max(srcBottom, tgtBottom) + kClearancePx;
        const float overCost = (sy0 - top) + (sy1 - top);
        const float underCost = (bottom - sy0) + (bottom - sy1);
        const float laneBias = std::abs(srcPortBias) + std::abs(tgtPortBias);
        const float laneY = (overCost <= underCost)
            ? top - static_cast<float>(trackIdx) * trackOffsetPx - laneBias
            : bottom + static_cast<float>(trackIdx) * trackOffsetPx + laneBias;
        push(exitX, laneY);
        push(enterX, laneY);
        push(enterX, sy1);
    }

    push(sx1, sy1);

    /* The geometry above reads well and costs nothing, but it only steers the
       riser around a body — a run it did not choose can still cross one. Pay
       for the shortest obstacle-free route only on the wires that do. */
    if (RouteCrossesObstacle(route, solidBodies, kClearancePx))
    {
        GraphRouting::RouteRequest request;
        request.Start = {sx0, sy0};
        request.End = {sx1, sy1};
        request.ExitX = exitX;
        request.EnterX = enterX;
        request.Clearance = kClearancePx;
        request.LaneSpread = std::abs(stagger);
        request.MinStub = kMinPortStubPx;

        std::vector<Mathematics::Vector2> solved;
        if (GraphRouting::SolveOrthogonalRoute(request, solidBodies, solved))
            route.swap(solved);
        else if (WireAuditEnabled())
        {
            Logger::Log::Warning(
                "graph wire audit: no route found from {} to {}; keeping the crossing fallback",
                srcNode.Id, tgtNode.Id);
        }
    }

    outPoints.swap(route);
}

namespace
{
/* A node as the routing helpers below need it: where it is, and what it is
   called for the audit to name it. */
struct NodeBody
{
    const std::string* Id = nullptr;
    Mathematics::Rect Box;
};

/* Everything a route is a function of: the graph's shape, the view it is drawn
   through, and the two states that take a wire out of the picture. */
uint64_t RouteKey(const Graph::Model& model, const std::vector<NodeBody>& bodies,
                  const std::string& reroutingLinkId,
                  const std::function<bool(const Graph::Edge&)>& hidden, float canvasX,
                  float canvasY, float panX, float panY, float zoom)
{
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](uint64_t value)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    auto mixFloat = [&mix](float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        mix(bits);
    };
    auto mixText = [&mix](const std::string& text) { mix(std::hash<std::string>{}(text)); };

    mixFloat(canvasX);
    mixFloat(canvasY);
    mixFloat(panX);
    mixFloat(panY);
    mixFloat(zoom);
    mixText(reroutingLinkId);
    for (const NodeBody& body : bodies)
    {
        mixText(*body.Id);
        mixFloat(body.Box.X);
        mixFloat(body.Box.Y);
        mixFloat(body.Box.Width);
        mixFloat(body.Box.Height);
    }
    for (const Graph::Edge& link : model.Links)
    {
        mixText(link.Id);
        mixText(link.SourceNodeId);
        mixText(link.SourcePortId);
        mixText(link.TargetNodeId);
        mixText(link.TargetPortId);
        mix(hidden(link) ? 1u : 0u);
    }
    return hash;
}

/* Every wire in the graph against every body, on demand: the claim "no wire
   crosses a node" is about the whole graph, and a screenshot only ever shows the
   part of it that is on screen. GE_GRAPH_WIRE_AUDIT=1 reports each crossing once
   per frame it survives, and each pair of wires still sharing a line. */
void AuditRoutes(const Graph::Model& model, const std::vector<NodeBody>& bodies,
                 const std::vector<GraphRouting::Route>& routes)
{
    if (!WireAuditEnabled())
        return;

    int crossings = 0;
    const size_t auditable = std::min(routes.size(), model.Links.size());
    for (size_t li = 0; li < auditable; ++li)
    {
        const GraphRouting::Route& route = routes[li];
        if (route.Points.size() < 2)
            continue;
        const Graph::Edge& link = model.Links[li];
        for (const NodeBody& body : bodies)
        {
            const bool own = *body.Id == link.SourceNodeId || *body.Id == link.TargetNodeId;
            const GraphRouting::Obstacle bare{body.Box, 0.f, own};
            if (!RouteCrossesObstacle(route.Points, {bare}, 0.f))
                continue;
            ++crossings;
            Logger::Log::Warning("graph wire audit: {}.{} -> {}.{} runs through {}",
                                 link.SourceNodeId, link.SourcePortId, link.TargetNodeId,
                                 link.TargetPortId, *body.Id);
        }
    }

    /* Runs that still share a line after the nudge: either they were clamped by
       what sits around them, or the pass never grouped them. */
    struct Run
    {
        size_t Link;
        bool Horizontal;
        float Coord;
        float SpanLo;
        float SpanHi;
    };
    std::vector<Run> runs;
    for (size_t li = 0; li < auditable; ++li)
    {
        const GraphRouting::Route& route = routes[li];
        for (size_t i = 0; i + 1 < route.Points.size(); ++i)
        {
            const Mathematics::Vector2 a = route.Points[i];
            const Mathematics::Vector2 b = route.Points[i + 1];
            const bool horizontal = std::abs(b.y - a.y) < 0.5f;
            const bool vertical = std::abs(b.x - a.x) < 0.5f;
            if (horizontal == vertical)
                continue;
            const float alongA = horizontal ? a.x : a.y;
            const float alongB = horizontal ? b.x : b.y;
            runs.push_back({li, horizontal, horizontal ? a.y : a.x, std::min(alongA, alongB),
                            std::max(alongA, alongB)});
        }
    }

    int shared = 0;
    for (size_t a = 0; a < runs.size(); ++a)
    {
        for (size_t b = a + 1; b < runs.size(); ++b)
        {
            if (runs[a].Link == runs[b].Link || runs[a].Horizontal != runs[b].Horizontal)
                continue;
            if (std::abs(runs[a].Coord - runs[b].Coord) > kWireSeparationGraph * 0.5f)
                continue;
            const float overlap = std::min(runs[a].SpanHi, runs[b].SpanHi) -
                                  std::max(runs[a].SpanLo, runs[b].SpanLo);
            if (overlap <= kWireSeparationGraph)
                continue;
            ++shared;
            const Graph::Edge& la = model.Links[runs[a].Link];
            const Graph::Edge& lb = model.Links[runs[b].Link];
            // Formatted here rather than deferred: the lazy path copies every argument
            // into a fixed 384-byte inline buffer, and these nine strings do not fit
            // (a Debug std::string is 40 bytes). Pre-formatted messages take the
            // string_view overload, which captures one string.
            Logger::Log::Warning(std::format(
                "graph wire audit: {} run shared by {}.{}->{}.{} and {}.{}->{}.{} at {:.1f}/{:.1f}, "
                "overlap {:.0f}",
                runs[a].Horizontal ? "horizontal" : "vertical", la.SourceNodeId, la.SourcePortId,
                la.TargetNodeId, la.TargetPortId, lb.SourceNodeId, lb.SourcePortId, lb.TargetNodeId,
                lb.TargetPortId, runs[a].Coord, runs[b].Coord, overlap));
        }
    }

    Logger::Log::Info("graph wire audit: {} links, {} bodies, {} crossings, {} shared runs",
                      routes.size(), bodies.size(), crossings, shared);
}
} // namespace

void GraphCanvas::BuildStraightRoutes(float canvasX, float canvasY,
                                      std::vector<GraphRouting::Route>& outRoutes) const
{
    outRoutes.clear();
    if (!m_Model)
        return;

    std::vector<NodeBody> nodeBodies;
    nodeBodies.reserve(m_Model->Nodes.size());
    for (const Graph::Node& node : m_Model->Nodes)
    {
        nodeBodies.push_back({&node.Id,
                              {node.PositionX, node.PositionY, GetNodeWidth(node),
                               GetNodeRectHeight(node)}});
    }

    /* Drawing and hit-testing both want these in one frame, and the second ask
       is the same answer as the first. */
    const uint64_t key =
        RouteKey(*m_Model, nodeBodies, m_ReroutingLinkId,
                 [this](const Graph::Edge& link) { return IsLinkHiddenPendingDelete(link); },
                 canvasX, canvasY, m_PanX, m_PanY, m_Zoom);
    if (key == m_RouteCacheKey && m_RouteCache.size() == m_Model->Links.size())
    {
        outRoutes = m_RouteCache;
        return;
    }

    const size_t numLinks = m_Model->Links.size();
    outRoutes.resize(numLinks);

    std::vector<float> segSx0(numLinks, 0.f), segSy0(numLinks, 0.f);
    std::vector<float> segSx1(numLinks, 0.f), segSy1(numLinks, 0.f);
    std::vector<int> track(numLinks, 0);
    std::vector<int> nTracks(numLinks, 1);

    auto graphToScreen = [&](float gx, float gy, float& sx, float& sy)
    {
        sx = canvasX + m_PanX + gx * m_Zoom;
        sy = canvasY + m_PanY + gy * m_Zoom;
    };

    for (size_t li = 0; li < numLinks; ++li)
    {
        const auto& link = m_Model->Links[li];
        if (IsLinkHiddenPendingDelete(link) ||
            (!m_ReroutingLinkId.empty() && link.Id == m_ReroutingLinkId))
            continue;
        const Graph::Node* src = m_Model->FindNode(link.SourceNodeId);
        const Graph::Node* tgt = m_Model->FindNode(link.TargetNodeId);
        if (!src || !tgt)
            continue;
        float gx0 = 0.f, gy0 = 0.f, gx1 = 0.f, gy1 = 0.f;
        GetPortCenterInGraph(*src, FindPortIndex(*src, link.SourcePortId), gx0, gy0);
        GetPortCenterInGraph(*tgt, FindPortIndex(*tgt, link.TargetPortId), gx1, gy1);
        graphToScreen(gx0, gy0, segSx0[li], segSy0[li]);
        graphToScreen(gx1, gy1, segSx1[li], segSy1[li]);
    }

    ComputeConnectionTracks(segSx0, segSy0, segSx1, segSy1, canvasX, canvasY, track, nTracks);

    constexpr float trackOffsetPx = kWireSeparationGraph;
    for (size_t li = 0; li < numLinks; ++li)
    {
        if (segSx0[li] == 0.f && segSy0[li] == 0.f && segSx1[li] == 0.f && segSy1[li] == 0.f)
            continue;
        const auto& link = m_Model->Links[li];
        const Graph::Node* src = m_Model->FindNode(link.SourceNodeId);
        const Graph::Node* tgt = m_Model->FindNode(link.TargetNodeId);
        if (!src || !tgt)
            continue;
        int srcSlot = -1, srcCount = 0, tgtSlot = -1, tgtCount = 0;
        PortSlotAndCount(*src, link.SourcePortId, srcSlot, srcCount);
        PortSlotAndCount(*tgt, link.TargetPortId, tgtSlot, tgtCount);
        ComputeStraightRoute(*src, *tgt, segSx0[li], segSy0[li], segSx1[li], segSy1[li],
                             canvasX, canvasY, track[li], nTracks[li], trackOffsetPx,
                             GraphConnectionRouting::PortLaneBias(srcSlot, srcCount, trackOffsetPx),
                             GraphConnectionRouting::PortLaneBias(tgtSlot, tgtCount, trackOffsetPx),
                             outRoutes[li].Points);
    }

    /* A shortest route hugs whatever it went around, so centre the runs that
       can move for free before separating the ones that share a line. Both in
       graph units, so neither decision moves with the view. */
    std::vector<GraphRouting::Obstacle> bodies;
    bodies.reserve(nodeBodies.size());
    for (const NodeBody& body : nodeBodies)
    {
        Mathematics::Rect box = body.Box;
        box.X -= kPortFootprintGraph;
        box.Width += 2.f * kPortFootprintGraph;
        bodies.push_back({box, -1.f, false});
    }
    GraphRouting::CentreRunsInFreeSpace(outRoutes, bodies, kWireClearanceGraph);
    std::vector<GraphRouting::NudgeNote> notes;
    GraphRouting::NudgeSharedRuns(outRoutes, bodies, kWireClearanceGraph, kWireSeparationGraph,
                                  WireAuditEnabled() ? &notes : nullptr);
    if (WireAuditEnabled())
    {
        for (const GraphRouting::NudgeNote& note : notes)
        {
            if (note.HasRoom && std::abs(note.To - note.From) > 0.5f)
                continue; // it moved; the audit below reports what is still shared
            const auto& link = m_Model->Links[note.RouteIndex];
            Logger::Log::Warning(
                "graph wire audit: {}.{}->{}.{} seg {} {} stayed at {:.1f} (room {:.1f}..{:.1f}{})",
                link.SourceNodeId, link.SourcePortId, link.TargetNodeId, link.TargetPortId,
                note.SegmentIndex, note.Horizontal ? "horizontal" : "vertical", note.From, note.Lo,
                note.Hi, note.HasRoom ? "" : ", none");
        }
    }

    AuditRoutes(*m_Model, nodeBodies, outRoutes);

    const float originX = canvasX + m_PanX;
    const float originY = canvasY + m_PanY;
    for (GraphRouting::Route& route : outRoutes)
    {
        for (Mathematics::Vector2& point : route.Points)
            point = {originX + point.x * m_Zoom, originY + point.y * m_Zoom};
    }

    m_RouteCacheKey = key;
    m_RouteCache = outRoutes;
}

std::string GraphCanvas::FindHitLink(float screenX, float screenY, float canvasX, float canvasY) const
{
    if (!m_Model || m_Model->Links.empty())
        return {};
    constexpr uint32_t kCurveSegments = 24;
    constexpr float kHitThreshold = 10.f; /* pixels */

    auto graphToScreen = [&](float gx, float gy, float& sx, float& sy)
    {
        sx = canvasX + m_PanX + gx * m_Zoom;
        sy = canvasY + m_PanY + gy * m_Zoom;
    };

    auto pointToSegmentDist = [](float px, float py, float ax, float ay, float bx, float by) -> float
    {
        float dx = bx - ax;
        float dy = by - ay;
        float len2 = dx * dx + dy * dy;
        if (len2 < 1e-10f)
            return std::sqrt((px - ax) * (px - ax) + (py - ay) * (py - ay));
        float t = ((px - ax) * dx + (py - ay) * dy) / len2;
        t = std::max(0.f, std::min(1.f, t));
        float qx = ax + t * dx;
        float qy = ay + t * dy;
        return std::sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy));
    };

    const size_t numLinks = m_Model->Links.size();
    std::vector<float> segSx0(numLinks), segSy0(numLinks), segSx1(numLinks), segSy1(numLinks);

    for (size_t i = 0; i < numLinks; ++i)
    {
        const auto& link = m_Model->Links[i];
        if (IsLinkHiddenPendingDelete(link) ||
            (!m_ReroutingLinkId.empty() && link.Id == m_ReroutingLinkId))
        {
            segSx0[i] = segSy0[i] = segSx1[i] = segSy1[i] = 0.f;
            continue;
        }
        const Graph::Node* srcNode = m_Model->FindNode(link.SourceNodeId);
        const Graph::Node* tgtNode = m_Model->FindNode(link.TargetNodeId);
        if (!srcNode || !tgtNode)
        {
            segSx0[i] = segSy0[i] = segSx1[i] = segSy1[i] = 0.f;
            continue;
        }
        const size_t si = FindPortIndex(*srcNode, link.SourcePortId);
        const size_t ti = FindPortIndex(*tgtNode, link.TargetPortId);
        float gx0, gy0, gx1, gy1;
        GetPortCenterInGraph(*srcNode, si, gx0, gy0);
        GetPortCenterInGraph(*tgtNode, ti, gx1, gy1);
        graphToScreen(gx0, gy0, segSx0[i], segSy0[i]);
        graphToScreen(gx1, gy1, segSx1[i], segSy1[i]);
    }

    /* Hit-test the wires the canvas actually drew, nudges included. */
    std::vector<GraphRouting::Route> routes;
    if (m_UseStraightLines)
        BuildStraightRoutes(canvasX, canvasY, routes);

    /* Iterate links in reverse so the top-most drawn link wins (same as draw order). */
    for (int li = static_cast<int>(m_Model->Links.size()) - 1; li >= 0; --li)
    {
        const auto& link = m_Model->Links[static_cast<size_t>(li)];
        if (IsLinkHiddenPendingDelete(link) ||
            (!m_ReroutingLinkId.empty() && link.Id == m_ReroutingLinkId))
            continue;
        const Graph::Node* srcNode = m_Model->FindNode(link.SourceNodeId);
        const Graph::Node* tgtNode = m_Model->FindNode(link.TargetNodeId);
        if (!srcNode || !tgtNode)
            continue;
        float sx0 = segSx0[static_cast<size_t>(li)];
        float sy0 = segSy0[static_cast<size_t>(li)];
        float sx1 = segSx1[static_cast<size_t>(li)];
        float sy1 = segSy1[static_cast<size_t>(li)];

        if (m_UseStraightLines)
        {
            const std::vector<Mathematics::Vector2>& points =
                routes[static_cast<size_t>(li)].Points;
            float d = 1e9f;
            for (size_t k = 1; k < points.size(); ++k)
                d = std::min(d, pointToSegmentDist(screenX, screenY, points[k - 1].x,
                                                   points[k - 1].y, points[k].x, points[k].y));
            if (d < kHitThreshold)
                return link.Id;
            continue;
        }

        float cx0 = 0.f, cy0 = 0.f, cx1 = 0.f, cy1 = 0.f;
        ComputeConnectionBezierControls(sx0, sy0, sx1, sy1, m_Zoom, cx0, cy0, cx1, cy1);

        float minDist = 1e9f;
        float prevX = sx0;
        float prevY = sy0;
        for (uint32_t i = 1; i <= kCurveSegments; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(kCurveSegments);
            float u = 1.0f - t;
            float u2 = u * u;
            float u3 = u2 * u;
            float t2 = t * t;
            float t3 = t2 * t;
            float x = u3 * sx0 + 3.f * u2 * t * cx0 + 3.f * u * t2 * cx1 + t3 * sx1;
            float y = u3 * sy0 + 3.f * u2 * t * cy0 + 3.f * u * t2 * cy1 + t3 * sy1;
            float d = pointToSegmentDist(screenX, screenY, prevX, prevY, x, y);
            minDist = std::min(minDist, d);
            prevX = x;
            prevY = y;
        }
        if (minDist < kHitThreshold)
            return link.Id;
    }
    return {};
}

bool GraphCanvas::FindHitLinkEndpoint(float screenX, float screenY, float canvasX, float canvasY,
                                            std::string& outLinkId, bool& outNearSource) const
{
    outLinkId.clear();
    outNearSource = false;
    if (!m_Model || m_Model->Links.empty())
        return false;

    constexpr float kEndpointHitThresholdPx = 18.f;
    const float thresholdSq = kEndpointHitThresholdPx * kEndpointHitThresholdPx;
    float bestDistSq = std::numeric_limits<float>::max();

    auto graphToScreen = [&](float gx, float gy, float& sx, float& sy)
    {
        sx = canvasX + m_PanX + gx * m_Zoom;
        sy = canvasY + m_PanY + gy * m_Zoom;
    };

    for (const auto& link : m_Model->Links)
    {
        if (IsLinkHiddenPendingDelete(link) ||
            (!m_ReroutingLinkId.empty() && link.Id == m_ReroutingLinkId))
            continue;

        const Graph::Node* srcNode = m_Model->FindNode(link.SourceNodeId);
        const Graph::Node* tgtNode = m_Model->FindNode(link.TargetNodeId);
        if (!srcNode || !tgtNode)
            continue;

        const size_t si = FindPortIndex(*srcNode, link.SourcePortId);
        const size_t ti = FindPortIndex(*tgtNode, link.TargetPortId);

        float srcGx = 0.f, srcGy = 0.f, tgtGx = 0.f, tgtGy = 0.f;
        GetPortCenterInGraph(*srcNode, si, srcGx, srcGy);
        GetPortCenterInGraph(*tgtNode, ti, tgtGx, tgtGy);

        float srcSx = 0.f, srcSy = 0.f, tgtSx = 0.f, tgtSy = 0.f;
        graphToScreen(srcGx, srcGy, srcSx, srcSy);
        graphToScreen(tgtGx, tgtGy, tgtSx, tgtSy);

        const float srcDx = screenX - srcSx;
        const float srcDy = screenY - srcSy;
        const float srcDistSq = srcDx * srcDx + srcDy * srcDy;
        if (srcDistSq < bestDistSq)
        {
            bestDistSq = srcDistSq;
            outLinkId = link.Id;
            outNearSource = true;
        }

        const float tgtDx = screenX - tgtSx;
        const float tgtDy = screenY - tgtSy;
        const float tgtDistSq = tgtDx * tgtDx + tgtDy * tgtDy;
        if (tgtDistSq < bestDistSq)
        {
            bestDistSq = tgtDistSq;
            outLinkId = link.Id;
            outNearSource = false;
        }
    }

    if (bestDistSq > thresholdSq)
    {
        outLinkId.clear();
        outNearSource = false;
        return false;
    }
    return true;
}

void GraphCanvas::RegisterEventHandlers()
{
    GraphCanvas* self = this;
    m_Tokens->mouseDown = RegisterEventHandler(kEventMouseDown, [self](UIEvent& ev)
    {
        /* The platform menu has no close callback, so a stale pending wire from
           a dismissed wire-drop menu is retired by the next press. */
        if (self->m_WireDropMenuOpen)
            self->SetWireDropMenuOpen(false);
        /* No editor-widget filtering here: editor surfaces inside the nodes
           claim their own primary presses (ClaimPrimaryPresses / the standard
           control consumption), so a press that reaches the canvas is a
           canvas gesture by construction. */
        const float cx = self->GetLayoutX();
        const float cy = self->GetLayoutY();
        float gx, gy;
        self->ScreenToGraph(ev.X, ev.Y, cx, cy, 0.f, 0.f, gx, gy);

        std::string nodeId, portId;
        bool isOutput;
        self->FindHit(gx, gy, nodeId, portId, isOutput);

        /* Left button (0): node drag, link drag, selection box */
        if (ev.Button == 0)
        {
            // Announced before the press is interpreted, and regardless of what
            // it turns out to mean: a host watching for "the user clicked this
            // canvas" cannot get that from the InputSystem, because the branch
            // below consumes the press.
            if (self->m_OnPrimaryPress)
                self->m_OnPrimaryPress();
            ev.Capture(self);
            auto beginRerouteLink = [&](const std::string& endpointLinkId, bool nearSourceEndpoint) -> bool
            {
                if (!self->m_Model)
                    return false;

                const Graph::Edge* rerouteLink = nullptr;
                for (const auto& link : self->m_Model->Links)
                {
                    if (link.Id == endpointLinkId)
                    {
                        rerouteLink = &link;
                        break;
                    }
                }
                if (!rerouteLink)
                    return false;

                self->m_DraggingLink = true;
                self->m_DraggingLinkToExistingInput = nearSourceEndpoint;
                self->m_ReroutingLinkId = rerouteLink->Id;
                self->m_ConnectionSourceNodeId = nearSourceEndpoint ? std::string() : rerouteLink->SourceNodeId;
                self->m_ConnectionSourcePortId = nearSourceEndpoint ? std::string() : rerouteLink->SourcePortId;
                self->m_ConnectionTargetNodeId = nearSourceEndpoint ? rerouteLink->TargetNodeId : std::string();
                self->m_ConnectionTargetPortId = nearSourceEndpoint ? rerouteLink->TargetPortId : std::string();
                self->m_ConnectionEndGraphX = gx;
                self->m_ConnectionEndGraphY = gy;
                self->m_HoveredLinkId.clear();
                self->SetTooltip("Drag wire\nRelease on a compatible connection.");
                self->MarkDirty(VisualDirty);
                return true;
            };

            std::string endpointLinkId;
            bool nearSourceEndpoint = false;
            if (self->FindHitLinkEndpoint(ev.X, ev.Y, cx, cy, endpointLinkId, nearSourceEndpoint) && self->m_Model)
            {
                const Graph::Edge* endpointLink = nullptr;
                for (const auto& link : self->m_Model->Links)
                {
                    if (link.Id == endpointLinkId)
                    {
                        endpointLink = &link;
                        break;
                    }
                }

                const bool hitEmptyCanvas = nodeId.empty() && portId.empty();
                const bool hitSourcePort = endpointLink && nearSourceEndpoint && isOutput &&
                    nodeId == endpointLink->SourceNodeId && portId == endpointLink->SourcePortId;
                const bool hitTargetPort = endpointLink && !nearSourceEndpoint && !isOutput &&
                    nodeId == endpointLink->TargetNodeId && portId == endpointLink->TargetPortId;
                if ((hitEmptyCanvas || hitSourcePort || hitTargetPort) &&
                    beginRerouteLink(endpointLinkId, nearSourceEndpoint))
                {
                    ev.Stop();
                    return;
                }
            }

            if (!portId.empty())
            {
                self->m_PendingNodeDoubleClick = false;
                /* Complete a sticky wire that started on an input port (mouse-down again is only
                   possible after the press-click released, i.e. click-to-connect mode). */
                if (self->m_DraggingLink && self->m_DraggingLinkToExistingInput && isOutput &&
                    self->m_ReroutingLinkId.empty() && self->m_Model)
                {
                    Graph::Node* sourceNode = self->m_Model->FindNode(nodeId);
                    const Graph::Port* sourcePort = sourceNode ? self->FindPort(*sourceNode, portId) : nullptr;
                    const auto pending = self->PendingConnection();
                    if (sourceNode && sourcePort && pending && pending->Accepts(*sourceNode, *sourcePort))
                    {
                        std::string sourceNodeId = nodeId;
                        std::string sourcePortId = portId;
                        std::string targetNodeId = self->m_ConnectionTargetNodeId;
                        std::string targetPortId = self->m_ConnectionTargetPortId;
                        auto addLink = [self, sourceNodeId, sourcePortId, targetNodeId, targetPortId]()
                        {
                            Graph::Edge link;
                            link.Id = self->m_Model->GenerateLinkId();
                            link.SourceNodeId = sourceNodeId;
                            link.SourcePortId = sourcePortId;
                            link.TargetNodeId = targetNodeId;
                            link.TargetPortId = targetPortId;
                            self->m_Model->Links.push_back(link);
                            self->NotifyGraphChanged();
                        };
                        if (self->m_UndoScope)
                            self->m_UndoScope("Add Link", std::move(addLink));
                        else
                            addLink();
                        self->m_DraggingLink = false;
                        self->m_DraggingLinkToExistingInput = false;
                        self->m_ConnectionTargetNodeId.clear();
                        self->m_ConnectionTargetPortId.clear();
                        self->SetTooltip({});
                        self->MarkDirty(VisualDirty);
                        ev.Stop();
                        return;
                    }
                }

                if (isOutput)
                {
                    const bool isDoubleClick = ClickedTwice(self->m_LastPress, nodeId, portId);
                    if (isDoubleClick && self->m_Model)
                    {
                        /* Auto-connect to single or closest valid target (green dot) */
                        self->m_ConnectionSourceNodeId = nodeId;
                        self->m_ConnectionSourcePortId = portId;
                        self->m_DraggingLink = true;
                        float srcX = 0.f, srcY = 0.f;
                        const Graph::Node* srcNode = self->m_Model->FindNode(nodeId);
                        if (srcNode)
                        {
                            const size_t srcPortIdx = FindPortIndex(*srcNode, portId);
                            self->GetPortCenterInGraph(*srcNode, srcPortIdx, srcX, srcY);
                        }
                        struct PortTarget { std::string nodeId; std::string portId; float distSq; };
                        std::vector<PortTarget> targets;
                        const auto pending = self->PendingConnection();
                        for (const auto& n : self->m_Model->Nodes)
                        {
                            if (n.Id == nodeId) continue; /* never auto-connect a node to itself */
                            for (size_t pi = 0; pi < n.Ports.size(); ++pi)
                            {
                                const auto& p = n.Ports[pi];
                                if (p.Direction != Graph::PortDirection::In) continue;
                                if (!pending || !pending->Accepts(n, p)) continue;
                                float tx, ty;
                                self->GetPortCenterInGraph(n, pi, tx, ty);
                                float dx = tx - srcX, dy = ty - srcY;
                                targets.push_back({ n.Id, p.Id, dx * dx + dy * dy });
                            }
                        }
                        self->m_DraggingLink = false;
                        self->m_ConnectionSourceNodeId.clear();
                        self->m_ConnectionSourcePortId.clear();
                        self->m_ConnectionTargetNodeId.clear();
                        self->m_ConnectionTargetPortId.clear();
                        self->m_DraggingLinkToExistingInput = false;
                        self->m_ReroutingLinkId.clear();
                        if (!targets.empty())
                        {
                            auto best = std::min_element(targets.begin(), targets.end(),
                                [](const PortTarget& a, const PortTarget& b) { return a.distSq < b.distSq; });
                            std::string tNodeId = best->nodeId;
                            std::string tPortId = best->portId;
                            auto addLink = [self, nodeId, portId, tNodeId, tPortId]()
                            {
                                Graph::Edge link;
                                link.Id = self->m_Model->GenerateLinkId();
                                link.SourceNodeId = nodeId;
                                link.SourcePortId = portId;
                                link.TargetNodeId = tNodeId;
                                link.TargetPortId = tPortId;
                                self->m_Model->Links.push_back(link);
                                self->NotifyGraphChanged();
                            };
                            if (self->m_UndoScope)
                                self->m_UndoScope("Add Link", std::move(addLink));
                            else
                                addLink();
                            self->MarkDirty(VisualDirty);
                        }
                        ev.Stop();
                        return;
                    }

                    self->m_DraggingLink = true;
                    self->m_ConnectionSourceNodeId = nodeId;
                    self->m_ConnectionSourcePortId = portId;
                    self->m_DraggingLinkToExistingInput = false;
                    self->m_ConnectionTargetNodeId.clear();
                    self->m_ConnectionTargetPortId.clear();
                    self->m_ReroutingLinkId.clear();
                    self->m_ConnectionEndGraphX = gx;
                    self->m_ConnectionEndGraphY = gy;
                }
                else if (self->m_Model && ClickedTwice(self->m_LastPress, nodeId, portId) &&
                         (!self->m_DraggingLink ||
                          (self->m_DraggingLinkToExistingInput &&
                           self->m_ConnectionTargetNodeId == nodeId &&
                           self->m_ConnectionTargetPortId == portId)))
                {
                    /* Mirror of the output-port double-click: from an input, find
                       the nearest output this port could legally take and connect
                       it. The first click of the pair already armed a pending
                       connection from this same port (the branch below), so the
                       second arrives with the drag live — cancel it rather than
                       re-arming, which is all a double-click used to do. */
                    self->m_DraggingLink = false;
                    self->m_DraggingLinkToExistingInput = false;
                    self->m_ConnectionTargetNodeId.clear();
                    self->m_ConnectionTargetPortId.clear();
                    self->SetTooltip({});
                    self->AutoConnectInputPort(nodeId, portId, gx, gy);
                    ev.Stop();
                    return;
                }
                else if (self->m_DraggingLink && !self->m_DraggingLinkToExistingInput && self->m_Model)
                {
                    Graph::Node* targetNode = self->m_Model->FindNode(nodeId);
                    const Graph::Port* targetPort = targetNode ? self->FindPort(*targetNode, portId) : nullptr;
                    const auto pending = self->PendingConnection();
                    if (targetNode && targetPort && pending && pending->Accepts(*targetNode, *targetPort))
                    {
                        auto addLink = [self, nodeId, portId]()
                        {
                            Graph::Edge link;
                            link.Id = self->m_Model->GenerateLinkId();
                            link.SourceNodeId = self->m_ConnectionSourceNodeId;
                            link.SourcePortId = self->m_ConnectionSourcePortId;
                            link.TargetNodeId = nodeId;
                            link.TargetPortId = portId;
                            self->m_Model->Links.push_back(link);
                            self->NotifyGraphChanged();
                        };
                        if (self->m_UndoScope)
                            self->m_UndoScope("Add Link", std::move(addLink));
                        else
                            addLink();
                    }
                    self->m_DraggingLink = false;
                    self->m_DraggingLinkToExistingInput = false;
                    self->m_ReroutingLinkId.clear();
                    self->m_ConnectionTargetNodeId.clear();
                    self->m_ConnectionTargetPortId.clear();
                }
                else if (self->m_Model)
                {
                    const Graph::Edge* existingLink = nullptr;
                    for (const auto& link : self->m_Model->Links)
                    {
                        if (link.TargetNodeId == nodeId && link.TargetPortId == portId)
                        {
                            existingLink = &link;
                            break;
                        }
                    }
                    if (existingLink && beginRerouteLink(existingLink->Id, true))
                    {
                        ev.Stop();
                        return;
                    }

                    self->m_DraggingLink = true;
                    self->m_DraggingLinkToExistingInput = true;
                    self->m_ConnectionTargetNodeId = nodeId;
                    self->m_ConnectionTargetPortId = portId;
                    self->m_ConnectionSourceNodeId.clear();
                    self->m_ConnectionSourcePortId.clear();
                    self->m_ReroutingLinkId.clear();
                    self->m_ConnectionEndGraphX = gx;
                    self->m_ConnectionEndGraphY = gy;
                    self->SetTooltip("Drag wire\nRelease on a compatible output connection.");
                }
            }
            else
            {
                if (self->m_DraggingLink)
                {
                    /* Sticky wire pending from a port click. A press on empty
                       canvas means the same as dragging the wire out and
                       dropping it here, so it opens the wire-drop menu; a press
                       on a node just cancels. */
                    if (nodeId.empty() && self->m_OnRequestWireDropMenu)
                    {
                        if (self->m_DraggingLinkToExistingInput)
                            self->m_OnRequestWireDropMenu(ev.X, ev.Y,
                                                          self->m_ConnectionTargetNodeId,
                                                          self->m_ConnectionTargetPortId, true);
                        else
                            self->m_OnRequestWireDropMenu(ev.X, ev.Y,
                                                          self->m_ConnectionSourceNodeId,
                                                          self->m_ConnectionSourcePortId, false);
                    }
                    self->m_DraggingLink = false;
                    self->m_DraggingLinkToExistingInput = false;
                    self->m_ReroutingLinkId.clear();
                    /* The drop menu draws the pending wire from these ids while
                       it is open; SetWireDropMenuOpen(false) clears them. */
                    if (!self->m_WireDropMenuOpen)
                    {
                        self->m_ConnectionSourceNodeId.clear();
                        self->m_ConnectionSourcePortId.clear();
                        self->m_ConnectionTargetNodeId.clear();
                        self->m_ConnectionTargetPortId.clear();
                    }
                    self->SetTooltip({});
                    self->MarkDirty(VisualDirty);
                }
                if (!nodeId.empty())
                {
                    self->SetSelectedLinkId({});
                    // Selection modifiers — mirrors the Hierarchy / TreeView
                    // convention. Range-select isn't meaningful in a node
                    // graph (no linear row order) so Shift collapses onto the
                    // same toggle behavior as the primary shortcut modifier.
                    //   No modifier:               replace (resolved on
                    //                              mouse-up via the pending-
                    //                              drag path so click+drag is
                    //                              still "select-and-move").
                    //   Cmd/Ctrl  (primary mod):   toggle
                    //   Shift / Shift+primary:     toggle (range N/A → falls
                    //                              back to toggle).
                    const bool primaryMod = Input::IsPrimaryShortcutModifier(ev.Mods);
                    const bool shift      = (ev.Mods & Input::kModShift) != 0;
                    if (primaryMod || shift)
                    {
                        if (self->m_SelectedNodeIds.count(nodeId))
                            self->m_SelectedNodeIds.erase(nodeId);
                        else
                            self->m_SelectedNodeIds.insert(nodeId);
                        self->ApplyNodeVisualStates();
                        self->MarkDirty(VisualDirty);
                        if (self->m_OnSelectionChanged)
                            self->m_OnSelectionChanged(self->GetSelectedNodeId());
                    }
                    else
                    {
                        self->m_PendingNodeDoubleClick = ClickedTwice(self->m_LastPress, nodeId, {});
                        self->m_PendingNodeDragId = nodeId;
                        self->m_DragStartX = ev.X;
                        self->m_DragStartY = ev.Y;
                        self->MarkDirty(VisualDirty);
                    }
                }
                else
                {
                    self->m_PendingNodeDragId.clear();
                    self->m_PendingNodeDoubleClick = false;
                    const std::string hitLinkId = self->FindHitLink(ev.X, ev.Y, cx, cy);
                    if (!hitLinkId.empty())
                    {
                        self->SetSelectedLinkId(hitLinkId);
                    }
                    else
                    {
                        // Start selection box on empty canvas area
                        self->m_DraggingSelectionBox = true;
                        self->m_DragStartX = ev.X;
                        self->m_DragStartY = ev.Y;
                        self->m_SelectionBoxStartX = ev.X;
                        self->m_SelectionBoxStartY = ev.Y;
                        self->m_SelectionBoxCurrentX = ev.X;
                        self->m_SelectionBoxCurrentY = ev.Y;
                        // Snapshot the current selection and the modifier state at
                        // the start of the gesture. The compose mode is latched
                        // here so the same gesture is honored even if the user
                        // releases the modifier mid-drag.
                        //   Shift held       → add hits to the base selection
                        //   Cmd/Ctrl held    → remove hits from the base selection
                        //   No modifier      → replace base with hits
                        {
                            const bool primaryMod = Input::IsPrimaryShortcutModifier(ev.Mods);
                            const bool shift      = (ev.Mods & Input::kModShift) != 0;
                            if (primaryMod)
                                self->m_MarqueeComposeMode = 2;
                            else if (shift)
                                self->m_MarqueeComposeMode = 1;
                            else
                                self->m_MarqueeComposeMode = 0;
                            self->m_MarqueeBaseSelection = self->m_SelectedNodeIds;
                        }
                        // Don't clear selection yet - will clear on mouse up if no nodes are in the box
                        self->m_HoveredLinkId.clear();
                        self->m_HoveredNodeId.clear();
                        self->m_HoveredPortId.clear();
                        self->SetTooltip({});
                        self->MarkDirty(VisualDirty);
                    }
                }
            }
            ev.Stop();
        }
        /* Right (1): pan, click-delete a wire, or context menu. Middle (2): pan only. */
        else if (ev.Button == Input::kMouseButton_Right || ev.Button == Input::kMouseButton_Middle)
        {
            ev.Capture(self);
            HideTooltip(self);
            self->m_DraggingPan = true;
            self->m_PendingLinkRemoveId = ev.Button == Input::kMouseButton_Right
                ? self->FindHitLink(ev.X, ev.Y, cx, cy)
                : std::string();
            self->m_RightMouseDragged = ev.Button == Input::kMouseButton_Middle;
            self->m_RightMouseDownX = ev.X;
            self->m_RightMouseDownY = ev.Y;
            self->m_RightMouseTravelPx = 0.f;
            self->m_RightMouseLastX = ev.X;
            self->m_RightMouseLastY = ev.Y;
            self->m_DragStartX = ev.X;
            self->m_DragStartY = ev.Y;
            self->MarkDirty(VisualDirty);
            ev.Stop();
        }
    });
    m_Tokens->mouseUp = RegisterEventHandler(kEventMouseUp, [self](UIEvent& ev)
    {
        if (ev.Button == 0)
        {
            if (!self->m_PendingNodeDragId.empty())
            {
                const std::string clickedId = self->m_PendingNodeDragId;
                const bool fireDoubleClick = self->m_PendingNodeDoubleClick;
                self->m_PendingNodeDoubleClick = false;
                self->m_SelectedNodeIds.clear();
                self->m_SelectedNodeIds.insert(clickedId);
                self->m_PendingNodeDragId.clear();
                self->ApplyNodeVisualStates();
                self->MarkDirty(VisualDirty);
                if (self->m_OnSelectionChanged)
                    self->m_OnSelectionChanged(self->GetSelectedNodeId());
                if (fireDoubleClick && self->m_OnNodeDoubleClicked)
                    self->m_OnNodeDoubleClicked(clickedId);
            }
            else if (self->m_DraggingSelectionBox && self->m_Model)
            {
                // Finalize selection box - select all nodes intersecting the rectangle
                const float cx = self->GetLayoutX();
                const float cy = self->GetLayoutY();

                // Calculate selection rectangle in screen space
                float left = std::min(self->m_SelectionBoxStartX, self->m_SelectionBoxCurrentX);
                float right = std::max(self->m_SelectionBoxStartX, self->m_SelectionBoxCurrentX);
                float top = std::min(self->m_SelectionBoxStartY, self->m_SelectionBoxCurrentY);
                float bottom = std::max(self->m_SelectionBoxStartY, self->m_SelectionBoxCurrentY);
                
                // Only select if box has some area
                if (right - left > 2.0f && bottom - top > 2.0f)
                {
                    std::unordered_set<std::string> hits;

                    for (const auto& node : self->m_Model->Nodes)
                    {
                        // Convert node position to screen space
                        float nodeScreenX, nodeScreenY;
                        self->GraphToScreen(node.PositionX, node.PositionY, cx, cy, nodeScreenX, nodeScreenY);

                        // Calculate full node rectangle in screen space (not just center point)
                        float nodeW = GetNodeWidth(node) * self->m_Zoom;
                        float nodeH = self->GetNodeRectHeight(node) * self->m_Zoom;
                        float nodeLeft = nodeScreenX;
                        float nodeRight = nodeScreenX + nodeW;
                        float nodeTop = nodeScreenY;
                        float nodeBottom = nodeScreenY + nodeH;

                        // Check if node rectangle intersects with selection box (AABB intersection)
                        if (nodeRight >= left && nodeLeft <= right &&
                            nodeBottom >= top && nodeTop <= bottom)
                        {
                            hits.insert(node.Id);
                        }
                    }

                    // Marquee compose semantics (latched at mouse-down so
                    // releasing the modifier mid-drag doesn't change the
                    // gesture).
                    //   0 → replace base with hits
                    //   1 → add hits to base   (Shift)
                    //   2 → remove hits from base (Cmd/Ctrl)
                    if (self->m_MarqueeComposeMode == 1)
                    {
                        std::unordered_set<std::string> merged = self->m_MarqueeBaseSelection;
                        for (const auto& id : hits) merged.insert(id);
                        self->m_SelectedNodeIds = std::move(merged);
                    }
                    else if (self->m_MarqueeComposeMode == 2)
                    {
                        std::unordered_set<std::string> remaining = self->m_MarqueeBaseSelection;
                        for (const auto& id : hits) remaining.erase(id);
                        self->m_SelectedNodeIds = std::move(remaining);
                    }
                    else
                    {
                        self->m_SelectedNodeIds = std::move(hits);
                    }
                    if (!self->m_SelectedNodeIds.empty())
                        self->SetSelectedLinkId({});
                    if (self->m_OnSelectionChanged)
                        self->m_OnSelectionChanged(self->GetSelectedNodeId());
                }
                else
                {
                    // Box was too small — treat as click on empty space.
                    // With any modifier latched, preserve the pre-drag
                    // selection (matches Hierarchy where modifier+empty-click
                    // is a no-op).
                    if (self->m_MarqueeComposeMode != 0)
                    {
                        self->m_SelectedNodeIds = self->m_MarqueeBaseSelection;
                        if (self->m_OnSelectionChanged)
                            self->m_OnSelectionChanged(self->GetSelectedNodeId());
                    }
                    else
                    {
                        self->m_SelectedNodeIds.clear();
                        self->SetSelectedLinkId({});
                        if (self->m_OnSelectionChanged)
                            self->m_OnSelectionChanged(std::string());
                    }
                }
                self->m_MarqueeBaseSelection.clear();
                self->m_MarqueeComposeMode = 0;
                self->ApplyNodeVisualStates();
                self->MarkDirty(VisualDirty);
            }
            if (self->m_DraggingLink && self->m_Model)
            {
                const float cx = self->GetLayoutX();
                const float cy = self->GetLayoutY();
                float boundsX = 0.f, boundsY = 0.f, boundsW = 0.f, boundsH = 0.f;
                self->GetHitTestBounds(boundsX, boundsY, boundsW, boundsH);
                const bool droppedInsideCanvas = ev.X >= boundsX && ev.Y >= boundsY &&
                    ev.X <= boundsX + boundsW && ev.Y <= boundsY + boundsH;
                float gx, gy;
                self->ScreenToGraph(ev.X, ev.Y, cx, cy, 0.f, 0.f, gx, gy);
                std::string nodeId, portId;
                bool isOutput;
                self->FindHit(gx, gy, nodeId, portId, isOutput);
                bool connectedLink = false;
                if (self->m_DraggingLinkToExistingInput)
                {
                    if (!portId.empty() && isOutput)
                    {
                        Graph::Node* sourceNode = self->m_Model->FindNode(nodeId);
                        const Graph::Port* sourcePort = sourceNode ? self->FindPort(*sourceNode, portId) : nullptr;
                        const auto pending = self->PendingConnection();
                        if (sourceNode && sourcePort && pending && pending->Accepts(*sourceNode, *sourcePort))
                        {
                            std::string sourceNodeId = nodeId;
                            std::string sourcePortId = portId;
                            std::string targetNodeId = self->m_ConnectionTargetNodeId;
                            std::string targetPortId = self->m_ConnectionTargetPortId;
                            std::string oldLinkId = self->m_ReroutingLinkId;
                            auto rerouteLink = [self, sourceNodeId, sourcePortId, targetNodeId, targetPortId, oldLinkId]()
                            {
                                auto& links = self->m_Model->Links;
                                if (!oldLinkId.empty())
                                {
                                    links.erase(std::remove_if(links.begin(), links.end(),
                                        [&oldLinkId](const Graph::Edge& l) { return l.Id == oldLinkId; }), links.end());
                                }
                                Graph::Edge link;
                                link.Id = self->m_Model->GenerateLinkId();
                                link.SourceNodeId = sourceNodeId;
                                link.SourcePortId = sourcePortId;
                                link.TargetNodeId = targetNodeId;
                                link.TargetPortId = targetPortId;
                                links.push_back(link);
                                self->NotifyGraphChanged();
                            };
                            if (self->m_UndoScope)
                                self->m_UndoScope("Reroute Link", std::move(rerouteLink));
                            else
                                rerouteLink();
                            connectedLink = true;
                        }
                    }
                }
                else if (!portId.empty() && !isOutput)
                {
                    Graph::Node* targetNode = self->m_Model->FindNode(nodeId);
                    const Graph::Port* targetPort = targetNode ? self->FindPort(*targetNode, portId) : nullptr;
                    const auto pending = self->PendingConnection();
                    if (targetNode && targetPort && pending && pending->Accepts(*targetNode, *targetPort))
                    {
                        std::string nid = nodeId;
                        std::string pid = portId;
                        std::string oldLinkId = self->m_ReroutingLinkId;
                        auto addLink = [self, nid, pid, oldLinkId]()
                        {
                            auto& links = self->m_Model->Links;
                            if (!oldLinkId.empty())
                            {
                                links.erase(std::remove_if(links.begin(), links.end(),
                                    [&oldLinkId](const Graph::Edge& l) { return l.Id == oldLinkId; }), links.end());
                            }
                            Graph::Edge link;
                            link.Id = self->m_Model->GenerateLinkId();
                            link.SourceNodeId = self->m_ConnectionSourceNodeId;
                            link.SourcePortId = self->m_ConnectionSourcePortId;
                            link.TargetNodeId = nid;
                            link.TargetPortId = pid;
                            links.push_back(link);
                            self->NotifyGraphChanged();
                        };
                        if (self->m_UndoScope)
                            self->m_UndoScope(oldLinkId.empty() ? "Add Link" : "Reroute Link", std::move(addLink));
                        else
                            addLink();
                        connectedLink = true;
                    }
                }
                /* Click-to-connect: press+release on the origin port arms a sticky wire — the
                   pending connection stays on the cursor and the next click on a compatible port
                   completes it. Clicking anywhere without a port or pressing Escape cancels. */
                const bool releasedOnOriginPort = !portId.empty() && self->m_ReroutingLinkId.empty() &&
                    (self->m_DraggingLinkToExistingInput
                         ? (!isOutput && nodeId == self->m_ConnectionTargetNodeId &&
                            portId == self->m_ConnectionTargetPortId)
                         : (isOutput && nodeId == self->m_ConnectionSourceNodeId &&
                            portId == self->m_ConnectionSourcePortId));
                if (!connectedLink && releasedOnOriginPort)
                {
                    self->SetTooltip(self->m_DraggingLinkToExistingInput
                        ? "Click a compatible output port to connect\nClick elsewhere or press Escape to cancel."
                        : "Click a compatible input port to connect\nClick elsewhere or press Escape to cancel.");
                    self->MarkDirty(VisualDirty);
                    return;
                }
                if (!connectedLink && droppedInsideCanvas &&
                    nodeId.empty() && portId.empty() && self->m_OnRequestWireDropMenu)
                {
                    if (self->m_DraggingLinkToExistingInput)
                    {
                        self->m_OnRequestWireDropMenu(ev.X, ev.Y,
                                                      self->m_ConnectionTargetNodeId,
                                                      self->m_ConnectionTargetPortId,
                                                      true);
                    }
                    else
                    {
                        self->m_OnRequestWireDropMenu(ev.X, ev.Y,
                                                      self->m_ConnectionSourceNodeId,
                                                      self->m_ConnectionSourcePortId,
                                                      false);
                    }
                }
            }
            if (self->m_DraggingNode)
            {
                /* Snap to grid on release even when the grid is hidden. */
                if (self->m_Model)
                {
                    for (auto& kv : self->m_DragNodeStarts)
                    {
                        Graph::Node* n = self->m_Model->FindNode(kv.first);
                        if (n)
                        {
                            SnapGraphPosition(n->PositionX, n->PositionY);
                            kv.second = Mathematics::Vector2(n->PositionX, n->PositionY);
                        }
                    }
                    /* The release point is the starting point; a drop on top
                       of another node is pushed to the nearest free cell. */
                    std::vector<std::string> movedNodeIds;
                    movedNodeIds.reserve(self->m_DragNodeStarts.size());
                    for (const auto& kv : self->m_DragNodeStarts)
                        movedNodeIds.push_back(kv.first);
                    self->ResolveNodeOverlaps(movedNodeIds);
                    if (self->m_NodePool)
                    {
                        for (const auto& kv : self->m_DragNodeStarts)
                        {
                            const Graph::Node* n = self->m_Model->FindNode(kv.first);
                            GraphPortedNode* slot = self->m_NodePool->FindByModelId(kv.first);
                            if (n && slot)
                                ApplyNodeWidgetRect(*slot, *n, self->m_Zoom,
                                                    self->GetNodeRectHeight(*n));
                        }
                    }
                    self->MarkDirty(VisualDirty);
                }
                /* Paint the snap this frame. Undo capture, rebind, and any
                   compile run after this Update so they cannot stall the jump. */
                self->PostSafeAction([self]()
                {
                    if (self->m_OnNodeDragEnded)
                        self->m_OnNodeDragEnded();
                    if (self->m_OnGraphChanged)
                        self->m_OnGraphChanged();
                });
            }
            self->m_DraggingPan = false;
            self->m_DraggingNode = false;
            self->m_DraggingLink = false;
            self->m_DraggingLinkToExistingInput = false;
            self->m_ReroutingLinkId.clear();
            if (!self->m_WireDropMenuOpen)
            {
                self->m_ConnectionTargetNodeId.clear();
                self->m_ConnectionTargetPortId.clear();
            }
            self->m_DraggingSelectionBox = false;
            self->MarkDirty(VisualDirty);
        }
        else if ((ev.Button == Input::kMouseButton_Right || ev.Button == Input::kMouseButton_Middle) &&
                 self->m_DraggingPan)
        {
            /* Armed at the canvas's own press (m_DraggingPan): a release whose
               press a child claimed — fields accept button 1 as primary —
               opens no menu and removes no link. */
            const bool isRight = ev.Button == Input::kMouseButton_Right;
            /* The travel test also runs HERE, against the release point: move
               events coalesce (this editor runs 10-30 fps in Debug), so a fast
               drag can reach the release with the move handler having never
               seen a sample past the threshold. A release that lands away from
               the press is a drag, whatever the samples in between said. */
            {
                constexpr float kRightDragThresholdPx = 4.f;
                const float stepDx = ev.X - self->m_RightMouseLastX;
                const float stepDy = ev.Y - self->m_RightMouseLastY;
                const float travel =
                    self->m_RightMouseTravelPx + std::sqrt(stepDx * stepDx + stepDy * stepDy);
                const float totalDx = ev.X - self->m_RightMouseDownX;
                const float totalDy = ev.Y - self->m_RightMouseDownY;
                if (travel >= kRightDragThresholdPx ||
                    totalDx * totalDx + totalDy * totalDy >=
                        kRightDragThresholdPx * kRightDragThresholdPx)
                    self->m_RightMouseDragged = true;
            }
            const bool requestMenu = isRight && !self->m_RightMouseDragged &&
                self->m_PendingLinkRemoveId.empty() &&
                static_cast<bool>(self->m_OnRequestContextMenu);
            if (isRight && !self->m_PendingLinkRemoveId.empty() && !self->m_RightMouseDragged && self->m_Model)
            {
                /* Hide now, mutate after dispatch: erasing the link here runs
                   graph-changed (and its rebinds) mid-event. */
                self->HideAndDeferDeleteLink(self->m_PendingLinkRemoveId, "Remove Link");
                self->m_HoveredLinkId.clear();
                self->m_HoveredNodeId.clear();
                self->m_HoveredPortId.clear();
                self->SetTooltip({});
            }

            if (requestMenu)
            {
                const float cx = self->GetLayoutX();
                const float cy = self->GetLayoutY();
                float gx = 0.f, gy = 0.f;
                self->ScreenToGraph(ev.X, ev.Y, cx, cy, 0.f, 0.f, gx, gy);
                std::string hitNodeId;
                std::string hitPortId;
                bool isOutput = false;
                self->FindHit(gx, gy, hitNodeId, hitPortId, isOutput);
                self->m_OnRequestContextMenu(ev.X, ev.Y, !hitNodeId.empty(), hitNodeId);
            }

            // Right mouse button up - stop panning / pending wire removal.
            self->m_DraggingPan = false;
            self->m_PendingLinkRemoveId.clear();
            self->m_RightMouseDragged = false;
            self->MarkDirty(VisualDirty);
            ev.Stop();
        }
    });
    m_Tokens->mouseMove = RegisterEventHandler(kEventMouseMove, [self](UIEvent& ev)
    {
        float cx = self->GetLayoutX();
        float cy = self->GetLayoutY();
        if (!self->m_PendingNodeDragId.empty())
        {
            float dx = ev.X - self->m_DragStartX;
            float dy = ev.Y - self->m_DragStartY;
            if (dx * dx + dy * dy >= self->kNodeDragThresholdPx * self->kNodeDragThresholdPx)
            {
                std::string nodeId = self->m_PendingNodeDragId;
                self->m_PendingNodeDragId.clear();
                self->m_PendingNodeDoubleClick = false;
                if (self->m_SelectedNodeIds.count(nodeId) == 0)
                {
                    self->m_SelectedNodeIds.clear();
                    self->m_SelectedNodeIds.insert(nodeId);
                }
                if (self->m_OnSelectionChanged)
                    self->m_OnSelectionChanged(self->GetSelectedNodeId());
                self->m_DraggingNode = true;
                if (self->m_OnNodeDragStarted)
                    self->m_OnNodeDragStarted();
                HideTooltip(self);
                self->m_DragStartX = ev.X;
                self->m_DragStartY = ev.Y;
                self->m_DragNodeStarts.clear();
                if (self->m_Model)
                {
                    for (const std::string& sid : self->m_SelectedNodeIds)
                    {
                        Graph::Node* n = self->m_Model->FindNode(sid);
                        if (n)
                            self->m_DragNodeStarts[sid] =
                                Mathematics::Vector2(n->PositionX, n->PositionY);
                    }
                }
                self->ApplyNodeVisualStates();
                self->MarkDirty(VisualDirty);
            }
        }
        if (self->m_DraggingPan)
        {
            /* A right gesture that travels counts as a drag wherever it started;
               the release only opens the menu (or removes a wire) for a still
               click within the threshold. */
            constexpr float kRightDragThresholdPx = 4.f;
            const float stepDx = ev.X - self->m_RightMouseLastX;
            const float stepDy = ev.Y - self->m_RightMouseLastY;
            self->m_RightMouseTravelPx += std::sqrt(stepDx * stepDx + stepDy * stepDy);
            self->m_RightMouseLastX = ev.X;
            self->m_RightMouseLastY = ev.Y;
            const float totalDx = ev.X - self->m_RightMouseDownX;
            const float totalDy = ev.Y - self->m_RightMouseDownY;
            const bool withinClickThreshold =
                totalDx * totalDx + totalDy * totalDy < kRightDragThresholdPx * kRightDragThresholdPx &&
                self->m_RightMouseTravelPx < kRightDragThresholdPx;
            if (!withinClickThreshold)
                self->m_RightMouseDragged = true;
            if (!self->m_PendingLinkRemoveId.empty() && !self->m_RightMouseDragged)
            {
                /* Press landed on a wire and hasn't left the click threshold:
                   hold the pan so a wire click stays a click. */
                ev.Stop();
                return;
            }

            float dx = ev.X - self->m_DragStartX;
            float dy = ev.Y - self->m_DragStartY;
            self->m_DragStartX = ev.X;
            self->m_DragStartY = ev.Y;
            self->m_PanX += dx;
            self->m_PanY += dy;
            self->SnapPanToDevicePixels();
            if (self->m_Model)
            {
                self->m_Model->Viewport.PanX = self->m_PanX;
                self->m_Model->Viewport.PanY = self->m_PanY;
            }
            self->SyncNodeLayerTransform();
            self->MarkDirty(VisualDirty);
            ev.Stop();
        }
        else if (self->m_DraggingSelectionBox)
        {
            // Update selection box current position and highlight intersected nodes
            self->m_SelectionBoxCurrentX = ev.X;
            self->m_SelectionBoxCurrentY = ev.Y;
            
            // Real-time highlighting: find nodes intersecting the current selection box
            if (self->m_Model)
            {
                // cx/cy are already in scope from the outer mouseMove handler; refresh in
                // case the canvas origin changed during the gesture.
                cx = self->GetLayoutX();
                cy = self->GetLayoutY();

                float left = std::min(self->m_SelectionBoxStartX, self->m_SelectionBoxCurrentX);
                float right = std::max(self->m_SelectionBoxStartX, self->m_SelectionBoxCurrentX);
                float top = std::min(self->m_SelectionBoxStartY, self->m_SelectionBoxCurrentY);
                float bottom = std::max(self->m_SelectionBoxStartY, self->m_SelectionBoxCurrentY);
                
                // Only compute intersection if box has some area
                if (right - left > 2.0f && bottom - top > 2.0f)
                {
                    // Compute the hits inside the current marquee box.
                    std::unordered_set<std::string> hits;
                    for (const auto& node : self->m_Model->Nodes)
                    {
                        float nodeScreenX, nodeScreenY;
                        self->GraphToScreen(node.PositionX, node.PositionY, cx, cy, nodeScreenX, nodeScreenY);

                        // Calculate full node rectangle in screen space
                        float nodeW = GetNodeWidth(node) * self->m_Zoom;
                        float nodeH = self->GetNodeRectHeight(node) * self->m_Zoom;
                        float nodeLeft = nodeScreenX;
                        float nodeRight = nodeScreenX + nodeW;
                        float nodeTop = nodeScreenY;
                        float nodeBottom = nodeScreenY + nodeH;

                        // Check if node rectangle intersects with selection box
                        if (nodeRight >= left && nodeLeft <= right &&
                            nodeBottom >= top && nodeTop <= bottom)
                        {
                            hits.insert(node.Id);
                        }
                    }

                    // Compose against the snapshot per the latched mode so
                    // the live preview matches what the user will get.
                    std::unordered_set<std::string> previewSelection;
                    if (self->m_MarqueeComposeMode == 1) // add
                    {
                        previewSelection = self->m_MarqueeBaseSelection;
                        for (const auto& id : hits) previewSelection.insert(id);
                    }
                    else if (self->m_MarqueeComposeMode == 2) // remove
                    {
                        previewSelection = self->m_MarqueeBaseSelection;
                        for (const auto& id : hits) previewSelection.erase(id);
                    }
                    else // replace
                    {
                        previewSelection = std::move(hits);
                    }

                    // Update selection preview (will be committed on mouse up)
                    self->m_SelectedNodeIds = std::move(previewSelection);
                    self->ApplyNodeVisualStates();
                    if (self->m_OnSelectionChanged)
                        self->m_OnSelectionChanged(self->GetSelectedNodeId());
                }
            }
            
            self->MarkDirty(VisualDirty);
            ev.Stop();
        }
        else if (self->m_DraggingNode && self->m_Model)
        {
            const float dx = (ev.X - self->m_DragStartX) / self->m_Zoom;
            const float dy = (ev.Y - self->m_DragStartY) / self->m_Zoom;
            for (const auto& kv : self->m_DragNodeStarts)
            {
                Graph::Node* n = self->m_Model->FindNode(kv.first);
                if (!n)
                    continue;
                n->PositionX = kv.second.x + dx;
                n->PositionY = kv.second.y + dy;
            }
            if (self->m_NodePool)
            {
                for (const auto& dragged : self->m_DragNodeStarts)
                {
                    const Graph::Node* n = self->m_Model->FindNode(dragged.first);
                    GraphPortedNode* slot = self->m_NodePool->FindByModelId(dragged.first);
                    if (n && slot)
                        ApplyNodeWidgetRect(*slot, *n, self->m_Zoom,
                                            self->GetNodeRectHeight(*n));
                }
            }
            self->MarkDirty(VisualDirty);
            ev.Stop();
        }
        else if (self->m_DraggingLink)
        {
            float gx, gy;
            self->ScreenToGraph(ev.X, ev.Y, cx, cy, 0.f, 0.f, gx, gy);
            self->m_ConnectionEndGraphX = gx;
            self->m_ConnectionEndGraphY = gy;
            std::string nodeId, portId;
            bool isOutput = false;
            self->FindHit(gx, gy, nodeId, portId, isOutput);
            if (nodeId.empty() || portId.empty())
            {
                self->SetTooltip(self->m_DraggingLinkToExistingInput
                    ? "Drag wire\nRelease on a compatible output connection."
                    : "Drag wire\nRelease on a compatible input connection.");
            }
            self->MarkDirty(VisualDirty);
            ev.Stop();
        }
        else
        {
            float gx, gy;
            self->ScreenToGraph(ev.X, ev.Y, cx, cy, 0.f, 0.f, gx, gy);
            std::string nodeId, portId;
            bool isOutput;
            self->FindHit(gx, gy, nodeId, portId, isOutput);
            bool portHoverChanged = (nodeId != self->m_HoveredNodeId || portId != self->m_HoveredPortId);
            bool linkHoverChanged = false;
            if (!portId.empty())
            {
                self->m_HoveredNodeId = nodeId;
                self->m_HoveredPortId = portId;
                linkHoverChanged = !self->m_HoveredLinkId.empty();
                self->m_HoveredLinkId.clear();
            }
            else if (!nodeId.empty())
            {
                self->m_HoveredNodeId.clear();
                self->m_HoveredPortId.clear();
                linkHoverChanged = !self->m_HoveredLinkId.empty();
                self->m_HoveredLinkId.clear();
            }
            else
            {
                self->m_HoveredNodeId.clear();
                self->m_HoveredPortId.clear();
                std::string linkId = self->FindHitLink(ev.X, ev.Y, cx, cy);
                linkHoverChanged = (linkId != self->m_HoveredLinkId);
                self->m_HoveredLinkId = linkId;
                if (!linkId.empty() && self->m_Model)
                {
                    const Graph::Edge* hoveredLink = nullptr;
                    for (const auto& link : self->m_Model->Links)
                    {
                        if (link.Id == linkId)
                        {
                            hoveredLink = &link;
                            break;
                        }
                    }
                    self->SetTooltip(hoveredLink ? self->BuildLinkTooltip(*hoveredLink) : std::string());
                }
                else
                {
                    self->SetTooltip({});
                }
            }
            if (portHoverChanged || linkHoverChanged)
                self->MarkDirty(VisualDirty);
        }
    });
    m_Tokens->mouseLeave = RegisterEventHandler(kEventMouseLeave, [self](UIEvent& ev)
    {
        const float originX = self->GetLayoutX();
        const float originY = self->GetLayoutY();
        const float width = self->GetLayoutWidth();
        const float height = self->GetLayoutHeight();
        if (ev.X >= originX && ev.X < originX + width && ev.Y >= originY && ev.Y < originY + height)
            return;
        self->m_HoveredLinkId.clear();
        self->m_HoveredNodeId.clear();
        self->m_HoveredPortId.clear();
        self->SetTooltip({});
        self->MarkDirty(VisualDirty);
    });
    m_Tokens->scroll = RegisterEventHandler(kEventScroll, [self](UIEvent& ev)
    {
        HideTooltip(self);
        const float cx = self->GetLayoutX();
        const float cy = self->GetLayoutY();
        float localX = ev.X - cx;
        float localY = ev.Y - cy;
        float gx = (localX - self->m_PanX) / self->m_Zoom;
        float gy = (localY - self->m_PanY) / self->m_Zoom;
        float zoomFactor = ev.ScrollY > 0 ? 0.9f : 1.1f; /* reversed: scroll up = zoom out, scroll down = zoom in */
        float newZoom = std::max(GraphCanvas::kMinZoom, std::min(GraphCanvas::kMaxZoom, self->m_Zoom * zoomFactor));
        self->m_PanX = localX - gx * newZoom;
        self->m_PanY = localY - gy * newZoom;
        self->m_Zoom = newZoom;
        self->SnapPanToDevicePixels();
        if (self->m_Model)
        {
            self->m_Model->Viewport.PanX = self->m_PanX;
            self->m_Model->Viewport.PanY = self->m_PanY;
            self->m_Model->Viewport.Zoom = self->m_Zoom;
        }
        self->SyncNodeLayerTransform();
        self->MarkDirty(VisualDirty);
        ev.Stop();
    });
    m_Tokens->keyDown = RegisterEventHandler(kEventKeyDown, [self](UIEvent& ev)
    {
        if (!self->m_Model)
            return;
        if (ev.Key == Input::kKeyCode_Escape && self->m_DraggingLink)
        {
            self->m_DraggingLink = false;
            self->m_DraggingLinkToExistingInput = false;
            self->m_ReroutingLinkId.clear();
            self->m_ConnectionSourceNodeId.clear();
            self->m_ConnectionSourcePortId.clear();
            self->m_ConnectionTargetNodeId.clear();
            self->m_ConnectionTargetPortId.clear();
            self->SetTooltip({});
            self->MarkDirty(VisualDirty);
            return;
        }
        if (Editor::MatchesCatalogShortcut("Node Graph", "Delete Nodes", ev.Key, ev.Mods))
        {
            if (GraphCanvasShouldIgnoreEditKeys(ev.Target))
                return;
            if (self->m_SelectedNodeIds.empty())
            {
                self->DeleteSelectedLink();
                ev.Stop();
                return;
            }
            self->DeleteSelectedNodes();
            ev.Stop();
        }
        else if (Editor::MatchesCatalogShortcut("Node Graph", "Copy Nodes", ev.Key, ev.Mods))
        {
            if (GraphCanvasShouldIgnoreEditKeys(ev.Target))
                return;
            if (self->m_SelectedNodeIds.empty())
                return;
            Graph::Model subset;
            subset.KindId = self->m_Model->KindId;
            for (const auto& n : self->m_Model->Nodes)
            {
                if (self->m_SelectedNodeIds.count(n.Id))
                    subset.Nodes.push_back(n);
            }
            for (const auto& l : self->m_Model->Links)
            {
                if (self->m_SelectedNodeIds.count(l.SourceNodeId) && self->m_SelectedNodeIds.count(l.TargetNodeId))
                    subset.Links.push_back(l);
            }
            self->m_CopyPasteClipboard = Graph::ToJson(subset);
            ev.Stop();
        }
        else if (Editor::MatchesCatalogShortcut("Node Graph", "Paste Nodes", ev.Key, ev.Mods))
        {
            if (GraphCanvasShouldIgnoreEditKeys(ev.Target))
                return;
            if (self->m_CopyPasteClipboard.empty())
                return;
            Graph::Model temp;
            if (!Graph::FromJson(self->m_CopyPasteClipboard, temp))
                return;
            auto doPaste = [self, temp = std::move(temp)]() mutable
            {
                constexpr float kPasteOffsetX = 40.f;
                constexpr float kPasteOffsetY = 40.f;
                std::unordered_map<std::string, std::string> oldToNewId;
                std::vector<std::string> pastedNodeIds;
                for (Graph::Node& n : temp.Nodes)
                {
                    std::string newId = self->m_Model->GenerateNodeId();
                    oldToNewId[n.Id] = newId;
                    pastedNodeIds.push_back(newId);
                    n.Id = newId;
                    n.PositionX += kPasteOffsetX;
                    n.PositionY += kPasteOffsetY;
                    self->m_Model->Nodes.push_back(std::move(n));
                }
                self->ResolveNodeOverlaps(pastedNodeIds);
                self->m_SelectedNodeIds.clear();
                for (const auto& l : temp.Links)
                {
                    auto sit = oldToNewId.find(l.SourceNodeId);
                    auto tit = oldToNewId.find(l.TargetNodeId);
                    if (sit == oldToNewId.end() || tit == oldToNewId.end())
                        continue;
                    Graph::Edge newLink;
                    newLink.Id = self->m_Model->GenerateLinkId();
                    newLink.SourceNodeId = sit->second;
                    newLink.SourcePortId = l.SourcePortId;
                    newLink.TargetNodeId = tit->second;
                    newLink.TargetPortId = l.TargetPortId;
                    self->m_Model->Links.push_back(std::move(newLink));
                }
                for (const auto& kv : oldToNewId)
                    self->m_SelectedNodeIds.insert(kv.second);
                self->SetSelectedLinkId({});
                self->NotifyGraphChanged();
                if (self->m_OnSelectionChanged)
                    self->m_OnSelectionChanged(self->GetSelectedNodeId());
            };
            if (self->m_UndoScope)
                self->m_UndoScope("Paste", std::move(doPaste));
            else
                doPaste();
            self->MarkDirty(VisualDirty);
            ev.Stop();
        }
    });
}

void GraphCanvas::UnregisterEventHandlers()
{
    if (m_Tokens)
    {
        UnregisterEventHandler(m_Tokens->mouseDown);
        UnregisterEventHandler(m_Tokens->mouseUp);
        UnregisterEventHandler(m_Tokens->mouseMove);
        UnregisterEventHandler(m_Tokens->mouseLeave);
        UnregisterEventHandler(m_Tokens->scroll);
        UnregisterEventHandler(m_Tokens->keyDown);
    }
}

// ---------------------------------------------------------------------------
// Primitive-based rendering (SDF pipeline)
// ---------------------------------------------------------------------------

/* Derived from the drag state rather than pushed from the twenty-odd places that
   flip it: a refresh per state change, driven from the frame that paints it. */
void GraphCanvas::AutoConnectInputPort(const std::string& nodeId, const std::string& portId,
                                      float gx, float gy)
{
    if (!m_Model)
        return;
    const Graph::Node* tgtNode = m_Model->FindNode(nodeId);
    if (!tgtNode)
        return;
    /* An input already carrying a link keeps it: replacing silently on a
       double-click would drop a connection the user cannot see they had. */
    for (const Graph::Edge& link : m_Model->Links)
    {
        if (IsLinkHiddenPendingDelete(link))
            continue;
        if (link.TargetNodeId == nodeId && link.TargetPortId == portId)
            return;
    }

    const size_t tgtPortIdx = FindPortIndex(*tgtNode, portId);
    float tx = 0.f, ty = 0.f;
    GetPortCenterInGraph(*tgtNode, tgtPortIdx, tx, ty);
    (void)gx;
    (void)gy;

    m_ConnectionTargetNodeId = nodeId;
    m_ConnectionTargetPortId = portId;
    m_DraggingLink = true;
    m_DraggingLinkToExistingInput = true;

    struct PortCandidate { std::string NodeId; std::string PortId; float DistSq; };
    std::vector<PortCandidate> candidates;
    const auto pending = PendingConnection();
    for (const Graph::Node& node : m_Model->Nodes)
    {
        if (node.Id == nodeId)
            continue; // never auto-connect a node to itself
        for (size_t pi = 0; pi < node.Ports.size(); ++pi)
        {
            const Graph::Port& port = node.Ports[pi];
            if (port.Direction != Graph::PortDirection::Out)
                continue;
            if (!pending || !pending->Accepts(node, port))
                continue;
            float sx = 0.f, sy = 0.f;
            GetPortCenterInGraph(node, pi, sx, sy);
            const float dx = sx - tx;
            const float dy = sy - ty;
            candidates.push_back({node.Id, port.Id, dx * dx + dy * dy});
        }
    }

    m_DraggingLink = false;
    m_DraggingLinkToExistingInput = false;
    m_ConnectionTargetNodeId.clear();
    m_ConnectionTargetPortId.clear();
    if (candidates.empty())
        return;

    const auto best = std::min_element(candidates.begin(), candidates.end(),
        [](const PortCandidate& a, const PortCandidate& b) { return a.DistSq < b.DistSq; });
    auto addLink = [this, srcNodeId = best->NodeId, srcPortId = best->PortId, nodeId, portId]()
    {
        Graph::Edge link;
        link.Id = m_Model->GenerateLinkId();
        link.SourceNodeId = srcNodeId;
        link.SourcePortId = srcPortId;
        link.TargetNodeId = nodeId;
        link.TargetPortId = portId;
        m_Model->Links.push_back(link);
        NotifyGraphChanged();
    };
    if (m_UndoScope)
        m_UndoScope("Add Link", std::move(addLink));
    else
        addLink();
    MarkDirty(VisualDirty);
}

void GraphCanvas::SetWireDropMenuOpen(bool open)
{
    if (m_WireDropMenuOpen == open)
        return;
    m_WireDropMenuOpen = open;
    if (!open)
    {
        m_ConnectionSourceNodeId.clear();
        m_ConnectionSourcePortId.clear();
        m_ConnectionTargetNodeId.clear();
        m_ConnectionTargetPortId.clear();
    }
    MarkDirty(VisualDirty);
}

void GraphCanvas::RefreshCompatiblePortHighlights()
{
    if (m_HighlightDragging == m_DraggingLink &&
        m_HighlightToExistingInput == m_DraggingLinkToExistingInput &&
        m_HighlightSourceNodeId == m_ConnectionSourceNodeId &&
        m_HighlightSourcePortId == m_ConnectionSourcePortId)
        return;
    m_HighlightDragging = m_DraggingLink;
    m_HighlightToExistingInput = m_DraggingLinkToExistingInput;
    m_HighlightSourceNodeId = m_ConnectionSourceNodeId;
    m_HighlightSourcePortId = m_ConnectionSourcePortId;
    UpdateCompatiblePortHighlights();
}

void GraphCanvas::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                                             const ResolvedStyle& /*style*/,
                                             float x, float y, float W, float H)
{
    RefreshCompatiblePortHighlights();

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

    /* UIManager passes x,y,W,H in physical px (layout * contentScale). Pan/zoom and
       sizing constants here are logical CSS px (pan from mouse events, which are logical).
       Do all math in logical and scale at emit time so child labels (whose rects are also
       logical) remain in sync with nodes at any contentScale. */
    const float cs = std::max(0.01f, ctx.ContentScale);
    float canvasX = x / cs, canvasY = y / cs, canvasW = W / cs, canvasH = H / cs;

    auto emitRect = [&](float rx, float ry, float rw, float rh, uint32_t color)
    {
        ctx.Emit(MakeRect(rx * cs, ry * cs, rw * cs, rh * cs, color));
    };
    auto emitRectRounded = [&](float rx, float ry, float rw, float rh, uint32_t color, float r)
    {
        ctx.Emit(MakeRect(rx * cs, ry * cs, rw * cs, rh * cs, color,
                          r * cs, r * cs, r * cs, r * cs));
    };
    auto emitLine = [&](float x0, float y0, float x1, float y1, float thickness, uint32_t color)
    {
        ctx.Emit(MakeLine(x0 * cs, y0 * cs, x1 * cs, y1 * cs, thickness * cs, color));
    };
    const float cornerRadiusPx = ConnectionCornerRadiusPx(m_Zoom);
    const bool roundedCorners = GetConnectionRoundedCorners();
    auto emitConnectionRoute = [&](const std::vector<Mathematics::Vector2>& route, float thickness,
                                   uint32_t color)
    {
        std::vector<float> xs, ys;
        xs.reserve(route.size());
        ys.reserve(route.size());
        for (const Mathematics::Vector2& point : route)
        {
            xs.push_back(point.x);
            ys.push_back(point.y);
        }
        if (xs.size() >= 2)
        {
            size_t w = 1;
            for (size_t i = 1; i < xs.size(); ++i)
            {
                if (xs[i] != xs[w - 1] || ys[i] != ys[w - 1])
                {
                    xs[w] = xs[i];
                    ys[w] = ys[i];
                    ++w;
                }
            }
            xs.resize(w);
            ys.resize(w);
        }
        if (!roundedCorners || xs.size() < 2)
        {
            for (size_t i = 0; i + 1 < xs.size(); ++i)
                emitLine(xs[i], ys[i], xs[i + 1], ys[i + 1], thickness, color);
            return;
        }
        // Render the straight runs as degenerate (collinear) beziers so every
        // part of the route goes through the exact same Bezier SDF as the
        // rounded corners. Line-mode and Bezier-mode antialiasing differ
        // slightly, which made the corners read fatter than the segments.
        // Square caps so each straight run and corner arc abuts its neighbour
        // exactly: they share an endpoint and tangent, so flat caps there tile
        // seamlessly instead of overlapping round caps (whose blended AA
        // fringes bump the seam).
        auto emitStraightBezier = [&](float x0, float y0, float x1, float y1)
        {
            const float cx0 = x0 + (x1 - x0) * (1.0f / 3.0f);
            const float cy0 = y0 + (y1 - y0) * (1.0f / 3.0f);
            const float cx1 = x0 + (x1 - x0) * (2.0f / 3.0f);
            const float cy1 = y0 + (y1 - y0) * (2.0f / 3.0f);
            ctx.Emit(MakeBezier(x0 * cs, y0 * cs, cx0 * cs, cy0 * cs,
                                cx1 * cs, cy1 * cs, x1 * cs, y1 * cs,
                                thickness * cs, color, kNoClip, /*squareCap=*/true));
        };
        EmitRoundedPolyline(
            xs, ys, thickness, color, cornerRadiusPx,
            emitStraightBezier,
            [&](float x0, float y0, float cx0, float cy0, float cx1, float cy1, float x1, float y1)
            {
                ctx.Emit(MakeBezier(x0 * cs, y0 * cs, cx0 * cs, cy0 * cs,
                                    cx1 * cs, cy1 * cs, x1 * cs, y1 * cs,
                                    thickness * cs, color, kNoClip, /*squareCap=*/true));
            });
    };

    // Background
    emitRect(canvasX, canvasY, canvasW, canvasH,
             PackFromARGB(NodeColorSettings::GetCanvasColorArgb()));

    // Grid
    if (m_ShowGrid && canvasW > 0.f && canvasH > 0.f)
    {
        const float gridSize = kGridSizeGraph;
        float gLeft   = (-m_PanX) / m_Zoom;
        float gRight  = (canvasW - m_PanX) / m_Zoom;
        float gTop    = (-m_PanY) / m_Zoom;
        float gBottom = (canvasH - m_PanY) / m_Zoom;

        int i0 = static_cast<int>(std::floor(gLeft  / gridSize));
        int i1 = static_cast<int>(std::ceil(gRight  / gridSize));
        int j0 = static_cast<int>(std::floor(gTop   / gridSize));
        int j1 = static_cast<int>(std::ceil(gBottom / gridSize));

        const uint32_t gridColor = PackFromARGB(0xFF383838u);
        constexpr float kGridThick = 1.f;

        auto gridToScreenX = [&](float gx) { return canvasX + m_PanX + gx * m_Zoom; };
        auto gridToScreenY = [&](float gy) { return canvasY + m_PanY + gy * m_Zoom; };

        for (int i = i0; i <= i1; ++i)
        {
            float sx = gridToScreenX(static_cast<float>(i) * gridSize);
            float sy0 = gridToScreenY(gTop);
            float sy1 = gridToScreenY(gBottom);
            emitRect(sx - 0.5f, std::min(sy0, sy1), kGridThick,
                     std::abs(sy1 - sy0) + kGridThick, gridColor);
        }
        for (int j = j0; j <= j1; ++j)
        {
            float sy = gridToScreenY(static_cast<float>(j) * gridSize);
            float sx0 = gridToScreenX(gLeft);
            float sx1 = gridToScreenX(gRight);
            emitRect(std::min(sx0, sx1), sy - 0.5f,
                     std::abs(sx1 - sx0) + kGridThick, kGridThick, gridColor);
        }
    }

    if (!m_Model)
        return;

    auto graphToScreen = [&](float gx, float gy, float& sx, float& sy)
    {
        sx = canvasX + m_PanX + gx * m_Zoom;
        sy = canvasY + m_PanY + gy * m_Zoom;
    };
    auto portColorPacked = [](const Graph::Port* port) -> uint32_t
    {
        return PackFromARGB(PortTypeColorArgb(port ? port->DataType : std::string()));
    };

    // --------------- Connections ---------------
    constexpr float kLineThickness   = 2.2f;
    const float trackOffsetPx = kWireSeparationGraph * m_Zoom; /* this preview is in screen px */
    const uint32_t hoveredColor = PackFromARGB(0xFF3A8FFFu);
    const uint32_t selectedColor = PackFromARGB(0xFFE8C547u);

    auto emitBezier = [&](float sx0, float sy0, float sx1, float sy1,
                          float thickness, uint32_t color)
    {
        float cx0 = 0.f, cy0 = 0.f, cx1 = 0.f, cy1 = 0.f;
        ComputeConnectionBezierControls(sx0, sy0, sx1, sy1, m_Zoom, cx0, cy0, cx1, cy1);
        ctx.Emit(MakeBezier(sx0 * cs, sy0 * cs, cx0 * cs, cy0 * cs,
                            cx1 * cs, cy1 * cs, sx1 * cs, sy1 * cs,
                            thickness * cs, color));
    };

    auto emitStraightElbow = [&](float sx0, float sy0, float sx1, float sy1,
                                 float thickness, uint32_t color, int trackIdx, int trackCount)
    {
        const float stubPx = ConnectionStubOffsetPx(m_Zoom);
        float dx = sx1 - sx0;
        float a0, b0;
        if (dx >= 0.f)
        {
            float gap = std::max(0.f, dx * 0.4f);
            a0 = sx0 + std::min(stubPx, gap);
            b0 = sx1 - std::min(stubPx, gap);
        }
        else
        {
            float gap = std::max(0.f, (-dx) * 0.4f);
            a0 = sx0 - std::min(stubPx, gap);
            b0 = sx1 + std::min(stubPx, gap);
        }
        float ex = b0;
        if (trackCount > 1)
        {
            const float approachPx = std::max(trackOffsetPx,
                static_cast<float>(trackIdx + 1) * trackOffsetPx);
            if (dx >= 0.f)
                ex = b0 - approachPx;
            else
            {
                float dir = (a0 >= b0) ? 1.f : -1.f;
                ex = b0 + dir * approachPx;
            }
            ex = std::max(std::min(a0, b0), std::min(ex, std::max(a0, b0)));
        }
        const std::vector<Mathematics::Vector2> preview{{sx0, sy0}, {a0, sy0},  {ex, sy0},
                                                        {ex, sy1},  {b0, sy1},  {sx1, sy1}};
        emitConnectionRoute(preview, thickness, color);
    };
    auto pointOnPolyline = [](const std::vector<Mathematics::Vector2>& route, float t, float& outX,
                              float& outY)
    {
        std::vector<float> xs, ys;
        xs.reserve(route.size());
        ys.reserve(route.size());
        for (const Mathematics::Vector2& point : route)
        {
            xs.push_back(point.x);
            ys.push_back(point.y);
        }
        if (xs.empty() || xs.size() != ys.size())
            return false;
        if (xs.size() == 1)
        {
            outX = xs[0];
            outY = ys[0];
            return true;
        }

        float total = 0.0f;
        for (size_t i = 0; i + 1 < xs.size(); ++i)
            total += std::hypot(xs[i + 1] - xs[i], ys[i + 1] - ys[i]);
        if (total <= 0.001f)
        {
            outX = xs.back();
            outY = ys.back();
            return true;
        }

        float remaining = std::clamp(t, 0.0f, 1.0f) * total;
        for (size_t i = 0; i + 1 < xs.size(); ++i)
        {
            const float dx = xs[i + 1] - xs[i];
            const float dy = ys[i + 1] - ys[i];
            const float segment = std::hypot(dx, dy);
            if (segment <= 0.001f)
                continue;
            if (remaining <= segment)
            {
                const float u = remaining / segment;
                outX = xs[i] + dx * u;
                outY = ys[i] + dy * u;
                return true;
            }
            remaining -= segment;
        }
        outX = xs.back();
        outY = ys.back();
        return true;
    };
    auto pointOnBezier = [&](float sx0, float sy0, float sx1, float sy1, float t,
                             float& outX, float& outY)
    {
        float cx0 = 0.0f;
        float cy0 = 0.0f;
        float cx1 = 0.0f;
        float cy1 = 0.0f;
        ComputeConnectionBezierControls(sx0, sy0, sx1, sy1, m_Zoom, cx0, cy0, cx1, cy1);
        t = std::clamp(t, 0.0f, 1.0f);
        const float u = 1.0f - t;
        const float u2 = u * u;
        const float u3 = u2 * u;
        const float t2 = t * t;
        const float t3 = t2 * t;
        outX = u3 * sx0 + 3.0f * u2 * t * cx0 + 3.0f * u * t2 * cx1 + t3 * sx1;
        outY = u3 * sy0 + 3.0f * u2 * t * cy0 + 3.0f * u * t2 * cy1 + t3 * sy1;
    };

    // Per-link screen coords for track-offset computation
    const size_t numLinks = m_Model->Links.size();
    std::vector<float> segSx0(numLinks), segSy0(numLinks), segSx1(numLinks), segSy1(numLinks);

    for (size_t li = 0; li < numLinks; ++li)
    {
        const auto& link = m_Model->Links[li];
        if (IsLinkHiddenPendingDelete(link) ||
            (!m_ReroutingLinkId.empty() && link.Id == m_ReroutingLinkId))
        {
            segSx0[li] = segSy0[li] = segSx1[li] = segSy1[li] = 0.f;
            continue;
        }
        const Graph::Node* src = m_Model->FindNode(link.SourceNodeId);
        const Graph::Node* tgt = m_Model->FindNode(link.TargetNodeId);
        if (!src || !tgt) { segSx0[li] = segSy0[li] = segSx1[li] = segSy1[li] = 0.f; continue; }
        const size_t si = FindPortIndex(*src, link.SourcePortId);
        const size_t ti = FindPortIndex(*tgt, link.TargetPortId);
        float gx0, gy0, gx1, gy1;
        GetPortCenterInGraph(*src, si, gx0, gy0);
        GetPortCenterInGraph(*tgt, ti, gx1, gy1);
        graphToScreen(gx0, gy0, segSx0[li], segSy0[li]);
        graphToScreen(gx1, gy1, segSx1[li], segSy1[li]);
    }

    std::vector<GraphRouting::Route> routes;
    if (m_UseStraightLines)
        BuildStraightRoutes(canvasX, canvasY, routes);

    /* Wires attached to a selected node stand out from the rest: "bright" lifts
       their colour toward white, "glow" adds a soft halo of the port colour
       under a lifted core, "halo" adds the same halo under the unchanged wire.
       All draw after the other wires so they sit on top. */
    const std::string wireEmphasis = GetSelectedNodeWireEmphasis();
    const bool emphasiseSelectedNodeWires = wireEmphasis != "off" && !m_SelectedNodeIds.empty();
    const bool haloSelectedNodeWires = wireEmphasis == "glow" || wireEmphasis == "halo";
    const bool liftSelectedNodeWires = wireEmphasis == "glow" || wireEmphasis == "bright";
    const bool glowSelectedNodeWires = wireEmphasis == "glow";
    constexpr float kEmphasisBrightenBright = 0.45f;
    constexpr float kEmphasisBrightenGlow = 0.30f;
    constexpr float kEmphasisThicknessScale = 1.25f;
    constexpr float kGlowOuterThicknessScale = 4.5f;
    constexpr float kGlowInnerThicknessScale = 2.6f;
    constexpr float kGlowOuterAlpha = 0.16f;
    constexpr float kGlowInnerAlpha = 0.30f;
    auto linkTouchesSelectedNode = [&](const Graph::Edge& link)
    {
        return emphasiseSelectedNodeWires &&
               (IsNodeSelected(link.SourceNodeId) || IsNodeSelected(link.TargetNodeId));
    };

    // Draw each link: plain wires first, then the emphasised ones on top.
    for (int pass = 0; pass < 2; ++pass)
    {
        const bool emphasisPass = pass == 1;
        if (emphasisPass && !emphasiseSelectedNodeWires)
            break;
        for (size_t li = 0; li < numLinks; ++li)
        {
            const auto& link = m_Model->Links[li];
            if (IsLinkHiddenPendingDelete(link) ||
                (!m_ReroutingLinkId.empty() && link.Id == m_ReroutingLinkId))
                continue;
            const bool emphasised = linkTouchesSelectedNode(link);
            if (emphasised != emphasisPass)
                continue;
            const Graph::Node* srcNode = m_Model->FindNode(link.SourceNodeId);
            const Graph::Node* targetNode = m_Model->FindNode(link.TargetNodeId);
            if (!srcNode || !targetNode)
                continue;
            const Graph::Port* srcPort = FindPort(*srcNode, link.SourcePortId);
            bool hovered = (link.Id == m_HoveredLinkId);
            const bool selected = (link.Id == m_SelectedLinkId);
            auto pulseIt = m_RuntimeTransitionPulses.find(link.Id);
            const bool runtimeActive = pulseIt != m_RuntimeTransitionPulses.end();
            float thick = (hovered || selected) ? kLineThickness * 1.4f : kLineThickness;
            if (runtimeActive)
                thick = std::max(thick, kLineThickness * (1.25f + 0.45f * pulseIt->second.Strength));
            uint32_t col = portColorPacked(srcPort);
            if (selected)
                col = selectedColor;
            else if (hovered)
                col = hoveredColor;
            if (runtimeActive && !hovered && !selected)
                col = PackFromARGB(0xFFFFB03Au);
            if (emphasised && liftSelectedNodeWires && !hovered && !selected && !runtimeActive)
            {
                thick = std::max(thick, kLineThickness * kEmphasisThicknessScale);
                col = WireColorBrightened(
                    col, glowSelectedNodeWires ? kEmphasisBrightenGlow : kEmphasisBrightenBright);
            }
            float pulseX = segSx0[li];
            float pulseY = segSy0[li];

            auto emitWire = [&](float wireThickness, uint32_t wireColor)
            {
                if (m_UseStraightLines)
                    emitConnectionRoute(routes[li].Points, wireThickness, wireColor);
                else
                    emitBezier(segSx0[li], segSy0[li], segSx1[li], segSy1[li], wireThickness,
                               wireColor);
            };
            if (emphasised && haloSelectedNodeWires)
            {
                const uint32_t haloColor = portColorPacked(srcPort);
                emitWire(thick * kGlowOuterThicknessScale, WireColorWithAlpha(haloColor, kGlowOuterAlpha));
                emitWire(thick * kGlowInnerThicknessScale, WireColorWithAlpha(haloColor, kGlowInnerAlpha));
            }
            emitWire(thick, col);
            if (runtimeActive)
            {
                if (m_UseStraightLines)
                    (void)pointOnPolyline(routes[li].Points, pulseIt->second.Phase, pulseX, pulseY);
                else
                    pointOnBezier(segSx0[li], segSy0[li], segSx1[li], segSy1[li],
                                  pulseIt->second.Phase, pulseX, pulseY);
            }

            if (runtimeActive)
            {
                const float pulseRadius = (3.0f + 3.0f * pulseIt->second.Strength) * m_Zoom;
                emitRectRounded(pulseX - pulseRadius, pulseY - pulseRadius,
                                pulseRadius * 2.0f, pulseRadius * 2.0f,
                                PackFromARGB(0xFFFFD37Au), pulseRadius);
            }
        }
    }

    // Dragging link preview — also while the wire-drop menu is choosing the node
    // this wire will land on, so the wire does not vanish under the menu.
    if ((m_DraggingLink || m_WireDropMenuOpen) && m_Model)
    {
        /* Which end is anchored cannot be read from m_DraggingLinkToExistingInput
           once the gesture has ended: mouse-up clears it while the drop menu
           keeps the wire alive. The surviving id is the anchor. */
        const bool anchoredOnInput = m_DraggingLink ? m_DraggingLinkToExistingInput
                                                    : !m_ConnectionTargetNodeId.empty();
        if (anchoredOnInput)
        {
            const Graph::Node* targetNode = m_Model->FindNode(m_ConnectionTargetNodeId);
            if (targetNode)
            {
                const size_t idx = FindPortIndex(*targetNode, m_ConnectionTargetPortId);
                float targetGx, targetGy;
                GetPortCenterInGraph(*targetNode, idx, targetGx, targetGy);
                float sx0, sy0, sx1, sy1;
                graphToScreen(m_ConnectionEndGraphX, m_ConnectionEndGraphY, sx0, sy0);
                graphToScreen(targetGx, targetGy, sx1, sy1);
                if (m_UseStraightLines)
                    emitStraightElbow(sx0, sy0, sx1, sy1, kLineThickness, portColorPacked(FindPort(*targetNode, m_ConnectionTargetPortId)), 0, 1);
                else
                    emitBezier(sx0, sy0, sx1, sy1, kLineThickness, portColorPacked(FindPort(*targetNode, m_ConnectionTargetPortId)));
            }
        }
        else
        {
            const Graph::Node* srcNode = m_Model->FindNode(m_ConnectionSourceNodeId);
            if (srcNode)
            {
                const size_t idx = FindPortIndex(*srcNode, m_ConnectionSourcePortId);
                float gx1, gy1;
                GetPortCenterInGraph(*srcNode, idx, gx1, gy1);
                float sx0, sy0, sx1, sy1;
                graphToScreen(gx1, gy1, sx0, sy0);
                graphToScreen(m_ConnectionEndGraphX, m_ConnectionEndGraphY, sx1, sy1);
                const Graph::Port* srcPort = FindPort(*srcNode, m_ConnectionSourcePortId);
                if (m_UseStraightLines)
                    emitStraightElbow(sx0, sy0, sx1, sy1, kLineThickness, portColorPacked(srcPort), 0, 1);
                else
                    emitBezier(sx0, sy0, sx1, sy1, kLineThickness, portColorPacked(srcPort));
            }
        }
    }

    // Selection box (marquee)
    if (m_DraggingSelectionBox)
    {
        float left = std::min(m_SelectionBoxStartX, m_SelectionBoxCurrentX);
        float right = std::max(m_SelectionBoxStartX, m_SelectionBoxCurrentX);
        float top = std::min(m_SelectionBoxStartY, m_SelectionBoxCurrentY);
        float bottom = std::max(m_SelectionBoxStartY, m_SelectionBoxCurrentY);
        
        // Only draw if box has some area
        if (right - left > 2.0f && bottom - top > 2.0f)
        {
            // Semi-transparent blue fill
            const uint32_t fillColor = PackFromARGB(0x403A8FFFu); // 25% alpha blue
            const uint32_t borderColor = PackFromARGB(0xFF3A8FFFu); // solid blue
            
            // Fill rectangle
            ctx.Emit(MakeRect(left * cs, top * cs, 
                             (right - left) * cs, (bottom - top) * cs, 
                             fillColor));
            
            // Border (1px dashed effect using thin rectangles)
            constexpr float kBorderThickness = 1.0f;
            // Top edge
            ctx.Emit(MakeRect(left * cs, top * cs, 
                             (right - left) * cs, kBorderThickness * cs, 
                             borderColor));
            // Bottom edge
            ctx.Emit(MakeRect(left * cs, (bottom - kBorderThickness) * cs, 
                             (right - left) * cs, kBorderThickness * cs, 
                             borderColor));
            // Left edge
            ctx.Emit(MakeRect(left * cs, top * cs, 
                             kBorderThickness * cs, (bottom - top) * cs, 
                             borderColor));
            // Right edge
            ctx.Emit(MakeRect((right - kBorderThickness) * cs, top * cs, 
                             kBorderThickness * cs, (bottom - top) * cs, 
                             borderColor));
        }
    }
}

} // namespace GameEngine
