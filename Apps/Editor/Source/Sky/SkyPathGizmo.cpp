#include "Sky/SkyPathGizmo.h"

#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunPath.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Sky/SolarPath.h"
#include "SceneView/ComponentGizmoRegistry.h"
#include "SceneView/SceneViewGizmos.h"
#include "Sky/SkySunDayKind.h"
#include "Types/Color.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace GameEngine::Editor
{
namespace
{
using Mathematics::Vector3;
using SceneTools::GizmoRenderContext;

constexpr float kPi = 3.14159265f;
constexpr float kTwoPi = 2.0f * kPi;
constexpr float kHoursPerDay = 24.0f;
constexpr float kNoonHours = 12.0f;

// The dome keeps about the same size on screen: a fraction of the camera's distance, or of the
// orthographic view's height, never smaller than a metre.
constexpr float kDomeScreenFraction = 0.3f;
constexpr float kMinimumDomeRadius = 1.0f;
constexpr float kFallbackDomeRadius = 10.0f;
// About how many screen pixels one dome radius spans at the default 60-degree field of view in a
// view about 900 pixels tall. The gizmo context carries no projection, so stroke widths are set
// from this: close to the stated pixels, not exact.
constexpr float kPixelsPerDomeRadius = 240.0f;
// Every stroke is a bright core over a dark halo one pixel wider on each side, so it reads against
// a bright sky and a pale ground alike.
constexpr float kCorePixels = 2.0f;
constexpr float kHaloPixels = 4.0f;
// Triangle layers below every other gizmo's (0 and up), so the transform handles draw over the dome;
// the halo below the core.
constexpr std::int32_t kHaloLayer = -2;
constexpr std::int32_t kCoreLayer = -1;
// Segments of the day's circle (one every 15 minutes), of the horizon ring and of a marker ring.
constexpr int kDayCircleSegments = 96;
constexpr int kRingSegments = 96;
constexpr int kMarkerSegments = 24;
// The axis arrow runs past the dome so it reads as leaving it.
constexpr float kAxisLengthScale = 1.2f;
constexpr float kArrowHeadScale = 0.08f;
// The Custom axis heading's tick straddles the horizon ring; north's triangle stands outside it.
constexpr float kHeadingTickInnerScale = 0.85f;
constexpr float kHeadingTickOuterScale = 1.15f;
constexpr float kNorthMarkInnerScale = 1.03f;
constexpr float kNorthMarkOuterScale = 1.22f;
constexpr float kNorthMarkHalfWidthScale = 0.06f;
constexpr float kMarkerRadiusScale = 0.04f;
constexpr float kSunDotRadiusScale = 0.035f;
constexpr float kMinimumLengthSquared = 1e-10f;
// A direction this close to straight up or down takes its marker ring's plane from the frame's east.
constexpr float kNearlyVerticalUp = 0.9f;

// Opaque colours: the halo's layering needs every stroke opaque. The axis is cyan so it never
// matches a sky; north is compass red; the day's arc is amber, its night half the same amber dimmed.
const Color kHaloColor(0.04f, 0.04f, 0.06f, 1.0f);
const Color kHorizonColor(0.90f, 0.92f, 0.95f, 1.0f);
const Color kAxisColor(0.15f, 0.95f, 0.95f, 1.0f);
const Color kNorthColor(0.95f, 0.25f, 0.20f, 1.0f);
const Color kDayArcColor(1.0f, 0.72f, 0.10f, 1.0f);
const Color kNightArcColor(0.50f, 0.36f, 0.05f, 1.0f);
const Color kNoonColor(1.0f, 0.95f, 0.70f, 1.0f);
const Color kHorizonCrossingColor(1.0f, 0.55f, 0.20f, 1.0f);
const Color kSunColor(1.0f, 0.90f, 0.45f, 1.0f);

struct Dome
{
    Vector3 Center;
    float Radius = kFallbackDomeRadius;
    Vector3 Camera;
    bool HasCamera = false;
    // A perspective view: what a pixel spans grows with the distance from the camera.
    bool Perspective = false;
};

// `v` at unit length; a vector too short to have a reliable direction is left as it is.
void Normalise(Vector3& v)
{
    if (v.LengthSquared() <= kMinimumLengthSquared)
        return;
    v = v.NormalizeOrZero();
}

float Distance(const Vector3& a, const Vector3& b)
{
    return std::sqrt((a - b).LengthSquared());
}

Dome DomeFor(const GizmoRenderContext& context, const ECS::World& world, ECS::EntityHandle entity)
{
    Dome dome;
    if (const auto* xf = world.GetComponent<Components::WorldTransform>(entity))
        dome.Center = Vector3(xf->matrix[12], xf->matrix[13], xf->matrix[14]);
    if (const Vector3* camera = context.GetCameraWorldPosition())
    {
        dome.Camera = *camera;
        dome.HasCamera = true;
    }
    if (context.HasOrthoHeight() && context.GetOrthoHeight() > 0.0f)
    {
        dome.Radius = std::max(kMinimumDomeRadius, context.GetOrthoHeight() * kDomeScreenFraction);
    }
    else if (dome.HasCamera)
    {
        dome.Perspective = true;
        dome.Radius = std::max(kMinimumDomeRadius, Distance(dome.Camera, dome.Center) * kDomeScreenFraction);
    }
    return dome;
}

// What one screen pixel spans at `point`, in world units. In a perspective view that grows with the
// distance from the camera, so a stroke that runs away from it, as the axis does, is drawn wider
// where it is farther and keeps its width on screen.
float UnitsPerPixel(const Dome& dome, const Vector3& point)
{
    if (!dome.Perspective)
        return dome.Radius / kPixelsPerDomeRadius;
    return Distance(dome.Camera, point) * kDomeScreenFraction / kPixelsPerDomeRadius;
}

// The point on the dome in `direction` (unit length), scaled by `scale` of its radius.
Vector3 OnDome(const Dome& dome, const Vector3& direction, float scale)
{
    return dome.Center + direction * dome.Radius * scale;
}

// The unit direction from `point` toward the camera; up when the context has no camera.
Vector3 TowardCamera(const Dome& dome, const Vector3& point)
{
    if (!dome.HasCamera)
        return Vector3(0.0f, 1.0f, 0.0f);
    Vector3 toward = dome.Camera - point;
    Normalise(toward);
    return toward;
}

// One segment as a quad about `pixels` wide, turned to face the camera, on `layer`.
void Quad(GizmoRenderContext& context, const Dome& dome, const Vector3& from, const Vector3& to, float pixels,
          std::int32_t layer, const Color& color)
{
    Vector3 along = to - from;
    Normalise(along);
    const Vector3 middle = (from + to) * 0.5f;
    Vector3 side = Vector3::Cross(along, TowardCamera(dome, middle));
    Normalise(side);
    const float halfAtFrom = 0.5f * pixels * UnitsPerPixel(dome, from);
    const float halfAtTo = 0.5f * pixels * UnitsPerPixel(dome, to);
    const Vector3 corners[4] = {from - side * halfAtFrom, from + side * halfAtFrom, to + side * halfAtTo,
                                to - side * halfAtTo};
    const Vector3 triangles[6] = {corners[0], corners[1], corners[2], corners[0], corners[2], corners[3]};
    context.SetTriangleLayer(layer);
    context.DrawTriangles(triangles, 6, color);
}

// A stroke: the bright core over its dark halo.
void Stroke(GizmoRenderContext& context, const Dome& dome, const Vector3& from, const Vector3& to,
            const Color& color)
{
    Quad(context, dome, from, to, kHaloPixels, kHaloLayer, kHaloColor);
    Quad(context, dome, from, to, kCorePixels, kCoreLayer, color);
}

// A circle of `radius` about `center` in the plane spanned by the unit vectors `u` and `v`.
void StrokeCircle(GizmoRenderContext& context, const Dome& dome, const Vector3& center, const Vector3& u,
                  const Vector3& v, float radius, int segments, const Color& color)
{
    Vector3 previous = center + u * radius;
    for (int i = 1; i <= segments; ++i)
    {
        const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(segments);
        const float c = std::cos(angle) * radius;
        const float s = std::sin(angle) * radius;
        const Vector3 point = center + u * c + v * s;
        Stroke(context, dome, previous, point, color);
        previous = point;
    }
}

// The sun's direction at `hours` of solar time on `frame`, with the same hour-to-angle mapping as
// the rendered sun.
Vector3 SunDirection(const Rendering::SolarFrame& frame, float hours)
{
    const float theta = (hours / kHoursPerDay - 0.25f) * kTwoPi;
    Vector3 direction;
    Rendering::SolarDirection(frame, std::cos(theta), std::sin(theta), direction.v);
    return direction;
}

// A solid triangle over a dark outline, outside the horizon ring, pointing toward `heading` and
// turned to face the camera as far as it can while lying across the heading.
void DrawNorthMark(GizmoRenderContext& context, const Dome& dome, const Vector3& heading)
{
    const Vector3 base = OnDome(dome, heading, kNorthMarkInnerScale);
    const Vector3 tip = OnDome(dome, heading, kNorthMarkOuterScale);
    Vector3 across = Vector3::Cross(heading, TowardCamera(dome, base));
    if (across.LengthSquared() <= kMinimumLengthSquared)
        across = Vector3::Cross(heading, Vector3(0.0f, 1.0f, 0.0f));
    Normalise(across);
    const float half = dome.Radius * kNorthMarkHalfWidthScale;
    const float halo = kHaloPixels * UnitsPerPixel(dome, base);
    const Vector3 triangle[3] = {base - across * half, base + across * half, tip};
    const Vector3 outline[3] = {triangle[0] - across * halo - heading * halo,
                                triangle[1] + across * halo - heading * halo, tip + heading * halo};
    context.SetTriangleLayer(kHaloLayer);
    context.DrawTriangles(outline, 3, kHaloColor);
    context.SetTriangleLayer(kCoreLayer);
    context.DrawTriangles(triangle, 3, kNorthColor);
}

// The horizon ring, and where the path's pole heading points: north's red triangle on the Earth
// path, a tick in the axis colour on a Custom one.
void DrawHorizon(GizmoRenderContext& context, const Dome& dome, float headingRadians, bool earthPath)
{
    const Vector3 east(1.0f, 0.0f, 0.0f);
    const Vector3 forward(0.0f, 0.0f, 1.0f);
    StrokeCircle(context, dome, dome.Center, east, forward, dome.Radius, kRingSegments, kHorizonColor);

    const Vector3 heading(std::sin(headingRadians), 0.0f, std::cos(headingRadians));
    if (earthPath)
    {
        DrawNorthMark(context, dome, heading);
        return;
    }
    const Vector3 inner = OnDome(dome, heading, kHeadingTickInnerScale);
    const Vector3 outer = OnDome(dome, heading, kHeadingTickOuterScale);
    Stroke(context, dome, inner, outer, kAxisColor);
}

// An arrow from the dome's centre along the pole the sun circles.
void DrawAxis(GizmoRenderContext& context, const Dome& dome, const Rendering::SolarFrame& frame)
{
    const Vector3 pole(frame.Pole[0], frame.Pole[1], frame.Pole[2]);
    const Vector3 east(frame.East[0], frame.East[1], frame.East[2]);
    const Vector3 tip = OnDome(dome, pole, kAxisLengthScale);
    Stroke(context, dome, dome.Center, tip, kAxisColor);

    // Two barbs in the plane of the pole and the frame's east, pointing back from the tip.
    const float head = dome.Radius * kArrowHeadScale;
    for (const float side : {1.0f, -1.0f})
    {
        const Vector3 barb = tip - pole * head * 2.0f + east * head * side;
        Stroke(context, dome, tip, barb, kAxisColor);
    }
}

// The day's circle: bright where the sun is above the horizon, dimmed where it is below.
void DrawDayCircle(GizmoRenderContext& context, const Dome& dome, const Rendering::SolarFrame& frame)
{
    Vector3 previousDirection = SunDirection(frame, 0.0f);
    for (int i = 1; i <= kDayCircleSegments; ++i)
    {
        const float hours = kHoursPerDay * static_cast<float>(i) / static_cast<float>(kDayCircleSegments);
        const Vector3 direction = SunDirection(frame, hours);
        const Vector3 from = OnDome(dome, previousDirection, 1.0f);
        const Vector3 to = OnDome(dome, direction, 1.0f);
        const bool aboveHorizon = previousDirection.y + direction.y >= 0.0f;
        Stroke(context, dome, from, to, aboveHorizon ? kDayArcColor : kNightArcColor);
        previousDirection = direction;
    }
}

// A small ring on the dome where the sun stands at `hours`, facing out from the centre.
void DrawMarker(GizmoRenderContext& context, const Dome& dome, const Rendering::SolarFrame& frame, float hours,
                const Color& color)
{
    const Vector3 direction = SunDirection(frame, hours);
    const Vector3 point = OnDome(dome, direction, 1.0f);
    const Vector3 reference = std::abs(direction.y) > kNearlyVerticalUp
                                  ? Vector3(frame.East[0], frame.East[1], frame.East[2])
                                  : Vector3(0.0f, 1.0f, 0.0f);
    Vector3 u = Vector3::Cross(direction, reference);
    Normalise(u);
    Vector3 v = Vector3::Cross(direction, u);
    Normalise(v);
    StrokeCircle(context, dome, point, u, v, dome.Radius * kMarkerRadiusScale, kMarkerSegments, color);
}

void DrawSkyPath(GizmoRenderContext& context, const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* sky = world.GetComponent<Components::SkyEnvironment>(entity);
    if (!sky || sky->Mode != Components::SkyMode::Physical)
        return;

    // The dome stands for the sky, which nothing in the scene can hide, so it draws over the scene.
    const SceneTools::GizmoDepthModeScope onTop(context, SceneTools::GizmoDepthMode::AlwaysOnTop);
    const Rendering::SolarPathAngles angles = Components::SkySunPath::PathAngles(*sky);
    const Rendering::SolarFrame frame = Rendering::MakeSolarFrame(angles);
    const Dome dome = DomeFor(context, world, entity);

    DrawHorizon(context, dome, static_cast<float>(angles.PoleHeadingRadians),
                sky->SunPath == Components::SkySunPathKind::Earth);
    DrawAxis(context, dome, frame);
    DrawDayCircle(context, dome, frame);

    DrawMarker(context, dome, frame, kNoonHours, kNoonColor);
    // The readout's reading of the day: a sun that circles along the horizon, or stands still on
    // it, has no sunrise to mark, whatever the sunrise equation makes of it.
    if (ClassifySkySunDay(*sky) == SkySunDayKind::RisesAndSets)
    {
        const float dayLength = Rendering::SolarDayLengthHours(angles);
        DrawMarker(context, dome, frame, kNoonHours - 0.5f * dayLength, kHorizonCrossingColor);
        DrawMarker(context, dome, frame, kNoonHours + 0.5f * dayLength, kHorizonCrossingColor);
    }

    const Vector3 sun = OnDome(dome, SunDirection(frame, sky->TimeOfDayHours), 1.0f);
    context.SetTriangleLayer(kCoreLayer);
    context.DrawSolidSphere(sun, dome.Radius * kSunDotRadiusScale, kSunColor);
    // The layer is sticky context state: hand the next gizmo the default.
    context.SetTriangleLayer(0);
}
} // namespace

void RegisterSkyPathGizmo()
{
    SceneTools::ComponentGizmoRegistry::Get().Register(ECS::GetComponentTypeId<Components::SkyEnvironment>(),
                                                       DrawSkyPath);
}

} // namespace GameEngine::Editor
