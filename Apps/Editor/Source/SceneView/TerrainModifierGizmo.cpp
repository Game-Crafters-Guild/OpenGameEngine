#include "SceneView/TerrainModifierGizmo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "SceneView/TerrainModifierGizmoGeometry.h"
#include "TerrainECS/PlanarHeightQuery.h"

#include "CBTTerrainECS/CBTRenderFeature.h"
#include "CBTTerrainECS/TerrainProvisioning.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Components.h"
#include "ECS/ECS.h"
#include "ECS/Query.h"
#include "Engine/Rendering/RenderServices.h"
#include "TerrainECS/TerrainService.h"

namespace GameEngine::Editor::SceneTools
{

namespace Geo = ModifierGizmoGeometry;

using GameEngine::Components::Terrain;
using GameEngine::Components::TerrainDomain;
using GameEngine::Components::TerrainFlattenEffect;
using GameEngine::Components::TerrainModifierVolume;
using GameEngine::Components::TerrainNoiseEffect;
using GameEngine::Components::TerrainStampEffect;
using GameEngine::Components::TerrainVolumeShape;
using GameEngine::Components::TerrainModifierShape;
using GameEngine::Components::TerrainPaintZone;
using GameEngine::Components::TerrainSculptZone;
using GameEngine::Components::WorldTransform;

namespace
{

// Disc tessellation for footprint rings. Matches LocalVolumeGizmo's ellipse
// resolution so the two gizmos read at the same smoothness.
constexpr std::uint32_t kRingSegments = 48u;

// Footprint outline color (teal) and its dimmer falloff-ring variant.
constexpr float kFootprintRGB[3] = {0.20f, 0.85f, 0.78f};
// Draped surface-projection color (amber) — "what it affects" on the ground.
constexpr float kSurfaceRGB[3] = {1.00f, 0.78f, 0.25f};
// Placed-but-inert on this planet (a type the sphere bake does not apply yet).
constexpr float kInertRGB[3] = {0.62f, 0.62f, 0.66f};

// The active terrain the modifiers apply to, resolved once per Render. Planar
// terrains carry a heightfield the footprint is draped onto; a planet carries a
// radius (centre = world origin, matching MakeSpherePlacement / RaycastPlanet).
struct TerrainContext
{
    bool  Valid = false;
    bool  Spherical = false;

    // Planet (Spherical).
    float PlanetRadius = 0.0f;
    CBTTerrainECS::CBTRenderFeature* Feature = nullptr;

