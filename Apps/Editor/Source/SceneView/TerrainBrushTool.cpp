#include "SceneView/TerrainBrushTool.h"

#include "SceneView/SphereSculptStrokeCommand.h"
#include "SceneView/TerrainZoneStrokeCommand.h"
#include "SceneViewController.h"
#include "Picking/TerrainPicking.h"
#include "UndoRedo/UndoRedoService.h"
#include "EditorChangeNotifications.h"

#include "CBTTerrain/CBTPlanetShading.h"
#include "CBTTerrainECS/CBTRenderFeature.h"
#include "Components/Name.h"
#include "Components/Transform.h"
#include "CBTTerrainECS/TerrainProvisioning.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Core/Engine.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h" // World::GetComponent/AddComponentImmediate<T> definitions (cross-DLL)
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Terrain/TerrainTypes.h" // kMaxTerrainMaterialLayers — the paint-role clamp
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainZoneAuthoring.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace GameEngine::Editor::SceneTools {

using TerrainECS::TerrainService;
using TerrainECS::TerrainZonePayload;
using TerrainECS::ZonePayloadFormat;
using TerrainECS::ZoneFootprint;

namespace {

constexpr float kRayDirEpsilon = 1e-6f;
constexpr float kMaxPickDistance = 100000.0f;

// Brush setting bounds + hotkey adjustment steps.
constexpr float kMinBrushRadius = 1.0f;
constexpr float kMaxBrushRadius = 256.0f;
constexpr float kBrushRadiusStep = 1.0f;
constexpr float kMinBrushStrength = 0.1f;
constexpr float kMaxBrushStrength = 64.0f;
constexpr float kBrushStrengthStep = 0.25f;

bool ZoneDebugEnabled()
{
    static const bool enabled = []
    {
        const char* v = std::getenv("GE_TERRAIN_ZONE_DEBUG");
        return v != nullptr && v[0] != '0';
    }();
    return enabled;
}

// Inverse-transform / forward-transform between world XZ and a zone's payload
// texels. Built per dab from a zone's position + Y-rotation + effective world
// half-extents (extent * transform scale), matching the bake's sampling.
struct ZoneMapping
{
    float PosX = 0.0f, PosZ = 0.0f;
    float CosYaw = 1.0f, SinYaw = 0.0f;
    float RectHalfX = 1.0f, RectHalfZ = 1.0f;
    uint32_t Width = 0, Height = 0;

    void WorldToTexel(float wx, float wz, float& tx, float& tz) const
    {
        const float dx = wx - PosX;
        const float dz = wz - PosZ;
        const float lx = CosYaw * dx + SinYaw * dz;   // R(-yaw)
        const float lz = -SinYaw * dx + CosYaw * dz;
        const float u = RectHalfX > 0.0f ? 0.5f + 0.5f * lx / RectHalfX : 0.5f;
        const float v = RectHalfZ > 0.0f ? 0.5f + 0.5f * lz / RectHalfZ : 0.5f;
        tx = u * static_cast<float>(Width - 1);
        tz = v * static_cast<float>(Height - 1);
    }

    void TexelToWorld(uint32_t tx, uint32_t tz, float& wx, float& wz) const
    {
        const float u = Width > 1 ? static_cast<float>(tx) / static_cast<float>(Width - 1) : 0.5f;
        const float v = Height > 1 ? static_cast<float>(tz) / static_cast<float>(Height - 1) : 0.5f;
        const float lx = (2.0f * u - 1.0f) * RectHalfX;
        const float lz = (2.0f * v - 1.0f) * RectHalfZ;
        wx = PosX + CosYaw * lx - SinYaw * lz;        // R(yaw)
        wz = PosZ + SinYaw * lx + CosYaw * lz;
    }
};

ZoneMapping MappingFromFootprint(const ZoneFootprint& fp, uint32_t w, uint32_t h)
{
    ZoneMapping m;
    m.PosX = fp.CenterX;
    m.PosZ = fp.CenterZ;
    m.RectHalfX = fp.ExtentX;
    m.RectHalfZ = fp.ExtentZ;
    m.Width = w;
    m.Height = h;
    return m;
}

// Extract yaw + XZ scale from a world-transform matrix (matches the modifier
// gather's ExtractYaw / ExtractXZScale).
ZoneMapping MappingFromWorldTransform(const Components::WorldTransform& xf,
                                      float extentX, float extentZ, uint32_t w, uint32_t h)
{
    ZoneMapping m;
    m.PosX = xf.matrix[12];
    m.PosZ = xf.matrix[14];
    const float yaw = std::atan2(xf.matrix[8], xf.matrix[10]);
    m.CosYaw = std::cos(yaw);
    m.SinYaw = std::sin(yaw);
    const float scaleX = std::sqrt(xf.matrix[0] * xf.matrix[0] + xf.matrix[1] * xf.matrix[1]
                                   + xf.matrix[2] * xf.matrix[2]);
    const float scaleZ = std::sqrt(xf.matrix[8] * xf.matrix[8] + xf.matrix[9] * xf.matrix[9]
                                   + xf.matrix[10] * xf.matrix[10]);
    m.RectHalfX = extentX * (scaleX > 0.0f ? scaleX : 1.0f);
    m.RectHalfZ = extentZ * (scaleZ > 0.0f ? scaleZ : 1.0f);
    m.Width = w;
    m.Height = h;
    return m;
}

// Capture the full zone state a stroke touches (components + payload) for the
// stroke undo command.
Editor::ZoneStrokeState CaptureZoneState(ECS::World& world, TerrainService& svc,
                                         ECS::EntityHandle entity, const GUID& guid, bool isPaint)
{
    Editor::ZoneStrokeState s;
    s.IsPaint = isPaint;
    if (const auto* c = world.GetComponent<Components::TerrainSculptZone>(entity)) s.Sculpt = *c;
    if (const auto* c = world.GetComponent<Components::TerrainPaintZone>(entity)) s.Paint = *c;
    if (const auto* c = world.GetComponent<Components::Transform>(entity)) s.Transform = *c;
    if (const auto* c = world.GetComponent<Components::WorldTransform>(entity)) s.WorldTransform = *c;
    if (const auto* c = world.GetComponent<Components::Name>(entity)) s.Name = *c;
    if (const TerrainZonePayload* p = svc.GetZonePayload(guid)) s.Payload = *p;
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// TerrainBrushCursorGizmo
// ---------------------------------------------------------------------------
void TerrainBrushCursorGizmo::Render(GizmoRenderContext& context)
{
    if (!Active)
        return;
    // Green = raise, amber = lower, blue = paint.
    Color color(0.35f, 0.9f, 0.4f, 0.9f);
    if (Mode == TerrainBrushMode::Lower)
        color = Color(0.95f, 0.65f, 0.25f, 0.9f);
    else if (Mode == TerrainBrushMode::Paint)
        color = Color(0.4f, 0.6f, 0.95f, 0.9f);
    context.DrawWireCircle(Center, Normal, Radius, color, 2.0f);
}

// ---------------------------------------------------------------------------
// TerrainBrushTool
// ---------------------------------------------------------------------------
TerrainBrushTool::TerrainBrushTool(SceneViewController& owner)
    : m_Owner(owner)
{
}

void TerrainBrushTool::SetPaintLayer(uint32_t layer)
{
    s_PaintLayer = std::min(layer, Terrain::kMaxTerrainMaterialLayers - 1u);
}

void TerrainBrushTool::OnActivated()
{
    m_IsStroking = false;
    m_Cursor.Active = false;
}

void TerrainBrushTool::OnDeactivated()
{
    ApplyStrokePhase(TerrainStrokePhase::End, Mathematics::Vector3{});
    m_IsStroking = false;
    m_Cursor.Active = false;
}

bool TerrainBrushTool::RaycastTerrain(const GizmoRay& ray, Mathematics::Vector3& outHit,
                                      Mathematics::Vector3& outNormal)
{
    auto* world = &m_Owner.GetWorld();
    if (std::abs(ray.direction.x) < kRayDirEpsilon && std::abs(ray.direction.y) < kRayDirEpsilon
        && std::abs(ray.direction.z) < kRayDirEpsilon)
        return false;

    Picking::TerrainPickHit hit{};
    if (!Picking::RaycastTerrain(ray, *world, kMaxPickDistance, hit))
        return false;
    outHit = hit.WorldPosition;
    outNormal = hit.WorldNormal;
    return true;
}

bool TerrainBrushTool::IsSphericalPlanet(float& outRadius) const
{
    auto* world = &m_Owner.GetWorld();
    // Route by the SAME active-terrain resolver the renderer tunes from, so the brush
    // never sculpts a sphere while the renderer draws a plane (or vice versa). The
    // active terrain being spherical takes the sphere path even with a degenerate
    // radius (RaycastPlanet rejects radius <= 0 and the dab is a no-op).
    Components::Terrain active{};
    if (!CBTTerrainECS::FindActiveTerrain(*world, active))
        return false;
    if (active.Domain != Components::TerrainDomain::Spherical)
        return false;
    outRadius = active.PlanetRadius;
    return true;
}

bool TerrainBrushTool::RaycastPlanet(const GizmoRay& ray, float radius, Mathematics::Vector3& outHit,
                                     Mathematics::Vector3& outNormal) const
{
    // The planet is centred at the world origin (cbt_domain.glsl). Solve
    // |origin + t*dir|^2 = radius^2 for the nearest non-negative t.
    if (radius <= 0.0f)
        return false;
    const Mathematics::Vector3 o = ray.origin;
    const Mathematics::Vector3 d = ray.direction.Normalize();
    const float b = Mathematics::Vector3::Dot(o, d);
    const float c = Mathematics::Vector3::Dot(o, o) - radius * radius;
    const float disc = b * b - c;
    if (disc < 0.0f)
        return false;
    const float sq = std::sqrt(disc);
    float t = -b - sq;
    if (t < 0.0f)
        t = -b + sq; // origin inside the sphere: take the far root
    if (t < 0.0f)
        return false;

    // The analytic hit sits on the BASE sphere, which ignores relief + sculpt — so the cursor
    // drifts off tall features (#488). Refine the hit against the CPU height function (the same
    // relief + sculpt the GPU displaces to) so it tracks the real surface. A few fixed-point
    // iterations converge because the height is << radius.
    if (auto* rs = EngineCore::GetInstance().GetRenderServices())
    {
        if (auto* feature = rs->GetFeature<CBTTerrainECS::CBTRenderFeature>())
        {
            const std::array<float, 3> ao{o.x, o.y, o.z};
            const std::array<float, 3> ad{d.x, d.y, d.z};
            auto heightFn = [feature](float dx, float dy, float dz) {
                return feature->SampleSphereSurfaceHeight(dx, dy, dz);
            };
            CBTTerrain::RefinePlanetHit(ao, ad, radius, heightFn, t);
        }
    }

    outHit = o + d * t;
    outNormal = outHit.Normalize();
    return true;
}

bool TerrainBrushTool::RaycastSurface(const GizmoRay& ray, Mathematics::Vector3& outHit,
                                     Mathematics::Vector3& outNormal)
{
    float planetRadius = 0.0f;
    if (IsSphericalPlanet(planetRadius))
        return RaycastPlanet(ray, planetRadius, outHit, outNormal);
    return RaycastTerrain(ray, outHit, outNormal);
}

bool TerrainBrushTool::ProbeSurfaceAt(const Mathematics::Vector3& worldTarget,
                                      Mathematics::Vector3& outHit, Mathematics::Vector3& outNormal)
{
    // Start the planar probe half way up the pick's reach, so it has as much headroom
    // above a raised terrain as it has range to reach one placed below the origin. The
    // pick clips the ray to each terrain's world AABB, so the altitude costs nothing.
    constexpr float kPlanarProbeAltitude = kMaxPickDistance * 0.5f;
    // Planet probes start this multiple of the radius out, so the origin is outside the
    // reference sphere whatever the relief does.
    constexpr float kPlanetProbeRadiusScale = 4.0f;

    float planetRadius = 0.0f;
    if (IsSphericalPlanet(planetRadius))
    {
        const float len = worldTarget.Length();
        if (len < 1e-6f)
            return false; // the planet centre names no surface direction
        const Mathematics::Vector3 dir = worldTarget * (1.0f / len);
        GizmoRay ray{};
        ray.origin = dir * (planetRadius * kPlanetProbeRadiusScale);
        ray.direction = dir * -1.0f;
        return RaycastPlanet(ray, planetRadius, outHit, outNormal);
    }

    GizmoRay ray{};
    ray.origin = Mathematics::Vector3(worldTarget.x, kPlanarProbeAltitude, worldTarget.z);
    ray.direction = Mathematics::Vector3(0.0f, -1.0f, 0.0f);
    return RaycastTerrain(ray, outHit, outNormal);
}

bool TerrainBrushTool::ApplyStrokePhase(TerrainStrokePhase phase,
                                        const Mathematics::Vector3& surfacePoint)
{
    if (phase == TerrainStrokePhase::End)
    {
        // Close whichever model is open rather than re-asking the domain: a stroke must
        // survive a domain flip mid-gesture, and this is the order OnDeactivated uses.
        if (m_SphereStroking)
        {
            m_IsStroking = false;
            return EndSphereStroke();
        }
        if (m_IsStroking)
        {
            FinalizeStroke();
            return true;
        }
        return false;
    }

    float planetRadius = 0.0f;
    if (IsSphericalPlanet(planetRadius))
    {
        // The planet is centred at the world origin, so the surface point's direction from
        // the origin IS the dab direction the viewport pick produces.
        const float len = surfacePoint.Length();
        if (len < 1e-6f)
            return false;
        const Mathematics::Vector3 dir = surfacePoint * (1.0f / len);
        if (phase == TerrainStrokePhase::Begin)
            BeginSphereStroke(); // undo boundary: one Begin..End = one entry
        ApplySphereDab(dir, planetRadius);
        return true;
    }

    if (phase == TerrainStrokePhase::Begin)
    {
        m_CapRestartGuard = 0; // new gesture: reset the cap-restart bound
        BeginStroke(surfacePoint);
        return m_IsStroking;
    }
    ApplyDab(surfacePoint);
    return true;
}

void TerrainBrushTool::ApplySphereDab(const Mathematics::Vector3& hitDir, float planetRadius)
{
    // v1: raise/lower sculpt only on the planet — paint (splat) on the sphere is a
    // documented v2 (needs a per-face splat layer + the surface's spherical splat path).
    if (m_Mode == TerrainBrushMode::Paint)
        return;

    auto* svc = TerrainService::TryGet();
    if (!svc)
        return;

    const float angularRadius = planetRadius > 0.0f ? m_Radius / planetRadius : 0.0f;
    const bool lower = (m_Mode == TerrainBrushMode::Lower);
    // Stroked path (sculpt shape-accuracy S3): with GE_TERRAIN_ANALYTIC_MODIFIERS on, the dab is
    // held as a TRANSIENT analytic placement while the stroke lasts (exact circle at any radius)
    // and committed to the store on mouse-up (EndSphereStroke -> CommitSphereSculptStroke). Flag
    // off routes to the immediate store write inside — the pre-S3 path, byte-identical.
    const CBTTerrain::SphereEditRegions regions = svc->ApplySphereSculptDabStroked(
        hitDir.x, hitDir.y, hitDir.z, angularRadius, m_Strength, lower);

    if (ZoneDebugEnabled())
    {
        for (uint32_t i = 0; i < regions.Count; ++i)
        {
            const CBTTerrain::SphereFaceUVRect& r = regions.Rects[i];
            Logger::Log::Info("Planet.Dab face={} rect=({:.3f},{:.3f},{:.3f},{:.3f}) dir=({:.2f},{:.2f},{:.2f}) angR={:.4f}",
                              r.Face, r.MinU, r.MinV, r.MaxU, r.MaxV, hitDir.x, hitDir.y, hitDir.z,
                              angularRadius);
        }
    }
}

void TerrainBrushTool::BeginSphereStroke()
{
    auto* svc = TerrainService::TryGet();
    if (!svc)
        return;
    svc->BeginSphereSculptStrokeCapture();
    // Both flags rise together so "a stroke is open" and "a sphere capture is armed" can
    // never disagree — the End phase closes by which model is open.
    m_SphereStroking = true;
    m_IsStroking = true;
}

bool TerrainBrushTool::EndSphereStroke()
{
    if (!m_SphereStroking)
        return false;
    m_SphereStroking = false;
    auto* svc = TerrainService::TryGet();
    if (!svc)
        return false;
    // Commit the held stroke's queued dabs to the store FIRST (S3 analytic-while-stroking): the
    // replay runs the same ApplyDab writes an immediate stroke would have, WHILE the capture is
    // still active — so the pre-images below are exactly the pages the stroke wrote and the
    // transient contributes nothing to the undo payload (it is gone by Take). Flag-off strokes
    // wrote the store immediately; the commit is then an empty no-op.
    svc->CommitSphereSculptStroke();
    // Always drain the capture (it must not leak into the next stroke), then decide whether the
    // stroke warrants an entry: an empty capture means no page was written (missed the planet,
    // zero strength, paint mode) — nothing to undo.
    std::vector<CBTTerrain::SphereSculptPageState> before = svc->TakeSphereSculptStrokeCapture();
    if (before.empty() || !m_UndoRedo)
        return false;
    std::vector<CBTTerrain::SphereSculptPageState> after = svc->SnapshotSphereSculptPages(before);
    m_UndoRedo->CommitAlreadyApplied(std::make_unique<Editor::SphereSculptStrokeCommand>(
        svc, std::move(before), std::move(after)));
    return true;
}

void TerrainBrushTool::OnPointerEvent(const ScenePointerEvent& event)
{
    // The pointer path's only exclusive job is turning a camera ray into a surface point
    // (and driving the cursor gizmo). The stroke itself — including the planet/planar
    // dispatch — runs in ApplyStrokePhase, which automation drives with the same points.
    Mathematics::Vector3 hit{};
    Mathematics::Vector3 normal{0.0f, 1.0f, 0.0f};
    const bool hitSurface = RaycastSurface(event.ray, hit, normal);

    if (hitSurface)
    {
        m_Cursor.Center = hit;
        m_Cursor.Normal = normal;
        m_Cursor.Radius = m_Radius;
        m_Cursor.Mode = m_Mode;
        m_Cursor.Active = true;
    }
    else
    {
        m_Cursor.Active = false;
    }

    switch (event.phase)
    {
    case PointerPhase::Down:
        if (event.button == PointerButton::Left && hitSurface)
            ApplyStrokePhase(TerrainStrokePhase::Begin, hit);
        break;
    case PointerPhase::Move:
        if (m_IsStroking && hitSurface)
            ApplyStrokePhase(TerrainStrokePhase::Continue, hit);
        break;
    case PointerPhase::Up:
        ApplyStrokePhase(TerrainStrokePhase::End, hit);
        break;
    }
}

void TerrainBrushTool::OnKeyEvent(const SceneKeyEvent& event)
{
    if (!event.pressed)
        return;
    // '[' / ']' adjust radius; '-' / '=' adjust strength; 1/2/3 pick the mode.
    switch (event.keyCode)
    {
    case '[': m_Radius = std::max(kMinBrushRadius, m_Radius - kBrushRadiusStep); break;
    case ']': m_Radius = std::min(kMaxBrushRadius, m_Radius + kBrushRadiusStep); break;
    case '-': m_Strength = std::max(kMinBrushStrength, m_Strength - kBrushStrengthStep); break;
    case '=': m_Strength = std::min(kMaxBrushStrength, m_Strength + kBrushStrengthStep); break;
    case '1': m_Mode = TerrainBrushMode::Raise; break;
    case '2': m_Mode = TerrainBrushMode::Lower; break;
    case '3': m_Mode = TerrainBrushMode::Paint; break;
    default: break;
    }
}

void TerrainBrushTool::GatherGizmos(GizmoCollector& collector)
{
    collector.AddGizmo(&m_Cursor);
}

bool TerrainBrushTool::EnsureActiveZone(const Mathematics::Vector3& hit)
{
    auto* world = &m_Owner.GetWorld();
    auto* svc = TerrainService::TryGet();
    if (!svc)
        return false;

    const bool paint = (m_Mode == TerrainBrushMode::Paint);

    // 1) A selected entity that already carries a matching zone component.
    for (const ECS::EntityHandle e : m_Owner.GetSelectedEntities())
    {
        if (paint && world->GetComponent<Components::TerrainPaintZone>(e))
        {
            const auto* z = world->GetComponent<Components::TerrainPaintZone>(e);
            m_ActiveZone = e;
            m_ActivePayload = z->PayloadRef.ToGuid();
            m_CreatedZoneThisStroke = false;
            if (m_ActivePayload.IsNull())
            {
                m_ActivePayload = GUID::Generate();
                Components::TerrainPaintZone updated = *z;
                updated.PayloadRef.Set(m_ActivePayload);
                world->AddComponentImmediate(e, updated);
                svc->EnsureZonePayload(m_ActivePayload, ZonePayloadFormat::PaintMaskR8,
                                       TerrainECS::PayloadDimForExtent(z->ExtentX, m_Authoring),
                                       TerrainECS::PayloadDimForExtent(z->ExtentZ, m_Authoring));
                m_CreatedZoneThisStroke = false; // component pre-existed
            }
            else if (!svc->GetZonePayload(m_ActivePayload))
            {
                svc->ResolveZonePayload(m_ActivePayload);
                if (!svc->GetZonePayload(m_ActivePayload))
                    svc->EnsureZonePayload(m_ActivePayload, ZonePayloadFormat::PaintMaskR8,
                                           TerrainECS::PayloadDimForExtent(z->ExtentX, m_Authoring),
                                           TerrainECS::PayloadDimForExtent(z->ExtentZ, m_Authoring));
            }
            return true;
        }
        if (!paint && world->GetComponent<Components::TerrainSculptZone>(e))
        {
            const auto* z = world->GetComponent<Components::TerrainSculptZone>(e);
            m_ActiveZone = e;
            m_ActivePayload = z->PayloadRef.ToGuid();
            m_CreatedZoneThisStroke = false;
            if (m_ActivePayload.IsNull())
            {
                m_ActivePayload = GUID::Generate();
                Components::TerrainSculptZone updated = *z;
                updated.PayloadRef.Set(m_ActivePayload);
                world->AddComponentImmediate(e, updated);
                svc->EnsureZonePayload(m_ActivePayload, ZonePayloadFormat::SculptOffsetR32F,
                                       TerrainECS::PayloadDimForExtent(z->ExtentX, m_Authoring),
                                       TerrainECS::PayloadDimForExtent(z->ExtentZ, m_Authoring));
            }
            else if (!svc->GetZonePayload(m_ActivePayload))
            {
                svc->ResolveZonePayload(m_ActivePayload);
                if (!svc->GetZonePayload(m_ActivePayload))
                    svc->EnsureZonePayload(m_ActivePayload, ZonePayloadFormat::SculptOffsetR32F,
                                           TerrainECS::PayloadDimForExtent(z->ExtentX, m_Authoring),
                                           TerrainECS::PayloadDimForExtent(z->ExtentZ, m_Authoring));
            }
            return true;
        }
    }

    // 2) Auto-create a zone fitted to this first dab (identity transform).
    m_Footprint = TerrainECS::FitZoneToDab(hit.x, hit.z, m_Radius, m_Authoring);
    const uint32_t dimX = TerrainECS::PayloadDimForExtent(m_Footprint.ExtentX, m_Authoring);
    const uint32_t dimZ = TerrainECS::PayloadDimForExtent(m_Footprint.ExtentZ, m_Authoring);
    m_ActivePayload = GUID::Generate();
    svc->EnsureZonePayload(m_ActivePayload,
                           paint ? ZonePayloadFormat::PaintMaskR8 : ZonePayloadFormat::SculptOffsetR32F,
                           dimX, dimZ);

    ECS::EntityHandle e = world->CreateEntity();

    Components::Transform xf{};
    xf.matrix[12] = m_Footprint.CenterX;
    xf.matrix[13] = hit.y;
    xf.matrix[14] = m_Footprint.CenterZ;
    world->AddComponentImmediate(e, xf);
    Components::WorldTransform wxf{};
    std::memcpy(wxf.matrix, xf.matrix, sizeof(wxf.matrix));
    world->AddComponentImmediate(e, wxf);

    Components::Name nm{};
    std::memset(nm.value, 0, sizeof(nm.value));
    std::strncpy(nm.value, paint ? "PaintZone" : "SculptZone", sizeof(nm.value) - 1);
    world->AddComponentImmediate(e, nm);

    if (paint)
    {
        Components::TerrainPaintZone z{};
        z.ExtentX = m_Footprint.ExtentX;
        z.ExtentZ = m_Footprint.ExtentZ;
        z.LayerIndex = s_PaintLayer;
        z.PayloadRef.Set(m_ActivePayload);
        world->AddComponentImmediate(e, z);
    }
    else
    {
        Components::TerrainSculptZone z{};
        z.ExtentX = m_Footprint.ExtentX;
        z.ExtentZ = m_Footprint.ExtentZ;
        z.Blend = Components::TerrainModifierBlend::Add;
        z.PayloadRef.Set(m_ActivePayload);
        world->AddComponentImmediate(e, z);
    }

    m_ActiveZone = e;
    m_CreatedZoneThisStroke = true;

    if (ZoneDebugEnabled())
        Logger::Log::Info("Zone.Create {} {} center=({:.1f},{:.1f}) extent=({:.1f},{:.1f}) dim={}x{}",
                          paint ? "paint" : "sculpt", m_ActivePayload.ToString(),
                          m_Footprint.CenterX, m_Footprint.CenterZ,
                          m_Footprint.ExtentX, m_Footprint.ExtentZ, dimX, dimZ);
    return true;
}

void TerrainBrushTool::BeginStroke(const Mathematics::Vector3& hit)
{
    auto* world = &m_Owner.GetWorld();
    auto* svc = TerrainService::TryGet();
    if (!svc)
        return;

    if (!EnsureActiveZone(hit))
        return;

    m_ZoneWasPaint = (m_Mode == TerrainBrushMode::Paint);
    // Capture the zone's pre-stroke state for the undo revert. Only a stroke on
    // an EXISTING zone needs it — an auto-created zone's undo destroys the
    // entity, so its "before" is empty.
    m_PayloadBefore = TerrainZonePayload{};
    m_SculptBefore = Components::TerrainSculptZone{};
    m_PaintBefore = Components::TerrainPaintZone{};
    m_TransformBefore = Components::Transform{};
    m_WorldTransformBefore = Components::WorldTransform{};
    if (!m_CreatedZoneThisStroke)
    {
        if (const TerrainZonePayload* p = svc->GetZonePayload(m_ActivePayload)) m_PayloadBefore = *p;
        if (const auto* c = world->GetComponent<Components::TerrainSculptZone>(m_ActiveZone)) m_SculptBefore = *c;
        if (const auto* c = world->GetComponent<Components::TerrainPaintZone>(m_ActiveZone)) m_PaintBefore = *c;
        if (const auto* c = world->GetComponent<Components::Transform>(m_ActiveZone)) m_TransformBefore = *c;
        if (const auto* c = world->GetComponent<Components::WorldTransform>(m_ActiveZone)) m_WorldTransformBefore = *c;
    }

    m_IsStroking = true;
    ApplyDab(hit);
}

void TerrainBrushTool::ApplyDab(const Mathematics::Vector3& hit)
{
    auto* world = &m_Owner.GetWorld();
    auto* svc = TerrainService::TryGet();
    if (!svc || m_ActivePayload.IsNull())
        return;

    TerrainZonePayload* payload = svc->GetZonePayload(m_ActivePayload);
    if (!payload)
        return;

    // Auto zones grow to keep the dab inside the payload; existing zones don't
    // (the user resizes them via the gizmo), so a dab outside them just clamps.
    if (m_CreatedZoneThisStroke)
    {
        const bool inside = std::abs(hit.x - m_Footprint.CenterX) + m_Radius <= m_Footprint.ExtentX
            && std::abs(hit.z - m_Footprint.CenterZ) + m_Radius <= m_Footprint.ExtentZ;
        if (!inside)
        {
            ZoneFootprint grown = m_Footprint;
            if (TerrainECS::GrowZoneToDab(grown, hit.x, hit.z, m_Radius, m_Authoring))
            {
                const uint32_t nx = TerrainECS::PayloadDimForExtent(grown.ExtentX, m_Authoring);
                const uint32_t nz = TerrainECS::PayloadDimForExtent(grown.ExtentZ, m_Authoring);
                TerrainZonePayload regrown = TerrainECS::RegrowPayload(*payload, m_Footprint, grown, nx, nz);
                svc->SetZonePayload(m_ActivePayload, std::move(regrown));
                payload = svc->GetZonePayload(m_ActivePayload);

                // Reflect the new footprint on the component + transform.
                if (auto* z = world->GetComponent<Components::TerrainSculptZone>(m_ActiveZone))
                {
                    Components::TerrainSculptZone u = *z;
                    u.ExtentX = grown.ExtentX; u.ExtentZ = grown.ExtentZ;
                    world->AddComponentImmediate(m_ActiveZone, u);
                }
                if (auto* z = world->GetComponent<Components::TerrainPaintZone>(m_ActiveZone))
                {
                    Components::TerrainPaintZone u = *z;
                    u.ExtentX = grown.ExtentX; u.ExtentZ = grown.ExtentZ;
                    world->AddComponentImmediate(m_ActiveZone, u);
                }
                if (auto* xf = world->GetComponentForWrite<Components::Transform>(m_ActiveZone))
                {
                    xf->matrix[12] = grown.CenterX;
                    xf->matrix[14] = grown.CenterZ;
                }
                if (auto* wxf = world->GetComponentForWrite<Components::WorldTransform>(m_ActiveZone))
                {
                    wxf->matrix[12] = grown.CenterX;
                    wxf->matrix[14] = grown.CenterZ;
                }
                if (ZoneDebugEnabled())
                    Logger::Log::Info("Zone.Grow {} extent ({:.1f},{:.1f})->({:.1f},{:.1f}) dim={}x{}",
                                      m_ActivePayload.ToString(), m_Footprint.ExtentX, m_Footprint.ExtentZ,
                                      grown.ExtentX, grown.ExtentZ, nx, nz);
                m_Footprint = grown;
            }
            else
            {
                // Crossed the cap: finish this zone and start a fresh one. The
                // guard bounds this against a pathological radius > cap (the
                // fresh zone could never contain its own dab) — after a couple
                // of restarts, keep painting the current zone clamped.
                if (m_CapRestartGuard >= 2)
                    return;
                ++m_CapRestartGuard;
                FinalizeStroke();
                BeginStroke(hit);
                return;
            }
        }
    }

    // Build the world<->texel mapping for this zone.
    ZoneMapping map;
    if (m_CreatedZoneThisStroke)
    {
        map = MappingFromFootprint(m_Footprint, payload->Width, payload->Height);
    }
    else if (const auto* wxf = world->GetComponent<Components::WorldTransform>(m_ActiveZone))
    {
        float extentX = 1.0f, extentZ = 1.0f;
        if (const auto* zs = world->GetComponent<Components::TerrainSculptZone>(m_ActiveZone))
        { extentX = zs->ExtentX; extentZ = zs->ExtentZ; }
        else if (const auto* zp = world->GetComponent<Components::TerrainPaintZone>(m_ActiveZone))
        { extentX = zp->ExtentX; extentZ = zp->ExtentZ; }
        map = MappingFromWorldTransform(*wxf, extentX, extentZ, payload->Width, payload->Height);
    }
    else
    {
        return;
    }

    // Dab's texel range: map ALL FOUR corners of the dab's world AABB through
    // the (possibly rotated) zone mapping, then take the min/max. Mapping only
    // two opposite corners under-covers a rotated zone (worst at 45 degrees).
    float fMinTx = std::numeric_limits<float>::max();
    float fMinTz = std::numeric_limits<float>::max();
    float fMaxTx = -std::numeric_limits<float>::max();
    float fMaxTz = -std::numeric_limits<float>::max();
    const float cornersX[2] = {hit.x - m_Radius, hit.x + m_Radius};
    const float cornersZ[2] = {hit.z - m_Radius, hit.z + m_Radius};
    for (int ci = 0; ci < 2; ++ci)
        for (int cj = 0; cj < 2; ++cj)
        {
            float tx, tz;
            map.WorldToTexel(cornersX[ci], cornersZ[cj], tx, tz);
            fMinTx = std::min(fMinTx, tx);
            fMinTz = std::min(fMinTz, tz);
            fMaxTx = std::max(fMaxTx, tx);
            fMaxTz = std::max(fMaxTz, tz);
        }
    const int32_t minTx = std::clamp(static_cast<int32_t>(std::floor(fMinTx)), 0,
                                     static_cast<int32_t>(payload->Width) - 1);
    const int32_t maxTx = std::clamp(static_cast<int32_t>(std::ceil(fMaxTx)), 0,
                                     static_cast<int32_t>(payload->Width) - 1);
    const int32_t minTz = std::clamp(static_cast<int32_t>(std::floor(fMinTz)), 0,
                                     static_cast<int32_t>(payload->Height) - 1);
    const int32_t maxTz = std::clamp(static_cast<int32_t>(std::ceil(fMaxTz)), 0,
                                     static_cast<int32_t>(payload->Height) - 1);
    if (minTx > maxTx || minTz > maxTz)
        return;

    const bool paint = (m_Mode == TerrainBrushMode::Paint);
    const float sign = (m_Mode == TerrainBrushMode::Lower) ? -1.0f : 1.0f;
    const float invRadius = m_Radius > 0.0f ? 1.0f / m_Radius : 0.0f;

    for (int32_t tz = minTz; tz <= maxTz; ++tz)
    {
        for (int32_t tx = minTx; tx <= maxTx; ++tx)
        {
            float wx, wz;
            map.TexelToWorld(static_cast<uint32_t>(tx), static_cast<uint32_t>(tz), wx, wz);
            const float dx = wx - hit.x;
            const float dz = wz - hit.z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            if (dist > m_Radius)
                continue;
            const float t = std::clamp(1.0f - dist * invRadius, 0.0f, 1.0f);
            const float falloff = t * t * (3.0f - 2.0f * t); // smoothstep
            const size_t idx = static_cast<size_t>(tz) * payload->Width + tx;
            if (paint)
            {
                const float w = static_cast<float>(payload->Mask[idx]) + 255.0f * m_Strength * falloff;
                payload->Mask[idx] = static_cast<uint8_t>(std::clamp(w, 0.0f, 255.0f));
            }
            else
            {
                payload->Offsets[idx] += sign * m_Strength * falloff;
            }
        }
    }

    svc->NotifyZonePayloadEdited(m_ActivePayload, minTx, minTz, maxTx + 1, maxTz + 1);

    if (ZoneDebugEnabled())
        Logger::Log::Info("Zone.Dab {} texels[{},{}..{},{}] world=({:.1f},{:.1f}) r={:.1f}",
                          m_ActivePayload.ToString(), minTx, minTz, maxTx, maxTz, hit.x, hit.z, m_Radius);
}

void TerrainBrushTool::FinalizeStroke()
{
    auto* world = &m_Owner.GetWorld();
    auto* svc = TerrainService::TryGet();
    const bool created = m_CreatedZoneThisStroke;

    m_IsStroking = false;
    m_CreatedZoneThisStroke = false;

    if (!svc || m_ActivePayload.IsNull())
    {
        // The dabs already mutated the payload/terrain; without a commit here
        // the edit is live but has no undo entry. Surface it rather than lose
        // the stroke silently.
        Logger::Log::Warning("TerrainBrushTool: stroke finalized with no world/service — edit is not undoable");
        m_PayloadBefore = TerrainZonePayload{};
        return;
    }

    if (m_UndoRedo && world->IsValid(m_ActiveZone) && svc->GetZonePayload(m_ActivePayload))
    {
        // Pre-stroke state (for a non-created stroke's revert).
        Editor::ZoneStrokeState before;
        before.IsPaint = m_ZoneWasPaint;
        before.Sculpt = m_SculptBefore;
        before.Paint = m_PaintBefore;
        before.Transform = m_TransformBefore;
        before.WorldTransform = m_WorldTransformBefore;
        if (const auto* c = world->GetComponent<Components::Name>(m_ActiveZone)) before.Name = *c;
        before.Payload = m_PayloadBefore;

        // Post-stroke state (for redo / created re-create).
        Editor::ZoneStrokeState after = CaptureZoneState(*world, *svc, m_ActiveZone, m_ActivePayload, m_ZoneWasPaint);

        // The command owns the entity/component/transform/payload revert AND its
        // own structure notification (undo/redo both fire it), so the hierarchy
        // stays in sync — no separate compound or post-commit notify needed.
        m_UndoRedo->CommitAlreadyApplied(std::make_unique<Editor::TerrainZoneStrokeCommand>(
            world, m_ChangeNotifications, svc, m_ActiveZone, m_ActivePayload, created,
            std::move(before), std::move(after)));

        if (created && m_ChangeNotifications)
        {
            EditorChangeNotifications::WorldStructureChangedEvent e{};
            e.world = world;
            e.kind = EditorChangeNotifications::ChangeKind::Commit;
            m_ChangeNotifications->NotifyWorldStructureChanged(e);
        }
    }

    m_PayloadBefore = TerrainZonePayload{};
}

} // namespace GameEngine::Editor::SceneTools
