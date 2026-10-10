#pragma once

#include "Engine/Rendering/ShadowReceiverMeasurement.h"
#include "Mathematics/Vector3.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RecomputeElision.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace GameEngine::Engine::Renderer
{
class ShadowMapRenderFeature;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline
{
struct ViewDeclare;
} // namespace GameEngine::Engine::Renderer::Pipeline

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{

// The SDSM measurement of the directional cascades' receivers
// (depth_reduce.comp and its multisampled twin depth_reduce_ms.comp): the
// visible depth range, and per view-depth bin the light-space box of the
// surfaces in it and of the view rays that reach it (ShadowReceiverMeasurement),
// read back by ShadowMapRenderFeature a few frames later. Owned by ShadowMapNode.
//
// It measures the view depth as it stands after the pipeline's last pass, not
// the depth prepass the ShadowMap node follows: the cascades are sampled by
// every surface that writes depth, and the prepass does not hold them all (the
// ocean surface writes its depth from its own node, and terrain is drawn by the
// world pass). The reduce is therefore declared through
// ViewDeclare::DeferDeclare, after every node.
class ShadowReceiverReduce
{
  public:
    ShadowReceiverReduce() = default;
    ~ShadowReceiverReduce();
    ShadowReceiverReduce(const ShadowReceiverReduce&) = delete;
    ShadowReceiverReduce& operator=(const ShadowReceiverReduce&) = delete;

    // Schedules this frame's reduce for d.View, to be declared once every node
    // has declared. `lightDirection` (unit) is this frame's cascade fit light:
    // the bins are expressed in its rotation-only basis. `maxShadowDistance`
    // bounds the binned depth range: a receiver beyond it samples no cascade.
    void DeclareForView(ViewDeclare& d, ShadowMapRenderFeature& feature,
                        const Rendering::CameraData& camera, float nearPlane, float farPlane,
                        bool orthographic, const Mathematics::Vector3& lightDirection,
                        float maxShadowDistance);

    // Uniform block of the reduce (ReceiverReduceParams, std140).
    struct ParamsGPU
    {
        float InvViewProjRel[16];
        float LightRow0[4];
        float LightRow1[4];
        float LightRow2[4];
        float CameraPosRel[4];
        float CameraForward[4];
        float BinParams[4];
        uint32_t Extent[4];
    };
    static_assert(sizeof(ParamsGPU) == 176, "ParamsGPU must match the std140 ReceiverReduceParams");

    // What one dispatch measures under and the uniform block that tells the
    // shader so, for a `width` x `height` depth seen through `camera`.
    struct Setup
    {
        ShadowReceiverMeasurement::Context Measured{};
        ParamsGPU Params{};
    };
    static Setup MakeSetup(const Rendering::CameraData& camera, float nearPlane, float farPlane,
                           bool orthographic, const Mathematics::Vector3& lightDirection,
                           float maxShadowDistance, uint32_t width, uint32_t height);

    // Workgroups that cover a `width` x `height` depth: each thread reduces a
    // 2 x 2 block of samples, one pixel in two on each axis
    // (shadow_receiver_reduce.glsl).
    static void DispatchGroups(uint32_t width, uint32_t height, uint32_t& outX, uint32_t& outY);

  private:

    // One view's reduce, captured at node time and declared late.
    struct Request
    {
        Rendering::ViewId ViewId = 0;
        Rendering::RenderGraph::RGTexture Depth{};
        uint32_t Width = 0;
        uint32_t Height = 0;
        bool Multisampled = false;
        std::string PassName;
        Setup Dispatch{};
    };

    struct ReducePipeline
    {
        Rendering::ComputePipelineId Id{};
        Rendering::DescriptorSetLayoutDesc Set0Layout{};
        std::unique_ptr<Rendering::ShaderMeta> Meta;
    };

    // Idle recompute elision (family SDSM). Over bit-identical depth the reduce
    // reproduces bit-identical results, and the feature keeps the latest
    // measurement when no new readback lands, so at rest the pass is skipped.
    struct ElisionGate
    {
        Rendering::RecomputeElisionGate Gate;
        bool LogState = false;
    };

    void EnsureResources(Rendering::IDevice* device);
    static void LoadPipeline(Rendering::IDevice* device, const char* package, const char* debugName,
                             ReducePipeline& out);
    bool IsElided(ViewDeclare& d, const ShadowMapRenderFeature& feature,
                  const Rendering::CameraData& camera, uint32_t width, uint32_t height);
    void DeclareLate(Rendering::RenderGraph::RGFrame& frame, ShadowMapRenderFeature& feature,
                     const Request& request) const;
    void Record(Rendering::RenderGraph::RGContext& ctx, ShadowMapRenderFeature& feature,
                const Request& request, Rendering::BufferHandle slot,
                Rendering::BufferHandle params, uint64_t paramsOffset) const;

    Rendering::IDevice* m_Device = nullptr;
    ReducePipeline m_SingleSample;
    ReducePipeline m_Multisampled;
    Rendering::SamplerHandle m_Sampler;
    bool m_LoadAttempted = false;
    std::unordered_map<uint32_t, ElisionGate> m_Elision; // keyed by ViewId
};

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
