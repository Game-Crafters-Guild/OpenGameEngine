#include "UI/Controls/Widgets/ViewportRotationGizmo.h"

#include "Editor/Settings/SceneViewSettings.h"
#include "Panels/SceneViewPanel.h"
#include "Rendering/Text/FontAtlas.h"
#include "SceneViewController.h"
#include "UI/Registration/ElementRegistration.h"
#include "UI/ResolvedStyle.h"
#include "UI/UIEvents.h"
#include "UI/UIPrimitive.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace GameEngine
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;

inline float ToRad(float deg) { return deg * (kPi / 180.0f); }

struct Vec3 { float X = 0, Y = 0, Z = 0; };

inline Vec3 Normalize(Vec3 v)
{
    float len = std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
    if (len > 1e-6f) { v.X /= len; v.Y /= len; v.Z /= len; }
    return v;
}

inline Vec3 Cross(Vec3 a, Vec3 b)
{
    return { a.Y * b.Z - a.Z * b.Y,
             a.Z * b.X - a.X * b.Z,
             a.X * b.Y - a.Y * b.X };
}

inline Vec3 Forward(float yawDeg, float pitchDeg)
{
    const float y = ToRad(yawDeg);
    const float p = ToRad(pitchDeg);
    return { std::cos(p) * std::cos(y), std::sin(p), std::cos(p) * std::sin(y) };
}

// Axis colors (RGB, 0-255). Index 0..2: +X, +Y, +Z. Negatives share the same
// hue but render as outlined circles.
constexpr uint8_t kAxisR[3] = {0xE0, 0x60, 0x4B};
constexpr uint8_t kAxisG[3] = {0x50, 0xD0, 0x8A};
constexpr uint8_t kAxisB[3] = {0x50, 0x4A, 0xE0};

// Target yaw/pitch for each axis view (index 0..5: +X, +Y, +Z, -X, -Y, -Z).
// "View from +X" means the camera sits on the +X side and looks in -X.
struct AxisView { float Yaw; float Pitch; };
constexpr AxisView kAxisViews[6] = {
    {180.0f,   0.0f}, // +X : look -X
    { 90.0f, -89.9f}, // +Y : top, look down
    {-90.0f,   0.0f}, // +Z : look -Z
    {  0.0f,   0.0f}, // -X : look +X
    { 90.0f,  89.9f}, // -Y : bottom, look up
    { 90.0f,   0.0f}, // -Z : look +Z (engine default)
};
} // namespace

ViewportRotationGizmo::ViewportRotationGizmo()
{
    AddClass("viewport-rotation-gizmo");

    RegisterEventHandler(kEventMouseDown,  [this](UIEvent& e) { OnMouseDown(e); });
    RegisterEventHandler(kEventMouseUp,    [this](UIEvent& e) { OnMouseUp(e); });
    RegisterEventHandler(kEventMouseMove,  [this](UIEvent& e) { OnMouseMove(e); });
    RegisterEventHandler(kEventMouseLeave, [this](UIEvent& e) { OnMouseLeave(e); });
}

