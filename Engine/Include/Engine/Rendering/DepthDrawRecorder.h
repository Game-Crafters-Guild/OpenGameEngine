// DepthDrawRecorder.h
// The shared depth-draw executor, extracted out of RenderServices as a free
// function over an injected dependency bundle (design s3-depth-executor-2026-07).
// Records the depth/shadow draws for one already-declared pass into the passed-in
// RGContext. Both the RenderServices world depth prepass and every
// ShadowMapRenderFeature shadow family call this from their pass exec lambda.
// The header deliberately does NOT include RenderServices.h — the recorder has no
// RenderServices dependency; the caller hands in the resolved subsystem
// references through DepthDrawServices.
#pragma once

#include "Engine/Rendering/DepthPassTypes.h" // DepthOnlyPassParamsRG (+ RGContext, GPUDrawStreamBuilder, IDevice, CullModeFlags)
#include "Engine/Rendering/DrawCommand.h"
#include "Rendering/Materials/MaterialAlphaMode.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/Materials/ShaderProfileDefines.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_set>

namespace GameEngine
{
namespace Rendering
{
class GPUScene;
class MeshGPURegistry;
} // namespace Rendering

namespace Engine::Renderer
{
class CpuDrawStreamBuilder;
class WorldDrawBuilder;
class MaterialSystem;
class ViewRegistry;

// Exec→declare under-draw feedback (cascade static-shadow cache correctness).
// RecordDepthOnlyPass has silent skip paths that drop draws the always-render
// pipeline would simply re-record next frame: the async depth-variant publish
// gate (depth-pipeline-invalid / material-bind-invalid), the M2a shadow-walk
// early-out on a frames-stale survivor readback (the pass already CLEARED the
// layer), and the async-content gates (material-null / mesh-entry-missing /
// mesh-not-drawable / drawstream-pipeline-invalid). Under
// GE_SHADOW_STATIC_CACHE such a frame must never become a settled cache
// baseline — the recorder Marks (view, passType, sliceIndex) whenever one of
// those paths fires, and the cascade declare sites Consume the mark next
// declare as a dirty cause, restoring always-render's one-frame self-heal
// exactly. exec(N) strictly precedes declare(N+1) (fork-join joins before
// RGFrame::Execute returns), so Consume never races a Mark for the same frame;
// the mutex covers the parallel-record workers within one exec. Marks are
// exceptional-path only — the lock is cold.
class DepthUnderDrawTracker
{
  public:
    void Mark(uint32_t viewId, uint8_t passType, uint32_t sliceIndex)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Marks.insert(Key(viewId, passType, sliceIndex));
    }
    bool Consume(uint32_t viewId, uint8_t passType, uint32_t sliceIndex)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Marks.erase(Key(viewId, passType, sliceIndex)) != 0;
    }
    // Non-consuming probe for the motion round-robin plan phase, which needs
    // every slot's would-be dirty cause BEFORE the declare sites Consume.
    bool Peek(uint32_t viewId, uint8_t passType, uint32_t sliceIndex) const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Marks.count(Key(viewId, passType, sliceIndex)) != 0;
    }
    void Reset()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Marks.clear();
    }

  private:
    static uint64_t Key(uint32_t viewId, uint8_t passType, uint32_t sliceIndex)
    {
        return (static_cast<uint64_t>(viewId) << 32) |
               (static_cast<uint64_t>(passType) << 24) | (sliceIndex & 0xFFFFFFu);
    }
    mutable std::mutex m_Mutex;
    std::unordered_set<uint64_t> m_Marks;
};

