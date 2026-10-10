#include "Markups/MarkupLabelOverlay.h"

#include "Components/Markup/Markup.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupHighlightState.h"
#include "Markups/MarkupPresentation.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "SceneView/SceneViewEvents.h"
#include "SceneView/SceneViewProjection.h"
#include "SceneViewController.h"
#include "UI/Controls/Label.h"
#include "UI/StyleProperties.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string_view>

namespace GameEngine::Editor
{

using Components::Markup;
using Components::MarkupRegion;
using Components::MarkupVolume;
using Mathematics::Vector3;

namespace
{

constexpr const char* kStyleAssetPath = "UI/controls/Markups/MarkupLabelOverlay.css";
// The anchor is a fixed-width row that centers its pill, so a label is centered on its point
// before its text is measured: MarkupLabelOverlay.css's .markup-label-anchor width.
constexpr float kAnchorWidth = 240.0f;
// The class of a pane's axis widget (ViewportRotationGizmo), a child of the viewport the
// labels sit on, and the gap a pill keeps from it.
constexpr const char* kAxisWidgetClass = "viewport-rotation-gizmo";
constexpr float kReservedGap = 4.0f;

bool InView(const Mathematics::Vector2& pixel, const SceneTools::ScenePointerEvent& view)
{
    return pixel.x >= 0.0f && pixel.y >= 0.0f && pixel.x <= view.viewW && pixel.y <= view.viewH;
}

void SetShown(UIElement& element, bool shown)
{
    element.Overrides().Set(Style::Display, shown ? DisplayMode::Flex : DisplayMode::None);
}

// What one Collect reads per mark-up.
struct PlacementSource
{
    ECS::World& World;
    const MarkupEditorBridge& Bridge;
    const MarkupHighlightState& Highlight;
    const SceneTools::ScenePointerEvent& View;
    std::vector<MarkupLabelOverlay::Placement>& Out;
};

// Where a mark-up's pill stands: over `top`, its shape's bounding sphere at `center` with
// `radius`, and whether the camera is inside the shape.
struct LabelAnchor
{
    Vector3 Top;
    Vector3 Center;
    float Radius = 0.0f;
    bool CameraInside = false;
};

void AddPlacement(const PlacementSource& source, ECS::EntityHandle entity, const LabelAnchor& shape)
{
    const SceneTools::ScenePointerEvent& view = source.View;
    const Vector3& top = shape.Top;
    const float distance = (top - view.cameraPos).Length();
    // An orthographic view has no distance that sets the projected size: its height does.
    const float span = view.tanHalfFovY > 0.0f ? distance : view.orthoHeight;
    if (shape.Radius < MarkupLabelOverlay::kMinAngularRadius * span)
        return;

    Mathematics::Vector2 anchor;
    const bool topProjects = SceneTools::ProjectWorldToView(view, top, anchor);
    const bool topInView = topProjects && InView(anchor, view);
    if (!topInView && shape.CameraInside)
    {
        // The camera is inside the volume: the pill holds at the view's top center.
        anchor = Mathematics::Vector2(view.viewW * 0.5f, 0.0f);
    }
    else if (!topInView)
    {
        // A framed volume: its top is out of the view, its center in it. Hold the pill at
        // the view's edge above the center rather than drop it.
        Mathematics::Vector2 center;
        if (!SceneTools::ProjectWorldToView(view, shape.Center, center) || !InView(center, view))
            return;
        if (!topProjects)
            anchor = center;
        anchor.x = std::clamp(anchor.x, 0.0f, view.viewW);
    }
    anchor.y = std::clamp(anchor.y, MarkupLabelOverlay::kMinAnchorY, std::max(view.viewH, MarkupLabelOverlay::kMinAnchorY));
    source.Out.push_back({entity, anchor.x, anchor.y, distance, source.Bridge.HasUnseenUpdate(source.World, entity),
                          source.Highlight.IsSelected(entity), source.Highlight.IsHovered(entity)});
}

void AddVolumePlacement(const PlacementSource& source, ECS::EntityHandle entity, const MarkupVolume& shape,
                        const Components::WorldTransform& transform)
{
    if (source.Bridge.IsHidden(source.World, entity))
        return;
    const std::optional<MarkupWorldVolume> volume = MarkupWorldVolumeFromMatrix(shape.Shape, transform.matrix);
    if (!volume)
        return;
    AddPlacement(source, entity,
                 LabelAnchor{Vector3(volume->Center.x, volume->TopY, volume->Center.z), volume->Center,
                             volume->BoundingRadius, MarkupVolumeContains(*volume, source.View.cameraPos)});
}

// A region's pill stands over its label point (inside even a C-shaped outline) at the walls'
// height, a path's over the middle of its way. Its display is the one the Scene View drew this
// frame; a region or path not drawn yet has none.
void AddRegionPlacement(const PlacementSource& source, ECS::EntityHandle entity)
{
    if (source.Bridge.IsHidden(source.World, entity))
        return;
    const MarkupECS::MarkupRegionDisplayCache::Entry* display = source.Bridge.RegionDisplaysOf(source.World).Find(entity);
    if (!display || display->Ground.Outline.empty())
        return;
    const Vector3 center = display->Ground.Center + Vector3(0.0f, display->ExtrudeHeight * 0.5f, 0.0f);
    AddPlacement(source, entity, LabelAnchor{display->Mesh.Label, center, display->Ground.Radius, false});
}

bool Nearer(const MarkupLabelOverlay::Placement& a, const MarkupLabelOverlay::Placement& b)
{
    return a.Distance < b.Distance;
}

// The rect of `viewport`'s axis widget relative to the viewport, empty while it is hidden.
Mathematics::Rect AxisWidgetRect(const UIElement& viewport)
{
    for (const std::unique_ptr<UIElement>& child : viewport.GetChildren())
    {
        if (!child->HasClass(kAxisWidgetClass) || child->HasClass("hidden"))
            continue;
        return Mathematics::Rect{child->GetLayoutX() - viewport.GetLayoutX(), child->GetLayoutY() - viewport.GetLayoutY(),
                                 child->GetLayoutWidth(), child->GetLayoutHeight()};
    }
    return {};
}

// Marks the placements of `component`'s members with `region`, unless an earlier region holds them.
void LinkMembers(ECS::EntityHandle region, const MarkupRegion& component,
                 std::vector<MarkupLabelOverlay::Placement>& placements)
{
    const uint32 count = std::min(component.MemberCount, Components::kMaxRegionMembers);
    for (MarkupLabelOverlay::Placement& placement : placements)
    {
        if (placement.Region.IsValid())
            continue;
        for (uint32 i = 0; i < count; ++i)
        {
            if (component.Members[i].Entity == placement.Entity)
            {
                placement.Region = region;
                break;
            }
        }
    }
}

Mathematics::Rect PillRect(const MarkupLabelOverlay::PillBox& box)
{
    return Mathematics::Rect{box.CenterX - box.HalfWidth, box.Top, 2.0f * box.HalfWidth,
                             MarkupLabelOverlay::kPillHeight};
}

// Whether `boxes[member]` overlaps the shown pill of another member of the same region.
bool OverlapsASibling(const std::vector<MarkupLabelOverlay::Placement>& placements,
                      const std::vector<MarkupLabelOverlay::PillBox>& boxes, std::size_t member)
{
    const Mathematics::Rect rect = PillRect(boxes[member]);
    for (std::size_t other = 0; other < placements.size(); ++other)
    {
        if (other != member && placements[other].Region == placements[member].Region && !boxes[other].Hidden &&
            rect.Overlaps(PillRect(boxes[other])))
            return true;
    }
    return false;
}

bool NotHighlighted(const MarkupLabelOverlay::Placement& placement)
{
    return !placement.Selected && !placement.Hovered;
}

void SetClass(UIElement& element, const char* className, bool on)
{
    if (on)
        element.AddClass(className);
    else
        element.RemoveClass(className);
}

} // namespace

void MarkupLabelOverlay::Collect(ECS::World& world, const MarkupEditorBridge& bridge,
                                 const MarkupHighlightState& highlight, const SceneTools::ScenePointerEvent& view,
                                 std::vector<Placement>& out)
{
    out.clear();
    // Mark-ups are editor-only and edited in the edit world: play labels none.
    if (bridge.IsInPlayMode())
        return;
    const PlacementSource source{world, bridge, highlight, view, out};
    world.Query<ECS::Read<Markup>, ECS::Read<MarkupVolume>, ECS::Read<Components::WorldTransform>>().Each(
        [&source](ECS::EntityHandle entity, const Markup&, const MarkupVolume& shape,
                  const Components::WorldTransform& transform) { AddVolumePlacement(source, entity, shape, transform); });
    world.Query<ECS::Read<Markup>, ECS::Read<Components::SplineComponent>>().Each(
        [&source](ECS::EntityHandle entity, const Markup&, const Components::SplineComponent&) {
            AddRegionPlacement(source, entity);
        });
    const std::size_t kept = std::min(out.size(), kLabelCap);
    std::partial_sort(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(kept), out.end(), Nearer);
    out.resize(kept);
    if (!out.empty())
    {
        world.Query<ECS::Read<Markup>, ECS::Read<MarkupRegion>>().Each(
            [&out](ECS::EntityHandle region, const Markup&, const MarkupRegion& component) {
                LinkMembers(region, component, out);
            });
    }
    // The container draws its pills in order: the highlighted ones last, over the rest.
    std::stable_partition(out.begin(), out.end(), NotHighlighted);
}

void MarkupLabelOverlay::Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& camera)
{
    if (view != ViewOverlayView::Scene || !camera)
        return;
    auto container = std::make_unique<UIElement>();
    container->AddClass("markup-labels");
    container->RequestSubtreeStyleAssetPath(kStyleAssetPath, "editor");
    m_Layers.push_back({UIElement::MakeWeakRef(container.get()), camera, {}});
    layer.AddChild(std::move(container));
}