void ViewportRotationGizmo::Tick()
{
    // Honor the SceneView "Show Rotation Gizmo" preference. Toggling the
    // "hidden" class removes the element from layout and hit testing so it
    // neither draws nor steals mouse events when disabled.
    const auto& settings = Editor::SceneViewSettings::Get();
    bool showForViewport = settings.GetShowRotationGizmoForViewport(m_ViewportIndex);
    if (m_ViewportIndex == 0 && (!m_Panel || !m_Panel->IsQuadViewEnabled()))
        showForViewport = true;
    const bool show = settings.GetShowRotationGizmo() && showForViewport;
    if (!show)
    {
        if (!HasClass("hidden"))
            AddClass("hidden");
        return;
    }
    if (HasClass("hidden"))
        RemoveClass("hidden");

    // Sync the corner anchor class with the user's SceneView preference.
    // Exactly one corner-* class is active at a time.
    {
        static const char* kAllCorners[] = {
            "corner-top-right", "corner-top-left", "corner-bottom-right", "corner-bottom-left"
        };
        const int wantedIdx = static_cast<int>(Editor::SceneViewSettings::Get().GetRotationGizmoCorner());
        for (int i = 0; i < 4; ++i)
        {
            if (i == wantedIdx)
            {
                if (!HasClass(kAllCorners[i]))
                    AddClass(kAllCorners[i]);
            }
            else if (HasClass(kAllCorners[i]))
            {
                RemoveClass(kAllCorners[i]);
            }
        }
    }

    // Shift the gizmo down only if the FPS/perf label overlaps its natural
    // top-corner rect. Two guards against flicker:
    //   1. Test against the *natural* (unshifted) gizmo rect. If we tested
    //      against the current rect, applying fps-active would move the
    //      gizmo out from under the label → remove class → overlap again →
    //      oscillation every frame.
    //   2. Hysteresis margin — require a small inward overlap to enter and
    //      a small gap to exit, so pixel-thin grazing cases don't oscillate.
    bool fpsOverlaps = false;
    if (m_Panel && m_Panel->IsFpsVisible())
    {
        if (const UIElement* fps = m_Panel->GetFpsLabelElement())
        {
            float gx = GetLayoutX();
            float gy = GetLayoutY();
            const float gw = GetLayoutWidth();
            const float gh = GetLayoutHeight();
            const float fx = fps->GetLayoutX();
            const float fy = fps->GetLayoutY();
            const float fw = fps->GetLayoutWidth();
            const float fh = fps->GetLayoutHeight();

            // Undo the fps-active shift (CSS: top 10px → 42px) so the test
            // is against the natural rect regardless of current state.
            const bool currentlyActive = HasClass("fps-active");
            if (currentlyActive)
                gy -= 32.0f;

            if (gw > 0.0f && gh > 0.0f && fw > 0.0f && fh > 0.0f)
            {
                // Margin: positive expands the gizmo rect (easier to enter
                // / harder to exit — enter=4, exit=8 gives clean hysteresis).
                const float margin = currentlyActive ? 8.0f : 4.0f;
                const float rx = gx - margin;
                const float ry = gy - margin;
                const float rw = gw + margin * 2.0f;
                const float rh = gh + margin * 2.0f;
                fpsOverlaps = !(fx + fw <= rx || rx + rw <= fx
                             || fy + fh <= ry || ry + rh <= fy);
            }
        }
    }
    if (fpsOverlaps)
    {
        if (!HasClass("fps-active"))
            AddClass("fps-active");
    }
    else if (HasClass("fps-active"))
    {
        RemoveClass("fps-active");
    }

    if (!m_Controller) return;
    const auto pose = m_Controller->GetCameraPose();
    if (pose.YawDeg != m_LastYaw || pose.PitchDeg != m_LastPitch)
    {
        m_LastYaw   = pose.YawDeg;
        m_LastPitch = pose.PitchDeg;
        MarkDirty(VisualDirty);
    }
}

void ViewportRotationGizmo::BuildAxes(float centerX, float centerY, float radius, Axis2D (&out)[6]) const
{
    // Construct the Scene View camera basis from yaw/pitch so we can express
    // each world axis in camera-local coordinates, then project to 2D.
    float yawDeg = 90.0f;
    float pitchDeg = 0.0f;
    if (m_Controller)
    {
        const auto pose = m_Controller->GetCameraPose();
        yawDeg = pose.YawDeg;
        pitchDeg = pose.PitchDeg;
    }

    const Vec3 fwd = Normalize(Forward(yawDeg, pitchDeg));
    const Vec3 worldUp{0.0f, 1.0f, 0.0f};
    Vec3 right = Normalize(Cross(worldUp, fwd));
    // Guard against gimbal when looking straight up/down.
    if (std::abs(right.X) + std::abs(right.Y) + std::abs(right.Z) < 1e-4f)
        right = {1.0f, 0.0f, 0.0f};
    const Vec3 up = Normalize(Cross(fwd, right));

    // Axis 3D directions in world space (X,Y,Z,-X,-Y,-Z).
    const Vec3 dirs[6] = {
        { 1,  0,  0}, { 0,  1,  0}, { 0,  0,  1},
        {-1,  0,  0}, { 0, -1,  0}, { 0,  0, -1},
    };

    // Shrink by the marker radius so +X and -X don't spill outside the circle.
    const float markerR = std::max(6.0f, radius * 0.18f);
    const float r = std::max(0.0f, radius - markerR - 2.0f);

    for (int i = 0; i < 6; ++i)
    {
        const Vec3 d = dirs[i];
        // World axis in camera-local coords: (right·d, up·d, fwd·d).
        const float lx = right.X * d.X + right.Y * d.Y + right.Z * d.Z;
        const float ly = up.X    * d.X + up.Y    * d.Y + up.Z    * d.Z;
        const float lz = fwd.X   * d.X + fwd.Y   * d.Y + fwd.Z   * d.Z;

        out[i].Index   = i;
        out[i].ScreenX = centerX + lx * r;
        out[i].ScreenY = centerY - ly * r; // flip Y (screen is Y-down)
        out[i].Z       = lz;
    }
}