// The complete dependency surface of the depth-draw executor, resolved as
// references/pointers into RenderServices-owned subsystems. Built fresh at EXEC
// time by RenderServices::MakeDepthDrawServices() and consumed synchronously
// within a single RecordDepthOnlyPass call; never stored across frames. CullMode
// is the set-once debug rasterizer override the executor formerly reached for as
// m_CullMode — carried by value so no getter joins the RenderServices surface.
struct DepthDrawServices
{
    WorldDrawBuilder&                DrawBuilder;
    ViewRegistry&                    Views;
    Rendering::GPUScene*             Scene;
    Rendering::MeshGPURegistry&      Meshes;
    Rendering::GPUDrawStreamBuilder* Streams;
    MaterialSystem&                  Materials;
    Rendering::IDevice*              Device;
    Rendering::CullModeFlags         CullMode;
    // The view's base winding (the set-once debug rasterizer override). Even
    // segments draw with this; mirrored (parity-1) segments draw flipped.
    Rendering::FrontFace             FrontFace;
    // Draw consolidation: meshIndex -> pool-group id (MeshPoolGroupPlan).
    // Non-empty = the scatter published this frame's ranges keyed by group, so
    // the recorder issues ONE indirect draw per (classKey, group) and looks
    // ranges up by group id. Empty = the per-bucket path.
    std::span<const uint32_t>        MeshPoolGroups;
    // Exec→declare under-draw feedback sink (see DepthUnderDrawTracker). Owned
    // by RenderServices; may be null in granular tests that never cache.
    DepthUnderDrawTracker*           UnderDraw = nullptr;
    // Compatibility profile marker AND data source: non-null means the device
    // has no buffer_device_address, so each entity batch replays this frame's
    // CPU-built instance list as one direct instanced draw instead of issuing
    // one DrawIndexedIndirectCount per scatter-published range. Null on the
    // GPU-driven path.
    const CpuDrawStreamBuilder*      CpuDrawStream = nullptr;
};

// Which depth pipeline ONE draw segment takes. The crossfade rule lives here
// rather than inline in the recorder because it is the load-bearing half of
// phase parity: a fading tail must reach the same dither the colour pass runs,
// and the shared minimal depth VS emits no fade code, so it cannot serve one.
struct DepthSegmentPipelineChoice
{
    Rendering::MaterialKeyword Keywords{};
    // The material-independent shared depth pipeline (no fragment stage, keeps
    // early-Z, collapses N pipeline binds to 1).
    bool UseSharedDepth = false;
    // A composed fragment stage is present, so the pipeline reflects the
    // material's own descriptor sets and the binder must derive the set count
    // from the variant meta instead of assuming the vertex-only set 0.
    bool ComposesFragment = false;
};

// `headKeywords` / `headUsesSharedDepth` / `headComposesFragment` describe the
// pass's non-fading draws (Instanced, plus DepthOnlyFragment for a Mask material
// or a parallax material's depth-offset prepass, with ParallaxDepthOffset for the
// latter, or DepthOnlyTransmissionColor for the glass tint pass, or MotionVectors
// for the deforming-motion producer).
inline DepthSegmentPipelineChoice ChooseDepthSegmentPipeline(
    Rendering::MaterialKeyword headKeywords, bool headUsesSharedDepth,
    bool headComposesFragment, bool crossfading)
{
    DepthSegmentPipelineChoice choice{};
    if (!crossfading)
    {
        choice.Keywords         = headKeywords;
        choice.UseSharedDepth   = headUsesSharedDepth;
        choice.ComposesFragment = headComposesFragment;
        return choice;
    }
    // A motion tail dithers with the same call the prepass tail dithers with,
    // but it must not pick up DepthOnlyFragment on the way: that keyword and
    // MotionVectors are two different attachment shapes, both claiming the
    // single output at location 0, and the adapter refuses the pair by name.
    // The motion fragment already IS a coverage fragment.
    const bool motion = Rendering::HasKeyword(headKeywords,
                                              Rendering::MaterialKeyword::MotionVectors);
    choice.Keywords = headKeywords | Rendering::MaterialKeyword::LodCrossfade;
    if (!motion)
        choice.Keywords |= Rendering::MaterialKeyword::DepthOnlyFragment;
    choice.UseSharedDepth   = false;
    choice.ComposesFragment = true;
    return choice;
}

// The depth/shadow layout of a head whose stages read only position, UV0 and skinning: the shared depth
// pipeline and every per-material head that adds no stream. Tangent, colour and UV1..7 are stripped whatever
// the mesh owns, so a mesh whose source happens to carry tangents keeps one shared depth pipeline.
inline Rendering::VertexAttributeFlags StrippedDepthVertexFlags(Rendering::VertexAttributeFlags flags)
{
    using Rendering::VertexAttributeFlags;
    return flags & ~(VertexAttributeFlags::HasTangent | VertexAttributeFlags::HasColor | VertexAttributeFlags::HasUV1 |
                     VertexAttributeFlags::HasUV2 | VertexAttributeFlags::HasUV3 | VertexAttributeFlags::HasUV4 |
                     VertexAttributeFlags::HasUV5 | VertexAttributeFlags::HasUV6 | VertexAttributeFlags::HasUV7);
}

