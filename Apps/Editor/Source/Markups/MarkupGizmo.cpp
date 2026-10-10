#include "Markups/MarkupGizmo.h"

#include "Core/Application.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "Markups/MarkupEditorBridge.h"
#include "Markups/MarkupPresentation.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector3.h"
#include "Types/Color.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace GameEngine::Editor
{

using Components::MarkupVolumeShape;
using Mathematics::Vector3;
using SceneTools::GizmoRenderContext;

namespace
{

// The fill's opacity at rest and selected or hovered; the outline's at rest and highlighted.
constexpr float kFillAlpha = 0.18f;
constexpr float kSelectedFillAlpha = 0.3f;
constexpr float kOutlineAlpha = 0.9f;
constexpr float kHighlightOutlineAlpha = 1.0f;
// The selection's second outline sits this fraction of the bounding radius outside the first:
// the overlay pass draws every line one pixel wide, so a doubled outline is its "thicker".
constexpr float kSelectedOutlineGrow = 0.015f;
// A path's band: at least this many meters either side of its line, growing with the camera's
// distance (kPathWidthPerMeter of it) so the band keeps a few pixels' width; at the fill's opacity
// (stronger highlighted).
constexpr float kPathBandHalfWidth = MarkupECS::kPathEdgeOffsets.front(); // its ground is read there
constexpr float kPathWidthPerMeter = 0.012f;
// The line and the band stand kPathLiftPerMeter of the camera's distance above the ground they are
// sampled on (its ground carries no lift of its own), never under kPathMinLift. The ground is the
// terrain's full-detail height and the terrain draws coarser with distance, so the band needs a lift
// that grows with it to stay above the displayed surface at range, while near the camera 0.1 % is a
// pixel or two and does not stand the band visibly off a crest into the sky. A band projected onto
// the rendered depth would need no lift; until then this is the trade.
constexpr float kPathMinLift = 0.05f;
constexpr float kPathLiftPerMeter = 0.001f;
constexpr float kPathBandAlpha = 0.35f;
constexpr float kHighlightPathBandAlpha = 0.6f;
// The fills draw after the scene's opaque gizmo meshes (layer 0), sorted back to front.
constexpr std::int32_t kFillLayer = 1;
constexpr float kLineThickness = 1.0f;

Mathematics::Matrix4x4 BoxFrame(const MarkupWorldVolume& volume)
{
    Mathematics::Matrix4x4 frame;
    for (int axis = 0; axis < 3; ++axis)
        frame[axis] = {volume.Axes[axis].x, volume.Axes[axis].y, volume.Axes[axis].z, 0.0f};
    frame[3] = {volume.Center.x, volume.Center.y, volume.Center.z, 1.0f};
    return frame;
}

void DrawFill(GizmoRenderContext& context, const MarkupWorldVolume& volume, const Color& color)
{
    if (volume.Shape == MarkupVolumeShape::Sphere)
        context.DrawSolidSphere(volume.Center, volume.BoundingRadius, color);
    else
        context.DrawSolidOrientedBox(volume.Center, volume.HalfExtents, volume.Axes, color);
}

// The outline grown by `grow` meters; a sphere draws three rings when `rings`, else the one
// circle facing the camera.
void DrawOutline(GizmoRenderContext& context, const MarkupWorldVolume& volume, const Color& color, float grow,
                 bool rings, const Vector3& cameraPos)
{
    if (volume.Shape == MarkupVolumeShape::Sphere)
    {
        const float radius = volume.BoundingRadius + grow;
        if (rings)
        {
            SceneTools::DrawWireEllipsoid(context, volume.Center, volume.Axes[0], volume.Axes[1], volume.Axes[2],
                                          Vector3(radius, radius, radius), color, kLineThickness);
            return;
        }
        const Vector3 toCamera = (cameraPos - volume.Center).NormalizeOrZero();
        context.DrawWireCircle(volume.Center, toCamera.LengthSquared() > 0.0f ? toCamera : Vector3(0.0f, 1.0f, 0.0f),
                               radius, color, kLineThickness);
        return;
    }
    const Vector3 half = volume.HalfExtents + Vector3(grow, grow, grow);
    context.DrawTransformedWireBox(half * -1.0f, half, BoxFrame(volume), color, kLineThickness);
}

// A region's walls and lid as one triangle list.
void DrawRegionFill(GizmoRenderContext& context, const MarkupECS::MarkupRegionMesh& mesh, const Color& color,
                    std::vector<Vector3>& scratch)
{
    scratch.clear();
    scratch.reserve(mesh.Vertices.size());
    for (const MarkupECS::MarkupRegionVertex& vertex : mesh.Vertices)
        scratch.push_back(vertex.Position);
    if (!scratch.empty())
        context.DrawTriangles(scratch.data(), scratch.size(), color);
}

// The outward normal on the ground plane at ring point `i` (the ring counter-clockwise from
// above, not closed by a repeated point): the mean of its two edges' outward normals.
Vector3 RingOutward(std::span<const Vector3> ring, std::size_t i)
{
    const Vector3& previous = ring[(i + ring.size() - 1) % ring.size()];
    const Vector3& next = ring[(i + 1) % ring.size()];
    const Vector3 along = next - previous;
    return Vector3(along.z, 0.0f, -along.x).NormalizeOrZero();
}

// One sample of a path's band as `cameraPos` sees it: the line's point and the band's left and
// right edges on the ground beside it (MarkupRegionGround::EdgeGround), all lifted. The half width
// and the lift follow the sample's own distance (kPathWidthPerMeter and kPathLiftPerMeter of it,
// never under their minimums), so a long path keeps a few pixels of band from its near end to its
// far one.
struct PathBandSample
{
    Vector3 Left;
    Vector3 Center;
    Vector3 Right;
};

PathBandSample PathBandAt(const MarkupECS::MarkupRegionGround& ground, std::size_t i, const Vector3& cameraPos)
{
    const Vector3& point = ground.Outline[i];
    const float distance = (point - cameraPos).Length();
    const float halfWidth = std::max(kPathBandHalfWidth, distance * kPathWidthPerMeter);
    const float lift = std::max(kPathMinLift, distance * kPathLiftPerMeter);
    const Vector3 across = MarkupECS::PathAcross(ground.Outline, i) * halfWidth;
    const Vector3 left = point - across;
    const Vector3 right = point + across;
    return PathBandSample{Vector3(left.x, MarkupECS::PathEdgeHeight(ground, i, -1.0f, halfWidth) + lift, left.z),
                          point + Vector3(0.0f, lift, 0.0f),
                          Vector3(right.x, MarkupECS::PathEdgeHeight(ground, i, 1.0f, halfWidth) + lift, right.z)};
}

// A path seen from `cameraPos`: its line along the draped samples and a translucent band either
// side of it on the ground, all lifted off it, so the way reads as a glowing ribbon on the terrain;
// a selected path's line is doubled at the band's edges.
void DrawPath(GizmoRenderContext& context, const MarkupECS::MarkupRegionGround& ground, const Vector3& cameraPos,
              const Color& lineColor, const Color& bandColor, bool selected, std::vector<Vector3>& scratch)
{
    const std::vector<Vector3>& line = ground.Outline;
    if (line.size() < 2)
        return;
    scratch.clear();
    PathBandSample a = PathBandAt(ground, 0, cameraPos);
    for (std::size_t i = 0; i + 1 < line.size(); ++i)
    {
        const PathBandSample b = PathBandAt(ground, i + 1, cameraPos);
        scratch.insert(scratch.end(), {a.Left, b.Left, b.Center, a.Left, b.Center, a.Center, a.Center, b.Center,
                                       b.Right, a.Center, b.Right, a.Right});
        a = b;
    }
    context.SetTriangleLayer(kFillLayer);
    context.DrawTriangles(scratch.data(), scratch.size(), bandColor);
    context.SetTriangleLayer(0);
    scratch.clear();
    a = PathBandAt(ground, 0, cameraPos);
    for (std::size_t i = 0; i + 1 < line.size(); ++i)
    {
        const PathBandSample b = PathBandAt(ground, i + 1, cameraPos);
        scratch.insert(scratch.end(), {a.Center, b.Center});
        if (selected)
            scratch.insert(scratch.end(), {a.Left, b.Left, a.Right, b.Right});
        a = b;
    }
    context.DrawColoredLines(scratch.data(), scratch.size() / 2, lineColor, kLineThickness);
}

// The region's rings at their base and along their top edge, `grow` meters out of each ring on
// the ground plane; an outline that is not closed, at its base only.
void DrawRegionOutline(GizmoRenderContext& context, const MarkupECS::MarkupRegionDisplayCache::Entry& region,
                       const Color& color, float grow, std::vector<Vector3>& scratch)
{
    const MarkupECS::MarkupRegionGround& ground = region.Ground;
    const bool closed = ground.Closed;
    const Vector3 lift(0.0f, region.ExtrudeHeight, 0.0f);
    scratch.clear();
    for (std::size_t r = 0; r < ground.GetRingCount(); ++r)
    {
        const std::span<const Vector3> ring = ground.GetRing(r);
        const std::size_t edges = closed ? ring.size() : ring.size() - 1;
        for (std::size_t i = 0; i < edges && ring.size() >= 2; ++i)
        {
            const std::size_t next = (i + 1) % ring.size();
            Vector3 from = ring[i];
            Vector3 to = ring[next];
            if (closed && grow > 0.0f)
            {
                from = from + RingOutward(ring, i) * grow;
                to = to + RingOutward(ring, next) * grow;
            }
            scratch.insert(scratch.end(), {from, to});
            if (closed)
                scratch.insert(scratch.end(), {from + lift, to + lift});
        }
    }
    if (!scratch.empty())
        context.DrawColoredLines(scratch.data(), scratch.size() / 2, color, kLineThickness);
}

} // namespace

void MarkupGizmo::SetViewFrustum(const Mathematics::Vector4* planes)
{
    m_HasFrustum = planes != nullptr;
    if (planes)
        std::copy(planes, planes + m_Frustum.size(), m_Frustum.begin());
}

Color MarkupGizmo::OutlineColor(const MarkupDrawItem& item)
{
    if (item.Hovered && !item.Selected)
        return Color(kMarkupAccentRgb[0], kMarkupAccentRgb[1], kMarkupAccentRgb[2], kHighlightOutlineAlpha);
    const float alpha = item.Selected ? kHighlightOutlineAlpha : kOutlineAlpha;
    return Color(item.Rgb[0], item.Rgb[1], item.Rgb[2], alpha);
}

void MarkupGizmo::Render(GizmoRenderContext& context)
{
    const MarkupEditorBridge* bridge = MarkupEditorBridge::TryGet();
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (!bridge || !world)
        return;
    const Vector3* cameraPos = context.GetCameraWorldPosition();
    const Application* application = Application::Get();
    Draw(context, *world, *bridge, bridge->GetHighlight(), cameraPos ? *cameraPos : Vector3(0.0f, 0.0f, 0.0f),
         application ? application->GetFrameCount() : 0);
}

void MarkupGizmo::Draw(GizmoRenderContext& context, ECS::World& world, const MarkupEditorBridge& bridge,
                       const MarkupHighlightState& highlight, const Vector3& cameraPos, uint64 frame)
{
    std::size_t filled = m_CollectedBodies;
    if (!m_Collected)
        filled = CollectMarkupDrawItems(world, bridge, highlight, cameraPos, m_HasFrustum ? m_Frustum.data() : nullptr,
                                        frame, m_Items, m_ExcludedScratch);
    const std::vector<MarkupDrawItem>& items = m_Collected ? *m_Collected : m_Items;
    if (items.empty())
        return;

    context.SetTriangleLayer(kFillLayer);
    for (std::size_t index = 0; m_DrawsFills && index < filled; ++index)
    {
        const MarkupDrawItem& item = items[index];
        if (!item.DrawsBody() || item.Path)
            continue;
        const std::array<float32, 3> rgb = item.Hovered ? MarkupHoverRgb(item.Rgb) : item.Rgb;
        const Color color(rgb[0], rgb[1], rgb[2], item.Selected || item.Hovered ? kSelectedFillAlpha : kFillAlpha);
        if (item.Region)
            DrawRegionFill(context, item.Region->Mesh, color, m_Scratch);
        else
            DrawFill(context, item.Volume, color);
    }
    context.SetTriangleLayer(0);

    for (std::size_t index = 0; index < items.size(); ++index)
    {
        const MarkupDrawItem& item = items[index];
        if (item.Path)
        {
            const std::array<float32, 3> rgb = item.Hovered ? MarkupHoverRgb(item.Rgb) : item.Rgb;
            const bool highlighted = item.Selected || item.Hovered;
            DrawPath(context, item.Region->Ground, cameraPos, OutlineColor(item),
                     Color(rgb[0], rgb[1], rgb[2], highlighted ? kHighlightPathBandAlpha : kPathBandAlpha), item.Selected,
                     m_Scratch);
            continue;
        }
        if (item.Region)
        {
            DrawRegionOutline(context, *item.Region, OutlineColor(item), 0.0f, m_Scratch);
            if (item.Selected)
                DrawRegionOutline(context, *item.Region, OutlineColor(item),
                                  item.Volume.BoundingRadius * kSelectedOutlineGrow, m_Scratch);
            continue;
        }
        // A filled sphere draws its rings; under the glow, whose rim already draws its silhouette,
        // the one circle facing the camera, as past the cap. From inside a sphere no circle faces
        // the camera: it draws its rings.
        const bool rings = (m_DrawsFills && index < filled) || item.CameraInside;
        DrawOutline(context, item.Volume, OutlineColor(item), 0.0f, rings, cameraPos);
        if (item.Selected)
            DrawOutline(context, item.Volume, OutlineColor(item), item.Volume.BoundingRadius * kSelectedOutlineGrow,
                        rings, cameraPos);
    }
}

} // namespace GameEngine::Editor