int ViewportRotationGizmo::HitTestAxis(float localX, float localY,
                                       float centerX, float centerY, float radius) const
{
    Axis2D axes[6];
    BuildAxes(centerX, centerY, radius, axes);
    const float markerR = std::max(6.0f, radius * 0.18f);
    int hit = -1;
    float bestZ = 1e9f;
    for (const auto& a : axes)
    {
        const float dx = localX - a.ScreenX;
        const float dy = localY - a.ScreenY;
        if (dx * dx + dy * dy <= markerR * markerR)
        {
            if (a.Z < bestZ) // pick the one closest to camera
            {
                bestZ = a.Z;
                hit = a.Index;
            }
        }
    }
    return hit;
}

void ViewportRotationGizmo::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
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

    if (w <= 0.0f || h <= 0.0f) return;

    const float centerX = x + w * 0.5f;
    const float centerY = y + h * 0.5f;
    const float radius  = std::min(w, h) * 0.5f - 2.0f;
    if (radius <= 4.0f) return;

    // Background disc. Currently disabled so the gizmo floats cleanly over
    // the viewport; flip kDrawBackground to re-enable the interaction tint.
    constexpr bool kDrawBackground = false;
    const bool active = m_Orbiting || m_HoveredAxis >= 0;
	if constexpr (kDrawBackground)
    {
        if (active)
        {
            ctx.Emit(MakeRect(centerX - radius, centerY - radius,
                              radius * 2.0f, radius * 2.0f,
                              PackColor(0.05f, 0.05f, 0.05f, 0.45f),
                              radius, radius, radius, radius));
        }
    }

    Axis2D axes[6];
    BuildAxes(centerX, centerY, radius, axes);

    // Sort back-to-front (largest Z first) so nearer axes draw on top.
    std::sort(std::begin(axes), std::end(axes),
              [](const Axis2D& a, const Axis2D& b) { return a.Z > b.Z; });

    const float markerR = std::max(6.0f, radius * 0.18f);

    for (const Axis2D& a : axes)
    {
        const int i    = a.Index;
        const int ci   = i % 3;
        const bool pos = (i < 3);

        // Alpha fades for axes pointing away from the camera.
        // a.Z in [-1,1]; +1 means directly toward +Z camera (away from viewer).
        float alpha = 0.70f - a.Z * 0.35f;
        alpha = std::clamp(alpha, 0.45f, 1.0f);

        const float rN = kAxisR[ci] / 255.0f;
        const float gN = kAxisG[ci] / 255.0f;
        const float bN = kAxisB[ci] / 255.0f;
        const uint32_t color = PackColor(rN, gN, bN, alpha);

        if (pos)
        {
            // Line from center to axis marker.
            ctx.Emit(MakeLine(centerX, centerY, a.ScreenX, a.ScreenY, 1.5f,
                              PackColor(rN, gN, bN, alpha * 0.9f)));

            // Filled marker circle.
            ctx.Emit(MakeRect(a.ScreenX - markerR, a.ScreenY - markerR,
                              markerR * 2.0f, markerR * 2.0f,
                              color, markerR, markerR, markerR, markerR));
        }
        else
        {
            // Outlined marker for negative axes. Gets a small opacity boost
            // when the ring is in the foreground (a.Z <= 0) so it reads
            // clearly against the viewport.
            float ringAlpha = alpha;
            if (a.Z <= 0.0f)
                ringAlpha = std::min(1.0f, ringAlpha + 0.15f);
            UIPrimitive ring = MakeRect(a.ScreenX - markerR, a.ScreenY - markerR,
                                        markerR * 2.0f, markerR * 2.0f,
                                        PackColor(0.0f, 0.0f, 0.0f, 0.0f),
                                        markerR, markerR, markerR, markerR);
            AddBorder(ring, 2.0f, PackColor(rN, gN, bN, ringAlpha));
            ctx.Emit(ring);
        }

        // Highlight outline when hovered — uses the axis's own color boosted
        // toward white so the hover ring is clearly brighter than the marker.
        if (m_HoveredAxis == i)
        {
            const float hR = markerR + 2.0f;
            UIPrimitive hl = MakeRect(a.ScreenX - hR, a.ScreenY - hR,
                                      hR * 2.0f, hR * 2.0f,
                                      PackColor(0.0f, 0.0f, 0.0f, 0.0f),
                                      hR, hR, hR, hR);
            constexpr float kHoverBrighten = 0.45f; // lerp toward white
            const float hrC = rN + (1.0f - rN) * kHoverBrighten;
            const float hgC = gN + (1.0f - gN) * kHoverBrighten;
            const float hbC = bN + (1.0f - bN) * kHoverBrighten;
            AddBorder(hl, 2.5f, PackColor(hrC, hgC, hbC, 1.0f));
            ctx.Emit(hl);
        }

        // Axis label text for positive axes — centered on the marker using
        // the glyph's ink bounds (line metrics include leading and would
        // push the baseline too low, leaving the letter biased downward).
        if (pos && ctx.FontAtlas)
        {
            const char* labels[3] = {"X", "Y", "Z"};
            const float fontSize = std::max(9.0f, markerR * 1.15f);
            const float pixelSize = fontSize;

            static thread_local Rendering::Text::FontAtlas::ShapeResult s_AxisShape;
            ctx.FontAtlas->ShapeText(labels[ci], pixelSize, s_AxisShape, 0xFFFFFFFFu);

            float inkMinX = 0.0f, inkMaxX = 0.0f;
            float inkMinY = 0.0f, inkMaxY = 0.0f;
            if (!s_AxisShape.glyphs.empty())
            {
                inkMinX = 1e30f; inkMaxX = -1e30f;
                inkMinY = 1e30f; inkMaxY = -1e30f;
                for (const auto& gp : s_AxisShape.glyphs)
                {
                    inkMinX = std::min(inkMinX, gp.x);
                    inkMaxX = std::max(inkMaxX, gp.x + gp.width);
                    inkMinY = std::min(inkMinY, gp.y);
                    inkMaxY = std::max(inkMaxY, gp.y + gp.height);
                }
            }
            const float inkW = std::max(1.0f, inkMaxX - inkMinX);
            const float inkH = std::max(1.0f, inkMaxY - inkMinY);
            const float tx = a.ScreenX - inkW * 0.5f - inkMinX;
            const float ty = a.ScreenY - inkH * 0.5f - inkMinY;
            // fontSize is already physical (derived from the physical-px radius).
            // EmitText internally multiplies by contentScale, so pass logical
            // to avoid double-scaling.
            const float logicalFontSize = (ctx.ContentScale > 0.0f) ? (fontSize / ctx.ContentScale) : fontSize;
            ctx.EmitText(labels[ci], tx, ty, logicalFontSize, 0xFFFFFFFFu, ctx.FontAtlas);
        }
    }
}