// The pass keywords every depth pass starts from: the GPU-driven vertex fetch (Instanced) when the
// view's world pass draws instanced, and nothing else, since lighting keywords are fragment-stage only.
// A forward producer's camera-prepass head starts from the same set, so its vertex stage is the colour
// draw's own.
inline Rendering::MaterialKeyword DepthPassKeywords(const std::optional<Rendering::MaterialKeyword>& worldPassKeywords)
{
    return worldPassKeywords && Rendering::HasKeyword(*worldPassKeywords, Rendering::MaterialKeyword::Instanced)
               ? Rendering::MaterialKeyword::Instanced
               : Rendering::MaterialKeyword::None;
}

// Whether a depth draw of a material reads the mesh's vertex colour. A Mask
// material's coverage fragment (the camera prepass, the shadow families, the
// deforming-motion pass) discards on the surface's opacity against the colour
// pass's own cutoff, and that opacity carries vertex alpha whenever the colour
// pass reads vertex colour. The two passes must therefore see the same Color
// stream: without it the depth pass keeps fragments the colour pass discards,
// so the prepass writes depth over a cut-out area that then shows the clear
// colour, and the cut-out area casts a shadow. Every other depth draw decides
// coverage without evaluating the surface (no fragment stage, the crossfade
// dither alone, or the glass tint's transmission colour), so it keeps the
// stripped layout and its cheaper variant. DepthHeadVertexFlags applies it and
// takes the Color stream from the colour pass's layout (BoundStreamVertexFlags),
// which already has none for a material that ignores vertex colour.
inline bool DepthPassReadsVertexColor(DepthPassType passType, MaterialAlphaMode alphaMode)
{
    return alphaMode == MaterialAlphaMode::Mask && passType != DepthPassType::TransmittanceCascade;
}

// The vertex layout one depth head draws with. The rule: a depth or shadow draw's layout carries every
// stream its stages read, and the draw binds exactly those streams. The depth pass's stripped layout
// (position, UV0, skinning) serves a head whose stages read nothing more; a head that reads more adds
// the stream here:
// - A vertex-modified material (`vertexModified`) draws with the colour pass's layout
//   (`colorPassVertexFlags`, BoundStreamVertexFlags). Its modifier runs in every depth pass and may
//   read any stream the colour pass binds, as the tree package's wind reads its per-leaf seed from UV1;
//   a head that read a stream's default instead would place the vertex somewhere the colour pass does
//   not, and its depth and shadow would part from the visible surface.
// - A head whose coverage reads vertex colour (`coverageReadsVertexColor`, DepthPassReadsVertexColor)
//   keeps the colour stream when the colour pass has one. A Mask material's coverage fragment discards
//   on the same opacity the colour pass discards on, vertex alpha included; a head that read the
//   stream's default (opaque white) would keep the fragments the colour pass cuts away. The colour
//   pass's layout has no colour stream when the mesh has none bound or the material ignores vertex
//   colour, and the head then reads none either.
// - The prepass head writing a parallax material's relief depth (ParallaxDepthOffset) keeps the tangent
//   when the colour pass has one. That head marches from the tangent frame the colour pass marches from
//   or reads the hit in; a frame built from the screen derivatives alone points a hair away, which moves
//   the hit across a relief edge.
// A forward contributor's camera-prepass head (CBT terrain, terrain grass) needs no call: it draws with its
// colour draw's layout (DrawCommand::VertexFlags), which is what this rule gives a vertex-modified head, and,
// where the producer pins the colour pipeline, with that pipeline's vertex input. Its vertex stage fetches
// the geometry from set-2 buffers, so what makes the head bind that geometry is the set count
// (DepthOnlyPipelineSetCount, below), not the layout.
inline Rendering::VertexAttributeFlags DepthHeadVertexFlags(Rendering::VertexAttributeFlags depthVertexFlags,
                                                            Rendering::VertexAttributeFlags colorPassVertexFlags,
                                                            bool vertexModified,
                                                            bool coverageReadsVertexColor,
                                                            Rendering::MaterialKeyword headKeywords)
{
    if (vertexModified)
        return colorPassVertexFlags;
    Rendering::VertexAttributeFlags flags = depthVertexFlags;
    if (coverageReadsVertexColor)
        flags |= colorPassVertexFlags & Rendering::VertexAttributeFlags::HasColor;
    const bool reliefHead = Rendering::HasKeyword(headKeywords, Rendering::MaterialKeyword::ParallaxDepthOffset);
    if (reliefHead && Rendering::HasFlag(colorPassVertexFlags, Rendering::VertexAttributeFlags::HasTangent))
        flags |= Rendering::VertexAttributeFlags::HasTangent;
    return flags;
}

