// RenderServicesDrawStreams.cpp
// Part of the RenderServices implementation — split by concern from the
// former single RenderServices.cpp. All files define members of the same
// RenderServices class; shared file-scope helpers live in RenderServicesDetail.h.
#include "Engine/Rendering/RenderServices.h"
#include "Core/CpuProfiler.h"
#include "Engine/Rendering/IRenderFeature.h"

#include "Core/DebugMetrics.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/MaterialCompiler.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialParamsLayout.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Assets/AssetManager.h"
#include "Assets/BinaryAsset.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Assets/TextureAsset.h"
#include "Engine/Rendering/EmbeddedImageDecoder.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Engine/Rendering/RetargetRenderFeature.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Logger/Logger.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/BindlessResourceManager.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/CameraDerivation.h"
#include "Rendering/Core/CullingStrategy.h"
#include "Rendering/Core/FrustumCullingStrategy.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Utils/BufferHelpers.h"
#include "Rendering/Utils/TextureUploadHelpers.h"
#include "Rendering/Utils/CubeLutFileParser.h"
#include "Rendering/Utils/CubeLutGpuUpload.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Types/StringId.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Common/MatrixUtils.h"

#include "Rendering/Core/ThreadingUtils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "RenderServicesDetail.h"

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

ScopedSubscription RenderServices::RegisterForwardEmit(ForwardEmitFn fn, bool writesDepth)
{
    if (!fn)
        return {};
    const uint64_t handle = m_DrawProducers->NextHandle++;
    m_DrawProducers->Forward.push_back({handle, std::move(fn), writesDepth});
    DrawProducerBlock* block = m_DrawProducers.get();
    return ScopedSubscription(m_DrawProducers, [block, handle]() {
        auto& fwd = block->Forward;
        for (auto it = fwd.begin(); it != fwd.end(); ++it)
        {
            if (it->Handle == handle)
            {
                fwd.erase(it);
                return;
            }
        }
    });
}

ScopedSubscription RenderServices::RegisterDepthEmit(DepthEmitFn fn)
{
    if (!fn)
        return {};
    const uint64_t handle = m_DrawProducers->NextHandle++;
    m_DrawProducers->Depth.push_back({handle, std::move(fn)});
    DrawProducerBlock* block = m_DrawProducers.get();
    return ScopedSubscription(m_DrawProducers, [block, handle]() {
        auto& dep = block->Depth;
        for (auto it = dep.begin(); it != dep.end(); ++it)
        {
            if (it->Handle == handle)
            {
                dep.erase(it);
                return;
            }
        }
    });
}

void RenderServices::EmitForwardCommand(Rendering::ViewId viewId, const DrawCommand& cmd,
                                        ForwardDrawDepth depth, const DrawCommand* prepassHead)
{
    // A Prepass or PrepassNonOccluding draw carries its own head, so the prepass streams hold the
    // depth of every draw that says they do; any other draw carries none.
    const bool wantsHead =
        depth == ForwardDrawDepth::Prepass || depth == ForwardDrawDepth::PrepassNonOccluding;
    assert(wantsHead == (prepassHead != nullptr));
    auto& pv = m_ViewRegistry.PerView(viewId);
    if (wantsHead && prepassHead == nullptr)
    {
        // Release builds: the world pass would attach the depth read-only over a surface nothing wrote,
        // so the draw writes its own.
        Logger::Log::Error("EmitForwardCommand(view {}): a ForwardDrawDepth::Prepass draw without its prepass "
                           "head; the draw writes its own depth. Pass the head the producer built",
                           viewId);
        depth = ForwardDrawDepth::ColourPass;
    }
    if (prepassHead != nullptr && depth == ForwardDrawDepth::Prepass)
        pv.DepthCommands[static_cast<size_t>(DepthPassType::Prepass)].push_back(*prepassHead);
    else if (prepassHead != nullptr && depth == ForwardDrawDepth::PrepassNonOccluding)
        pv.NonOccludingPrepassHeads.push_back(*prepassHead);
    pv.ForwardCommands.push_back(cmd);
    if (depth == ForwardDrawDepth::ColourPass)
        ++pv.ForwardDrawsWritingDepth;
}

