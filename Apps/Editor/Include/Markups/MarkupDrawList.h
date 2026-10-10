#pragma once

#include "MarkupECS/MarkupRegionDisplayCache.h"
#include "Markups/MarkupHighlightState.h"
#include "Markups/MarkupPresentation.h"

#include "ECS/Entity.h"

#include <array>
#include <cstddef>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Mathematics
{
struct Vector4;
}

namespace GameEngine::Editor
{
class MarkupEditorBridge;

// How many mark-ups get a body (the gizmo's fill or the glow): the highlighted ones, then the
// nearest to the camera. The rest draw their outline only.
inline constexpr std::size_t kMarkupBodyCap = 64;

// One mark-up as a frame of the Scene View draws it: a volume, or a region (Region set), whose
// Volume holds only the bounding sphere of its display (Center, BoundingRadius, TopY).
struct MarkupDrawItem
{
    MarkupWorldVolume Volume;
    // A region's or a path's display, valid for this frame; null for a volume.
    const MarkupECS::MarkupRegionDisplayCache::Entry* Region = nullptr;
    // The display is a path's: its draped line, drawn by the gizmo only (it has no glow body).
    bool Path = false;
    ECS::EntityHandle Entity{};
    std::array<float32, 3> Rgb{};
    float DistanceSq = 0.0f;
    bool Selected = false;
    bool Hovered = false;      // its Mark-ups panel row or Hierarchy row is under the pointer
    bool CameraInside = false; // draws no body
    bool OutOfView = false;    // its bounding sphere is outside the view's frustum: draws no body
    // An Exclude member of a region the viewer has not hidden: at rest (neither selected nor
    // hovered) it draws no body, only its outline, so the hole it cuts reads as a hole and the
    // ground shows through it; selected or hovered it draws as any mark-up.
    bool ExcludeMember = false;

    bool DrawsBody() const { return !CameraInside && !OutOfView && !(ExcludeMember && !Selected && !Hovered); }
};

// The hover highlight, one drawing for a box and a sphere alike: the hovered mark-up's body (the
// glow's or the gizmo's fill) takes its color lifted kMarkupHoverLift of the way to white at a
// denser alpha, and the gizmo outlines it once in the editor's light accent (kMarkupAccentRgb).
// The strength comes from the fill: a second or third outline reads as another wireframe.
inline constexpr float32 kMarkupHoverLift = 0.7f;
// The editor's light accent, which reads on any ground: the hovered mark-up's outline and the
// Mark-up tool's outline preview. The value of the theme's --ui_color_accent_blue_hover
// (tokens.css, #4C9AFF), copied: the gizmo pass reads no stylesheet, so a change to the token is
// made here too.
inline constexpr std::array<float32, 3> kMarkupAccentRgb{76.0f / 255.0f, 154.0f / 255.0f, 1.0f};

// `rgb` lifted kMarkupHoverLift of the way to white.
inline std::array<float32, 3> MarkupHoverRgb(const std::array<float32, 3>& rgb)
{
    return {rgb[0] + kMarkupHoverLift * (1.0f - rgb[0]), rgb[1] + kMarkupHoverLift * (1.0f - rgb[1]),
            rgb[2] + kMarkupHoverLift * (1.0f - rgb[2])};
}

// The mark-ups of `world` the Scene View draws, seen from `cameraPos`, into `items` (cleared
// first): every volume, region and path but those the viewer hid and those without a shape (a
// switched-off entity is not queried), and none in play mode. `frustumPlanes` is the view's six
// planes (Rendering::ExtractFrustumPlanes), or null for a caller that has none, which counts every
// mark-up in view. A region or a path in view resolves its display in the bridge's cache on `frame`
// (ResolveMarkupRegionDisplay); one out of view is not built. Returns how many lead `items` as the
// ones that draw a body: at most kMarkupBodyCap, highlighted first, then the nearest, those that
// draw no body (the camera inside, out of view, or an Exclude member at rest) last, counted. Main
// thread only. `excludedScratch` is the collection's scratch, kept by the caller beside `items` to
// reuse its allocation.
std::size_t CollectMarkupDrawItems(ECS::World& world, const MarkupEditorBridge& bridge,
                                   const MarkupHighlightState& highlight, const Mathematics::Vector3& cameraPos,
                                   const Mathematics::Vector4* frustumPlanes, uint64 frame,
                                   std::vector<MarkupDrawItem>& items, std::vector<uint32>& excludedScratch);

} // namespace GameEngine::Editor