// The descriptor-set half of the rule above: a depth pipeline with no fragment stage declares the
// descriptor sets up to the highest one its vertex stage reads (set 0 alone for a mesh material), and
// the binder binds exactly those. A vertex modifier that fetches its geometry from set-2 buffers (CBT
// terrain, terrain grass) needs them in the layout, or the pipeline does not translate on Metal and
// reads unbound descriptors on Vulkan. Returns one past the highest set the vertex stage reads (0 when
// it reads none).
inline uint32_t DepthOnlyPipelineSetCount(const Rendering::ShaderMeta& meta)
{
    uint32_t count = 0;
    for (const auto& set : meta.Sets)
    {
        for (const auto& binding : set.Bindings)
        {
            if ((binding.StagesMask & Rendering::ShaderMetaStage::kVertex) != 0u && set.Set + 1u > count)
                count = set.Set + 1u;
        }
    }
    return count;
}

// What a depth pipeline with no fragment stage declares under the compatibility profile: the sets
// DepthOnlyPipelineSetCount names, each holding only the bindings its vertex stage reads. A
// variant composed from a lit material's keywords reflects the fragment's shadow arrays and IBL
// cubes in set 0, and WebGPU validates every entry of a bind group against its layout whether a
// stage reads it or not, so a fragment-less draw that declared them would have to bind a texture
// of each one's exact shape from passes that provide none (the camera prepass). Set 1 stays whole:
// its layout is the material texture set's, patched in by the pipeline, and the binder recognizes
// it by its binding 0. The binder walks this meta and the pipeline interns its layouts from it, so
// the set it builds is the set the pipeline declares.
inline Rendering::ShaderMeta CompatVertexStageDepthMeta(const Rendering::ShaderMeta& meta)
{
    constexpr uint32_t kMaterialTextureSet = 1u;
    const uint32_t setCount = DepthOnlyPipelineSetCount(meta);
    Rendering::ShaderMeta out = meta;
    std::erase_if(out.Sets, [setCount](const Rendering::DescriptorSetMeta& set) { return set.Set >= setCount; });
    for (Rendering::DescriptorSetMeta& set : out.Sets)
    {
        if (set.Set == kMaterialTextureSet)
            continue;
        std::erase_if(set.Bindings, [](const Rendering::DescriptorBindingMeta& binding) {
            return (binding.StagesMask & Rendering::ShaderMetaStage::kVertex) == 0u;
        });
        Rendering::FinalizeSetLayout(set);
    }
    return out;
}

// The contributor depth commands one depth pass draws for a view: the depth-only heads the forward
// producers emit with their colour draws (ForwardDrawDepth::Prepass: CBT terrain) in the camera prepass, and the registered depth
// producers' commands in the light-space passes. Phase A draws them whole. The HZB recover pass (phase B)
// redraws only the entity batches the occlusion test revealed, so a contributor drawn there would be drawn
// twice. The deforming-motion pass draws none: a contributor is a procedural-geometry material with no
// composed motion variant, and its surface keeps the sentinel and reprojects analytically.
std::span<const DrawCommand> ContributorDepthCommands(const ViewRegistry& views, Rendering::ViewId viewId,
                                                      DepthPassType passType,
                                                      Rendering::GPUDrawStreamBuilder::SlicePhase phase);

// The heads of the view's ForwardDrawDepth::PrepassNonOccluding draws this frame: what the
// non-occluding prepass draws after DepthResolve, or the camera prepass when none was declared.
std::span<const DrawCommand> NonOccludingPrepassHeads(const ViewRegistry& views, Rendering::ViewId viewId);

// Whether a depth pass ever draws a crossfading tail. Only a camera slice
// publishes a tail block (GPUDrawStreamBuilder's sliceCrossfades: the Color table
// at cascade None), so the camera prepass and the deforming-motion pass draw
// tails; every light-space pass (the shadow families and the glass tint cascade)
// draws heads alone. A tail of the tint pass has no composable variant at all:
// ChooseDepthSegmentPipeline would add DepthOnlyFragment, which declares no
// colour output, to a head whose fragment writes the tint.
inline bool DepthPassDrawsCrossfadeTails(DepthPassType passType)
{
    return passType == DepthPassType::Prepass || passType == DepthPassType::DeformationMotion;
}

