#pragma once

#include "Markups/MarkupDrawList.h"
#include "Markups/MarkupHighlightState.h"
#include "Mathematics/Vector4.h"
#include "SceneView/SceneViewGizmos.h"

#include <array>
#include <cstddef>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class MarkupEditorBridge;

// Draws every mark-up in the Scene View through the gizmo overlay pass: a translucent fill in its
// color (its own, else its status tag's) and a wire outline, both against scene depth, so the
// pass dims the part behind terrain. A region's fill is its display's walls and lid
// (MarkupRegionDisplayCache) and its outline the rings at the base and along the top edge. A path
// draws its draped line and a translucent band either side of it, whether or not the glow draws
// the bodies (a path has no glow body). A selected mark-up (MarkupHighlightState)
// draws a stronger fill and a doubled outline in its color; one whose panel or Hierarchy row is
// hovered, and is not selected, a lifted fill and one outline in the editor's light accent
// (MarkupHoverRgb, kMarkupAccentRgb), the same on a box and a sphere. A volume
// always draws in its own color: an update the viewer has not seen is the label's unseen dot
// (MarkupLabelOverlay), so the status stays readable on the volume. A volume the camera is
// inside draws its outline only, so the fill does not wash over the whole view. Where the
// Scene View draws the bodies with the glow (MarkupRenderFeature, desktop), the fill is off and
// the outline stays, a sphere's as the one circle facing the camera; on the compat profile the
// fill draws. Mark-ups the viewer hid,
// switched-off ones and every mark-up in play mode draw nothing; with nothing to draw it emits
// no group. Main thread only.
class MarkupGizmo final : public SceneTools::IGizmo
{
  public:
    // How many mark-ups get a fill: the highlighted ones, then the nearest to the camera. The
    // rest draw their outline only, a sphere the camera is outside of as the one circle facing it.
    static constexpr std::size_t kFillCap = kMarkupBodyCap;

    void Render(SceneTools::GizmoRenderContext& context) override;
    // Whether Render draws the fills: off while the glow draws the bodies, for the frame it does.
    void SetDrawsFills(bool drawsFills) { m_DrawsFills = drawsFills; }
    // The view's six frustum planes for the frame Render draws, so a region out of view is not
    // built; null counts every mark-up in view.
    void SetViewFrustum(const Mathematics::Vector4* planes);
    // The mark-ups the glow collected for this pane and frame (MarkupRenderFeature::CollectedItems
    // and its body count), which Render draws instead of collecting them again; null, and Render
    // collects its own (no glow this frame, or the compat profile). Set before every Render.
    void UseCollectedItems(const std::vector<MarkupDrawItem>* items, std::size_t bodies)
    {
        m_Collected = items;
        m_CollectedBodies = bodies;
    }

    // Render's drawing of `world`'s mark-ups on `frame`, seen from `cameraPos`; nothing in play mode.
    void Draw(SceneTools::GizmoRenderContext& context, ECS::World& world, const MarkupEditorBridge& bridge,
              const MarkupHighlightState& highlight, const Mathematics::Vector3& cameraPos, uint64 frame);

  private:
    // The outline's color: the mark-up's, opaque when selected or hovered.
    static Color OutlineColor(const MarkupDrawItem& item);

    std::vector<MarkupDrawItem> m_Items; // this frame's mark-ups, kept to reuse the allocation
    std::vector<uint32> m_ExcludedScratch; // CollectMarkupDrawItems' scratch, kept to reuse the allocation
    const std::vector<MarkupDrawItem>* m_Collected = nullptr; // the glow's, when set
    std::size_t m_CollectedBodies = 0;
    std::vector<Mathematics::Vector3> m_Scratch; // a region's fill or outline lines, reused
    std::array<Mathematics::Vector4, 6> m_Frustum{};
    bool m_HasFrustum = false;
    bool m_DrawsFills = true;
};

} // namespace GameEngine::Editor