void MarkupLabelOverlay::Update()
{
    m_Layers.erase(std::remove_if(m_Layers.begin(), m_Layers.end(),
                                  [](const LabelLayer& layer) { return layer.Container.Get() == nullptr; }),
                   m_Layers.end());
    if (m_Layers.empty())
        return;
    const MarkupEditorBridge* bridge = MarkupEditorBridge::TryGet();
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    for (LabelLayer& layer : m_Layers)
        UpdateLayer(layer, world, bridge);
}

void MarkupLabelOverlay::UpdateLayer(LabelLayer& layer, ECS::World* world, const MarkupEditorBridge* bridge)
{
    UIElement* container = layer.Container.Get();
    const UIElement* overlayLayer = container->GetParent();
    const UIElement* viewport = overlayLayer ? overlayLayer->GetParent() : nullptr;
    // The layer lives, so the pane's camera source may be asked.
    SceneViewController* pane = layer.Camera();
    if (!world || !bridge || !viewport || !pane || !pane->AreGizmosVisible() || !pane->AreMarkupGizmosVisible())
    {
        Hide(layer);
        return;
    }
    SceneTools::ScenePointerEvent view;
    view.viewW = viewport->GetLayoutWidth();
    view.viewH = viewport->GetLayoutHeight();
    layer.ViewWidth = view.viewW;
    layer.ViewHeight = view.viewH;
    layer.Reserved = AxisWidgetRect(*viewport);
    pane->PopulatePointerCameraState(view);
    Collect(*world, *bridge, bridge->GetHighlight(), view, m_Placements);

    const float offsetX = viewport->GetLayoutX() - overlayLayer->GetLayoutX();
    const float offsetY = viewport->GetLayoutY() - overlayLayer->GetLayoutY();
    if (offsetX != layer.OffsetX || offsetY != layer.OffsetY)
    {
        layer.OffsetX = offsetX;
        layer.OffsetY = offsetY;
        container->Overrides()
            .Set(Style::PositionLeft, StyleLength::Px(offsetX))
            .Set(Style::PositionTop, StyleLength::Px(offsetY));
    }
    Show(layer, *world);
}

