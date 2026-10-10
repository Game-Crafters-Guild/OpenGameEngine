#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Engine/Rendering/Pipeline/ViewMotionVectors.h"
#include "Engine/Rendering/PostProcessSettings.h"

#include "Mathematics/Rect.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>

namespace GameEngine::Engine::Renderer
{
struct ViewLetterbox;
}

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// FidelityFX SSSR-style Forward+ reflections: material classification and a
// compact indirect ray list, stochastic GGX VNDF HiZ intersection, dedicated
// reflection reprojection/prefilter/temporal accumulation, then BRDF-aware HDR
// composite. The history is independent of the engine's TAA implementation.
class ScreenSpaceReflectionsNode final : public IRenderPipelineNode
{
  public:
    // Node objects are re-instantiated wholesale on blueprint reload, device
    // rebuild and frame-stream eviction, so the samplers and the uploaded
    // blue-noise tile are released here instead of leaking a set per rebuild.
    ~ScreenSpaceReflectionsNode() override;

    const char* GetTypeName() const override { return "ScreenSpaceReflections"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void Declare(RenderPipelineInstance& instance, const PipelineDeclareContext& ctx) override;
    void DeclareForView(ViewDeclare& d) override;

    // The camera's viewport inside the render target, in target UV. A
    // letterboxed view rasterizes the world into a sub-rectangle of a
    // full-size target, and the SSR kernels convert between target UV and the
    // camera's NDC on every ray, so they are handed this rather than assuming
    // the viewport fills the target. Degenerate inputs fall back to the whole
    // target, which is what an inactive letterbox means.
    static Mathematics::Rect ComputeViewportRect(const ViewLetterbox& letterbox,
                                                 uint32_t targetWidth, uint32_t targetHeight);

  private:
    enum Program : size_t
    {
        Classify,
        PrepareArgs,
        Intersect,
        Reproject,
        Prefilter,
        Temporal,
        Composite,
        ProgramCount
    };

    struct ComputeProgram
    {
        Rendering::ComputePipelineId Pipeline{};
        std::unique_ptr<Rendering::ShaderMeta> Meta;
        Rendering::DescriptorSetLayoutDesc Set0Layout{};
    };

    struct ViewHistory
    {
        uint64_t LastWrittenFrame = ~0ull;
        uint64_t HistoryFrame = 0;
        uint32_t Width = 0;
        uint32_t Height = 0;
    };

    void LoadShaders(Rendering::IDevice* device);
    Rendering::DescriptorSetHandle NewSet(Rendering::IDevice* device, Program program,
                                           const char* debugName) const;
    Rendering::PipelineHandle BeginProgram(Rendering::RenderGraph::RGContext& ctx,
                                            Program program,
                                            Rendering::DescriptorSetHandle set,
                                            const PostProcessSettings& settings,
                                            uint32_t frameIndex,
                                            bool historyValid,
                                            const Mathematics::Rect& viewportRect) const;

    // Non-owning; retained from LoadShaders so the destructor can release the
    // device objects created there.
    Rendering::IDevice* m_Device = nullptr;
    ViewMotionVectors m_MotionVectors;
    std::string m_Id;
    std::string m_InputKey = "SceneColor";
    std::string m_OutputKey = "HDRSSSR";
    bool m_LoadAttempted = false;
    bool m_ShadersLoaded = false;
    // One-time diagnostic: SSSR active but the world pass published no
    // *.Written G-buffer signal (e.g. MSAA on the view).
    bool m_WarnedGBufferMissing = false;
    bool m_LoggedSkipReason = false;
    bool m_LoggedChainDeclared = false;
    std::array<ComputeProgram, ProgramCount> m_Programs{};
    Rendering::SamplerHandle m_PointSampler{};
    Rendering::SamplerHandle m_LinearSampler{};
    // Precomputed void-and-cluster blue-noise tile (RG8), uploaded once in
    // LoadShaders and sampled by the intersect pass with a per-frame R2 rotation.
    Rendering::TextureHandle m_BlueNoiseTexture{};
    std::unordered_map<uint32_t, ViewHistory> m_ViewHistory;
};
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