// Whether a depth pass draws a material at all. Blend writes no depth anywhere.
// The opaque depth passes peel glass (keeps the post-opaque grab and
// ge_sceneDepth opaque-only, and glass casts no hard shadow); the
// TransmittanceCascade pass is the inverse, drawing ONLY glass into the tint
// array. Same per-cascade cull streams either way.
enum class DepthPassMaterialFilter : uint8_t
{
    Draws,
    AlphaBlend,
    Transmission,
};

inline DepthPassMaterialFilter FilterDepthPassMaterial(DepthPassType passType,
                                                       MaterialAlphaMode alphaMode,
                                                       bool transmissive)
{
    if (alphaMode == MaterialAlphaMode::Blend)
        return DepthPassMaterialFilter::AlphaBlend;
    if (transmissive != (passType == DepthPassType::TransmittanceCascade))
        return DepthPassMaterialFilter::Transmission;
    return DepthPassMaterialFilter::Draws;
}

// The pipeline a pass draws one material's non-fading (head) segments with.
// A crossfading tail derives from it through ChooseDepthSegmentPipeline.
//
// The motion key carries the pass keyword and nothing from the shading path.
// The variant cache's attachment-count warning reads the KEY rather than the
// composed defines, so a motion key that had picked up the reflection G-buffer
// keyword would report a correct one-attachment variant as under-attached; and
// the coverage keyword the prepass adds for a Mask material is a different
// attachment shape the adapter refuses by name. Neither can reach the key from
// here: `passKeywords` is the pass's own Instanced narrowing. The motion
// variant always composes a fragment stage: it exports the payload, and on a
// masked material it runs the prepass's own cutoff first (the alpha test rides
// the material's own key).
//
// `sharedDepthOffered`: the pass may draw on the material-independent shared
// depth pipeline (the shadow families; the camera prepass unless disabled or
// the mesh is SKINNED_8; never under the compatibility profile). The shared
// pipeline has no fragment stage and no modifier, so a Mask material, a
// vertex-modified one (the modifier must run in every depth pass or the visible
// mesh and its depth diverge), the motion pass and the tint pass all take the
// per-material variant.
//
// `writesReliefDepth`: the draw's material writes its relief's depth
// (WritesReliefDepth, ParallaxReliefDepth.h). The camera prepass is the one
// depth pass whose keywords carry ParallaxDepthOffset: its head then composes the
// fragment that marches the relief and writes the hit's depth (DepthOnlyFragment |
// ParallaxDepthOffset), off the shared pipeline. Every other pass keeps the
// polygon, so the flat polygon casts and the depth class is unchanged
// (ClassifyMaterialDepthClass). The compatibility profile never writes it.
inline DepthSegmentPipelineChoice ChooseDepthHeadPipeline(
    Rendering::MaterialKeyword passKeywords, DepthPassType passType, bool alphaTest,
    bool vertexModified, bool sharedDepthOffered, bool writesReliefDepth)
{
    const bool motion = passType == DepthPassType::DeformationMotion;
    const bool tint = passType == DepthPassType::TransmittanceCascade;
    const bool depthOffset = writesReliefDepth && passType == DepthPassType::Prepass &&
                             !Rendering::IsCompatShaderProfile();
    DepthSegmentPipelineChoice choice{};
    choice.Keywords = passKeywords & ~Rendering::MaterialKeyword::ParallaxDepthOffset;
    if (motion)
        choice.Keywords |= Rendering::MaterialKeyword::MotionVectors;
    else if (tint)
        choice.Keywords |= Rendering::MaterialKeyword::DepthOnlyTransmissionColor;
    else if (alphaTest || depthOffset)
        choice.Keywords |= Rendering::MaterialKeyword::DepthOnlyFragment;
    if (depthOffset)
        choice.Keywords |= Rendering::MaterialKeyword::ParallaxDepthOffset;
    choice.ComposesFragment = motion || tint || alphaTest || depthOffset;
    choice.UseSharedDepth =
        sharedDepthOffered && !motion && !tint && !alphaTest && !vertexModified && !depthOffset;
    return choice;
}

// The camera prepass may draw eligible heads on the shared depth pipeline
// (GE_PREPASS_SHARED_DEPTH=0 disables it for triage).
bool PrepassSharedDepthEnabled();

// Record the depth/shadow draws for one declared depth-only pass. params arrives
// by value (moved from the exec lambda); the services bundle by const-ref.
void RecordDepthOnlyPass(Rendering::RenderGraph::RGContext& c,
                         DepthOnlyPassParamsRG params,
                         const DepthDrawServices& services);

} // namespace Engine::Renderer
} // namespace GameEngine