void MarkupLabelOverlay::AddPill(LabelLayer& layer, UIElement& container)
{
    auto anchor = std::make_unique<UIElement>();
    anchor->AddClass("markup-label-anchor");
    auto pill = std::make_unique<UIElement>();
    pill->AddClass("markup-label");
    auto dot = std::make_unique<UIElement>();
    dot->AddClass("markup-label-dot");
    auto title = std::make_unique<Label>();
    title->AddClass("markup-label-title");
    auto unread = std::make_unique<UIElement>();
    unread->AddClass("markup-label-unread");
    SetShown(*unread, false);
    Pill& entry = layer.Pills.emplace_back();
    entry.Anchor = UIElement::MakeWeakRef(anchor.get());
    entry.Body = UIElement::MakeWeakRef(pill.get());
    entry.Dot = UIElement::MakeWeakRef(dot.get());
    entry.Title = UIElement::MakeWeakRef(title.get());
    entry.UnreadDot = UIElement::MakeWeakRef(unread.get());
    pill->AddChild(std::move(dot));
    pill->AddChild(std::move(title));
    pill->AddChild(std::move(unread));
    anchor->AddChild(std::move(pill));
    SetShown(*anchor, false);
    container.AddChild(std::move(anchor));
}

void MarkupLabelOverlay::Show(LabelLayer& layer, ECS::World& world)
{
    UIElement* container = layer.Container.Get();
    while (layer.Pills.size() < m_Placements.size())
        AddPill(layer, *container);
    LayOut(layer);
    for (std::size_t index = 0; index < layer.Pills.size(); ++index)
    {
        Pill& pill = layer.Pills[index];
        UIElement* anchor = pill.Anchor.Get();
        if (!anchor)
            continue;
        if (index < m_Placements.size())
        {
            ShowPill(pill, m_Placements[index], m_Boxes[index], world);
            continue;
        }
        if (pill.Shown)
            SetShown(*anchor, pill.Shown = false);
    }
}

