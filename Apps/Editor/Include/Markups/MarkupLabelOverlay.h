#pragma once

#include "ECS/ECS.h"
#include "Mathematics/Rect.h"
#include "UI/UIElement.h"
#include "UI/ViewOverlay.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine
{
class Label;
}

namespace GameEngine::Editor::SceneTools
{
struct ScenePointerEvent;
}

namespace GameEngine::Editor
{
class MarkupEditorBridge;
struct MarkupHighlightState;

// The mark-ups' titles in the Scene View: one pill per visible volume mark-up, a dark plate
// with a light rim, its status color as a dot before the title and the Mark-ups panel's unseen
// dot, filled, on the pill's corner while the agent has an update the viewer has not seen,
// standing on the volume's top center. The selected and hovered mark-ups' pills draw over the
// others, a selected one with an accent border and a hovered one on a lighter plate with its
// rim in its status color (a selected and hovered one: the lighter plate and the accent
// border). A mark-up is labeled while its bounding sphere spans at least kMinAngularRadius
// radians from the camera (a 20 m box, about 15 m in radius, until about 1.5 km away; a 2 m
// one until 170 m), at most kLabelCap of them, the nearest. A volume whose top center is out of the
// view but whose center is in it (a framed one) keeps its pill, held at the view's edge, and
// one the camera is inside keeps it at the view's top center while its top is out of view.
// A pill stays inside its pane and clear of the pane's axis widget, and a region member's pill
// yields to its region's: where the two overlap, the member's stands under the region's, or is
// hidden when the view has no room below.
// Hidden, switched-off and out-of-view mark-ups have none, and none draw in play mode or
// while the pane's mark-up gizmos are hidden. Each pane projects with its own camera.
// Main thread only.
class MarkupLabelOverlay final : public ViewOverlay
{
  public:
    static constexpr float kMinAngularRadius = 0.01f;
    static constexpr std::size_t kLabelCap = 64;
    // The pill's height and the gap between its bottom edge and the point it stands on, in
    // view pixels; MarkupLabelOverlay.css sizes the pill to match.
    static constexpr float kPillHeight = 22.0f;
    static constexpr float kPillGap = 6.0f;
    // The gap between a region's pill and its member's pill standing under it, in view pixels.
    static constexpr float kPillStackGap = 2.0f;
    // The highest a point may sit so the pill standing on it stays in the view.
    static constexpr float kMinAnchorY = kPillHeight + kPillGap + 4.0f;

    // Where a label stands: in view pixels (x right, y down), the volume's top center, held
    // into the view when only the volume's center or the camera is in it.
    struct Placement
    {
        ECS::EntityHandle Entity{};
        float X = 0.0f;
        float Y = 0.0f;
        float Distance = 0.0f; // from the camera to the top center, in meters
        bool Unseen = false;   // the agent changed it since the viewer last looked
        bool Selected = false;
        bool Hovered = false; // its Mark-ups panel row or Hierarchy row is under the pointer
        // The region whose member this mark-up is (the first such region); none when it is no
        // region's member.
        ECS::EntityHandle Region{};
    };

    // A pill as laid out in its view, in view pixels: centered on CenterX, HalfWidth either
    // side, its top edge at Top, kPillHeight tall; Hidden when it yields with no room.
    struct PillBox
    {
        float CenterX = 0.0f;
        float Top = 0.0f;
        float HalfWidth = 0.0f;
        bool Hidden = false;
    };

    // The labels of `world`'s mark-ups in `view` (a pane's camera and size, as
    // SceneViewController::PopulatePointerCameraState writes it), the nearest kLabelCap,
    // ordered nearest first and then the selected and hovered ones, so theirs draw on top;
    // none in play mode.
    static void Collect(ECS::World& world, const MarkupEditorBridge& bridge, const MarkupHighlightState& highlight,
                        const SceneTools::ScenePointerEvent& view, std::vector<Placement>& out);

    // Where a pill `halfWidth` wide, its top at `top`, centers to stand over `anchorX`, in view
    // pixels: inside a view `viewWidth` wide when it fits, and beside `reserved` (the pane's
    // axis widget; none when empty) on the side away from the widget's half of the view.
    static float PillCenterX(float anchorX, float top, float halfWidth, float viewWidth,
                             const Mathematics::Rect& reserved);

    // The member's yield, `boxes` laid out for `placements` (one each, in order): a pill whose
    // Region has a pill it overlaps moves down to the first row under the region's pill (rows
    // kPillHeight + kPillStackGap apart) clear of the region's other members' pills, placed again
    // by PillCenterX, and is Hidden when no such row fits in a view `viewHeight` tall.
    static void YieldToRegions(const std::vector<Placement>& placements, std::vector<PillBox>& boxes,
                               float viewWidth, float viewHeight, const Mathematics::Rect& reserved);

    void Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& camera) override;
    void Update() override;

  private:
    // One label element and what it shows, so an unchanged label is not written again.
    struct Pill
    {
        UIElement::WeakRef<UIElement> Anchor;
        UIElement::WeakRef<UIElement> Body; // the pill itself, centered in its anchor
        UIElement::WeakRef<UIElement> Dot;
        UIElement::WeakRef<Label> Title;
        UIElement::WeakRef<UIElement> UnreadDot;
        std::string Text;
        uint32_t DotArgb = 0;
        float X = -1.0f;
        float Y = -1.0f;
        float HalfWidth = 0.0f; // as last laid out while shown, kept while the pill is hidden
        bool Shown = false;
        bool UnreadShown = false;
        bool Selected = false;
        bool Hovered = false;
    };
    // The labels of one Scene View pane.
    struct LabelLayer
    {
        UIElement::WeakRef<UIElement> Container;
        ViewOverlayCamera Camera;
        std::vector<Pill> Pills;
        float OffsetX = 0.0f; // the viewport's top-left corner from the overlay layer's
        float OffsetY = 0.0f;
        float ViewWidth = 0.0f;
        float ViewHeight = 0.0f;
        Mathematics::Rect Reserved{}; // the pane's axis widget in view pixels; empty when hidden
    };

    void UpdateLayer(LabelLayer& layer, ECS::World* world, const MarkupEditorBridge* bridge);
    // Shows m_Placements in `layer`, adding pills as needed and hiding the rest.
    void Show(LabelLayer& layer, ECS::World& world);
    static void AddPill(LabelLayer& layer, UIElement& container);
    // Lays out m_Boxes for m_Placements in `layer`'s view: PillCenterX, then YieldToRegions.
    void LayOut(LabelLayer& layer);
    // Shows `placement` in `pill` at `box`, or hides the pill when the box is Hidden.
    static void ShowPill(Pill& pill, const Placement& placement, const PillBox& box, ECS::World& world);
    static void Hide(LabelLayer& layer);

    std::vector<LabelLayer> m_Layers;
    std::vector<Placement> m_Placements;
    std::vector<PillBox> m_Boxes;
};

} // namespace GameEngine::Editor