void RenderServices::EmitLateForwardCommand(Rendering::ViewId viewId, const DrawCommand& cmd)
{
    m_ViewRegistry.PerView(viewId).LateForwardCommands.push_back(cmd);
}

void RenderServices::EmitForwardSampledRead(Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId,
                                            Rendering::RenderGraph::RGTexture texture)
{
    if (!texture.IsValid())
        return;
    // The caller names the frame the texture was imported into: its ids die with that frame, and a
    // stamp borrowed from the app-frame spine would be stale on a frame the spine's stages never
    // touched (no skinned meshes, no culling stage), silently dropping the world pass's read.
    auto& pv = m_ViewRegistry.PerView(viewId);
    pv.ForwardSampledFor.Stamp(frame);
    pv.ForwardSampledRG.push_back(texture);
}

void RenderServices::EmitForwardSampledBufferRead(Rendering::RenderGraph::RGFrame& frame,
                                                  Rendering::ViewId viewId,
                                                  Rendering::RenderGraph::RGBuffer buffer,
                                                  Rendering::RenderGraph::RGBufferRead access,
                                                  ForwardBufferReaders readers)
{
    if (!buffer.IsValid())
        return;
    // The prepass declares its reads when it is declared; one declared already cannot take this one,
    // and would draw the heads before the compute that writes their buffers.
    if (readers == ForwardBufferReaders::WorldPassAndPrepass)
    {
        if (const ViewFrameRG* vfr = FindViewFrameRGFor(frame, viewId); vfr && vfr->DepthPrepassDepth.IsValid())
            Logger::Log::Error(
                "[RenderGraph] EmitForwardSampledBufferRead(view {}): the depth prepass already declared, so "
                "it cannot read this producer's buffers; declare the producer's node BEFORE DepthPrepass",
                viewId);
    }
    Rendering::RenderGraph::RGFrameStamp importFrame;
    importFrame.Stamp(frame);
    m_ViewRegistry.PerView(viewId).ForwardSampledBufferRG.push_back({buffer, access, importFrame, readers});
}

std::span<const DrawCommand> RenderServices::GetForwardCommands(Rendering::ViewId viewId) const
{
    const auto* pv = m_ViewRegistry.FindPerView(viewId);
    if (!pv)
        return {};
    return {pv->ForwardCommands.data(), pv->ForwardCommands.size()};
}

bool RenderServices::HasForwardCommands(Rendering::ViewId viewId) const
{
    const auto* pv = m_ViewRegistry.FindPerView(viewId);
    return pv && !pv->ForwardCommands.empty();
}

std::span<const DrawCommand> RenderServices::GetLateForwardCommands(Rendering::ViewId viewId) const
{
    const auto* pv = m_ViewRegistry.FindPerView(viewId);
    if (!pv)
        return {};
    return {pv->LateForwardCommands.data(), pv->LateForwardCommands.size()};
}

bool RenderServices::HasLateForwardCommands(Rendering::ViewId viewId) const
{
    const auto* pv = m_ViewRegistry.FindPerView(viewId);
    return pv && !pv->LateForwardCommands.empty();
}

void RenderServices::ClearForwardCommands()
{
    m_ViewRegistry.ForEachPerView(
        [](ViewRegistry::PerViewResources& pv)
        {
            pv.ForwardCommands.clear();
            pv.LateForwardCommands.clear();
            pv.ForwardSampledRG.clear();
            pv.ForwardSampledBufferRG.clear();
            pv.ForwardSampledFor = {};
            pv.ForwardDrawsWritingDepth = 0;
        });
}

void RenderServices::EmitDepthCommand(Rendering::ViewId viewId,
                                       DepthPassType passType,
                                       const DrawCommand& cmd)
{
    m_ViewRegistry.PerView(viewId).DepthCommands[static_cast<size_t>(passType)].push_back(cmd);
}