void ViewportRotationGizmo::OnMouseDown(UIEvent& ev)
{
    if (ev.Button != 0) return; // left button only
    if (m_Panel)
        m_Panel->ActivateViewportForController(m_Controller);

    const float lx = GetLayoutX();
    const float ly = GetLayoutY();
    const float w  = GetLayoutWidth();
    const float h  = GetLayoutHeight();
    const float centerX = lx + w * 0.5f;
    const float centerY = ly + h * 0.5f;
    const float radius  = std::min(w, h) * 0.5f - 2.0f;
    if (radius <= 4.0f) return;

    const float dx = ev.X - centerX;
    const float dy = ev.Y - centerY;
    if (dx * dx + dy * dy > radius * radius)
        return;

    m_PressedInside = true;
    m_PressX = ev.X;
    m_PressY = ev.Y;
    m_LastX  = ev.X;
    m_LastY  = ev.Y;

    ev.Capture(this);
    ev.Stop();

    MarkDirty(VisualDirty);
}

void ViewportRotationGizmo::OnMouseMove(UIEvent& ev)
{
    const float lx = GetLayoutX();
    const float ly = GetLayoutY();
    const float w  = GetLayoutWidth();
    const float h  = GetLayoutHeight();
    const float centerX = lx + w * 0.5f;
    const float centerY = ly + h * 0.5f;
    const float radius  = std::min(w, h) * 0.5f - 2.0f;

    if (m_PressedInside)
    {
        const float dxFromPress = ev.X - m_PressX;
        const float dyFromPress = ev.Y - m_PressY;
        // Start orbit once the cursor has moved past a small threshold.
        if (!m_Orbiting && (dxFromPress * dxFromPress + dyFromPress * dyFromPress) > 16.0f)
        {
            m_Orbiting = true;
            if (m_Panel)
                m_Panel->NotifyRotationGizmoOrbit(m_ViewportIndex);
        }

        if (m_Orbiting && m_Panel)
        {
            const float dx = ev.X - m_LastX;
            const float dy = ev.Y - m_LastY;
            // Route through SceneViewPanel so the panel's stored yaw/pitch
            // stay in sync (the panel pushes SetCameraAnglesDeg every frame).
            const float sensitivity = 0.4f; // degrees per pixel
            m_Panel->ApplyLookDelta(dx * sensitivity, dy * sensitivity);
        }

        m_LastX = ev.X;
        m_LastY = ev.Y;
        ev.Stop();
    }

    // Update hover state (only meaningful when not actively orbiting).
    const int prevHover = m_HoveredAxis;
    if (!m_Orbiting && radius > 4.0f)
        m_HoveredAxis = HitTestAxis(ev.X, ev.Y, centerX, centerY, radius);
    else
        m_HoveredAxis = -1;

    if (m_HoveredAxis != prevHover)
        MarkDirty(VisualDirty);
}

