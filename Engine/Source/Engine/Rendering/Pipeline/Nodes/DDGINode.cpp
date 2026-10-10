#include "Engine/Rendering/Pipeline/Nodes/DDGINode.h"

#include "Engine/Rendering/DDGIProbeFeature.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SceneAccelerationStructureService.h"

#include "Rendering/Core/Device.h"

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
using namespace ::GameEngine::Rendering;

bool DDGINode::Initialize(std::string nodeId, std::string /*nodeJson*/, std::string* /*outError*/)
{
    m_Id = std::move(nodeId);
    return true;
}

void DDGINode::Declare(RenderPipelineInstance& /*instance*/, const PipelineDeclareContext& ctx)
{
    GameEngine::Rendering::IDevice* device = ctx.Services.GetDevice();
    if (!device)
        return;

    // Shared BLAS pool + TLAS backend — gated on supportsRayQuery only, not
    // on any consumer's activation, so this succeeds independently of
    // whether RT shadows have ever been requested (see
    // RenderServices::EnsureSceneAccelerationStructureService's doc). Null
    // on a non-ray-query device: M7 gives DDGI a software fallback lane, so
    // Initialize is still called (with a null sceneAS) rather than declining
    // outright — DeclareProbePasses picks hardware-vs-software per tick from
    // the device's own capability, not from whether this pointer is valid.
    Engine::Renderer::SceneAccelerationStructureService* sceneAS =
        ctx.Services.EnsureSceneAccelerationStructureService();

    auto& feature = ctx.Services.EnsureFeature<Engine::Renderer::DDGIProbeFeature>();
    if (!feature.IsInitialized())
        feature.Initialize(device, sceneAS, ctx.Services.GetGPUScene(), &ctx.Services.GetMeshGPURegistry(),
                           &ctx.Services.Materials());
    if (!feature.IsInitialized())
        return;

    // The frame spine ran the shared pool's BeginFrame for this app frame.
    // Its result feeds DeclareProbePasses' epoch/hash TLAS-rebuild gate: a
    // mesh's BLAS becoming ready this frame forces a TLAS refresh even without
    // a GPUScene content-epoch bump. No shared pool at all on a non-ray-query
    // device (sceneAS null) — nothing became ready because nothing was ever
    // pending.
    const bool anyBlasBecameReady = sceneAS ? sceneAS->BlasBecameReadyThisFrame() : false;

    feature.DeclareProbePasses(ctx.Frame, ctx.Services, ctx.DeltaTimeSeconds, anyBlasBecameReady);
}

}  // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
