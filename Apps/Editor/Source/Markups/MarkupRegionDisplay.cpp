#include "Markups/MarkupRegionDisplay.h"

#include "Components/Markup/Markup.h"
#include "Components/Rendering/Ocean.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplinePlacement.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Transform.h"
#include "Core/Application.h"
#include "Core/CpuProfiler.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "MarkupECS/MarkupRegionArea.h"
#include "MarkupECS/MarkupRegionBoolean.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Ray.h"
#include "Ocean/OceanSeaLevel.h"
#include "Markups/MarkupEditorBridge.h"
#include "Picking/MeshPickingService.h"
#include "Placement/CenterlineSampling.h"
#include "Placement/SplineSurfaceConform.h"
#include "Placement/TileLayout.h"
#include "SceneView/SplineDrapePolylines.h"
#include "Spline/SplineData.h"
#include "Spline/SplineEvaluator.h"
#include "SplineECS/SplineService.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace GameEngine::Editor
{

using Mathematics::Vector2;
using Mathematics::Vector3;

namespace
{

// A region's ground rays: from this high, above any terrain, this far down. The terrain march
// starts where a ray enters the terrain's bounds, so the height costs nothing.
constexpr float32 kTerrainProbeTop = 20000.0f;
constexpr float32 kTerrainProbeReach = 40000.0f;

bool WorldHasTerrain(ECS::World& world)
{
    bool hasTerrain = false;
    world.Query<ECS::Read<Components::Terrain>>().Each(
        [&hasTerrain](ECS::EntityHandle, const Components::Terrain&) { hasTerrain = true; });
    return hasTerrain;
}

// The terrain's height under world (x, z), the terrain only: nullopt off every terrain.
std::optional<float32> ProbeTerrainHeight(ECS::World& world, const Vector2& xz)
{
    Mathematics::Ray3D ray;
    ray.origin = Vector3(xz.x, kTerrainProbeTop, xz.y);
    ray.direction = Vector3(0.0f, -1.0f, 0.0f);
    Picking::PickOptions options;
    options.IncludeMeshes = false;
    options.IncludePrimitives = false;
    options.IncludeBoundsFallback = false;
    options.MaxDistance = kTerrainProbeReach;
    const Picking::PickResult result = Picking::RaycastScene(ray, world, options);
    if (!result.Hit)
        return std::nullopt;
    return result.Best.WorldPosition.y;
}

// The outline's ground samples: the spline sampled as the drape samples it (kDrapeSampleStepMetres
// along its world length, at most kDrapeMaxSamples), each standing on the terrain under it
// (ProbeTerrainHeight) and lifted `surfaceLift`. The drape's own conform ray starts a
// fixed height above the outline, whose knots stand at the region's label point, so a hill taller
// than that inside the region would hold the ray's start. A miss holds the nearest measured height
// (HoldSurfaceAcrossGaps); an outline with none keeps its placed height.
std::vector<Vector3> SampleRegionOutlineGround(ECS::World& world, const Spline::SplineData& data,
                                               const Mathematics::Matrix4x4& worldMatrix, bool hasTerrain,
                                               float32 surfaceLift)
{
    std::vector<Vector3> outline;
    const float32 worldLength = WorldCenterlineLength(data.TotalArcLength, worldMatrix);
    if (data.TotalArcLength <= 0.0f || !std::isfinite(worldLength))
        return outline;
    const float32 cappedLength =
        std::min(worldLength, static_cast<float32>(SceneTools::kDrapeMaxSamples) * SceneTools::kDrapeSampleStepMetres);
    const uint32 sampleCount = std::clamp<uint32>(
        static_cast<uint32>(cappedLength / SceneTools::kDrapeSampleStepMetres) + 1u, 2u, SceneTools::kDrapeMaxSamples);
    std::vector<Spline::SplineFrame> frames;
    Spline::SampleUniform(data, sampleCount, frames);
    std::vector<CenterSample> samples;
    samples.reserve(frames.size());
    bool anyMeasured = false;
    for (const Spline::SplineFrame& frame : frames)
    {
        CenterSample& sample = samples.emplace_back();
        sample.Pos = worldMatrix.TransformPoint(frame.Position);
        sample.Normal = Vector3(0.0f, 1.0f, 0.0f);
        const std::optional<float32> ground =
            hasTerrain ? ProbeTerrainHeight(world, Vector2(sample.Pos.x, sample.Pos.z)) : std::nullopt;
        sample.SurfaceValid = ground.has_value();
        if (ground)
            sample.Pos.y = *ground;
        anyMeasured = anyMeasured || sample.SurfaceValid;
    }
    HoldSurfaceAcrossGaps(samples);
    outline.reserve(samples.size());
    for (const CenterSample& sample : samples)
        outline.push_back(anyMeasured ? sample.Pos + Vector3(0.0f, surfaceLift, 0.0f) : sample.Pos);
    return outline;
}

// The footprints of the members that cut (Exclude) or extend (Include) `region`'s area; none for a
// path, and none for a member skipped (deleted, or no longer a mark-up with a shape).
struct MemberFootprints
{
    std::vector<MarkupECS::MarkupFootprint> Includes;
    std::vector<MarkupECS::MarkupFootprint> Excludes;
};

MemberFootprints ReadMemberFootprints(const ECS::World& world, ECS::EntityHandle region)
{
    MemberFootprints footprints;
    const std::optional<MarkupECS::MarkupRegionArea> area = MarkupECS::MarkupRegionArea::Read(world, region);
    if (!area)
        return footprints;
    for (const MarkupECS::MarkupRegionArea::Member& member : area->GetMembers())
    {
        if (member.Footprint)
            (member.Mode == Components::MarkupMemberMode::Exclude ? footprints.Excludes : footprints.Includes)
                .push_back(*member.Footprint);
    }
    return footprints;
}

// The ground of one region: its area's ground samples (the outline's, cut and extended by its
// members) and the lid's sampler, the terrain's height under each point lifted `surfaceLift` and
// raised to the sea level.
MarkupECS::MarkupRegionGround BuildRegionGround(ECS::World& world, ECS::EntityHandle region,
                                                const Spline::SplineData& data,
                                                const Mathematics::Matrix4x4& worldMatrix,
                                                const MarkupRegionFrameGround& frameGround, float32 surfaceLift)
{
    GE_CPU_PROFILE_SCOPE("Markups.RegionGround");
    const bool hasTerrain = WorldHasTerrain(world);
    std::vector<Vector3> outline = SampleRegionOutlineGround(world, data, worldMatrix, hasTerrain, surfaceLift);
    float32 mean = 0.0f;
    for (Vector3& point : outline)
    {
        point.y = std::max(point.y, frameGround.SeaLevel);
        mean += point.y;
    }
    mean = outline.empty() ? 0.0f : mean / static_cast<float32>(outline.size());
    // A lid point off the terrain takes the outline's mean height.
    const MarkupECS::MarkupGroundSampler groundAt = [&world, &frameGround, hasTerrain, mean,
                                                     surfaceLift](const Vector2& xz) {
        const std::optional<float32> ground = hasTerrain ? ProbeTerrainHeight(world, xz) : std::nullopt;
        const float32 height = ground ? *ground + surfaceLift : mean;
        return std::max(height, frameGround.SeaLevel);
    };
    const MemberFootprints members = ReadMemberFootprints(world, region);
    if (members.Includes.empty() && members.Excludes.empty())
        return MarkupECS::BuildMarkupRegionGround(outline, data.IsEffectivelyClosed(), groundAt);
    std::vector<Vector2> base;
    base.reserve(outline.size());
    for (const Vector3& point : outline)
        base.emplace_back(point.x, point.z);
    const std::vector<MarkupECS::MarkupAreaPiece> area =
        MarkupECS::CombineRegionArea(base, members.Includes, members.Excludes);
    return MarkupECS::BuildMarkupRegionAreaGround(outline, area, SceneTools::kDrapeSampleStepMetres, groundAt);
}

} // namespace

float32 ResolveMarkupSeaLevel(ECS::World& world)
{
    bool found = false;
    float32 seaLevel = Components::kNoTerrainSeaLevel;
    world
        .Query<ECS::Read<Components::OceanSurface>, ECS::Optional<Components::Transform>,
               ECS::Optional<Components::WorldTransform>>()
        .Each([&found, &seaLevel](ECS::EntityHandle, const Components::OceanSurface& ocean,
                                  const Components::Transform* transform,
                                  const Components::WorldTransform* worldTransform) {
            if (found)
                return;
            found = true;
            seaLevel = Ocean::ResolveOceanSeaLevel(ocean, transform, worldTransform);
        });
    if (found)
        return seaLevel;
    world.Query<ECS::Read<Components::Terrain>>().Each([&seaLevel](ECS::EntityHandle, const Components::Terrain& terrain) {
        seaLevel = std::max(seaLevel, terrain.SeaLevel);
    });
    return seaLevel;
}

MarkupRegionFrameGround ReadMarkupRegionFrameGround(ECS::World& world)
{
    MarkupRegionFrameGround frameGround;
    frameGround.SeaLevel = ResolveMarkupSeaLevel(world);
    // The regions stand on the terrain by raycast, which reads the resident tiles only
    // (ConformSurfaceRevision).
    frameGround.GroundRevision = ConformSurfaceRevision(world, nullptr);
    return frameGround;
}

const MarkupECS::MarkupRegionDisplayCache::Entry* ResolveMarkupRegionDisplay(
    ECS::World& world, MarkupECS::MarkupRegionDisplayCache& cache, ECS::EntityHandle region,
    const MarkupRegionFrameGround& frameGround, uint64 frame)
{
    const auto* component = world.GetComponent<Components::MarkupRegion>(region);
    const auto* spline = world.GetComponent<Components::SplineComponent>(region);
    const auto* placed = world.GetComponent<Components::WorldTransform>(region);
    SplineECS::SplineService* splines = SplineECS::SplineService::TryGet();
    if (!world.GetComponent<Components::Markup>(region) || !spline || !placed || !splines)
        return nullptr;
    const Spline::SplineData* data =
        splines->GetSplineData(SplineECS::SplineHandle(spline->SplineDataIndex, spline->SplineDataGeneration));
    if (!data || !data->IsValid())
        return nullptr;

    MarkupECS::MarkupRegionGroundKey key;
    key.SplineIndex = spline->SplineDataIndex;
    key.SplineGeneration = spline->SplineDataGeneration;
    key.SplineVersion = data->Version;
    std::memcpy(key.Matrix, placed->matrix, sizeof(key.Matrix));
    key.SeaLevel = frameGround.SeaLevel;
    key.GroundRevision = frameGround.GroundRevision;
    if (component)
    {
        key.MemberCount = std::min(component->MemberCount, Components::kMaxRegionMembers);
        std::copy_n(component->Members, key.MemberCount, key.Members);
    }
    const Mathematics::Matrix4x4 worldMatrix = Mathematics::Matrix4x4::FromColumnMajor(placed->matrix);
    // A region's walls and lid stand the drape's lift above the ground; a path's ground is the ground
    // itself, its band lifting by the camera's distance as it draws (MarkupGizmo), and it stands no
    // height above it.
    const float32 surfaceLift = component ? SceneTools::kDrapeSurfaceLiftMetres : 0.0f;
    const MarkupECS::MarkupRegionDisplayCache::MemberKeyReader readMember = [&world](ECS::EntityHandle member) {
        return MarkupECS::ReadMarkupFootprintKey(world, member);
    };
    return &cache.Resolve(region, key, component ? component->ExtrudeHeight : 0.0f, frame, readMember, [&]() {
        return BuildRegionGround(world, region, *data, worldMatrix, frameGround, surfaceLift);
    });
}

const MarkupECS::MarkupRegionDisplayCache::Entry* ResolveMarkupDisplayNow(ECS::World& world,
                                                                         const MarkupEditorBridge& bridge,
                                                                         ECS::EntityHandle entity)
{
    const Application* application = Application::Get();
    const uint64 frame = application ? application->GetFrameCount() : 0;
    const MarkupECS::MarkupRegionDisplayCache::Entry* display = ResolveMarkupRegionDisplay(
        world, bridge.RegionDisplaysOf(world), entity, bridge.RegionFrameGroundOf(world, frame), frame);
    return display && !display->Ground.Outline.empty() ? display : nullptr;
}

std::optional<float32> MarkupTerrainHeightAt(ECS::World& world, const Vector2& xz)
{
    if (!WorldHasTerrain(world))
        return std::nullopt;
    return ProbeTerrainHeight(world, xz);
}

} // namespace GameEngine::Editor