std::span<const DrawCommand> RenderServices::GetDepthCommands(
    Rendering::ViewId viewId, DepthPassType passType) const
{
    const auto* pv = m_ViewRegistry.FindPerView(viewId);
    if (!pv)
        return {};
    const auto& cmds = pv->DepthCommands[static_cast<size_t>(passType)];
    return {cmds.data(), cmds.size()};
}

bool RenderServices::HasDepthCommands(Rendering::ViewId viewId,
                                       DepthPassType passType) const
{
    const auto* pv = m_ViewRegistry.FindPerView(viewId);
    return pv && !pv->DepthCommands[static_cast<size_t>(passType)].empty();
}

void RenderServices::ClearDepthCommands()
{
    m_ViewRegistry.ForEachPerView(
        [](ViewRegistry::PerViewResources& pv)
        {
            for (auto& cmds : pv.DepthCommands)
                cmds.clear();
            pv.NonOccludingPrepassHeads.clear();
        });
}

void RenderServices::EmitProducerForwardCommands()
{
    for (const auto& view : m_ViewRegistry.GetViews())
    {
        // Mask-0 views render no world content (disarmed probe capture views,
        // parked utility views); special views that do want producer commands
        // (thumbnails, probe bakes) drive their own per-view emit explicitly.
        if (view.ActiveRenderLayerMask() == 0u)
            continue;
        EmitProducerForwardCommandsForView(view.id);
    }
}

void RenderServices::EmitProducerForwardCommandsForView(Rendering::ViewId viewId,
                                                        ForwardEmitPurpose purpose)
{
    auto& pv = m_ViewRegistry.PerView(viewId);
    pv.ForwardCommands.clear();
    pv.LateForwardCommands.clear();
    pv.ForwardSampledRG.clear();
    pv.ForwardSampledBufferRG.clear();
    pv.ForwardSampledFor = {};
    pv.ForwardDrawsWritingDepth = 0;
    // The prepass heads ride with the forward draws they belong to.
    pv.DepthCommands[static_cast<size_t>(DepthPassType::Prepass)].clear();
    pv.NonOccludingPrepassHeads.clear();
    pv.NonOccludingPrepassDeclared = false;

    if (m_DrawProducers->Forward.empty())
        return;

    ForwardEmitContext ctx{};
    ctx.Device = m_Device;
    ctx.Services = this;
    // Emits run during declaration, so the frame stream tracked by m_FrameRG is
    // the one the emitted draws execute in — producers sub-allocate per-frame
    // binding data from its upload ring.
    ctx.Frame = m_FrameRG.For.Frame;
    ctx.FrameIndex = m_Device ? m_Device->GetFrameIndex() : 0;
    ctx.ViewId = viewId;
    ctx.Purpose = purpose;

    for (const auto& entry : m_DrawProducers->Forward)
        entry.Fn(ctx);
}

void RenderServices::EmitProducerDepthCommands()
{
    for (const auto& view : m_ViewRegistry.GetViews())
        EmitProducerDepthCommandsForView(view.id);
}

void RenderServices::EmitProducerDepthCommandsForView(Rendering::ViewId viewId)
{
    // The light-space passes only: the camera prepass draws the heads the forward producers emit
    // beside their colour draws, and this runs again after the prepass declared (ShadowMapNode).
    constexpr DepthPassType kPassTypes[] = {
        DepthPassType::ShadowCascade,
        DepthPassType::AreaShadow,
        DepthPassType::SpotShadow,
        DepthPassType::PointShadow,
    };

    for (DepthPassType passType : kPassTypes)
    {
        if (auto* pv = m_ViewRegistry.FindPerView(viewId))
            pv->DepthCommands[static_cast<size_t>(passType)].clear();
    }

    if (m_DrawProducers->Depth.empty())
        return;

    DepthEmitContext ctx{};
    ctx.Device = m_Device;
    ctx.Services = this;
    ctx.FrameIndex = m_Device ? m_Device->GetFrameIndex() : 0;
    ctx.ViewId = viewId;

    for (const auto& entry : m_DrawProducers->Depth)
    {
        for (DepthPassType passType : kPassTypes)
            entry.Fn(ctx, passType);
    }
}

} // namespace Engine::Renderer
} // namespace GameEngine
