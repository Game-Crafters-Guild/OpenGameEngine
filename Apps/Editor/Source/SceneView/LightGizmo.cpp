#include "SceneView/LightGizmo.h"

#include <algorithm>
#include <cmath>

#include "Core/Engine.h"
#include "ECS/Components.h"
#include "ECS/ECS.h"
#include "ECS/Query.h"
#include "Components/Rendering/Light.h"
#include "Components/Transform.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "Mathematics/Vector3.h"
#include "Types/Color.h"

namespace GameEngine::Editor::SceneTools
{

using GameEngine::Components::Light;
using GameEngine::Components::LightType;
using GameEngine::Components::WorldTransform;
using GameEngine::Mathematics::Vector3;

namespace
{

constexpr float kPi      = 3.14159265358979323846f;
constexpr float kTwoPi   = 6.28318530717958647693f;
constexpr int   kRingSegments = 32;

float ComputeIconScale(const Vector3& pos, const Vector3& camPos, const GizmoRenderContext& ctx)
{
    const float orthoH = (ctx.HasOrthoHeight() && ctx.GetOrthoHeight() > 0.0f) ? ctx.GetOrthoHeight() : 0.0f;
    return ComputeGizmoIconWorldRadius(pos, camPos, orthoH);
}

// Unit `side` and `perp` axes perpendicular to `dir`, seeded from world up
// (world X when `dir` is near vertical).
void BuildPerpendicularBasis(const Vector3& dir, Vector3& side, Vector3& perp)
{
    const Vector3 seed = (std::fabs(Vector3::Dot(dir, Vector3(0.0f, 1.0f, 0.0f))) > 0.95f)
                             ? Vector3(1.0f, 0.0f, 0.0f)
                             : Vector3(0.0f, 1.0f, 0.0f);
    side = Vector3::Cross(dir, seed).NormalizeOrZero();
    perp = Vector3::Cross(dir, side).NormalizeOrZero();
}

// Sun-style billboard icon: a center disc made of a small ring + 8 short
// rays radiating outward. Drawn in the camera-facing (right, up) plane at
// `pos` with overall radius `r`. ~32 lines total, all batched into a
// single DrawColoredLines submission via GizmoLineBatch.
void DrawDirectionalIcon(GizmoRenderContext& ctx,
                         const Vector3& pos,
                         const Vector3& right, const Vector3& up,
                         float r,
                         const Color& color,
                         float thickness)
{
    constexpr int kRayCount = 8;
    GizmoLineBatch batch(ctx, color, thickness);

    DrawCircleInto(batch, pos, right, r * 0.45f, up, r * 0.45f, 24);

    const float inner = r * 0.55f;
    const float outer = r * 1.0f;
    for (int i = 0; i < kRayCount; ++i)
    {
        const float a = (static_cast<float>(i) / static_cast<float>(kRayCount)) * kTwoPi;
        const float ca = std::cos(a);
        const float sa = std::sin(a);
        const Vector3 dir = right * ca + up * sa;
        batch.AddLine(pos + dir * inner, pos + dir * outer);
    }
}

// Bulb-style billboard icon for point lights: an Edison-bulb silhouette
// (round glass top tapering into a narrow neck) with zig-zag screw threads
// and a base contact, drawn in the camera-facing plane. 21 lines total
// batched via GizmoLineBatch into one DrawColoredLines submission.
void DrawPointIcon(GizmoRenderContext& ctx,
                   const Vector3& pos,
                   const Vector3& right, const Vector3& up,
                   float r,
                   const Color& color,
                   float thickness)
{
    // Bulb + neck silhouette (clockwise from the top). Round glass top
    // (y~0.95..0.40) narrowing into a short neck (y~0.05..-0.45) so the
    // contour reads as a screw-base bulb rather than just a circle on a stick.
    static const float kSilhouette[][2] = {
        {  0.00f,  0.95f },
        {  0.28f,  0.88f },
        {  0.46f,  0.66f },
        {  0.50f,  0.40f },
        {  0.42f,  0.18f },
        {  0.30f,  0.05f },
        {  0.20f, -0.05f },
        {  0.20f, -0.45f },
        { -0.20f, -0.45f },
        { -0.20f, -0.05f },
        { -0.30f,  0.05f },
        { -0.42f,  0.18f },
        { -0.50f,  0.40f },
        { -0.46f,  0.66f },
        { -0.28f,  0.88f },
        {  0.00f,  0.95f }
    };
    constexpr int kSilhouetteCount = static_cast<int>(sizeof(kSilhouette) / sizeof(kSilhouette[0]));

    // Screw threads: zig-zag down from the neck bottom.
    static const float kThreads[][2] = {
        {  0.20f, -0.45f },
        { -0.20f, -0.55f },
        {  0.20f, -0.65f },
        { -0.20f, -0.75f },
        {  0.20f, -0.85f }
    };
    constexpr int kThreadCount = static_cast<int>(sizeof(kThreads) / sizeof(kThreads[0]));

    GizmoLineBatch batch(ctx, color, thickness);

    // Local point: project (xc, yc) into the camera-facing plane at `pos`.
    auto pt = [&](float xc, float yc)
    {
        return pos + right * xc * r + up * yc * r;
    };
    auto seg = [&](float x0, float y0, float x1, float y1)
    {
        batch.AddLine(pt(x0, y0), pt(x1, y1));
    };

    for (int i = 0; i + 1 < kSilhouetteCount; ++i)
    {
        seg(kSilhouette[i][0],     kSilhouette[i][1],
            kSilhouette[i + 1][0], kSilhouette[i + 1][1]);
    }

    // Inner highlight: a small filament-style horizontal stroke inside the
    // glass to differentiate from a plain ring at low icon sizes.
    seg(-0.18f, 0.45f, 0.18f, 0.45f);

    for (int i = 0; i + 1 < kThreadCount; ++i)
    {
        seg(kThreads[i][0],     kThreads[i][1],
            kThreads[i + 1][0], kThreads[i + 1][1]);
    }

    // Base contact stub at the bottom.
    seg(-0.12f, -0.95f, 0.12f, -0.95f);
}

// 3-D flashlight icon for spot lights. The flashlight body is oriented in
// world space along `dir` so it instantly communicates the light's aim.
// The lens disc sits at `pos` (the apex), and the body extends behind it.
// The separate DrawWireConeOpen call in Render() shows the emission cone.
void DrawSpotIcon3D(GizmoRenderContext& ctx,
                    const Vector3& pos,
                    const Vector3& dir,
                    float r,
                    const Color& color,
                    float thickness)
{
    // Orthonormal basis perpendicular to the light direction.
    Vector3 side;
    Vector3 perp;
    BuildPerpendicularBasis(dir, side, perp);

    const float kHeadRadius = r * 0.42f;
    const float kHeadLen    = r * 0.32f;
    const float kBodyRadius = r * 0.22f;
    const float kBodyLen    = r * 1.40f;
    const float kTaper      = r * 0.12f;

    // Rim helper: point on a circle in the (side, perp) plane.
    auto rimPt = [&](const Vector3& center, float radius, float angle)
    {
        const float ca = std::cos(angle);
        const float sa = std::sin(angle);
        return center + side * ca * radius + perp * sa * radius;
    };

    // Ring centres at increasing distance behind pos (opposite to dir).
    const Vector3 headBackCenter = pos - dir * kHeadLen;
    const Vector3 taperEndCenter = pos - dir * (kHeadLen + kTaper);
    const Vector3 bodyEndCenter  = pos - dir * (kHeadLen + kTaper + kBodyLen);

    // ~77 lines total (4 rings + 12 longitudinal + glint), all sharing color
    // and thickness — single batched submission.
    GizmoLineBatch batch(ctx, color, thickness);

    // Lens cap ring (at pos, facing along dir).
    DrawCircleInto(batch, pos,           side, kHeadRadius, perp, kHeadRadius, 16);
    // Bezel ring (back of flared head).
    DrawCircleInto(batch, headBackCenter, side, kHeadRadius, perp, kHeadRadius, 16);
    // Body start ring (after short taper).
    DrawCircleInto(batch, taperEndCenter, side, kBodyRadius, perp, kBodyRadius, 12);
    // End cap ring.
    DrawCircleInto(batch, bodyEndCenter,  side, kBodyRadius, perp, kBodyRadius, 12);

    // Four longitudinal lines along the head cylinder.
    for (int i = 0; i < 4; ++i)
    {
        const float a = (static_cast<float>(i) / 4.0f) * kTwoPi;
        batch.AddLine(rimPt(pos, kHeadRadius, a), rimPt(headBackCenter, kHeadRadius, a));
    }

    // Four taper lines from head bezel to body.
    for (int i = 0; i < 4; ++i)
    {
        const float a = (static_cast<float>(i) / 4.0f) * kTwoPi;
        batch.AddLine(rimPt(headBackCenter, kHeadRadius, a), rimPt(taperEndCenter, kBodyRadius, a));
    }

    // Four longitudinal lines along the body cylinder.
    for (int i = 0; i < 4; ++i)
    {
        const float a = (static_cast<float>(i) / 4.0f) * kTwoPi;
        batch.AddLine(rimPt(taperEndCenter, kBodyRadius, a), rimPt(bodyEndCenter, kBodyRadius, a));
    }

    // Glint line inside the lens face so it still reads as "light" when small.
    batch.AddLine(rimPt(pos, kHeadRadius * 0.35f, kPi * 0.25f),
                  rimPt(pos, kHeadRadius * 0.35f, -kPi * 0.75f));
}

void DrawDirectionArrow(GizmoRenderContext& ctx,
                        const Vector3& origin,
                        const Vector3& dir,
                        float length,
                        const Vector3& right, const Vector3& up,
                        const Color& color,
                        float thickness);

// Area emitter gizmo: a world-oriented rectangle in the light's local XY
// plane, plus a center normal arrow along the emitted-light direction (+Z).
void DrawAreaIcon3D(GizmoRenderContext& ctx,
                    const Vector3& pos,
                    const Vector3& dir,
                    const Vector3& planeRight,
                    const Vector3& planeUp,
                    float width,
                    float height,
                    float fallbackRadius,
                    const Color& color,
                    float thickness)
{
    const Vector3 right = planeRight.NormalizeOrZero();
    const Vector3 up = planeUp.NormalizeOrZero();

    const float minSize = std::max(fallbackRadius * 1.75f, 0.001f);
    const float halfW = std::max(width, minSize) * 0.5f;
    const float halfH = std::max(height, minSize) * 0.5f;

    auto corner = [&](float x, float y)
    {
        return pos + right * x + up * y;
    };

    const Vector3 c0 = corner(-halfW, -halfH);
    const Vector3 c1 = corner( halfW, -halfH);
    const Vector3 c2 = corner( halfW,  halfH);
    const Vector3 c3 = corner(-halfW,  halfH);

    GizmoLineBatch batch(ctx, color, thickness);
    batch.AddLine(c0, c1);
    batch.AddLine(c1, c2);
    batch.AddLine(c2, c3);
    batch.AddLine(c3, c0);

    batch.AddLine(pos - right * halfW, pos + right * halfW);
    batch.AddLine(pos - up * halfH, pos + up * halfH);

    const Vector3 tickCenter = pos + dir * fallbackRadius * 0.25f;
    const Vector3 tickRight = right * fallbackRadius * 0.28f;
    const Vector3 tickUp = up * fallbackRadius * 0.28f;
    batch.AddLine(tickCenter - tickRight, tickCenter + tickRight);
    batch.AddLine(tickCenter - tickUp, tickCenter + tickUp);
    batch.Flush();

    DrawDirectionArrow(ctx, pos, dir, fallbackRadius * 3.0f, right, up, color, thickness);
}

// Ambient indicator: a 3-D half-sun. Flat horizon ring in the world XZ
// plane, four dome arcs rising to a crown, and rays fanning outward at 45°
// above the ring plus one straight-up crown ray. Always upright in world
// space (Y+ up), communicating "omnidirectional sky / hemisphere light".
void DrawAmbientIcon(GizmoRenderContext& ctx,
                     const Vector3& pos,
                     const Vector3& /*right*/, const Vector3& /*up*/,
                     float r,
                     const Color& color,
                     float thickness)
{
    constexpr int kArcCount = 4;
    constexpr int kArcSegs  = 18;
    constexpr int kRayCount = 8;

    // ~108 lines: horizon (28) + 4 arcs (72) + 8 rays + crown ray = 109.
    GizmoLineBatch batch(ctx, color, thickness);

    // Horizon ring — full circle in the XZ plane.
    const Vector3 kAxisX(1.0f, 0.0f, 0.0f);
    const Vector3 kAxisZ(0.0f, 0.0f, 1.0f);
    DrawCircleInto(batch, pos, kAxisX, r, kAxisZ, r, 28);

    // Four great-circle arcs spanning the upper hemisphere, spaced 45° apart
    // in azimuth. Each arc goes from one horizon edge, arches up through the
    // crown, and comes back down to the opposite horizon edge.
    for (int arc = 0; arc < kArcCount; ++arc)
    {
        const float phi = (static_cast<float>(arc) / kArcCount) * kPi;
        const float cp = std::cos(phi);
        const float sp = std::sin(phi);
        // Horizontal axis (cp, 0, sp) of this arc's great-circle plane.
        Vector3 prev;
        for (int s = 0; s <= kArcSegs; ++s)
        {
            const float t  = (static_cast<float>(s) / kArcSegs) * kPi;
            // cos(t): -1→0→1 sweeps across the diameter.
            // sin(t):  0→1→0 gives the arch height.
            const Vector3 p(pos.x + cp * std::cos(t) * r,
                            pos.y + std::sin(t) * r,
                            pos.z + sp * std::cos(t) * r);
            if (s > 0)
                batch.AddLine(prev, p);
            prev = p;
        }
    }

    // Eight rays fanning outward at 45° elevation — one per octant around the
    // dome. Each ray starts just beyond the dome surface and extends outward.
    constexpr float kElevation = kPi * 0.25f;   // 45° above horizon
    const float ce = std::cos(kElevation);
    const float se = std::sin(kElevation);
    for (int i = 0; i < kRayCount; ++i)
    {
        const float phi = (static_cast<float>(i) / kRayCount) * kTwoPi;
        const float cp  = std::cos(phi);
        const float sp  = std::sin(phi);
        const Vector3 d(cp * ce, se, sp * ce);
        batch.AddLine(pos + d * r, pos + d * r * 1.40f);
    }

    // Crown ray — straight up from the top of the dome.
    batch.AddLine(Vector3(pos.x, pos.y + r, pos.z), Vector3(pos.x, pos.y + r * 1.40f, pos.z));
}

// Arrow along `dir` of `length` world units, with a small chevron head built
// from two short side lines in the camera-facing plane. 5 lines batched.
void DrawDirectionArrow(GizmoRenderContext& ctx,
                        const Vector3& origin,
                        const Vector3& dir,
                        float length,
                        const Vector3& right, const Vector3& up,
                        const Color& color,
                        float thickness)
{
    GizmoLineBatch batch(ctx, color, thickness);

    const Vector3 tip = origin + dir * length;
    batch.AddLine(origin, tip);

    const float headLen = length * 0.18f;
    const float headWidth = length * 0.08f;
    const Vector3 back = tip - dir * headLen;
    batch.AddLine(tip, back - right * headWidth);
    batch.AddLine(tip, back + right * headWidth);
    batch.AddLine(tip, back - up * headWidth);
    batch.AddLine(tip, back + up * headWidth);
}

// Wire cone with apex at `apex` opening along `dir` to a base circle of
// radius `baseRadius` at distance `length`. Drawn as the base ring plus
// 4 rim spokes from apex to the base.
void DrawWireConeOpen(GizmoRenderContext& ctx,
                      const Vector3& apex,
                      const Vector3& dir,
                      float length,
                      float baseRadius,
                      const Color& color,
                      float thickness)
{
    if (length <= 0.0f || baseRadius <= 0.0f)
        return;

    // Build an orthonormal basis perpendicular to dir.
    Vector3 side;
    Vector3 other;
    BuildPerpendicularBasis(dir, side, other);

    const Vector3 baseCenter = apex + dir * length;

    constexpr int kSpokes = 4;
    GizmoLineBatch batch(ctx, color, thickness);

    DrawCircleInto(batch, baseCenter, side, baseRadius, other, baseRadius, kRingSegments);

    for (int i = 0; i < kSpokes; ++i)
    {
        const float a = (static_cast<float>(i) / static_cast<float>(kSpokes)) * kTwoPi;
        const float ca = std::cos(a);
        const float sa = std::sin(a);
        batch.AddLine(apex, baseCenter + side * ca * baseRadius + other * sa * baseRadius);
    }
}

// Map a raw light color to a vivid icon color: normalize so the brightest
// channel saturates to 1, then lift to ensure a near-black light is still
// visible. Alpha is supplied separately. The icon shows the light's hue, not
// its brightness: a sky-driven sun at night carries moonlight in a color four
// orders of magnitude below 1, which normalizes to the moon's blue-white like
// any other color. Only a black light (nothing to normalize) falls back to grey.
Color IconColorFromLight(const float src[3], float alpha)
{
    constexpr float kBlackLightThreshold = 1e-12f;
    const float maxC = std::max({ src[0], src[1], src[2] });
    const float scale = maxC > kBlackLightThreshold ? 1.0f / maxC : 0.0f;
    // Lift toward white so very dim or pure-black lights still show.
    return Color(0.4f + 0.6f * std::clamp(src[0] * scale, 0.0f, 1.0f),
                 0.4f + 0.6f * std::clamp(src[1] * scale, 0.0f, 1.0f),
                 0.4f + 0.6f * std::clamp(src[2] * scale, 0.0f, 1.0f),
                 alpha);
}

} // namespace

void LightGizmo::SetSelection(const std::vector<GameEngine::ECS::EntityHandle>& entities)
{
    m_SelectedEntities = entities;
}

void LightGizmo::SetHovered(GameEngine::ECS::EntityHandle entity)
{
    m_HoveredEntity = entity;
}

void LightGizmo::Render(GizmoRenderContext& context)
{
    GameEngine::ECS::World* world = context.GetWorld();
    if (!world)
        return;

    const Vector3* camPosPtr = context.GetCameraWorldPosition();
    if (!camPosPtr)
        return;
    const Vector3& camPos = *camPosPtr;
    const auto& sceneViewSettings = GameEngine::Editor::SceneViewSettings::Get();
    const bool directionalDetailsOnlyOnSelection = sceneViewSettings.GetDirectionalLightGizmoDetailsOnlyOnSelection();
    const bool pointDetailsOnlyOnSelection = sceneViewSettings.GetPointLightGizmoDetailsOnlyOnSelection();
    const bool spotDetailsOnlyOnSelection = sceneViewSettings.GetSpotLightGizmoDetailsOnlyOnSelection();

    world->Query<
             GameEngine::ECS::Read<Light>,
             GameEngine::ECS::Read<WorldTransform>>()
        .Each([&](GameEngine::ECS::EntityHandle e,
                  const Light& light,
                  const WorldTransform& xf)
              {
                  const bool isHovered  = m_HoveredEntity.IsValid() && e == m_HoveredEntity;
                  const bool isSelected = std::find(m_SelectedEntities.begin(),
                                                    m_SelectedEntities.end(), e)
                                          != m_SelectedEntities.end();
                  const bool emphasize = isSelected || isHovered;

                  const float* m = xf.matrix;
                  const Vector3 pos(m[12], m[13], m[14]);

                  // Shine along entity forward (+Z column).
                  const Vector3 dir = Vector3(m[8], m[9], m[10]).NormalizeOrZero();

                  Vector3 right;
                  Vector3 up;
                  BuildBillboardBasis(pos, camPos, right, up);

                  const float iconR = ComputeIconScale(pos, camPos, context);

                  const float iconAlpha   = emphasize ? 1.0f : 0.85f;
                  const float volumeAlpha = emphasize ? 0.9f : 0.35f;
                  const float iconThick   = emphasize ? 2.0f : 1.5f;
                  const float volumeThick = emphasize ? 1.5f : 1.0f;

                  const Color iconColor = IconColorFromLight(light.Color, iconAlpha);
                  const Color volumeColor = IconColorFromLight(light.Color, volumeAlpha);

                  switch (light.Type)
                  {
                      case LightType::Directional:
                      {
                          DrawDirectionalIcon(context, pos, right, up, iconR, iconColor, iconThick);
                          if (!directionalDetailsOnlyOnSelection || isSelected)
                          {
                              const float arrowLen = iconR * 6.0f;
                              DrawDirectionArrow(context, pos, dir, arrowLen, right, up,
                                                 volumeColor, volumeThick);
                          }
                          break;
                      }
                      case LightType::Point:
                      {
                          DrawPointIcon(context, pos, right, up, iconR, iconColor, iconThick);
                          if ((!pointDetailsOnlyOnSelection || isSelected) && light.Range > 0.0f)
                              context.DrawWireSphere(pos, light.Range, volumeColor, volumeThick);
                          break;
                      }
                      case LightType::Spot:
                      {
                          // 3-D flashlight icon oriented in world space along
                          // the light direction — no billboard basis needed.
                          DrawSpotIcon3D(context, pos, dir, iconR, iconColor, iconThick);
                          if ((!spotDetailsOnlyOnSelection || isSelected) && light.Range > 0.0f)
                          {
                              const float outerAngle = std::clamp(light.OuterAngle, 0.001f, kPi * 0.5f - 0.001f);
                              const float outerRadius = light.Range * std::tan(outerAngle);

                              // Outer cone: desaturated blue-grey so it reads
                              // as "falloff edge" rather than the hot center.
                              const Color outerConeColor(0.55f, 0.65f, 0.80f, volumeAlpha);

                              DrawWireConeOpen(context, pos, dir, light.Range, outerRadius,
                                               outerConeColor, volumeThick);

                              if (light.InnerAngle > 0.0f && light.InnerAngle < light.OuterAngle)
                              {
                                  const float innerAngle = std::clamp(light.InnerAngle, 0.001f, outerAngle);
                                  const float innerRadius = light.Range * std::tan(innerAngle);
                                  // Inner cone keeps the light's own color —
                                  // this is the full-intensity beam area.
                                  DrawWireConeOpen(context, pos, dir, light.Range, innerRadius,
                                                   volumeColor, volumeThick);
                              }
                          }
                          break;
                      }
                      case LightType::Area:
                      {
                          const Vector3 planeRight(m[0], m[1], m[2]);
                          const Vector3 planeUp(m[4], m[5], m[6]);
                          DrawAreaIcon3D(context, pos, dir, planeRight, planeUp,
                                         light.AreaWidth, light.AreaHeight,
                                         iconR, iconColor, iconThick);
                          break;
                      }
                      case LightType::Ambient:
                      default:
                      {
                          DrawAmbientIcon(context, pos, right, up, iconR, iconColor, iconThick);
                          break;
                      }
                  }
              });
}

} // namespace GameEngine::Editor::SceneTools

namespace GameEngine::Editor::SceneTools
{
}
