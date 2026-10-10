#include "CBTTerrainECS/CBTValidationReadback.h"

#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTLayout.h"
#include "CBTTerrain/CBTResources.h"

#include "Engine/Rendering/ViewReadbackUtils.h"

#include "Rendering/Core/RenderGraph/RGFrame.h"

#include "Logger/Logger.h"

#include <cstring>

namespace GameEngine::CBTTerrainECS
{

namespace RG = ::GameEngine::Rendering::RenderGraph;

void CBTValidationReadback::Tick(RG::RGFrame& frame, Rendering::IDevice* device,
                                 CBTTerrain::CBTInstance& instance)
{
    if (m_Ticket)
    {
        Rendering::BufferReadbackResult readback{};
        if (m_Ticket->TryGet(readback) && readback.bytes.size() >= CBTTerrain::kValidationWords * 4u)
        {
            uint32_t counters[CBTTerrain::kValidationWords];
            std::memcpy(counters, readback.bytes.data(), sizeof(counters));
            ++m_Frames;
            const bool changed = std::memcmp(counters, m_Last, sizeof(m_Last)) != 0;
            if (changed || (m_Frames % kSummaryEveryFrames) == 0u)
            {
                std::memcpy(m_Last, counters, sizeof(m_Last));
                Logger::Log::Info(
                    "CBT.Validate links={}/{} linksCur={} budget={} zombie={} "
                    "compact={} parity={}->{} depths={}/{} (frame {})",
                    counters[CBTTerrain::kValidationErrorCounter], counters[10], counters[11],
                    counters[CBTTerrain::kValidationBudgetCounter],
                    counters[CBTTerrain::kValidationZombieCounter],
                    counters[CBTTerrain::kValidationCompactCounter], counters[12], counters[13],
                    counters[14] >> 16u, counters[14] & 0xFFFFu, m_Frames);
                Logger::Log::Info(
                    "CBT.Validate.work split={} alloc={} propBisect={} "
                    "propSimplify={} simplify={} simplifyClass={}",
                    counters[4], counters[5], counters[6], counters[7], counters[8], counters[9]);
            }
            m_Ticket.reset();
        }
        else if (m_Ticket->IsConsumed())
        {
            m_Ticket.reset(); // a device rebuild killed it: read again on the new device
        }
    }
    if (!m_Ticket)
    {
        const RG::RGBuffer validationBuffer = frame.ImportExternalBuffer(
            "CBT.Validation",
            instance.GetResources().GetBuffer(CBTTerrain::CBTBinding::Validation));
        if (validationBuffer.IsValid())
            m_Ticket = Rendering::RequestBufferReadbackRG(device, frame, validationBuffer, 0u,
                                                          CBTTerrain::kValidationWords * 4u,
                                                          "CBT.Validation");
    }
}

} // namespace GameEngine::CBTTerrainECS