void ViewportRotationGizmo::OnMouseUp(UIEvent& ev)
{
    if (ev.Button != 0) return;
    if (!m_PressedInside) return;

    const bool wasOrbiting = m_Orbiting;
    m_PressedInside = false;
    m_Orbiting = false;

    if (!wasOrbiting)
    {
        const float lx = GetLayoutX();
        const float ly = GetLayoutY();
        const float w  = GetLayoutWidth();
        const float h  = GetLayoutHeight();
        const float centerX = lx + w * 0.5f;
        const float centerY = ly + h * 0.5f;
        const float radius  = std::min(w, h) * 0.5f - 2.0f;
        if (radius > 4.0f)
        {
            // Check the central hotspot BEFORE axis hit testing. An axis
            // pointing nearly along the camera forward direction projects
            // onto (or very near) the gizmo center, which would otherwise
            // swallow the center click and snap the camera 180° instead of
            // toggling ortho/perspective.
            const float markerR = std::max(6.0f, radius * 0.18f);
            const float cdx = ev.X - centerX;
            const float cdy = ev.Y - centerY;
            const bool inCenterHotspot =
                (cdx * cdx + cdy * cdy) <= (markerR * 0.75f) * (markerR * 0.75f);

            if (inCenterHotspot && m_Controller)
            {
                ToggleGameProjection();
            }
            else
            {
                const int axis = HitTestAxis(ev.X, ev.Y, centerX, centerY, radius);
                if (axis >= 0)
                    SnapToAxis(axis);
            }
        }
    }

    ev.Stop();
    MarkDirty(VisualDirty);
}

