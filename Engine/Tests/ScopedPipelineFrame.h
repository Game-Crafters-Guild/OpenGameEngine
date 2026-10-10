#pragma once

#include "Engine/Rendering/FrameOrchestrator.h"

namespace GameEngine::Testing
{
// Match the window owner's paired lifetime, including assertion/exception exits.
// Declare this after the frame and before its first BuildFrameGraph call.
class ScopedPipelineFrame
{
  public:
    ScopedPipelineFrame(Engine::Renderer::FrameOrchestrator& spine,
                        Rendering::RenderGraph::RGFrame& frame)
        : m_Spine(spine), m_Frame(frame) {}
    ~ScopedPipelineFrame() { m_Spine.RemovePipelineInstanceForFrame(&m_Frame); }
    ScopedPipelineFrame(const ScopedPipelineFrame&) = delete;
    ScopedPipelineFrame& operator=(const ScopedPipelineFrame&) = delete;

  private:
    Engine::Renderer::FrameOrchestrator& m_Spine;
    Rendering::RenderGraph::RGFrame& m_Frame;
};
} // namespace GameEngine::Testing
