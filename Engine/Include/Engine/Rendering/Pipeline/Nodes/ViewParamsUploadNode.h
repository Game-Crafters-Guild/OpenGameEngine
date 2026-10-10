#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <string>

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{
// Uploads per-view camera parameters required by compute passes (Forward+).
//
// Writes a std140 uniform buffer with:
//   mat4 uInvProj;
//   mat4 uView;
//
// JSON shape:
// {
//   "id": "ViewParamsUpload",
//   "type": "ViewParamsUpload",
//   "enabled": true,
//   "buffer": "ViewParams" // optional (default "ViewParams")
// }
class ViewParamsUploadNode final : public IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "ViewParamsUpload"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(ViewDeclare& d) override;

    // ge_mipBiasParams[0]: TAAU texture-sharpness compensation, derived from the
    // ACTUAL internal/display extent ratio — log2(renderWidth/outputWidth),
    // clamped to [-2, 0]. Exactly 0.0 whenever the view is not upscaling
    // (renderWidth >= outputWidth, or either extent is zero) — the scale-1.0
    // byte-neutrality invariant RenderPipelineDeclareTests pins.
    static float ComputeTaauMipBias(uint32_t renderWidth, uint32_t outputWidth);

  private:
    std::string m_Id;
    std::string m_Json;
    std::string m_BufferRef = "ViewParams";
};
} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
