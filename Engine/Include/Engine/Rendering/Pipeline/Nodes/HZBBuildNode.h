#pragma once

#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"

#include <cstdint>
#include <string>

namespace GameEngine::Rendering { struct ShaderPackage; }

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::ComputePipelineId;
using ::GameEngine::Rendering::DescriptorSetLayoutDesc;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Hierarchical-Z pyramid build (R2.1 two-phase HZB occlusion, design §5-A5).
// Declares a per-view pooled R32F texture at the EXACT phase-A depth extent
// (not next-pow2-down — that would weaken culling ~2x) with a full mip chain:
// mip 0 copies View.DepthResolved into the base, each further mip is a
// conservative 2x2 MIN reduce of the mip below (reverse-Z ⇒ MIN = farthest).
// Publishes the pyramid as "View.HZB".
//
// When SSSR is active for the view it ALSO publishes a second pyramid,
// "View.SSRHiZ", MAX-reduced (reverse-Z ⇒ MAX = nearest surface) — the opposite
// bound, which is what the SSR ray march needs. Separate pool; the occlusion
// pyramid is unaffected.
//
// ONE node object serves EVERY view in the frame (perView declares per view, it
// does not instantiate per view), so this node owns no per-texture device state:
// the single-mip views it binds come from the persistent pool via
// RGContext::GetOrCreatePooledMipView, which ties their lifetime to the image.
//
// A view needing BOTH takes the FUSED SPD path (hzb_spd_minmax.comp +
// hzb_spd_minmax_finalize.comp): one pair of dispatches reads depth ONCE and
// emits both reductions, replacing the MAX pyramid's whole per-mip chain. The
// two pyramids stay separate R32F images rather than one two-channel image —
// SSSR marches the MAX chain heavily, and an interleaved RG32F would make it
// pull 8 B per texel for the 4 B it uses (the same penalty applies to culling's
// sparse MIN taps). The fused shaders reuse one set of LDS tiles across two
// sequential phases, so shared memory stays at the MIN-only path's figure;
// doubling it would break the maxComputeSharedMemorySize gate below.
//
// Two build paths producing BIT-IDENTICAL pyramids (HzbSpdParityTests):
//   * SPD two-dispatch (default): hzb_spd.comp builds mips 0..6 per 64x64
//     tile (LDS ping-pong, folds skipped for mips 2..6 — the miss is confined
//     to each level's global last row/column), then hzb_spd_finalize.comp
//     (ONE workgroup) repairs those edges with the exact fold rule and builds
//     mips 7..12 whole-level from LDS. No storage image is re-read within a
//     dispatch, so nothing is `coherent` (a measured ~2x dispatch cost) and
//     no atomic election exists; the render-graph barrier between the two
//     passes is the only synchronization. Capped at mip-7 extents <= 32
//     (depth extents <= 4223 per axis); larger falls back to the chain.
//   * Per-mip chain (fallback: GE_HZB_SPD=0, over-large extents, or the SPD
//     shaderpkgs missing): one dispatch per mip via hzb_build.comp. Per-mip
//     subresource edges — each reduce pass Reads mip m-1 and Writes mip m of
//     the SAME texture (RGRange BaseMip) — give the graph the RAW barrier
//     between dispatches without self-hazarding (RGTypes' Hi-Z case).
//
// JSON shape:
// {
//   "id": "HZBBuild",
//   "type": "HZBBuild",
//   "enabled": true,
//   "input": "View.DepthResolved",  // optional depth source key
//   "output": "View.HZB",           // optional blackboard key to publish
//   "poolName": "Pipeline.HZB.View" // optional persistent-pool prefix
// }
class HZBBuildNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "HZBBuild"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;
    void DeclareForView(ViewDeclare& d) override;

  private:
    void LoadShaders(IDevice* device);
    void LoadSpdShader(IDevice* device);
    void LoadSpdMinMaxShader(IDevice* device);
    // Slang-lane spike (GE_SHADER_SLANG_LANE=1): compile the staged
    // Shaders/Slang/hzb_build.slang through ShaderCompileService instead of
    // loading the prebuilt GLSL package — the Gate-2 A/B wiring. Fail-visible:
    // any miss warns and the caller falls back to the .shaderpkg.
    bool TryCompileSlangChainShader(::GameEngine::Rendering::ShaderSourceKind kind,
                                    ::GameEngine::Rendering::ShaderPackage& outPkg);
    // Two-dispatch SPD build (tile pass + finalize pass). MIN-only (occlusion).
    void DeclareSpdBuild(ViewDeclare& d, ::GameEngine::Rendering::RenderGraph::RGTexture depth,
                         ::GameEngine::Rendering::RenderGraph::RGTexture hzb, uint32_t w,
                         uint32_t h, uint32_t levels);
    // Fused two-dispatch SPD build emitting BOTH reductions from ONE depth
    // read: the MIN occlusion pyramid and the MAX SSR traversal pyramid.
    void DeclareSpdMinMaxBuild(ViewDeclare& d,
                               ::GameEngine::Rendering::RenderGraph::RGTexture depth,
                               ::GameEngine::Rendering::RenderGraph::RGTexture hzbMin,
                               ::GameEngine::Rendering::RenderGraph::RGTexture hzbMax, uint32_t w,
                               uint32_t h, uint32_t levels);
    // Per-mip chain build (fallback path; content-identical to SPD for MIN).
    // reduceMode selects the mip>=1 op: kReduceMin = farthest/occlusion HZB,
    // kReduceMax = nearest-surface SSR Hi-Z.
    void DeclareChainBuild(ViewDeclare& d, ::GameEngine::Rendering::RenderGraph::RGTexture depth,
                           ::GameEngine::Rendering::RenderGraph::RGTexture hzb, uint32_t w,
                           uint32_t h, uint32_t levels, uint32_t reduceMode);

    // hzb_build.comp push-constant `mode` values for mips >= 1.
    static constexpr uint32_t kReduceMin = 1u;
    static constexpr uint32_t kReduceMax = 2u;

    std::string m_Id;
    std::string m_InputKey = Names::View::DepthResolved;
    std::string m_OutputKey = Names::View::HZB;
    std::string m_PoolPrefix = "Pipeline.HZB.View";
    // Second pyramid: MAX-reduced (nearest surface) Hi-Z for SSR ray traversal,
    // built via the chain path only when SSSR is active for the view.
    // Independent pool entry, so the occlusion HZB path is untouched.
    std::string m_SsrOutputKey = Names::View::SSRHiZ;
    std::string m_SsrPoolPrefix = "Pipeline.SSRHiZ.View";

    bool m_LoadAttempted = false;
    ComputePipelineId m_PipelineId{};
    DescriptorSetLayoutDesc m_Set0Layout{};
    ComputePipelineId m_SpdPipelineId{};
    DescriptorSetLayoutDesc m_SpdSet0Layout{};
    ComputePipelineId m_SpdFinalizePipelineId{};
    DescriptorSetLayoutDesc m_SpdFinalizeSet0Layout{};
    ComputePipelineId m_SpdMinMaxPipelineId{};
    DescriptorSetLayoutDesc m_SpdMinMaxSet0Layout{};
    ComputePipelineId m_SpdMinMaxFinalizePipelineId{};
    DescriptorSetLayoutDesc m_SpdMinMaxFinalizeSet0Layout{};

    // Set0 binding indices resolved from the shaders' reflected meta by name
    // at load time (fallback to the GLSL literals on a reflection miss).
    // Cached so the execute paths add zero lookup.
    uint32_t m_SrcBinding = 0u;      // hzb_build.comp uSrc
    uint32_t m_DstBinding = 1u;      // hzb_build.comp uDst
    uint32_t m_SpdDepthBinding = 0u; // hzb_spd.comp uDepth
    uint32_t m_SpdMip0Binding = 1u;  // hzb_spd.comp uMip0
    uint32_t m_SpdMipsBinding = 2u;  // hzb_spd.comp uMips[6]
    uint32_t m_SpdFinalizeMipsBinding = 0u; // hzb_spd_finalize.comp uMips[12]
    uint32_t m_SpdMinMaxDepthBinding = 0u;   // hzb_spd_minmax.comp uDepth
    uint32_t m_SpdMinMaxMip0MinBinding = 1u; // hzb_spd_minmax.comp uMip0Min
    uint32_t m_SpdMinMaxMipsMinBinding = 2u; // hzb_spd_minmax.comp uMipsMin[6]
    uint32_t m_SpdMinMaxMip0MaxBinding = 3u; // hzb_spd_minmax.comp uMip0Max
    uint32_t m_SpdMinMaxMipsMaxBinding = 4u; // hzb_spd_minmax.comp uMipsMax[6]
    // hzb_spd_minmax_finalize.comp uMipsMin[12] / uMipsMax[12]
    uint32_t m_SpdMinMaxFinalizeMipsMinBinding = 0u;
    uint32_t m_SpdMinMaxFinalizeMipsMaxBinding = 1u;

    // Once-per-episode fail-visible warnings for SPD pipeline-creation
    // failures at exec time (a silently skipped finalize would over-cull).
    bool m_WarnedSpdTilePipeline = false;
    bool m_WarnedSpdFinalizePipeline = false;
    bool m_WarnedSpdMinMaxTilePipeline = false;
    bool m_WarnedSpdMinMaxFinalizePipeline = false;
};
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
