#pragma once

// CBTValidationReadback — GE_CBT_VALIDATE's read of the Validate kernel's error counters. The
// update dispatches Validate (re-armed in CBTRenderFeature); this reads its counters back through
// the async render-graph ticket path (browser WebGPU cannot block, so no DebugReadWords) and logs
// on every change. Non-reciprocal neighbour links are the error class that shows up as conformity
// cracks and sliver holes in the drawn mesh.
//
// Owned by CBTRenderFeature: the in-flight ticket is released when the feature is destroyed,
// which RenderServices does before it shuts the device down. A ticket must never outlive the
// device (its destructor calls into it), so it is never a static.

#include <cstdint>
#include <memory>

namespace GameEngine::Rendering
{
class IDevice;
class RGBufferReadbackTicket;
namespace RenderGraph
{
class RGFrame;
}
} // namespace GameEngine::Rendering

namespace GameEngine::CBTTerrain
{
class CBTInstance;
}

namespace GameEngine::CBTTerrainECS
{

class CBTValidationReadback
{
  public:
    // Logs the counters of the last ticket once they land, then keeps one ticket in flight on
    // `frame`. A ticket a device rebuild killed is dropped and requested again.
    void Tick(Rendering::RenderGraph::RGFrame& frame, Rendering::IDevice* device,
              CBTTerrain::CBTInstance& instance);

  private:
    static constexpr uint32_t kLoggedWords = 4u;
    static constexpr uint32_t kSummaryEveryFrames = 240u;

    std::shared_ptr<Rendering::RGBufferReadbackTicket> m_Ticket;
    uint32_t m_Last[kLoggedWords] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    uint32_t m_Frames = 0u;
};

} // namespace GameEngine::CBTTerrainECS