void MarkupLabelOverlay::LayOut(LabelLayer& layer)
{
    m_Boxes.resize(m_Placements.size());
    for (std::size_t index = 0; index < m_Placements.size(); ++index)
    {
        // The pill's width as last laid out: a pill held at a pane's side stays inside the pane.
        // A hidden pill keeps the width it had while shown.
        Pill& pill = layer.Pills[index];
        if (const UIElement* body = pill.Body.Get(); body && body->GetLayoutWidth() > 0.0f)
            pill.HalfWidth = body->GetLayoutWidth() * 0.5f;
        const Placement& placement = m_Placements[index];
        const float top = placement.Y - kPillHeight - kPillGap;
        m_Boxes[index] = {PillCenterX(placement.X, top, pill.HalfWidth, layer.ViewWidth, layer.Reserved), top,
                          pill.HalfWidth, false};
    }
    YieldToRegions(m_Placements, m_Boxes, layer.ViewWidth, layer.ViewHeight, layer.Reserved);
}

void MarkupLabelOverlay::ShowPill(Pill& pill, const Placement& placement, const PillBox& box, ECS::World& world)
{
    UIElement* anchor = pill.Anchor.Get();
    if (box.Hidden)
    {
        if (pill.Shown)
            SetShown(*anchor, pill.Shown = false);
        return;
    }
    if (!pill.Shown)
        SetShown(*anchor, pill.Shown = true);
    // The title is written, and copied, only when it changed.
    const std::string_view text = MarkupTitleView(world, placement.Entity);
    if (text != pill.Text)
    {
        pill.Text.assign(text);
        if (Label* title = pill.Title.Get())
            title->SetText(pill.Text);
    }
    const uint32_t dotArgb = MarkupDisplayArgb(*world.GetComponent<Markup>(placement.Entity));
    const bool dotChanged = dotArgb != pill.DotArgb;
    if (dotChanged)
    {
        if (UIElement* dot = pill.Dot.Get())
            dot->Overrides().Set(Style::BackgroundColor, dotArgb);
        pill.DotArgb = dotArgb;
    }
    const bool highlightChanged = placement.Selected != pill.Selected || placement.Hovered != pill.Hovered;
    if (UIElement* body = pill.Body.Get(); body && (highlightChanged || dotChanged))
    {
        SetClass(*body, "markup-label-selected", placement.Selected);
        SetClass(*body, "markup-label-hovered", placement.Hovered);
        // A hovered pill's rim is its status color; the selection's accent border wins.
        if (placement.Hovered && !placement.Selected)
            body->Overrides().Set(Style::BorderColor, BorderColorsTRBL{dotArgb, dotArgb, dotArgb, dotArgb});
        else
            body->Overrides().Reset(Style::BorderColor);
        pill.Selected = placement.Selected;
        pill.Hovered = placement.Hovered;
    }
    if (placement.Unseen != pill.UnreadShown)
    {
        if (UIElement* unread = pill.UnreadDot.Get())
            SetShown(*unread, placement.Unseen);
        pill.UnreadShown = placement.Unseen;
    }
    const float x = std::round(box.CenterX - kAnchorWidth * 0.5f);
    const float y = std::round(box.Top);
    if (x != pill.X || y != pill.Y)
    {
        anchor->Overrides()
            .Set(Style::PositionLeft, StyleLength::Px(x))
            .Set(Style::PositionTop, StyleLength::Px(y));
        pill.X = x;
        pill.Y = y;
    }
}

