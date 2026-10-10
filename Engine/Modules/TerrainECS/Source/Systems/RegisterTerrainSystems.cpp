#include "TerrainECS/Systems/RegisterTerrainSystems.h"
#include "TerrainECS/Systems/TerrainExtractionSystem.h"
#include "TerrainECS/Systems/TerrainPhysicsSystem.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainUploadNode.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/RenderServices.h"
#include "ECS/Systems.h"
#include "ECS/SystemScheduling.h"

namespace GameEngine::TerrainECS
{

void AddTerrainSystemsToSchedule(ECS::SystemScheduleBuilder& b,
                                 Engine::Renderer::RenderServices* renderServices)
{
    // Modifiers read WorldTransform, so they must run after TransformHierarchy.
    // They also read SplineComponent handles and the SplineService data behind
    // them, both of which SplineExtraction creates and rebuilds — and systems
    // within one wave execute in parallel, so that ordering has to be a declared
    // edge, not a by-product of which other dependencies happen to push the two
    // apart. The edge also places a same-frame control-point edit ahead of the
    // gather that has to notice it (SplineService::GetEditEpoch).
    // They still run before terrain extraction so heightfield changes from
    // mouse-driven modifier transforms are uploaded in the same frame.
    //
    // The same rule binds CBTUpdate, for the sphere sculpt store. CBTUpdate
    // establishes the store's IDENTITY (TerrainService::ResetPlanetSculpt when the
    // active planet entity changes, EnsureSphereSculptLoaded for a planet carrying a
    // saved .tsculpt); the bake below writes its CONTENT (ConfigurePlanetSculpt +
    // BakePlanetModifierRegions). Identity has to settle first, because
    // SphereSculptLayer::Reset() drops the store's geometry as well as its content:
    // a reset landing after a bake discards that bake, and the modifier change gate
    // keys on a hash the reset does not move, so the lost bake is never re-run.
    b.Add<TerrainModifierSystem>("TerrainModifiers", ECS::SystemPhase::Extraction, 1,
                                 {"TransformHierarchy", "SplineExtraction", "CBTUpdate"});
    // TerrainExtraction reads WorldTransform (needs TransformHierarchy) and
    // camera positions from RenderServices (needs Camera). These were implicit
    // ordering dependencies under sequential execution; wave scheduling requires
    // them to be explicit.
    b.Add<TerrainExtractionSystem>("TerrainExtraction", ECS::SystemPhase::Extraction, 1,
                                   {"TerrainModifiers", "TransformHierarchy", "Camera"}, renderServices);
    b.Add<TerrainPhysicsSystem>("TerrainPhysics", ECS::SystemPhase::Extraction, 2,
                                {"PhysicsWorldBootstrap", "TerrainExtraction", "TerrainModifiers"});
}

void RegisterTerrainPipelineNodes(Engine::Renderer::Pipeline::RenderPipelineNodeRegistry& registry)
{
    // CBT is the terrain renderer (CBTRender node). This node only flushes the shared
    // heightmap/splat/normal texture uploads CBT + grass consume — the renderer-agnostic
    // upload seam that outlived the deleted CDLOD render node. It feeds the depth prepass:
    // CBT's update and the grass placement sample what it uploads, and both draw prepass heads.
    registry.Register(
        "TerrainUpload",
        []() -> std::unique_ptr<Engine::Renderer::Pipeline::IRenderPipelineNode>
        { return std::make_unique<TerrainUploadNode>(); },
        /*perView*/ true, /*resourceFields*/ {}, /*feedsDepthPrepass*/ true);
}

} // namespace GameEngine::TerrainECS