    // Planar heightfield sampling, shared with the spline fill.
    TerrainECS::PlanarHeightQuery Planar;
};

// The single active terrain: first enabled terrain (single-active-terrain by
// design, mirroring CBTTerrainECS::FindActiveTerrain's scope). The planar half —
// the heightfield the footprint drapes onto — is resolved by the shared query;
// the domain flag and the planet's radius/feature are the gizmo's own, because
// only the drawing code needs them.
TerrainContext ResolveTerrainContext(ECS::World& world)
{
    TerrainContext ctx{};
    ctx.Planar = TerrainECS::ResolvePlanarHeightQuery(world);

    bool found = false;
    world.Query<ECS::Read<Terrain>, ECS::Read<WorldTransform>>()
        .Each([&](const Terrain& terrain, const WorldTransform&)
        {
            if (found)
                return;
            found = true;
            ctx.Valid = true;
            ctx.Spherical = terrain.Domain == TerrainDomain::Spherical;
            ctx.PlanetRadius = terrain.PlanetRadius;
        });

    if (ctx.Valid && ctx.Spherical)
    {
        if (auto* rs = EngineCore::GetInstance().GetRenderServices())
            ctx.Feature = rs->GetFeature<CBTTerrainECS::CBTRenderFeature>();
    }
    return ctx;
}

struct Footprint
{
    Geo::FootprintShape Shape = Geo::FootprintShape::Disc;
    float WorldX = 0.0f, WorldY = 0.0f, WorldZ = 0.0f;
    float Radius = 0.0f, RectHalfX = 0.0f, RectHalfZ = 0.0f, Falloff = 0.0f, Yaw = 0.0f;
    bool  SphereSupported = false; // does the sphere bake apply this modifier type?
};

float ExtractYaw(const WorldTransform& xf)
{
    return std::atan2(xf.matrix[8], xf.matrix[10]);
}

// Draw a planar footprint: the authored outline at the entity Y (teal, inner +
// falloff), both of those boundaries draped onto the terrain surface (amber, the
// outer ring at half alpha), and a few vertical spokes tying the inner pair so the
// affected column reads clearly.
void DrawPlanar(GizmoRenderContext& ctx, const TerrainContext& terrain, const Footprint& fp,
                bool selected)
{
    const float alpha = selected ? 1.0f : 0.6f;
    const float thickness = selected ? 2.0f : 1.4f;

    const float outerRadius = fp.Radius + std::max(fp.Falloff, 0.0f);
    const float outerHX = fp.RectHalfX + std::max(fp.Falloff, 0.0f);
    const float outerHZ = fp.RectHalfZ + std::max(fp.Falloff, 0.0f);

    // Authored footprint outline at the entity Y (the "where it is" volume).
    {
        const Color color(kFootprintRGB[0], kFootprintRGB[1], kFootprintRGB[2], alpha);
        std::vector<Mathematics::Vector3> lines;
        Geo::AppendPlanarFootprint(lines, fp.Shape, fp.WorldX, fp.WorldY, fp.WorldZ, fp.Radius,
                                   fp.RectHalfX, fp.RectHalfZ, fp.Yaw, kRingSegments);
        if (fp.Falloff > 0.0f)
            Geo::AppendPlanarFootprint(lines, fp.Shape, fp.WorldX, fp.WorldY, fp.WorldZ, outerRadius,
                                       outerHX, outerHZ, fp.Yaw, kRingSegments);
        if (!lines.empty())
            ctx.DrawColoredLines(lines.data(), lines.size() / 2u, color, thickness);
    }

    // Affected region draped onto the terrain surface (the "what it affects").
    // Both boundaries drape: the weight leaves 0 at the OUTER edge and reaches full
    // strength at the inner one, and an effect whose only authored quantity is a
    // target inside that ramp (a grass region) is tuned by the outer edge. Half alpha
    // on the outer ring, matching the spherical path.
    std::vector<std::array<float, 3>> ring, draped;
    bool anyDraped = false;
    const auto drapeRing = [&](float radius, float halfX, float halfZ)
    {
        ring.clear();
        draped.clear();
        Geo::ComputePlanarRingPoints(ring, fp.Shape, fp.WorldX, fp.WorldY, fp.WorldZ, radius,
                                     halfX, halfZ, fp.Yaw, kRingSegments);
        draped.reserve(ring.size());
        anyDraped = false;
        for (const auto& p : ring)
        {
            float y;
            if (terrain.Planar.SampleHeight(p[0], p[2], y))
            {
                draped.push_back({p[0], y, p[2]});
                anyDraped = true;
            }
            else
            {
                draped.push_back(p); // fall back to the authored Y where the surface is absent
            }
        }
    };

    if (fp.Falloff > 0.0f)
    {
        drapeRing(outerRadius, outerHX, outerHZ);
        if (anyDraped)
        {
            const Color outerColor(kSurfaceRGB[0], kSurfaceRGB[1], kSurfaceRGB[2], alpha * 0.5f);
            std::vector<Mathematics::Vector3> outerLines;
            Geo::AppendClosedLoopSegments(outerLines, draped);
            ctx.DrawColoredLines(outerLines.data(), outerLines.size() / 2u, outerColor, thickness);
        }
    }

    drapeRing(fp.Radius, fp.RectHalfX, fp.RectHalfZ);
    if (anyDraped)
    {
        const Color color(kSurfaceRGB[0], kSurfaceRGB[1], kSurfaceRGB[2], alpha);
        std::vector<Mathematics::Vector3> lines;
        Geo::AppendClosedLoopSegments(lines, draped);
        ctx.DrawColoredLines(lines.data(), lines.size() / 2u, color, thickness);

        // Vertical spokes at up to four ring points connect the authored footprint
        // to the draped region so the affected column is legible when the entity
        // floats above the surface.
        const Color spokeColor(kFootprintRGB[0], kFootprintRGB[1], kFootprintRGB[2], alpha * 0.6f);
        std::vector<Mathematics::Vector3> spokes;
        const std::size_t n = ring.size();
        const std::size_t step = std::max<std::size_t>(1u, n / 4u);
        for (std::size_t i = 0; i < n; i += step)
        {
            spokes.emplace_back(ring[i][0], ring[i][1], ring[i][2]);
            spokes.emplace_back(draped[i][0], draped[i][1], draped[i][2]);
        }
        if (!spokes.empty())
            ctx.DrawColoredLines(spokes.data(), spokes.size() / 2u, spokeColor, thickness);
    }
}

// Draw a planet footprint projected radially onto the sphere. Supported types
// (Noise / SculptZone) draw in teal; types the sphere bake does not apply yet
// draw greyed so a placed-but-inert modifier is honest rather than implying it
// affects the terrain.
void DrawSpherical(GizmoRenderContext& ctx, const TerrainContext& terrain, const Footprint& fp,
                   bool selected)
{
    if (terrain.PlanetRadius <= 0.0f)
        return;
    const std::array<float, 3> planetCenter = {0.0f, 0.0f, 0.0f};
    const std::array<float, 3> modPos = {fp.WorldX, fp.WorldY, fp.WorldZ};
    const Geo::SphereFrame frame = Geo::MakeSphereFrame(planetCenter, modPos);
    if (!frame.Valid)
        return; // modifier at the planet centre has no radial direction

    const float alpha = selected ? 1.0f : 0.6f;
    const float thickness = selected ? 2.0f : 1.4f;
    const float* rgb = fp.SphereSupported ? kFootprintRGB : kInertRGB;
    const Color color(rgb[0], rgb[1], rgb[2], alpha);

    // Lift the ring onto the visible relief/sculpt surface so it hugs the planet.
    CBTTerrainECS::CBTRenderFeature* feature = terrain.Feature;
    auto heightAbove = [feature](float dx, float dy, float dz) -> float
    {
        return feature ? feature->SampleSphereSurfaceHeight(dx, dy, dz) : 0.0f;
    };

    const float outerRadius = fp.Radius + std::max(fp.Falloff, 0.0f);
    const float outerHX = fp.RectHalfX + std::max(fp.Falloff, 0.0f);
    const float outerHZ = fp.RectHalfZ + std::max(fp.Falloff, 0.0f);

    std::vector<Mathematics::Vector3> lines;
    Geo::AppendSphereFootprint(lines, planetCenter, frame, terrain.PlanetRadius, fp.Shape,
                               fp.Radius, fp.RectHalfX, fp.RectHalfZ, fp.Yaw, kRingSegments,
                               heightAbove);
    if (fp.Falloff > 0.0f)
    {
        const Color outerColor(rgb[0], rgb[1], rgb[2], alpha * 0.5f);
        std::vector<Mathematics::Vector3> outer;
        Geo::AppendSphereFootprint(outer, planetCenter, frame, terrain.PlanetRadius, fp.Shape,
                                   outerRadius, outerHX, outerHZ, fp.Yaw, kRingSegments,
                                   heightAbove);
        if (!outer.empty())
            ctx.DrawColoredLines(outer.data(), outer.size() / 2u, outerColor, thickness);
    }
    if (!lines.empty())
        ctx.DrawColoredLines(lines.data(), lines.size() / 2u, color, thickness);
}

void DrawFootprint(GizmoRenderContext& ctx, const TerrainContext& terrain, const Footprint& fp,
                   bool selected)
{
    if (terrain.Spherical)
        DrawSpherical(ctx, terrain, fp, selected);
    else
        DrawPlanar(ctx, terrain, fp, selected);
}

bool IsEmphasized(ECS::EntityHandle e, const std::vector<ECS::EntityHandle>& selected,
                  ECS::EntityHandle hovered, bool& outSelected)
{
    outSelected = std::find(selected.begin(), selected.end(), e) != selected.end();
    const bool hover = hovered.IsValid() && e == hovered;
    return outSelected || hover;
}

// Paintable zones (SculptZone / PaintZone) are rectangular; their world half-
// extents are the local extents times the transform's XZ scale, matching the
// bake's ExtractXZScale.
template <typename ZoneT>
void DrawZoneModifiers(GizmoRenderContext& ctx, ECS::World& world, const TerrainContext& terrain,
                       const std::vector<ECS::EntityHandle>& selected, ECS::EntityHandle hovered,
                       bool sphereSupported)
{
    world.Query<ECS::Read<ZoneT>, ECS::Read<WorldTransform>>()
        .Each([&](ECS::EntityHandle e, const ZoneT& zone, const WorldTransform& xf)
        {
            bool sel = false;
            if (!IsEmphasized(e, selected, hovered, sel))
                return;
            const float scaleX = std::sqrt(xf.matrix[0] * xf.matrix[0] + xf.matrix[1] * xf.matrix[1]
                                           + xf.matrix[2] * xf.matrix[2]);
            const float scaleZ = std::sqrt(xf.matrix[8] * xf.matrix[8] + xf.matrix[9] * xf.matrix[9]
                                           + xf.matrix[10] * xf.matrix[10]);
            Footprint fp{};
            fp.Shape = Geo::FootprintShape::Rect;
            fp.WorldX = xf.matrix[12];
            fp.WorldY = xf.matrix[13];
            fp.WorldZ = xf.matrix[14];
            fp.RectHalfX = zone.ExtentX * scaleX;
            fp.RectHalfZ = zone.ExtentZ * scaleZ;
            fp.Falloff = zone.Falloff;
            fp.Yaw = ExtractYaw(xf);
            fp.SphereSupported = sphereSupported;
            DrawFootprint(ctx, terrain, fp, sel);
        });
}

// A modifier volume draws the region its whole effect stack shares. Rect and
// circle shapes draw the same footprint the zones do; spline
// shapes are visualized by the SplineSceneGizmo that already renders the path
// they are bound to, so they are skipped here rather than drawn as a rectangle.
// A global volume is skipped too: it has no boundary, and drawing its authored
// half-extents would show an edge the bake does not have.
//
// Sphere support follows the bake: a volume greys out on a planet unless it
// carries at least one effect the sphere path applies (noise / flatten / stamp)
// with a non-spline shape - the same predicate as IsSphereSupportedModifier.
void DrawModifierVolumes(GizmoRenderContext& ctx, ECS::World& world, const TerrainContext& terrain,
                         const std::vector<ECS::EntityHandle>& selected, ECS::EntityHandle hovered)
{
    world.Query<ECS::Read<TerrainModifierVolume>, ECS::Read<WorldTransform>>()
        .Each([&](ECS::EntityHandle e, const TerrainModifierVolume& vol, const WorldTransform& xf)
        {
            if (vol.Shape == TerrainVolumeShape::SplinePath || vol.Shape == TerrainVolumeShape::SplineArea
                || vol.Shape == TerrainVolumeShape::Global)
                return;
            bool sel = false;
            if (!IsEmphasized(e, selected, hovered, sel))
                return;

            const auto* flatten = world.GetComponent<TerrainFlattenEffect>(e);
            const auto* noise = world.GetComponent<TerrainNoiseEffect>(e);
            const auto* stamp = world.GetComponent<TerrainStampEffect>(e);
            const bool sphereSupported = (flatten && flatten->Enabled) || (noise && noise->Enabled)
                                      || (stamp && stamp->Enabled);

            Footprint fp{};
            fp.Shape = vol.Shape == TerrainVolumeShape::Rectangle ? Geo::FootprintShape::Rect
                                                                 : Geo::FootprintShape::Disc;
            fp.WorldX = xf.matrix[12];
            fp.WorldY = xf.matrix[13];
            fp.WorldZ = xf.matrix[14];
            fp.Radius = vol.Radius;
            fp.RectHalfX = vol.RectHalfX;
            fp.RectHalfZ = vol.RectHalfZ;
            fp.Falloff = vol.Falloff;
            fp.Yaw = ExtractYaw(xf);
            fp.SphereSupported = sphereSupported;
            DrawFootprint(ctx, terrain, fp, sel);
        });
}

} // namespace

void TerrainModifierGizmo::SetSelection(const std::vector<GameEngine::ECS::EntityHandle>& entities)
{
    m_SelectedEntities = entities;
}

void TerrainModifierGizmo::SetHovered(GameEngine::ECS::EntityHandle entity)
{
    m_HoveredEntity = entity;
}

void TerrainModifierGizmo::Render(GizmoRenderContext& context)
{
    if (m_SelectedEntities.empty() && !m_HoveredEntity.IsValid())
        return;

    ECS::World* world = context.GetWorld();
    if (!world)
        return;

    const TerrainContext terrain = ResolveTerrainContext(*world);
    if (!terrain.Valid)
        return;

    DrawModifierVolumes(context, *world, terrain, m_SelectedEntities, m_HoveredEntity);
    // SculptZone is sphere-supported; a paint zone is drawn greyed on a planet
    // (keep in lockstep with IsSphereSupportedModifier).
    DrawZoneModifiers<TerrainSculptZone>(context, *world, terrain, m_SelectedEntities,
                                         m_HoveredEntity, /*sphereSupported*/ true);
    DrawZoneModifiers<TerrainPaintZone>(context, *world, terrain, m_SelectedEntities,
                                        m_HoveredEntity, /*sphereSupported*/ false);
}

} // namespace GameEngine::Editor::SceneTools
