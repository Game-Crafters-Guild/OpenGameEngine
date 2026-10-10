#include "Engine/Rendering/Pipeline/ViewMotionVectors.h"
#include "Engine/Rendering/Pipeline/DeformationMotionProducer.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/RenderServices.h"
#include "Core/Time.h"
#include "Logger/Logger.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Core/GPUInstanceWorldKey.h"
#include "Rendering/Core/NamedDescriptorWriter.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include <algorithm>
#include <cstring>

namespace GameEngine::Engine::Renderer::Pipeline
{
using namespace ::GameEngine::Rendering;
namespace
{
// GLSL std140 mirror of taa_motion_vectors.vert TaaMVParams.
struct TaaMVParamsCPU
{
    float CurrViewProj[16];
    float PrevViewProj[16];
};
static_assert(sizeof(TaaMVParamsCPU) == 128, "must match taa_motion_vectors.vert TaaMVParams");

// One recorded mover draw, fully resolved at declaration.
struct MoverDraw
{
    BufferHandle CoreVB;
    BufferHandle IndexBuffer;
    // Skinned draws only: the joint/weight streams the dual-skinned vertex
    // stage reads. Null on rigid draws.
    BufferHandle JointsVB;
    BufferHandle WeightsVB;
    GraphicsPipelineId Pipeline;
    uint32_t IndexCount = 0;
    uint32_t FirstIndex = 0;
    int32_t VertexOffset = 0;
    uint32_t InstanceSlot = 0;
    uint32_t PrevPaletteOffset = 0;
    IndexType IndexKind = IndexType::Uint16;
    bool Skinned = false;
};

} // namespace

ViewMotionVectors::MoverMotionPath ViewMotionVectors::ClassifyMoverMotionPath(
    VertexAttributeFlags vertexFlags, bool paletteActive, bool hasSkinStreams,
    bool skinnedShaderAvailable)
{
    // No live palette: the instance transform carries all of this mover's
    // motion, which is exactly right for a bind-pose skinned mesh being moved.
    if (!paletteActive)
        return MoverMotionPath::Rigid;

    // Everything below deforms this frame, so the rigid stage would raster the
    // BIND POSE against skinned prepass depth. Under reverse-Z GreaterOrEqual
    // that still passes wherever the two surfaces coincide, and the exact MV it
    // writes overrides the resolve's camera reprojection — worse than not
    // drawing at all. Skipping leaves the MV clear sentinel, so those pixels
    // reproject analytically: conservative, but never wrong about the surface.
    if (!hasSkinStreams || !skinnedShaderAvailable)
        return MoverMotionPath::Skip;

    // SKINNED_8 content is blended with eight influences by the world and
    // prepass stages; the shared skinned MV stage reads joints0/weights0 only,
    // and those weights are not normalized on their own. Its surface therefore
    // diverges from the depth it tests against — the same divergence that keeps
    // SKINNED_8 off the shared 4-influence depth VS in DepthDrawRecorder.
    if (HasFlag(vertexFlags, VertexAttributeFlags::HasJoints1) ||
        HasFlag(vertexFlags, VertexAttributeFlags::HasWeights1))
        return MoverMotionPath::Skip;

    return MoverMotionPath::Skinned;
}

RenderGraph::RGTexture ViewMotionVectors::Declare(ViewDeclare& d)
{
    if (const auto existing = d.ResolveTexture(Names::View::MotionVectors); existing.IsValid())
        return existing;
    const uint32_t w = d.RenderWidth;
    const uint32_t h = d.RenderHeight;
    if (w == 0 || h == 0 || !d.ViewDepth.IsValid())
        return {};
    const bool letterboxed = d.Services.Views().GetViewLetterbox(d.View.id).active;
    auto* device = d.Services.GetDevice();
    if (!device || (!letterboxed && !EnsureLoaded(device))) return {};
    // The independent history exists with AA off too. Advance is idempotent
    // with ViewParamsUpload, so both use the same rendered-frame sample pair.
    const ViewDeformationClock clock =
        d.Services.TemporalHistory().ResolveDeformationClock(Time::GetCumulativeSeconds());
    const ViewTemporalSample current{d.Services.Views().ResolveCameraData(d.View.id),
                                     clock.TimeSeconds,
                                     d.Services.GetScrollAnimationTimeSeconds(), clock.Origin};
    const uint64_t frameIndex = d.Frame.FrameIndex();
    // A declare path: the rotation takes the frame stream's submitted-frame
    // count so a declared-and-abandoned frame never becomes a later previous.
    bool previousValid = false;
    const auto* previous = d.Services.TemporalHistory().Advance(
        d.View.id, frameIndex, current, d.Frame.SubmittedFrameCount(), &previousValid);
    TextureDesc mvDesc{};
    mvDesc.width = w;
    mvDesc.height = h;
    mvDesc.mipLevels = 1;
    mvDesc.arrayLayers = 1;
    mvDesc.sampleCount = 1;
    mvDesc.format = static_cast<uint32_t>(TextureFormat::R16G16B16A16_FLOAT);
    mvDesc.usage = static_cast<uint32_t>(TextureUsage::RenderTarget) |
                   static_cast<uint32_t>(TextureUsage::ShaderResource);
    mvDesc.debugName = "View.MotionVectors";
    const RenderGraph::RGTexture mvTex = d.Frame.CreateTexture(d.PassName("MotionVectors").c_str(), mvDesc);

    if (!mvTex.IsValid()) return {};

    // The deforming-motion producer. It draws the surfaces the mover lane
    // cannot reproduce, and it runs FIRST because whichever producer runs owns
    // the clear: the mover writes that follow load into its image rather than
    // erasing it. With no arm enabled this records nothing and the frame below
    // is the frame it has always been.
    const bool producedDeformation = DeclareDeformationMotion(d, mvTex, previous, previousValid,
                                                              clock.Origin);

    if (letterboxed)
    {
        // Mover rasterization assumes a full viewport. The clear sentinel
        // lets temporal consumers use analytic camera reprojection instead.
        // A producer that ran already cleared and filled the target.
        if (!producedDeformation)
        {
            d.Frame.AddPass(d.PassName("MotionVectors").c_str(), Rendering::PassPhase::kDefault,
                [&](RenderGraph::RGPassBuilder& p)
                {
                    RenderGraph::RGAttachmentOps ops{};
                    ops.Load = RenderGraph::RGLoadOp::Clear;
                    ops.Store = RenderGraph::RGStoreOp::Store;
                    ops.Clear.Color[0] = kMotionVectorSentinelDelta;
                    ops.Clear.Color[1] = kMotionVectorSentinelDelta;
                    p.AttachColor(0, mvTex, ops);
                }, [](RenderGraph::RGContext&) {});
        }
        d.PublishTexture(Names::View::MotionVectors, mvTex);
        return mvTex;
    }
    // ── Movers MV pass ─────────────────────────────────────────────────────
    // Exact per-instance motion for everything whose transform changed OR whose
    // pose deformed this frame. World-key gated (multi-world editor: a Game
    // View and the Scene View share the frame). A mover whose exact motion this
    // pass cannot reproduce is dropped rather than approximated — see
    // ClassifyMoverMotionPath.
    auto* scene = d.Services.GetGPUScene();
    auto& meshReg = d.Services.GetMeshGPURegistry();
    std::vector<MoverDraw> draws;
    const auto movers = d.Services.GetFrameMovers();
    // Last frame's palette ring. Absent on the first frame and after a device
    // rebuild; the skinned draws then evaluate both endpoints at the current
    // pose (zero pose motion) rather than sampling undefined memory.
    auto& paletteAtlas = d.Services.GetSkinPaletteAtlas();
    const bool prevPaletteValid = paletteAtlas.HasPreviousFrame();
    const BufferHandle paletteBuf = paletteAtlas.GetBuffer();
    const BufferHandle prevPaletteBuf =
        prevPaletteValid ? paletteAtlas.GetPreviousBuffer() : paletteBuf;
    const size_t paletteBytes = paletteAtlas.GetCapacityBytes();
    // Rings grow independently, so the previous slot's extent is its own.
    const size_t prevPaletteBytes =
        prevPaletteValid ? paletteAtlas.GetPreviousCapacityBytes() : paletteBytes;
    uint32_t viewWorldKey = 0u;
    if (const auto* vd = d.Services.Views().FindViewDesc(d.View.id); vd && vd->worldId != 0)
        viewWorldKey = Rendering::PackWorldKey16(vd->worldId);
    if (scene != nullptr && !movers.empty())
    {
        auto* dev = d.Services.GetDevice();
        draws.reserve(movers.size());
        const auto& instances = scene->GetInstances();
        for (const auto& mover : movers)
        {
            if (mover.InstanceIndex >= instances.size())
                continue;
            if (viewWorldKey != 0u &&
                ((instances[mover.InstanceIndex].flags >> 16u) & 0xFFFFu) != viewWorldKey)
                continue;
            // The two lanes must not both write one pixel. An instance whose
            // material deforms is covered by the producer, which composes its
            // transform motion and its deformation into ONE pair of endpoints;
            // writing it here as well would put two vectors on the same pixel
            // and let record order decide which survives.
            if (producedDeformation &&
                d.Services.Materials().SupportsDeformationMotionAt(
                    instances[mover.InstanceIndex].materialIndex))
                continue;
            const MeshGPUEntry* entry = meshReg.Find(mover.MeshHandle);
            if (entry == nullptr || entry->indexCount == 0)
                continue;
            MeshGPUEntryBindings bindings{};
            if (!meshReg.TryGetDrawableBindings(*entry, bindings))
                continue;
            const auto vertexFlags = static_cast<VertexAttributeFlags>(entry->bucketKey);
            const uint32_t stride = CoreVertexStride(vertexFlags);
            if (stride == 0)
                continue;
            const MoverMotionPath path = ClassifyMoverMotionPath(
                vertexFlags, mover.SkinPaletteOffset != 0u,
                bindings.jointsVB.IsValid() && bindings.weightsVB.IsValid(),
                !m_MVSkinnedVertexSpv.empty());
            if (path == MoverMotionPath::Skip)
                continue;
            const bool skinned = path == MoverMotionPath::Skinned;
            const GraphicsPipelineId pso = GetOrCreateMVPipeline(dev, stride, skinned);
            if (!pso.IsValid())
                continue;
            MoverDraw draw{};
            draw.CoreVB = bindings.coreVB;
            draw.IndexBuffer = bindings.indexBuffer;
            draw.Pipeline = pso;
            draw.IndexCount = entry->indexCount;
            draw.FirstIndex = entry->firstIndex;
            draw.VertexOffset = static_cast<int32_t>(entry->vertexOffset);
            draw.InstanceSlot = mover.InstanceIndex;
            draw.IndexKind = static_cast<IndexType>(bindings.indexType);
            draw.Skinned = skinned;
            if (skinned)
            {
                draw.JointsVB = bindings.jointsVB;
                draw.WeightsVB = bindings.weightsVB;
                // No previous-frame atlas at all is the same statement as "this
                // instance had no pose last frame": the stage must evaluate its
                // previous endpoint at the CURRENT pose, not index a ring that
                // either does not exist or belonged to another runtime.
                draw.PrevPaletteOffset = prevPaletteValid
                                             ? mover.PrevSkinPaletteOffset
                                             : RenderServices::kNoPreviousSkinPalette;
            }
            draws.push_back(draw);
        }
    }

    // Raster-domain camera for the MV pass (bit-identical to the world/depth
    // seams: the frozen NDC offsets guarantee it) + the unjittered MV domain.
    auto camAlloc = d.Frame.AllocUpload<Rendering::CameraData>();
    auto mvParamsAlloc = d.Frame.AllocUpload<TaaMVParamsCPU>();
    if (!camAlloc.Valid() || !mvParamsAlloc.Valid())
        return {};
    *camAlloc.Ptr =
        d.Services.Views().ResolveJitteredCameraData(d.View.id, frameIndex, w, h);
    std::memcpy(mvParamsAlloc.Ptr->CurrViewProj, current.Camera.viewProj, sizeof(float) * 16);
    std::memcpy(mvParamsAlloc.Ptr->PrevViewProj,
                previous->Camera.viewProj,
                sizeof(float) * 16);

    const auto sceneRG = scene != nullptr ? scene->ImportFrameResources(d.Frame)
                                          : GPUScene::GPUSceneFrameRG{};
    const uint32_t instanceRangeBytes =
        scene != nullptr ? scene->GetInstanceCount() * static_cast<uint32_t>(sizeof(GPUInstance))
                         : 0u;

    d.Frame.AddPass(
        d.PassName("MotionVectors").c_str(), Rendering::PassPhase::kDefault,
        [&](RenderGraph::RGPassBuilder& p)
        {
            RenderGraph::RGAttachmentOps colorOps{};
            // One owner clears. When the deformer producer ran it already
            // cleared and filled its surfaces, so these writes must LOAD —
            // clearing here would erase exactly the vectors it just produced.
            colorOps.Load = producedDeformation ? RenderGraph::RGLoadOp::Load
                                                : RenderGraph::RGLoadOp::Clear;
            colorOps.Store = RenderGraph::RGStoreOp::Store;
            colorOps.Clear.Color[0] = kMotionVectorSentinelDelta;
            colorOps.Clear.Color[1] = kMotionVectorSentinelDelta;
            p.AttachColor(0, mvTex, colorOps);
            RenderGraph::RGAttachmentOps depthOps{};
            depthOps.Load = RenderGraph::RGLoadOp::Load;
            p.AttachDepth(d.ViewDepth, depthOps, RenderGraph::RGDepthAccess::ReadOnly);
            if (sceneRG.Instances.IsValid())
                p.Read(sceneRG.Instances, RenderGraph::RGBufferRead::Storage);
        },
        [this, draws = std::move(draws), sceneRG, camAlloc, mvParamsAlloc, instanceRangeBytes,
         paletteBuf, prevPaletteBuf, paletteBytes, prevPaletteBytes, w, h](RenderGraph::RGContext& ctx)
        {
            auto* dev = ctx.GetDevice();
            auto* cl = ctx.Cmd;
            if (!dev || !cl || draws.empty() || !m_MVMeta)
                return;
            const BufferHandle instanceBuf =
                sceneRG.Instances.IsValid() ? ctx.GetBuffer(sceneRG.Instances) : BufferHandle{};
            if (!instanceBuf.IsValid() || instanceRangeBytes == 0)
                return;

            cl->SetViewport(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
            cl->SetScissor(0, 0, w, h);

            // A set-0 layout is only known once that variant's PSO has been
            // interned, so each set is built ONLY when its variant actually has
            // draws this frame. Building the rigid set unconditionally would
            // hand the device an empty layout on an all-skinned frame.
            const bool anyRigid = std::any_of(draws.begin(), draws.end(),
                                              [](const MoverDraw& d) { return !d.Skinned; });
            const bool anySkinned = std::any_of(draws.begin(), draws.end(),
                                                [](const MoverDraw& d) { return d.Skinned; });

            DescriptorSetHandle ds{};
            if (anyRigid && m_MVMeta)
            {
                DescriptorSetDesc dsDesc{};
                dsDesc.layout = m_MVSet0Layout;
                dsDesc.transient = true;
                dsDesc.debugName = "MotionVectors.Set0";
                ds = dev->CreateDescriptorSet(dsDesc);
                // Reflection names UBOs and SSBOs with an instance name by that
                // instance ("Cam"/"MV"/"BonePaletteAtlas"), and a block without
                // one by its block name ("GPUInstances"). TryAdd + warn-once: a
                // silently skipped write here is an unwritten descriptor —
                // black output at best, undefined access at worst.
                NamedDescriptorWriter wd(dev, ds, *m_MVMeta, 0);
                bool ok = true;
                ok &= wd.TryAddStorageBuffer("GPUInstances", instanceBuf, 0, instanceRangeBytes);
                ok &= wd.TryAddUniformBuffer("Cam", camAlloc.Buffer, camAlloc.Offset,
                                             sizeof(Rendering::CameraData));
                ok &= wd.TryAddUniformBuffer("MV", mvParamsAlloc.Buffer, mvParamsAlloc.Offset,
                                             sizeof(TaaMVParamsCPU));
                if (!ok)
                {
                    if (!m_WarnedBindingMismatch)
                    {
                        m_WarnedBindingMismatch = true;
                        Logger::Log::Error(
                            "ViewMotionVectors: MV pass descriptor name mismatch vs "
                            "taa_motion_vectors reflection — rigid movers skipped (would draw "
                            "with unwritten descriptors)");
                    }
                    ds = {};
                }
                else
                    wd.Flush();
            }

            // Skinned draws need their own set: the dual-skinned stage reflects
            // two extra storage buffers (current + previous palette atlas), so
            // its set-0 layout differs from the rigid one.
            DescriptorSetHandle dsSkinned{};
            if (anySkinned && m_MVSkinnedMeta && paletteBuf.IsValid() && prevPaletteBuf.IsValid())
            {
                DescriptorSetDesc skinnedDesc{};
                skinnedDesc.layout = m_MVSkinnedSet0Layout;
                skinnedDesc.transient = true;
                skinnedDesc.debugName = "MotionVectors.Skinned.Set0";
                dsSkinned = dev->CreateDescriptorSet(skinnedDesc);
                NamedDescriptorWriter wd(dev, dsSkinned, *m_MVSkinnedMeta, 0);
                bool ok = true;
                ok &= wd.TryAddStorageBuffer("GPUInstances", instanceBuf, 0, instanceRangeBytes);
                ok &= wd.TryAddUniformBuffer("Cam", camAlloc.Buffer, camAlloc.Offset,
                                             sizeof(Rendering::CameraData));
                ok &= wd.TryAddUniformBuffer("MV", mvParamsAlloc.Buffer, mvParamsAlloc.Offset,
                                             sizeof(TaaMVParamsCPU));
                // Reflection names these by INSTANCE (matching how the world
                // pass binds "BonePaletteAtlas"), not by block.
                ok &= wd.TryAddStorageBuffer("BonePaletteAtlas", paletteBuf, 0, paletteBytes);
                ok &= wd.TryAddStorageBuffer("PrevBonePaletteAtlas", prevPaletteBuf, 0,
                                             prevPaletteBytes);
                if (!ok)
                {
                    if (!m_WarnedSkinnedBindingMismatch)
                    {
                        m_WarnedSkinnedBindingMismatch = true;
                        Logger::Log::Error(
                            "ViewMotionVectors: skinned MV descriptor name mismatch vs "
                            "taa_motion_vectors_skinned reflection — skinned movers skipped");
                    }
                    dsSkinned = {};
                }
                else
                    wd.Flush();
            }

            GraphicsPipelineId lastPso{};
            BufferHandle lastVB{};
            BufferHandle lastIB{};
            BufferHandle lastJointsVB{};
            BufferHandle lastWeightsVB{};
            IndexType lastIndexKind = IndexType::Uint16;
            bool first = true;
            for (const MoverDraw& draw : draws)
            {
                // A draw whose set could not be built must be dropped, not
                // demoted: its PSO expects exactly that set's bindings.
                const DescriptorSetHandle drawSet = draw.Skinned ? dsSkinned : ds;
                if (!drawSet.IsValid())
                    continue;
                if (first || !(draw.Pipeline == lastPso))
                {
                    const PipelineHandle pipe = ctx.GetOrCreatePipelineVariant(draw.Pipeline);
                    if (!pipe.IsValid())
                        continue;
                    cl->SetPipeline(pipe);
                    cl->BindDescriptorSet(0, drawSet, pipe);
                    lastPso = draw.Pipeline;
                }
                if (draw.Skinned &&
                    (first || !(draw.JointsVB == lastJointsVB) ||
                     !(draw.WeightsVB == lastWeightsVB)))
                {
                    cl->SetVertexBuffer(draw.JointsVB, Rendering::VertexBinding::Joints);
                    cl->SetVertexBuffer(draw.WeightsVB, Rendering::VertexBinding::Weights);
                    lastJointsVB = draw.JointsVB;
                    lastWeightsVB = draw.WeightsVB;
                }
                if (first || !(draw.CoreVB == lastVB))
                {
                    cl->SetVertexBuffer(draw.CoreVB, 0);
                    lastVB = draw.CoreVB;
                }
                if (first || !(draw.IndexBuffer == lastIB) || draw.IndexKind != lastIndexKind)
                {
                    cl->SetIndexBuffer(draw.IndexBuffer, draw.IndexKind);
                    lastIB = draw.IndexBuffer;
                    lastIndexKind = draw.IndexKind;
                }
                first = false;
                // Layouts are per-shader, so a skinned draw must write through
                // its own meta even though the block's members are identical.
                const ShaderMeta& pcMeta =
                    draw.Skinned && m_MVSkinnedMeta ? *m_MVSkinnedMeta : *m_MVMeta;
                if (!pcMeta.PushConstants.empty())
                {
                    NamedPushConstantWriter pcw(pcMeta, pcMeta.PushConstants[0].Name);
                    if (pcw.IsValid())
                    {
                        pcw.Add("uInstanceSlot", draw.InstanceSlot);
                        // Written for rigid draws too: the push-constant block is
                        // shared by both variants, and a member left unwritten is
                        // undefined data in the block.
                        pcw.Add("uPrevPaletteOffset", draw.PrevPaletteOffset);
                        pcw.Flush(cl);
                    }
                }
                cl->DrawIndexed(draw.IndexCount, 1, draw.FirstIndex, draw.VertexOffset, 0);
            }
        });

    d.PublishTexture(Names::View::MotionVectors, mvTex);
    return mvTex;
}

GraphicsPipelineId ViewMotionVectors::GetOrCreateMVPipeline(IDevice* device, uint32_t strideBytes,
                                                         bool skinned)
{
    if (device == nullptr || m_MVFragmentSpv.empty())
        return {};
    const std::vector<uint8_t>& vertexSpv = skinned ? m_MVSkinnedVertexSpv : m_MVVertexSpv;
    if (vertexSpv.empty())
        return {};
    const MVPipelineKey key{strideBytes, skinned};
    if (const auto it = m_MVPipelines.find(key); it != m_MVPipelines.end())
        return it->second;

    GraphicsPipelineDesc gd{};
    gd.Kind = GraphicsPipelineKind::VertexFragment;
    gd.VertexShader = std::make_shared<const std::vector<uint8_t>>(vertexSpv);
    gd.PixelShader = std::make_shared<const std::vector<uint8_t>>(m_MVFragmentSpv);
    gd.DebugName = (skinned ? "ViewMotionVectors.Skinned.Stride" : "ViewMotionVectors.Stride") +
                   std::to_string(strideBytes);
    // Cull none: the mover set is tiny and this sidesteps per-instance
    // mirrored-winding handling entirely; the read-only GreaterOrEqual depth
    // test against the prepass depth keeps only the visible surface.
    gd.Rasterization.cullMode = CullModeFlagBits::None;
    gd.DepthStencil.depthTestEnable = true;
    gd.DepthStencil.depthWriteEnable = false;
    gd.DepthStencil.depthCompareOp = CompareOp::GreaterOrEqual;
    DynamicStateInfo dyn{};
    dyn.states = {DynamicState::Viewport, DynamicState::Scissor};
    gd.DynamicState = dyn;
    // Position-only vertex input over the bucket's interleaved core stream
    // (position always leads at offset 0; stride varies per bucket). The
    // skinned variant adds the joint/weight streams at the engine-wide
    // VertexBinding/VertexLocation slots so the mesh registry's existing
    // buffers bind unchanged.
    gd.VertexBindings = {{Rendering::VertexBinding::Core, strideBytes, 0u}};
    gd.VertexAttributes = {
        {Rendering::VertexLocation::Position, Rendering::VertexBinding::Core,
         Format::R32G32B32_FLOAT, 0u}};
    if (skinned)
    {
        gd.VertexBindings.push_back(
            {Rendering::VertexBinding::Joints, sizeof(uint16_t) * 4u, 0u});
        gd.VertexBindings.push_back(
            {Rendering::VertexBinding::Weights, sizeof(float) * 4u, 0u});
        gd.VertexAttributes.push_back({Rendering::VertexLocation::Joints,
                                       Rendering::VertexBinding::Joints,
                                       Format::R16G16B16A16_UINT, 0u});
        gd.VertexAttributes.push_back({Rendering::VertexLocation::Weights,
                                       Rendering::VertexBinding::Weights,
                                       Format::R32G32B32A32_FLOAT, 0u});
    }

    if (const ShaderMeta* meta = skinned ? m_MVSkinnedMeta.get() : m_MVMeta.get(); meta != nullptr)
    {
        auto patchLayout = [this, skinned](uint32_t setIndex, DescriptorSetLayoutDesc& dsl)
        {
            if (setIndex != 0)
                return;
            if (skinned)
                m_MVSkinnedSet0Layout = dsl;
            else
                m_MVSet0Layout = dsl;
        };
        std::string err;
        MaterialHelper::ApplyShaderMetaToGraphicsDesc(*device, *meta, gd,
                                                      MaterialBuilder::MergeMode::Auto,
                                                      {true, 128}, patchLayout, &err);
    }
    const GraphicsPipelineId id = device->InternGraphicsPipeline(std::move(gd));
    m_MVPipelines[key] = id;
    return id;
}

bool ViewMotionVectors::EnsureLoaded(IDevice* device)
{
    bool mvOk = !m_MVVertexSpv.empty();
    if (!mvOk)
    {
        ShaderPackage pkg{};
        std::string loadErr;
        if (LoadShaderPkg("Shaders/taa_motion_vectors.shaderpkg", device->PreferredShaderSource(), pkg, &loadErr))
        {
            auto itVs = pkg.stageBytes.find("vs");
            auto itFs = pkg.stageBytes.find("fs");
            if (itVs != pkg.stageBytes.end() && itFs != pkg.stageBytes.end())
            {
                m_MVVertexSpv = std::move(itVs->second);
                m_MVFragmentSpv = std::move(itFs->second);
                m_MVMeta = std::make_unique<ShaderMeta>(std::move(pkg.meta));
                mvOk = true;
            }
            else
                LOG_WARNING("ViewMotionVectors: taa_motion_vectors.shaderpkg missing vs/fs");
        }
        else
            LOG_WARNING("ViewMotionVectors: failed to load taa_motion_vectors.shaderpkg: {}", loadErr);
    }

    // Skinned movers. Not part of the ShadersLoaded latch: an unstaged skinned
    // package must not stop the rigid movers pass from running — skinned movers
    // simply fall back to the analytic camera reprojection they get today.
    if (m_MVSkinnedVertexSpv.empty())
    {
        ShaderPackage skinnedPkg{};
        std::string skinnedErr;
        if (LoadShaderPkg("Shaders/taa_motion_vectors_skinned.shaderpkg", device->PreferredShaderSource(), skinnedPkg, &skinnedErr))
        {
            auto itVs = skinnedPkg.stageBytes.find("vs");
            if (itVs != skinnedPkg.stageBytes.end() && !itVs->second.empty())
            {
                m_MVSkinnedVertexSpv = std::move(itVs->second);
                m_MVSkinnedMeta = std::make_unique<ShaderMeta>(std::move(skinnedPkg.meta));
            }
            else if (!m_WarnedSkinnedMVUnstaged)
            {
                m_WarnedSkinnedMVUnstaged = true;
                LOG_WARNING("ViewMotionVectors: taa_motion_vectors_skinned.shaderpkg missing vs — "
                            "skinned movers fall back to camera reprojection");
            }
        }
        else if (!m_WarnedSkinnedMVUnstaged)
        {
            m_WarnedSkinnedMVUnstaged = true;
            LOG_WARNING("ViewMotionVectors: failed to load taa_motion_vectors_skinned.shaderpkg ({}) "
                        "— skinned movers fall back to camera reprojection",
                        skinnedErr);
        }
    }

    return mvOk;
}
} // namespace GameEngine::Engine::Renderer::Pipeline
