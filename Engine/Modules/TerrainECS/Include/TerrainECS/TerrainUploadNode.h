#pragma once

#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"

#include <string>

namespace GameEngine::TerrainECS
{

// Flushes the terrain heightmap/splatmap/normalmap texture uploads the extraction
// system queues on TerrainRenderFeature, in a kEarlySetup render-graph pass. This is
// the renderer-agnostic upload seam that survives the CDLOD render-core deletion — the
// CBT vertex-eval (heights), the CBT surface (splat/normal) and grass all sample these
// bindless textures, so the upload must land before any of them reads.
//
// Being bindless, those textures are never RenderGraph-imported, so this pass declares no
// access its consumers share and its phase does not order it: the scheduler ranks connected
// components, so an access-less pass is a component of one and is emitted after any consumer
// component that holds an earlier kEarlySetup pass. What orders it is the explicit ordering
// edge each consumer adds from this pass to its own, using the pass this node publishes via
// TerrainRenderFeature::RecordHeightmapUploadPassRG. Declare this node ahead of its consumers
// in the pipeline — a consumer declared first finds no pass to order against.
//
// Also owns TerrainRenderFeature initialization + the per-frame deferred-destroy retirement
// that the CDLOD node used to drive.
//   { "type": "TerrainUpload", "enabled": true }
class TerrainUploadNode final : public Engine::Renderer::Pipeline::IRenderPipelineNode
{
public:
    const char* GetTypeName() const override { return "TerrainUpload"; }

    bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) override;
    void DeclareForView(Engine::Renderer::Pipeline::ViewDeclare& d) override;

private:
    std::string m_Id;
    std::string m_Json;
};

} // namespace GameEngine::TerrainECS
