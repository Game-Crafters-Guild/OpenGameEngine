#pragma once

namespace GameEngine::Engine::Renderer::Pipeline
{
class RenderPipelineNodeRegistry;
}

namespace GameEngine::Engine::Renderer::Pipeline::Nodes
{

// Registers every pipeline node type the engine itself ships — the pass types
// a .rendergraph may name without any module or plugin loaded. Engine modules
// (terrain, ocean, grass) register their own node types into the same registry
// from their Register*PipelineNodes entry points. FrameOrchestrator::Initialize
// populates the runtime registry through this function, and the blueprint fleet
// gate compiles the repo's .rendergraph files against a registry built the same
// way, so a pass type exists for the compiler exactly when the runtime can build it.
void RegisterEngineNodeTypes(RenderPipelineNodeRegistry& registry);

} // namespace GameEngine::Engine::Renderer::Pipeline::Nodes