void ViewportRotationGizmo::OnMouseLeave(UIEvent& /*ev*/)
{
    if (m_HoveredAxis != -1)
    {
        m_HoveredAxis = -1;
        MarkDirty(VisualDirty);
    }
}

void ViewportRotationGizmo::ToggleGameProjection()
{
    if (!m_Controller) return;
    const auto current = m_Controller->GetCameraPose();
    const bool enteringOrtho = !m_Controller->IsOrthographic() && !current.Is2D;
    if (enteringOrtho)
    {
        m_PerspectiveYaw = current.YawDeg;
        m_PerspectivePitch = current.PitchDeg;
        m_HasPerspectiveAngle = true;
    }

    // Select a free 3D view so the preset can still be rotated afterward.
    if (m_Panel) m_Panel->NotifyRotationGizmoOrbit(m_ViewportIndex);
    m_Controller->SetFixedViewOrientation(SceneViewController::FixedViewOrientation::Free);
    m_Controller->Set2DMode(false);
    m_Controller->SetOrthographic(enteringOrtho);

    auto target = current;
    target.Is2D = false;
    if (enteringOrtho)
    {
        target.YawDeg = 45.0f;
        target.PitchDeg = -30.0f; // 2:1 ground-plane diagonals for pixel-art game views.
    }
    else if (m_HasPerspectiveAngle)
    {
        target.YawDeg = m_PerspectiveYaw;
        target.PitchDeg = m_PerspectivePitch;
    }

    // Keep the point being viewed centered, including any pan/zoom made in
    // ortho. Only the viewing angle is restored when returning to perspective.
    const Vec3 oldForward = Normalize(Forward(current.YawDeg, current.PitchDeg));
    const Vec3 newForward = Normalize(Forward(target.YawDeg, target.PitchDeg));
    target.Pos[0] += (oldForward.X - newForward.X) * current.Distance;
    target.Pos[1] += (oldForward.Y - newForward.Y) * current.Distance;
    target.Pos[2] += (oldForward.Z - newForward.Z) * current.Distance;
    m_Controller->StartCameraTween(target, 0.35f);
}

void ViewportRotationGizmo::SnapToAxis(int axis)
{
    if (!m_Controller) return;
    if (axis < 0 || axis > 5) return;
    if (m_Panel)
        m_Panel->NotifyRotationGizmoAxisSnap(m_ViewportIndex, axis);

    auto current = m_Controller->GetCameraPose();

    // Compute the orbit pivot: point the camera is currently looking at.
    const Vec3 curFwd = Normalize(Forward(current.YawDeg, current.PitchDeg));
    const float dist = (current.Distance > 0.01f) ? current.Distance : 6.0f;
    const float pivotX = current.Pos[0] + curFwd.X * dist;
    const float pivotY = current.Pos[1] + curFwd.Y * dist;
    const float pivotZ = current.Pos[2] + curFwd.Z * dist;

    // New orientation.
    SceneViewCameraPose target = current;
    target.YawDeg   = kAxisViews[axis].Yaw;
    target.PitchDeg = kAxisViews[axis].Pitch;
    target.Is2D     = false;

    // Place the camera so that it still looks at the same pivot at the same distance.
    const Vec3 newFwd = Normalize(Forward(target.YawDeg, target.PitchDeg));
    target.Pos[0] = pivotX - newFwd.X * dist;
    target.Pos[1] = pivotY - newFwd.Y * dist;
    target.Pos[2] = pivotZ - newFwd.Z * dist;

    m_Controller->StartCameraTween(target, 0.35f);
}

} // namespace GameEngine

namespace RegisterWidgets
{
static auto s_reg_viewportRotationGizmo =
    GameEngine::UIRegistration::RegisterWithFactory<GameEngine::ViewportRotationGizmo>(
        "ViewportRotationGizmo",
        []() { return std::make_unique<GameEngine::ViewportRotationGizmo>(); })
    .TagAlias("viewportrotationgizmo");
}
