#pragma once

#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"

namespace GameEngine::Testing
{
// Pins a one-pass pipeline on `spine`: the world pass, whose resolve is the view's FinalColor. For
// tests that run the spine without an EngineCore, which has no asset manager to load the engine's
// pipeline from, so the spine draws only a blueprint set on it.
inline void PinWorldOnlyPipeline(Engine::Renderer::FrameOrchestrator& spine)
{
    namespace Pipeline = Engine::Renderer::Pipeline;
    Pipeline::RenderPipelineBlueprint blueprint{};
    blueprint.schemaVersion = 2;
    blueprint.pipelineName = "WorldOnly";
    blueprint.sourcePath = "<test>";
    blueprint.contentHash = 0x3011D0u;

    Pipeline::RenderPipelineBlueprint::Pass world{};
    world.id = "World";
    world.type = "WorldRender";
    world.enabled = true;
    world.perView = true;
    world.passJson = R"({"id":"World","type":"WorldRender","enabled":true})";
    blueprint.passes.push_back(world);

    Pipeline::RenderPipelineBlueprint::Output output{};
    output.name = Pipeline::Names::Output::FinalColor;
    output.resourceRef = Pipeline::Names::View::Resolve;
    blueprint.outputs.push_back(output);

    spine.SetActiveRenderPipelineBlueprint(std::move(blueprint));
}
} // namespace GameEngine::Testing
