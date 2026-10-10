#include "CBTTerrainECS/CBTTreeSeed.h"

namespace GameEngine::CBTTerrainECS
{

const char* ToString(CBTTreeRestart restart)
{
    switch (restart)
    {
    case CBTTreeRestart::None:
        return "none";
    case CBTTreeRestart::DomainChanged:
        return "domain changed";
    case CBTTreeRestart::TerrainRetired:
        return "terrain replaced";
    case CBTTreeRestart::DepthCapLowered:
        return "depth cap lowered";
    }
    return "unknown";
}

void CBTTreeSeed::Seeded(uint32_t domain, uint32_t maxDepth)
{
    m_Domain = domain;
    m_DeepestAllowedDepth = maxDepth;
    m_TerrainRetired = false;
    m_Holding = false;
}

CBTTreeRestart CBTTreeSeed::RestartReason(uint32_t domain, uint32_t maxDepth, uint32_t frameCounter)
{
    CBTTreeRestart reason = CBTTreeRestart::None;
    if (domain != m_Domain)
        reason = CBTTreeRestart::DomainChanged;
    else if (m_TerrainRetired)
        reason = CBTTreeRestart::TerrainRetired;
    else if (maxDepth < m_DeepestAllowedDepth)
        reason = CBTTreeRestart::DepthCapLowered;

    if (reason == CBTTreeRestart::None)
    {
        m_DeepestAllowedDepth = maxDepth;
        m_Holding = false;
        return CBTTreeRestart::None;
    }

    // Leading edge: a request that arrives with no restart in the last kRestartSettleFrames frames
    // fires at once, inside the frame that changed the terrain (a scene open restarts within its
    // load hitch). Repeats inside that window are held and coalesced until they stop changing.
    const Request request{domain, maxDepth, m_RetireCount};
    if (!m_Holding)
    {
        const bool restartedRecently =
            m_HasRestarted && frameCounter - m_LastRestartFrame < kRestartSettleFrames;
        if (!restartedRecently)
            return Fire(reason, frameCounter);
        m_Holding = true;
        m_HeldRequest = request;
        m_HeldSinceFrame = frameCounter;
        return CBTTreeRestart::None;
    }
    if (request != m_HeldRequest)
    {
        m_HeldRequest = request;
        m_HeldSinceFrame = frameCounter;
        return CBTTreeRestart::None;
    }
    return frameCounter - m_HeldSinceFrame >= kRestartSettleFrames ? Fire(reason, frameCounter)
                                                                   : CBTTreeRestart::None;
}

CBTTreeRestart CBTTreeSeed::Fire(CBTTreeRestart reason, uint32_t frameCounter)
{
    m_Holding = false;
    m_HasRestarted = true;
    m_LastRestartFrame = frameCounter;
    return reason;
}

} // namespace GameEngine::CBTTerrainECS