float MarkupLabelOverlay::PillCenterX(float anchorX, float top, float halfWidth, float viewWidth,
                                     const Mathematics::Rect& reserved)
{
    float centerX = viewWidth > 2.0f * halfWidth ? std::clamp(anchorX, halfWidth, viewWidth - halfWidth) : anchorX;
    const Mathematics::Rect pill{centerX - halfWidth, top, 2.0f * halfWidth, kPillHeight};
    if (reserved.Width <= 0.0f || !pill.Overlaps(reserved.Inflated(kReservedGap)))
        return centerX;
    // Step aside toward the view's middle.
    if (reserved.X + reserved.Width * 0.5f >= viewWidth * 0.5f)
        return reserved.X - kReservedGap - halfWidth;
    return reserved.Right() + kReservedGap + halfWidth;
}

void MarkupLabelOverlay::YieldToRegions(const std::vector<Placement>& placements, std::vector<PillBox>& boxes,
                                        float viewWidth, float viewHeight, const Mathematics::Rect& reserved)
{
    for (std::size_t member = 0; member < placements.size(); ++member)
    {
        const ECS::EntityHandle regionEntity = placements[member].Region;
        if (!regionEntity.IsValid())
            continue;
        const auto region = std::find_if(placements.begin(), placements.end(),
                                         [regionEntity](const Placement& p) { return p.Entity == regionEntity; });
        if (region == placements.end())
            continue;
        const PillBox& regionBox = boxes[static_cast<std::size_t>(region - placements.begin())];
        PillBox& box = boxes[member];
        if (regionBox.Hidden || !PillRect(box).Overlaps(PillRect(regionBox)))
            continue;
        // The first row under the region's pill clear of every other member's pill of that region.
        box.Top = regionBox.Top;
        do
        {
            box.Top += kPillHeight + kPillStackGap;
            box.CenterX = PillCenterX(placements[member].X, box.Top, box.HalfWidth, viewWidth, reserved);
        } while (OverlapsASibling(placements, boxes, member) && box.Top + kPillHeight <= viewHeight);
        box.Hidden = box.Top + kPillHeight > viewHeight;
    }
}

void MarkupLabelOverlay::Hide(LabelLayer& layer)
{
    for (Pill& pill : layer.Pills)
    {
        if (UIElement* anchor = pill.Anchor.Get(); anchor && pill.Shown)
            SetShown(*anchor, pill.Shown = false);
    }
}

} // namespace GameEngine::Editor
