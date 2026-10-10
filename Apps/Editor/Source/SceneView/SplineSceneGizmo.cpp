#include "SceneView/SplineSceneGizmo.h"

#include "Components/Spline/SplineComponent.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Components.h"
#include "ECS/ECS.h"
#include "ECS/Query.h"
#include "Editor/Settings/SplineEditorSettings.h"
#include "Mathematics/BezierCurve.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/VectorOps.h"
#include "Placement/SplineSurfaceConform.h"
#include "SceneView/SplineDrapePolylines.h"
#include "SceneView/SplineOwnerQuery.h"
#include "SceneView/SplineTool.h"
#include "Spline/SplineData.h"
#include "SplineECS/SplineService.h"
#include "Types/ColorUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace GameEngine::Editor::SceneTools
{
namespace
{

using Mathematics::Vector3;

constexpr uint32 kSegSubdivisions = 16;
constexpr uint32 kPassiveSegSubdivisions = 4;

// Width envelope (selected spline only): the spline color at reduced alpha.
constexpr float kEnvelopeAlphaScale = 0.55f;
// Band fill between the two draped edges; kept well below the edge alpha so
// the ground and tiles stay readable through it.
constexpr float kEnvelopeFillAlphaScale = 0.28f;
// With draping on, a knot stands at its authored height while its line lies on the surface; a
// stem in the spline color at this fraction of its alpha joins the knot to the draped line below
// or above it, so a knot off the ground still reads as part of its line. No stem shorter than the
// knot marker's radius. It draws at the handles' thickness and screen scaling.
constexpr float kKnotStemAlphaScale = 0.5f;
// Cache entries stop being refreshed when their spline is deleted, disabled,
// or switches draw mode; drop them once unseen for this many Render passes.
constexpr uint64_t kGizmoCacheEvictEpochs = 64;

bool IsClaimedSpline(const ECS::World& world, ECS::EntityHandle entity)
{
    return entity.IsValid() && QuerySplineOwner(world, entity).Claimed;
}

void FlushTriangleBatch(GizmoRenderContext& context,
                        const std::vector<Vector3>& vertices,
                        const Color& color)
{
    if (!vertices.empty())
        context.DrawTriangles(vertices.data(), vertices.size(), color);
}

// Hover: brighten the base color by blending toward white; alpha is kept.
Color HoverColor(const Color& base)
{
    constexpr float kTowardWhite = 0.4f;
    return Color(base.r + (1.0f - base.r) * kTowardWhite,
                 base.g + (1.0f - base.g) * kTowardWhite,
                 base.b + (1.0f - base.b) * kTowardWhite,
                 base.a);
}

bool MatrixEquals(const float* a, const float* b)
{
    for (uint32 i = 0; i < 16u; ++i)
    {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

void AppendLine(std::vector<Vector3>& vertices, const Vector3& a, const Vector3& b)
{
    vertices.push_back(a);
    vertices.push_back(b);
}

// Each knot's draped height: the surface under the knot (the drape's own ray, Scene target) plus
// the drape's lift, NaN where the ray missed.
void DrapeKnots(ECS::World& world, const Spline::SplineData& data, const Mathematics::Matrix4x4& worldM,
                std::span<const ECS::EntityHandle> ignore, std::vector<float>& out)
{
    out.clear();
    out.reserve(data.Points.size());
    for (const Spline::SplineControlPoint& point : data.Points)
    {
        Vector3 hit;
        out.push_back(ConformRayDown(world, worldM.TransformPoint(point.Position), ignore, hit)
                          ? hit.y + kDrapeSurfaceLiftMetres
                          : std::numeric_limits<float>::quiet_NaN());
    }
}

void RebuildPassiveLineCache(const Spline::SplineData& data,
                             const Components::WorldTransform& xf,
                             std::vector<Vector3>& vertices)
{
    vertices.clear();

    const auto& srcPts = data.Points;
    const uint32 n = static_cast<uint32>(srcPts.size());
    const uint32 segCount = data.GetSegmentCount();
    if (segCount == 0u)
        return;

    Mathematics::Matrix4x4 worldM;
    worldM = Mathematics::Matrix4x4::FromColumnMajor(xf.matrix);
    const Vector3 origin = worldM.TransformPoint(Vector3(0, 0, 0));

    std::vector<Spline::SplineControlPoint> pts(srcPts.size());
    for (uint32 i = 0; i < n; ++i)
    {
        pts[i] = srcPts[i];
        pts[i].Position = worldM.TransformPoint(srcPts[i].Position);
        pts[i].TangentIn = worldM.TransformPoint(srcPts[i].TangentIn) - origin;
        pts[i].TangentOut = worldM.TransformPoint(srcPts[i].TangentOut) - origin;
    }

    const Spline::SplineType type = data.Type;
    const uint32 linesPerSegment = type == Spline::SplineType::Linear ? 1u : kPassiveSegSubdivisions;
    vertices.reserve(static_cast<size_t>(segCount) * linesPerSegment * 2u);

    for (uint32 seg = 0; seg < segCount; ++seg)
    {
        const uint32 i0 = seg;
        const uint32 i1 = (seg + 1u) % n;
        if (type == Spline::SplineType::Linear)
        {
            AppendLine(vertices, pts[i0].Position, pts[i1].Position);
            continue;
        }

        Vector3 p0, p1, p2, p3;
        if (type == Spline::SplineType::CubicBezier)
        {
            p0 = pts[i0].Position;
            p1 = pts[i0].Position + pts[i0].TangentOut;
            p2 = pts[i1].Position + pts[i1].TangentIn;
            p3 = pts[i1].Position;
        }
        else
        {
            const uint32 iPrev = data.IsEffectivelyClosed()
                ? ((i0 + n - 1u) % n)
                : (i0 > 0u ? i0 - 1u : 0u);
            const uint32 iNext = data.IsEffectivelyClosed()
                ? ((i1 + 1u) % n)
                : (i1 + 1u < n ? i1 + 1u : n - 1u);
            p0 = pts[iPrev].Position;
            p1 = pts[i0].Position;
            p2 = pts[i1].Position;
            p3 = pts[iNext].Position;
        }

        Vector3 prev = type == Spline::SplineType::CubicBezier ? p0 : p1;
        for (uint32 s = 1; s <= kPassiveSegSubdivisions; ++s)
        {
            const float32 t = static_cast<float32>(s) / static_cast<float32>(kPassiveSegSubdivisions);
            const Vector3 curr = type == Spline::SplineType::CubicBezier
                ? Math::CubicBezier(p0, p1, p2, p3, t)
                : Math::CatmullRom(p0, p1, p2, p3, t);
            AppendLine(vertices, prev, curr);
            prev = curr;
        }
    }
}

} // namespace

void SplineSceneGizmo::Render(GizmoRenderContext& context)
{
    auto* splineService = SplineECS::SplineService::TryGet();
    if (!splineService)
        return;

    auto* world = context.GetWorld();
    if (!world)
        return;

    const auto& settings = SplineEditorSettings::Get();
    const Color splineColor = ColorUtils::UnpackArgb(settings.GetSplineColor());
    const Color knotColor = ColorUtils::UnpackArgb(settings.GetKnotColor());
    const Color handleColor = ColorUtils::UnpackArgb(settings.GetHandleColor());
    const Color selectedColor = ColorUtils::UnpackArgb(settings.GetKnotSelectedColor());

    const SplineSelection& selection = m_State->Selection();
    const SplineHover& hover = m_State->Hover();
    const Color hoverKnotColor = HoverColor(knotColor);
    const Color hoverHandleColor = HoverColor(handleColor);
    const float32 lineThickness = settings.GetSplineThickness();
    const float32 knotRadius = settings.GetKnotSize();
    const float32 knotThickness = settings.GetKnotThickness();
    const float32 handleRadius = settings.GetHandleSize();
    const float32 handleThickness = settings.GetHandleThickness();
    const bool constantScreen = settings.GetConstantScreenSize();
    const bool smartDist = settings.GetSmartDistanceScaling();
    const bool drapeToSurface = settings.GetDrapeToSurface();
    const bool showWidthEnvelope = settings.GetShowWidthEnvelope();
    const bool lineSmart = constantScreen && smartDist;
    const bool canBatchThickTriangles =
        context.HasCameraWorldPosition() &&
        lineThickness > kThinLineThickness &&
        knotThickness > kThinLineThickness &&
        handleThickness > kThinLineThickness;
    const ECS::EntityHandle selectedSceneEntity = m_SelectionQuery
        ? m_SelectionQuery()
        : ECS::EntityHandle{};
    // A spline another feature owns as its shape (a mark-up region's outline) is edited alone:
    // while it is selected, "show all controls" offers no other spline's knots, as SplineTool
    // picks (CollectEditableSplineEntities).
    const bool showAllControls = settings.GetShowAllControls() && !IsClaimedSpline(*world, selectedSceneEntity) &&
                                 !IsClaimedSpline(*world, selection.Entity);
    const uint64_t cacheEpoch = ++m_CacheEpoch;

    std::vector<Vector3> splineTriBatch;
    std::vector<Vector3> knotTriBatch;
    std::vector<Vector3> handleTriBatch;
    std::vector<Vector3> selectedTriBatch;
    std::vector<Vector3> hoverKnotTriBatch;
    std::vector<Vector3> hoverHandleTriBatch;
    std::vector<Vector3> knotStems;
    const Color knotStemColor(splineColor.r, splineColor.g, splineColor.b, splineColor.a * kKnotStemAlphaScale);

    // Width envelope: the spline color at reduced alpha, so the authored width
    // band reads as a tint of its own spline. The band interior fills at a
    // still lower alpha; the transparent-group sort in the overlay pass orders
    // fill and edges by depth, both blending over the scene.
    const Color envelopeColor(splineColor.r, splineColor.g, splineColor.b,
                              splineColor.a * kEnvelopeAlphaScale);
    const Color envelopeFillColor(splineColor.r, splineColor.g, splineColor.b,
                                  splineColor.a * kEnvelopeFillAlphaScale);
    std::vector<Vector3> envelopeTriBatch;
    std::vector<Vector3> envelopeFillBatch;
    // Conform-ray exclusions, gathered once per pass and only if a drape
    // actually rebuilds this frame.
    std::optional<std::vector<ECS::EntityHandle>> conformIgnore;
    // Line-pair scratch for drawing a draped centerline as passive lines.
    std::vector<Vector3> passiveDrapedVerts;
    // The ground revision, read once per pass and only when a spline is
    // actually draped, so a scene with no splines pays no terrain scan.
    std::optional<uint64_t> passSurfaceRevision;

    // Refreshes (and rebuilds when dirty) the draped-polyline cache entry for
    // one spline. wantEdges toggles the ±width envelope edges, so selecting a
    // spline — or switching the width-envelope setting — rebuilds its entry with
    // or without them.
    const auto refreshDrapeCache = [&](ECS::EntityHandle e,
                                       const Components::SplineComponent& comp,
                                       const Spline::SplineData& data,
                                       const Components::WorldTransform& xf,
                                       const Mathematics::Matrix4x4& worldM,
                                       bool wantEdges) -> const DrapeCache&
    {
        auto& cache = m_DrapeCache[e.id];
        // The drape measures the ground, so the ground is one of its inputs:
        // without this the displayed centerline keeps a pre-bake (or pre-sculpt)
        // altitude while the placed pieces, which observe the same revision,
        // move off it.
        if (!passSurfaceRevision.has_value())
            passSurfaceRevision = ConformSurfaceRevision(*world, /*conforms by raycast*/ nullptr);
        const bool dirty =
            cache.DataIndex != comp.SplineDataIndex ||
            cache.Generation != comp.SplineDataGeneration ||
            cache.DataVersion != data.Version ||
            cache.HasEdges != wantEdges ||
            cache.SurfaceRevision != *passSurfaceRevision ||
            !MatrixEquals(cache.Matrix, xf.matrix);
        if (dirty)
        {
            cache.DataIndex = comp.SplineDataIndex;
            cache.Generation = comp.SplineDataGeneration;
            cache.DataVersion = data.Version;
            cache.HasEdges = wantEdges;
            cache.SurfaceRevision = *passSurfaceRevision;
            std::memcpy(cache.Matrix, xf.matrix, sizeof(cache.Matrix));
            if (!conformIgnore.has_value())
                conformIgnore = CollectConformRayIgnoreList(*world);
            BuildSplineDrapedPolylines(*world, data, worldM, *conformIgnore, wantEdges,
                                       Components::SplineConformTarget::Scene, cache.Polylines);
            DrapeKnots(*world, data, worldM, *conformIgnore, cache.KnotDrapedY);
        }
        cache.LastSeen = cacheEpoch;
        return cache;
    };

    // Spline control points are stored in local space (relative to entity
    // origin). Apply the entity's WorldTransform to render in world space,
    // consistent with TerrainModifierSystem which does the same transform.
    world->Query<ECS::Read<Components::SplineComponent>,
                 ECS::Read<Components::WorldTransform>>()
        .Each([&](ECS::EntityHandle e,
                  const Components::SplineComponent& comp,
                  const Components::WorldTransform& xf)
        {

            const bool isSplinePointSelected =
                selection.Entity == e && (selection.Kind != 0 || !selection.KnotIndices.empty());
            const bool hasSelectedControl = std::any_of(selection.Controls.begin(),
                                                        selection.Controls.end(),
                                                        [&](const SplineControlSelection& c) {
                                                            return c.Entity == e && c.Kind != 0;
                                                        });
            const bool isSelectedEntity =
                (m_IsEntitySelectedQuery ? m_IsEntitySelectedQuery(e) : selectedSceneEntity == e) ||
                isSplinePointSelected || hasSelectedControl;
            // A spline another feature owns as its shape draws no centerline (its owner draws
            // it) and shows its knots only while it is selected and its owner is not hidden.
            const SplineOwnerClaim claim = QuerySplineOwner(*world, e);
            if (claim.Claimed && (!isSelectedEntity || claim.Hidden))
                return;
            const bool showEditControls = isSelectedEntity || showAllControls;

            SplineECS::SplineHandle handle(comp.SplineDataIndex, comp.SplineDataGeneration);
            const auto* data = splineService->GetSplineData(handle);
            if (!data || !data->IsValid())
                return;

            if (!showEditControls)
            {
                if (drapeToSurface)
                {
                    Mathematics::Matrix4x4 worldM;
                    worldM = Mathematics::Matrix4x4::FromColumnMajor(xf.matrix);
                    const DrapeCache& drape =
                        refreshDrapeCache(e, comp, *data, xf, worldM, /*wantEdges=*/false);
                    const auto& center = drape.Polylines.Center;
                    if (center.size() >= 2u)
                    {
                        passiveDrapedVerts.clear();
                        passiveDrapedVerts.reserve((center.size() - 1u) * 2u);
                        for (size_t i = 1; i < center.size(); ++i)
                            AppendLine(passiveDrapedVerts, center[i - 1], center[i]);
                        context.DrawColoredLines(passiveDrapedVerts.data(),
                                                 passiveDrapedVerts.size() / 2u,
                                                 splineColor, 1.0f);
                        return;
                    }
                    // Zero-length spline: fall through to the analytic line.
                }
                auto& cache = m_PassiveLineCache[e.id];
                const bool cacheDirty =
                    cache.DataIndex != comp.SplineDataIndex ||
                    cache.Generation != comp.SplineDataGeneration ||
                    cache.DataVersion != data->Version ||
                    !MatrixEquals(cache.Matrix, xf.matrix);
                if (cacheDirty)
                {
                    cache.DataIndex = comp.SplineDataIndex;
                    cache.Generation = comp.SplineDataGeneration;
                    cache.DataVersion = data->Version;
                    std::memcpy(cache.Matrix, xf.matrix, sizeof(cache.Matrix));
                    RebuildPassiveLineCache(*data, xf, cache.Vertices);
                }
                cache.LastSeen = cacheEpoch;
                if (!cache.Vertices.empty())
                    context.DrawColoredLines(cache.Vertices.data(), cache.Vertices.size() / 2u,
                                             splineColor, 1.0f);
                return;
            }

            // Transform local-space points to world space for rendering.
            Mathematics::Matrix4x4 worldM;
            worldM = Mathematics::Matrix4x4::FromColumnMajor(xf.matrix);

            const auto& srcPts = data->Points;
            const uint32 n = static_cast<uint32>(srcPts.size());
            // Build world-space point array for rendering.
            std::vector<Spline::SplineControlPoint> pts(srcPts.size());
            for (uint32 i = 0; i < n; ++i)
            {
                pts[i] = srcPts[i];
                pts[i].Position = worldM.TransformPoint(srcPts[i].Position);
                // TangentIn/Out are direction vectors — transform via point
                // difference to apply rotation+scale without translation.
                Vector3 origin = worldM.TransformPoint(Vector3(0, 0, 0));
                pts[i].TangentIn = worldM.TransformPoint(srcPts[i].TangentIn) - origin;
                pts[i].TangentOut = worldM.TransformPoint(srcPts[i].TangentOut) - origin;
            }
            const Spline::SplineType type = data->Type;

            // Draped polylines: the centerline when draping is on, plus the
            // ±width envelope edges when the selected spline shows its band —
            // one cache entry serves both, so the drawn centerline and the
            // envelope cannot disagree. The band's edge rays are the expensive
            // two-thirds, so the setting gates the cast, not just the draw.
            const bool wantEnvelope = isSelectedEntity && showWidthEnvelope;
            const DrapeCache* drape = nullptr;
            if (!claim.Claimed && (drapeToSurface || wantEnvelope))
                drape = &refreshDrapeCache(e, comp, *data, xf, worldM,
                                           /*wantEdges=*/wantEnvelope);
            const bool drawDrapedCenterline =
                drapeToSurface && drape && drape->Polylines.Center.size() >= 2u;

            if (canBatchThickTriangles)
            {
                const uint32 segCount = data->GetSegmentCount();
                const size_t curveLines = drawDrapedCenterline
                    ? drape->Polylines.Center.size() - 1u
                    : (type == Spline::SplineType::Linear
                           ? static_cast<size_t>(segCount)
                           : static_cast<size_t>(segCount) * kSegSubdivisions);
                splineTriBatch.reserve(splineTriBatch.size() + curveLines * 6u);
                knotTriBatch.reserve(knotTriBatch.size() + static_cast<size_t>(n) * 32u * 6u);
                if (type == Spline::SplineType::CubicBezier)
                    handleTriBatch.reserve(handleTriBatch.size() + static_cast<size_t>(n) * 2u * (1u + 32u) * 6u);
            }

            // Draw the spline centerline. With draping on it follows the
            // cached draped polyline (matching the envelope and placed tiles);
            // otherwise the authored curve is subdivided analytically using
            // the spline's own stored type (not the editor's authoring
            // setting). When the spline is effectively closed, one extra
            // segment wraps from the last knot back to the first so the
            // visualization matches the evaluator's topology.
            if (claim.Claimed)
            {
                // The owner draws the outline; only the knots below draw here.
            }
            else if (drawDrapedCenterline)
            {
                const auto& center = drape->Polylines.Center;
                for (size_t i = 1; i < center.size(); ++i)
                {
                    if (canBatchThickTriangles)
                        AppendThickLineTriangles(context, center[i - 1], center[i], lineThickness,
                                                 constantScreen, lineSmart, splineTriBatch);
                    else
                        DrawThickLine(context, center[i - 1], center[i], splineColor,
                                      lineThickness, constantScreen, lineSmart);
                }
            }
            else
            {
                const uint32 segCount = data->GetSegmentCount();
                for (uint32 seg = 0; seg < segCount; ++seg)
                {
                    const uint32 i0 = seg;
                    const uint32 i1 = (seg + 1) % n;

                    if (type == Spline::SplineType::Linear)
                    {
                        if (canBatchThickTriangles)
                        {
                            AppendThickLineTriangles(context, pts[i0].Position, pts[i1].Position,
                                                     lineThickness, constantScreen, lineSmart,
                                                     splineTriBatch);
                        }
                        else
                        {
                            DrawThickLine(context, pts[i0].Position, pts[i1].Position,
                                          splineColor, lineThickness, constantScreen, lineSmart);
                        }
                        continue;
                    }

                    Vector3 p0, p1, p2, p3;
                    if (type == Spline::SplineType::CubicBezier)
                    {
                        p0 = pts[i0].Position;
                        p1 = pts[i0].Position + pts[i0].TangentOut;
                        p2 = pts[i1].Position + pts[i1].TangentIn;
                        p3 = pts[i1].Position;
                    }
                    else // CatmullRom
                    {
                        // For closed splines, neighbor indices wrap; for open splines
                        // they clamp at the ends (duplicating the endpoint tangent).
                        const uint32 iPrev = data->IsEffectivelyClosed()
                            ? ((i0 + n - 1) % n)
                            : (i0 > 0 ? i0 - 1 : 0);
                        const uint32 iNext = data->IsEffectivelyClosed()
                            ? ((i1 + 1) % n)
                            : (i1 + 1 < n ? i1 + 1 : n - 1);
                        p0 = pts[iPrev].Position;
                        p1 = pts[i0].Position;
                        p2 = pts[i1].Position;
                        p3 = pts[iNext].Position;
                    }

                    Vector3 prev = (type == Spline::SplineType::CubicBezier) ? p0 : p1;
                    for (uint32 s = 1; s <= kSegSubdivisions; ++s)
                    {
                        float32 t = static_cast<float32>(s) / static_cast<float32>(kSegSubdivisions);
                        Vector3 curr = (type == Spline::SplineType::CubicBezier)
                            ? Math::CubicBezier(p0, p1, p2, p3, t)
                            : Math::CatmullRom (p0, p1, p2, p3, t);

                        if (canBatchThickTriangles)
                        {
                            AppendThickLineTriangles(context, prev, curr, lineThickness,
                                                     constantScreen, lineSmart, splineTriBatch);
                        }
                        else
                        {
                            DrawThickLine(context, prev, curr, splineColor, lineThickness,
                                          constantScreen, lineSmart);
                        }
                        prev = curr;
                    }
                }
            }

            // Width envelope for the selected spline: ±width offset polylines
            // draped on the scene by the same conform rays tile placement
            // uses, from the same cache entry as the draped centerline; the
            // camera-facing thick-line expansion below runs per frame.
            if (wantEnvelope && drape)
            {
                const std::vector<Vector3>& envLeft = drape->Polylines.Left;
                const std::vector<Vector3>& envRight = drape->Polylines.Right;

                // Fill the band between the draped edges so the width reads as
                // a surface, not an outline: one quad (two triangles) per
                // sample pair. World-space geometry straight from the cached
                // polylines — no camera-dependent expansion, no extra rays.
                const size_t rungCount = std::min(envLeft.size(), envRight.size());
                for (size_t i = 1; i < rungCount; ++i)
                {
                    const Vector3& l0 = envLeft[i - 1];
                    const Vector3& r0 = envRight[i - 1];
                    const Vector3& l1 = envLeft[i];
                    const Vector3& r1 = envRight[i];
                    const Vector3 quad[6] = {l0, r0, r1, l0, r1, l1};
                    envelopeFillBatch.insert(envelopeFillBatch.end(), std::begin(quad), std::end(quad));
                }

                for (const auto* side : {&envLeft, &envRight})
                {
                    const std::vector<Vector3>& polyline = *side;
                    for (size_t i = 1; i < polyline.size(); ++i)
                    {
                        if (canBatchThickTriangles)
                            AppendThickLineTriangles(context, polyline[i - 1], polyline[i],
                                                     lineThickness, constantScreen, lineSmart,
                                                     envelopeTriBatch);
                        else
                            DrawThickLine(context, polyline[i - 1], polyline[i], envelopeColor,
                                          lineThickness, constantScreen, lineSmart);
                    }
                }
            }

            // Draw Bezier tangent handles: line from knot to handle endpoint
            // plus a small circle marker at the endpoint.
            if (showEditControls && type == Spline::SplineType::CubicBezier)
            {
                for (uint32 i = 0; i < n; ++i)
                {
                    const Vector3& knot = pts[i].Position;
                    const Vector3 hIn  = knot + pts[i].TangentIn;
                    const Vector3 hOut = knot + pts[i].TangentOut;

                    if (i > 0)
                    {
                        bool sel = selection.Entity == e &&
                            selection.Kind == 2 && selection.PointIndex == static_cast<int32>(i);
                        if (!sel)
                        {
                            sel = std::any_of(selection.Controls.begin(), selection.Controls.end(),
                                              [&](const SplineControlSelection& c) {
                                                  return c.Entity == e && c.Kind == 2 &&
                                                         c.PointIndex == static_cast<int32>(i);
                                              });
                        }
                        const bool hov = isSelectedEntity && hover.Entity == e &&
                            hover.Kind == 2 && hover.PointIndex == static_cast<int32>(i);
                        const Color& col = sel ? selectedColor : (hov ? hoverHandleColor : handleColor);
                        const float ht = hov ? handleThickness * 1.5f : handleThickness;
                        const float hrIn =
                            handleRadius * ComputeSplineScreenScale(context, hIn, constantScreen, smartDist);
                        if (canBatchThickTriangles)
                        {
                            auto& batch = sel ? selectedTriBatch : (hov ? hoverHandleTriBatch : handleTriBatch);
                            AppendThickLineTriangles(context, knot, hIn, ht, constantScreen, lineSmart, batch);
                            DrawSplineControlMarker(context, hIn, hrIn, col, ht, constantScreen,
                                                    lineSmart, &batch);
                        }
                        else
                        {
                            DrawThickLine(context, knot, hIn, col, ht, constantScreen, lineSmart);
                            DrawSplineControlMarker(context, hIn, hrIn, col, ht, constantScreen,
                                                    lineSmart, nullptr);
                        }
                    }
                    if (i + 1 < n)
                    {
                        bool sel = selection.Entity == e &&
                            selection.Kind == 3 && selection.PointIndex == static_cast<int32>(i);
                        if (!sel)
                        {
                            sel = std::any_of(selection.Controls.begin(), selection.Controls.end(),
                                              [&](const SplineControlSelection& c) {
                                                  return c.Entity == e && c.Kind == 3 &&
                                                         c.PointIndex == static_cast<int32>(i);
                                              });
                        }
                        const bool hov = isSelectedEntity && hover.Entity == e &&
                            hover.Kind == 3 && hover.PointIndex == static_cast<int32>(i);
                        const Color& col = sel ? selectedColor : (hov ? hoverHandleColor : handleColor);
                        const float ht = hov ? handleThickness * 1.5f : handleThickness;
                        const float hrOut =
                            handleRadius * ComputeSplineScreenScale(context, hOut, constantScreen, smartDist);
                        if (canBatchThickTriangles)
                        {
                            auto& batch = sel ? selectedTriBatch : (hov ? hoverHandleTriBatch : handleTriBatch);
                            AppendThickLineTriangles(context, knot, hOut, ht, constantScreen, lineSmart, batch);
                            DrawSplineControlMarker(context, hOut, hrOut, col, ht, constantScreen,
                                                    lineSmart, &batch);
                        }
                        else
                        {
                            DrawThickLine(context, knot, hOut, col, ht, constantScreen, lineSmart);
                            DrawSplineControlMarker(context, hOut, hrOut, col, ht, constantScreen,
                                                    lineSmart, nullptr);
                        }
                    }
                }
            }

            // Draw control point (knot) markers. A knot is highlighted when
            // either the single-pick selection points at it, or it is part of
            // the multi-selection produced by a marquee drag.
            if (showEditControls)
            {
                knotStems.clear();
                for (uint32 i = 0; i < n; ++i)
                {
                    const Vector3& pos = pts[i].Position;
                    bool sel = selection.Entity == e &&
                        selection.Kind == 1 && selection.PointIndex == static_cast<int32>(i);
                    if (!sel && selection.Entity == e)
                    {
                        for (int32 idx : selection.KnotIndices)
                        {
                            if (idx == static_cast<int32>(i)) { sel = true; break; }
                        }
                    }
                    if (!sel)
                    {
                        sel = std::any_of(selection.Controls.begin(), selection.Controls.end(),
                                          [&](const SplineControlSelection& c) {
                                              return c.Entity == e && c.Kind == 1 &&
                                                     c.PointIndex == static_cast<int32>(i);
                                          });
                    }
                    const bool hovered = isSelectedEntity && hover.Entity == e &&
                        hover.Kind == 1 && hover.PointIndex == static_cast<int32>(i);
                    const Color& col = sel ? selectedColor : (hovered ? hoverKnotColor : knotColor);
                    const float thickness = hovered ? knotThickness * 1.5f : knotThickness;
                    const float kr =
                        knotRadius * ComputeSplineScreenScale(context, pos, constantScreen, smartDist);
                    if (drawDrapedCenterline && i < drape->KnotDrapedY.size() &&
                        std::isfinite(drape->KnotDrapedY[i]) && std::abs(drape->KnotDrapedY[i] - pos.y) > kr)
                        AppendLine(knotStems, pos, Vector3(pos.x, drape->KnotDrapedY[i], pos.z));
                    if (canBatchThickTriangles)
                    {
                        auto& batch = sel ? selectedTriBatch : (hovered ? hoverKnotTriBatch : knotTriBatch);
                        DrawSplineControlMarker(context, pos, kr, col, thickness,
                                                constantScreen, lineSmart, &batch);
                    }
                    else
                    {
                        DrawSplineControlMarker(context, pos, kr, col, thickness,
                                                constantScreen, lineSmart, nullptr);
                    }
                }
                // At the handles' thickness and screen scaling, as the selected spline's other strokes.
                for (std::size_t s = 0; s + 1 < knotStems.size(); s += 2)
                    DrawThickLine(context, knotStems[s], knotStems[s + 1], knotStemColor, handleThickness,
                                  constantScreen, lineSmart);
            }
        });

    // The band fill is plain world-space geometry — no camera-dependent
    // expansion — so it flushes even when thick-line batching is unavailable.
    //
    // Always-on-top: the fill is draped onto the very surface it describes, and
    // the tiles it accompanies are base-anchored at that same surface, so a
    // depth test measures it against geometry it is coincident with by
    // construction and dims it to near-invisibility over its own tiles. The
    // curve, knots and envelope edges below stay depth-tested — that is where
    // the occlusion cue lives.
    {
        GizmoDepthModeScope depthScope(context, GizmoDepthMode::AlwaysOnTop);
        FlushTriangleBatch(context, envelopeFillBatch, envelopeFillColor);
    }
    if (canBatchThickTriangles)
    {
        FlushTriangleBatch(context, envelopeTriBatch, envelopeColor);
        FlushTriangleBatch(context, splineTriBatch, splineColor);
        FlushTriangleBatch(context, handleTriBatch, handleColor);
        FlushTriangleBatch(context, knotTriBatch, knotColor);
        FlushTriangleBatch(context, hoverHandleTriBatch, hoverHandleColor);
        FlushTriangleBatch(context, hoverKnotTriBatch, hoverKnotColor);
        FlushTriangleBatch(context, selectedTriBatch, selectedColor);
    }

    // Drop cache entries whose spline stopped being rendered in that mode
    // (deleted, disabled, or switched between passive and selected drawing);
    // both maps key on entity id and would otherwise grow for the session.
    const auto evictStale = [cacheEpoch](auto& cacheMap)
    {
        for (auto it = cacheMap.begin(); it != cacheMap.end();)
        {
            if (cacheEpoch - it->second.LastSeen > kGizmoCacheEvictEpochs)
                it = cacheMap.erase(it);
            else
                ++it;
        }
    };
    evictStale(m_PassiveLineCache);
    evictStale(m_DrapeCache);
}

} // namespace GameEngine::Editor::SceneTools
