#include "Rendering/Core/RenderGraph/RGFrameStamp.h"

#include "Rendering/Core/RenderGraph/RGFrame.h"

namespace GameEngine::Rendering::RenderGraph
{

bool RGFrameStamp::IsFor(const RGFrame& f) const
{
    return Frame == &f && FrameIndex == f.FrameIndex();
}

void RGFrameStamp::Stamp(RGFrame& f)
{
    Frame = &f;
    FrameIndex = f.FrameIndex();
}

} // namespace GameEngine::Rendering::RenderGraph
