#pragma once

// CBTRenderNode — the per-view pipeline node that drives the CBT terrain render.
// Declared before the DepthPrepass node (it is registered as feeding the prepass,
// and the pipeline compiler declares it ahead of one); a no-op each frame until a
// live Terrain entity activates the feature (CBTUpdateSystem). Records the global
// CBT.Update compute pass once per frame, then emits one indexed-indirect forward
// draw per view into the MAIN world pass as a contributor (via EmitForwardCommand)
// — NOT a separate pass — and its depth-only head into the camera prepass, so the
// prepass depth holds the ground and the world pass only depth-tests against it.
// Until the device has built the head's pipeline the forward draw writes its own depth.
// CBT is still excluded from the shadow passes, so it casts no shadows (plan §8
// shadow rule); shadow/reflection participation is a later slice. EditorPreview
// draws only when this graph already recorded CBT.Update (bookmark preview);
// thumbnails and other solo preview graphs stay skipped.

#include <string>

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

namespace GameEngine::CBTTerrainECS
{

class CBTRenderNode final : public Engine::Renderer::Pipeline::IRenderPipelineNode
{
  public:
    const char* GetTypeName() const override { return "CBTRender"; }
    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d) override;
};

} // namespace GameEngine::CBTTerrainECS
